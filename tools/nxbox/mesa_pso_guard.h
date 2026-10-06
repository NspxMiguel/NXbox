/* SPDX-License-Identifier: MIT */
#pragma once
#include <initializer_list>

/* D3D12CreateDevice is a singleton per adapter, not an isolated canary.
 * Independent DeviceFactory support/fault isolation is unverified on Xbox UWP.
 * Skip suspect draws before either creation API. This intentionally conservative
 * shape rule can suppress valid draws; NXBOX_PSO_GUARD=0 disables only the rule.
 * The two known device-removing pairs remain blocked independently.
 */
static bool nxbox_pso_guard_enabled() {
   static const bool enabled = [] {
      char value[4] = {};
      return !(GetEnvironmentVariableA("NXBOX_PSO_GUARD", value, sizeof(value)) == 1 &&
               value[0] == '0');
   }();
   return enabled;
}

template <typename Desc> static bool nxbox_pso_suspect_shape(const Desc &d) {
   if (!nxbox_pso_guard_enabled() || !d.VS.pShaderBytecode || !d.PS.pShaderBytecode ||
       d.VS.BytecodeLength < 2048 || d.VS.BytecodeLength > 2560 || d.PS.BytecodeLength < 2048 ||
       d.PS.BytecodeLength > 2304 || d.HS.BytecodeLength || d.DS.BytecodeLength ||
       d.GS.BytecodeLength || d.NumRenderTargets != 1 ||
       d.RTVFormats[0] != DXGI_FORMAT_R8G8B8A8_UNORM ||
       d.PrimitiveTopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE ||
       d.SampleDesc.Count != 1 || d.SampleDesc.Quality != 0 || d.InputLayout.NumElements < 1 ||
       d.InputLayout.NumElements > 2 || !d.InputLayout.pInputElementDescs)
      return false;
   NxboxInputSignature signature;
   if (!signature.parse(d.VS) || signature.count != d.InputLayout.NumElements)
      return false;
   for (uint32_t i = 0; i < signature.count; ++i) {
      const auto *e = signature.element(i);
      if (signature.word(e + 16) != 3 || signature.word(e + 28) != 0)
         return false;
   }
   unsigned indices = 0;
   for (UINT i = 0; i < d.InputLayout.NumElements; ++i) {
      const auto &e = d.InputLayout.pInputElementDescs[i];
      if (!signature.same_name(e.SemanticName, "TEXCOORD") ||
          e.SemanticIndex >= d.InputLayout.NumElements || (indices & (1u << e.SemanticIndex)) ||
          (e.Format != DXGI_FORMAT_R32G32B32A32_FLOAT && e.Format != DXGI_FORMAT_R32G32B32_FLOAT) ||
          e.InputSlotClass != D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA ||
          e.InstanceDataStepRate || !signature.declares(e))
         return false;
      indices |= 1u << e.SemanticIndex;
   }
   return true;
}

/* Normalize only ignored fields. Zero is not a legal comparison/stencil enum,
 * even though desktop runtimes commonly ignore it when the feature is disabled.
 * Do not strip active flags, rewrite resources, or invent shader inputs.
 */
template <typename Depth> static void nxbox_pso_fix_depth(Depth &depth) {
   if (!nxbox_pso_fix_enabled())
      return;
   if (!depth.DepthEnable)
      depth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
   if (!depth.StencilEnable) {
      for (auto face : {&depth.FrontFace, &depth.BackFace}) {
         face->StencilFailOp = face->StencilDepthFailOp = face->StencilPassOp =
             D3D12_STENCIL_OP_KEEP;
         face->StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
      }
   }
}
