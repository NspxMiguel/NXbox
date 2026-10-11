# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the generated draw observer and exercise gated binding diagnostics."""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest

import test_mesa_render_safety as safety
from test_mesa_full_chain import ROOT, SOURCE


@unittest.skipUnless(SOURCE.is_dir(), "Set NXBOX_MESA_SRC or provide pristine /tmp/mesa-pin")
class GeometryDiagnosticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="nxbox-geometry-test-")
        cls.root = ROOT.__class__(cls.temp.name)
        mesa = cls.root / "mesa"
        shutil.copytree(SOURCE, mesa, ignore=shutil.ignore_patterns(".git"))
        environment = os.environ.copy()
        environment.pop("NXBOX_MESA_SKIP", None)
        subprocess.run(
            [sys.executable, str(ROOT / "tools/nxbox/patch_mesa_uwp.py"), str(mesa)],
            env=environment,
            check=True,
            capture_output=True,
            timeout=60,
        )
        cls.driver = mesa / "src/gallium/drivers/d3d12"

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_draw_and_binding_observations(self):
        source = (self.driver / "d3d12_draw.cpp").read_text()
        observer = (
            "static void\nnxbox_record_geometry_draw"
            + source.split("static void\nnxbox_record_geometry_draw", 1)[1].split(
                "static void\nnxbox_refresh_bound_srv_shadows", 1
            )[0]
        )
        helper = (self.driver / "nxbox_geometry_diag.h").read_text().replace("#pragma once", "")
        runner = safety.MesaRenderSafetyTests()
        runner.root = self.root
        code = (
            r"""
#include <cassert>
#include <map>
#include <string>
#include <cstdlib>
#include <cstdint>
std::map<std::string,std::string> reports;
unsigned publications=0;
void SetEnvironmentVariableA(const char *key,const char *value) {
 reports[key]=value; ++publications;
}
"""
            + helper
            + r"""
constexpr unsigned DXGI_FORMAT_UNKNOWN=0, PIPE_FORMAT_NONE=0;
constexpr unsigned D3D12_GFX_SHADER_STAGES=5;
constexpr unsigned D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT=256;
constexpr unsigned D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT=4096;
struct pipe_resource { uint64_t address=0; };
struct d3d12_resource : pipe_resource {};
struct d3d12_resource *d3d12_resource(pipe_resource *r) {
 return static_cast<struct d3d12_resource *>(r);
}
uint64_t d3d12_resource_gpu_virtual_address(struct d3d12_resource *r) { return r->address; }
struct Element { unsigned InputSlot=0, AlignedByteOffset=0, Format=1; };
struct d3d12_vertex_elements_state {
 unsigned num_elements=1; Element elements[2]; unsigned format_conversion[2]{};
};
struct VB { struct { pipe_resource *resource=nullptr; } buffer; };
struct VBV { uint64_t BufferLocation=0; unsigned StrideInBytes=0; };
struct CB { pipe_resource *buffer=nullptr; unsigned buffer_offset=0,buffer_size=0; };
struct Binding { unsigned binding=0; };
struct Shader { unsigned num_cb_bindings=1; Binding cb_bindings[2]; };
struct Selector { Shader *current=nullptr; };
struct d3d12_context {
 struct { d3d12_vertex_elements_state *ves=nullptr; unsigned num_so_targets=0; } gfx_pipeline_state;
 unsigned num_vbs=1; VB vbs[2]; VBV vbvs[2];
 Selector *gfx_stages[5]{}; CB cbufs[5][3];
};
"""
            + observer
            + r"""
int main(int argc,char **argv) {
 assert(argc==2); setenv("NXBOX_GEOMETRY_DIAG",argv[1],1);
 const bool enabled=std::string(argv[1])=="1";
 struct d3d12_resource resource; resource.address=256;
 d3d12_vertex_elements_state ves; Shader shader; Selector selector{&shader};
 d3d12_context ctx; ctx.gfx_pipeline_state.ves=&ves;
 ctx.vbs[0].buffer.resource=&resource; ctx.vbvs[0]={256,16};
 ctx.gfx_stages[0]=&selector; ctx.cbufs[0][0]={&resource,256,65536};
 // An unused misaligned CBV must not be counted.
 ctx.cbufs[0][1]={&resource,1,65537};
 nxbox_record_geometry_draw(&ctx);
 if(enabled) assert(reports["NXBOX_D3D12_GEOMETRY"].find("cbv_offset=0 cbv_size=0")!=std::string::npos);
 ves.elements[0]={0,2,0}; ves.format_conversion[0]=123;
 ctx.vbvs[0]={257,15}; ctx.cbufs[0][0]={&resource,1,65537};
 ctx.gfx_pipeline_state.num_so_targets=1;
 nxbox_record_geometry_draw(&ctx);
 if(enabled) assert(reports["NXBOX_D3D12_GEOMETRY"]==
  "draws=2 vb_offset=1 vb_stride=1 element_offset=1 unknown_format=1 emulated_format=1 cbv_offset=1 cbv_size=1 so=1");
 // More than one bad element still increments draws-with-condition only once.
 ves.num_elements=2; ves.elements[1]=ves.elements[0]; ves.format_conversion[1]=123;
 nxbox_record_geometry_draw(&ctx);
 if(enabled) assert(reports["NXBOX_D3D12_GEOMETRY"].find("vb_offset=2")!=std::string::npos);
 // Fresh shader state and unbound buffers are safe to inspect.
 ctx.gfx_pipeline_state.ves=nullptr; ctx.gfx_stages[0]=nullptr;
 nxbox_record_geometry_draw(&ctx);
 for(unsigned i=0;i<20;++i) nxbox_record_uniform_binding(0,0,256,65536,256);
 nxbox_record_uniform_binding(0,7,255,65537,256);
 if(enabled) {
  assert(reports["NXBOX_GL_UBO_FIRST"]=="stage=0 slot=7 offset=255 size=65537 alignment=256");
  assert(reports["NXBOX_GL_UBO_GEOMETRY"]=="bindings=21 misaligned=1 oversized=1");
 }
 const unsigned before=publications;
 nxbox_record_uniform_binding(5,2,257,64,256);
 nxbox_record_uniform_binding(5,2,99,64,1);
 assert(publications==before);
 for(unsigned i=23;i<4096;++i) nxbox_record_uniform_binding(0,0,0,64,256);
 nxbox_record_uniform_binding(0,0,0,64,256);
 if(enabled) {
  assert(reports["NXBOX_GL_UBO_FIRST"].find("slot=7")!=std::string::npos);
  assert(reports["NXBOX_GL_UBO_GEOMETRY"]=="bindings=4096 misaligned=2 oversized=1");
 } else assert(reports.empty() && publications==0);
}
"""
        )
        for value in ("1", "0", "", "10"):
            with self.subTest(value=value):
                runner.compile_run(code, [value])

    def test_full_chain_observes_final_state_and_frontend_collects(self):
        source = (self.driver / "d3d12_draw.cpp").read_text()
        draw = source.split("void\nd3d12_draw_vbo", 1)[1].split(
            "ctx->state_dirty &= D3D12_DIRTY_COMPUTE_MASK", 1
        )[0]
        self.assertEqual(draw.count("nxbox_record_geometry_draw(ctx);"), 1)
        self.assertLess(
            draw.index("IASetVertexBuffers"), draw.index("nxbox_record_geometry_draw(ctx);")
        )
        self.assertLess(
            draw.index("nxbox_record_geometry_draw(ctx);"), draw.index(".ExecuteIndirect")
        )
        eden = (ROOT / "src/video_core/renderer_opengl/gl_buffer_cache.cpp").read_text()
        self.assertEqual(eden.count("nxbox_record_uniform_binding("), 2)
        session = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
        for name in ("NXBOX_GL_UBO_FIRST", "NXBOX_GL_UBO_GEOMETRY", "NXBOX_D3D12_GEOMETRY"):
            self.assertIn(name, session)


if __name__ == "__main__":
    unittest.main()
