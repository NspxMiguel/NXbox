# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute quarantine, ignored-state normalization, and loss capture on the host."""

import json
import tempfile
import unittest
from pathlib import Path

import test_mesa_render_safety as safety

ROOT = Path(__file__).resolve().parents[2]


class MesaPsoGuardTests(unittest.TestCase):
    compile_run = safety.MesaRenderSafetyTests.compile_run

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="nxbox-pso-guard-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_shapes_opt_out_and_disabled_state_normalization(self):
        capture = json.loads((ROOT / "tests/port/fixtures/botw-pipe6-dxil.json").read_text())
        arrays = "\n".join(
            f"unsigned char {stage.lower()}[] = {{{','.join(str(b) for b in bytes.fromhex(data))}}};"
            for stage, data in capture.items()
        )
        headers = "\n".join(
            (ROOT / f"tools/nxbox/mesa_pso_{name}.h").read_text().replace("#pragma once", "")
            for name in ("input", "guard")
        )
        code = (
            r"""
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
using UINT=unsigned;
struct D3D12_SHADER_BYTECODE { const void *pShaderBytecode; size_t BytecodeLength; };
struct D3D12_INPUT_ELEMENT_DESC {
 const char *SemanticName; UINT SemanticIndex, Format, InputSlot, AlignedByteOffset,
 InputSlotClass, InstanceDataStepRate;
};
struct D3D12_INPUT_LAYOUT_DESC { const D3D12_INPUT_ELEMENT_DESC *pInputElementDescs; UINT NumElements; };
constexpr UINT D3D12_APPEND_ALIGNED_ELEMENT=~0u;
constexpr int D3D12_BLEND_ONE=2, D3D12_BLEND_ZERO=1, D3D12_BLEND_OP_ADD=1, D3D12_LOGIC_OP_NOOP=4;
constexpr int DXGI_FORMAT_R8G8B8A8_UNORM=28, DXGI_FORMAT_R32G32B32A32_FLOAT=2,
 DXGI_FORMAT_R32G32B32_FLOAT=6, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE=3,
 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA=0, D3D12_COMPARISON_FUNC_ALWAYS=8,
 D3D12_STENCIL_OP_KEEP=1;
struct Target { bool BlendEnable, LogicOpEnable; int SrcBlend, SrcBlendAlpha, DestBlend,
 DestBlendAlpha, BlendOp, BlendOpAlpha, LogicOp; };
struct D3D12_BLEND_DESC { Target RenderTarget[8]; };
struct Desc {
 D3D12_SHADER_BYTECODE VS,PS,HS{},DS{},GS{};
 UINT NumRenderTargets=1, RTVFormats[8]={28}, PrimitiveTopologyType=3;
 struct { UINT Count=1, Quality=0; } SampleDesc;
 D3D12_INPUT_LAYOUT_DESC InputLayout;
};
unsigned GetEnvironmentVariableA(const char *name, char *out, unsigned size) {
 const char *value=getenv(name); if (!value) return 0;
 const unsigned n=strlen(value); if(n<size) memcpy(out,value,n+1); return n;
}
void SetEnvironmentVariableA(const char *,const char *) {}
"""
            + headers
            + arrays
            + r"""
int main(int argc, char**) {
 if(argc>1) setenv("NXBOX_PSO_GUARD","0",1);
 D3D12_INPUT_ELEMENT_DESC inputs[]={{"TEXCOORD",0,2,0,0,0,0},{"TEXCOORD",1,2,0,16,0,0}};
 Desc d{}; d.VS={vs,sizeof(vs)}; d.PS={ps,sizeof(ps)}; d.InputLayout={inputs,2};
 assert(nxbox_pso_quarantined(d.VS,d.PS));
 assert(nxbox_pso_suspect_shape(d)==(argc==1));
 if(argc>1) return 0;
 // An unseen shader hash with the captured layout is quarantined by shape.
 ps[4]^=1; assert(!nxbox_pso_quarantined(d.VS,d.PS)); assert(nxbox_pso_suspect_shape(d));
 d.RTVFormats[0]=29; assert(!nxbox_pso_suspect_shape(d)); d.RTVFormats[0]=28;
 d.PrimitiveTopologyType=2; assert(!nxbox_pso_suspect_shape(d)); d.PrimitiveTopologyType=3;
 d.GS.BytecodeLength=1; assert(!nxbox_pso_suspect_shape(d)); d.GS.BytecodeLength=0;
 d.PS.BytecodeLength=2305; assert(!nxbox_pso_suspect_shape(d)); d.PS.BytecodeLength=2192;
 inputs[1].SemanticIndex=0; assert(!nxbox_pso_suspect_shape(d)); inputs[1].SemanticIndex=1;
 inputs[0].Format=42; assert(!nxbox_pso_suspect_shape(d)); inputs[0].Format=2;
 d.InputLayout.pInputElementDescs=nullptr; assert(!nxbox_pso_suspect_shape(d));
 // Synthetic MK8D-shaped VS: only the ISG1 is fabricated, never presented as captured DXIL.
 unsigned char mk8[2304]{}; memcpy(mk8,"DXBC",4);
 auto put=[&](unsigned off,uint32_t v){memcpy(mk8+off,&v,4);};
 put(24,sizeof(mk8)); put(28,1); put(32,36); memcpy(mk8+36,"ISG1",4); put(40,52);
 put(44,1); put(48,8); put(56,40); put(68,3); mk8[76]=15; mk8[77]=1;
 memcpy(mk8+84,"TEXCOORD",9);
 d.VS={mk8,sizeof(mk8)}; d.PS.BytecodeLength=2212; inputs[0].Format=6;
 d.InputLayout={inputs,1}; assert(nxbox_pso_suspect_shape(d));
 inputs[0].InstanceDataStepRate=1; assert(!nxbox_pso_suspect_shape(d));
 struct Face {int StencilFailOp=0, StencilDepthFailOp=0, StencilPassOp=0, StencilFunc=0;} ;
 struct Depth {bool DepthEnable=false, StencilEnable=false; int DepthFunc=0; Face FrontFace,BackFace;} z;
 nxbox_pso_fix_depth(z); assert(z.DepthFunc==8 && z.FrontFace.StencilFunc==8 && z.BackFace.StencilPassOp==1);
 z.DepthEnable=z.StencilEnable=true; z.DepthFunc=3; z.FrontFace.StencilPassOp=7;
 nxbox_pso_fix_depth(z); assert(z.DepthFunc==3 && z.FrontFace.StencilPassOp==7);
}
"""
        )
        self.compile_run(code)
        self.compile_run(code, ("disabled",))

    def test_removed_before_or_after_success_captures_before_loss_publication(self):
        header = (
            (ROOT / "tools/nxbox/mesa_lifetime.h")
            .read_text()
            .split("#ifdef NXBOX_DRED_IMPLEMENTATION")[0]
        )
        self.compile_run(
            r"""
#include <cassert>
#include <cstdint>
using HRESULT=int32_t;
constexpr HRESULT S_OK=0;
#define FAILED(x) ((x)<0)
#define SUCCEEDED(x) ((x)>=0)
struct ID3D12Object {}; struct ID3D12PipelineState {};
struct ID3D12Device { HRESULT status=0; HRESULT GetDeviceRemovedReason(){return status;} };
"""
            + header.replace("#pragma once", "")
            + r"""
std::atomic<unsigned long long> nxbox_pso_calls{0};
std::atomic<unsigned> nxbox_pso_active{0};
bool reported=false; int calls=0;
void nxbox_pso_sample(ID3D12Device*,const char*,HRESULT,HRESULT,HRESULT,unsigned long long,unsigned) {
 assert(reported);
}
int main() {
 ID3D12Device dev;
 auto report=[&](HRESULT before,HRESULT hr,HRESULT after){
   assert(before<0 || hr<0 || after<0); reported=true;
 };
 dev.status=-1;
 auto result=nxbox_create_pso(&dev,"gfx-pso",[&](){++calls;return S_OK;},report);
 assert(result==-1 && calls==0 && reported && nxbox_pso_active==0);
 dev.status=0; reported=false;
 result=nxbox_create_pso(&dev,"gfx-pso",[&](){++calls;dev.status=-2;return S_OK;},report);
 assert(result==-2 && calls==1 && reported && nxbox_pso_active==0);
 dev.status=0; reported=false;
 result=nxbox_create_pso(&dev,"gfx-pso",[&](){++calls;return HRESULT(-3);},report);
 assert(result==-3 && calls==2 && reported);
}
"""
        )

    def test_capture_priority_and_manifest_publication(self):
        # Execute the producer through publication, before the unrelated desc formatting.
        producer = (
            (ROOT / "tools/nxbox/mesa_pso_first.h").read_text().split("   std::string text;", 1)[0]
        )
        producer = producer.replace("#pragma once", "") + "(void)unfixed;\n}\n"
        self.compile_run(
            r"""
#include <cassert>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <cstdint>
using HRESULT=int32_t;
constexpr HRESULT S_FALSE=1;
struct D3D12_SHADER_BYTECODE {const void* pShaderBytecode; size_t BytecodeLength;};
struct D3D12_INPUT_LAYOUT_DESC {};
struct Desc {D3D12_SHADER_BYTECODE VS,PS;};
struct CD3DX12_PIPELINE_STATE_STREAM3 {Desc d; Desc GraphicsDescV0() const {return d;}};
std::mutex nxbox_pso_report_mutex;
std::map<std::string,std::string> env;
bool publishing=false;
bool SetEnvironmentVariableA(const char *name,const char *value) {
 if (!strcmp(name,"NXBOX_D3D12_PSO_DXIL")) {
   if (!value) {publishing=true;env.erase(name);return true;}
   assert(publishing && env.count("NXBOX_D3D12_PSO_DXIL_VS_0")); publishing=false;
 } else {assert(publishing);}
 env[name]=value; return true;
}
bool nxbox_pso_quarantined(const D3D12_SHADER_BYTECODE& v,const D3D12_SHADER_BYTECODE&) {
 return *static_cast<const unsigned char*>(v.pShaderBytecode)==1;
}
"""
            + producer
            + r"""
int main() {
 unsigned char vs[2]={2,3},ps[2]={4,5};
 CD3DX12_PIPELINE_STATE_STREAM3 stream{{{vs,2},{ps,2}}};
 nxbox_pso_first(stream,S_FALSE,{});
 assert(env["NXBOX_D3D12_PSO_DXIL"].find("complete=1 capture=1")!=std::string::npos);
 vs[0]=1; nxbox_pso_first(stream,S_FALSE,{});
 assert(env["NXBOX_D3D12_PSO_DXIL_VS_0"]=="0103");
 assert(env["NXBOX_D3D12_PSO_DXIL"].find("capture=2")!=std::string::npos);
 vs[0]=6; nxbox_pso_first(stream,-1,{});
 assert(env["NXBOX_D3D12_PSO_DXIL_VS_0"]=="0603");
 assert(env["NXBOX_D3D12_PSO_DXIL"].find("capture=3")!=std::string::npos);
 vs[0]=1; nxbox_pso_first(stream,S_FALSE,{}); nxbox_pso_first(stream,-2,{});
 assert(env["NXBOX_D3D12_PSO_DXIL_VS_0"]=="0603" && !publishing);
}
"""
        )

    def test_terminal_dxil_drain_multiple_chunks_and_missing_part(self):
        header = (ROOT / "src/eden_uwp/diagnostic_report.h").read_text().replace("#pragma once", "")
        self.compile_run(
            r"""
#include <cassert>
#include <map>
"""
            + header
            + r"""
int main() {
 std::map<std::string,std::string> env{
 {"NXBOX_D3D12_PSO_DXIL","vs_bytes=4096 vs_parts=2 ps_bytes=2 ps_parts=1 chunk_bytes=4095 complete=1"},
 {"NXBOX_D3D12_PSO_DXIL_VS_0",std::string(8190,'a')},
 {"NXBOX_D3D12_PSO_DXIL_VS_1","bb"}, {"NXBOX_D3D12_PSO_DXIL_PS_0","cdef"}};
 std::map<std::string,std::string> emitted;
 auto read=[&](const char* name){return env[name];};
 auto emit=[&](const char* name,const std::string& value){emitted[name]=value;};
 EdenXbox::CollectPsoDxil(read,emit); assert(emitted==env);
 env.erase("NXBOX_D3D12_PSO_DXIL_VS_1"); emitted.clear();
 EdenXbox::CollectPsoDxil(read,emit);
 assert(emitted.count("NXBOX_D3D12_PSO_DXIL_ERROR"));
 assert(!emitted.count("NXBOX_D3D12_PSO_DXIL_VS_1"));
 env["NXBOX_D3D12_PSO_DXIL_VS_1"]="bb"; emitted.clear();
 auto racing_read=[&](const char* name) {
   if (std::string(name)=="NXBOX_D3D12_PSO_DXIL_VS_0") env["NXBOX_D3D12_PSO_DXIL"]+=" capture=2";
   return env[name];
 };
 EdenXbox::CollectPsoDxil(racing_read,emit); assert(emitted.empty());
}
"""
        )
        session = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
        terminal = session.split("device_lost[0] == '1'")[1].split("throw std::runtime_error")[0]
        self.assertIn("CollectPsoDxil(read_report, log_report)", terminal)


if __name__ == "__main__":
    unittest.main()
