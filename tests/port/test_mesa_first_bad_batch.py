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
 assert(!commands.full && commands.copied.right==28 && commands.copied.bottom==28);
 // Mips 14, 7, 3, 1 must not inherit an oversized NULL footprint copy.
 for(unsigned mip=1; mip<5; ++mip) {
   tex.SubresourceIndex=5+mip; // Array layer 1, not mip index 5+mip.
   unsigned physical=28>>mip;
   unsigned footprint=(physical+3)&~3u;
   buf.PlacedFootprint.Footprint.Width=buf.PlacedFootprint.Footprint.Height=footprint;
   c.CopyTextureRegion(&tex,0,0,0,&buf,nullptr);
   assert(commands.copied.right==physical && commands.copied.bottom==physical);
   assert(commands.copied.back==1);
   c.CopyTextureRegion(&buf,0,0,0,&tex,nullptr);
   assert(commands.copied.right==physical && commands.copied.bottom==physical);
 }
 tex.SubresourceIndex=0;
 D3D12_BOX partial{24,24,0,27,27,1};
 c.CopyTextureRegion(&tex,24,24,0,&tex,&partial);
 assert(commands.copied.left==24 && commands.copied.right==28);
 assert(commands.copied.top==24 && commands.copied.bottom==28);
 // A full logical 27x27 readback/blit copies the padded last block.
 D3D12_BOX logical{0,0,0,27,27,1};
 c.CopyTextureRegion(&tex,0,0,0,&tex,&logical);
 assert(commands.copied.right==28 && commands.copied.bottom==28);
 unsigned before=commands.calls;
 c.CopyTextureRegion(&tex,1,0,0,&tex,&partial);
 c.CopyTextureRegion(&tex,28,0,0,&tex,&partial);
 assert(commands.calls==before);
 // All DXGI BC families, including typed and typeless forms.
 for(unsigned format=0;format<110;++format) {
   texture.desc.Format=buf.PlacedFootprint.Footprint.Format=format;
   buf.PlacedFootprint.Footprint.Width=buf.PlacedFootprint.Footprint.Height=28;
   c.CopyTextureRegion(&tex,0,0,0,&buf,nullptr);
   assert(commands.full == !((format>=70 && format<=84)||(format>=94 && format<=99)));
 }
 // Mixed BC/uncompressed copies use different units and must be left alone.
 texture.desc.Format=83; buf.PlacedFootprint.Footprint.Format=28;
 c.CopyTextureRegion(&tex,0,0,0,&buf,nullptr); assert(commands.full);
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
