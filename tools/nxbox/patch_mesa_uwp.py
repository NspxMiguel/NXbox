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
    patch_bisect_switches(root)
    patch_image_slot(root)
    patch_root_signature_report(root)
    patch_batch(root)
    patch_dred(root)
    if "fence" in SKIP:
        print("skipping the null fence patch")
    else:
        patch_fence(root)
    patch_lifetime(root)
    patch_sync_batch(root)
    patch_batch_reuse(root)
    patch_render_safety(root)
    patch_invalid_commands(root)
    patch_first_bad_batch(root)
    patch_heap_policy(root)
    patch_query_policy(root)
    patch_device_api_ring(root)
    patch_bo_counters(root)
    patch_vidmem_report(root)
    patch_no_evict(root)
    patch_resident_create(root)
    patch_view_cast(root)


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
    gfx_new = gfx_old.replace("      assert(ctx->current_gfx_pso);\n", "") + (
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
    compute_new = (
        compute_old.replace("      assert(ctx->current_compute_pso);\n", "")
        + "   if (!ctx->current_compute_pso)\n      return;\n"
    )
    for old in (gfx_old, compute_old):
        if source.count(old) != 1:
            raise RuntimeError("Pinned Mesa d3d12_draw.cpp does not match the null PSO patch")
    path.write_text(source.replace(gfx_old, gfx_new).replace(compute_old, compute_new))


def patch_root_signature_report(root: Path) -> None:
    # A null root signature made Breath of the Wild's pipeline states fail (E_INVALIDARG, rs=0
    # in the rejected description). Its creation only reaches debug_printf; publish the
    # serializer's own error text, or the CreateRootSignature HRESULT, and the parameter count.
    path = root / "src/gallium/drivers/d3d12/d3d12_root_signature.cpp"
    source = path.read_text()
    helper = (
        "#include <stdio.h>\n#include <string.h>\n\n"
        "static void\n"
        "nxbox_report_root_signature(const char *what, HRESULT hr, ID3DBlob *error, unsigned params)\n"
        "{\n"
        "   char text[400];\n"
        "   int n = snprintf(text, sizeof(text), \"rootsig %s hr=0x%08lx params=%u \", what,\n"
        "                    (unsigned long)hr, params);\n"
        "   if (error && n > 0 && n < (int)sizeof(text)) {\n"
        "      size_t room = sizeof(text) - (size_t)n - 1;\n"
        "      size_t size = error->GetBufferSize();\n"
        "      memcpy(text + n, error->GetBufferPointer(), size < room ? size : room);\n"
        "      text[n + (size < room ? size : room)] = 0;\n"
        "   }\n"
        "   SetEnvironmentVariableA(\"NXBOX_D3D12_ROOTSIG\", text);\n"
        "}\n\n"
    )
    anchor = "static ID3D12RootSignature *\ncreate_root_signature(struct d3d12_context *ctx, struct d3d12_root_signature_key *key)\n"
    ser_old = (
        "      if (FAILED(ctx->D3D12SerializeVersionedRootSignature(&root_sig_desc,\n"
        "                                                           &sig, &error))) {\n"
        "         debug_printf(\"D3D12SerializeRootSignature failed\\n\");\n"
        "         return NULL;\n"
        "      }\n"
    )
    ser_new = (
        "      HRESULT nxbox_hr = ctx->D3D12SerializeVersionedRootSignature(&root_sig_desc,\n"
        "                                                                   &sig, &error);\n"
        "      if (FAILED(nxbox_hr)) {\n"
        "         nxbox_report_root_signature(\"serialize\", nxbox_hr, error.Get(), num_params);\n"
        "         return NULL;\n"
        "      }\n"
    )
    create_old = (
        "   if (FAILED(screen->dev->CreateRootSignature(0,\n"
        "                                               sig->GetBufferPointer(),\n"
        "                                               sig->GetBufferSize(),\n"
        "                                               IID_PPV_ARGS(&ret)))) {\n"
        "      debug_printf(\"CreateRootSignature failed\\n\");\n"
        "      return NULL;\n"
        "   }\n"
    )
    create_new = (
        "   HRESULT nxbox_create_hr = screen->dev->CreateRootSignature(0, sig->GetBufferPointer(),\n"
        "                                                              sig->GetBufferSize(),\n"
        "                                                              IID_PPV_ARGS(&ret));\n"
        "   if (FAILED(nxbox_create_hr)) {\n"
        "      /* The device removed reason tells a lost device from a bad signature. */\n"
        "      nxbox_report_root_signature(\"create\", nxbox_create_hr, NULL, num_params);\n"
        "      char nxbox_removed[64];\n"
        "      snprintf(nxbox_removed, sizeof(nxbox_removed), \"0x%08lx\",\n"
        "               (unsigned long)screen->dev->GetDeviceRemovedReason());\n"
        "      SetEnvironmentVariableA(\"NXBOX_D3D12_REMOVED\", nxbox_removed);\n"
        "      return NULL;\n"
        "   }\n"
    )
    for old in (anchor, ser_old, create_old):
        if source.count(old) != 1:
            raise RuntimeError("Pinned Mesa d3d12_root_signature.cpp does not match the report patch")
    source = source.replace(anchor, helper + anchor).replace(ser_old, ser_new)
    path.write_text(source.replace(create_old, create_new))


def patch_image_slot(root: Path) -> None:
    # d3d12_set_shader_images() stores the image view at ctx->image_views[shader][i + start_slot]
    # but its format-emulation entry at [i]; the draw and compile paths read both at the slot
    # index. With start_slot != 0 the view then got another slot's format (or none), a wrong UAV
    # format. This device reports RelaxedFormatCastingSupported=0, so the emulation path is used.
    path = root / "src/gallium/drivers/d3d12/d3d12_context.cpp"
    source = path.read_text()
    reset_old = "      ctx->image_view_emulation_formats[shader][i] = PIPE_FORMAT_NONE;\n"
    reset_new = "      ctx->image_view_emulation_formats[shader][i + start_slot] = PIPE_FORMAT_NONE;\n"
    set_old = "            ctx->image_view_emulation_formats[shader][i] =\n"
    set_new = "            ctx->image_view_emulation_formats[shader][i + start_slot] =\n"
    for old in (reset_old, set_old):
        if source.count(old) != 1:
            raise RuntimeError("Pinned Mesa d3d12_context.cpp does not match the image slot patch")
    path.write_text(source.replace(reset_old, reset_new).replace(set_old, set_new))


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
    # Cache a degraded PSO under its original Gallium key; never mutate shared state.
    pso = root / "src/gallium/drivers/d3d12/d3d12_pipeline_state.cpp"
    source = pso.read_text()
    anchor = "static ID3D12PipelineState *\ncreate_gfx_pipeline_state(struct d3d12_context *ctx)\n"
    helper = r"""#include <stdio.h>
#include <mutex>
#include <string.h>

static std::mutex nxbox_pso_report_mutex;

static void
nxbox_report_pso(bool ok, HRESULT hr)
{
   std::lock_guard<std::mutex> lock(nxbox_pso_report_mutex);
   static unsigned long long created = 0, failed = 0;
   static unsigned last_hr = 0;
   if (ok)
      created++;
   else {
      failed++;
      last_hr = (unsigned)hr;
   }
   if (!ok || created <= 4 || (created & (created - 1)) == 0) {
      char text[128];
      snprintf(text, sizeof(text), "created=%llu failed=%llu last_hr=0x%08x", created, failed,
               last_hr);
      SetEnvironmentVariableA("NXBOX_D3D12_PSO", text);
   }
}

/* Count color slots, not depth/stencil/sample-mask bits or dual-source indices. */
static unsigned
nxbox_ps_color_count(nir_shader *ps)
{
   if (!ps)
      return 0;
   uint64_t written = ps->info.outputs_written;
   unsigned count = (written & BITFIELD64_BIT(FRAG_RESULT_COLOR)) ? 1 : 0;
   for (unsigned i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
      if (written & BITFIELD64_BIT(FRAG_RESULT_DATA0 + i))
         count = i + 1;
   }
   return count;
}

static UINT8
nxbox_rtv_write_mask(DXGI_FORMAT format)
{
   switch (format) {
   case DXGI_FORMAT_UNKNOWN: return 0;
   case DXGI_FORMAT_A8_UNORM: return D3D12_COLOR_WRITE_ENABLE_ALPHA;
   case DXGI_FORMAT_R8_UNORM:
   case DXGI_FORMAT_R8_SNORM:
   case DXGI_FORMAT_R8_UINT:
   case DXGI_FORMAT_R8_SINT:
   case DXGI_FORMAT_R16_FLOAT:
   case DXGI_FORMAT_R16_UNORM:
   case DXGI_FORMAT_R16_SNORM:
   case DXGI_FORMAT_R16_UINT:
   case DXGI_FORMAT_R16_SINT:
   case DXGI_FORMAT_R32_FLOAT:
   case DXGI_FORMAT_R32_UINT:
   case DXGI_FORMAT_R32_SINT: return D3D12_COLOR_WRITE_ENABLE_RED;
   case DXGI_FORMAT_R8G8_UNORM:
   case DXGI_FORMAT_R8G8_SNORM:
   case DXGI_FORMAT_R8G8_UINT:
   case DXGI_FORMAT_R8G8_SINT:
   case DXGI_FORMAT_R16G16_FLOAT:
   case DXGI_FORMAT_R16G16_UNORM:
   case DXGI_FORMAT_R16G16_SNORM:
   case DXGI_FORMAT_R16G16_UINT:
   case DXGI_FORMAT_R16G16_SINT:
   case DXGI_FORMAT_R32G32_FLOAT:
   case DXGI_FORMAT_R32G32_UINT:
   case DXGI_FORMAT_R32G32_SINT:
      return D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN;
   case DXGI_FORMAT_R11G11B10_FLOAT:
   case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
   case DXGI_FORMAT_R32G32B32_FLOAT:
   case DXGI_FORMAT_R32G32B32_UINT:
   case DXGI_FORMAT_R32G32B32_SINT:
   case DXGI_FORMAT_B5G6R5_UNORM:
   case DXGI_FORMAT_B8G8R8X8_UNORM:
   case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
      return D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN |
             D3D12_COLOR_WRITE_ENABLE_BLUE;
   default: return D3D12_COLOR_WRITE_ENABLE_ALL;
   }
}

/* Report the ORIGINAL rejection. Each value fits the frontend's 512-byte buffer. */
static unsigned long long
nxbox_report_pso_desc(struct d3d12_screen *screen, const D3D12_GRAPHICS_PIPELINE_STATE_DESC &d,
                      struct d3d12_shader *shader)
{
   std::lock_guard<std::mutex> lock(nxbox_pso_report_mutex);
   static unsigned long long serial = 0;
   const unsigned long long id = ++serial;
   const auto &b = d.BlendState.RenderTarget[0];
   const auto &r = d.RasterizerState;
   char text[512];
   snprintf(text, sizeof(text),
            "id=%llu rt=%u fmt=%d,%d,%d,%d dsv=%d samples=%u/%u topo=%d "
            "vs=%zu hs=%zu ds=%zu gs=%zu ps=%zu in=%u so=%u/%u rs=%p z/s=%d/%d "
            "ind=%d b/l=%d/%d rgb=%d/%d/%d a=%d/%d/%d op=%d wm=%x atc=%d "
            "rast=%d/%d/%d/%d/%.3g/%.3g/%d/%d/%d/%u/%d sm=%08x cut=%d node=%x flags=%x",
            id, d.NumRenderTargets, d.RTVFormats[0], d.RTVFormats[1], d.RTVFormats[2],
            d.RTVFormats[3], d.DSVFormat, d.SampleDesc.Count, d.SampleDesc.Quality,
            d.PrimitiveTopologyType, d.VS.BytecodeLength, d.HS.BytecodeLength, d.DS.BytecodeLength,
            d.GS.BytecodeLength, d.PS.BytecodeLength, d.InputLayout.NumElements,
            d.StreamOutput.NumEntries, d.StreamOutput.NumStrides, (void *)d.pRootSignature,
            d.DepthStencilState.DepthEnable, d.DepthStencilState.StencilEnable,
            d.BlendState.IndependentBlendEnable, b.BlendEnable, b.LogicOpEnable, b.SrcBlend,
            b.DestBlend, b.BlendOp, b.SrcBlendAlpha, b.DestBlendAlpha, b.BlendOpAlpha, b.LogicOp,
            (unsigned)b.RenderTargetWriteMask, d.BlendState.AlphaToCoverageEnable, r.FillMode,
            r.CullMode, r.FrontCounterClockwise, r.DepthBias, r.DepthBiasClamp,
            r.SlopeScaledDepthBias, r.DepthClipEnable, r.MultisampleEnable, r.AntialiasedLineEnable,
            r.ForcedSampleCount, r.ConservativeRaster, d.SampleMask, d.IBStripCutValue, d.NodeMask,
            (unsigned)d.Flags);
   SetEnvironmentVariableA("NXBOX_D3D12_PSO_FAIL", text);

   nir_shader *ps = d.PS.BytecodeLength && shader ? shader->nir : NULL;
   /* Fixed per-field budgets leave room for all four vertex elements. */
   char outputs[176] = "", inputs[160] = "";
   unsigned output_count = 0;
   if (ps) {
      nir_foreach_variable_with_modes(var, ps, nir_var_shader_out)
      {
         output_count++;
         if (output_count > 8)
            continue;
         const glsl_type *type = glsl_without_array(var->type);
         char kind =
             glsl_type_is_float_16_32_64(type) ? 'f'
             : (glsl_type_is_uint_16_32_64(type) || glsl_get_base_type(type) == GLSL_TYPE_UINT8)
                 ? 'u'
             : glsl_type_is_integer(type) ? 'i'
                                          : '?';
         size_t n = strlen(outputs);
         snprintf(outputs + n, sizeof(outputs) - n, "%s%d.%u.%u:%c%ux%u", n ? "," : "",
                  var->data.location, var->data.index, var->data.location_frac, kind,
                  glsl_type_is_numeric(type) ? glsl_get_bit_size(type) : 0u,
                  glsl_get_vector_elements(type));
      }
   }
   for (unsigned i = 0; i < MIN2(d.InputLayout.NumElements, 4u); ++i) {
      const auto &e = d.InputLayout.pInputElementDescs[i];
      size_t n = strlen(inputs);
      snprintf(inputs + n, sizeof(inputs) - n, "%s%.12s:%u/%d", n ? "," : "",
               e.SemanticName ? e.SemanticName : "null", e.SemanticIndex, e.Format);
   }
   D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {};
   support.Format = d.RTVFormats[0];
   HRESULT support_hr = d.NumRenderTargets && support.Format != DXGI_FORMAT_UNKNOWN
                            ? screen->dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                                                               &support, sizeof(support))
                            : S_FALSE;
   snprintf(text, sizeof(text),
            "id=%llu out=%llx colors=%u vars=%u types=%s cast=%u/%u ia=%s "
            "fmt0cap=%08x/%x/%x fmt4-7=%d,%d,%d,%d",
            id, (unsigned long long)(ps ? ps->info.outputs_written : 0), nxbox_ps_color_count(ps),
            output_count, outputs, shader ? shader->key.fs.cast_to_uint : 0u,
            shader ? shader->key.fs.cast_to_int : 0u, inputs, (unsigned)support_hr,
            (unsigned)support.Support1, (unsigned)support.Support2, d.RTVFormats[4],
            d.RTVFormats[5], d.RTVFormats[6], d.RTVFormats[7]);
   SetEnvironmentVariableA("NXBOX_D3D12_PSO_FAIL2", text);
   return id;
}

static void
nxbox_report_pso_fallback(unsigned long long id, unsigned level, unsigned tried,
                          const HRESULT *results, HRESULT removed)
{
   std::lock_guard<std::mutex> lock(nxbox_pso_report_mutex);
   static unsigned long long counts[6] = {};
   counts[level]++;
   char text[512];
   snprintf(text, sizeof(text),
            "level1=%llu level2=%llu level3=%llu level4=%llu level5=%llu failed=%llu "
            "id=%llu last=%u tried=%02x hr=%08x,%08x,%08x,%08x,%08x removed=%08x",
            counts[1], counts[2], counts[3], counts[4], counts[5], counts[0], id, level, tried,
            (unsigned)results[0], (unsigned)results[1], (unsigned)results[2], (unsigned)results[3],
            (unsigned)results[4], (unsigned)removed);
   SetEnvironmentVariableA("NXBOX_D3D12_PSO_FALLBACK", text);
}

/* Levels 1-4 accumulate; level 5 changes only DSVFormat in the original stream. */
static void
nxbox_simplify_pso(CD3DX12_PIPELINE_STATE_STREAM3 &desc,
                   const CD3DX12_PIPELINE_STATE_STREAM3 &original, unsigned level, nir_shader *ps)
{
   auto &blend = (D3D12_BLEND_DESC &)desc.BlendState;
   auto &rast = (D3D12_RASTERIZER_DESC &)desc.RasterizerState;
   auto &targets = (D3D12_RT_FORMAT_ARRAY &)desc.RTVFormats;
   switch (level) {
   case 1:
      for (auto &rt : blend.RenderTarget)
         rt.LogicOpEnable = false;
      break;
   case 2:
      /* Different format masks require independent state, even with blending off. */
      blend.IndependentBlendEnable = true;
      for (unsigned i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
         auto &rt = blend.RenderTarget[i];
         rt.BlendEnable = false;
         rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
         rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ZERO;
         rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
         rt.RenderTargetWriteMask =
             i < targets.NumRenderTargets ? nxbox_rtv_write_mask(targets.RTFormats[i]) : 0;
      }
      break;
   case 3:
      blend.AlphaToCoverageEnable = false;
      rast.ForcedSampleCount = 0;
      rast.DepthBias = 0;
      rast.DepthBiasClamp = 0;
      rast.SlopeScaledDepthBias = 0;
      break;
   case 4: {
      /* Never invent an RTV for a shader output without an attached color buffer. */
      targets.NumRenderTargets = MIN2(targets.NumRenderTargets, nxbox_ps_color_count(ps));
      for (unsigned i = targets.NumRenderTargets; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
         targets.RTFormats[i] = DXGI_FORMAT_UNKNOWN;
         blend.RenderTarget[i].RenderTargetWriteMask = 0;
      }
      break;
   }
   case 5:
      desc = original;
      desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
      break;
   }
}
"""
    create_old = (
        "   ID3D12PipelineState *ret;\n"
        "\n"
        "   if (screen->opts14.IndependentFrontAndBackStencilRefMaskSupported) {\n"
        "      D3D12_PIPELINE_STATE_STREAM_DESC pso_stream_desc{\n"
        "          sizeof(pso_desc),\n"
        "          &pso_desc\n"
        "      };\n"
        "\n"
        "      if (FAILED(screen->dev->CreatePipelineState(&pso_stream_desc,\n"
        "                                                  IID_PPV_ARGS(&ret)))) {\n"
        '         debug_printf("D3D12: CreateGraphicsPipelineState failed!\\n");\n'
        "         return NULL;\n"
        "      }\n"
        "   } \n"
        "   else {\n"
        "      D3D12_GRAPHICS_PIPELINE_STATE_DESC v0desc = pso_desc.GraphicsDescV0();\n"
        "      if (FAILED(screen->dev->CreateGraphicsPipelineState(&v0desc,\n"
        "                                                       IID_PPV_ARGS(&ret)))) {\n"
        '         debug_printf("D3D12: CreateGraphicsPipelineState failed!\\n");\n'
        "         return NULL;\n"
        "      }\n"
        "   }\n"
        "\n"
        "   return ret;"
    )
    create_new = r"""   const auto nxbox_original_inputs = input_layout;
   D3D12_INPUT_ELEMENT_DESC nxbox_inputs[PIPE_MAX_ATTRIBS * 4];
   nxbox_pso_fix_inputs(pso_desc.VS, input_layout, nxbox_inputs, ARRAY_SIZE(nxbox_inputs));
   nxbox_pso_fix_blend((D3D12_BLEND_DESC &)pso_desc.BlendState);
   nxbox_pso_fix_depth((D3D12_DEPTH_STENCIL_DESC2 &)pso_desc.DepthStencilState);
   /* Quarantine known pairs and related shapes before either driver API. */
   if (nxbox_pso_quarantined(pso_desc.VS, pso_desc.PS) ||
       nxbox_pso_suspect_shape(pso_desc.GraphicsDescV0())) {
      SetEnvironmentVariableA("NXBOX_D3D12_PSO_FIX", "quarantined graphics signature; draw skipped; canary=unverified-isolation");
      return NULL;
   }
   ID3D12PipelineState *ret = NULL;
   cache_entry->fallback_level = 0;
   cache_entry->num_render_targets = render_targets.NumRenderTargets;
   cache_entry->has_dsv = (DXGI_FORMAT)pso_desc.DSVFormat != DXGI_FORMAT_UNKNOWN;
   /* Keep STREAM3 (including stencil masks/view instancing) on the stream path. */
   auto create = [&]() -> HRESULT {
      ret = NULL;
      if (screen->opts14.IndependentFrontAndBackStencilRefMaskSupported) {
         D3D12_PIPELINE_STATE_STREAM_DESC stream = {sizeof(pso_desc), &pso_desc};
         return screen->dev->CreatePipelineState(&stream, IID_PPV_ARGS(&ret));
      }
      D3D12_GRAPHICS_PIPELINE_STATE_DESC v0desc = pso_desc.GraphicsDescV0();
      return screen->dev->CreateGraphicsPipelineState(&v0desc, IID_PPV_ARGS(&ret));
   };
   HRESULT hr = create();
   nxbox_report_pso(SUCCEEDED(hr), hr);
   if (SUCCEEDED(hr))
      return ret;

   auto shader = state->stages[PIPE_SHADER_FRAGMENT];
   const auto original = pso_desc;
   const auto original_v0 = original.GraphicsDescV0();
   const auto id = nxbox_report_pso_desc(screen, original_v0, shader);
   nir_shader *ps = original_v0.PS.BytecodeLength && shader ? shader->nir : NULL;
   HRESULT results[5] = {S_FALSE, S_FALSE, S_FALSE, S_FALSE, S_FALSE};
   unsigned tried = 0;
   HRESULT removed = screen->dev->GetDeviceRemovedReason();
   for (unsigned level = 1; level <= 5 && hr == E_INVALIDARG && SUCCEEDED(removed); ++level) {
      nxbox_simplify_pso(pso_desc, original, level, ps);
      tried |= 1u << (level - 1);
      hr = results[level - 1] = create();
      if (SUCCEEDED(hr)) {
         cache_entry->fallback_level = level;
         cache_entry->num_render_targets = render_targets.NumRenderTargets;
         cache_entry->has_dsv = (DXGI_FORMAT)pso_desc.DSVFormat != DXGI_FORMAT_UNKNOWN;
         nxbox_report_pso(true, hr);
         nxbox_report_pso_fallback(id, level, tried, results, removed);
         return ret;
      }
      removed = screen->dev->GetDeviceRemovedReason();
   }
   nxbox_report_pso_fallback(id, 0, tried, results, removed);
   return NULL;"""
    entry_old = (
        "struct d3d12_gfx_pso_entry {\n"
        "   struct d3d12_gfx_pipeline_state key;\n"
        "   ID3D12PipelineState *pso;\n"
    )
    call_old = "      data->pso = create_gfx_pipeline_state(ctx);"
    replacements = {
        anchor: '#include "nxbox_pso_input.h"\n#include "nxbox_pso_guard.h"\n'
        + helper
        + anchor.replace(" *ctx)", " *ctx, struct d3d12_gfx_pso_entry *cache_entry)"),
        create_old: create_new,
        entry_old: entry_old
        + "   unsigned fallback_level;\n   unsigned num_render_targets;\n   bool has_dsv;\n",
        call_old: "      data->pso = create_gfx_pipeline_state(ctx, data);",
    }
    bind_anchor = "void\nd3d12_gfx_pipeline_state_cache_init(struct d3d12_context *ctx)\n"
    bind_helper = r"""/* Keep attachment bindings consistent with a cached fallback PSO. */
void
d3d12_nxbox_set_render_targets(struct d3d12_context *ctx, unsigned count,
                               const D3D12_CPU_DESCRIPTOR_HANDLE *targets,
                               const D3D12_CPU_DESCRIPTOR_HANDLE *depth)
{
   uint32_t hash = hash_gfx_pipeline_state(&ctx->gfx_pipeline_state);
   struct hash_entry *entry =
       _mesa_hash_table_search_pre_hashed(ctx->pso_cache, hash, &ctx->gfx_pipeline_state);
   if (entry) {
      auto data = (struct d3d12_gfx_pso_entry *)entry->data;
      if (data->fallback_level) {
         count = MIN2(count, data->num_render_targets);
         if (!data->has_dsv)
            depth = NULL;
      }
   }
   ctx->cmdlist->OMSetRenderTargets(count, targets, false, depth);
}
"""
    replacements[bind_anchor] = bind_helper + bind_anchor
    draw = root / "src/gallium/drivers/d3d12/d3d12_draw.cpp"
    draw_source = draw.read_text()
    declaration_anchor = '#include "d3d12_context.h"\n'
    bind_old = "      ctx->cmdlist->OMSetRenderTargets(ctx->fb.nr_cbufs, render_targets, false, depth_desc);"
    dirty_old = "   if (ctx->cmdlist_dirty & D3D12_DIRTY_FRAMEBUFFER) {"
    draw_replacements = {
        declaration_anchor: declaration_anchor
        + r"""
/* Defined by the NXbox PSO patch; the upstream cache entry remains private. */
void d3d12_nxbox_set_render_targets(struct d3d12_context *ctx, unsigned count,
                                  const D3D12_CPU_DESCRIPTOR_HANDLE *targets,
                                  const D3D12_CPU_DESCRIPTOR_HANDLE *depth);
""",
        bind_old: "      d3d12_nxbox_set_render_targets(ctx, ctx->fb.nr_cbufs, render_targets, depth_desc);",
        dirty_old: "   if (ctx->cmdlist_dirty & (D3D12_DIRTY_FRAMEBUFFER | D3D12_DIRTY_GFX_PSO)) {",
    }
    for old in draw_replacements:
        if draw_source.count(old) != 1:
            raise RuntimeError(f"Pinned Mesa PSO framebuffer anchor mismatch: {old[:80]}")
    for old in replacements:
        if source.count(old) != 1:
            raise RuntimeError(f"Pinned Mesa PSO patch anchor mismatch: {old[:80]}")
    for old, new in replacements.items():
        source = source.replace(old, new, 1)
    for old, new in draw_replacements.items():
        draw_source = draw_source.replace(old, new, 1)
    guard_helper = Path(__file__).with_name("mesa_pso_guard.h").read_text()
    guard_target = pso.with_name("nxbox_pso_guard.h")
    if guard_target.exists():
        raise RuntimeError("Pinned Mesa PSO guard helper already exists")
    input_helper = Path(__file__).with_name("mesa_pso_input.h").read_text()
    input_target = pso.with_name("nxbox_pso_input.h")
    if input_target.exists():
        raise RuntimeError("Pinned Mesa PSO input helper already exists")
    draw.write_text(draw_source)
    pso.write_text(source)
    input_target.write_text(input_helper)
    guard_target.write_text(guard_helper)


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


def patch_dred(root: Path) -> None:
    """Instrument the pinned driver after the batch/root-signature/optional PSO patches.

    Validate every anchor before writing any file. The helper lives in screen.cpp so
    every worker shares one capture latch per device; no Meson source list change.
    """
    driver = root / "src/gallium/drivers/d3d12"
    sources = {
        name: (driver / name).read_text()
        for name in (
            "d3d12_screen.cpp",
            "d3d12_batch.cpp",
            "d3d12_pipeline_state.cpp",
            "d3d12_root_signature.cpp",
        )
    }

    def replace(name: str, old: str, new: str, count: int = 1) -> None:
        if sources[name].count(old) != count:
            raise RuntimeError(f"Pinned Mesa DRED anchor mismatch in {name}: {old[:80]}")
        sources[name] = sources[name].replace(old, new)

    screen = "d3d12_screen.cpp"
    anchor = "#include <dxguids/dxguids.h>\n"
    replace(screen, anchor, anchor + '#define NXBOX_DRED_IMPLEMENTATION\n#include "nxbox_dred.h"\n')
    anchor = "      screen->dev = create_device(screen->d3d12_mod, adapter, factory);"
    replace(screen, anchor, "      nxbox_dred_enable(screen->d3d12_mod, factory);\n" + anchor)
    anchor = "   assert(screen->base.destroy != nullptr);"
    replace(
        screen,
        anchor,
        anchor + "\n   if (screen->dev) {\n"
        '      SetEnvironmentVariableA("NXBOX_D3D12_DRED", "unavailable stage=ImportedDevice settings-too-late");\n'
        '      SetEnvironmentVariableA("NXBOX_D3D12_DRED2", "pending device removal");\n   }',
    )
    anchor = "      screen->dev->Release();\n      screen->dev = nullptr;"
    replace(screen, anchor, "      nxbox_dred_forget(screen->dev);\n" + anchor)
    for name in sources:
        if name != screen:
            anchor = '#include "d3d12_screen.h"\n'
            replace(name, anchor, anchor + '#include "nxbox_dred.h"\n')

    batch = "d3d12_batch.cpp"
    replace(
        batch,
        "#include <directx/d3d12sdklayers.h>\n",
        "#include <directx/d3d12sdklayers.h>\n#include <mutex>\n#include <wchar.h>\n",
    )
    replace(
        batch,
        "nxbox_report_batch(struct d3d12_screen *screen, HRESULT close_hr)\n{",
        "nxbox_report_batch(struct d3d12_context *ctx, struct d3d12_batch *batch,\n"
        "                   HRESULT close_hr, const char *stage, unsigned long long fence)\n{\n"
        "   auto screen = d3d12_screen(ctx->base.screen);\n"
        "   static std::mutex report_mutex;\n"
        "   std::lock_guard<std::mutex> lock(report_mutex);",
    )
    replace(batch, "   batches++;\n", '   if (strcmp(stage, "close") == 0)\n      batches++;\n')
    replace(
        batch,
        "   const bool report = FAILED(close_hr) ?",
        "   HRESULT removed = screen->dev->GetDeviceRemovedReason();\n"
        "   const bool report = FAILED(close_hr) ?",
    )
    replace(
        batch,
        "   if (!report)\n      return;",
        "   if (!report && SUCCEEDED(removed))\n      return;",
    )
    old = r"""   char text[160];
   snprintf(text, sizeof(text), "batches=%ld close_failed=%ld close_hr=0x%08lx removed=0x%08lx",
            batches, close_failed, (unsigned long)last_hr,
            (unsigned long)screen->dev->GetDeviceRemovedReason());
   SetEnvironmentVariableA("NXBOX_D3D12_BATCH", text);"""
    new = r"""   char text[256];
   snprintf(text, sizeof(text), "batches=%ld close_failed=%ld close_hr=0x%08lx removed=0x%08lx "
            "stage=%s ctx=%p batch=%u submit=%llu fence_target=%llu fence_ok=%u",
            batches, close_failed, (unsigned long)last_hr, (unsigned long)removed, stage,
            (void *)ctx, (unsigned)(batch - ctx->batches),
            (unsigned long long)batch->submit_id, fence, batch->fence ? 1u : 0u);
   if (FAILED(removed))
      nxbox_dred_capture(screen->dev, removed, stage, text);
   else
      nxbox_dred_publish_batch(screen->dev, text);"""
    replace(batch, old, new)
    replace(
        batch,
        "   nxbox_report_batch(screen, nxbox_close_hr);",
        '   nxbox_report_batch(ctx, batch, nxbox_close_hr, "close", 0);',
    )
    anchor = "   screen->cmdqueue->ExecuteCommandLists(count_to_execute, to_execute);"
    replace(
        batch,
        anchor,
        "   /* submit_mutex serializes this target with d3d12_create_fence. */\n"
        "   const auto nxbox_fence_target = screen->fence_value + 1;\n"
        "   nxbox_dred_submit(screen->dev, ctx, (unsigned)(batch - ctx->batches),\n"
        "                     batch->submit_id, nxbox_fence_target);\n" + anchor,
    )
    anchor = "   batch->fence = d3d12_create_fence(screen);"
    replace(
        batch,
        anchor,
        anchor + '\n   nxbox_report_batch(ctx, batch, S_OK, "execute", nxbox_fence_target);',
    )
    anchor = "   batch->submit_id = ++ctx->submit_id;"
    replace(
        batch,
        anchor,
        anchor
        + r"""
   wchar_t nxbox_name[80];
   swprintf(nxbox_name, ARRAY_SIZE(nxbox_name), L"%p.b%u.s%llu", (void *)ctx, (unsigned)(batch - ctx->batches),
            (unsigned long long)batch->submit_id);
   ctx->cmdlist->SetName(nxbox_name);
""",
    )
    # Name the state-fixup list separately; it contains submission-time barriers.
    anchor = "   bool has_state_fixup = d3d12_context_state_resolve_submission(ctx, batch);"
    replace(
        batch,
        anchor,
        anchor
        + r"""
   if (has_state_fixup) {
      wchar_t nxbox_name[80];
      swprintf(nxbox_name, ARRAY_SIZE(nxbox_name), L"fix.%p.b%u.s%llu", (void *)ctx, (unsigned)(batch - ctx->batches),
               (unsigned long long)batch->submit_id);
      ctx->state_fixup_cmdlist->SetName(nxbox_name);
   }
""",
    )

    pso = "d3d12_pipeline_state.cpp"
    if "   HRESULT removed = screen->dev->GetDeviceRemovedReason();" in sources[pso]:
        anchor = "   HRESULT removed = screen->dev->GetDeviceRemovedReason();"
        replace(pso, anchor, anchor + '\n   nxbox_dred_capture(screen->dev, removed, "gfx-pso");')
        anchor = "      removed = screen->dev->GetDeviceRemovedReason();"
        replace(
            pso,
            anchor,
            anchor + '\n      nxbox_dred_capture(screen->dev, removed, "gfx-pso-retry");',
        )
    else:
        # NXBOX_MESA_SKIP=pso still needs DRED on the two upstream graphics paths.
        anchor = '         debug_printf("D3D12: CreateGraphicsPipelineState failed!\\n");'
        replace(
            pso,
            anchor,
            '         nxbox_dred_capture(screen->dev, screen->dev->GetDeviceRemovedReason(), "gfx-pso");\n'
            + anchor,
            2,
        )
    anchor = '      debug_printf("D3D12: CreateComputePipelineState failed!\\n");'
    replace(
        pso,
        anchor,
        '      nxbox_dred_capture(screen->dev, screen->dev->GetDeviceRemovedReason(), "compute-pso");\n'
        + anchor,
    )
    rootsig = "d3d12_root_signature.cpp"
    anchor = '      nxbox_report_root_signature("create", nxbox_create_hr, NULL, num_params);'
    replace(
        rootsig,
        anchor,
        anchor
        + '\n      nxbox_dred_capture(screen->dev, screen->dev->GetDeviceRemovedReason(), "rootsig");',
    )
    anchor = '         nxbox_report_root_signature("serialize", nxbox_hr, error.Get(), num_params);'
    replace(
        rootsig,
        anchor,
        anchor + "\n         auto nxbox_dev = d3d12_screen(ctx->base.screen)->dev;\n"
        '         nxbox_dred_capture(nxbox_dev, nxbox_dev->GetDeviceRemovedReason(), "rootsig-serialize");',
    )
    helper = Path(__file__).with_name("mesa_dred.h").read_text()
    if (driver / "nxbox_dred.h").exists():
        raise RuntimeError("Pinned Mesa DRED anchor mismatch: helper already exists")
    for name, source in sources.items():
        (driver / name).write_text(source)
    (driver / "nxbox_dred.h").write_text(helper)


def patch_lifetime(root: Path) -> None:
    """Observe removal at API boundaries and bound context-owned PSO caches.

    Applied after the older patches. Validate the complete transaction before writing.
    """
    driver = root / "src/gallium/drivers/d3d12"
    names = ("pipeline_state", "context", "batch", "draw", "compiler", "descriptor_pool", "fence")
    sources = {f"d3d12_{name}.cpp": (driver / f"d3d12_{name}.cpp").read_text() for name in names}
    sources["d3d12_context.h"] = (driver / "d3d12_context.h").read_text()

    def replace(name: str, old: str, new: str, count: int = 1) -> None:
        if sources[name].count(old) != count:
            raise RuntimeError(f"Pinned Mesa lifetime anchor mismatch in {name}: {old[:100]}")
        sources[name] = sources[name].replace(old, new)

    for name in names:
        file = f"d3d12_{name}.cpp"
        if '#include "nxbox_dred.h"' not in sources[file]:
            anchor = '#include "d3d12_screen.h"\n'
            replace(file, anchor, anchor + '#include "nxbox_dred.h"\n')

    file = "d3d12_pipeline_state.cpp"
    # Wrap both graphics APIs without altering fallback semantics, including the skip=pso path.
    import re
    for api in ("CreatePipelineState", "CreateGraphicsPipelineState", "CreateComputePipelineState"):
        pattern = rf"screen->dev->{api}\(([^,]+),\s*IID_PPV_ARGS\(&ret\)\)"
        kind = "compute-pso" if api == "CreateComputePipelineState" else "gfx-pso"
        def wrap(match: re.Match) -> str:
            report = ", [&](HRESULT before, HRESULT result, HRESULT after) {\n"
            if kind == "gfx-pso" and "nxbox_original_inputs" in sources[file]:
                # Publish capture before sampling publishes DEVICE_LOST to the frontend.
                report += (
                    "         if (FAILED(before) || FAILED(result) || FAILED(after))\n"
                    "            nxbox_pso_first(pso_desc, FAILED(before) ? before : "
                    "(FAILED(after) ? after : result), nxbox_original_inputs);\n"
                )
            else:
                report += "         (void)before;\n"
            report += (
                "         if (SUCCEEDED(result) && FAILED(after) && ret) {\n"
                "            nxbox_object_ref(ret, -1);\n"
                "            ret->Release();\n"
                "            ret = NULL;\n"
                "         }\n      }"
            )
            return (
                f'nxbox_create_pso(screen->dev, "{kind}", [&]() -> HRESULT {{\n'
                f"         HRESULT result = {match[0]};\n"
                "         if (SUCCEEDED(result)) nxbox_pso_created(ret);\n"
                f"         return result;\n      }}{report})"
            )

        sources[file], count = re.subn(pattern, wrap, sources[file])
        if count != 1:
            raise RuntimeError(f"Pinned Mesa lifetime API mismatch: {api}")
    if "nxbox_report_pso_mutex" in sources[file]:
        raise RuntimeError("Unexpected PSO report mutex spelling")
    if "nxbox_pso_report_mutex" in sources[file]:
        anchor = 'static std::mutex nxbox_pso_report_mutex;'
        replace(file, anchor, anchor + '\n#include "nxbox_pso_first.h"')
        anchor = '      SetEnvironmentVariableA("NXBOX_D3D12_PSO_FIX", "quarantined graphics signature; draw skipped; canary=unverified-isolation");'
        replace(
            file,
            anchor,
            anchor + "\n      nxbox_pso_first(pso_desc, S_FALSE, nxbox_original_inputs);",
        )
        anchor = "   const auto original_v0 = original.GraphicsDescV0();"
        replace(file, anchor, anchor + "\n   nxbox_pso_first(original, hr, nxbox_original_inputs);")
        replace(file, "(created & (created - 1)) == 0", "(created % 8) == 0")

    # Entry metadata is owned by its context. Batch AddRef keeps evicted PSOs alive until reset.
    for kind, cache in (("gfx", "pso_cache"), ("compute", "compute_pso_cache")):
        entry = f"d3d12_{kind}_pso_entry"
        anchor = f"struct {entry} {{\n"
        replace(file, anchor, anchor + "   struct d3d12_context *owner;\n   uint64_t last_frame, last_use, dxil_bytes;\n")
        anchor = f"      data->key = ctx->{kind}_pipeline_state;"
        bytecode = ("for (auto shader : data->key.stages)\n"
                    "         if (shader) data->dxil_bytes += shader->bytecode_length;"
                    if kind == "gfx" else
                    "if (data->key.stage) data->dxil_bytes = data->key.stage->bytecode_length;")
        replace(file, anchor, anchor + "\n      data->owner = ctx;\n      data->dxil_bytes = 0;\n      " + bytecode)
        anchor = f"      entry = _mesa_hash_table_insert_pre_hashed(ctx->{cache}, hash, &data->key, data);"
        replace(file, anchor, f"      ctx->nxbox_{kind}_bytes += data->dxil_bytes;\n" + anchor)
        anchor = f"   return ((struct {entry} *)(entry->data))->pso;"
        replace(file, anchor, f"   auto data = (struct {entry} *)entry->data;\n"
                "   data->last_frame = ctx->nxbox_frame;\n"
                "   data->last_use = ++ctx->nxbox_use;\n   return data->pso;")
        anchor = f"   struct {entry} *data = (struct {entry} *)entry->data;\n   data->pso->Release();"
        replace(file, anchor, f"   struct {entry} *data = (struct {entry} *)entry->data;\n"
                f"   data->owner->nxbox_{kind}_bytes -= data->dxil_bytes;\n"
                "   nxbox_object_ref(data->pso, -1);\n   data->pso->Release();")
        # The trim helper follows remove_*_entry; a forward declaration covers the getter.
        anchor = f"ID3D12PipelineState *\nd3d12_get_{kind}_pipeline_state(struct d3d12_context *ctx)\n{{"
        replace(file, anchor, f"static void nxbox_trim_{kind}(struct d3d12_context *ctx);\n\n" + anchor +
                "\n   if (nxbox_device_lost.load(std::memory_order_relaxed)) return NULL;")
        anchor = f"      struct {entry} *data = (struct {entry} *)MALLOC(sizeof(struct {entry}));"
        replace(file, anchor, f"      nxbox_trim_{kind}(ctx);\n" + anchor)
        anchor = f"void\nd3d12_{kind}_pipeline_state_cache_destroy(struct d3d12_context *ctx)"
        helper = f'''static void
nxbox_trim_{kind}(struct d3d12_context *ctx)
{{
   /* Soft target: cold entries first. Hard ceiling: evict the LRU even during prewarming.
    * Both limits count cache references, not opaque driver allocations or unique DXIL.
    */
   while (ctx->{cache}->entries >= 2048 || ctx->nxbox_{kind}_bytes >= (1ull << 30)) {{
      hash_entry *victim = nullptr;
      uint64_t oldest = UINT64_MAX;
      const bool hard = ctx->{cache}->entries >= 4096 || ctx->nxbox_{kind}_bytes >= (2ull << 30);
      hash_table_foreach(ctx->{cache}, candidate) {{
         auto data = (struct {entry} *)candidate->data;
         if (data->pso == ctx->current_{kind}_pso)
            continue;
         if ((hard || ctx->nxbox_frame - data->last_frame >= 120) && data->last_use < oldest) {{
            oldest = data->last_use;
            victim = candidate;
         }}
      }}
      if (!victim)
         break;
      remove_{kind}_entry(ctx, victim);
      ++ctx->nxbox_evicted;
   }}
   char text[256];
   snprintf(text, sizeof(text), "ctx=%p kind={kind} cached=%u attributed_dxil=%llu frame=%llu evicted=%llu soft=2048 hard=4096 age=120",
            (void *)ctx, ctx->{cache}->entries, (unsigned long long)ctx->nxbox_{kind}_bytes,
            (unsigned long long)ctx->nxbox_frame, (unsigned long long)ctx->nxbox_evicted);
   SetEnvironmentVariableA("NXBOX_D3D12_CACHE", text);
}}

'''
        replace(file, anchor, helper + anchor)

    replace("d3d12_context.h", "   struct hash_table *pso_cache;",
            "   uint64_t nxbox_frame, nxbox_use, nxbox_gfx_bytes, nxbox_compute_bytes, nxbox_evicted;\n"
            "   struct hash_table *pso_cache;")
    file = "d3d12_context.cpp"
    anchor = "   struct d3d12_context *ctx = d3d12_context(pipe);\n   struct d3d12_batch *batch = d3d12_current_batch(ctx);"
    replace(file, anchor, anchor + "\n   if (flags & PIPE_FLUSH_END_OF_FRAME) ++ctx->nxbox_frame;")
    # Entry-point guards run before any command-list use (not merely before SetPipelineState).
    for file, functions in {
        "d3d12_context.cpp": ("d3d12_clear_render_target", "d3d12_clear_depth_stencil", "d3d12_clear"),
        "d3d12_draw.cpp": ("d3d12_draw_vbo", "d3d12_launch_grid"),
    }.items():
        for function in functions:
            pattern = rf"(\n{function}\([^{{]+\n\{{)"
            sources[file], count = re.subn(pattern, r"\1\n   if (nxbox_device_lost.load(std::memory_order_acquire)) return;", sources[file])
            if count != 1:
                raise RuntimeError(f"Pinned Mesa lifetime entry point mismatch: {function}")

    file = "d3d12_batch.cpp"
    replace(file, "   object->Release();", "   nxbox_object_ref(object, -1);\n   object->Release();")
    replace(file, "      object->AddRef();", "      object->AddRef();\n      nxbox_object_ref(object, 1);")
    anchor = "   screen->cmdqueue->ExecuteCommandLists(count_to_execute, to_execute);"
    replace(file, anchor, anchor + '\n   nxbox_observe(screen->dev, "execute-return");')
    for function in ("d3d12_start_batch", "d3d12_end_batch"):
        anchor = f"{function}(struct d3d12_context *ctx, struct d3d12_batch *batch)\n{{"
        replace(file, anchor, anchor + "\n   if (nxbox_device_lost.load(std::memory_order_acquire)) {\n"
                "      batch->has_errors = true;\n      return;\n   }")
    replace(file, "   if (FAILED(batch->cmdalloc->Reset())) {", "   if (FAILED(batch->cmdalloc->Reset())) {") if False else None
    # Cleanup on removal still releases the batch references, but never resets the dead allocator.
    anchor = "   HRESULT nxbox_alloc_hr = batch->cmdalloc->Reset();"
    replace(file, anchor, "   if (nxbox_device_lost.load(std::memory_order_acquire)) return true;\n" + anchor)
    replace(file, "   if (!batch->sampler_heap && !batch->view_heap)", "   if (!batch->sampler_heap || !batch->view_heap)")

    file = "d3d12_compiler.cpp"
    anchor = "   blob_finish_get_buffer(&tmp, &shader->bytecode, &shader->bytecode_length);"
    replace(file, anchor, anchor + "\n   nxbox_metrics[NxboxDxil] += shader->bytecode_length;\n   ++nxbox_metrics[NxboxVariants];")
    anchor = "      free(shader->bytecode);"
    replace(file, anchor, "      nxbox_metrics[NxboxDxil] -= shader->bytecode_length;\n"
            "      --nxbox_metrics[NxboxVariants];\n" + anchor)

    file = "d3d12_descriptor_pool.cpp"
    replace(file, "   uint32_t next;", "   uint32_t next;\n   uint32_t nxbox_live;")
    anchor = "   heap->dev = dev;"
    replace(file, anchor, "   ++nxbox_metrics[NxboxHeaps];\n   nxbox_metrics[NxboxCapacity] += num_descriptors;\n" + anchor)
    anchor = "   heap->heap->Release();"
    replace(file, anchor, "   if (!heap) return;\n   --nxbox_metrics[NxboxHeaps];\n"
            "   nxbox_metrics[NxboxCapacity] -= heap->desc.NumDescriptors;\n"
            "   nxbox_metrics[NxboxHandles] -= heap->nxbox_live;\n" + anchor)
    anchor = "   handle->cpu_handle.ptr = heap->cpu_base + offset;"
    replace(file, anchor, "   ++heap->nxbox_live;\n   ++nxbox_metrics[NxboxHandles];\n" + anchor)
    anchor = "   const uint32_t index = handle->cpu_handle.ptr - handle->heap->cpu_base;"
    replace(file, anchor, "   --handle->heap->nxbox_live;\n   --nxbox_metrics[NxboxHandles];\n" + anchor)
    anchor = "   heap->next += num_handles * heap->desc_size;"
    replace(file, anchor, anchor + "\n   heap->nxbox_live += num_handles;\n   nxbox_metrics[NxboxHandles] += num_handles;")
    anchor = "   heap->next = 0;"
    replace(file, anchor, "   nxbox_metrics[NxboxHandles] -= heap->nxbox_live;\n   heap->nxbox_live = 0;\n" + anchor)
    anchor = "      list_addtail(&valid_heap->link, &pool->heaps);"
    replace(file, anchor, "      if (!valid_heap) return 0;\n" + anchor)
    anchor = '      assert(0 && "No handles available in descriptor heap");'
    replace(file, anchor, '      SetEnvironmentVariableA("NXBOX_D3D12_DESCRIPTOR_FAIL", "handle exhaustion");\n' + anchor)
    anchor = "      FREE(heap);\n      return NULL;"
    replace(file, anchor, '      nxbox_observe(dev, "descriptor-heap-create");\n'
            '      SetEnvironmentVariableA("NXBOX_D3D12_DESCRIPTOR_FAIL", "heap creation failed");\n' + anchor)

    file = "d3d12_fence.cpp"
    anchor = "   if (FAILED(screen->cmdqueue->Signal(screen->fence, ret->value)))"
    replace(file, anchor, "   HRESULT nxbox_signal = screen->cmdqueue->Signal(screen->fence, ret->value);\n"
            '   nxbox_observe(screen->dev, "signal-return", nxbox_signal);\n   if (FAILED(nxbox_signal))')
    # GetDevice gives the exact device even for externally imported fences. Bound infinite waits
    # to 100 ms slices so a lost device cannot leave the frontend stuck in the wait forever.
    old = "   bool complete = fence->cmdqueue_fence->GetCompletedValue() >= fence->value;\n   if (!complete && timeout_ns)\n      complete = d3d12_fence_wait_event(fence->event, fence->event_fd, timeout_ns);"
    new = '''   ID3D12Device *dev = nullptr;
   fence->cmdqueue_fence->GetDevice(IID_PPV_ARGS(&dev));
   if (dev) nxbox_observe(dev, "fence-before-wait");
   bool complete = fence->cmdqueue_fence->GetCompletedValue() >= fence->value;
   uint64_t remaining = timeout_ns;
   while (!complete && remaining && !nxbox_device_lost.load(std::memory_order_acquire)) {
      const uint64_t slice = MIN2(remaining, 100000000ull);
      complete = d3d12_fence_wait_event(fence->event, fence->event_fd, slice);
      if (dev) nxbox_observe(dev, "fence-after-wait");
      complete |= fence->cmdqueue_fence->GetCompletedValue() >= fence->value;
      if (remaining != OS_TIMEOUT_INFINITE) remaining -= slice;
   }
   if (dev) {
      nxbox_observe(dev, "fence-return");
      dev->Release();
   }
   /* On removal UINT64_MAX is not GPU success; it permits releasing dead-device objects. */
   complete |= nxbox_device_lost.load(std::memory_order_acquire);'''
    replace(file, old, new)
    helpers = {"nxbox_lifetime.h": "mesa_lifetime.h", "nxbox_pso_first.h": "mesa_pso_first.h"}
    for target in helpers:
        if (driver / target).exists():
            raise RuntimeError(f"Pinned Mesa lifetime helper already exists: {target}")
    for name, source in sources.items():
        (driver / name).write_text(source)
    for target, source in helpers.items():
        (driver / target).write_text(Path(__file__).with_name(source).read_text())


def patch_sync_batch(root: Path) -> None:
    """Serialize GPU completion by default and retain the last 64 command records.

    Apply after lifetime instrumentation. Validate the entire patch before writing;
    command counts deliberately reject drift in the pinned graphics driver.
    """
    import re

    driver = root / "src/gallium/drivers/d3d12"
    calls = {
        "d3d12_context.cpp": {
            "ResourceBarrier": 2,
            "ClearRenderTargetView": 1,
            "ClearDepthStencilView": 1,
        },
        "d3d12_draw.cpp": {
            "SetPipelineState": 2,
            "SetGraphicsRootSignature": 1,
            "SetComputeRootSignature": 1,
            "DrawInstanced": 1,
            "DrawIndexedInstanced": 1,
            "Dispatch": 1,
            "ExecuteIndirect": 2,
        },
        "d3d12_blit.cpp": {
            "CopyBufferRegion": 1,
            "CopyTextureRegion": 3,
            "ResolveSubresource": 1,
        },
        "d3d12_resource.cpp": {"CopyBufferRegion": 1, "CopyTextureRegion": 1},
        "d3d12_resource_state.cpp": {"ResourceBarrier": 1},
    }
    names = (*calls, "d3d12_batch.cpp", "d3d12_context.h")
    sources = {name: (driver / name).read_text() for name in names}

    def replace(name: str, old: str, new: str, count: int = 1) -> None:
        if sources[name].count(old) != count:
            raise RuntimeError(f"Pinned Mesa sync batch anchor mismatch in {name}: {old[:100]}")
        sources[name] = sources[name].replace(old, new)

    target = driver / "nxbox_sync_batch.h"
    if target.exists():
        raise RuntimeError("Pinned Mesa sync batch helper already exists")
    helper = Path(__file__).with_name("mesa_sync_batch.h").read_text()
    helper = helper.replace(
        "  static std::atomic<bool> stopped{false};\n  return stopped;",
        "  return nxbox_command_stopped();",
    )
    for name in (*calls, "d3d12_batch.cpp"):
        anchor = '#include "d3d12_context.h"\n'
        replace(name, anchor, anchor + '#include "nxbox_sync_batch.h"\n')
    replace(
        "d3d12_context.h",
        "   struct hash_table *pso_cache;",
        "   struct NxboxBatchJournal *nxbox_journal;\n   struct hash_table *pso_cache;",
    )
    # This destruction anchor occurs only after all batches have been ended/released.
    anchor = "   util_dynarray_fini(&ctx->recently_destroyed_bos);"
    replace("d3d12_context.cpp", anchor, anchor + "\n   delete ctx->nxbox_journal;")
    anchor = "   ctx->cmdlist->SetDescriptorHeaps(2, heaps);"
    replace("d3d12_batch.cpp", anchor, "   nxbox_journal_reset(ctx->nxbox_journal);\n" + anchor)

    for name, methods in calls.items():
        for method, count in methods.items():
            old = f"ctx->cmdlist->{method}("
            new = f"nxbox_journal_commands(ctx->nxbox_journal, ctx->cmdlist).{method}("
            replace(name, old, new, count)
    # Fixup barriers are recorded at submission time but execute BEFORE the main list.
    replace(
        "d3d12_resource_state.cpp",
        "         cmdlist->ResourceBarrier(",
        '         nxbox_journal_commands(ctx->nxbox_journal, cmdlist, "fixup").ResourceBarrier(',
    )
    # No CopyResource calls exist in this pin, but the wrapper supports it. Reject any
    # uninstrumented graphics call (including new files/aliases) instead of silently missing it.
    methods = sorted(
        {method for entries in calls.values() for method in entries} | {"CopyResource", "ExecuteBundle"}
    )
    pattern = re.compile(r"->\s*(?:" + "|".join(methods) + r")\s*\(")
    copy_pattern = re.compile(r"->\s*(?:CopyTextureRegion|CopyBufferRegion|CopyResource)\s*\(")
    for path in driver.iterdir():
        if path.suffix not in (".cpp", ".c", ".h"):
            continue
        source = sources.get(path.name, path.read_text())
        if copy_pattern.search(source):
            raise RuntimeError(f"Pinned Mesa unnormalized copy in {path.name}")
        if not path.name.startswith("d3d12_") or path.suffix != ".cpp":
            continue
        if path.name.startswith("d3d12_video"):
            continue  # Separate video queues are not Gallium graphics batches.
        if pattern.search(source):
            raise RuntimeError(f"Pinned Mesa sync batch unjournaled command in {path.name}")

    file = "d3d12_batch.cpp"
    # A non-null fence is not evidence of GPU completion. Report the selected
    # mode and journal revision so the next console capture identifies the DLL.
    replace(
        file,
        "fence_target=%llu fence_ok=%u",
        "fence_target=%llu fence_present=%u sync=%u journal=4",
    )
    replace(
        file,
        "fence, batch->fence ? 1u : 0u);",
        "fence, batch->fence ? 1u : 0u, nxbox_sync_batch_enabled() ? 1u : 0u);",
    )
    anchor = "   mtx_lock(&screen->submit_mutex);"
    replace(
        file,
        anchor,
        anchor
        + r"""
   if (nxbox_sync_batch_enabled()) {
      const HRESULT removed = screen->dev->GetDeviceRemovedReason();
      if (FAILED(removed) || nxbox_sync_stopped().load()) {
         /* Loss predating this Execute is not evidence against this batch. */
         if (FAILED(removed)) {
            SetEnvironmentVariableA("NXBOX_D3D12_SYNC_ERROR", "device lost before Execute; batch not submitted");
            nxbox_dred_capture(screen->dev, removed, "sync-before-execute");
         }
         batch->has_errors = true;
         mtx_unlock(&screen->submit_mutex);
         return;
      }
   }
""",
    )
    anchor = "   screen->cmdqueue->ExecuteCommandLists(count_to_execute, to_execute);"
    replace(
        file,
        anchor,
        anchor
        + r"""
   if (nxbox_sync_batch_enabled()) {
      /* No observer or fence allocation between Execute and its dedicated drain.
       * submit_mutex prevents another context from replacing the submission snapshot. */
      const UINT64 target = ++screen->fence_value;
      const HRESULT signal = screen->cmdqueue->Signal(screen->fence, target);
      HRESULT removed = SUCCEEDED(signal) ? nxbox_sync_wait(screen->dev, screen->fence, target)
                                         : screen->dev->GetDeviceRemovedReason();
      const HRESULT after_wait = screen->dev->GetDeviceRemovedReason();
      if (FAILED(after_wait))
         removed = after_wait;
      if (FAILED(removed)) {
         nxbox_sync_capture(screen->dev, removed);
         nxbox_dred_capture(screen->dev, removed, "sync-submit-fence");
         batch->has_errors = true;
      } else if (FAILED(signal)) {
         /* Attribution is no longer possible. Do not submit another batch unsynchronized. */
         char text[128];
         snprintf(text, sizeof(text), "fence Signal failed hr=0x%08x; sync stopped", (unsigned)signal);
         SetEnvironmentVariableA("NXBOX_D3D12_SYNC_ERROR", text);
         nxbox_sync_stopped().store(true);
         batch->has_errors = true;
      } else {
         nxbox_sync_completed(screen->dev);
      }
   }
""",
    )
    anchor = "   screen->cmdqueue->ExecuteCommandLists(count_to_execute, to_execute);"
    replace(
        file,
        anchor,
        "   nxbox_sync_submit(screen->dev, ctx->nxbox_journal, ctx, (unsigned)(batch - ctx->batches),\n"
        "                     batch->submit_id, nxbox_fence_target, has_state_fixup);\n" + anchor,
    )
    anchor = "   struct pipe_stream_output_target **so_targets = ctx->fake_so_buffer_factor ? ctx->fake_so_targets"
    replace(
        "d3d12_draw.cpp",
        anchor,
        r"""   if (ctx->nxbox_journal) {
      unsigned rtv[8] = {};
      for (unsigned i = 0; i < ctx->gfx_pipeline_state.num_cbufs; ++i)
         rtv[i] = (unsigned)d3d12_rtv_format(ctx, i);
      snprintf(ctx->nxbox_journal->targets, sizeof(ctx->nxbox_journal->targets),
               "pso_rtv=%u,%u,%u,%u,%u,%u,%u,%u pso_dsv=%u samples=%u",
               rtv[0], rtv[1], rtv[2], rtv[3], rtv[4], rtv[5], rtv[6], rtv[7],
               (unsigned)ctx->gfx_pipeline_state.dsv_format, ctx->gfx_pipeline_state.samples);
      for (unsigned i = 0; i < 8; ++i) {
         rtv[i] = i < ctx->fb.nr_cbufs && ctx->fb.cbufs[i] ?
            (unsigned)(conversion_modes[i] == D3D12_SURFACE_CONVERSION_NONE ?
               d3d12_get_resource_rt_format(ctx->fb.cbufs[i]->format) : DXGI_FORMAT_R8G8B8A8_UINT) : 0;
      }
      const size_t used = strlen(ctx->nxbox_journal->targets);
      snprintf(ctx->nxbox_journal->targets + used, sizeof(ctx->nxbox_journal->targets) - used,
               " bound_rtv=%u,%u,%u,%u,%u,%u,%u,%u bound_dsv=%u",
               rtv[0], rtv[1], rtv[2], rtv[3], rtv[4], rtv[5], rtv[6], rtv[7],
               ctx->fb.zsbuf ? (unsigned)d3d12_get_resource_rt_format(ctx->fb.zsbuf->format) : 0);
   }

"""
        + anchor,
    )
    for name, source in sources.items():
        (driver / name).write_text(source)
    target.write_text(helper)


def patch_batch_reuse(root: Path) -> None:
    """Require actual GPU completion before recycling any batch-owned storage."""
    driver = root / "src/gallium/drivers/d3d12"
    names = (
        "d3d12_batch.cpp",
        "d3d12_context.h",
        "d3d12_fence.cpp",
        "d3d12_context.cpp",
        "d3d12_draw.cpp",
    )
    sources = {name: (driver / name).read_text() for name in names}

    def replace(name: str, old: str, new: str, count: int = 1) -> None:
        if sources[name].count(old) != count:
            raise RuntimeError(f"Pinned Mesa batch reuse anchor mismatch in {name}: {old[:100]}")
        sources[name] = sources[name].replace(old, new)

    batch = "d3d12_batch.cpp"
    anchor = '#include "nxbox_sync_batch.h"\n'
    replace(batch, anchor, anchor + '#include "nxbox_batch_reuse.h"\n')
    anchor = "   struct d3d12_batch batches[8];"
    replace("d3d12_context.h", anchor, anchor + "\n   bool nxbox_unfenced_batches[8];")
    anchor = "   // batch hasn't been submitted before"
    replace(
        batch,
        anchor,
        r"""   auto screen = d3d12_screen(ctx->base.screen);
   if (nxbox_command_stopped().load() && SUCCEEDED(screen->dev->GetDeviceRemovedReason()))
      return false; // Retain resources referenced by the poisoned command list.
   if (ctx->nxbox_unfenced_batches[batch - ctx->batches]) {
      const HRESULT removed = screen->dev->GetDeviceRemovedReason();
      if (SUCCEEDED(removed))
         return false; // A failed Signal gives no proof that reuse is safe.
      nxbox_dred_capture(screen->dev, removed, "unfenced-batch-reuse");
   }
"""
        + anchor,
    )
    anchor = "      if (!d3d12_fence_finish(batch->fence, timeout_ns))"
    replace(
        batch,
        anchor,
        "      if (!nxbox_batch_wait(screen->dev, batch->fence->cmdqueue_fence,\n"
        "                            batch->fence->value, timeout_ns))",
    )
    anchor = "   d3d12_reset_batch(ctx, batch, OS_TIMEOUT_INFINITE);"
    # Both start and destroy must honor reset failure. Retain GPU-owned objects
    # on an unfenced live device rather than releasing them during teardown.
    replace(
        batch,
        anchor,
        "   if (!d3d12_reset_batch(ctx, batch, OS_TIMEOUT_INFINITE)) {\n"
        "      batch->has_errors = true;\n      nxbox_sync_stopped().store(true);\n      return;\n   }",
        2,
    )
    anchor = "   if (!ctx->queries_disabled)\n      d3d12_suspend_queries(ctx);"
    replace(batch, anchor, "   if (batch->has_errors) return;\n" + anchor)
    anchor = "   batch->fence = d3d12_create_fence(screen);"
    replace(
        batch,
        anchor,
        anchor
        + r"""
   if (!batch->fence) {
      /* Execute already happened. No fence must not mean 'never submitted'.
       * Only this exceptional path drains immediately, under submit_mutex.
       * Keep a poison bit if even the fallback Signal fails on a live device.
       */
      auto &unfenced = ctx->nxbox_unfenced_batches[batch - ctx->batches];
      unfenced = true;
      batch->has_errors = true;
      const UINT64 target = ++screen->fence_value;
      const HRESULT hr = screen->cmdqueue->Signal(screen->fence, target);
      if (SUCCEEDED(hr))
         unfenced = !nxbox_batch_wait(screen->dev, screen->fence, target, OS_TIMEOUT_INFINITE);
      else
         nxbox_dred_capture(screen->dev, screen->dev->GetDeviceRemovedReason(), "batch-fallback-signal");
      if (unfenced) {
         nxbox_sync_stopped().store(true);
         SetEnvironmentVariableA("NXBOX_D3D12_SYNC_ERROR", "unfenced batch retained; submissions stopped");
      }
   }
""",
    )
    # Stop recording/submitting when safe recovery is impossible. Do not fold
    # this latch into the device-loss flag: a failed Signal need not be removal.
    for name, count in ((batch, 2), ("d3d12_context.cpp", 3), ("d3d12_draw.cpp", 2)):
        suffix = " {" if name == batch else " return;"
        anchor = "if (nxbox_device_lost.load(std::memory_order_acquire))" + suffix
        replace(
            name,
            anchor,
            "if (nxbox_device_lost.load(std::memory_order_acquire) || nxbox_sync_stopped().load())"
            + suffix,
            count,
        )
    # Descriptor exhaustion can flush in the middle of draw/dispatch. If starting
    # the next batch failed, do not write that batch's still-live descriptor heap.
    for compute in (False, True):
        anchor = f"   if (!check_descriptors_left(ctx, {str(compute).lower()}))\n      d3d12_flush_cmdlist(ctx);\n   batch = d3d12_current_batch(ctx);"
        cleanup = (
            ""
            if compute
            else "      if (index_buffer && dinfo->has_user_indices)\n         pipe_resource_reference(&index_buffer, NULL);\n"
        )
        replace(
            "d3d12_draw.cpp",
            anchor,
            anchor
            + "\n   if (batch->has_errors || nxbox_sync_stopped().load()) {\n"
            + cleanup
            + "      return;\n   }",
        )
    # A wake-up must never be ORed into completion. Other users of fence_finish
    # (including resource maps and GL sync) need the same counter-based proof.
    fence = "d3d12_fence.cpp"
    replace(
        fence,
        "      complete = d3d12_fence_wait_event(fence->event, fence->event_fd, slice);",
        "      d3d12_fence_wait_event(fence->event, fence->event_fd, slice);",
    )
    replace(
        fence,
        "      complete |= fence->cmdqueue_fence->GetCompletedValue() >= fence->value;",
        "      complete = fence->cmdqueue_fence->GetCompletedValue() >= fence->value;",
    )
    target = driver / "nxbox_batch_reuse.h"
    if target.exists():
        raise RuntimeError("Pinned Mesa batch reuse helper already exists")
    helper = Path(__file__).with_name("mesa_batch_reuse.h").read_text()
    for name, source in sources.items():
        (driver / name).write_text(source)
    target.write_text(helper)


def patch_render_safety(root: Path) -> None:
    """Fix subresource ranges and retire lost-device queries without blocking."""
    driver = root / "src/gallium/drivers/d3d12"
    sources = {
        name: (driver / name).read_text()
        for name in ("d3d12_draw.cpp", "d3d12_blit.cpp", "d3d12_query.cpp")
    }

    def replace(name: str, old: str, new: str) -> None:
        if sources[name].count(old) != 1:
            raise RuntimeError(f"Pinned Mesa render safety anchor mismatch in {name}: {old[:100]}")
        sources[name] = sources[name].replace(old, new)

    replace(
        "d3d12_draw.cpp",
        "               transition_array_size = 0;",
        "               transition_array_size = 1; // One subresource per 3D mip, not per Z slice.",
    )
    # Equal-sample MSAA is the only MSAA case that reaches direct_copy_supported.
    # It still requires full rectangles, even with programmable sample positions.
    replace(
        "d3d12_blit.cpp",
        "        info->src.resource->nr_samples != info->dst.resource->nr_samples) {",
        "        MAX2(info->src.resource->nr_samples, info->dst.resource->nr_samples) > 1) {",
    )
    replace(
        "d3d12_blit.cpp",
        "      if (info->dst.box.x != 0 ||",
        """      if (info->dst.box.width != (int)u_minify(info->dst.resource->width0, info->dst.level) ||
          info->dst.box.height != (int)u_minify(info->dst.resource->height0, info->dst.level))
         return false;
      if (info->dst.box.x != 0 ||""",
    )
    replace(
        "d3d12_blit.cpp",
        "   struct pipe_resource *tmp = resolve_stencil_to_temp(ctx, info);",
        "   struct pipe_resource *tmp = resolve_stencil_to_temp(ctx, info);\n   if (!tmp) return;",
    )
    replace(
        "d3d12_blit.cpp",
        "                                       0, 1, 0, 1, 1, 1,",
        """                                       info->dst.level, 1,
                                       d3d12_subresource_id_uses_layer(dst->base.b.target) ? info->dst.box.z : 0,
                                       1, 1, 1,""",
    )
    replace(
        "d3d12_blit.cpp",
        "   dst_loc.SubresourceIndex = 1;",
        """   unsigned dst_z = info->dst.box.z;
   dst_loc.SubresourceIndex = get_subresource_id(dst->base.b.target, info->dst.level,
                                                dst->base.b.last_level + 1, dst_z, &dst_z,
                                                dst->base.b.array_size, 1);""",
    )
    replace(
        "d3d12_blit.cpp",
        "                                   info->dst.box.y, info->dst.box.z,",
        "                                   info->dst.box.y, dst_z,",
    )
    replace(
        "d3d12_query.cpp",
        '#include "d3d12_context.h"\n',
        '#include "d3d12_context.h"\n#include "nxbox_query_wait.h"\n',
    )
    replace(
        "d3d12_query.cpp",
        """   if (screen->fence->GetCompletedValue() < query->fence_value){
      if (!wait)
         return false;

      screen->fence->SetEventOnCompletion(query->fence_value, NULL);
   }

   return true;""",
        """   return nxbox_query_ready(screen->dev, screen->fence, query->fence_value, wait,
                            [] { return nxbox_command_stopped().load(); });""",
    )
    replace(
        "d3d12_query.cpp",
        "   return accumulate_result_cpu(ctx, query, result);",
        """   if (nxbox_device_lost.load(std::memory_order_acquire) || nxbox_command_stopped().load()) {
      memset(result, 0, sizeof(*result));
      return true; // Retire the query without mapping a dead-device readback resource.
   }
   return accumulate_result_cpu(ctx, query, result);""",
    )
    target = driver / "nxbox_query_wait.h"
    if target.exists():
        raise RuntimeError("Pinned Mesa query wait helper already exists")
    for name, source in sources.items():
        (driver / name).write_text(source)
    target.write_text(Path(__file__).with_name("mesa_query_wait.h").read_text())


def patch_invalid_commands(root: Path) -> None:
    """Keep promotions read-only and preserve array layers in planar copies."""
    driver = root / "src/gallium/drivers/d3d12"
    sources = {
        name: (driver / name).read_text()
        for name in ("d3d12_resource_state.cpp", "d3d12_blit.cpp", "d3d12_draw.cpp")
    }

    def replace(name: str, old: str, new: str, count: int = 1) -> None:
        if sources[name].count(old) != count:
            raise RuntimeError(
                f"Pinned Mesa invalid command anchor mismatch in {name}: {old[:100]}"
            )
        sources[name] = sources[name].replace(old, new)

    # CBV, SRV, sampler, SSBO and image tables can all be dirty in every stage.
    replace(
        "d3d12_draw.cpp",
        "#define MAX_DESCRIPTOR_TABLES (D3D12_GFX_SHADER_STAGES * 4)",
        "#define MAX_DESCRIPTOR_TABLES (D3D12_GFX_SHADER_STAGES * 5)",
    )
    replace(
        "d3d12_resource_state.cpp",
        "      if (current_state->is_promoted &&",
        "      if (current_state->is_promoted &&\n"
        "          !d3d12_is_write_state(desired_state) &&\n"
        "          !d3d12_is_write_state(current_state->state) &&",
    )
    # Even the first COMMON -> exact read promotion must retain its promotion bit.
    # A subsequent read -> write requires an explicit barrier, not a read/write union.
    replace(
        "d3d12_resource_state.cpp", "   } else if (after != state_if_promoted) {", "   } else {"
    )
    replace(
        "d3d12_blit.cpp",
        "       dst->base.b.format == PIPE_FORMAT_Z32_FLOAT_S8X24_UINT) {\n      stencil_src_res_offset",
        "       src->base.b.format == PIPE_FORMAT_Z32_FLOAT_S8X24_UINT) {\n      stencil_src_res_offset",
    )
    replace(
        "d3d12_blit.cpp",
        "       util_format_get_depth_only(dst) == src)\n      return true;",
        "       util_format_get_depth_only(dst) == src)\n"
        "      return d3d12_get_typeless_format(src) == d3d12_get_typeless_format(dst);",
    )
    replace(
        "d3d12_blit.cpp",
        "   dst_box.height = psrc_box->height;",
        "   dst_box.height = psrc_box->height;\n   dst_box.depth = psrc_box->depth;",
    )
    # CopyTextureRegion addresses one array subresource at a time. Gallium's
    # box.depth instead counts array layers; split before recording transitions.
    anchor = """                  unsigned mask)
{
   struct d3d12_batch *batch = d3d12_current_batch(ctx);

   unsigned src_subres ="""
    replace(
        "d3d12_blit.cpp",
        anchor,
        """                  unsigned mask)
{
   if (psrc_box->depth > 1 &&
       (d3d12_subresource_id_uses_layer(src->base.b.target) ||
        d3d12_subresource_id_uses_layer(dst->base.b.target))) {
      struct pipe_box src_slice = *psrc_box, dst_slice = *pdst_box;
      src_slice.depth = dst_slice.depth = 1;
      for (int layer = 0; layer < psrc_box->depth; ++layer) {
         src_slice.z = psrc_box->z + layer;
         dst_slice.z = pdst_box->z + layer;
         d3d12_direct_copy(ctx, dst, dst_level, &dst_slice, src, src_level, &src_slice, mask);
      }
      return;
   }
   struct d3d12_batch *batch = d3d12_current_batch(ctx);

   unsigned src_subres =""",
    )
    # get_subresource_id normalizes a layer into a Z offset. Do not feed that
    # normalized Z back into the next plane, or plane 1 uses layer zero.
    replace(
        "d3d12_blit.cpp",
        "   unsigned src_z = psrc_box->z;",
        "   unsigned src_z = psrc_box->z;\n   const unsigned dst_layer = dstz;",
    )
    replace(
        "d3d12_blit.cpp",
        "      src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;",
        "      src_z = psrc_box->z;\n      dstz = dst_layer;\n"
        "      src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;",
    )
    # Full array-layer copies have a nonzero box.z but zero subresource-local Z.
    # Test the latter to emit a null box for depth/stencil and MSAA layer copies.
    replace(
        "d3d12_blit.cpp",
        "psrc_box->x == 0 && psrc_box->y == 0 && psrc_box->z == 0",
        "psrc_box->x == 0 && psrc_box->y == 0 && src_z == 0",
    )
    for name, source in sources.items():
        (driver / name).write_text(source)


def patch_first_bad_batch(root: Path) -> None:
    """Align BC allocations, serialize texture feedback, and validate RTV views."""
    driver = root / "src/gallium/drivers/d3d12"
    names = (
        "d3d12_resource.cpp",
        "d3d12_blit.cpp",
        "d3d12_context.cpp",
        "d3d12_surface.cpp",
        "d3d12_surface.h",
        "d3d12_draw.cpp",
    )
    sources = {name: (driver / name).read_text() for name in names}

    def replace(name: str, old: str, new: str) -> None:
        if sources[name].count(old) != 1:
            raise RuntimeError(
                f"Pinned Mesa first bad batch anchor mismatch in {name}: {old[:100]}"
            )
        sources[name] = sources[name].replace(old, new)

    # Keep pipe_resource/GL dimensions unchanged. Only the physical allocation
    # is padded; the common CopyTextureRegion wrapper handles uploads, readbacks
    # and blits using the actual subresource dimensions (including small mips).
    replace(
        "d3d12_resource.cpp",
        "   desc.Height = templ->height0;",
        """   desc.Height = templ->height0;
   if (util_format_is_compressed(templ->format)) {
      desc.Width = ALIGN(desc.Width, util_format_get_blockwidth(templ->format));
      desc.Height = ALIGN(desc.Height, util_format_get_blockheight(templ->format));
   }""",
    )
    # Uploads already use a NULL box and fill_buffer_location's block-aligned
    # footprint. Readbacks and partial direct copies construct logical boxes;
    # expand those edges here as well as in the final command wrapper.
    for name, anchor, indent in (
        ("d3d12_resource.cpp", "      src_box.back = start_box_z + depth;", "      "),
        ("d3d12_blit.cpp", "         src_box.back = src_z + psrc_box->depth;", "         "),
    ):
        resource = "res" if name == "d3d12_resource.cpp" else "src"
        replace(
            name,
            anchor,
            anchor
            + "\n"
            + indent
            + f"src_box.right = ALIGN(src_box.right, util_format_get_blockwidth({resource}->base.b.format));\n"
            + indent
            + f"src_box.bottom = ALIGN(src_box.bottom, util_format_get_blockheight({resource}->base.b.format));",
        )
    # A null/null aliasing barrier was being used as a global texture feedback
    # flush, not to activate an aliased allocation. Preserve ordering with an
    # explicit submission/completion boundary, even when NXBOX_SYNC_BATCH=0.
    replace(
        "d3d12_context.cpp",
        """   /* D3D doesn't really have an equivalent in the legacy barrier model. When using enhanced barriers,
    * this could be a more specific global barrier. But for now, just flush the world with an aliasing barrier. */
   D3D12_RESOURCE_BARRIER aliasingBarrier;
   aliasingBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
   aliasingBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
   aliasingBarrier.Aliasing.pResourceBefore = nullptr;
   aliasingBarrier.Aliasing.pResourceAfter = nullptr;
   nxbox_journal_commands(ctx->nxbox_journal, ctx->cmdlist).ResourceBarrier(1, &aliasingBarrier);""",
        """   nxbox_journal_add(ctx->nxbox_journal, "main", "TEXTURE_BARRIER submit-and-wait");
   d3d12_flush_cmdlist_and_wait(ctx);""",
    )
    # NULL is explicitly legal for a global UAV barrier. Keep the image/SSBO
    # dependency; removing it based on current bindings misses earlier writes.
    replace(
        "d3d12_context.cpp",
        "      D3D12_RESOURCE_BARRIER uavBarrier;",
        "      D3D12_RESOURCE_BARRIER uavBarrier = {};",
    )
    replace(
        "d3d12_surface.h",
        "   struct pipe_resource *rgba_texture;",
        """   struct pipe_resource *rgba_texture;
   unsigned nxbox_rtv_format;
   unsigned nxbox_uint_rtv_format;""",
    )
    replace(
        "d3d12_surface.cpp",
        "   DXGI_FORMAT dxgi_format = d3d12_get_resource_rt_format(tpl->format);",
        """   DXGI_FORMAT dxgi_format = d3d12_get_resource_rt_format(tpl->format);
   surface->nxbox_rtv_format = (unsigned)dxgi_format;""",
    )
    replace(
        "d3d12_surface.cpp",
        "                     &surface->uint_rtv_handle, DXGI_FORMAT_R8G8B8A8_UINT);",
        """                     &surface->uint_rtv_handle, DXGI_FORMAT_R8G8B8A8_UINT);
      surface->nxbox_uint_rtv_format = (unsigned)DXGI_FORMAT_R8G8B8A8_UINT;""",
    )
    anchor = "   struct d3d12_rasterizer_state *rast = ctx->gfx_pipeline_state.rast;\n   if (rast->twoface_back) {"
    replace(
        "d3d12_draw.cpp",
        anchor,
        """   // Compare the PSO format with the format actually used to create the RTV.
   // A nearby resource transition may refer to an SRV, not this attachment.
   for (unsigned i = 0; i < ctx->fb.nr_cbufs; ++i) {
      if (!ctx->fb.cbufs[i]) continue;
      struct d3d12_surface *surface = d3d12_surface(ctx->fb.cbufs[i]);
      const unsigned view = conversion_modes[i] == D3D12_SURFACE_CONVERSION_NONE ?
         surface->nxbox_rtv_format : surface->nxbox_uint_rtv_format;
      const unsigned expected = (unsigned)d3d12_rtv_format(ctx, i);
      if (view != expected) {
         nxbox_journal_add(ctx->nxbox_journal, "main",
                           "RTV_FORMAT_MISMATCH slot=%u pso=%u view=%u draw-skipped", i, expected, view);
         debug_printf("NXBOX RTV_FORMAT_MISMATCH slot=%u pso=%u view=%u draw-skipped\\n", i, expected, view);
         return;
      }
   }

"""
        + anchor,
    )
    replace(
        "d3d12_draw.cpp",
        """            (unsigned)(conversion_modes[i] == D3D12_SURFACE_CONVERSION_NONE ?
               d3d12_get_resource_rt_format(ctx->fb.cbufs[i]->format) : DXGI_FORMAT_R8G8B8A8_UINT) : 0;""",
        """            (conversion_modes[i] == D3D12_SURFACE_CONVERSION_NONE ?
               d3d12_surface(ctx->fb.cbufs[i])->nxbox_rtv_format :
               d3d12_surface(ctx->fb.cbufs[i])->nxbox_uint_rtv_format) : 0;
         if (i < ctx->fb.nr_cbufs && ctx->fb.cbufs[i]) {
            struct d3d12_surface *surface = d3d12_surface(ctx->fb.cbufs[i]);
            struct pipe_resource *texture = conversion_modes[i] == D3D12_SURFACE_CONVERSION_BGRA_UINT ?
               surface->rgba_texture : surface->base.texture;
            char resource[192];
            NxboxJournalCommands::resource(resource, sizeof(resource),
               d3d12_resource_resource(d3d12_resource(texture)));
            nxbox_journal_add(ctx->nxbox_journal, "main",
                              "RTV_BIND slot=%u view=%u res=%s", i, rtv[i], resource);
         }""",
    )
    for name, source in sources.items():
        (driver / name).write_text(source)


def instrument_api_calls(source: str, methods: list[str], name: str):
    """Wrap complete postfix receivers without adding local identifiers.

    Walk backwards across balanced calls/indexes and member access. Matching
    only a receiver suffix silently turns a free wrapper into a member call.
    Reject unsupported prefixes rather than publishing a partial receiver.
    """
    import re

    token = re.compile(r"//[^\n]*|/\*[\s\S]*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'")
    mask = token.sub(lambda m: "".join("\n" if c == "\n" else " " for c in m[0]), source)
    identifier = re.compile(r"[A-Za-z_]\w*$")

    def receiver_start(end):
        end = len(mask[:end].rstrip())
        if not end:
            raise RuntimeError(f"Missing D3D12 receiver in {name}")
        if mask[end - 1] in ")]":
            closing = mask[end - 1]
            opening = "(" if closing == ")" else "["
            depth, start = 1, end - 1
            while depth and start:
                start -= 1
                if mask[start] == closing:
                    depth += 1
                elif mask[start] == opening:
                    depth -= 1
            if depth:
                raise RuntimeError(f"Unbalanced D3D12 receiver in {name}")
            # Calls/indexes extend a preceding postfix expression; grouping
            # parentheses are already a complete primary expression.
            prefix = mask[:start].rstrip()
            if prefix and (prefix[-1].isalnum() or prefix[-1] in "_)]"):
                return receiver_start(len(prefix))
        else:
            match = identifier.search(mask[:end])
            if not match:
                raise RuntimeError(f"Unrecognized D3D12 receiver in {name}")
            start = match.start()
        prefix = mask[:start].rstrip()
        if prefix.endswith("->"):
            return receiver_start(len(prefix) - 2)
        if prefix.endswith("."):
            return receiver_start(len(prefix) - 1)
        if prefix.endswith(("::", ">", "*", "&")):
            raise RuntimeError(f"Unsupported D3D12 receiver prefix in {name}: {prefix[-40:]}")
        return start

    pattern = re.compile(r"->(?P<api>" + "|".join(methods) + r")\s*\(")
    calls = []
    edits = []
    for match in pattern.finditer(mask):
        start = receiver_start(match.start())
        obj = source[start : match.start()].rstrip()
        line = source.count("\n", 0, start) + 1
        method = match["api"]
        calls.append({"file": name, "line": line, "api": method, "receiver": obj})
        edits.append((start, match.end(), f'nxbox_api({obj}, "{name}:{line}").{method}('))
        # NULL loses its null-pointer-constant semantics when forwarded.
        end, depth = match.end(), 1
        while depth:
            if mask[end] == "(":
                depth += 1
            elif mask[end] == ")":
                depth -= 1
            end += 1
        for null in re.finditer(r"\bNULL\b", mask[match.end() : end]):
            edits.append((match.end() + null.start(), match.end() + null.end(), "nullptr"))
    boundary = len(source)
    for start, end, replacement in sorted(set(edits), reverse=True):
        if end > boundary:
            raise RuntimeError(f"Overlapping D3D12 instrumentation in {name}")
        source = source[:start] + replacement + source[end:]
        boundary = start
    return source, calls


def patch_heap_policy(root: Path) -> None:
    """Use abstract GPU heaps and feature-gated residency on the UWP build."""
    driver = root / "src/gallium/drivers/d3d12"
    sources = {
        name: (driver / name).read_text()
        for name in ("d3d12_resource.cpp", "d3d12_bufmgr.cpp", "d3d12_screen.cpp")
    }

    def replace(name, old, new, count=1):
        if sources[name].count(old) != count:
            raise RuntimeError(f"Pinned Mesa heap policy anchor mismatch in {name}: {old[:80]}")
        sources[name] = sources[name].replace(old, new)

    for name in sources:
        anchor = '#include "d3d12_screen.h"'
        replace(name, anchor, anchor + '\n#include "nxbox_heap_policy.h"')
    replace(
        "d3d12_resource.cpp",
        "GetCustomHeapProperties(screen->dev, D3D12_HEAP_TYPE_DEFAULT)",
        "nxbox_heap_properties(screen->architecture, D3D12_HEAP_TYPE_DEFAULT)",
        2,
    )
    replace(
        "d3d12_bufmgr.cpp",
        "GetCustomHeapProperties(dev, heap_type)",
        "nxbox_heap_properties(screen->architecture, heap_type)",
    )
    replace(
        "d3d12_screen.cpp",
        "      screen->support_create_not_resident = true;",
        """      // Device8 plus OPTIONS7 success is a conservative runtime capability gate.
      D3D12_FEATURE_DATA_D3D12_OPTIONS7 nxbox_options7{};
      screen->support_create_not_resident = SUCCEEDED(screen->dev->CheckFeatureSupport(
         D3D12_FEATURE_D3D12_OPTIONS7, &nxbox_options7, sizeof(nxbox_options7)));""",
    )
    anchor = "   static constexpr uint64_t known_good_warp_version"
    replace(
        "d3d12_screen.cpp",
        anchor,
        """   nxbox_report_heap_policy(screen->architecture, screen->support_create_not_resident);

"""
        + anchor,
    )
    for name, source in sources.items():
        (driver / name).write_text(source)
    (driver / "nxbox_heap_policy.h").write_text(
        Path(__file__).with_name("mesa_heap_policy.h").read_text()
    )


def patch_device_api_ring(root: Path) -> None:
    """Instrument all matching calls, including video and generated helper headers.

    Mask comments/strings before matching. Fail closed on new receiver syntax so
    a pin update cannot silently introduce an unchecked call. Keep a per-call
    source inventory beside the patched driver for host verification.
    """
    import json
    import re

    driver = root / "src/gallium/drivers/d3d12"
    helper = Path(__file__).with_name("mesa_api_ring.h").read_text()
    methods = re.findall(r"NXBOX_API_METHOD\((\w+)\)", helper)
    methods.remove("name")
    methods.extend(("CopyBufferRegion", "CopyDescriptors"))
    sources = {p.name: p.read_text() for p in driver.iterdir() if p.suffix in (".cpp", ".h")}

    def replace(name, old, new, count=1):
        if sources[name].count(old) != count:
            raise RuntimeError(f"Pinned Mesa API anchor mismatch in {name}: {old[:80]}")
        sources[name] = sources[name].replace(old, new)

    # Do not issue another MakeResident for an already-resident resource, and
    # remove permanent resources from the eviction LRU before changing status.
    replace(
        "d3d12_residency.cpp",
        """      /* Mark as permanently resident*/
      base_bo->residency_status = d3d12_permanently_resident;

      /* If it wasn't made resident before, make it*/
      bool was_made_resident = (base_bo->residency_status == d3d12_resident);""",
        """      const bool was_made_resident = (base_bo->residency_status == d3d12_resident);
      if (was_made_resident)
         list_del(&base_bo->residency_list_entry);

      /* If it wasn't made resident before, make it*/""",
    )
    replace(
        "d3d12_residency.cpp",
        """         assert(SUCCEEDED(hr));
      }
   }
   mtx_unlock""",
        """         if (FAILED(hr)) {
            mtx_unlock(&screen->submit_mutex);
            return;
         }
      }
      base_bo->residency_status = d3d12_permanently_resident;
   }
   mtx_unlock""",
    )
    # Retry a full pending batch after OOM; do not gather it twice. Retire
    # accepted partial batches as well, so their BOs are not made resident twice.
    replace(
        "d3d12_residency.cpp",
        "if ((available_memory || !anything_to_wait_for) && batch_count < residency_batch_size)",
        "if (available_memory > 0 || !anything_to_wait_for || batch_count)",
    )
    replace(
        "d3d12_residency.cpp",
        "for (; entry; entry = _mesa_set_next_entry(base_bo_set, entry))",
        "for (; entry && batch_count < residency_batch_size; entry = _mesa_set_next_entry(base_bo_set, entry))",
    )
    replace(
        "d3d12_residency.cpp",
        """         if (SUCCEEDED(hr) && batch_count == residency_batch_size) {
            batch_count = 0;
            size_to_make_resident -= batch_memory_size;
            continue;
         }""",
        """         if (SUCCEEDED(hr) && batch_count) {
            if (batch_count == residency_batch_size)
               entry = _mesa_set_next_entry(base_bo_set, entry);
            batch_count = 0;
            size_to_make_resident -= batch_memory_size;
            batch_memory_size = 0;
            if (!entry)
               break;
            continue;
         }""",
    )
    replace(
        "d3d12_residency.cpp",
        "   screen->fence->SetEventOnCompletion(target_fence, nullptr);",
        """   if (FAILED(screen->fence->SetEventOnCompletion(target_fence, nullptr)) ||
       screen->fence->GetCompletedValue() == UINT64_MAX ||
       screen->fence->GetCompletedValue() < target_fence)
      return; // Never evict allocations whose completion was not established.""",
    )
    # Both draw paths must recheck after flushing: a single draw can exceed an
    # empty heap too. Never append descriptors or set tables after that failure.
    for compute in ("false", "true"):
        replace(
            "d3d12_draw.cpp",
            f"   if (!check_descriptors_left(ctx, {compute}))\n      d3d12_flush_cmdlist(ctx);",
            f"""   if (!check_descriptors_left(ctx, {compute})) {{
      d3d12_flush_cmdlist(ctx);
      if (!check_descriptors_left(ctx, {compute})) {{
         SetEnvironmentVariableA("NXBOX_D3D12_SYNC_ERROR", "descriptor capacity exceeded; draw skipped");
         {"if (index_buffer && dinfo->has_user_indices) pipe_resource_reference(&index_buffer, nullptr);" if compute == "false" else ""}
         return;
      }}
   }}""",
        )
    replace(
        "d3d12_descriptor_pool.cpp",
        "      list_addtail(&valid_heap->link, &pool->heaps);",
        "      if (!valid_heap)\n         return 0;\n      list_addtail(&valid_heap->link, &pool->heaps);",
    )
    # Video completion helpers previously treated removal's UINT64_MAX as
    # completion, and release builds discarded failed/timeout wait results.
    for name, fence in (
        ("d3d12_video_dec.cpp", "fence"),
        ("d3d12_video_proc.cpp", "pD3D12Proc->m_spFence"),
    ):
        ending = (
            "\n}\n\nbool\nd3d12_video_decoder_sync_completion"
            if name.endswith("dec.cpp")
            else "\n\nensure_fence_finished_fail:"
        )
        replace(
            name,
            "   return wait_result;" + ending,
            f"""   completedValue = {fence}->GetCompletedValue();
   return wait_result && completedValue != UINT64_MAX && completedValue >= fenceValueToWaitOn;"""
            + ending,
        )
        replace(name, "   assert(wait_result);", "   if (!wait_result)\n      return false;")
        replace(name, "assert(wait_res);", "if (!wait_res) return;")
    replace(
        "d3d12_video_enc.cpp",
        "      d3d12_video_encoder_ensure_fence_finished(codec, fenceValueToWaitOn, timeout_ns);",
        """      d3d12_video_encoder_ensure_fence_finished(codec, fenceValueToWaitOn, timeout_ns);
      const uint64_t verified = pD3D12Enc->m_spFence->GetCompletedValue();
      if (verified == UINT64_MAX || verified < fenceValueToWaitOn)
         return; // Preserve in-flight resources and allocator on timeout/removal.""",
    )
    # The PSO boundary must enter the ring before an attempted creation.
    replace(
        "nxbox_lifetime.h",
        "   const HRESULT before = dev->GetDeviceRemovedReason();",
        """   const HRESULT before = dev->GetDeviceRemovedReason();
   nxbox_api_record(dev, kind, "pso-boundary", "phase=before", S_OK, before);""",
    )
    replace(
        "nxbox_lifetime.h",
        "   const HRESULT after = dev->GetDeviceRemovedReason();",
        """   const HRESULT after = dev->GetDeviceRemovedReason();
   nxbox_api_record(dev, kind, "pso-boundary", "phase=after", hr, after);""",
    )
    replace(
        "nxbox_dred.h",
        '#include "nxbox_lifetime.h"',
        '#include "nxbox_api_ring.h"\n#include "nxbox_lifetime.h"',
    )
    replace(
        "nxbox_dred.h",
        """   if (SUCCEEDED(removed))
      return;
   std::lock_guard<std::mutex> lock(nxbox_dred_mutex);""",
        """   if (SUCCEEDED(removed))
      return;
   nxbox_api_capture(dev, removed, where);
   std::lock_guard<std::mutex> lock(nxbox_dred_mutex);""",
    )
    replace(
        "d3d12_screen.cpp",
        "#ifndef DEBUG\n      if (d3d12_debug & D3D12_DEBUG_DEBUG_LAYER)\n#endif",
        "      if (!nxbox_debug_enabled() && (d3d12_debug & D3D12_DEBUG_DEBUG_LAYER))",
    )
    # ResolveSubresource addresses a single array layer. Mesa's old fast path
    # passed only the mip and resolved layer zero even for array-layer blits.
    replace(
        "d3d12_blit.cpp",
        "   // can only resolve full subresource\n",
        """   // ResolveSubresource cannot address an offset rectangle or a 3D slice.
   if (info->src.box.x || info->src.box.y || info->dst.box.x || info->dst.box.y ||
       info->src.box.z < 0 || info->dst.box.z < 0 ||
       info->src.box.depth <= 0 || info->src.box.depth != info->dst.box.depth ||
       info->src.resource->target == PIPE_TEXTURE_3D ||
       info->dst.resource->target == PIPE_TEXTURE_3D ||
       (unsigned)info->src.box.z > info->src.resource->array_size ||
       (unsigned)info->dst.box.z > info->dst.resource->array_size ||
       (unsigned)info->src.box.depth > info->src.resource->array_size - (unsigned)info->src.box.z ||
       (unsigned)info->dst.box.depth > info->dst.resource->array_size - (unsigned)info->dst.box.z)
      return false;

   // can only resolve full subresource
""",
    )
    # The prior journal pass has replaced the receiver, not these arguments.
    replace(
        "d3d12_blit.cpp",
        """   nxbox_journal_commands(ctx->nxbox_journal, ctx->cmdlist).ResolveSubresource(
      d3d12_resource_resource(dst), info->dst.level,
      d3d12_resource_resource(src), info->src.level,
      dxgi_format);""",
        """   for (int layer = 0; layer < info->src.box.depth; ++layer) {
      const unsigned dst_sub = info->dst.level +
         (info->dst.box.z + layer) * (dst->base.b.last_level + 1);
      const unsigned src_sub = info->src.level +
         (info->src.box.z + layer) * (src->base.b.last_level + 1);
      nxbox_journal_commands(ctx->nxbox_journal, ctx->cmdlist).ResolveSubresource(
         d3d12_resource_resource(dst), dst_sub,
         d3d12_resource_resource(src), src_sub, dxgi_format);
   }""",
    )
    # Debug-layer activation must precede device creation. It is never enabled
    # reactively after a Close failure (doing so removes an existing device).
    replace(
        "d3d12_screen.cpp",
        "ID3D12DeviceFactory *factory = try_create_device_factory(screen->d3d12_mod);",
        "ID3D12DeviceFactory *factory = nxbox_debug_enabled() ? nullptr : try_create_device_factory(screen->d3d12_mod);",
    )
    replace(
        "d3d12_screen.cpp",
        "      nxbox_dred_enable(screen->d3d12_mod, factory);",
        """      if (nxbox_debug_enabled()) {
         typedef HRESULT(WINAPI *NxboxGetDebug)(REFIID, void **);
         auto get_debug = (NxboxGetDebug)util_dl_get_proc_address(screen->d3d12_mod, "D3D12GetDebugInterface");
         ID3D12Debug *debug = nullptr;
         const HRESULT hr = get_debug ? get_debug(IID_PPV_ARGS(&debug)) : E_NOINTERFACE;
         if (SUCCEEDED(hr) && debug) {
            debug->EnableDebugLayer();
            char gbv[4] = {};
            if (GetEnvironmentVariableA("NXBOX_D3D12_GBV", gbv, sizeof(gbv)) == 1 && gbv[0] == '1') {
               ID3D12Debug1 *debug1 = nullptr;
               if (SUCCEEDED(debug->QueryInterface(IID_PPV_ARGS(&debug1))) && debug1) {
                  debug1->SetEnableGPUBasedValidation(TRUE);
                  debug1->SetEnableSynchronizedCommandQueueValidation(TRUE);
                  debug1->Release();
                  SetEnvironmentVariableA("NXBOX_D3D12_GBV_STATUS", "enabled=1");
               } else {
                  SetEnvironmentVariableA("NXBOX_D3D12_GBV_STATUS", "unavailable");
               }
            }
            debug->Release();
            SetEnvironmentVariableA("NXBOX_D3D12_DEBUG_STATUS", "enabled=1");
         } else {
            char text[96];
            snprintf(text, sizeof(text), "hr=0x%08x stage=D3D12GetDebugInterface", (unsigned)hr);
            SetEnvironmentVariableA("NXBOX_D3D12_DEBUG_UNAVAILABLE", text);
         }
      }
      nxbox_dred_enable(screen->d3d12_mod, factory);""",
    )
    replace(
        "d3d12_screen.cpp",
        "      info_queue->PushStorageFilter(&NewFilter);",
        "      if (!nxbox_debug_enabled())\n         info_queue->PushStorageFilter(&NewFilter);",
    )
    replace(
        "d3d12_screen.cpp",
        "   screen->adapter_luid = GetAdapterLuid(screen->dev);",
        "   nxbox_infoqueue_setup(screen->dev);\n   screen->adapter_luid = GetAdapterLuid(screen->dev);",
    )
    inventory = []
    for name, source in sources.items():
        if name == "nxbox_api_ring.h":
            continue
        source, calls = instrument_api_calls(source, methods, name)
        inventory.extend(calls)
        if calls:
            if name.endswith(".cpp"):
                # Some first headers (notably blit.h) contain only forward
                # declarations. Import the COM types explicitly before the ring.
                anchor = re.search(r'^#include "d3d12_[^"\n]+".*$', source, re.M)
                if not anchor:
                    raise RuntimeError(f"Missing D3D12 include in {name}")
                source = (
                    source[: anchor.end()]
                    + '\n#include "d3d12_common.h"\n#include "nxbox_api_ring.h"'
                    + source[anchor.end() :]
                )
            else:
                source = source.replace(
                    "#pragma once", '#pragma once\n#include "nxbox_api_ring.h"', 1
                )
        sources[name] = source
    for name, source in sources.items():
        (driver / name).write_text(source)
    (driver / "nxbox_api_ring.h").write_text(helper)
    (driver / "nxbox_list_ring.h").write_text(
        Path(__file__).with_name("mesa_list_ring.h").read_text()
    )
    (driver / "nxbox_api_inventory.json").write_text(
        json.dumps(sorted(inventory, key=lambda x: (x["file"], x["line"])), indent=2) + "\n"
    )


def patch_query_policy(root: Path) -> None:
    """Keep pipeline-statistics and stream-output statistics queries off the Xbox GPU.

    Both Breath of the Wild and Mario Kart 8 lose the device right after a batch that begins an
    occlusion, a pipeline-statistics and a stream-output-statistics query (D3D12 query types 0, 3
    and 4); the API ring shows Reset, SetDescriptorHeaps, three BeginQuery calls and then the
    removal. Those two statistics types never feed anything Eden presents, so they become software
    queries that report zero, unless NXBOX_D3D12_QUERIES=full."""
    path = root / "src/gallium/drivers/d3d12/d3d12_query.cpp"
    source = path.read_text()

    def replace(old, new):
        nonlocal source
        if source.count(old) != 1:
            raise RuntimeError(f"Pinned Mesa query policy anchor mismatch: {old[:80]}")
        source = source.replace(old, new)

    replace(
        "#include <dxguids/dxguids.h>\n",
        "#include <dxguids/dxguids.h>\n"
        "#include <stdlib.h>\n"
        "#include <string.h>\n"
        "\n"
        "static bool\n"
        "nxbox_soft_subquery(const struct d3d12_query *q, unsigned sub_query)\n"
        "{\n"
        "   static int mode = -1; /* 0 safe, 1 full, 2 none */\n"
        "   if (mode < 0) {\n"
        "      const char *text = getenv(\"NXBOX_D3D12_QUERIES\");\n"
        "      mode = text && !strcmp(text, \"full\") ? 1 : (text && !strcmp(text, \"none\") ? 2 : 0);\n"
        "   }\n"
        "   if (mode == 1)\n"
        "      return false;\n"
        "   D3D12_QUERY_TYPE type = q->subqueries[sub_query].d3d12qtype;\n"
        "   if (mode == 2)\n"
        "      return type != D3D12_QUERY_TYPE_TIMESTAMP;\n"
        "   return type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS ||\n"
        "          (type >= D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 &&\n"
        "           type <= D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3);\n"
        "}\n",
    )
    replace(
        "   struct pipe_transfer *transfer = NULL;\n",
        "   struct pipe_transfer *transfer = NULL;\n"
        "   if (nxbox_soft_subquery(q_parent, sub_query)) {\n"
        "      memset(result, 0, sizeof(*result));\n"
        "      /* A software occlusion query reports one visible sample, so nothing is culled. */\n"
        "      if (q_parent->subqueries[sub_query].d3d12qtype == D3D12_QUERY_TYPE_OCCLUSION ||\n"
        "          q_parent->subqueries[sub_query].d3d12qtype == D3D12_QUERY_TYPE_BINARY_OCCLUSION)\n"
        "         result->u64 = 1;\n"
        "      return true;\n"
        "   }\n",
    )
    replace(
        "subquery_should_be_active(struct d3d12_context *ctx, struct d3d12_query *q, unsigned sub_query)\n{\n",
        "subquery_should_be_active(struct d3d12_context *ctx, struct d3d12_query *q, unsigned sub_query)\n{\n"
        "   if (nxbox_soft_subquery(q, sub_query))\n"
        "      return false;\n",
    )
    path.write_text(source)


def patch_bisect_switches(root: Path) -> None:
    """Runtime switches that drop whole classes of GPU work, to bisect a device removal on the
    console without rebuilding: NXBOX_SKIP_COMPUTE=1 drops dispatches and NXBOX_SKIP_SO=1 drops
    draws that have stream-output targets bound. Both are off by default."""
    path = root / "src/gallium/drivers/d3d12/d3d12_draw.cpp"
    source = path.read_text()
    gfx_old = (
        "   if (!ctx->current_gfx_pso) {\n"
        "      if (index_buffer && dinfo->has_user_indices)\n"
        "         pipe_resource_reference(&index_buffer, NULL);\n"
        "      return;\n"
        "   }\n"
    )
    gfx_new = gfx_old + (
        "   if (ctx->gfx_pipeline_state.num_so_targets && nxbox_switch_on(\"NXBOX_SKIP_SO\")) {\n"
        "      if (index_buffer && dinfo->has_user_indices)\n"
        "         pipe_resource_reference(&index_buffer, NULL);\n"
        "      return;\n"
        "   }\n"
        "   if (nxbox_draw_bucket_skip(ctx)) {\n"
        "      if (index_buffer && dinfo->has_user_indices)\n"
        "         pipe_resource_reference(&index_buffer, NULL);\n"
        "      return;\n"
        "   }\n"
    )
    compute_old = (
        "   if (!ctx->current_compute_pso)\n      return;\n"
    )
    compute_new = compute_old + (
        "   if (nxbox_switch_on(\"NXBOX_SKIP_COMPUTE\"))\n      return;\n"
    )
    for old in (gfx_old, compute_old):
        if source.count(old) != 1:
            raise RuntimeError("Pinned Mesa d3d12_draw.cpp does not match the bisect switch patch")
    helper = (
        "#include <stdlib.h>\n\n"
        "static bool\n"
        "nxbox_switch_on(const char *name)\n"
        "{\n"
        "   const char *value = getenv(name);\n"
        "   return value && value[0] == '1';\n"
        "}\n\n"
        "/* NXBOX_DRAW_BUCKETS=<n>,<k> drops every draw whose vertex+fragment shader hash is k mod n,\n"
        " * so the draw that removes the device can be found by halving the set run after run. */\n"
        "#include <stdio.h>\n"
        "static uint64_t\n"
        "nxbox_bytes_hash(const void *bytes, size_t length)\n"
        "{\n"
        "   struct Entry { const void *pointer; size_t length; uint64_t hash; };\n"
        "   static Entry cache[1024];\n"
        "   Entry &slot = cache[((uintptr_t)bytes >> 4) & 1023];\n"
        "   if (slot.pointer == bytes && slot.length == length)\n"
        "      return slot.hash;\n"
        "   uint64_t hash = 14695981039346656037ull;\n"
        "   for (size_t i = 0; i < length; ++i)\n"
        "      hash = (hash ^ ((const unsigned char *)bytes)[i]) * 1099511628211ull;\n"
        "   slot = {bytes, length, hash};\n"
        "   return hash;\n"
        "}\n\n"
        "static bool\n"
        "nxbox_draw_bucket_skip(struct d3d12_context *ctx)\n"
        "{\n"
        "   static bool ready = false;\n"
        "   static unsigned buckets = 0, chosen = 0;\n"
        "   if (!ready) {\n"
        "      const char *value = getenv(\"NXBOX_DRAW_BUCKETS\");\n"
        "      if (value)\n"
        "         sscanf(value, \"%u,%u\", &buckets, &chosen);\n"
        "      ready = true;\n"
        "   }\n"
        "   if (buckets < 2)\n"
        "      return false;\n"
        "   uint64_t hash = 0;\n"
        "   for (int stage : {PIPE_SHADER_VERTEX, PIPE_SHADER_FRAGMENT}) {\n"
        "      struct d3d12_shader_selector *selector = ctx->gfx_stages[stage];\n"
        "      if (selector && selector->current && selector->current->bytecode)\n"
        "         hash ^= nxbox_bytes_hash(selector->current->bytecode, selector->current->bytecode_length);\n"
        "   }\n"
        "   const bool skip = (hash % buckets) == chosen;\n"
        "   if (skip) {\n"
        "      static uint64_t seen[64];\n"
        "      static unsigned count = 0;\n"
        "      bool known = false;\n"
        "      for (unsigned i = 0; i < count; ++i)\n"
        "         known |= seen[i] == hash;\n"
        "      const char *log = getenv(\"NXBOX_DRAW_BUCKET_LOG\");\n"
        "      if (!known && count < 64 && log) {\n"
        "         seen[count++] = hash;\n"
        "         if (FILE *file = fopen(log, \"a\")) {\n"
        "            struct d3d12_shader_selector *vs = ctx->gfx_stages[PIPE_SHADER_VERTEX];\n"
        "            struct d3d12_shader_selector *ps = ctx->gfx_stages[PIPE_SHADER_FRAGMENT];\n"
        "            fprintf(file, \"hash=%016llx vs=%zu ps=%zu\\n\", (unsigned long long)hash,\n"
        "                    vs && vs->current ? vs->current->bytecode_length : 0,\n"
        "                    ps && ps->current ? ps->current->bytecode_length : 0);\n"
        "            fclose(file);\n"
        "         }\n"
        "      }\n"
        "   }\n"
        "   return skip;\n"
        "}\n\n"
    )
    source = source.replace(gfx_old, gfx_new).replace(compute_old, compute_new)
    anchor = '#include "util/u_math.h"\n'
    if source.count(anchor) != 1:
        raise RuntimeError("Pinned Mesa d3d12_draw.cpp does not match the bisect helper anchor")
    path.write_text(source.replace(anchor, anchor + "\n" + helper.rstrip("\n") + "\n"))


def patch_bo_counters(root: Path) -> None:
    """Count the live resource objects and their footprint bytes, printed with every API ring line,
    to see whether the device is lost when the number of resources or their total size peaks."""
    path = root / "src/gallium/drivers/d3d12/d3d12_bufmgr.cpp"
    source = path.read_text()
    wrap_old = (
        "   if (residency == d3d12_resident) {\n"
        "      mtx_lock(&screen->submit_mutex);\n"
        "      list_add(&bo->residency_list_entry, &screen->residency_list);\n"
    )
    wrap_new = (
        "   nxbox_api_ring().bo_live.fetch_add(1);\n"
        "   nxbox_api_ring().bo_total.fetch_add(1);\n"
        "   nxbox_api_ring().bo_live_bytes.fetch_add((int64_t)bo->estimated_size);\n"
    ) + wrap_old
    free_old = "      if (bo->res)\n         bo->res->Release();\n"
    free_new = (
        "      if (bo->res) {\n"
        "         nxbox_api_ring().bo_live.fetch_sub(1);\n"
        "         nxbox_api_ring().bo_live_bytes.fetch_sub((int64_t)bo->estimated_size);\n"
        "         static const bool leak = [] {\n"
        "            const char *value = getenv(\"NXBOX_LEAK_RESOURCES\");\n"
        "            return value && value[0] == '1';\n"
        "         }();\n"
        "         if (!leak) /* NXBOX_LEAK_RESOURCES=1 never frees: a premature release would then vanish */\n"
        "            bo->res->Release();\n"
        "      }\n"
    )
    for old in (wrap_old, free_old):
        if source.count(old) != 1:
            raise RuntimeError("Pinned Mesa d3d12_bufmgr.cpp does not match the bo counter patch")
    path.write_text(source.replace(wrap_old, wrap_new).replace(free_old, free_new))


def patch_vidmem_report(root: Path) -> None:
    """Publish the video memory budget and usage the residency manager sees, every time it
    refreshes them, so a device loss near the budget shows up in the diag."""
    path = root / "src/gallium/drivers/d3d12/d3d12_residency.cpp"
    source = path.read_text()
    old = (
        "      screen->get_memory_info(screen, &mem_info);\n"
        "\n"
        "      int64_t available_memory = (int64_t)mem_info.budget - (int64_t)mem_info.usage;\n"
    )
    new = (
        "      screen->get_memory_info(screen, &mem_info);\n"
        "      {\n"
        "         static uint64_t peak_usage = 0;\n"
        "         if (mem_info.usage > peak_usage)\n"
        "            peak_usage = mem_info.usage;\n"
        "         char text[160];\n"
        "         snprintf(text, sizeof(text), \"budget=%llu usage=%llu peak=%llu to_make_resident=%llu\",\n"
        "                  (unsigned long long)mem_info.budget, (unsigned long long)mem_info.usage,\n"
        "                  (unsigned long long)peak_usage, (unsigned long long)size_to_make_resident);\n"
        "         SetEnvironmentVariableA(\"NXBOX_D3D12_VIDMEM\", text);\n"
        "      }\n"
        "\n"
        "      int64_t available_memory = (int64_t)mem_info.budget - (int64_t)mem_info.usage;\n"
    )
    if source.count(old) != 1:
        raise RuntimeError("Pinned Mesa d3d12_residency.cpp does not match the vidmem report patch")
    if "#include <stdio.h>" not in source:
        source = source.replace('#include "d3d12_residency.h"', '#include "d3d12_residency.h"\n#include <stdio.h>', 1)
    path.write_text(source.replace(old, new))


def patch_no_evict(root: Path) -> None:
    """NXBOX_NO_EVICT=1 keeps every resource resident: a GPU read of an evicted resource that the
    batch did not track would remove the device asynchronously, which is what Breath of the Wild
    shows. Counters of evictions are published either way."""
    path = root / "src/gallium/drivers/d3d12/d3d12_residency.cpp"
    source = path.read_text()
    helper = (
        "#include <stdlib.h>\n"
        "static bool\n"
        "nxbox_no_evict(void)\n"
        "{\n"
        "   static int value = -1;\n"
        "   if (value < 0) {\n"
        "      const char *text = getenv(\"NXBOX_NO_EVICT\");\n"
        "      value = text && text[0] == '1';\n"
        "   }\n"
        "   return value == 1;\n"
        "}\n"
        "static void\n"
        "nxbox_count_eviction(const char *how, unsigned count)\n"
        "{\n"
        "   static unsigned aged = 0, budget = 0;\n"
        "   if (how[0] == 'a') aged += count; else budget += count;\n"
        "   char text[96];\n"
        "   snprintf(text, sizeof(text), \"aged=%u budget=%u no_evict=%d\", aged, budget, nxbox_no_evict() ? 1 : 0);\n"
        "   SetEnvironmentVariableA(\"NXBOX_D3D12_EVICT\", text);\n"
        "}\n\n"
    )
    aged_old = (
        "evict_aged_allocations(struct d3d12_screen *screen, uint64_t completed_fence, int64_t time, int64_t grace_period)\n"
        "{\n"
    )
    aged_new = aged_old + "   if (nxbox_no_evict())\n      return;\n"
    budget_old = (
        "evict_to_fence_or_budget(struct d3d12_screen *screen, uint64_t target_fence, uint64_t current_usage, uint64_t target_budget)\n"
        "{\n"
    )
    budget_new = budget_old + "   if (nxbox_no_evict())\n      return;\n"
    for old in (aged_old, budget_old):
        if source.count(old) != 1:
            raise RuntimeError("Pinned Mesa d3d12_residency.cpp does not match the no-evict patch")
    source = source.replace(aged_old, aged_new).replace(budget_old, budget_new)
    # Without eviction the residency loop must never spin waiting for room: make resident anyway.
    gate_old = "if (available_memory > 0 || !anything_to_wait_for || batch_count)"
    gate_new = "if (available_memory > 0 || !anything_to_wait_for || batch_count || nxbox_no_evict())"
    room_old = "            if (anything_to_wait_for &&\n"
    room_new = "            if (anything_to_wait_for && !nxbox_no_evict() &&\n"
    for old in (gate_old, room_old):
        if source.count(old) != 1:
            raise RuntimeError("Pinned Mesa d3d12_residency.cpp does not match the no-evict loop patch")
    source = source.replace(gate_old, gate_new).replace(room_old, room_new)
    # Count what each path evicts. The API ring already wrapped the calls as
    # nxbox_api(...).Evict(num_pending_evictions, to_evict); count them by function.
    call = ".Evict(num_pending_evictions, to_evict);"
    split = source.index("static void\nevict_to_fence_or_budget")
    head, tail = source[:split], source[split:]
    if head.count(call) < 1 or tail.count(call) < 1:
        raise RuntimeError("Pinned Mesa d3d12_residency.cpp has no Evict calls to count")
    head = head.replace(call, call + " nxbox_count_eviction(\"aged\", num_pending_evictions);")
    tail = tail.replace(call, call + " nxbox_count_eviction(\"budget\", num_pending_evictions);")
    source = head + tail
    anchor = "static void\nevict_aged_allocations"
    if source.count(anchor) != 1:
        raise RuntimeError("Pinned Mesa d3d12_residency.cpp does not match the no-evict helper anchor")
    path.write_text(source.replace(anchor, helper + anchor, 1))


def patch_resident_create(root: Path) -> None:
    """NXBOX_RESIDENT_CREATE=1 creates every resource resident (no CREATE_NOT_RESIDENT heap flag):
    on both games the device is lost right after a view is created on a resource that was just
    created non-resident, so this tells whether that sequence is what the Xbox rejects."""
    path = root / "src/gallium/drivers/d3d12/d3d12_screen.cpp"
    source = path.read_text()
    anchor = "   nxbox_report_heap_policy(screen->architecture, screen->support_create_not_resident);"
    if source.count(anchor) != 1:
        raise RuntimeError("Pinned Mesa d3d12_screen.cpp does not match the resident-create anchor")
    new = (
        "   {\n"
        "      char resident[4] = {};\n"
        "      if (GetEnvironmentVariableA(\"NXBOX_RESIDENT_CREATE\", resident, sizeof(resident)) == 1 &&\n"
        "          resident[0] == '1')\n"
        "         screen->support_create_not_resident = false;\n"
        "   }\n"
    ) + anchor
    path.write_text(source.replace(anchor, new, 1))


def patch_view_cast(root: Path) -> None:
    """Never create a view whose format is not castable from the resource's format.

    Breath of the Wild's title screen samples an R11G11B10_FLOAT cube map (32x32x6 render target)
    through an R8G8B8A8_UNORM shader resource view; the two formats share no typeless family, so
    the view is invalid and the Xbox removes the device (DXGI_ERROR_INVALID_CALL) the moment it is
    created. Fall back to a view in the resource's own format and count it."""
    driver = root / "src/gallium/drivers/d3d12"
    helper = driver / "nxbox_view_cast.h"
    helper.write_text(
        "#pragma once\n"
        "#include <stdio.h>\n"
        "static inline void\n"
        "nxbox_count_view_cast(const char *kind, unsigned view_format, unsigned resource_format)\n"
        "{\n"
        "   static unsigned srv = 0, rtv = 0, dsv = 0, uav = 0;\n"
        "   if (kind[0] == 's') ++srv; else if (kind[0] == 'r') ++rtv; else if (kind[0] == 'u') ++uav; else ++dsv;\n"
        "   char text[128];\n"
        "   snprintf(text, sizeof(text), \"srv=%u rtv=%u dsv=%u uav=%u last=%s:%u->%u\", srv, rtv, dsv, uav,\n"
        "            kind, view_format, resource_format);\n"
        "   SetEnvironmentVariableA(\"NXBOX_D3D12_VIEW_CAST\", text);\n"
        "}\n"
    )
    context = driver / "d3d12_context.cpp"
    source = context.read_text()
    srv_old = "   desc.Format = d3d12_get_resource_srv_format(state->format, state->target);\n"
    srv_new = srv_old + (
        "   if (d3d12_get_typeless_format(state->format) != d3d12_get_typeless_format(res->overall_format)) {\n"
        "      nxbox_count_view_cast(\"srv\", (unsigned)state->format, (unsigned)res->overall_format);\n"
        "      desc.Format = d3d12_get_resource_srv_format(res->overall_format, state->target);\n"
        "   }\n"
    )
    if source.count(srv_old) != 1:
        raise RuntimeError("Pinned Mesa d3d12_context.cpp does not match the view cast patch")
    include = '#include "d3d12_context.h"\n'
    if source.count(include) < 1:
        raise RuntimeError("Pinned Mesa d3d12_context.cpp has no context include for the view cast patch")
    source = source.replace(include, include + '#include "nxbox_view_cast.h"\n', 1)
    context.write_text(source.replace(srv_old, srv_new, 1))
    surface = driver / "d3d12_surface.cpp"
    source = surface.read_text()
    rt_old = "   DXGI_FORMAT dxgi_format = d3d12_get_resource_rt_format(tpl->format);\n"
    rt_new = rt_old + (
        "   if (d3d12_get_typeless_format(tpl->format) !=\n"
        "       d3d12_get_typeless_format(d3d12_resource(pres)->overall_format)) {\n"
        "      nxbox_count_view_cast(is_depth_or_stencil ? \"dsv\" : \"rtv\", (unsigned)tpl->format,\n"
        "                            (unsigned)d3d12_resource(pres)->overall_format);\n"
        "      dxgi_format = d3d12_get_resource_rt_format(d3d12_resource(pres)->overall_format);\n"
        "   }\n"
    )
    if source.count(rt_old) != 1:
        raise RuntimeError("Pinned Mesa d3d12_surface.cpp does not match the view cast patch")
    include = '#include "d3d12_surface.h"\n'
    if source.count(include) < 1:
        raise RuntimeError("Pinned Mesa d3d12_surface.cpp has no surface include for the view cast patch")
    source = source.replace(include, include + '#include "nxbox_view_cast.h"\n', 1)
    surface.write_text(source.replace(rt_old, rt_new, 1))
    # Image views: a UAV on a resource created without ALLOW_UNORDERED_ACCESS, or in a format outside
    # the resource's family, is invalid too; bind a null UAV in its place (writes are dropped).
    draw = driver / "d3d12_draw.cpp"
    source = draw.read_text()
    call = "CreateUnorderedAccessView(d3d12_res, nullptr, &uav_desc, handle.cpu_handle);"
    lines = source.split("\n")
    hits = [i for i, line in enumerate(lines) if call in line]
    if len(hits) != 2:
        raise RuntimeError("Pinned Mesa d3d12_draw.cpp does not match the UAV view cast patch")
    # Only the image site (the second) has a view format and a resource; the first binds buffers.
    for i in [hits[1]]:
        indent = lines[i][: len(lines[i]) - len(lines[i].lstrip())]
        guard = (
            indent + "if (!(d3d12_res->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||\n"
            + indent + "    d3d12_get_typeless_format(view->format) != d3d12_get_typeless_format(res->overall_format)) {\n"
            + indent + "   nxbox_count_view_cast(\"uav\", (unsigned)view->format, (unsigned)res->overall_format);\n"
            + indent + "   d3d12_res = nullptr;\n"
            + indent + "}"
        )
        lines.insert(i, guard)
    source = "\n".join(lines)
    include = '#include "d3d12_context.h"\n'
    if source.count(include) < 1:
        raise RuntimeError("Pinned Mesa d3d12_draw.cpp has no context include for the view cast patch")
    draw.write_text(source.replace(include, include + '#include "nxbox_view_cast.h"\n', 1))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    patch(parser.parse_args().root)

