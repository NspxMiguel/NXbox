// SPDX-License-Identifier: GPL-3.0-or-later
#include <wsl/winadapter.h>
#define D3D12_IGNORE_SDK_LAYERS
#include <directx/d3d12.h>
#include <directx/d3d12video.h>
unsigned GetEnvironmentVariableA(const char *, char *, unsigned);
int SetEnvironmentVariableA(const char *, const char *);
unsigned long long GetTickCount64();
unsigned long GetCurrentThreadId();
void Sleep(unsigned);
#include "nxbox_api_ring.h"
#include "nxbox_sync_batch.h"
void check_bc_copy(ID3D12GraphicsCommandList *list,
                   D3D12_TEXTURE_COPY_LOCATION *locations) {
  D3D12_BOX tail{0, 0, 0, 2, 2, 1};
  nxbox_journal_commands(nullptr, list)
      .CopyTextureRegion(locations, 0, 0, 0, locations + 1, &tail);
}

void check(ID3D12GraphicsCommandList *list, ID3D12Device *dev,
           ID3D12Resource *res, ID3D12DescriptorHeap **heaps,
           D3D12_RESOURCE_BARRIER *barriers, D3D12_VIEWPORT *views,
           D3D12_RECT *rects, D3D12_CPU_DESCRIPTOR_HANDLE *handles,
           D3D12_TEXTURE_COPY_LOCATION *locations,
           ID3D12CommandAllocator *allocator, ID3D12QueryHeap *queries) {
  auto api = nxbox_api(list, "test:1");
  api.ResourceBarrier(2, barriers);
  api.SetDescriptorHeaps(2, heaps);
  api.RSSetViewports(2, views);
  api.RSSetScissorRects(2, rects);
  api.OMSetRenderTargets(2, handles, false, handles + 2);
  api.CopyTextureRegion(locations, 0, 0, 0, locations + 1, nullptr);
  api.SetGraphicsRoot32BitConstants(0, 4, static_cast<void *>(rects), 0);
  api.DrawIndexedInstanced(3, 1, 0, 0, 0);
  api.ResolveQueryData(queries, D3D12_QUERY_TYPE_TIMESTAMP, 0, 1, res, 0);
  api.Close();
  api.Reset(allocator, nullptr);
  nxbox_api(dev, "test:2")
      .CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                         IID_PPV_ARGS(&list));
}

void check_more(ID3D12GraphicsCommandList8 *list, ID3D12Device *dev,
                ID3D12DescriptorHeap *heap, ID3D12CommandSignature *signature,
                ID3D12Resource *res, ID3D12RootSignature *root,
                ID3D12PipelineState *pso) {
  auto api = nxbox_api(list, "extended:1");
  D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
  D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
  D3D12_DESCRIPTOR_HEAP_DESC desc{};
  nxbox_api(dev, "heap:1").CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap));
  api.SetGraphicsRootSignature(root);
  api.SetComputeRootSignature(root);
  api.SetPipelineState(pso);
  api.SetGraphicsRootDescriptorTable(0, gpu);
  api.SetComputeRootDescriptorTable(0, gpu);
  api.SetGraphicsRoot32BitConstant(0, 0, 0);
  api.SetComputeRoot32BitConstant(0, 0, 0);
  api.SetGraphicsRootConstantBufferView(0, 0);
  api.SetComputeRootShaderResourceView(0, 0);
  api.SetComputeRootUnorderedAccessView(0, 0);
  api.SetComputeRoot32BitConstants(0, 0, nullptr, 0);
  api.IASetIndexBuffer(nullptr);
  api.IASetVertexBuffers(0, 0, nullptr);
  api.SOSetTargets(0, 0, nullptr);
  api.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  api.OMSetStencilRef(0);
  api.OMSetFrontAndBackStencilRef(0, 0);
  float color[4]{};
  api.OMSetBlendFactor(color);
  api.ClearRenderTargetView(cpu, color, 0, nullptr);
  api.ClearUnorderedAccessViewFloat(gpu, cpu, res, color, 0, nullptr);
  UINT values[4]{};
  api.ClearUnorderedAccessViewUint(gpu, cpu, res, values, 0, nullptr);
  D3D12_DISCARD_REGION discard{};
  api.DiscardResource(res, &discard);
  nxbox_api(dev, "view:1").CreateRenderTargetView(res, nullptr, cpu);
  nxbox_api(dev, "view:2").CreateDepthStencilView(res, nullptr, cpu);
  api.ClearDepthStencilView(cpu, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
  api.BeginQuery(nullptr, D3D12_QUERY_TYPE_OCCLUSION, 0);
  api.EndQuery(nullptr, D3D12_QUERY_TYPE_OCCLUSION, 0);
  api.ExecuteIndirect(signature, 0, res, 0, res, 0);
  api.SetPredication(res, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  api.ResolveSubresource(res, 0, res, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
  api.CopyBufferRegion(res, 0, res, 0, 16);
  api.CopyResource(res, res);
  api.DrawInstanced(1, 1, 0, 0);
  api.Dispatch(1, 1, 1);
  D3D12_WRITEBUFFERIMMEDIATE_PARAMETER immediate{};
  api.WriteBufferImmediate(1, &immediate, nullptr);
}
void check_video(ID3D12VideoDecodeCommandList2 *decode,
                 ID3D12VideoProcessCommandList1 *process,
                 ID3D12VideoEncodeCommandList2 *encode) {
  nxbox_api(decode, "video:1").DecodeFrame1(nullptr, nullptr, nullptr);
  nxbox_api(process, "video:2").ProcessFrames1(nullptr, nullptr, 0, nullptr);
  nxbox_api(encode, "video:3").EncodeFrame(nullptr, nullptr, nullptr, nullptr);
  nxbox_api(encode, "video:4").ResolveEncoderOutputMetadata(nullptr, nullptr);
  nxbox_api(decode, "video:5").Close();
  nxbox_api(process, "video:6").Reset(nullptr);
  nxbox_api(encode, "video:7").ResourceBarrier(0, nullptr);
}
