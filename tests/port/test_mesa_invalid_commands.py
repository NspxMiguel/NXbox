# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute pinned Mesa promotion and copy paths with host D3D12 mocks."""

import shutil
import unittest

import test_mesa_render_safety as safety
from test_mesa_full_chain import SOURCE


@unittest.skipUnless(SOURCE.is_dir(), "Set NXBOX_MESA_SRC or provide /tmp/mesa-pin")
class MesaInvalidCommandTests(unittest.TestCase):
    compile_run = safety.MesaRenderSafetyTests.compile_run

    def setUp(self):
        safety.MesaRenderSafetyTests.setUp(self)
        for name in ("d3d12_resource_state.cpp", "d3d12_blit.cpp", "d3d12_draw.cpp"):
            shutil.copyfile(SOURCE / safety.DRIVER / name, self.driver / name)
        safety.PATCH.patch_invalid_commands(self.root)

    def test_read_promotions_require_a_barrier_before_writes(self):
        source = (self.driver / "d3d12_resource_state.cpp").read_text()
        promote = source.split("static D3D12_RESOURCE_STATES\nresource_state_if_promoted", 1)[
            1
        ].split("static void\ncopy_resource_state", 1)[0]
        self.compile_run(
            r"""
#include <cassert>
using D3D12_RESOURCE_STATES=unsigned;
constexpr unsigned D3D12_RESOURCE_STATE_COMMON=0, D3D12_RESOURCE_STATE_GENERIC_READ=3;
bool d3d12_is_write_state(unsigned s) { return (s & 12)!=0; }
struct d3d12_subresource_state { unsigned state; bool is_promoted; };
D3D12_RESOURCE_STATES resource_state_if_promoted
"""
            + promote
            + r"""
int main() {
 d3d12_subresource_state state{1,true};
 assert(resource_state_if_promoted(2,true,&state)==3);
 assert(resource_state_if_promoted(4,true,&state)==0);
 assert(resource_state_if_promoted(8,true,&state)==0);
 assert(resource_state_if_promoted(2,false,&state)==0);
 state={4,true}; assert(resource_state_if_promoted(1,true,&state)==0);
 state={0,false}; assert(resource_state_if_promoted(4,true,&state)==4);
 assert(resource_state_if_promoted(1,true,&state)==1);
}
"""
        )
        self.assertNotIn("else if (after != state_if_promoted)", source)

    def test_copy_planes_layers_and_full_subresource_box(self):
        source = (self.driver / "d3d12_blit.cpp").read_text()
        subresource = source.split("inline static unsigned\nget_subresource_id", 1)[1].split(
            "static void\ncopy_subregion", 1
        )[0]
        copy = source.split("static void\ncopy_subregion_no_barriers", 1)[1].split(
            "static void\ncopy_resource_y_flipped", 1
        )[0]
        self.compile_run(
            r"""
#include <cassert>
#include <vector>
#include <algorithm>
#define UNUSED
#define MIN2(a,b) std::min(a,b)
constexpr unsigned PIPE_MASK_S=0x20, PIPE_MASK_Z=0x10;
constexpr int PIPE_FORMAT_Z24_UNORM_S8_UINT=1, PIPE_FORMAT_S8_UINT_Z24_UNORM=2,
 PIPE_FORMAT_Z32_FLOAT_S8X24_UINT=3, PIPE_FORMAT_Z32_FLOAT=4;
constexpr int D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX=0;
constexpr int D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED=0;
enum pipe_texture_target { TEXTURE_2D, TEXTURE_ARRAY, TEXTURE_3D };
bool d3d12_subresource_id_uses_layer(pipe_texture_target t) {return t==TEXTURE_ARRAY;}
bool util_format_is_depth_or_stencil(int) { return true; }
unsigned u_minify(unsigned n,unsigned l) {return std::max(n>>l,1u);}
struct pipe_box { int x,y,z,width,height,depth; };
struct resource { pipe_texture_target target=TEXTURE_ARRAY; int format=3;
 unsigned last_level=3, array_size=8, width0=64, height0=64, depth0=1, nr_samples=1; };
struct d3d12_resource { struct { resource b; } base; unsigned plane_slice=0; };
d3d12_resource *d3d12_resource_resource(d3d12_resource *r) { return r; }
struct D3D12_BOX { unsigned left,right,top,bottom,front,back; };
struct D3D12_TEXTURE_COPY_LOCATION { int Type; unsigned SubresourceIndex; d3d12_resource *pResource; };
struct CommandList {
 struct Copy { unsigned src,dst; bool full; };
 std::vector<Copy> copies;
 void CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION *dst, unsigned x,unsigned y,unsigned z,
                        const D3D12_TEXTURE_COPY_LOCATION *src,const D3D12_BOX *box) {
   assert(x==0 && y==0 && z==0);
   copies.push_back({src->SubresourceIndex,dst->SubresourceIndex,box==nullptr});
 }
};
struct d3d12_screen { struct { int ProgrammableSamplePositionsTier=0; } opts2; };
struct d3d12_context { struct { struct d3d12_screen *screen; } base; CommandList *cmdlist; };
struct d3d12_screen *d3d12_screen(struct d3d12_screen *p) {return p;}
unsigned get_subresource_id
"""
            + subresource
            + "void copy_subregion_no_barriers"
            + copy
            + r"""
int main() {
 struct d3d12_screen screen; CommandList commands; d3d12_context ctx{{&screen},&commands};
 d3d12_resource src,dst; pipe_box box{0,0,3,32,32,1};
 copy_subregion_no_barriers(&ctx,&dst,1,0,0,5,&src,1,&box,PIPE_MASK_S|PIPE_MASK_Z);
 assert(commands.copies.size()==2);
 assert(commands.copies[0].src==13 && commands.copies[0].dst==21 && commands.copies[0].full);
 assert(commands.copies[1].src==45 && commands.copies[1].dst==53 && commands.copies[1].full);
 commands.copies.clear(); src.base.b.format=PIPE_FORMAT_Z32_FLOAT;
 copy_subregion_no_barriers(&ctx,&dst,1,0,0,5,&src,1,&box,PIPE_MASK_Z);
 assert(commands.copies.size()==1 && commands.copies[0].src==13 && commands.copies[0].dst==21);
}
"""
        )

    def test_descriptor_table_capacity(self):
        source = (self.driver / "d3d12_draw.cpp").read_text()
        capacity = source.split("#define MAX_DESCRIPTOR_TABLES ", 1)[1].splitlines()[0]
        self.compile_run(
            "#include <cassert>\n#define D3D12_GFX_SHADER_STAGES 5\n"
            f"#define MAX_DESCRIPTOR_TABLES {capacity}\n"
            + r"""
int main() {
 unsigned tables[MAX_DESCRIPTOR_TABLES]={}; unsigned count=0;
 for(unsigned stage=0;stage<D3D12_GFX_SHADER_STAGES;++stage)
   for(unsigned binding=0;binding<5;++binding) { assert(count<MAX_DESCRIPTOR_TABLES); tables[count++]=binding; }
 assert(count==25 && tables[24]==4);
}
"""
        )

    def test_anchor_failure_does_not_write(self):
        before = {p: p.read_bytes() for p in self.driver.iterdir()}
        with self.assertRaisesRegex(RuntimeError, "anchor mismatch"):
            safety.PATCH.patch_invalid_commands(self.root)
        self.assertEqual(before, {p: p.read_bytes() for p in self.driver.iterdir()})

    def test_depth_copy_requires_compatible_dxgi_family(self):
        source = (self.driver / "d3d12_blit.cpp").read_text()
        compatible = source.split("static bool\nformats_are_copy_compatible", 1)[1].split(
            "static bool\nbox_fits", 1
        )[0]
        self.compile_run(
            r"""
#include <cassert>
enum pipe_format { Z24S8, Z24, Z32S8, Z32, RGBA };
pipe_format util_format_get_depth_only(pipe_format f) {return f==Z24S8 ? Z24 : f==Z32S8 ? Z32 : f;}
unsigned d3d12_get_typeless_format(pipe_format f) {return f==Z24S8 || f==Z24 ? 44 : f==Z32S8 ? 19 : f==Z32 ? 39 : 27;}
bool formats_are_copy_compatible
"""
            + compatible
            + r"""
int main() {
 assert(formats_are_copy_compatible(Z24S8,Z24));
 assert(formats_are_copy_compatible(Z24,Z24S8));
 assert(!formats_are_copy_compatible(Z32S8,Z32));
 assert(!formats_are_copy_compatible(Z32,Z32S8));
 assert(formats_are_copy_compatible(Z32S8,Z32S8));
 assert(!formats_are_copy_compatible(RGBA,Z32));
}
"""
        )

    def test_array_copy_splits_before_transitions(self):
        source = (self.driver / "d3d12_blit.cpp").read_text()
        split = source.split("   if (psrc_box->depth > 1 &&", 1)[1].split(
            "   struct d3d12_batch *batch", 1
        )[0]
        self.compile_run(
            r"""
#include <cassert>
#include <vector>
struct pipe_box { int x,y,z,width,height,depth; };
struct d3d12_resource { struct { struct { bool target; } b; } base; };
struct d3d12_context {};
bool d3d12_subresource_id_uses_layer(bool t) {return t;}
struct Copy { int src,dst,depth; };
std::vector<Copy> copies;
void d3d12_direct_copy(d3d12_context *ctx, d3d12_resource *dst,unsigned dst_level,
                       const pipe_box *pdst_box, d3d12_resource *src,unsigned src_level,
                       const pipe_box *psrc_box,unsigned mask) {
   if (psrc_box->depth > 1 &&
"""
            + split
            + r"""
 copies.push_back({psrc_box->z,pdst_box->z,psrc_box->depth});
}
int main() {
 d3d12_context ctx; d3d12_resource src{{{true}}},dst{{{true}}};
 pipe_box s{0,0,2,32,32,3},d{0,0,4,32,32,3};
 d3d12_direct_copy(&ctx,&dst,1,&d,&src,1,&s,48);
 assert(copies.size()==3);
 for(unsigned i=0;i<3;++i) assert(copies[i].src==int(2+i) && copies[i].dst==int(4+i) && copies[i].depth==1);
 copies.clear(); src.base.b.target=dst.base.b.target=false;
 d3d12_direct_copy(&ctx,&dst,1,&d,&src,1,&s,48);
 assert(copies.size()==1 && copies[0].depth==3);
}
"""
        )
