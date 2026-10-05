/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdarg.h>
#include <string>

/* First rejection only. No shader payloads: lengths and stable content hashes identify them. */
static void nxbox_pso_first(const CD3DX12_PIPELINE_STATE_STREAM3 &stream, HRESULT hr) {
   std::lock_guard<std::mutex> lock(nxbox_pso_report_mutex);
   static bool captured = false;
   if (captured)
      return;
   captured = true;
   const auto d = stream.GraphicsDescV0();
   std::string text;
   auto add = [&](const char *format, ...) {
      char part[1024];
      va_list args;
      va_start(args, format);
      vsnprintf(part, sizeof(part), format, args);
      va_end(args);
      text += part;
   };
   add("hr=%08x root=%p mask=%08x topology=%u cut=%u node=%u flags=%u cached=%zu rt_count=%u "
       "dsv=%u samples=%u/%u atc=%d independent=%d ", (unsigned)hr, (void *)d.pRootSignature,
       d.SampleMask, d.PrimitiveTopologyType, d.IBStripCutValue, d.NodeMask, d.Flags,
       d.CachedPSO.CachedBlobSizeInBytes, d.NumRenderTargets, d.DSVFormat,
       d.SampleDesc.Count, d.SampleDesc.Quality, d.BlendState.AlphaToCoverageEnable,
       d.BlendState.IndependentBlendEnable);
   const D3D12_SHADER_BYTECODE shaders[] = {d.VS, d.HS, d.DS, d.GS, d.PS};
   const char *names[] = {"vs", "hs", "ds", "gs", "ps"};
   for (unsigned i = 0; i < 5; ++i) {
      uint64_t hash = 14695981039346656037ull;
      auto bytes = (const unsigned char *)shaders[i].pShaderBytecode;
      for (size_t j = 0; j < shaders[i].BytecodeLength; ++j)
         hash = (hash ^ bytes[j]) * 1099511628211ull;
      add("%s=%zu/%016llx ", names[i], shaders[i].BytecodeLength, (unsigned long long)hash);
   }
   for (unsigned i = 0; i < 8; ++i) {
      const auto &b = d.BlendState.RenderTarget[i];
      add("rt%u={fmt=%u blend=%d logic=%d rgb=%u/%u/%u alpha=%u/%u/%u op=%u write=%u} ",
          i, d.RTVFormats[i], b.BlendEnable, b.LogicOpEnable, b.SrcBlend, b.DestBlend,
          b.BlendOp, b.SrcBlendAlpha, b.DestBlendAlpha, b.BlendOpAlpha, b.LogicOp,
          b.RenderTargetWriteMask);
   }
   const auto &r = d.RasterizerState;
   add("raster={fill=%u cull=%u ccw=%d bias=%d clamp=%.9g slope=%.9g clip=%d ms=%d aa=%d forced=%u conservative=%u} ",
       r.FillMode, r.CullMode, r.FrontCounterClockwise, r.DepthBias, r.DepthBiasClamp,
       r.SlopeScaledDepthBias, r.DepthClipEnable, r.MultisampleEnable,
       r.AntialiasedLineEnable, r.ForcedSampleCount, r.ConservativeRaster);
   const auto &z = (const D3D12_DEPTH_STENCIL_DESC2 &)stream.DepthStencilState;
   add("depth={enable=%d write=%u func=%u bounds=%d stencil=%d} ", z.DepthEnable,
       z.DepthWriteMask, z.DepthFunc, z.DepthBoundsTestEnable, z.StencilEnable);
   const D3D12_DEPTH_STENCILOP_DESC1 faces[] = {z.FrontFace, z.BackFace};
   for (unsigned i = 0; i < 2; ++i) {
      const auto &f = faces[i];
      add("face%u={fail=%u zfail=%u pass=%u func=%u read=%u write=%u} ", i, f.StencilFailOp,
          f.StencilDepthFailOp, f.StencilPassOp, f.StencilFunc, f.StencilReadMask, f.StencilWriteMask);
   }
   add("inputs=%u ", d.InputLayout.NumElements);
   for (unsigned i = 0; i < d.InputLayout.NumElements; ++i) {
      const auto &e = d.InputLayout.pInputElementDescs[i];
      add("in%u={%s idx=%u fmt=%u slot=%u offset=%u class=%u step=%u} ", i,
          e.SemanticName ? e.SemanticName : "null", e.SemanticIndex, e.Format,
          e.InputSlot, e.AlignedByteOffset, e.InputSlotClass, e.InstanceDataStepRate);
   }
   add("so=%u/%u raster_stream=%u ", d.StreamOutput.NumEntries, d.StreamOutput.NumStrides,
       d.StreamOutput.RasterizedStream);
   for (unsigned i = 0; i < d.StreamOutput.NumEntries; ++i) {
      const auto &e = d.StreamOutput.pSODeclaration[i];
      add("so%u={stream=%u %s idx=%u start=%u count=%u slot=%u} ", i, e.Stream,
          e.SemanticName ? e.SemanticName : "null", e.SemanticIndex, e.StartComponent,
          e.ComponentCount, e.OutputSlot);
   }
   for (unsigned i = 0; i < d.StreamOutput.NumStrides; ++i)
      add("stride%u=%u ", i, d.StreamOutput.pBufferStrides[i]);
   const auto &v = (const D3D12_VIEW_INSTANCING_DESC &)stream.ViewInstancingDesc;
   add("views=%u flags=%u ", v.ViewInstanceCount, v.Flags);
   for (unsigned i = 0; i < v.ViewInstanceCount; ++i)
      add("view%u=%u/%u ", i, v.pViewInstanceLocations[i].ViewportArrayIndex,
          v.pViewInstanceLocations[i].RenderTargetArrayIndex);
   SetEnvironmentVariableA("NXBOX_D3D12_PSO_FAIL_FIRST", text.c_str());
}
