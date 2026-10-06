# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise every Mesa patch on an optional pristine checkout, including CRLF."""

import hashlib
import os
import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import test_mesa_render_safety as safety
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(os.environ.get("NXBOX_MESA_SRC", "/tmp/mesa-pin"))


def snapshot(root):
    return {
        path.relative_to(root): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in root.rglob("*")
        if path.is_file()
    }


class MesaFullChainTests(unittest.TestCase):
    def check_residency_batches(self, driver, temporary):
        source = (driver / "d3d12_residency.cpp").read_text()
        loop = source.split("   while (true) {", 1)[1].split(
            "   _mesa_set_destroy(base_bo_set, nullptr);", 1
        )[0]
        runner = safety.MesaRenderSafetyTests()
        runner.root = Path(temporary)
        runner.compile_run(
            r"""
#include <cassert>
#include <cstdint>
#include <vector>
#define SUCCEEDED(hr) ((hr)>=0)
#define FAILED(hr) ((hr)<0)
using HRESULT=int;
constexpr int S_OK=0, D3D12_RESIDENCY_FLAG_NONE=0;
struct ID3D12Pageable { unsigned index=0, resident=0; };
struct d3d12_bo { ID3D12Pageable *res; uint64_t estimated_size=1, last_used_fence=1; };
struct set_entry { d3d12_bo *key; };
struct Set { std::vector<set_entry> entries; };
set_entry *_mesa_set_next_entry(Set *set, set_entry *entry) {
 auto index=entry?entry-set->entries.data()+1:0;
 return index<(long)set->entries.size()?&set->entries[index]:nullptr;
}
struct d3d12_memory_info { uint64_t budget=0, usage=0; };
struct Device {
 bool fail_once=true; unsigned accepted=0, attempts=0; uint64_t usage=0;
 HRESULT EnqueueMakeResident(int,unsigned count,ID3D12Pageable **items,void *,uint64_t) {
  ++attempts; assert(attempts<20);
  if(fail_once) { fail_once=false; return -1; }
  for(unsigned i=0;i<count;++i) { assert(items[i]->resident++==0); ++accepted; }
  usage+=count;
  return 0;
 }
};
template <typename T> T &nxbox_api(T *value,const char *) { return *value; }
struct Screen {
 Device *dev; d3d12_bo residency_list{}; uint64_t residency_fence_value=0, budget=0;
 void *residency_fence=nullptr;
 void get_memory_info(Screen *,d3d12_memory_info *out) { *out={budget,dev->usage}; }
};
bool list_is_empty(d3d12_bo *) { return false; }
#define list_first_entry(list,type,member) (list)
void evict_to_fence_or_budget(Screen *screen,uint64_t,uint64_t,uint64_t) { screen->dev->usage=0; }
void run(unsigned count,unsigned budget) {
 Device dev; Screen screen_storage{&dev}; screen_storage.budget=budget;
 Screen *screen=&screen_storage;
 std::vector<ID3D12Pageable> pages(count); std::vector<d3d12_bo> bos(count);
 Set storage; for(unsigned i=0;i<count;++i) { bos[i].res=&pages[i]; storage.entries.push_back({&bos[i]}); }
 Set *base_bo_set=&storage;
 auto *entry=_mesa_set_next_entry(base_bo_set,nullptr);
 constexpr unsigned residency_batch_size=128;
 unsigned batch_count=0;
 ID3D12Pageable *to_make_resident[residency_batch_size];
 uint64_t batch_memory_size=0, size_to_make_resident=count, pending_fence_value=5;
 d3d12_memory_info mem_info;
 while(true) {
"""
            + loop
            + r"""
 assert(dev.accepted==count && size_to_make_resident==0);
 for(const auto &page:pages) assert(page.resident==1);
}
int main() {
 for(unsigned count:{1u,127u,128u,129u,256u,257u}) {
  run(count,1024); run(count,63);
 }
}
"""
        )

    def check_heap_feature_gate(self, driver, temporary):
        source = (driver / "d3d12_screen.cpp").read_text()
        gate = (
            "   ID3D12Device8 *dev8;"
            + source.split("   ID3D12Device8 *dev8;", 1)[1].split(
                "   screen->dev->QueryInterface(&screen->dev10);", 1
            )[0]
        )
        runner = safety.MesaRenderSafetyTests()
        runner.root = Path(temporary)
        runner.compile_run(
            r"""
#include <cassert>
#include <initializer_list>
#define SUCCEEDED(hr) ((hr)>=0)
struct ID3D12Device8 { unsigned releases=0; void Release() { ++releases; } };
struct D3D12_FEATURE_DATA_D3D12_OPTIONS7 {};
constexpr unsigned D3D12_FEATURE_D3D12_OPTIONS7=7;
struct Device {
 bool has_device8, has_options7; ID3D12Device8 version; unsigned queries=0;
 int QueryInterface(ID3D12Device8 **out) { *out=&version; return has_device8 ? 0 : -1; }
 int CheckFeatureSupport(unsigned feature,void *,unsigned size) {
  assert(feature==7 && size==sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS7));
  ++queries; return has_options7 ? 0 : -1;
 }
};
struct Screen { Device *dev; bool support_create_not_resident=false; };
void check(Screen *screen) {
"""
            + gate
            + r"""
}
int main() {
 for(bool device8:{false,true}) for(bool options7:{false,true}) {
  Device dev{device8,options7,{}}; Screen screen{&dev}; check(&screen);
  assert(screen.support_create_not_resident==(device8 && options7));
  assert(dev.queries==(device8 ? 1u : 0u));
  assert(dev.version.releases==(device8 ? 1u : 0u));
 }
}
"""
        )

    @unittest.skipUnless(SOURCE.is_dir(), "Set NXBOX_MESA_SRC or provide /tmp/mesa-pin")
    def test_pristine_chain_and_repeat_rejection(self):
        environment = os.environ.copy()
        environment.pop("NXBOX_MESA_SKIP", None)
        outputs = []
        for newline in (b"\n", b"\r\n"):
            with (
                self.subTest(newline=newline),
                tempfile.TemporaryDirectory(prefix="nxbox-mesa-chain-") as temporary,
            ):
                root = Path(temporary) / "mesa"
                shutil.copytree(SOURCE, root, ignore=shutil.ignore_patterns(".git"))
                # Simulate checkout line endings for all driver/winsys patch inputs.
                for directory in ("src/gallium/drivers/d3d12", "src/gallium/winsys"):
                    for path in (root / directory).rglob("*"):
                        if path.suffix in (".cpp", ".h"):
                            data = path.read_bytes().replace(b"\r\n", b"\n")
                            path.write_bytes(data.replace(b"\n", newline))
                command = [sys.executable, str(ROOT / "tools/nxbox/patch_mesa_uwp.py"), str(root)]
                result = subprocess.run(
                    command,
                    env=environment,
                    capture_output=True,
                    text=True,
                    timeout=60,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                driver = root / "src/gallium/drivers/d3d12"
                resource = (driver / "d3d12_resource.cpp").read_text()
                self.assertEqual(
                    resource.count(
                        "nxbox_heap_properties(screen->architecture, D3D12_HEAP_TYPE_DEFAULT)"
                    ),
                    2,
                )
                self.assertNotIn("GetCustomHeapProperties", resource)
                bufmgr = (driver / "d3d12_bufmgr.cpp").read_text()
                self.assertIn("nxbox_heap_properties(screen->architecture, heap_type)", bufmgr)
                screen = (driver / "d3d12_screen.cpp").read_text()
                self.assertIn("D3D12_FEATURE_ARCHITECTURE,", screen)
                self.assertIn("D3D12_FEATURE_D3D12_OPTIONS7, &nxbox_options7", screen)
                self.assertIn(
                    "return nxbox_command_stopped();", (driver / "nxbox_sync_batch.h").read_text()
                )
                self.assertIn("desc.Width = ALIGN(desc.Width, util_format_get_blockwidth", resource)
                self.assertIn("nxbox_bc_copy_end", (driver / "nxbox_sync_batch.h").read_text())
                context = (driver / "d3d12_context.cpp").read_text()
                self.assertNotIn("D3D12_RESOURCE_BARRIER_TYPE_ALIASING", context)
                self.assertIn("TEXTURE_BARRIER submit-and-wait", context)
                draw = (driver / "d3d12_draw.cpp").read_text()
                self.assertIn("RTV_FORMAT_MISMATCH", draw)
                self.assertIn("RTV_BIND slot=%u view=%u res=%s", draw)
                query = (driver / "d3d12_query.cpp").read_text()
                self.assertIn("return nxbox_query_ready(", query)
                self.assertNotIn("SetEventOnCompletion(query->fence_value, NULL)", query)
                self.assertTrue((driver / "nxbox_query_wait.h").is_file())
                batch = (driver / "d3d12_batch.cpp").read_text()
                submission = batch.split(".ExecuteCommandLists(count_to_execute, to_execute);", 1)[
                    1
                ]
                sequence = [
                    ".Signal(screen->fence, target)",
                    "nxbox_sync_wait(screen->dev, screen->fence, target)",
                    "const HRESULT after_wait = screen->dev->GetDeviceRemovedReason()",
                    "nxbox_sync_completed(screen->dev)",
                    'nxbox_observe(screen->dev, "execute-return")',
                    "batch->fence = d3d12_create_fence(screen)",
                ]
                positions = [submission.index(item) for item in sequence]
                self.assertEqual(positions, sorted(positions))
                pipeline = (driver / "d3d12_pipeline_state.cpp").read_text()
                self.assertTrue((driver / "nxbox_pso_guard.h").is_file())
                self.assertLess(
                    pipeline.index("nxbox_pso_suspect_shape("),
                    pipeline.index("auto create = [&]()"),
                )
                self.assertEqual(pipeline.count("nxbox_pso_first(pso_desc, FAILED(before)"), 2)
                self.assertEqual(pipeline.count("ret->Release();"), 3)
                lifetime = (driver / "nxbox_lifetime.h").read_text()
                self.assertLess(
                    lifetime.index("report(before, hr, after);"),
                    lifetime.index("nxbox_pso_sample(dev, kind, hr"),
                )
                self.check_heap_feature_gate(driver, temporary)
                self.check_residency_batches(driver, temporary)
                inventory = json.loads((driver / "nxbox_api_inventory.json").read_text())
                self.assertEqual(sum(item["api"] == "ExecuteCommandLists" for item in inventory), 4)
                for method in (
                    "CreateCommittedResource",
                    "CreatePlacedResource",
                    "Evict",
                    "MakeResident",
                    "EnqueueMakeResident",
                    "CreateDescriptorHeap",
                    "CopyDescriptors",
                    "Signal",
                    "Wait",
                    "SetEventOnCompletion",
                    "Reset",
                    "SetDescriptorHeaps",
                    "SetGraphicsRootDescriptorTable",
                    "SetComputeRootDescriptorTable",
                    "ResolveQueryData",
                ):
                    self.assertTrue(any(item["api"] == method for item in inventory), method)
                # The final helper also covers APIs absent in this pin. Check all
                # real matching calls are proxied, including generated headers.
                helper = (driver / "nxbox_api_ring.h").read_text()
                methods = re.findall(r"NXBOX_API_METHOD\((\w+)\)", helper) + [
                    "CopyBufferRegion",
                    "CopyDescriptors",
                ]
                for path in driver.iterdir():
                    if path.suffix not in (".cpp", ".h") or path.name == "nxbox_api_ring.h":
                        continue
                    source = path.read_text()
                    if path.suffix == ".cpp" and '#include "nxbox_api_ring.h"' in source:
                        self.assertIn(
                            '#include "d3d12_common.h"\n#include "nxbox_api_ring.h"', source
                        )
                    source = re.sub(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"', "", source)
                    self.assertIsNone(
                        re.search(r"->(?:" + "|".join(methods) + r")\s*\(", source), path.name
                    )
                self.assertIn('"phase=before", S_OK, before', lifetime)
                dred = (driver / "nxbox_dred.h").read_text().split("void\nnxbox_dred_capture", 1)[1]
                self.assertLess(
                    dred.index("nxbox_api_capture"), dred.index('"NXBOX_D3D12_DEVICE_LOST"')
                )
                outputs.append(
                    {path.name: path.read_text() for path in driver.iterdir() if path.is_file()}
                )
                before = snapshot(root)
                result = subprocess.run(
                    command,
                    env=environment,
                    capture_output=True,
                    text=True,
                    timeout=60,
                    check=False,
                )
                # Like the existing patches, the full chain rejects repeat application.
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(
                    "Pinned Mesa source does not match patch: volatile bool finished = false;",
                    result.stderr,
                )
                self.assertEqual(before, snapshot(root))
        self.assertEqual(outputs[0], outputs[1])


if __name__ == "__main__":
    unittest.main()
