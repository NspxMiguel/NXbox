# SPDX-License-Identifier: GPL-3.0-or-later
"""Host execution of Mesa safety helpers with captured DXIL and mock D3D12."""

import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_mesa_pso import DRIVER, FIXTURES, PATCH, ROOT


class MesaRenderSafetyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="nxbox-render-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.driver = self.root / DRIVER
        self.driver.mkdir(parents=True)
        for source in FIXTURES.glob("*.cpp"):
            shutil.copyfile(source, self.driver / source.name)

    def compile_run(self, code, args=()):
        source = self.root / "test.cpp"
        binary = self.root / "test"
        source.write_text(code)
        subprocess.run(
            [
                "clang++",
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wno-unused-function",
                str(source),
                "-o",
                str(binary),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        subprocess.run([str(binary), *args], check=True, timeout=10)

    def test_fixture_integrity_and_session_loss_exit(self):
        expected = {
            "d3d12_blit.cpp": "d1d0133d5f3d0b2714b8492a97b19f00777165210e5b97ae8f0460db46707631",
            "d3d12_query.cpp": "1926cc2731ef817e9b05a63b68f5494e7a66b1116f2b2c10e8de604a45f4cdab",
        }
        for name, digest in expected.items():
            self.assertEqual(hashlib.sha256((FIXTURES / name).read_bytes()).hexdigest(), digest)
        session = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
        report_header = (
            (ROOT / "src/eden_uwp/diagnostic_report.h").read_text().replace("#pragma once", "")
        )
        report_helpers = session.split("    const auto read_report =", 1)[1].split(
            "    while (!closed.load", 1
        )[0]
        report_helpers = "    const auto read_report =" + report_helpers
        guard = session.split("        char device_lost[2]{};", 1)[1].split(
            "        const int request =", 1
        )[0]
        self.compile_run(
            r"""
#include <cassert>
#include <cstring>
#include <stdexcept>
#include <string>
#include <initializer_list>
bool removed=false;
unsigned GetEnvironmentVariableA(const char *name, char *value, size_t) {
 if (!removed || strcmp(name,"NXBOX_D3D12_DEVICE_LOST")) return 0;
 value[0]='1'; value[1]=0; return 1;
}
void Diagnostic(const std::string &) {}
"""
            + report_header
            + "\nusing namespace EdenXbox;\nvoid check() {\n"
            + report_helpers
            + "char device_lost[2]{};\n"
            + guard
            + r"""
}
int main() {
 check(); removed=true;
 try { check(); assert(false); }
 catch (const std::runtime_error &) {}
}
"""
        )

    def test_captured_signatures_and_exact_pair_quarantine(self):
        capture = json.loads((ROOT / "tests/port/fixtures/botw-pipe6-dxil.json").read_text())
        arrays = "\n".join(
            f"unsigned char {stage.lower()}[] = {{{','.join(str(b) for b in bytes.fromhex(data))}}};"
            for stage, data in capture.items()
        )
        header = (ROOT / "tools/nxbox/mesa_pso_input.h").read_text().replace("#pragma once", "")
        self.compile_run(
            r"""
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
using UINT = unsigned;
struct D3D12_SHADER_BYTECODE { const void *pShaderBytecode; size_t BytecodeLength; };
struct D3D12_INPUT_ELEMENT_DESC {
 const char *SemanticName; UINT SemanticIndex, Format, InputSlot, AlignedByteOffset,
 InputSlotClass, InstanceDataStepRate;
};
struct D3D12_INPUT_LAYOUT_DESC { const D3D12_INPUT_ELEMENT_DESC *pInputElementDescs; UINT NumElements; };
constexpr UINT D3D12_APPEND_ALIGNED_ELEMENT = ~0u;
constexpr int D3D12_BLEND_ONE=2, D3D12_BLEND_ZERO=1, D3D12_BLEND_OP_ADD=1, D3D12_LOGIC_OP_NOOP=4;
struct Target { bool BlendEnable, LogicOpEnable; int SrcBlend, SrcBlendAlpha, DestBlend,
 DestBlendAlpha, BlendOp, BlendOpAlpha, LogicOp; };
struct D3D12_BLEND_DESC { Target RenderTarget[8]; };
unsigned GetEnvironmentVariableA(const char *, char *, unsigned) { return 0; }
void SetEnvironmentVariableA(const char *, const char *) {}
"""
            + header
            + arrays
            + r"""
int main() {
 D3D12_SHADER_BYTECODE v{vs,sizeof(vs)}, p{ps,sizeof(ps)};
 assert(nxbox_pso_quarantined(v,p));
 NxboxInputSignature sig;
 assert(sig.parse(v) && sig.count == 2);
 D3D12_INPUT_ELEMENT_DESC input[] = {{"TEXCOORD",0,2,0,0,0,0}, {"TEXCOORD",1,2,0,16,0,0}};
 D3D12_INPUT_ELEMENT_DESC storage[2];
 D3D12_INPUT_LAYOUT_DESC layout{input,2};
 for (unsigned i=0; i<2; ++i) {
   auto e=sig.element(i);
   assert(sig.declares(input[i]) && sig.word(e+16)==3 && e[24]==15);
 }
 nxbox_pso_fix_inputs(v,layout,storage,2);
 assert(layout.NumElements==2 && layout.pInputElementDescs==input);
 NxboxInputSignature fragment;
 assert(fragment.parse(p) && fragment.count==4);
 for (unsigned i=0; i<4; ++i) {
   auto e=fragment.element(i);
   assert(fragment.word(e+8)==i && fragment.word(e+16)==3 && e[24]==15);
 }
 // Same sizes/layout are insufficient: each individual byte change must pass.
 for (size_t i=0;i<sizeof(vs);++i) { vs[i]^=1; assert(!nxbox_pso_quarantined(v,p)); vs[i]^=1; }
 for (size_t i=0;i<sizeof(ps);++i) { ps[i]^=1; assert(!nxbox_pso_quarantined(v,p)); ps[i]^=1; }
 assert(!nxbox_pso_quarantined({nullptr,2492},p));
 assert(!nxbox_pso_quarantined({vs,2491},p));
}
"""
        )
        PATCH.patch_pso(self.root)
        source = (self.driver / "d3d12_pipeline_state.cpp").read_text()
        self.assertLess(
            source.index("nxbox_pso_quarantined(pso_desc.VS"), source.index("auto create = [&]()")
        )
        PATCH.patch_null_pso(self.root)
        self.assertIn("if (!ctx->current_gfx_pso)", (self.driver / "d3d12_draw.cpp").read_text())

    def test_sync_default_opt_out_and_removal_during_wait(self):
        header = (ROOT / "tools/nxbox/mesa_sync_batch.h").read_text()
        enabled = header.split("inline bool nxbox_sync_batch_enabled()", 1)[1].split(
            "struct NxboxBatchJournal", 1
        )[0]
        wait = header.split("inline HRESULT nxbox_sync_wait", 1)[1]
        code = (
            r"""
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
using UINT64=uint64_t; using HRESULT=int32_t;
constexpr HRESULT S_OK = 0, DXGI_ERROR_DEVICE_REMOVED = -1;
#define FAILED(hr) ((hr)<0)
unsigned ticks=0;
void Sleep(unsigned) { ++ticks; }
unsigned GetEnvironmentVariableA(const char *name, char *out, unsigned size) {
 const char *v=getenv(name); if(!v) return 0;
 unsigned n=strlen(v); if(n<size) memcpy(out,v,n+1); return n;
}
struct ID3D12Device { HRESULT GetDeviceRemovedReason() {return ticks>=3 ? -1 : 0;} };
struct ID3D12Fence { UINT64 value=0; UINT64 GetCompletedValue() { return value; } };
"""
            + "bool nxbox_sync_batch_enabled()"
            + enabled
            + "HRESULT nxbox_sync_wait"
            + wait
            + r"""
int main(int argc, char **argv) {
 assert(argc==2 && nxbox_sync_batch_enabled()==(argv[1][0]=='1'));
 ID3D12Device dev; ID3D12Fence fence;
 assert(nxbox_sync_wait(&dev,&fence,10)<0 && ticks==3);
 ticks=0; fence.value=10; assert(nxbox_sync_wait(&dev,&fence,10)==0 && ticks==0);
 fence.value=UINT64_MAX; assert(nxbox_sync_wait(&dev,&fence,10)<0 && ticks==0);
}
"""
        )
        previous = os.environ.pop("NXBOX_SYNC_BATCH", None)
        try:
            for value in (None, "0", "1", "", "false", "00"):
                if value is None:
                    os.environ.pop("NXBOX_SYNC_BATCH", None)
                else:
                    os.environ["NXBOX_SYNC_BATCH"] = value
                self.compile_run(code, ["0" if value == "0" else "1"])
        finally:
            if previous is None:
                os.environ.pop("NXBOX_SYNC_BATCH", None)
            else:
                os.environ["NXBOX_SYNC_BATCH"] = previous

    def test_lost_query_and_nonblocking_query(self):
        header = (ROOT / "tools/nxbox/mesa_query_wait.h").read_text()
        header = header.replace('#include "nxbox_dred.h"', "").replace("#pragma once", "")
        self.compile_run(
            r"""
#include <cassert>
#include <cstdint>
using UINT64=uint64_t; using HRESULT=int32_t;
constexpr HRESULT DXGI_ERROR_DEVICE_REMOVED=-1;
#define FAILED(hr) ((hr)<0)
unsigned ticks=0; bool lost=false;
struct ID3D12Device { unsigned fail_at=3; HRESULT GetDeviceRemovedReason() {return ticks>=fail_at ? -1 : 0;} };
struct ID3D12Fence { UINT64 value=0; UINT64 GetCompletedValue() {return value;} };
void Sleep(unsigned) { ++ticks; }
void nxbox_dred_capture(ID3D12Device *, HRESULT hr, const char *) {assert(hr<0); lost=true;}
"""
            + header
            + r"""
int main() {
 ID3D12Device dev; ID3D12Fence fence;
 assert(!nxbox_query_ready(&dev,&fence,5,false) && ticks==0 && !lost);
 assert(!nxbox_query_ready(&dev,&fence,UINT64_MAX,true) && ticks==0);
 assert(nxbox_query_ready(&dev,&fence,5,true) && ticks==3 && lost);
 ticks=0; lost=false; dev.fail_at=0;
 assert(nxbox_query_ready(&dev,&fence,5,true) && ticks==0 && lost);
 dev.fail_at=3; lost=false; fence.value=5;
 assert(nxbox_query_ready(&dev,&fence,5,true) && !lost);
 fence.value=UINT64_MAX;
 assert(nxbox_query_ready(&dev,&fence,5,true) && lost);
}
"""
        )

    def test_patch_composition_and_atomic_anchor_failure(self):
        PATCH.patch_pso(self.root)
        PATCH.patch_draw(self.root)
        PATCH.patch_null_pso(self.root)
        PATCH.patch_render_safety(self.root)
        draw = (self.driver / "d3d12_draw.cpp").read_text()
        self.assertIn("transition_array_size = 1;", draw)
        self.assertLess(
            draw.index("if (!ctx->current_gfx_pso)"), draw.index("assert(ctx->current_gfx_pso)")
        )
        blit = (self.driver / "d3d12_blit.cpp").read_text()
        self.assertIn("info->dst.level, 1,", blit)
        self.assertIn("info->dst.box.y, dst_z,", blit)
        self.assertNotIn("dst_loc.SubresourceIndex = 1;", blit)
        query = (self.driver / "d3d12_query.cpp").read_text()
        self.assertNotIn("SetEventOnCompletion(query->fence_value, NULL)", query)
        self.assertLess(
            query.index("memset(result, 0, sizeof(*result))"),
            query.index("return accumulate_result_cpu"),
        )
        before = {p: p.read_bytes() for p in self.driver.iterdir()}
        with self.assertRaisesRegex(RuntimeError, "anchor mismatch"):
            PATCH.patch_render_safety(self.root)
        self.assertEqual(before, {p: p.read_bytes() for p in self.driver.iterdir()})

    def test_msaa_copy_selection_and_stencil_plane(self):
        PATCH.patch_render_safety(self.root)
        source = (self.driver / "d3d12_blit.cpp").read_text()
        direct = source.split("static bool\ndirect_copy_supported", 1)[1].split(
            "inline static unsigned", 1
        )[0]
        subresource = source.split("inline static unsigned\nget_subresource_id", 1)[1].split(
            "static void\ncopy_subregion", 1
        )[0]
        self.compile_run(
            r"""
#include <cassert>
#include <cstdlib>
#define MAX2(a,b) ((a)>(b)?(a):(b))
constexpr int PIPE_MASK_ZS=48, PIPE_BIND_DEPTH_STENCIL=1;
constexpr int D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED=0;
enum pipe_texture_target { TEXTURE_2D, TEXTURE_ARRAY };
struct pipe_box { int x,y,z,width,height,depth; };
struct pipe_resource { int nr_samples=4, format=28, bind=0; unsigned width0=64,height0=64,depth0=1; };
struct view { pipe_resource *resource; int format=28; unsigned level=0; pipe_box box{0,0,0,64,64,1}; };
struct pipe_blit_info { view src,dst; bool scissor_enable=false,alpha_blend=false,render_condition_enable=false; int mask=15; };
struct d3d12_screen { struct {int ProgrammableSamplePositionsTier=0;} opts2; };
bool formats_are_copy_compatible(int a,int b) {return a==b;}
bool util_format_is_depth_or_stencil(int f) {return f==45;}
int util_format_get_mask(int) {return 15;}
bool box_fits(const pipe_box *,pipe_resource *,unsigned) {return true;}
unsigned u_minify(unsigned n,unsigned level) {return n>>level ? n>>level : 1;}
bool d3d12_subresource_id_uses_layer(pipe_texture_target t) {return t==TEXTURE_ARRAY;}
"""
            + "bool direct_copy_supported"
            + direct
            + "unsigned get_subresource_id"
            + subresource
            + r"""
int main() {
 d3d12_screen screen; pipe_resource src,dst; pipe_blit_info info{{&src},{&dst}};
 assert(direct_copy_supported(&screen,&info,false));
 info.src.box.width=info.dst.box.width=32;
 assert(!direct_copy_supported(&screen,&info,false));
 screen.opts2.ProgrammableSamplePositionsTier=2;
 assert(!direct_copy_supported(&screen,&info,false));
 src.nr_samples=dst.nr_samples=1;
 assert(direct_copy_supported(&screen,&info,false));
 src.nr_samples=dst.nr_samples=4; info.src.box.width=info.dst.box.width=64;
 dst.width0=128; assert(!direct_copy_supported(&screen,&info,false));
 dst.width0=64; info.dst.box.x=1; assert(!direct_copy_supported(&screen,&info,false));
 info.dst.box.x=0; info.src.box.height=-64;
 assert(!direct_copy_supported(&screen,&info,false));
 unsigned z=2;
 assert(get_subresource_id(TEXTURE_ARRAY,3,5,z,&z,4,1)==33 && z==0);
 z=0; assert(get_subresource_id(TEXTURE_2D,2,5,z,&z,1,1)==7);
}
"""
        )


if __name__ == "__main__":
    unittest.main()
