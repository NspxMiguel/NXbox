#!/usr/bin/env python3
"""Apply the NXbox worker-context fixes to the pinned Mesa UWP source."""

from pathlib import Path
import argparse
import os

# NXBOX_MESA_SKIP=query,fence leaves those patches out, to bisect rendering problems.
SKIP = set(filter(None, os.environ.get("NXBOX_MESA_SKIP", "").split(",")))


def patch(root: Path) -> None:
    path = root / "src/gallium/winsys/uwp/gdi_uwp.cpp"
    source = path.read_text()
    replacements = {
        "#include <vector>": "#include <vector>\n#include <atomic>",
        "volatile bool finished = false;": "std::atomic<bool> finished{false};",
        "      CoreWindow^ coreWindow = CoreWindow::GetForCurrentThread();\n      DisplayInformation^ currentDisplayInformation = DisplayInformation::GetForCurrentView();\n": "",
        "   CoreWindow^ coreWindow = CoreWindow::GetForCurrentThread();\n   Platform::Agile<Windows::UI::Core::CoreWindow> m_window;\n   m_window = coreWindow;\n   return (HWND)reinterpret_cast<IUnknown*>(m_window.Get());": "   // The frontend retains the CoreWindow ABI pointer used as this HDC.\n"
        "   // GPU and shader worker threads have no thread-local CoreWindow.\n"
        "   return reinterpret_cast<HWND>(hDC);",
    }
    for old, new in replacements.items():
        if old not in source:
            raise RuntimeError(f"Pinned Mesa source does not match patch: {old[:70]}")
        source = source.replace(old, new)
    path.write_text(source)

    present_path = root / "src/gallium/winsys/d3d12/wgl/d3d12_wgl_framebuffer_uwp.cpp"
    present_source = present_path.read_text()
    present_old = (
        "   if (interval < 1)\n"
        "      return S_OK == framebuffer->swapchain->Present(0, DXGI_PRESENT_ALLOW_TEARING);\n"
        "   else\n"
        "      return S_OK == framebuffer->swapchain->Present(interval, 0);"
    )
    present_new = (
        "   // The Xbox compositor is a fixed-refresh sink; DXGI_PRESENT_ALLOW_TEARING is untested\n"
        "   // there and is not needed on a console. Present with vsync by default;\n"
        "   // NXBOX_PRESENT_INTERVAL=0 queues without blocking (flip model, still no tearing), so a\n"
        "   // frame that just misses 33 ms does not wait a third vblank (30 FPS -> 20 FPS).\n"
        "   const char *nxbox_interval = getenv(\"NXBOX_PRESENT_INTERVAL\");\n"
        "   UINT nxbox_sync = nxbox_interval && *nxbox_interval ? (UINT)atoi(nxbox_interval)\n"
        "                                                      : (interval < 1 ? 1 : interval);\n"
        "   return S_OK == framebuffer->swapchain->Present(nxbox_sync, 0);"
    )
    if present_old not in present_source:
        raise RuntimeError("Pinned Mesa source does not match the present patch")
    present_source = present_source.replace(present_old, present_new)
    if "#include <new>\n" not in present_source:
        raise RuntimeError("Pinned Mesa present source does not match the stdlib include patch")
    present_path.write_text(present_source.replace("#include <new>\n", "#include <new>\n#include <stdlib.h>\n", 1))

    if "query" in SKIP:
        print("skipping the query reentrancy patch")
    else:
        patch_query(root)
    if "pso" in SKIP:
        print("skipping the PSO report patch")
    else:
        patch_pso(root)
    if "dxil" in SKIP:
        print("skipping the DXIL report patch")
    else:
        patch_dxil(root)
    patch_shader_model(root)
    patch_format_cast_report(root)
    patch_draw(root)
    patch_null_pso(root)
    patch_batch(root)
    if "fence" in SKIP:
        print("skipping the null fence patch")
    else:
        patch_fence(root)


def patch_null_pso(root: Path) -> None:
    # When CreatePipelineState fails (counted in NXBOX_D3D12_PSO), d3d12_get_*_pipeline_state
    # returns NULL and the draw goes on with only an assert, disabled in release: the batch then
    # AddRef()s a null object. Breath of the Wild died that way about 80 s in (access violation
    # reading 0x0 in d3d12_batch_reference_object). Drop the draw or dispatch instead.
    path = root / "src/gallium/drivers/d3d12/d3d12_draw.cpp"
    source = path.read_text()
    gfx_old = (
        "      ctx->current_gfx_pso = d3d12_get_gfx_pipeline_state(ctx);\n"
        "      assert(ctx->current_gfx_pso);\n"
        "   }\n"
    )
    gfx_new = gfx_old + (
        "   if (!ctx->current_gfx_pso) {\n"
        "      if (index_buffer && dinfo->has_user_indices)\n"
        "         pipe_resource_reference(&index_buffer, NULL);\n"
        "      return;\n"
        "   }\n"
    )
    compute_old = (
        "      ctx->current_compute_pso = d3d12_get_compute_pipeline_state(ctx);\n"
        "      assert(ctx->current_compute_pso);\n"
        "   }\n"
    )
    compute_new = compute_old + "   if (!ctx->current_compute_pso)\n      return;\n"
    for old in (gfx_old, compute_old):
        if source.count(old) != 1:
            raise RuntimeError("Pinned Mesa d3d12_draw.cpp does not match the null PSO patch")
    path.write_text(source.replace(gfx_old, gfx_new).replace(compute_old, compute_new))


def patch_query(root: Path) -> None:
    query_dir = root / "src/gallium/drivers/d3d12"
    # begin_subquery() accumulates a full query heap with a compute pass; saving and restoring the
    # compute state suspends and resumes every active query, which re-enters begin_subquery() for
    # the same subquery while curr_query still equals num_queries. That recursed until the stack
    # overflowed (about 15,000 nested cycles, after the query heap filled at frame 12).
    query_dir = root / "src/gallium/drivers/d3d12"
    header = query_dir / "d3d12_query.h"
    header_source = header.read_text()
    field_old = "   bool active;\n};\n\nstruct d3d12_query {"
    field_new = "   bool active;\n   bool accumulating;\n};\n\nstruct d3d12_query {"
    if field_old not in header_source:
        raise RuntimeError("Pinned Mesa d3d12_query.h does not match the reentrancy patch")
    header.write_text(header_source.replace(field_old, field_new))

    query = query_dir / "d3d12_query.cpp"
    query_source = query.read_text()
    begin_old = (
        "   if (q->curr_query == q->num_queries) {\n"
        "      /* Accumulate current results and store in first slot */\n"
        "      accumulate_subresult_gpu(ctx, q_parent, sub_query);\n"
        "      q->curr_query = 1;\n"
        "   }\n"
    )
    begin_new = (
        "   if (q->curr_query == q->num_queries) {\n"
        "      /* Accumulating resumes the active queries, which re-enters this function for the\n"
        "       * same subquery; the outer call begins it once the accumulation is done. */\n"
        "      if (q->accumulating)\n"
        "         return;\n"
        "      q->accumulating = true;\n"
        "      /* Accumulate current results and store in first slot */\n"
        "      accumulate_subresult_gpu(ctx, q_parent, sub_query);\n"
        "      q->accumulating = false;\n"
        "      q->curr_query = 1;\n"
        "   }\n"
    )
    if begin_old not in query_source:
        raise RuntimeError("Pinned Mesa d3d12_query.cpp does not match the reentrancy patch")
    query_source = query_source.replace(begin_old, begin_new)
    # The accumulation above resumes every active query, which begins the *other* subqueries of
    # this query too (e.g. the stream-output statistics half of PRIMITIVES_GENERATED); the outer
    # begin_query() loop then began them a second time. D3D12 rejects BeginQuery on an index
    # that is already open, and Close() then fails with "queries outstanding", dropping the
    # whole batch and, through list reuse, every batch after it. Beginning must be idempotent.
    guard_old = (
        "   struct d3d12_query_impl *q = &q_parent->subqueries[sub_query];\n"
        "   if (q->curr_query == q->num_queries) {\n"
    )
    guard_new = (
        "   struct d3d12_query_impl *q = &q_parent->subqueries[sub_query];\n"
        "   if (q->active && q_parent->type != PIPE_QUERY_TIMESTAMP)\n"
        "      return;\n"
        "   if (q->curr_query == q->num_queries) {\n"
    )
    if query_source.count(guard_old) != 1:
        raise RuntimeError("Pinned Mesa d3d12_query.cpp does not match the double-begin patch")
    query.write_text(query_source.replace(guard_old, guard_new))

def patch_fence(root: Path) -> None:
    query_dir = root / "src/gallium/drivers/d3d12"
    # fence_finish() dereferences the fence unconditionally, but the GL frontend reaches it with a
    # null handle when a flush had nothing to submit (access violation at d3d12_fence.cpp:113,
    # reading offset 0x28 of null). A null fence means there is no outstanding work: complete.
    fence = query_dir / "d3d12_fence.cpp"
    fence_source = fence.read_text()
    fence_old = "   bool ret = d3d12_fence_finish(d3d12_fence(pfence), timeout_ns);\n"
    fence_new = (
        "   if (!pfence)\n"
        "      return true;\n"
        "   bool ret = d3d12_fence_finish(d3d12_fence(pfence), timeout_ns);\n"
    )
    if fence_old not in fence_source:
        raise RuntimeError("Pinned Mesa d3d12_fence.cpp does not match the null-fence patch")
    fence.write_text(fence_source.replace(fence_old, fence_new))


def patch_pso(root: Path) -> None:
    # A failed CreateGraphicsPipelineState only reaches debug_printf, which is invisible on the
    # Xbox, and the draw then silently produces nothing. Publish counters and the last HRESULT
    # in the process environment so the frontend can log them.
    pso = root / "src/gallium/drivers/d3d12/d3d12_pipeline_state.cpp"
    source = pso.read_text()
    anchor = "static ID3D12PipelineState *\ncreate_gfx_pipeline_state(struct d3d12_context *ctx)\n"
    helper = (
        "#include <stdio.h>\n\n"
        "static void\n"
        "nxbox_report_pso(bool ok, HRESULT hr)\n"
        "{\n"
        "   static long created = 0, failed = 0, last_hr = 0;\n"
        "   if (ok)\n"
        "      created++;\n"
        "   else {\n"
        "      failed++;\n"
        "      last_hr = (long)hr;\n"
        "   }\n"
        "   if (!ok || created <= 4 || (created & (created - 1)) == 0) {\n"
        "      char text[96];\n"
        "      snprintf(text, sizeof(text), \"created=%ld failed=%ld last_hr=0x%08lx\",\n"
        "               created, failed, (unsigned long)last_hr);\n"
        "      SetEnvironmentVariableA(\"NXBOX_D3D12_PSO\", text);\n"
        "   }\n"
        "}\n\n"
    )
    stream_old = (
        "      if (FAILED(screen->dev->CreatePipelineState(&pso_stream_desc,\n"
        "                                                  IID_PPV_ARGS(&ret)))) {\n"
        "         debug_printf(\"D3D12: CreateGraphicsPipelineState failed!\\n\");\n"
        "         return NULL;\n"
        "      }\n"
    )
    stream_new = (
        "      HRESULT nxbox_hr = screen->dev->CreatePipelineState(&pso_stream_desc,\n"
        "                                                          IID_PPV_ARGS(&ret));\n"
        "      nxbox_report_pso(SUCCEEDED(nxbox_hr), nxbox_hr);\n"
        "      if (FAILED(nxbox_hr))\n"
        "         return NULL;\n"
    )
    v0_old = (
        "      if (FAILED(screen->dev->CreateGraphicsPipelineState(&v0desc,\n"
        "                                                       IID_PPV_ARGS(&ret)))) {\n"
        "         debug_printf(\"D3D12: CreateGraphicsPipelineState failed!\\n\");\n"
        "         return NULL;\n"
        "      }\n"
    )
    v0_new = (
        "      HRESULT nxbox_hr = screen->dev->CreateGraphicsPipelineState(&v0desc,\n"
        "                                                               IID_PPV_ARGS(&ret));\n"
        "      nxbox_report_pso(SUCCEEDED(nxbox_hr), nxbox_hr);\n"
        "      if (FAILED(nxbox_hr))\n"
        "         return NULL;\n"
    )
    for old in (anchor, stream_old, v0_old):
        if old not in source:
            raise RuntimeError("Pinned Mesa d3d12_pipeline_state.cpp does not match the PSO patch")
    source = source.replace(anchor, helper + anchor)
    source = source.replace(stream_old, stream_new).replace(v0_old, v0_new)
    pso.write_text(source)


def patch_dxil(root: Path) -> None:
    # Without DXIL.dll Mesa emits unsigned DXIL, which the D3D12 runtime rejects when the PSO is
    # created; the validator failures also only reach debug_printf. Publish whether DXIL.dll
    # loaded and how validation went, for the frontend to log.
    path = root / "src/microsoft/compiler/dxil_validator.cpp"
    source = path.read_text()
    anchor = "static HMODULE\nload_dxil_mod()\n"
    helper = (
        "#include <stdio.h>\n\n"
        "static const char *nxbox_dxil_source = \"search_path\";\n\n"
        "static void\n"
        "nxbox_report_dxil(const char *stage, unsigned long detail)\n"
        "{\n"
        "   char text[96];\n"
        "   snprintf(text, sizeof(text), \"%s detail=0x%08lx\", stage, detail);\n"
        "   SetEnvironmentVariableA(\"NXBOX_DXIL\", text);\n"
        "}\n\n"
        "static void\n"
        "nxbox_report_validation(HRESULT hr)\n"
        "{\n"
        "   static long passed = 0, failed = 0, last_hr = 0;\n"
        "   if (SUCCEEDED(hr))\n"
        "      passed++;\n"
        "   else {\n"
        "      failed++;\n"
        "      last_hr = (long)hr;\n"
        "   }\n"
        "   if (FAILED(hr) || passed <= 4 || (passed & (passed - 1)) == 0) {\n"
        "      char text[96];\n"
        "      snprintf(text, sizeof(text), \"passed=%ld failed=%ld last_hr=0x%08lx\",\n"
        "               passed, failed, (unsigned long)last_hr);\n"
        "      SetEnvironmentVariableA(\"NXBOX_DXIL_VALIDATE\", text);\n"
        "   }\n"
        "}\n\n"
    )
    # A UWP process may only load package DLLs through LoadPackagedLibrary.
    packaged_old = "   return LoadLibraryA(self_path);\n}\n"
    packaged_new = (
        "   nxbox_dxil_source = \"next_to_module\";\n"
        "   HMODULE local = LoadLibraryA(self_path);\n"
        "   if (local)\n"
        "      return local;\n"
        "   nxbox_dxil_source = \"packaged\";\n"
        "   return LoadPackagedLibrary(L\"dxil.dll\", 0);\n"
        "}\n"
    )
    replacements = {
        anchor: helper + anchor,
        packaged_old: packaged_new,
        "   val->dxil_mod = load_dxil_mod();\n   if (!val->dxil_mod) {\n":
            "   val->dxil_mod = load_dxil_mod();\n   if (!val->dxil_mod) {\n"
            "      nxbox_report_dxil(\"load_failed\", GetLastError());\n",
        "   val->dxc_validator = create_dxc_validator(val->dxil_mod);\n   if (!val->dxc_validator)\n      goto fail;\n":
            "   val->dxc_validator = create_dxc_validator(val->dxil_mod);\n   if (!val->dxc_validator) {\n"
            "      nxbox_report_dxil(\"validator_failed\", 0);\n      goto fail;\n   }\n",
        "   val->version = get_filtered_validator_version(\n      val->dxil_mod,\n      get_validator_version(val->dxc_validator));\n":
            "   val->version = get_filtered_validator_version(\n      val->dxil_mod,\n      get_validator_version(val->dxc_validator));\n"
            "   nxbox_report_dxil(nxbox_dxil_source, val->version);\n",
        "   HRESULT hr;\n   result->GetStatus(&hr);\n":
            "   HRESULT hr;\n   result->GetStatus(&hr);\n   nxbox_report_validation(hr);\n",
    }
    for old, new in replacements.items():
        if old not in source:
            raise RuntimeError(f"Pinned Mesa dxil_validator.cpp does not match patch: {old[:60]}")
        source = source.replace(old, new, 1)
    path.write_text(source)

    compiler = root / "src/gallium/drivers/d3d12/d3d12_compiler.cpp"
    compiler_source = compiler.read_text()
    error_old = (
        "               \"== END ==========================================================\\n\",\n"
        "               err);\n"
    )
    error_new = error_old + (
        "            static bool nxbox_error_reported = false;\n"
        "            if (!nxbox_error_reported) {\n"
        "               nxbox_error_reported = true;\n"
        "               char nxbox_text[400];\n"
        "               snprintf(nxbox_text, sizeof(nxbox_text), \"%s\", err);\n"
        "               for (char *c = nxbox_text; *c; ++c)\n"
        "                  if (*c == '\\n' || *c == '\\r') *c = ' ';\n"
        "               SetEnvironmentVariableA(\"NXBOX_DXIL_ERROR\", nxbox_text);\n"
        "            }\n"
    )
    if error_old not in compiler_source:
        raise RuntimeError("Pinned Mesa d3d12_compiler.cpp does not match the error report patch")
    compiler.write_text(compiler_source.replace(error_old, error_new, 1))



def patch_shader_model(root: Path) -> None:
    # The pinned Mesa emits DXIL for the highest model the device reports (6.8 on the Xbox);
    # NXBOX_D3D12_MAX_SM=<minor> caps it (e.g. 7 for 6.7) so models can be bisected at runtime.
    path = root / "src/gallium/drivers/d3d12/d3d12_screen.cpp"
    source = path.read_text()
    old = "         break;\n      }\n   }\n\n   D3D12_COMMAND_QUEUE_DESC queue_desc;\n"
    new = (
        "         break;\n      }\n   }\n"
        "   {\n"
        "      const char *nxbox_sm = getenv(\"NXBOX_D3D12_MAX_SM\");\n"
        "      if (nxbox_sm && *nxbox_sm) {\n"
        "         unsigned nxbox_minor = (unsigned)strtoul(nxbox_sm, NULL, 10);\n"
        "         dxil_shader_model nxbox_cap = (dxil_shader_model)(SHADER_MODEL_6_0 + nxbox_minor);\n"
        "         if (nxbox_cap < screen->max_shader_model)\n"
        "            screen->max_shader_model = nxbox_cap;\n"
        "      }\n"
        "      char nxbox_text[32];\n"
        "      snprintf(nxbox_text, sizeof(nxbox_text), \"0x%x\", (unsigned)screen->max_shader_model);\n"
        "      SetEnvironmentVariableA(\"NXBOX_D3D12_SM\", nxbox_text);\n"
        "   }\n"
        "\n   D3D12_COMMAND_QUEUE_DESC queue_desc;\n"
    )
    if source.count(old) != 1:
        raise RuntimeError("Pinned Mesa d3d12_screen.cpp does not match the shader model patch")
    path.write_text(source.replace(old, new))


def patch_format_cast_report(root: Path) -> None:
    # Whether the device relaxes format casting decides if sRGB textures can be written through a
    # typed UAV (the GPU ASTC decoder does that); without it they get no UAV flag at all.
    path = root / "src/gallium/drivers/d3d12/d3d12_screen.cpp"
    source = path.read_text()
    old = "   screen->dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &screen->opts12, sizeof(screen->opts12));\n"
    new = old + (
        "   SetEnvironmentVariableA(\"NXBOX_D3D12_RELAXED_CAST\",\n"
        "                           screen->opts12.RelaxedFormatCastingSupported ? \"1\" : \"0\");\n"
    )
    if source.count(old) != 1:
        raise RuntimeError("Pinned Mesa d3d12_screen.cpp does not match the format cast report patch")
    path.write_text(source.replace(old, new))


def patch_draw(root: Path) -> None:
    # Count every silent exit of d3d12_draw_vbo and snapshot the state of issued draws, so the
    # frontend can tell whether draws are dropped or issued with state that yields no pixels.
    path = root / "src/gallium/drivers/d3d12/d3d12_draw.cpp"
    source = path.read_text()
    helper = r"""
#include <stdio.h>

static long nxbox_draw_count[6];

static void
nxbox_draw_snapshot(struct d3d12_context *ctx, const struct pipe_draw_info *dinfo,
                    const struct pipe_draw_start_count_bias *draws, const char *name)
{
   struct d3d12_rasterizer_state *rast = ctx->gfx_pipeline_state.rast;
   struct d3d12_blend_state *blend = ctx->gfx_pipeline_state.blend;
   char text[400];
   snprintf(text, sizeof(text),
            "calls=%ld zero=%ld prim=%ld cullfab=%ld sofail=%ld issued=%ld mode=%u count=%u "
            "pred=%d disc=%d clip=0x%x smask=0x%x cb=%u zs=%d fb=%ux%u vp=%.0fx%.0f@%.0f,%.0f "
            "depth=%.2f-%.2f sc=%d:%ld,%ld-%ld,%ld wm=0x%x cull=%d fill=%d so=%u",
            nxbox_draw_count[0], nxbox_draw_count[1], nxbox_draw_count[2], nxbox_draw_count[3],
            nxbox_draw_count[4], nxbox_draw_count[5], (unsigned)dinfo->mode, draws[0].count,
            ctx->current_predication != NULL, rast ? (int)rast->base.rasterizer_discard : -1,
            rast ? (unsigned)rast->base.clip_plane_enable : 0u, ctx->gfx_pipeline_state.sample_mask,
            ctx->fb.nr_cbufs, ctx->fb.zsbuf != NULL, ctx->fb.width, ctx->fb.height,
            ctx->viewports[0].Width, ctx->viewports[0].Height, ctx->viewports[0].TopLeftX,
            ctx->viewports[0].TopLeftY, ctx->viewports[0].MinDepth, ctx->viewports[0].MaxDepth,
            rast ? (int)rast->base.scissor : -1, (long)ctx->scissors[0].left,
            (long)ctx->scissors[0].top, (long)ctx->scissors[0].right, (long)ctx->scissors[0].bottom,
            blend ? (unsigned)blend->desc.RenderTarget[0].RenderTargetWriteMask : 0u,
            rast ? (int)rast->desc.CullMode : -1, rast ? (int)rast->desc.FillMode : -1,
            ctx->gfx_pipeline_state.num_so_targets);
   SetEnvironmentVariableA(name, text);
}

"""
    anchor = "void\nd3d12_draw_vbo(struct pipe_context *pctx,\n"
    replacements = [
        (anchor, helper + anchor),
        ("   if (!indirect && (!draws[0].count || !dinfo->instance_count))\n      return;\n",
         "   nxbox_draw_count[0]++;\n"
         "   if (!indirect && (!draws[0].count || !dinfo->instance_count)) {\n"
         "      nxbox_draw_count[1]++;\n      return;\n   }\n"),
        ("      util_primconvert_draw_vbo(ctx->primconvert, dinfo, drawid_offset, indirect, draws, num_draws);\n      return;\n",
         "      nxbox_draw_count[2]++;\n"
         "      util_primconvert_draw_vbo(ctx->primconvert, dinfo, drawid_offset, indirect, draws, num_draws);\n      return;\n"),
        ("       ctx->gfx_pipeline_state.rast->base.cull_face == PIPE_FACE_FRONT_AND_BACK)\n      return;\n",
         "       ctx->gfx_pipeline_state.rast->base.cull_face == PIPE_FACE_FRONT_AND_BACK) {\n"
         "      nxbox_draw_count[3]++;\n      return;\n   }\n"),
        ("      debug_printf(\"validate_stream_output_targets() failed\\n\");\n      return;\n",
         "      debug_printf(\"validate_stream_output_targets() failed\\n\");\n"
         "      nxbox_draw_count[4]++;\n      return;\n"),
        ("   if (indirect) {\n      unsigned draw_count = draw_auto ? 1 : indirect->draw_count;\n",
         "   nxbox_draw_count[5]++;\n"
         "   if ((nxbox_draw_count[5] & 63) == 1)\n"
         "      nxbox_draw_snapshot(ctx, dinfo, draws, \"NXBOX_D3D12_DRAW\");\n"
         "   if (!indirect && dinfo->mode == MESA_PRIM_TRIANGLE_STRIP && draws[0].count == 4)\n"
         "      nxbox_draw_snapshot(ctx, dinfo, draws, \"NXBOX_D3D12_QUAD\");\n"
         "   if (indirect) {\n      unsigned draw_count = draw_auto ? 1 : indirect->draw_count;\n"),
    ]
    for old, new in replacements:
        if source.count(old) != 1:
            raise RuntimeError(f"Pinned Mesa d3d12_draw.cpp does not match the draw patch: {old[:60]}")
        source = source.replace(old, new)
    path.write_text(source)


def patch_batch(root: Path) -> None:
    # A failed ID3D12GraphicsCommandList::Close() only reaches debug_printf: the batch is never
    # executed and gets no fence, so every draw, clear and copy in it silently disappears.
    # Publish close failures, the device removed reason and the last debug layer message.
    path = root / "src/gallium/drivers/d3d12/d3d12_batch.cpp"
    source = path.read_text()
    helper = r"""
#include <directx/d3d12sdklayers.h>
#include <stdio.h>
#include <stdlib.h>

static void
nxbox_report_batch(struct d3d12_screen *screen, HRESULT close_hr)
{
   static long batches = 0, close_failed = 0, last_hr = 0;
   batches++;
   if (FAILED(close_hr)) {
      close_failed++;
      last_hr = (long)close_hr;
   }
   const bool report = FAILED(close_hr) ? (close_failed <= 8 || (close_failed & (close_failed - 1)) == 0)
                                        : (batches <= 4 || (batches & 255) == 0);
   if (!report)
      return;
   char text[160];
   snprintf(text, sizeof(text), "batches=%ld close_failed=%ld close_hr=0x%08lx removed=0x%08lx",
            batches, close_failed, (unsigned long)last_hr,
            (unsigned long)screen->dev->GetDeviceRemovedReason());
   SetEnvironmentVariableA("NXBOX_D3D12_BATCH", text);
   if (!FAILED(close_hr))
      return;
   ID3D12InfoQueue *queue;
   if (FAILED(screen->dev->QueryInterface(IID_PPV_ARGS(&queue))))
      return;
   UINT64 count = queue->GetNumStoredMessages();
   /* The first failure starts the cascade: keep its last three messages separately. */
   const bool first = close_failed == 1;
   char joined[1000] = "";
   size_t used = 0;
   for (UINT64 i = count > 3 ? count - 3 : 0; i < count; ++i) {
      SIZE_T length = 0;
      queue->GetMessage(i, NULL, &length);
      D3D12_MESSAGE *message = (D3D12_MESSAGE *)malloc(length);
      if (message && SUCCEEDED(queue->GetMessage(i, message, &length)) && used < sizeof(joined)) {
         int n = snprintf(joined + used, sizeof(joined) - used, "%s[id=%d %s]", used ? " " : "",
                          (int)message->ID, message->pDescription);
         if (n > 0)
            used += (size_t)n;
      }
      free(message);
   }
   SetEnvironmentVariableA(first ? "NXBOX_D3D12_FIRST_MESSAGE" : "NXBOX_D3D12_MESSAGE", joined);
   if (first) {
      char where[64];
      snprintf(where, sizeof(where), "batch=%ld stored=%llu", batches, (unsigned long long)count);
      SetEnvironmentVariableA("NXBOX_D3D12_FIRST_FAILURE", where);
   }
   queue->Release();
}

static void
nxbox_report_reset(const char *what, HRESULT hr)
{
   static long list_failed = 0, alloc_failed = 0, last_hr = 0;
   if (what[0] == 'l')
      list_failed++;
   else
      alloc_failed++;
   last_hr = (long)hr;
   char text[128];
   snprintf(text, sizeof(text), "list_reset_failed=%ld alloc_reset_failed=%ld last_hr=0x%08lx",
            list_failed, alloc_failed, (unsigned long)last_hr);
   SetEnvironmentVariableA("NXBOX_D3D12_RESET", text);
}

"""
    anchor = "void\nd3d12_end_batch(struct d3d12_context *ctx, struct d3d12_batch *batch)\n"
    close_old = (
        "   if (FAILED(ctx->cmdlist->Close())) {\n"
        "      debug_printf(\"D3D12: closing ID3D12GraphicsCommandList failed\\n\");\n"
    )
    close_new = (
        "   HRESULT nxbox_close_hr = ctx->cmdlist->Close();\n"
        "   nxbox_report_batch(screen, nxbox_close_hr);\n"
        "   if (FAILED(nxbox_close_hr)) {\n"
        "      debug_printf(\"D3D12: closing ID3D12GraphicsCommandList failed\\n\");\n"
    )
    alloc_old = (
        "   if (FAILED(batch->cmdalloc->Reset())) {\n"
        "      debug_printf(\"D3D12: resetting ID3D12CommandAllocator failed\\n\");\n"
    )
    alloc_new = (
        "   HRESULT nxbox_alloc_hr = batch->cmdalloc->Reset();\n"
        "   if (FAILED(nxbox_alloc_hr)) {\n"
        "      nxbox_report_reset(\"allocator\", nxbox_alloc_hr);\n"
        "      debug_printf(\"D3D12: resetting ID3D12CommandAllocator failed\\n\");\n"
    )
    list_old = (
        "      if (FAILED(ctx->cmdlist->Reset(batch->cmdalloc, NULL))) {\n"
        "         debug_printf(\"D3D12: resetting ID3D12GraphicsCommandList failed\\n\");\n"
    )
    list_new = (
        "      HRESULT nxbox_list_hr = ctx->cmdlist->Reset(batch->cmdalloc, NULL);\n"
        "      if (FAILED(nxbox_list_hr)) {\n"
        "         nxbox_report_reset(\"list\", nxbox_list_hr);\n"
        "         debug_printf(\"D3D12: resetting ID3D12GraphicsCommandList failed\\n\");\n"
    )
    # The helper must precede d3d12_reset_batch, the first function that uses it.
    reset_anchor = "bool\nd3d12_reset_batch(struct d3d12_context *ctx, struct d3d12_batch *batch, uint64_t timeout_ns)\n"
    # NXBOX_D3D12_RECOVER=1: drop a command list whose Close() failed and let the next batch
    # create a fresh one, instead of trying to reuse it (which cascades into every later batch).
    recover_old = (
        "      batch->has_errors = true;\n"
        "      return;\n"
        "   }\n\n"
        "   mtx_lock(&screen->submit_mutex);\n"
    )
    recover_new = (
        "      batch->has_errors = true;\n"
        "      if (getenv(\"NXBOX_D3D12_RECOVER\")) {\n"
        "         if (ctx->cmdlist2) {\n"
        "            ctx->cmdlist2->Release();\n"
        "            ctx->cmdlist2 = nullptr;\n"
        "         }\n"
        "         if (ctx->cmdlist8) {\n"
        "            ctx->cmdlist8->Release();\n"
        "            ctx->cmdlist8 = nullptr;\n"
        "         }\n"
        "         ctx->cmdlist->Release();\n"
        "         ctx->cmdlist = nullptr;\n"
        "      }\n"
        "      return;\n"
        "   }\n\n"
        "   mtx_lock(&screen->submit_mutex);\n"
    )
    for old in (reset_anchor, anchor, close_old, alloc_old, list_old, recover_old):
        if source.count(old) != 1:
            raise RuntimeError(f"Pinned Mesa d3d12_batch.cpp does not match the batch patch: {old[:50]}")
    source = source.replace(reset_anchor, helper + reset_anchor)
    source = source.replace(close_old, close_new).replace(alloc_old, alloc_new).replace(list_old, list_new)
    source = source.replace(recover_old, recover_new)
    path.write_text(source)

    # Context destruction ends the batch and then releases the list, which recovery may have
    # already dropped.
    context = root / "src/gallium/drivers/d3d12/d3d12_context.cpp"
    context_source = context.read_text()
    release_old = "      d3d12_destroy_batch(ctx, &ctx->batches[i]);\n   ctx->cmdlist->Release();\n"
    release_new = "      d3d12_destroy_batch(ctx, &ctx->batches[i]);\n   if (ctx->cmdlist)\n      ctx->cmdlist->Release();\n"
    if context_source.count(release_old) != 1:
        raise RuntimeError("Pinned Mesa d3d12_context.cpp does not match the recovery patch")
    context.write_text(context_source.replace(release_old, release_new))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    patch(parser.parse_args().root)
