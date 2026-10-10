# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate GPU SRV reinterpretation and its composition with the pinned Mesa patches."""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import test_mesa_render_safety as safety
from test_mesa_full_chain import ROOT, SOURCE

DIAGNOSTICS = (ROOT / "tools/nxbox/mesa_view_cast.h").read_text().replace("#pragma once", "")
COPY = (ROOT / "tools/nxbox/mesa_view_cast_copy.h").read_text()
COPY = COPY.replace('#include "nxbox_view_cast.h"', "").replace('#include "d3d12_query.h"', "")
MOCK = r"""
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
std::map<std::string,std::string> env;
void SetEnvironmentVariableA(const char *key,const char *value) { env[key]=value; }
using UINT=unsigned;
using UINT64=uint64_t;
enum pipe_format { F16=1, RG8, F32, RGBA8, BC, DEPTH, PLANAR, EMULATED };
constexpr unsigned PIPE_BUFFER=0, PIPE_TEXTURE_2D_ARRAY=1, PIPE_TEXTURE_3D=2;
constexpr unsigned PIPE_BIND_SAMPLER_VIEW=1, PIPE_USAGE_DEFAULT=0;
constexpr unsigned D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT=512;
constexpr unsigned D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX=0;
constexpr unsigned D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT=1;
constexpr unsigned D3D12_PREDICATION_OP_EQUAL_ZERO=0;
unsigned blocksize(pipe_format f) { return (f==F16 || f==RG8) ? 2 : 4; }
unsigned util_format_get_blocksize(pipe_format f) { return blocksize(f); }
bool util_format_is_compressed(pipe_format f) { return f==BC; }
bool util_format_is_depth_or_stencil(pipe_format f) { return f==DEPTH; }
unsigned util_format_get_num_planes(pipe_format f) { return f==PLANAR ? 2 : 1; }
unsigned util_format_get_blockwidth(pipe_format f) { return f==BC ? 4 : 1; }
unsigned util_format_get_blockheight(pipe_format f) { return f==BC ? 4 : 1; }
struct pipe_screen;
struct pipe_resource {
 pipe_screen *screen=nullptr;
 unsigned target=PIPE_TEXTURE_2D_ARRAY,width0=131,height0=7,depth0=1,array_size=2,last_level=2;
 unsigned nr_samples=0,nr_storage_samples=0,bind=0,usage=0,flags=0;
 pipe_format format=F16; pipe_resource *next=nullptr;
};
struct D3D12_RESOURCE_DESC { pipe_resource resource; };
struct Footprint { unsigned Format=0,Width=0,Height=0,Depth=0,RowPitch=0; };
struct D3D12_PLACED_SUBRESOURCE_FOOTPRINT { uint64_t Offset=0; Footprint Footprint; };
struct ID3D12Resource {
 D3D12_RESOURCE_DESC desc; std::vector<std::vector<uint8_t>> images;
 std::vector<uint8_t> bytes;
};
struct D3D12_TEXTURE_COPY_LOCATION {
 ID3D12Resource *pResource=nullptr; unsigned Type=0,SubresourceIndex=0;
 D3D12_PLACED_SUBRESOURCE_FOOTPRINT PlacedFootprint;
};
struct nxbox_srv_shadow;
struct d3d12_resource {
 struct { pipe_resource b; } base; pipe_format overall_format=F16;
 nxbox_srv_shadow *nxbox_srv_shadows=nullptr; ID3D12Resource *native=nullptr;
};
struct d3d12_resource *d3d12_resource(pipe_resource *r) {
 return reinterpret_cast<struct d3d12_resource *>(r);
}
ID3D12Resource *d3d12_resource_resource(struct d3d12_resource *r) { return r->native; }
ID3D12Resource *d3d12_resource_underlying(struct d3d12_resource *r,uint64_t *offset) {
 *offset=512; return r->native;
}
D3D12_RESOURCE_DESC GetDesc(ID3D12Resource *r) { return r->desc; }
struct Device {
 void GetCopyableFootprints(const D3D12_RESOURCE_DESC *d,unsigned sub,unsigned count,uint64_t offset,
                           D3D12_PLACED_SUBRESOURCE_FOOTPRINT *out,UINT *rows,UINT64 *row_bytes,UINT64 *size) {
  assert(count==1); const auto &r=d->resource; unsigned mip=sub%(r.last_level+1);
  auto &f=out->Footprint; f.Format=r.format;
  f.Width=std::max(r.width0>>mip,1u); f.Height=std::max(r.height0>>mip,1u);
  f.Depth=r.target==PIPE_TEXTURE_3D ? std::max(r.depth0>>mip,1u) : 1;
  unsigned bytes=f.Width*(r.format==EMULATED ? 8 : blocksize(r.format));
  f.RowPitch=(bytes+255)&~255u; out->Offset=(offset+511)&~UINT64(511);
  if(rows) *rows=f.Height; if(row_bytes) *row_bytes=bytes;
  if(size) *size=UINT64(f.RowPitch)*f.Height*f.Depth;
 }
};
unsigned allocations=0,live_resources=0;
bool fail_texture=false,fail_buffer=false;
pipe_resource *create_resource(pipe_screen *,const pipe_resource *);
struct pipe_screen {
 bool is_format_supported(pipe_screen *,pipe_format,unsigned,unsigned,unsigned,unsigned) { return true; }
 pipe_resource *resource_create(pipe_screen *s,const pipe_resource *r) { return create_resource(s,r); }
};
struct d3d12_screen : pipe_screen { Device *dev; };
struct d3d12_screen *d3d12_screen(pipe_screen *s) { return static_cast<struct d3d12_screen *>(s); }
pipe_resource *create_resource(pipe_screen *screen,const pipe_resource *templ) {
 if((templ->target==PIPE_BUFFER && fail_buffer) || (templ->target!=PIPE_BUFFER && fail_texture)) return nullptr;
 auto *r=new struct d3d12_resource; r->base.b=*templ; r->base.b.screen=screen;
 r->overall_format=templ->format; r->native=new ID3D12Resource; r->native->desc.resource=r->base.b;
 if(templ->target==PIPE_BUFFER) r->native->bytes.resize(templ->width0+512);
 else {
  unsigned layers=templ->target==PIPE_TEXTURE_3D ? 1 : templ->array_size;
  r->native->images.resize((templ->last_level+1)*layers);
  for(unsigned i=0;i<r->native->images.size();++i) {
   D3D12_PLACED_SUBRESOURCE_FOOTPRINT f;
   d3d12_screen(screen)->dev->GetCopyableFootprints(&r->native->desc,i,1,0,&f,nullptr,nullptr,nullptr);
   r->native->images[i].resize(f.Footprint.Width*f.Footprint.Height*f.Footprint.Depth*blocksize(templ->format));
  }
 }
 ++allocations; ++live_resources; return &r->base.b;
}
void pipe_resource_reference(pipe_resource **p,pipe_resource *value) {
 assert(!value); if(*p) { auto *r=d3d12_resource(*p); delete r->native; delete r; --live_resources; }
 *p=value;
}
pipe_resource *pipe_buffer_create(pipe_screen *s,unsigned bind,unsigned usage,unsigned size) {
 pipe_resource r; r.target=PIPE_BUFFER; r.width0=size; r.bind=bind; r.usage=usage;
 return create_resource(s,&r);
}
#define CALLOC_STRUCT(type) static_cast<struct type *>(calloc(1,sizeof(struct type)))
#define FREE free
struct Commands { unsigned predication_changes=0; void SetPredication(void *,unsigned,unsigned) { ++predication_changes; } };
template<typename T> T &nxbox_api(T *p,const char *) { return *p; }
struct d3d12_context { struct { pipe_screen *screen; } base; Commands *cmdlist; bool current_predication=true; };
void d3d12_enable_predication(d3d12_context *c) { ++c->cmdlist->predication_changes; }
struct copy_info {
 struct d3d12_resource *dst; D3D12_TEXTURE_COPY_LOCATION dst_loc;
 UINT dst_x,dst_y,dst_z; struct d3d12_resource *src;
 D3D12_TEXTURE_COPY_LOCATION src_loc; void *src_box;
};
unsigned copies=0;
void copy_texture_region(d3d12_context *,copy_info &info) {
 ++copies; assert(!info.src_box && !info.dst_x && !info.dst_y && !info.dst_z);
 bool upload=info.src_loc.Type==D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
 const auto &loc=upload ? info.src_loc : info.dst_loc;
 const auto &tex=upload ? info.dst_loc : info.src_loc;
 const auto &f=loc.PlacedFootprint.Footprint;
 assert(f.Format==unsigned(tex.pResource->desc.resource.format));
 assert(f.RowPitch%256==0 && loc.PlacedFootprint.Offset%512==0);
 auto &image=tex.pResource->images[tex.SubresourceIndex]; auto &buffer=loc.pResource->bytes;
 unsigned stride=f.Width*blocksize(static_cast<pipe_format>(f.Format));
 for(unsigned row=0;row<f.Height*f.Depth;++row) {
  auto *a=image.data()+row*stride;
  auto *b=buffer.data()+loc.PlacedFootprint.Offset+row*f.RowPitch;
  assert(loc.PlacedFootprint.Offset+row*f.RowPitch+stride<=buffer.size());
  if(upload) memcpy(a,b,stride); else memcpy(b,a,stride);
 }
}
"""


class MesaViewCastTests(unittest.TestCase):
    setUp = safety.MesaRenderSafetyTests.setUp
    compile_run = safety.MesaRenderSafetyTests.compile_run

    def test_gpu_bytes_cache_filters_and_cleanup(self):
        self.compile_run(
            MOCK
            + DIAGNOSTICS
            + COPY
            + r"""
int main() {
 Device device; struct d3d12_screen screen; screen.dev=&device;
 Commands commands; d3d12_context ctx{{&screen},&commands};
 for(unsigned target:{PIPE_TEXTURE_2D_ARRAY,PIPE_TEXTURE_3D}) {
  for(auto formats:{std::pair<pipe_format,pipe_format>{F16,RG8},{F32,RGBA8},{RGBA8,F32}}) {
   pipe_resource templ; templ.target=target; templ.depth0=target==PIPE_TEXTURE_3D ? 5 : 1;
   templ.format=formats.first;
   auto *source=create_resource(&screen,&templ); auto *res=d3d12_resource(source);
   auto before=allocations;
   auto *shadow=nxbox_get_srv_shadow(&ctx,res,formats.second); assert(shadow);
   assert(allocations==before+2 && nxbox_get_srv_shadow(&ctx,res,formats.second)==shadow);
   auto *dest=d3d12_resource(nxbox_srv_shadow_texture(shadow));
   assert(dest->base.b.width0==templ.width0 && dest->base.b.height0==templ.height0);
   assert(dest->base.b.array_size==templ.array_size && dest->base.b.last_level==templ.last_level);
   for(unsigned iteration=0;iteration<2;++iteration) {
    for(auto &image:res->native->images)
     for(unsigned i=0;i<image.size();++i) image[i]=uint8_t(i*37+iteration*83);
    auto count=copies; nxbox_refresh_srv_shadow(&ctx,res,shadow);
    assert(copies-count==res->native->images.size()*2);
    assert(res->native->images==dest->native->images);
   }
   nxbox_destroy_srv_shadows(res); assert(!res->nxbox_srv_shadows);
   pipe_resource_reference(&source,nullptr); assert(live_resources==0);
  }
 }
 pipe_resource templ; auto *source=create_resource(&screen,&templ); auto *res=d3d12_resource(source);
 for(pipe_format f:{F32,BC,DEPTH,PLANAR}) assert(!nxbox_get_srv_shadow(&ctx,res,f));
 for(pipe_format f:{BC,DEPTH,PLANAR}) { res->overall_format=f; assert(!nxbox_get_srv_shadow(&ctx,res,RGBA8)); }
 res->overall_format=F16;
 res->base.b.nr_samples=4; assert(!nxbox_get_srv_shadow(&ctx,res,RG8)); res->base.b.nr_samples=0;
 res->base.b.nr_storage_samples=4; assert(!nxbox_get_srv_shadow(&ctx,res,RG8)); res->base.b.nr_storage_samples=0;
 res->base.b.target=PIPE_BUFFER; assert(!nxbox_get_srv_shadow(&ctx,res,RG8)); res->base.b.target=templ.target;
 fail_texture=true; assert(!nxbox_get_srv_shadow(&ctx,res,RG8)); fail_texture=false;
 fail_buffer=true; assert(!nxbox_get_srv_shadow(&ctx,res,RG8)); fail_buffer=false;
 assert(live_resources==1 && !res->nxbox_srv_shadows);
 pipe_resource_reference(&source,nullptr); assert(live_resources==0);
 templ.format=F32; source=create_resource(&screen,&templ); res=d3d12_resource(source);
 assert(!nxbox_get_srv_shadow(&ctx,res,EMULATED)); assert(live_resources==1);
 pipe_resource_reference(&source,nullptr);
 assert(commands.predication_changes==24);
}
"""
        )

    def test_switch_is_read_once_and_pairs_are_bounded(self):
        self.compile_run(
            MOCK.split("using UINT=", 1)[0]
            + DIAGNOSTICS
            + r"""
int main(int argc,char **argv) {
 assert(argc==2); setenv("NXBOX_VIEW_CAST_COPY",argv[1],1);
 bool enabled=strcmp(argv[1],"0")!=0;
 assert(nxbox_view_cast_copy_enabled()==enabled);
 setenv("NXBOX_VIEW_CAST_COPY",enabled ? "0" : "1",1);
 assert(nxbox_view_cast_copy_enabled()==enabled);
 nxbox_count_view_cast("srv",1,2); nxbox_count_view_cast("srv_copy",1,2);
 nxbox_count_view_cast("rtv",1,2); nxbox_count_view_cast("dsv",1,2); nxbox_count_view_cast("uav",1,2);
 assert(env["NXBOX_D3D12_VIEW_CAST"].find("srv=1 srv_copy=1 rtv=1 dsv=1 uav=1")!=std::string::npos);
 assert(env["NXBOX_D3D12_VIEW_CAST_PAIRS"]=="1->2");
 for(unsigned i=2;i<=20;++i) nxbox_count_view_cast("srv_copy",i,i+1);
 auto pairs=env["NXBOX_D3D12_VIEW_CAST_PAIRS"];
 assert(std::count(pairs.begin(),pairs.end(),' ')==15);
 assert(pairs.find("16->17")!=std::string::npos && pairs.find("17->18")==std::string::npos);
}
""",
            args=("0",),
        )
        subprocess.run([str(self.root / "test"), "1"], check=True, timeout=10)

    @unittest.skipUnless(SOURCE.is_dir(), "Set NXBOX_MESA_SRC or provide /tmp/mesa-pin")
    def test_chain_installs_shadow_and_preserves_fallback(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-view-chain-") as temporary:
            root = Path(temporary)
            shutil.copytree(SOURCE / "src", root / "src")
            environment = os.environ.copy()
            environment.pop("NXBOX_MESA_SKIP", None)
            subprocess.run(
                [sys.executable, str(ROOT / "tools/nxbox/patch_mesa_uwp.py"), str(root)],
                env=environment,
                capture_output=True,
                text=True,
                check=True,
                timeout=60,
            )
            driver = root / "src/gallium/drivers/d3d12"
            context = (driver / "d3d12_context.cpp").read_text()
            draw = (driver / "d3d12_draw.cpp").read_text()
            resource = (driver / "d3d12_resource.cpp").read_text()
            self.assertIn("sampler_view->nxbox_srv_shadow = nxbox_get_srv_shadow", context)
            self.assertIn("nxbox_srv_shadow_texture(sampler_view->nxbox_srv_shadow)", context)
            self.assertIn(
                "desc.Format = d3d12_get_resource_srv_format(res->overall_format", context
            )
            self.assertIn(
                "nxbox_refresh_srv_shadow(ctx, d3d12_resource(new_view->texture)", context
            )
            self.assertIn("d3d12_transition_subresources_state(ctx, res,", draw)
            for compute in ("false", "true"):
                self.assertLess(
                    draw.index(f"nxbox_refresh_bound_srv_shadows(ctx, {compute});"),
                    draw.index(f"if (!check_descriptors_left(ctx, {compute}))"),
                )
            self.assertIn("nxbox_destroy_srv_shadows(resource);", resource)
            self.assertIn('#include "nxbox_view_cast_copy.h"', resource)
            self.assertIn("d3d12_batch_reference_resource(batch, info.src, false)", resource)
            self.assertIn("d3d12_batch_reference_resource(batch, info.dst, true)", resource)
            self.assertIn("D3D12_RESOURCE_STATE_COPY_DEST", resource)
            self.assertIn("NXBOX_VIEW_CAST_COPY", (driver / "nxbox_view_cast.h").read_text())
            self.assertEqual(
                (driver / "nxbox_view_cast_copy.h").read_text(),
                (ROOT / "tools/nxbox/mesa_view_cast_copy.h").read_text(),
            )
            self.assertIn(
                'nxbox_count_view_cast(is_depth_or_stencil ? "dsv" : "rtv"',
                (driver / "d3d12_surface.cpp").read_text(),
            )
            self.assertIn('nxbox_count_view_cast("uav"', draw)
