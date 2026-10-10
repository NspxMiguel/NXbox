# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise list ownership, wrapping, snapshots, publication and terminal draining."""

from pathlib import Path
import unittest
import os
import subprocess

import test_mesa_render_safety as safety

ROOT = Path(__file__).resolve().parents[2]


class MesaListRingTests(unittest.TestCase):
    setUp = safety.MesaRenderSafetyTests.setUp
    compile_run = safety.MesaRenderSafetyTests.compile_run

    def code(self):
        mock = (ROOT / "tests/port/fixtures/list_ring/mock.h").read_text()
        api = (ROOT / "tools/nxbox/mesa_api_ring.h").read_text().replace("#pragma once", "")
        ring = (
            (ROOT / "tools/nxbox/mesa_list_ring.h")
            .read_text()
            .replace("#pragma once", "")
            .replace("#include <directx/d3d12sdklayers.h>", "")
        )
        frontend = (
            (ROOT / "src/eden_uwp/diagnostic_report.h").read_text().replace("#pragma once", "")
        )
        return mock + api.replace('#include "nxbox_list_ring.h"', ring) + frontend

    def test_real_sdk_syntax(self):
        candidates = list(
            (Path.home() / ".local/share/nxbox/artifacts").glob(
                "mesa-*/nxbox-mesa-uwp/mesa-install/include"
            )
        )
        headers = (
            Path(os.environ["NXBOX_DIRECTX_HEADERS"])
            if os.environ.get("NXBOX_DIRECTX_HEADERS")
            else (sorted(candidates)[-1] if candidates else None)
        )
        if not headers or not (headers / "directx/d3d12.h").is_file():
            self.skipTest("Set NXBOX_DIRECTX_HEADERS to DirectX-Headers include directory")
        for name in ("api_ring", "list_ring", "sync_batch"):
            (self.root / f"nxbox_{name}.h").write_text(
                (ROOT / f"tools/nxbox/mesa_{name}.h").read_text()
            )
        result = subprocess.run(
            [
                "clang++",
                "-std=c++17",
                "-fms-extensions",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wno-ignored-attributes",
                "-Wno-undefined-inline",
                "-fsyntax-only",
                "-I",
                str(self.root),
                "-isystem",
                str(headers),
                "-isystem",
                str(headers / "wsl/stubs"),
                str(ROOT / "tests/port/fixtures/list_ring/syntax.cpp"),
            ],
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_debug_default_off_and_unavailable_interface(self):
        code = (
            self.code()
            + r"""
int main(int argc,char **argv) {
 assert(argc==2);
 if(argv[1][0]=='1') env["NXBOX_D3D12_DEBUG"]="1";
 ID3D12Device dev; ID3D12InfoQueue queue;
 nxbox_infoqueue_setup(&dev);
 if(argv[1][0]=='1') assert(env["NXBOX_D3D12_DEBUG_UNAVAILABLE"].find("hr=0x")!=std::string::npos);
 else assert(!env.count("NXBOX_D3D12_DEBUG_UNAVAILABLE"));
 dev.queue=&queue; nxbox_infoqueue_setup(&dev);
 assert(queue.setups==(argv[1][0]=='1'?3u:0u));
}
"""
        )
        for enabled in ("0", "1"):
            self.compile_run(code, (enabled,))

    def test_creation_reset_wrap_failure_and_drain(self):
        self.compile_run(
            self.code()
            + r"""
int main() {
 ID3D12Device dev; ID3D12GraphicsCommandList *a = nullptr, *b = nullptr;
 nxbox_api(&dev,"create-a:7").CreateCommandList(0,0,nullptr,nullptr,IID_PPV_ARGS(&a));
 nxbox_api(&dev,"create-b:8").CreateCommandList1(0,0,0,IID_PPV_ARGS(&b));
 assert(a->data && b->data && a->data != b->data);
 for(unsigned i=0;i<160;++i) nxbox_api(a,"draw:1").DrawInstanced(i,1,0,0);
 nxbox_api(b,"other:2").DrawInstanced(999,1,0,0);
 {
  NxboxListRef j(a); assert(j.p->next==160); assert(j.p->creation.find("create-a:7")!=std::string::npos);
 }
 a->close_hr=E_INVALIDARG;
 assert(nxbox_api(a,"close:3").Close()==E_INVALIDARG);
 assert(nxbox_command_stopped());
 assert(published.back()=="NXBOX_D3D12_COMMAND_FAILURE");
 assert(env["NXBOX_D3D12_LIST_CAPTURE_0"].find("ring_parts=131")!=std::string::npos);
 assert(env["NXBOX_D3D12_LIST_0_RING_2"].find("recorded=161 retained=128 dropped=33 full=0")!=std::string::npos);
 std::string log;
 EdenXbox::CollectDeviceApiRing([](const char *key){return env[key];},
  [&](const char *key,const std::string &v){EdenXbox::DiagnosticReportLines(key,v,[&](const std::string &line){log+=line+'\n';});});
 assert(log.find("D3D12_LIST_RING")!=std::string::npos);
 assert(log.find("vertices=32 ")==std::string::npos);
 for(unsigned i=33;i<160;++i) assert(log.find("vertices="+std::to_string(i)+" ")!=std::string::npos);
 assert(log.find("vertices=999")==std::string::npos);
 assert(log.find("call=Close")!=std::string::npos);
 assert(log.find("custom_created=0")!=std::string::npos);
 assert(log.find("D3D12_INFOQUEUE capture=0")!=std::string::npos);
 nxbox_api(b,"blocked").DrawInstanced(123,1,0,0); assert(b->draws==1);
 assert(nxbox_api(b,"blocked").Reset(nullptr,nullptr)==E_INVALIDARG && b->resets==0);
 delete a; delete b;
}
"""
        )

    def test_reset_success_clears_only_its_list_failure_preserves_history(self):
        self.compile_run(
            self.code()
            + r"""
int main() {
env["NXBOX_SYNC_BATCH"]="0";
 env["NXBOX_D3D12_LIST_FULL"]="1";
 ID3D12Device dev; ID3D12GraphicsCommandList *a=nullptr,*b=nullptr;
 nxbox_api(&dev,"create:1").CreateCommandList(0,0,nullptr,nullptr,IID_PPV_ARGS(&a));
 nxbox_api(&dev,"create:2").CreateCommandList(0,0,nullptr,nullptr,IID_PPV_ARGS(&b));
 nxbox_api(a,"old").DrawInstanced(1,1,0,0);
 nxbox_api(b,"other").DrawInstanced(9,1,0,0);
 assert(nxbox_api(a,"reset").Reset(nullptr,nullptr)==S_OK);
 { NxboxListRef ja(a),jb(b); assert(ja.p->next==1 && ja.p->generation==1 && jb.p->next==1 && jb.p->generation==0);
   assert(ja.p->full_entries.size()==1 && jb.p->full_entries.size()==1); }
 nxbox_api(a,"new").DrawInstanced(2,1,0,0);
 a->reset_hr=E_INVALIDARG;
 assert(nxbox_api(a,"bad-reset").Reset(nullptr,nullptr)==E_INVALIDARG);
 std::string log;
 EdenXbox::CollectCommandListReports([](const char *key){return env[key];},[&](const char *,const std::string &v){log+=v;});
 assert(log.find("site=old")==std::string::npos && log.find("site=new")!=std::string::npos);
 assert(log.find("call=Reset site=bad-reset")!=std::string::npos);
 assert(!env.count("NXBOX_D3D12_API_RING"));
 delete a; delete b;
}
"""
        )

    def test_full_generation_keeps_early_barrier_and_copy_when_ring_wraps(self):
        self.compile_run(
            self.code()
            + r"""
int main() {
 env["NXBOX_D3D12_LIST_FULL"]="1";
 env["NXBOX_SYNC_BATCH"]="0";
 ID3D12Device dev; ID3D12GraphicsCommandList *a=nullptr,*b=nullptr;
 nxbox_api(&dev,"create-a").CreateCommandList(0,0,nullptr,nullptr,IID_PPV_ARGS(&a));
 nxbox_api(&dev,"create-b").CreateCommandList(0,0,nullptr,nullptr,IID_PPV_ARGS(&b));
 ID3D12Resource resource;
 D3D12_RESOURCE_BARRIER barrier; barrier.Transition={&resource,7,0x800,0x400};
 nxbox_api(a,"early-barrier").ResourceBarrier(1,&barrier);
 D3D12_TEXTURE_COPY_LOCATION loc; loc.pResource=&resource;
 D3D12_BOX box{0,0,0,4,4,1};
 nxbox_api(a,"early-copy").CopyTextureRegion(&loc,1,2,3,&loc,&box);
 for(unsigned i=0;i<1511;++i) nxbox_api(a,"draw").DrawInstanced(i,1,0,0);
 nxbox_api(b,"other-list").DrawInstanced(9999,1,0,0);
 { NxboxListRef j(a); assert(j.p->full_entries.size()==1513); }
 a->close_hr=E_INVALIDARG;
 nxbox_api(a,"close").Close();
 std::string log;
 EdenXbox::CollectCommandListReports([](const char *key){return env[key];},
   [&](const char *,const std::string &value){log+=value+'\n';});
 assert(log.find("recorded=1514 retained=1514 dropped=0 full=1")!=std::string::npos);
 assert(log.find("call=ResourceBarrier site=early-barrier")!=std::string::npos);
 assert(log.find("sub=7 before=0x800 after=0x400")!=std::string::npos);
 assert(log.find("call=CopyTextureRegion site=early-copy")!=std::string::npos);
 assert(log.find("box=0,0,0:4,4,1")!=std::string::npos);
 assert(log.find("format=28")!=std::string::npos);
 assert(log.find("seq=0 ")<log.find("seq=1513 "));
 assert(log.find("other-list")==std::string::npos);
 assert(env["NXBOX_D3D12_LIST_CAPTURE_0"].find("ring_parts=1517")!=std::string::npos);
 delete a; delete b;
}
"""
        )

    def test_all_messages_long_arguments_first_five_immutable(self):
        self.compile_run(
            self.code()
            + r"""
int main() {
 env["NXBOX_D3D12_DEBUG"]="1"; env["NXBOX_D3D12_DUMP_EVERY_CLOSE"]="1";
 ID3D12Device dev; ID3D12InfoQueue queue; dev.queue=&queue;
 queue.messages={"first",std::string(10000,'x')+"last-byte"};
 nxbox_infoqueue_setup(&dev); assert(queue.setups==3);
 ID3D12GraphicsCommandList *a=nullptr;
 nxbox_api(&dev,"create").CreateCommandList(0,0,nullptr,nullptr,IID_PPV_ARGS(&a));
 ID3D12Resource resource; D3D12_RESOURCE_BARRIER barriers[256];
 for(unsigned i=0;i<256;++i) { barriers[i].Transition.pResource=&resource; barriers[i].Transition.Subresource=i; barriers[i].Transition.StateAfter=4; }
 nxbox_api(a,"barriers").ResourceBarrier(256,barriers);
 a->close_hr=E_INVALIDARG;
 nxbox_api(a,"close").Close();
 const auto first=env["NXBOX_D3D12_LIST_0_RING_2"];
 for(unsigned i=0;i<6;++i) nxbox_api(a,"again").Close();
 assert(env["NXBOX_D3D12_LIST_CAPTURES"]=="5" && !env.count("NXBOX_D3D12_LIST_CAPTURE_5"));
 assert(env["NXBOX_D3D12_LIST_0_RING_2"]==first);
 std::string log;
 EdenXbox::CollectCommandListReports([](const char *key){auto it=env.find(key);return it==env.end()?std::string{}:it->second;},[&](const char *,const std::string &v){log+=v;});
 assert(log.find("sub=255")!=std::string::npos);
 assert(log.find("id=0 severity=2 category=3 description=first")!=std::string::npos);
 assert(log.find("last-byte")!=std::string::npos);
 assert(log.find("capture=4")!=std::string::npos);
 delete a;
}
"""
        )

    def test_first_failed_close_keeps_error_and_corruption_with_list_ring(self):
        self.compile_run(
            self.code().replace(
                "unsigned(index), 2, 3,", "873 + unsigned(index), unsigned(index), 3,"
            )
            + r"""
int main() {
 env["NXBOX_D3D12_DEBUG"]="1";
 ID3D12Device dev; ID3D12InfoQueue queue; dev.queue=&queue;
 queue.messages={"corruption", "unaligned BC source box", "warning retained"};
 ID3D12GraphicsCommandList *list=nullptr;
 nxbox_api(&dev,"create").CreateCommandList(0,0,nullptr,nullptr,IID_PPV_ARGS(&list));
 nxbox_api(list,"before-error").DrawInstanced(3,1,0,0);
 list->close_hr=E_INVALIDARG;
 nxbox_api(list,"first-failure").Close();
 nxbox_api(list,"later-failure").Close();
 assert(env["NXBOX_D3D12_LIST_CAPTURES"]=="1");
 std::string log;
 EdenXbox::CollectCommandListReports(
   [](const char *key){return env[key];},
   [&](const char *,const std::string &value){log+=value;});
 assert(log.find("site=before-error")!=std::string::npos);
 assert(log.find("site=first-failure")!=std::string::npos);
 assert(log.find("severity=0 category=3 description=corruption")!=std::string::npos);
 assert(log.find("severity=1 category=3 description=unaligned BC source box")!=std::string::npos);
 assert(log.find("severity=2 category=3 description=warning retained")!=std::string::npos);
 delete list;
}
"""
        )

    def test_argument_snapshots_arrays_nulls_and_resource_descriptions(self):
        self.compile_run(
            self.code()
            + r"""
int main() {
 NxboxListArgs draw("DrawIndexedInstanced"); draw.collect(12u,1u,0u,-7,0u);
 assert(draw.text.find("base_vertex=-7")!=std::string::npos);
 D3D12_VIEWPORT view[2]{{0,0,64,32,0,1},{1,2,3,4,0.2f,0.8f}};
 NxboxListArgs vp("RSSetViewports"); vp.collect(2,view);
 assert(vp.text.find("viewport[1]=1,2:3x4")!=std::string::npos);
 D3D12_RECT rect[2]{{0,0,4,4},{1,2,3,4}};
 NxboxListArgs clear("ClearRenderTargetView"); float color[4]{1,0,0,1};
 clear.collect(D3D12_CPU_DESCRIPTOR_HANDLE{1},color,2,rect);
 assert(clear.text.find("rect[1]=1,2:3,4")!=std::string::npos);
 NxboxListArgs bad("ClearRenderTargetView"); bad.collect(D3D12_CPU_DESCRIPTOR_HANDLE{1},color,2,static_cast<const D3D12_RECT *>(nullptr));
 assert(bad.text.find("rects=NULL")!=std::string::npos);
 ID3D12Resource resource; D3D12_TEXTURE_COPY_LOCATION loc; loc.pResource=&resource;
 NxboxListArgs copy("CopyTextureRegion"); D3D12_BOX box{0,0,0,4,4,1}; copy.collect(&loc,1,2,3,&loc,&box);
 assert(copy.text.find("dim=2 format=28 size=4096x1x1 mips=1 samples=1:0 flags=0x4")!=std::string::npos);
 assert(copy.text.find("box=0,0,0:4,4,1")!=std::string::npos);
 D3D12_CPU_DESCRIPTOR_HANDLE rtv[2]{{100},{200}}, dsv{300};
 NxboxListArgs targets("OMSetRenderTargets"); targets.collect(2,rtv,false,&dsv);
 assert(targets.text.find("handle[1]=cpu=200 expected=RTV")!=std::string::npos);
 assert(targets.text.find("300 expected=DSV")!=std::string::npos);
 ID3D12DescriptorHeap h1,h2; h2.desc.Type=0; ID3D12DescriptorHeap *heaps[]{&h1,&h2};
 NxboxListArgs h("SetDescriptorHeaps"); h.collect(2,heaps);
 assert(h.text.find("type=1")!=std::string::npos && h.text.find("type=0")!=std::string::npos);
 ID3D12Device dev;
 {
   ID3D12DescriptorHeap tracked; tracked.desc.Type=2; tracked.start=100;
   nxbox_heap_created(&dev,&tracked);
   NxboxListArgs handles("OMSetRenderTargets"); handles.collect(2,rtv,false,&dsv);
   assert(handles.text.find("heap_type=2 heap_start=100 aligned=0")!=std::string::npos);
 }
 assert(nxbox_descriptor_ranges().empty());
 uint32_t words[]{7,8,9}; NxboxListArgs constants("SetGraphicsRoot32BitConstants");
 constants.collect(2,3,words,4);
 assert(constants.text.find("root_parameter=2 count=3")!=std::string::npos && constants.text.find("0x00000009")!=std::string::npos);
}
"""
        )


if __name__ == "__main__":
    unittest.main()
