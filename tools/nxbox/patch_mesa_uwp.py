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
        "   // there and is not needed on a console. Always present with vsync.\n"
        "   return S_OK == framebuffer->swapchain->Present(interval < 1 ? 1 : interval, 0);"
    )
    if present_old not in present_source:
        raise RuntimeError("Pinned Mesa source does not match the present patch")
    present_path.write_text(present_source.replace(present_old, present_new))

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
    if "fence" in SKIP:
        print("skipping the null fence patch")
    else:
        patch_fence(root)


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
    query.write_text(query_source.replace(begin_old, begin_new))

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


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    patch(parser.parse_args().root)
