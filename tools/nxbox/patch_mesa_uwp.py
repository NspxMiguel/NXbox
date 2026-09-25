#!/usr/bin/env python3
"""Apply the NXbox worker-context fixes to the pinned Mesa UWP source."""

from pathlib import Path
import argparse


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


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    patch(parser.parse_args().root)
