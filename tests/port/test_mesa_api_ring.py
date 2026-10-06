# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute the device API ring and frontend drain with mock COM objects."""

from pathlib import Path
import subprocess
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
#include <atomic>
using HRESULT = int32_t; using UINT64 = uint64_t; using UINT = unsigned; using D3D12_DESCRIPTOR_HEAP_TYPE=unsigned;
constexpr HRESULT E_INVALIDARG=-22;
constexpr HRESULT S_OK=0;
#define SUCCEEDED(hr) ((hr)>=0)
#define IID_PPV_ARGS(pp) 0, reinterpret_cast<void **>(pp)
std::map<std::string,std::string> env;
std::vector<std::string> published;
unsigned GetEnvironmentVariableA(const char *name,char *out,unsigned size) {
 auto it=env.find(name); if(it==env.end()) return 0;
 unsigned n=it->second.size(); if(n<size) memcpy(out,it->second.c_str(),n+1); return n;
}
void SetEnvironmentVariableA(const char *name,const char *value) {
 env[name]=value; published.emplace_back(name);
}
uint64_t GetTickCount64() { static std::atomic<uint64_t> tick{0}; return ++tick; }
unsigned long GetCurrentThreadId() { return std::hash<std::thread::id>{}(std::this_thread::get_id()); }
void Sleep(unsigned) { std::this_thread::yield(); }
constexpr unsigned D3D12_HEAP_TYPE_CUSTOM=4;
struct D3D12_RESOURCE_DESC {
 unsigned Alignment=0, Layout=0;
 uint64_t Width=4096; unsigned Height=1, DepthOrArraySize=1, Format=28, Flags=4, Dimension=2, MipLevels=1;
 struct { unsigned Count=1; } SampleDesc;
};
struct D3D12_RESOURCE_DESC1 : D3D12_RESOURCE_DESC {};
struct D3D12_HEAP_PROPERTIES { unsigned Type=2, CPUPageProperty=0, MemoryPoolPreference=0; };
struct D3D12_HEAP_DESC { uint64_t SizeInBytes=8192; unsigned Flags=0; D3D12_HEAP_PROPERTIES Properties; };
struct D3D12_DESCRIPTOR_HEAP_DESC { unsigned NumDescriptors=512, Type=1, Flags=1; };
struct D3D12_GPU_DESCRIPTOR_HANDLE { uint64_t ptr; };
struct D3D12_CPU_DESCRIPTOR_HANDLE { uint64_t ptr; };
struct ID3D12Device {
 struct AllocationInfo { uint64_t SizeInBytes; };
 AllocationInfo GetResourceAllocationInfo(unsigned,unsigned,const D3D12_RESOURCE_DESC *) { return {65536}; }
 HRESULT removed=0; unsigned checks=0, calls=0, refs=1;
 HRESULT GetDeviceRemovedReason() { ++checks; return removed; }
 void Release() { --refs; }
 HRESULT CreateCommittedResource(const D3D12_HEAP_PROPERTIES *,unsigned,const D3D12_RESOURCE_DESC *,unsigned,const void *,int,void **) { ++calls; return -9; }
 HRESULT CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC *,int,void **) { ++calls; return 0; }
};
struct ID3D12Resource { D3D12_RESOURCE_DESC desc; auto GetDesc() { return desc; } };
struct ID3D12DeviceChild {};
struct ID3D12GraphicsCommandList : ID3D12DeviceChild {};
struct ID3D12CommandAllocator : ID3D12DeviceChild {};
struct ID3D12CommandQueue : ID3D12DeviceChild {};
struct Queue : ID3D12DeviceChild {
 explicit Queue(ID3D12Device *d) : dev(d) {}
 ID3D12Device *dev; unsigned calls=0;
 HRESULT GetDevice(int,void **out) { *out=dev; ++dev->refs; return 0; }
 void ExecuteCommandLists(unsigned,void *const *) { ++calls; dev->removed=-2; }
 HRESULT Signal(void *,uint64_t) { ++calls; return -7; }
};
unsigned dred_calls=0;
void nxbox_dred_capture(ID3D12Device *, HRESULT hr, const char *, const char *) {
 if(hr) { ++dred_calls; assert(env.count("NXBOX_D3D12_API_RING")); }
}
"""


class MesaApiRingTests(unittest.TestCase):
    setUp = safety.MesaRenderSafetyTests.setUp
    compile_run = safety.MesaRenderSafetyTests.compile_run

    def code(self):
        return MOCK + (ROOT / "tools/nxbox/mesa_api_ring.h").read_text().replace("#pragma once", "")

    def test_wrap_first_failure_and_complete_frontend_drain(self):
        report = (ROOT / "src/eden_uwp/diagnostic_report.h").read_text().replace("#pragma once", "")
        self.compile_run(
            self.code()
            + report
            + r"""
int main() {
 ID3D12Device dev;
 for(unsigned i=0;i<300;++i) nxbox_api_record(&dev,"Signal","queue:42","value=9",0,0);
 nxbox_api_record(&dev,"CreateHeap","heap:17","heap_size=65536",-3,-2);
 assert(published.back()=="NXBOX_D3D12_API_RING");
 assert(env["NXBOX_D3D12_API_RING"].find("parts=256 first_seq=300 first_call=CreateHeap") == 0);
 assert(env["NXBOX_D3D12_API_RING_0"].find("seq=45 ")==0);
 assert(env["NXBOX_D3D12_API_RING_255"].find("seq=300 ")==0);
 assert(env["NXBOX_D3D12_API_FIRST"].find("hr=0xfffffffd removed=0xfffffffe heap_size=65536")!=std::string::npos);
 const auto count=published.size();
 nxbox_api_record(&dev,"later","other:1","",0,-4); nxbox_api_capture(&dev,-4,"observer");
 assert(published.size()==count);
 std::string log;
 EdenXbox::CollectDeviceApiRing([](const char *name){return env[name];},[&](const char *name,const std::string &text){
  EdenXbox::DiagnosticReportLines(name,text,[&](const std::string &line){ log+=line+"\n"; });
 });
 for(unsigned i=45;i<=300;++i) assert(log.find("D3D12_API_RING seq="+std::to_string(i)+" ")!=std::string::npos);
 assert(log.find("D3D12_API_FIRST seq=300 ")!=std::string::npos);
}
"""
        )

    def test_wrappers_preserve_hresult_void_args_and_opt_out(self):
        self.compile_run(
            self.code()
            + r"""
int main(int argc,char **argv) {
 assert(argc==2); const bool enabled=argv[1][0]=='1';
 if(!enabled) env["NXBOX_SYNC_BATCH"]="0";
 ID3D12Device dev; Queue queue{&dev};
 D3D12_HEAP_PROPERTIES heap; D3D12_RESOURCE_DESC desc; void *out=nullptr;
 auto hr=nxbox_api(&dev,"alloc:1").CreateCommittedResource(&heap,0,&desc,4,nullptr,IID_PPV_ARGS(&out));
 assert(hr==-9 && dev.calls==1);
 D3D12_DESCRIPTOR_HEAP_DESC descriptors;
 assert(nxbox_api(&dev,"desc:2").CreateDescriptorHeap(&descriptors,IID_PPV_ARGS(&out))==0);
 assert(nxbox_api(&queue,"signal:3").Signal(nullptr,42)==-7);
 nxbox_api(&queue,"submit:4").ExecuteCommandLists(1,nullptr);
 assert(dev.refs==1 && queue.calls==2 && dev.checks==(enabled?4u:0u));
 if(!enabled) { assert(published.empty() && dred_calls==0); return 0; }
 assert(env["NXBOX_D3D12_API_RING_0"].find("heap_type=2")!=std::string::npos);
 assert(env["NXBOX_D3D12_API_RING_0"].find("size=4096x1x1 format=28 flags=4")!=std::string::npos);
 assert(env["NXBOX_D3D12_API_RING_1"].find("descriptors=512 type=1 flags=1")!=std::string::npos);
 assert(env["NXBOX_D3D12_API_FIRST"].find("call=ExecuteCommandLists")!=std::string::npos);
}
""",
            args=("1",),
        )
        subprocess.run([str(self.root / "test"), "0"], check=True, timeout=10)

    def test_workers_share_ring_and_external_observer_preserves_first(self):
        self.compile_run(
            self.code()
            + r"""
int main() {
 ID3D12Device dev;
 std::vector<std::thread> workers;
 for(unsigned t=0;t<4;++t) workers.emplace_back([&] {
  for(unsigned i=0;i<32;++i) nxbox_api_record(&dev,"CopyDescriptors","worker:3","counts=1",0,0);
 });
 for(auto &t:workers) t.join();
 assert(nxbox_api_ring().next==128);
 nxbox_api_capture(&dev,-2,"query-wait");
 assert(env["NXBOX_D3D12_API_RING"].find("parts=129 ")==0);
 assert(env["NXBOX_D3D12_API_RING"].find("gaps=0")!=std::string::npos);
 for(unsigned i=0;i<128;++i) assert(env["NXBOX_D3D12_API_RING_"+std::to_string(i)].find("call=CopyDescriptors")!=std::string::npos);
}
"""
        )

    def test_buffer_copy_bounds_reject_overflow_and_forward_valid_edges(self):
        self.compile_run(
            self.code()
            + r"""
struct List : ID3D12DeviceChild {
 explicit List(ID3D12Device *d) : dev(d) {}
 ID3D12Device *dev; unsigned copies=0;
 HRESULT GetDevice(int,void **out) { *out=dev; ++dev->refs; return 0; }
 void CopyBufferRegion(ID3D12Resource *, UINT64, ID3D12Resource *, UINT64, UINT64) { ++copies; }
 HRESULT Reset() { return 0; }
};
int main() {
 ID3D12Device dev; List list{&dev}; ID3D12Resource src,dst;
 auto api=nxbox_api(&list,"copy:10");
 api.CopyBufferRegion(&dst,4000,&src,0,96); assert(list.copies==1);
 api.CopyBufferRegion(&dst,4000,&src,0,97); assert(list.copies==1);
 api.CopyBufferRegion(&dst,0,&src,4097,0); assert(list.copies==1);
 api.CopyBufferRegion(&dst,UINT64_MAX,&src,0,2); assert(list.copies==1);
 api.CopyBufferRegion(nullptr,0,&src,0,0); assert(list.copies==1);
 api.CopyBufferRegion(&dst,4096,&src,4096,0); assert(list.copies==2);
 assert(api.Reset()==0);
 assert(env["NXBOX_D3D12_SYNC_ERROR"].find("CopyBufferRegion rejected") == 0);
 assert(dev.refs==1);
}
"""
        )

    def test_failed_close_is_terminal_even_without_diagnostics(self):
        self.compile_run(
            self.code()
            + r"""
struct List : ID3D12GraphicsCommandList {
 ID3D12Device *dev; unsigned calls=0;
 HRESULT GetDevice(int,void **out) { *out=dev; ++dev->refs; return 0; }
 HRESULT Close() { ++calls; return E_INVALIDARG; }
 HRESULT Reset() { ++calls; return E_INVALIDARG; }
 void DrawInstanced(unsigned,unsigned,unsigned,unsigned) { ++calls; }
};
int main(int argc,char **argv) {
 assert(argc>=2); bool enabled=argv[1][0]=='1';
 if(!enabled) env["NXBOX_SYNC_BATCH"]="0";
 ID3D12Device dev; List list; list.dev=&dev;
 const bool reset=argc>2;
 auto api=nxbox_api(&list,"command:1");
 assert((reset ? api.Reset() : api.Close())==E_INVALIDARG);
 assert(nxbox_command_stopped());
 assert(env["NXBOX_D3D12_COMMAND_FAILURE"]=="1");
 nxbox_api(&list,"draw:2").DrawInstanced(3,1,0,0);
 assert(nxbox_api(&list,"reset:3").Reset()==E_INVALIDARG);
 assert(list.calls==1 && dev.refs==1);
 if(enabled) {
  assert(env["NXBOX_D3D12_API_FIRST"].find(reset ? "call=Reset" : "call=Close")!=std::string::npos);
  assert(env["NXBOX_D3D12_API_FIRST"].find("removed=0x00000000")!=std::string::npos);
  assert(env["NXBOX_D3D12_API_RING"].find("attribution=command-api-failure")!=std::string::npos);
 } else assert(!env.count("NXBOX_D3D12_API_RING"));
}
""",
            args=("1",),
        )
        subprocess.run([str(self.root / "test"), "0"], check=True, timeout=10)
        subprocess.run([str(self.root / "test"), "1", "reset"], check=True, timeout=10)
        subprocess.run([str(self.root / "test"), "0", "reset"], check=True, timeout=10)

    def test_custom_creation_totals_ignore_failed_and_default_allocations(self):
        mock = self.code().replace("return -9;", "return removed;")
        self.compile_run(
            mock
            + r"""
int main() {
 ID3D12Device dev; D3D12_HEAP_PROPERTIES heap; D3D12_RESOURCE_DESC desc; void *out=nullptr;
 auto create=[&] { return nxbox_api(&dev,"alloc:1").CreateCommittedResource(&heap,2048,&desc,0,nullptr,IID_PPV_ARGS(&out)); };
 create(); assert(nxbox_api_ring().custom_created==0);
 heap.Type=D3D12_HEAP_TYPE_CUSTOM; heap.CPUPageProperty=2; heap.MemoryPoolPreference=1;
 for(unsigned i=0;i<300;++i) assert(create()==0);
 assert(nxbox_api_ring().custom_created==300 && nxbox_api_ring().custom_bytes==300*65536);
 dev.removed=-9; assert(create()==-9);
 assert(nxbox_api_ring().custom_created==300);
 const auto &line=env["NXBOX_D3D12_API_FIRST"];
 assert(line.find("custom_created=300 custom_bytes=19660800 custom_unknown=0")!=std::string::npos);
 assert(line.find("heap_type=4 cpu_page=2 pool=1")!=std::string::npos);
 assert(line.find("heap_flags_hex=0x800 not_resident=1 not_zeroed=0")!=std::string::npos);
 assert(env["NXBOX_D3D12_API_RING"].find("parts=256")!=std::string::npos);
}
"""
        )

    def test_terminal_loss_drains_all_entries_before_throw(self):
        session = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
        helpers = (
            "    const auto read_report ="
            + session.split("    const auto read_report =", 1)[1].split(
                "    while (!closed.load", 1
            )[0]
        )
        guard = (
            "        char command_failure[2]{};"
            + session.split("        char command_failure[2]{};", 1)[1].split(
                "        const int request =", 1
            )[0]
        )
        report = (ROOT / "src/eden_uwp/diagnostic_report.h").read_text().replace("#pragma once", "")
        self.compile_run(
            self.code()
            + report
            + r"""
#include <stdexcept>
std::string log_text;
void Diagnostic(const std::string &line) { log_text += line + "\n"; }
using namespace EdenXbox;
void poll() {
"""
            + helpers
            + guard
            + r"""
}
int main() {
 ID3D12Device dev;
 for(unsigned i=0;i<256;++i) nxbox_api_record(&dev,"Signal","queue:1","value=42",0,i==255?-2:0);
 env["NXBOX_D3D12_DEVICE_LOST"]="1";
 try { poll(); assert(false); } catch(const std::runtime_error &) {}
 for(unsigned i=0;i<256;++i) assert(log_text.find("D3D12_API_RING seq="+std::to_string(i)+" ")!=std::string::npos);
 assert(log_text.find("D3D12_API_FIRST seq=255 ")!=std::string::npos);
 env.erase("NXBOX_D3D12_DEVICE_LOST"); env["NXBOX_D3D12_COMMAND_FAILURE"]="1";
 try { poll(); assert(false); } catch(const std::runtime_error &e) {
  assert(std::string(e.what()).find("command recording failed")!=std::string::npos);
 }
}
"""
        )
        shutdown = session.split("            graphics.reset();", 1)[1].split(
            "            done.store", 1
        )[0]
        self.assertIn("FlushDeviceApiRing();", shutdown)
        cleanup = session.split("        system.ShutdownMainProcess();", 1)[0]
        self.assertIn("FlushDeviceApiRing();", cleanup.rsplit("    SCOPE_EXIT {", 1)[1])

    def test_busy_slot_is_reported_without_blocking_or_losing_first(self):
        self.compile_run(
            self.code()
            + r"""
int main() {
 ID3D12Device dev;
 nxbox_api_ring().slots[0].stamp.store(1); // Simulate a preempted producer.
 nxbox_api_record(&dev,"Wait","queue:7","target=42",-3,-2);
 assert(env["NXBOX_D3D12_API_RING"].find("gaps=1")!=std::string::npos);
 assert(env["NXBOX_D3D12_API_RING_0"].find("unavailable=")!=std::string::npos);
 assert(env["NXBOX_D3D12_API_FIRST"].find("call=Wait")!=std::string::npos);
}
"""
        )


if __name__ == "__main__":
    unittest.main()
