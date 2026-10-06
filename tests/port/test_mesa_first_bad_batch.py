# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay MK8D BC copies and validate the pinned texture/RTV barrier patches."""

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import test_mesa_render_safety as safety
from test_mesa_full_chain import SOURCE
from test_mesa_journal import MOCK

ROOT = Path(__file__).resolve().parents[2]
HEADER = (ROOT / "tools/nxbox/mesa_sync_batch.h").read_text().replace("#pragma once", "")


class MesaBlockCopyTests(unittest.TestCase):
    setUp = safety.MesaRenderSafetyTests.setUp
    compile_run = safety.MesaRenderSafetyTests.compile_run

    def test_bc_boxes_round_up_clamp_and_preserve_offsets(self):
        mock = MOCK.replace(
            " unsigned calls=0;",
            " unsigned calls=0; bool full=false; D3D12_BOX copied{}; UINT dx=0,dy=0,dz=0;",
        ).replace(
            "const D3D12_TEXTURE_COPY_LOCATION *, UINT, UINT, UINT,\n"
            "                        const D3D12_TEXTURE_COPY_LOCATION *, const D3D12_BOX *) { ++calls; }",
            "const D3D12_TEXTURE_COPY_LOCATION *, UINT x, UINT y, UINT z,\n"
            "                        const D3D12_TEXTURE_COPY_LOCATION *, const D3D12_BOX *box) { "
            "++calls; full=!box; if(box) copied=*box; dx=x; dy=y; dz=z; }",
        )
        self.compile_run(
            mock
            + HEADER
            + r"""
int main() {
 env["NXBOX_SYNC_BATCH"]="0";
 ID3D12Resource texture, buffer;
 texture.desc.Width=texture.desc.Height=1024;
 texture.desc.MipLevels=11; texture.desc.DepthOrArraySize=83;
 buffer.desc.Dimension=1;
 D3D12_TEXTURE_COPY_LOCATION tex, buf;
 tex.pResource=&texture; buf.pResource=&buffer; buf.Type=1;
 auto &footprint=buf.PlacedFootprint.Footprint;
 ID3D12GraphicsCommandList commands;
 auto c=nxbox_journal_commands(nullptr,&commands);
 for(unsigned format : {70u,71u,72u,73u,74u,75u,76u,77u,78u,79u,80u,
                       81u,82u,83u,84u,94u,95u,96u,97u,98u,99u}) {
   texture.desc.Format=footprint.Format=format;
   for(unsigned size : {2u,1u,5u,4u,8u}) {
     const unsigned aligned=(size+3)&~3u;
     D3D12_BOX box{0,0,0,size,size,1};
     footprint.Width=footprint.Height=16;
     tex.SubresourceIndex=0;
     auto before=commands.calls;
     c.CopyTextureRegion(&tex,4,8,0,&buf,&box);
     assert(commands.calls==before+1 && !commands.full);
     assert(commands.copied.right==aligned && commands.copied.bottom==aligned);
     assert(commands.dx==4 && commands.dy==8 && commands.dz==0);
     // A full footprint is NULL; padding beyond it must never be copied.
     footprint.Width=footprint.Height=aligned;
     c.CopyTextureRegion(&tex,0,0,0,&buf,&box); assert(commands.full);
     footprint.Width=footprint.Height=4;
     c.CopyTextureRegion(&tex,0,0,0,&buf,&box); assert(commands.full);
   }
   // Actual array subresources from the console: layer 46/47/48, mip 9.
   for(unsigned sub : {515u,526u,537u,516u,527u,538u}) {
     tex.SubresourceIndex=sub;
     const unsigned size=sub%11==9 ? 2u : 1u;
     D3D12_BOX box{0,0,0,size,size,1};
     footprint.Width=footprint.Height=4;
     c.CopyTextureRegion(&tex,0,0,0,&buf,&box); assert(commands.full);
     c.CopyTextureRegion(&buf,0,0,0,&tex,&box); assert(commands.full);
     c.CopyTextureRegion(&tex,0,0,0,&tex,&box); assert(commands.full);
     // A larger upload footprint is clipped to one complete destination block.
     footprint.Width=footprint.Height=8;
     c.CopyTextureRegion(&tex,0,0,0,&buf,nullptr);
     assert(!commands.full && commands.copied.right==4 && commands.copied.bottom==4);
     auto before=commands.calls;
     c.CopyTextureRegion(&tex,1,0,0,&buf,&box);
     c.CopyTextureRegion(&tex,0,1,0,&buf,&box);
     c.CopyTextureRegion(&tex,4,0,0,&buf,&box);
     c.CopyTextureRegion(&tex,0,0,1,&buf,&box);
     box.left=1;
     c.CopyTextureRegion(&tex,0,0,0,&buf,&box);
     assert(commands.calls==before);
   }
 }
 // Empty/reversed boxes are not expanded into actual writes.
 tex.SubresourceIndex=0; footprint.Width=footprint.Height=8;
 auto before=commands.calls;
 for(D3D12_BOX box : {D3D12_BOX{4,0,0,4,4,1}, D3D12_BOX{4,0,0,2,4,1},
                       D3D12_BOX{0,0,1,4,4,1}})
   c.CopyTextureRegion(&tex,0,0,0,&buf,&box);
 assert(commands.calls==before);
 // BC depth is one texel per block, not four. Preserve 3D slice offsets.
 texture.desc.Dimension=4; texture.desc.DepthOrArraySize=8;
 footprint.Depth=8;
 D3D12_BOX volume{0,0,2,5,5,4};
 c.CopyTextureRegion(&tex,4,4,3,&buf,&volume);
 assert(!commands.full && commands.copied.right==8 && commands.copied.back==4);
 assert(commands.copied.front==2 && commands.dz==3);
 // Overflow must clamp to the footprint, never wrap to an empty copy.
 D3D12_BOX huge{0,0,0,UINT32_MAX,UINT32_MAX,8};
 c.CopyTextureRegion(&tex,0,0,0,&buf,&huge); assert(commands.full);
 // Reinterpret copies measure one uncompressed texel per compressed block.
 texture.desc.Dimension=3; texture.desc.MipLevels=1;
 texture.desc.Width=texture.desc.Height=8;
 texture.desc.Format=83; footprint.Format=3;
 footprint.Width=footprint.Height=footprint.Depth=1;
 D3D12_BOX mixed{0,0,0,5,5,1};
 c.CopyTextureRegion(&buf,0,0,0,&tex,&mixed);
 assert(!commands.full && commands.copied.right==4 && commands.copied.bottom==4);
 texture.desc.Format=3; footprint.Format=83;
 texture.desc.Width=texture.desc.Height=2;
 footprint.Width=footprint.Height=8;
 c.CopyTextureRegion(&buf,0,0,0,&tex,nullptr); assert(commands.full);
 // Uncompressed copies retain their exact logical box.
 texture.desc.Format=footprint.Format=28;
 c.CopyTextureRegion(&buf,1,2,0,&tex,&mixed);
 assert(!commands.full && commands.copied.right==5 && commands.copied.bottom==5);
 assert(commands.dx==1 && commands.dy==2);
}
"""
        )

    def test_bc_upload_readback_blit_and_small_mips(self):
        mock = MOCK.replace(
            " unsigned calls=0;",
            " unsigned calls=0; bool full=false; D3D12_BOX copied{};",
        ).replace(
            "const D3D12_TEXTURE_COPY_LOCATION *, const D3D12_BOX *) { ++calls; }",
            "const D3D12_TEXTURE_COPY_LOCATION *, const D3D12_BOX *box) { ++calls; "
            "full=!box; if(box) copied=*box; }",
        )
        self.compile_run(
            mock
            + HEADER
            + r"""
int main() {
 ID3D12Resource texture, buffer;
 texture.desc.Format=83; texture.desc.Width=texture.desc.Height=28;
 texture.desc.MipLevels=5; texture.desc.DepthOrArraySize=3;
 D3D12_TEXTURE_COPY_LOCATION tex, buf;
 tex.pResource=&texture; buf.pResource=&buffer; buf.Type=1;
 buf.PlacedFootprint.Footprint.Format=83;
 buf.PlacedFootprint.Footprint.Width=buf.PlacedFootprint.Footprint.Height=28;
 ID3D12GraphicsCommandList commands;
 // Safety must not depend on diagnostics being enabled.
 auto c=nxbox_journal_commands(nullptr,&commands);
 c.CopyTextureRegion(&tex,0,0,0,&buf,nullptr);
 assert(commands.full);
 // Mips 14, 7, 3, 1 occupy full blocks, including the 4x4 tail.
 for(unsigned mip=1; mip<5; ++mip) {
   tex.SubresourceIndex=5+mip; // Array layer 1, not mip index 5+mip.
   unsigned physical=28>>mip;
   unsigned footprint=(physical+3)&~3u;
   buf.PlacedFootprint.Footprint.Width=buf.PlacedFootprint.Footprint.Height=footprint;
   c.CopyTextureRegion(&tex,0,0,0,&buf,nullptr);
   assert(commands.full);
   c.CopyTextureRegion(&buf,0,0,0,&tex,nullptr);
   assert(commands.full);
 }
 tex.SubresourceIndex=0;
 D3D12_BOX partial{24,24,0,27,27,1};
 c.CopyTextureRegion(&tex,24,24,0,&tex,&partial);
 assert(commands.copied.left==24 && commands.copied.right==28);
 assert(commands.copied.top==24 && commands.copied.bottom==28);
 // A full logical 27x27 readback/blit copies the padded last block.
 D3D12_BOX logical{0,0,0,27,27,1};
 c.CopyTextureRegion(&tex,0,0,0,&tex,&logical);
 assert(commands.full);
 unsigned before=commands.calls;
 c.CopyTextureRegion(&tex,1,0,0,&tex,&partial);
 c.CopyTextureRegion(&tex,28,0,0,&tex,&partial);
 assert(commands.calls==before);
 // All DXGI BC families, including typed and typeless forms.
 for(unsigned format=0;format<110;++format) {
   texture.desc.Format=buf.PlacedFootprint.Footprint.Format=format;
   buf.PlacedFootprint.Footprint.Width=buf.PlacedFootprint.Footprint.Height=28;
   c.CopyTextureRegion(&tex,0,0,0,&buf,nullptr);
   assert(commands.full);
 }
 // Mixed copies clip in block units: 7 source texels fill 28 BC texels.
 texture.desc.Format=83; buf.PlacedFootprint.Footprint.Format=28;
 c.CopyTextureRegion(&tex,0,0,0,&buf,nullptr);
 assert(!commands.full && commands.copied.right==7 && commands.copied.bottom==7);
}
"""
        )


@unittest.skipUnless(SOURCE.is_dir(), "Set NXBOX_MESA_SRC or provide /tmp/mesa-pin")
class MesaFirstBadBatchTests(unittest.TestCase):
    compile_run = safety.MesaRenderSafetyTests.compile_run

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="nxbox-first-bad-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.root = Path(cls.temp.name) / "mesa"
        shutil.copytree(SOURCE, cls.root, ignore=shutil.ignore_patterns(".git"))
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools/nxbox/patch_mesa_uwp.py"), str(cls.root)],
            capture_output=True,
            text=True,
            timeout=60,
        )
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        cls.driver = cls.root / safety.DRIVER

    def test_physical_allocation_preserves_logical_dimensions(self):
        source = (self.driver / "d3d12_resource.cpp").read_text()
        code = source.split("   desc.Width = templ->width0;", 1)[1].split(
            "   desc.DepthOrArraySize = templ->array_size;", 1
        )[0]
        self.compile_run(
            r"""
#include <cassert>
#define ALIGN(n,a) (((n)+(a)-1)&~((a)-1))
bool util_format_is_compressed(unsigned f) {return f!=0;}
unsigned util_format_get_blockwidth(unsigned f) {return f;}
unsigned util_format_get_blockheight(unsigned f) {return f;}
struct Resource {unsigned width0,height0,format;};
int main() {
 for(unsigned block : {1u,4u,8u}) {
   Resource original{27,27,block==1?0:block}; const auto *templ=&original;
   struct {unsigned Width,Height;} desc{templ->width0,0};
""".replace("#include <cassert>", "#include <cassert>\n#include <initializer_list>")
            + code
            + r"""
   assert(desc.Width==(block==1?27u:block==4?28u:32u));
   assert(desc.Height==desc.Width);
   assert(original.width0==27 && original.height0==27);
 }
}
"""
        )
        # The common wrapper covers both transfer directions and direct blits.
        blit = (self.driver / "d3d12_blit.cpp").read_text()
        self.assertIn(
            "nxbox_journal_commands(ctx->nxbox_journal, ctx->cmdlist).CopyTextureRegion", blit
        )
        self.assertIn(
            "nxbox_journal_commands(ctx->nxbox_journal, ctx->cmdlist).CopyTextureRegion", source
        )

    def test_readback_and_direct_copy_boxes_align_at_creation(self):
        for name, resource in (("d3d12_resource.cpp", "res"), ("d3d12_blit.cpp", "src")):
            source = (self.driver / name).read_text()
            anchor = "src_box.right = ALIGN(src_box.right,"
            # Include both assignments, taken from the actual patched source.
            start = source.index(anchor)
            code = source[
                start : source.index(";", source.index("src_box.bottom = ALIGN", start)) + 1
            ]
            self.compile_run(
                r"""
#include <cassert>
#include <initializer_list>
#define ALIGN(n,a) (((n)+(a)-1)&~((a)-1))
unsigned util_format_get_blockwidth(unsigned f) {return f;}
unsigned util_format_get_blockheight(unsigned f) {return f;}
int main() {
 for(unsigned block : {1u,4u,8u}) {
   struct {struct {struct {unsigned format;} b;} base;} resource{{{block}}};
"""
                + f"auto *{resource}=&resource;\n"
                + r"""
   for(unsigned size : {1u,2u,4u,5u,8u}) {
     struct {unsigned right,bottom;} src_box{size,size};
"""
                + code
                + r"""
     assert(src_box.right==((size+block-1)/block)*block);
     assert(src_box.bottom==src_box.right);
   }
 }
}
"""
            )

    def test_texture_barrier_waits_and_global_uav_is_retained(self):
        source = (self.driver / "d3d12_context.cpp").read_text()
        body = source.split(
            "d3d12_texture_barrier(struct pipe_context *pctx, unsigned flags)\n{", 1
        )[1].split("\n}\n", 1)[0]
        self.compile_run(
            r"""
#include <cassert>
#include <cstring>
struct pipe_context {};
struct Context {void *nxbox_journal=nullptr;} context;
unsigned waits=0, records=0;
#define d3d12_context Context
Context *Context(pipe_context *) {return &context;}
void nxbox_journal_add(void *, const char *, const char *text) {
 assert(strcmp(text,"TEXTURE_BARRIER submit-and-wait")==0); ++records;
}
void d3d12_flush_cmdlist_and_wait(struct Context *) {assert(records==1); ++waits;}
void texture_barrier(pipe_context *pctx) {
"""
            + body
            + "\n}\nint main() {pipe_context ctx; texture_barrier(&ctx); assert(waits==1);}\n"
        )
        self.assertNotIn("D3D12_RESOURCE_BARRIER_TYPE_ALIASING", source)
        self.assertIn("if (flags & (PIPE_BARRIER_IMAGE | PIPE_BARRIER_SHADER_BUFFER))", source)
        self.assertIn("D3D12_RESOURCE_BARRIER uavBarrier = {};", source)
        self.assertIn("uavBarrier.UAV.pResource = nullptr;", source)

    def test_rtv_guard_uses_created_view_and_skips_mismatch(self):
        source = (self.driver / "d3d12_draw.cpp").read_text()
        body = source.split("   // Compare the PSO format", 1)[1].split(
            "   struct d3d12_rasterizer_state *rast", 1
        )[0]
        body = body[body.index("   for (unsigned i") :]
        self.compile_run(
            r"""
#include <cassert>
constexpr unsigned D3D12_SURFACE_CONVERSION_NONE=0;
struct d3d12_surface {unsigned nxbox_rtv_format=61,nxbox_uint_rtv_format=30;};
#define d3d12_surface(s) (s)
struct Context {struct {unsigned nr_cbufs=1; d3d12_surface *cbufs[1];} fb;
 void *nxbox_journal=nullptr; unsigned expected=61;};
unsigned d3d12_rtv_format(Context *ctx,unsigned) {return ctx->expected;}
unsigned draws=0,rejections=0;
void nxbox_journal_add(void *,const char *,const char *,unsigned,unsigned,unsigned) {++rejections;}
void debug_printf(const char *,unsigned,unsigned,unsigned) {}
void draw(Context *ctx,unsigned mode) {unsigned conversion_modes[]={mode};
"""
            + body
            + r"""
 ++draws;
}
int main() {
 d3d12_surface surface; Context ctx{{1,{&surface}},nullptr,61};
 draw(&ctx,0); assert(draws==1 && rejections==0);
 ctx.expected=28; draw(&ctx,0); assert(draws==1 && rejections==1);
 ctx.expected=30; draw(&ctx,1); assert(draws==2 && rejections==1);
 surface.nxbox_uint_rtv_format=0; draw(&ctx,1); assert(draws==2 && rejections==2);
 ctx.fb.cbufs[0]=nullptr; draw(&ctx,0); assert(draws==3);
}
"""
        )
        surface = (self.driver / "d3d12_surface.cpp").read_text()
        self.assertIn("surface->nxbox_rtv_format = (unsigned)dxgi_format;", surface)
        self.assertIn(
            "surface->nxbox_uint_rtv_format = (unsigned)DXGI_FORMAT_R8G8B8A8_UINT;", surface
        )
        self.assertIn("RTV_BIND slot=%u view=%u res=%s", source)
        self.assertIn("surface->rgba_texture : surface->base.texture", source)

    def test_anchor_failure_is_atomic(self):
        before = {p: p.read_bytes() for p in self.driver.iterdir() if p.is_file()}
        with self.assertRaisesRegex(RuntimeError, "anchor mismatch"):
            safety.PATCH.patch_first_bad_batch(self.root)
        self.assertEqual(before, {p: p.read_bytes() for p in before})
