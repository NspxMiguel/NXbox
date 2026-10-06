# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise heap policy across UMA, coherent UMA, and discrete architectures."""

from pathlib import Path
import unittest

import test_mesa_render_safety as safety

ROOT = Path(__file__).resolve().parents[2]


class MesaHeapPolicyTests(unittest.TestCase):
    setUp = safety.MesaRenderSafetyTests.setUp
    compile_run = safety.MesaRenderSafetyTests.compile_run

    def test_gpu_and_mapped_heaps(self):
        helper = (ROOT / "tools/nxbox/mesa_heap_policy.h").read_text().replace("#pragma once", "")
        self.compile_run(
            r"""
#include <cassert>
#include <string>
#include <initializer_list>
enum D3D12_HEAP_TYPE { D3D12_HEAP_TYPE_DEFAULT=1, D3D12_HEAP_TYPE_UPLOAD=2,
 D3D12_HEAP_TYPE_READBACK=3, D3D12_HEAP_TYPE_CUSTOM=4 };
enum { D3D12_MEMORY_POOL_L0=1, D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE=2,
 D3D12_CPU_PAGE_PROPERTY_WRITE_BACK=3 };
struct D3D12_FEATURE_DATA_ARCHITECTURE { unsigned NodeIndex=0; bool UMA=false, CacheCoherentUMA=false; };
struct D3D12_HEAP_PROPERTIES { D3D12_HEAP_TYPE Type; unsigned CPUPageProperty,
 MemoryPoolPreference, CreationNodeMask, VisibleNodeMask; };
std::string report;
void SetEnvironmentVariableA(const char *, const char *text) { report=text; }
"""
            + helper
            + r"""
int main() {
 for(bool uma:{false,true}) for(bool coherent:{false,true}) {
  D3D12_FEATURE_DATA_ARCHITECTURE arch{0,uma,coherent};
  auto gpu=nxbox_heap_properties(arch,D3D12_HEAP_TYPE_DEFAULT);
  assert(gpu.Type==D3D12_HEAP_TYPE_DEFAULT && gpu.CPUPageProperty==0 && gpu.MemoryPoolPreference==0);
  assert(gpu.CreationNodeMask==1 && gpu.VisibleNodeMask==1);
  auto upload=nxbox_heap_properties(arch,D3D12_HEAP_TYPE_UPLOAD);
  assert(upload.Type==D3D12_HEAP_TYPE_CUSTOM && upload.MemoryPoolPreference==D3D12_MEMORY_POOL_L0);
  assert(upload.CPUPageProperty==(uma && coherent ? D3D12_CPU_PAGE_PROPERTY_WRITE_BACK : D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE));
  auto readback=nxbox_heap_properties(arch,D3D12_HEAP_TYPE_READBACK);
  assert(readback.Type==D3D12_HEAP_TYPE_CUSTOM && readback.MemoryPoolPreference==D3D12_MEMORY_POOL_L0);
  assert(readback.CPUPageProperty==D3D12_CPU_PAGE_PROPERTY_WRITE_BACK);
  nxbox_report_heap_policy(arch,false);
  assert(report.find("create_not_resident=0 create_not_zeroed=0")!=std::string::npos);
 }
}
"""
        )


if __name__ == "__main__":
    unittest.main()
