# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute the complete command journal on a host with mock D3D12 objects."""

import ast
from pathlib import Path

import unittest
import test_mesa_render_safety as safety

ROOT = Path(__file__).resolve().parents[2]

MOCK = r"""
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <map>
#include <vector>
#include <thread>
using UINT=unsigned; using UINT64=uint64_t; using UINT8=uint8_t;
using INT=int; using FLOAT=float; using HRESULT=int32_t;
using DXGI_FORMAT=unsigned; using D3D12_CLEAR_FLAGS=unsigned;
[[maybe_unused]] constexpr HRESULT S_OK=0, DXGI_ERROR_DEVICE_REMOVED=-1;
#define FAILED(hr) ((hr)<0)
std::map<std::string,std::string> env;
std::vector<std::string> published;
unsigned GetEnvironmentVariableA(const char *name, char *out, unsigned size) {
 auto it=env.find(name); if(it==env.end()) return 0;
 unsigned n=it->second.size(); if(n<size) memcpy(out,it->second.c_str(),n+1);
 return n;
}
void SetEnvironmentVariableA(const char *name, const char *value) {
 env[name]=value; published.emplace_back(name);
}
void Sleep(unsigned) {}
struct ID3D12Device {
 unsigned checks=0, fail_at=0;
 HRESULT GetDeviceRemovedReason() { return ++checks >= fail_at && fail_at ? -2 : S_OK; }
};
struct ID3D12Fence { UINT64 GetCompletedValue() { return 42; } };
struct ID3D12PipelineState {};
struct ID3D12RootSignature {};
struct ID3D12CommandSignature {};
struct D3D12_RESOURCE_DESC {
 unsigned Dimension=3, Format=28; UINT64 Width=64; UINT Height=64;
 uint16_t DepthOrArraySize=1, MipLevels=1;
 struct { unsigned Count=1, Quality=0; } SampleDesc;
 unsigned Flags=1;
};
struct ID3D12Resource { D3D12_RESOURCE_DESC desc; auto GetDesc() { return desc; } };
[[maybe_unused]] constexpr int D3D12_RESOURCE_BARRIER_TYPE_TRANSITION=0, D3D12_RESOURCE_BARRIER_TYPE_UAV=1;
struct D3D12_RESOURCE_BARRIER {
 int Type=0; unsigned Flags=0;
 struct { ID3D12Resource *pResource; UINT Subresource, StateBefore, StateAfter; } Transition{};
 struct { ID3D12Resource *pResource; } UAV{};
 struct { ID3D12Resource *pResourceBefore, *pResourceAfter; } Aliasing{};
};
[[maybe_unused]] constexpr int D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX=0;
struct D3D12_TEXTURE_COPY_LOCATION {
 ID3D12Resource *pResource=nullptr; int Type=0; unsigned SubresourceIndex=0;
 struct { UINT64 Offset=0;
   struct { unsigned Format=28, Width=64, Height=64, Depth=1, RowPitch=256; } Footprint;
 } PlacedFootprint;
};
struct D3D12_BOX { UINT left,top,front,right,bottom,back; };
struct D3D12_RECT { int left,top,right,bottom; };
struct D3D12_CPU_DESCRIPTOR_HANDLE { uintptr_t ptr; };
struct ID3D12GraphicsCommandList {
 unsigned calls=0;
 void ResourceBarrier(UINT, const D3D12_RESOURCE_BARRIER *) { ++calls; }
 void CopyBufferRegion(ID3D12Resource *, UINT64, ID3D12Resource *, UINT64, UINT64) { ++calls; }
 void CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION *, UINT, UINT, UINT,
                        const D3D12_TEXTURE_COPY_LOCATION *, const D3D12_BOX *) { ++calls; }
 void CopyResource(ID3D12Resource *, ID3D12Resource *) { ++calls; }
 void ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT *, UINT, const D3D12_RECT *) { ++calls; }
 void ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8,
                            UINT, const D3D12_RECT *) { ++calls; }
 void ResolveSubresource(ID3D12Resource *, UINT, ID3D12Resource *, UINT, DXGI_FORMAT) { ++calls; }
 void SetPipelineState(ID3D12PipelineState *) { ++calls; }
 void SetGraphicsRootSignature(ID3D12RootSignature *) { ++calls; }
 void SetComputeRootSignature(ID3D12RootSignature *) { ++calls; }
 void DrawInstanced(UINT,UINT,UINT,UINT) { ++calls; }
 void DrawIndexedInstanced(UINT,UINT,UINT,INT,UINT) { ++calls; }
 void Dispatch(UINT,UINT,UINT) { ++calls; }
 void ExecuteIndirect(ID3D12CommandSignature *, UINT, ID3D12Resource *, UINT64,
                      ID3D12Resource *, UINT64) { ++calls; }
 void ExecuteBundle(ID3D12GraphicsCommandList *) { ++calls; }
};
"""


class MesaJournalTests(unittest.TestCase):
    setUp = safety.MesaRenderSafetyTests.setUp
    compile_run = safety.MesaRenderSafetyTests.compile_run

    def test_complete_journal_snapshot_and_publication(self):
        header = (ROOT / "tools/nxbox/mesa_sync_batch.h").read_text().replace("#pragma once", "")
        self.compile_run(
            MOCK
            + header
            + r"""
std::string journal() {
 std::string result;
 for(unsigned i=0;env.count("NXBOX_D3D12_BATCH_JOURNAL_"+std::to_string(i));++i) result += env["NXBOX_D3D12_BATCH_JOURNAL_"+std::to_string(i)];
 return result;
}
int main() {
 ID3D12Device dev; ID3D12GraphicsCommandList list;
 NxboxBatchJournal *j=nullptr; nxbox_journal_reset(j); assert(j);
 auto c=nxbox_journal_commands(j,&list);
 ID3D12Resource res; ID3D12PipelineState pso; ID3D12RootSignature rs;
 D3D12_RESOURCE_BARRIER b; b.Transition={&res,3,4,8};
 c.ResourceBarrier(1,&b);
 D3D12_TEXTURE_COPY_LOCATION loc; loc.pResource=&res;
 c.CopyTextureRegion(&loc,0,0,0,&loc,nullptr);
 c.CopyBufferRegion(&res,3,&res,7,11); c.CopyResource(&res,&res);
 FLOAT rgba[4]={0,0,0,1};
 c.ClearRenderTargetView({12},rgba,0,nullptr);
 c.ClearDepthStencilView({13},3,1,0,0,nullptr);
 c.ResolveSubresource(&res,1,&res,2,28);
 c.SetPipelineState(&pso); c.SetGraphicsRootSignature(&rs); c.SetComputeRootSignature(&rs);
 strcpy(j->targets,"pso_rtv=28 pso_dsv=45 bound_rtv=28 bound_dsv=45");
 c.DrawInstanced(3,1,0,0); c.DrawIndexedInstanced(6,1,0,-2,0); c.Dispatch(1,2,3);
 c.ExecuteIndirect(nullptr,1,&res,16,nullptr,0); c.ExecuteBundle(&list);
 // Recorded after main, but executes first. A large fixup must not evict main.
 for(unsigned i=0;i<100;++i)
   nxbox_journal_commands(j,&list,"fixup").ResourceBarrier(1,&b);
 assert(list.calls==115);
 nxbox_sync_submit(&dev,j,&list,2,123,42,true);
 // Reset and destroy the context-owned journal before another worker notices loss.
 nxbox_journal_reset(j); c.DrawInstanced(999,1,0,0); delete j;
 std::thread worker([&]{ nxbox_sync_capture(&dev,-2); }); worker.join();
 const auto text=journal();
 assert(text.find("vertices=999")==std::string::npos);
 assert(text.find("before=4 after=8")!=std::string::npos);
 assert(text.find("dim=3/fmt=28/size=64x64x1/mips=1/ms=1:0/flags=1")!=std::string::npos);
 assert(text.find("CopyTextureRegion")!=std::string::npos);
 assert(text.find("CopyBufferRegion")!=std::string::npos);
 assert(text.find("ClearRenderTargetView")!=std::string::npos);
 assert(text.find("DrawIndexedInstanced pso=")!=std::string::npos);
 assert(text.find("pso_rtv=28 pso_dsv=45 bound_rtv=28 bound_dsv=45")!=std::string::npos);
 assert(text.find("ExecuteBundle")!=std::string::npos);
 assert(text.find("fixup")<text.find("main"));
 auto manifest=env["NXBOX_D3D12_FIRST_BAD_BATCH"];
 assert(manifest.find("id=123")!=std::string::npos);
 assert(manifest.find("entries=115 dropped=0")!=std::string::npos);
 assert(manifest.find("completed=0 attribution=in-flight-submit")!=std::string::npos);
 assert(published[published.size()-2]=="NXBOX_D3D12_FIRST_BAD_BATCH");
 auto count=published.size(); nxbox_sync_capture(&dev,-3); assert(published.size()==count);
 nxbox_sync_forget(&dev);
 env.clear(); published.clear(); j=nullptr; nxbox_journal_reset(j);
 // Bound each chunk without limiting the total number of entries or chunks.
 char payload[600]; memset(payload,'x',599); payload[599]=0;
 for(unsigned i=0;i<80;++i) nxbox_journal_add(j,"main","%s",payload);
 nxbox_sync_submit(&dev,j,&list,0,123,42,false); nxbox_sync_completed(&dev);
 nxbox_journal_reset(j);
 for(unsigned i=0;i<80;++i) nxbox_journal_add(j,"main","%s",payload);
 nxbox_sync_submit(&dev,j,&list,0,124,43,false); nxbox_sync_completed(&dev);
 nxbox_sync_capture(&dev,-2);
 manifest=env["NXBOX_D3D12_FIRST_BAD_BATCH"];
 assert(manifest.find("parts=10 entries=80 dropped=0")!=std::string::npos);
 assert(manifest.find("completed=1 attribution=delayed-after-checked-submit")!=std::string::npos);
 for(unsigned i=0;i<10;++i) {
   auto &part=env["NXBOX_D3D12_BATCH_JOURNAL_"+std::to_string(i)];
   assert(!part.empty() && part.size()<4096);
 }
 assert(env.count("NXBOX_D3D12_BATCH_JOURNAL_10")==0);
 assert(env["NXBOX_D3D12_PREVIOUS_GOOD_BATCH"].find("id=123")!=std::string::npos);
 assert(env["NXBOX_D3D12_PREVIOUS_GOOD_BATCH"].find("checked-good-submit")!=std::string::npos);
 assert(env["NXBOX_D3D12_BATCH_JOURNAL_PREVIOUS_9"].find("79 main")!=std::string::npos);
 nxbox_sync_forget(&dev); delete j;
 // Opt-out forwards calls without allocation or logging (enabled is process-cached).
 auto empty=nxbox_journal_commands(nullptr,&list); empty.DrawInstanced(1,1,0,0);
 assert(list.calls==117);
}
"""
        )

    def test_submit_drain_checks_removal_before_marking_complete(self):
        tree = ast.parse((ROOT / "tools/nxbox/patch_mesa_uwp.py").read_text())
        blocks = [
            node.value
            for node in ast.walk(tree)
            if isinstance(node, ast.Constant)
            and isinstance(node.value, str)
            and "/* No observer or fence allocation" in node.value
        ]
        self.assertEqual(len(blocks), 1)
        header = (ROOT / "tools/nxbox/mesa_sync_batch.h").read_text().replace("#pragma once", "")
        self.compile_run(
            MOCK
            + header
            + r"""
#define SUCCEEDED(hr) ((hr)>=0)
struct Queue {
 unsigned signals=0; HRESULT result=0;
 HRESULT Signal(ID3D12Fence *, UINT64 value) { ++signals; assert(value==42); return result; }
};
struct Screen { ID3D12Device *dev; ID3D12Fence *fence; Queue *cmdqueue; UINT64 fence_value=41; };
struct Batch { bool has_errors=false; };
void nxbox_dred_capture(ID3D12Device *, HRESULT hr, const char *site) {
 assert(hr==-2); assert(strcmp(site,"sync-submit-fence")==0);
}
void execute_return(Screen *screen, Batch *batch) {
"""
            + blocks[0]
            + r"""
}
int main(int argc, char **argv) {
 assert(argc==2);
 const bool enabled=argv[1][0]=='1';
 if(!enabled) env["NXBOX_SYNC_BATCH"]="0";
 for(unsigned scenario=0;scenario<4;++scenario) {
   ID3D12Device dev; ID3D12Fence fence; Queue queue; Batch batch;
   Screen screen{&dev,&fence,&queue};
   // Healthy, removal during polling, removal on the explicit post-wait check,
   // and failed Signal on a still-live device.
   if(scenario==1) dev.fail_at=1;
   if(scenario==2) dev.fail_at=3;
   if(scenario==3) queue.result=-3;
   nxbox_sync_submit(&dev,nullptr,nullptr,0,123,42,false);
   execute_return(&screen,&batch);
   assert(queue.signals==unsigned(enabled));
   assert(batch.has_errors==(enabled && scenario!=0));
   if(enabled) {
     const auto &snapshot=nxbox_submissions().at(&dev);
     assert(snapshot.completed==(scenario==0));
     assert(snapshot.captured==(scenario==1 || scenario==2));
     assert(nxbox_sync_stopped().load()==(scenario==3));
     if(scenario==2) assert(dev.checks==3);
     if(snapshot.captured)
       assert(env["NXBOX_D3D12_FIRST_BAD_BATCH"].find("completed=0 attribution=in-flight-submit")!=std::string::npos);
   } else assert(dev.checks==0);
   nxbox_sync_forget(&dev); nxbox_sync_stopped().store(false); env.clear();
 }
}
""",
            args=("1",),
        )
        # Run the same compiled executable in a fresh process for the cached opt-out.
        import subprocess

        subprocess.run([str(self.root / "test"), "0"], check=True, timeout=10)

    def test_dred_publishes_journal_before_exit_notification(self):
        source = (ROOT / "tools/nxbox/mesa_dred.h").read_text()
        capture = source.split("void\nnxbox_dred_capture", 1)[1]
        self.assertLess(
            capture.index("nxbox_sync_capture"), capture.index('"NXBOX_D3D12_DEVICE_LOST"')
        )
        self.assertLess(
            capture.index('"NXBOX_D3D12_DRED", crumbs.text'),
            capture.index('"NXBOX_D3D12_DEVICE_LOST"'),
        )

    def test_frontend_collects_every_chunk_on_immediate_loss(self):
        session = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
        report_header = (
            (ROOT / "src/eden_uwp/diagnostic_report.h").read_text().replace("#pragma once", "")
        )
        report_helpers = session.split("    const auto read_report =", 1)[1].split(
            "    while (!closed.load", 1
        )[0]
        report_helpers = "    const auto read_report =" + report_helpers
        guard = session.split("        char command_failure[2]{};", 1)[1].split(
            "        const int request =", 1
        )[0]
        self.compile_run(
            MOCK
            + r"""
#include <stdexcept>
std::string diagnostic_log;
void Diagnostic(const std::string &line) { diagnostic_log += line + "\n"; }
"""
            + report_header
            + "\nusing namespace EdenXbox;\nvoid check() {\n"
            + report_helpers
            + "char command_failure[2]{};\n"
            + guard
            + r"""
}
int main() {
 check(); assert(diagnostic_log.empty());
 env["NXBOX_D3D12_DEVICE_LOST"]="1";
 env["NXBOX_D3D12_FIRST_BAD_BATCH"]="id=123 parts=10";
 env["NXBOX_D3D12_PREVIOUS_GOOD_BATCH"]="id=122 parts=1";
 env["NXBOX_D3D12_BATCH_JOURNAL_PREVIOUS_0"]="0 main good\n1 main good\r\n";
 env["NXBOX_D3D12_BATCH"]="removed=0x887a0001";
 for(unsigned i=0;i<10;++i) env["NXBOX_D3D12_BATCH_JOURNAL_"+std::to_string(i)]="0 main first\n1 main second\r\n2 main last";
 try { check(); assert(false); } catch(const std::runtime_error &) {}
 assert(diagnostic_log.find("D3D12_FIRST_BAD_BATCH id=123 parts=10")!=std::string::npos);
 assert(diagnostic_log.find("D3D12_BATCH removed=0x887a0001")!=std::string::npos);
 for(unsigned i=0;i<10;++i) {
   const auto prefix="D3D12_BATCH_JOURNAL_"+std::to_string(i)+" ";
   assert(diagnostic_log.find(prefix+"0 main first\n"+prefix+"1 main second\n"+prefix+"2 main last\n")!=std::string::npos);
 }
 assert(diagnostic_log.find("D3D12_BATCH_JOURNAL_PREVIOUS_0 1 main good\n")!=std::string::npos);
}
"""
        )
