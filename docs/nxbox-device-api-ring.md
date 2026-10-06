# Device API ring audit (pkg 0.3.264 follow-up)

The supplied BotW and MK8D captures both report `0x887a0001` at `gfx-pso`
after a checked-good submission (`completed=1 attribution=delayed-after-checked-submit`).
This supports checking other API boundaries; it does not establish which call caused removal.

## Capture and next-run grep

`NXBOX_SYNC_BATCH` defaults on. `NXBOX_SYNC_BATCH=0` disables API recording and
post-call removal probes; copy bounds protection remains active. Every instrumented
call preserves its HRESULT/void return. Device children resolve their owning device
with `GetDevice`; video device interfaces use `QueryInterface`. Temporary references
are released immediately. No object references are retained by the ring.

The process-wide ring records 256 recent completions across workers and queues:
sequence, monotonic `GetTickCount64()` milliseconds, thread ID, device, object,
API name, source site, arguments, HRESULT, and post-call removal reason. Void calls
have `hr=0`; `removed` is the result of the removal probe. Resource descriptions
include dimensions, format, flags, heap properties and initial state/layout;
descriptor copies include both total counts. Numeric enum values are unchanged.

The first non-S_OK observation freezes publication. Slots use lock-free atomics,
including payload bytes. Producers never wait for another producer. A preempted
writer or wraparound collision is explicitly reported as
`unavailable=in-progress-or-overwritten` and counted in `gaps`; the winning failure
line is always retained independently. This avoids torn records or blocking a
healthy worker. First-observed completion order is not proof of GPU causation.
The ring is intentionally process-scoped: restart the app for another capture.

All chunks are published before the manifest and before `NXBOX_D3D12_DEVICE_LOST`.
A competing loss observer waits for publication before notifying the frontend.
The frontend drains every chunk on periodic polling, before throwing on loss,
before main-process shutdown, and around final graphics teardown. `Diagnostic`
closes its output stream per physical line. Repeated terminal drains may duplicate
the immutable capture; they do not truncate it.

```sh
rg '^D3D12_API_(FIRST|RING)( |_)' next-diag.txt
rg '^D3D12_CONTEXT ' next-diag.txt
rg '^D3D12_(SYNC_ERROR|FIRST_BAD_BATCH|PREVIOUS_GOOD_BATCH|BATCH_JOURNAL|DRED)' next-diag.txt
```

Expected line forms (values are examples, not new console evidence):

```text
D3D12_API_RING parts=256 first_seq=300 first_call=CreateCommittedResource removed=0x887a0001 gaps=0 attribution=first-observed-after-call
D3D12_API_FIRST seq=300 ms=... tid=... dev=... call=CreateCommittedResource site=d3d12_resource.cpp:... hr=0x... removed=0x887a0001 object=... args=...
D3D12_API_RING seq=299 ms=... tid=... dev=... call=Signal site=d3d12_fence.cpp:... hr=0x00000000 removed=0x00000000 ...
D3D12_CONTEXT tid=... context=... role=render-worker current=1
```

A `call=gfx-pso ... phase=before` FIRST record means removal was already visible
before PSO creation; that creation is skipped. `call=observation` identifies a
pre-existing observer (query wait, fence polling, etc.), not a newly executed API.

## Paths and safety findings

- Graphics has one ExecuteCommandLists site in `d3d12_batch.cpp`: its array includes
  fixup first, main second. The ring probes immediately on return, before Signal,
  completion polling, journal completion marking, and fence allocation.
- `d3d12_context_flush`, screen resource helpers, buffer/texture transfer and
  readback, blits and query resolves all feed that graphics batch. There is no
  separate screen/resource/query ExecuteCommandLists site in this pin.
- Video decode, encode and processing each have their own queue and submission
  site, cross-queue Wait/Signal calls, and fence events; all are instrumented even
  though the Xbox GL game path does not normally use Gallium video.
- No CreateHeap, CopyDescriptorsSimple, or production ExecuteBundle call exists
  in the pristine driver. Wrapper support is present, including the journal's
  ExecuteBundle forwarder. Imported placed resources and all committed-resource
  variants are covered.
- `SetDescriptorHeaps` binds exactly the batch's view and sampler heaps. The
  earlier patch already expands root-table storage from four to five tables per
  stage (CBV, SRV, sampler, SSBO, image). This change rechecks capacity after a
  flush and skips an oversized draw/dispatch before filling/binding tables; it
  also handles a failed descriptor-pool heap creation without dereferencing NULL.
- CopyBufferRegion now checks both physical resource sizes using subtraction
  after validating offsets, so integer wrap cannot bypass the guard. Invalid
  copies are skipped and reported via `D3D12_SYNC_ERROR`; valid edge copies pass.
  This protects physical bounds, not the smaller logical bounds of a suballocation.
- Main/fixup allocator reuse retains the existing completed-fence guard. Video
  decode/process now verify GetCompletedValue after an event and honor failed
  waits in release builds. Encode verifies completion before allocator reset and
  resource release. UINT64_MAX is never accepted as completed work.
- Residency promotion previously changed status before testing the old status,
  producing redundant MakeResident calls and leaving permanently resident BOs in
  the eviction LRU. It now preserves/checks the old state, removes the LRU entry,
  and only promotes after successful residency. The residency chunk loop now
  advances the last entry after a full chunk, resets accepted byte accounting,
  retires partial chunks, and retries full chunks after a failed enqueue. Eviction
  after a fence wait requires confirmed completion. Residency API failures are
  captured; this does not provide a general recovery strategy for OOM.
- Graphics ResourceBarrier calls use the direct queue; fixup shares its allocator
  and queue ordering. Video lists retain their video-specific transitions and
  inter-queue fences. No concrete wrong-queue barrier or production bundle misuse
  was found. Their API boundaries now preserve evidence for the next capture.

## Eden contexts

`MesaWindow::CreateSharedContext` gives the first context to the renderer and
marks subsequent contexts surfaceless. `MesaGraphicsContext::MakeCurrent` now
logs its thread ID and context handle. The `shader-worker` label includes the
surfaceless CPU-side context. `OpenGL::ShaderContext::Context` obtains the shared
context; `ShaderCache::CreateWorkers` creates the `GlShaderBuilder` pool.
Xbox's `precompile_on_current_context=true` makes strict cache compilation use
the current renderer context. Any worker that reaches Mesa uses the same API ring,
with no per-context blind spot or separate capture latch.

## Host validation

`python3 -m unittest discover -s tests/port` exercises the ring, HRESULT and void
forwarding, resource argument formatting, multi-thread publication, wraparound,
explicit contention gaps, opt-out, copy bounds and immediate-loss draining.
`test_mesa_full_chain.py` applies the entire patch to `/tmp/mesa-pin` with LF and
CRLF input, validates anchors/coverage and repeat rejection, and executes the
actual patched residency loop with 1/127/128/129/256/257 resources, partial chunks
and a failed first enqueue. No console run or UWP compilation was performed.

## Complete site inventory

Paths below are relative to `src/gallium/drivers/d3d12`. Pristine columns refer
to `/tmp/mesa-pin`. Patched columns are the `site=` identifiers emitted by the
final instrumentation pass (before its include insertion), also saved as
`nxbox_api_inventory.json` beside the patched driver. A dash means the site was
removed, moved into a shared journal helper, or introduced by an earlier patch.
PSO before/after observations in `nxbox_lifetime.h` are additional to these calls.

The final pass wraps **169 call sites**.

| File | API | Pristine lines | Patched site lines |
| --- | --- | --- | --- |
| `d3d12_batch.cpp` | `Close` | 262 | 384 |
| `d3d12_batch.cpp` | `CreateCommandAllocator` | 79 | 82 |
| `d3d12_batch.cpp` | `CreateCommandList` | 226 | 337 |
| `d3d12_batch.cpp` | `ExecuteCommandLists` | 283 | 446 |
| `d3d12_batch.cpp` | `Reset` | 183, 220 | 278, 329 |
| `d3d12_batch.cpp` | `SetDescriptorHeaps` | 241 | 353 |
| `d3d12_batch.cpp` | `Signal` | — | 451, 485 |
| `d3d12_blit.cpp` | `CopyBufferRegion` | 50 | — |
| `d3d12_blit.cpp` | `CopyTextureRegion` | 339, 357, 846 | — |
| `d3d12_blit.cpp` | `ResolveSubresource` | 134 | — |
| `d3d12_blit.cpp` | `SetPredication` | 906 | 931 |
| `d3d12_bufmgr.cpp` | `CreateCommittedResource` | 145 | 145 |
| `d3d12_bufmgr.cpp` | `Map` | 238 | 238 |
| `d3d12_bufmgr.cpp` | `Unmap` | 263 | 263 |
| `d3d12_cmd_signature.cpp` | `CreateCommandSignature` | 59 | 59 |
| `d3d12_compute_transforms.cpp` | `SetPredication` | 486 | 486 |
| `d3d12_context.cpp` | `ClearDepthStencilView` | 2157 | — |
| `d3d12_context.cpp` | `ClearRenderTargetView` | 2115 | — |
| `d3d12_context.cpp` | `CreateSampler` | 731, 739, 2261 | 735, 743, 2269 |
| `d3d12_context.cpp` | `CreateShaderResourceView` | 977 | 981 |
| `d3d12_context.cpp` | `ResourceBarrier` | 2372, 2388 | — |
| `d3d12_context.cpp` | `SetPredication` | 2047, 2140 | 2052, 2146 |
| `d3d12_context.cpp` | `Signal` | 2231 | 2239 |
| `d3d12_context.cpp` | `Wait` | 2240 | 2248 |
| `d3d12_descriptor_pool.cpp` | `CopyDescriptors` | 181 | 196 |
| `d3d12_descriptor_pool.cpp` | `CreateDescriptorHeap` | 70 | 72 |
| `d3d12_draw.cpp` | `CreateConstantBufferView` | 79 | 86 |
| `d3d12_draw.cpp` | `CreateUnorderedAccessView` | 180, 351 | 187, 358 |
| `d3d12_draw.cpp` | `Dispatch` | 1405 | — |
| `d3d12_draw.cpp` | `DrawIndexedInstanced` | 1255 | — |
| `d3d12_draw.cpp` | `DrawInstanced` | 1259 | — |
| `d3d12_draw.cpp` | `ExecuteIndirect` | 1251, 1403 | — |
| `d3d12_draw.cpp` | `IASetIndexBuffer` | 1151 | 1228 |
| `d3d12_draw.cpp` | `IASetVertexBuffers` | 1137 | 1214 |
| `d3d12_draw.cpp` | `OMSetRenderTargets` | 1175 | — |
| `d3d12_draw.cpp` | `SOSetTargets` | 1200 | 1308 |
| `d3d12_draw.cpp` | `SetComputeRoot32BitConstants` | 605 | 612 |
| `d3d12_draw.cpp` | `SetComputeRootDescriptorTable` | 1399 | 1523 |
| `d3d12_draw.cpp` | `SetComputeRootSignature` | 1371 | — |
| `d3d12_draw.cpp` | `SetGraphicsRoot32BitConstants` | 579 | 586 |
| `d3d12_draw.cpp` | `SetGraphicsRootDescriptorTable` | 1246 | 1354 |
| `d3d12_draw.cpp` | `SetGraphicsRootSignature` | 1052 | — |
| `d3d12_draw.cpp` | `SetPipelineState` | 1058, 1377 | — |
| `d3d12_draw.cpp` | `SetPredication` | 1299 | 1412 |
| `d3d12_fence.cpp` | `OpenSharedHandle` | 81 | 84 |
| `d3d12_fence.cpp` | `OpenSharedHandleByName` | 77 | 80 |
| `d3d12_fence.cpp` | `SetEventOnCompletion` | 54 | 57 |
| `d3d12_fence.cpp` | `Signal` | 52 | 53 |
| `d3d12_pipeline_state.cpp` | `CreateComputePipelineState` | 533 | 897 |
| `d3d12_pipeline_state.cpp` | `CreateGraphicsPipelineState` | 405 | 667 |
| `d3d12_pipeline_state.cpp` | `CreatePipelineState` | 397 | 652 |
| `d3d12_pipeline_state.cpp` | `OMSetRenderTargets` | — | 780 |
| `d3d12_query.cpp` | `BeginQuery` | 458 | 460 |
| `d3d12_query.cpp` | `CreateQueryHeap` | 156 | 157 |
| `d3d12_query.cpp` | `EndQuery` | 495, 541 | 497, 543 |
| `d3d12_query.cpp` | `ResolveQueryData` | 544 | 546 |
| `d3d12_query.cpp` | `SetEventOnCompletion` | 355 | — |
| `d3d12_query.cpp` | `SetPredication` | 689, 716 | 695, 722 |
| `d3d12_query.cpp` | `WriteBufferImmediate` | 625 | 631 |
| `d3d12_residency.cpp` | `CreateFence` | 256 | 264 |
| `d3d12_residency.cpp` | `EnqueueMakeResident` | 216 | 219 |
| `d3d12_residency.cpp` | `Evict` | 59, 65, 93, 99 | 59, 65, 96, 102 |
| `d3d12_residency.cpp` | `MakeResident` | 288 | 296 |
| `d3d12_residency.cpp` | `SetEventOnCompletion` | 71 | 71 |
| `d3d12_residency.cpp` | `Wait` | 249 | 257 |
| `d3d12_resource.cpp` | `CopyBufferRegion` | 1349 | — |
| `d3d12_resource.cpp` | `CopyTextureRegion` | 1158 | — |
| `d3d12_resource.cpp` | `CreateCommittedResource` | 358 | 363 |
| `d3d12_resource.cpp` | `CreateCommittedResource3` | 332 | 337 |
| `d3d12_resource.cpp` | `CreatePlacedResource` | 345 | 350 |
| `d3d12_resource.cpp` | `CreatePlacedResource2` | 316 | 321 |
| `d3d12_resource.cpp` | `CreateSharedHandle` | 536, 781 | 541, 786 |
| `d3d12_resource.cpp` | `OpenSharedHandle` | 570, 981 | 575, 986 |
| `d3d12_resource.cpp` | `OpenSharedHandleByName` | 554, 976 | 559, 981 |
| `d3d12_resource_state.cpp` | `Close` | 382 | 385 |
| `d3d12_resource_state.cpp` | `CreateCommandList` | 277 | 280 |
| `d3d12_resource_state.cpp` | `Reset` | 282 | 285 |
| `d3d12_resource_state.cpp` | `ResourceBarrier` | 380, 593 | — |
| `d3d12_root_signature.cpp` | `CreateRootSignature` | 233 | 255 |
| `d3d12_screen.cpp` | `CreateCommandQueue` | 1609 | 1631 |
| `d3d12_screen.cpp` | `CreateCommandQueue1` | 1602 | 1624 |
| `d3d12_screen.cpp` | `CreateFence` | 1614 | 1636 |
| `d3d12_screen.cpp` | `CreateRenderTargetView` | 1199 | 1202 |
| `d3d12_screen.cpp` | `CreateShaderResourceView` | 1125 | 1128 |
| `d3d12_screen.cpp` | `CreateSharedHandle` | 1293 | 1296 |
| `d3d12_screen.cpp` | `CreateUnorderedAccessView` | 1185 | 1188 |
| `d3d12_surface.cpp` | `CreateDepthStencilView` | 147 | 147 |
| `d3d12_surface.cpp` | `CreateRenderTargetView` | 226 | 226 |
| `d3d12_video_array_of_textures_dpb_manager.cpp` | `CreateCommittedResource` | 50 | 50 |
| `d3d12_video_buffer.cpp` | `OpenSharedHandle` | 145 | 145 |
| `d3d12_video_dec.cpp` | `Close` | 783 | 783 |
| `d3d12_video_dec.cpp` | `CreateCommandAllocator` | 847 | 847 |
| `d3d12_video_dec.cpp` | `CreateCommandList1` | 865 | 865 |
| `d3d12_video_dec.cpp` | `CreateCommandQueue` | 829 | 829 |
| `d3d12_video_dec.cpp` | `CreateCommittedResource` | 979 | 979 |
| `d3d12_video_dec.cpp` | `CreateFence` | 838 | 838 |
| `d3d12_video_dec.cpp` | `CreateVideoDecoder` | 938, 1149 | 938, 1149 |
| `d3d12_video_dec.cpp` | `CreateVideoDecoderHeap` | 1200 | 1200 |
| `d3d12_video_dec.cpp` | `DecodeFrame1` | 659 | 659 |
| `d3d12_video_dec.cpp` | `ExecuteCommandLists` | 793 | 793 |
| `d3d12_video_dec.cpp` | `Reset` | 216, 1710 | 216, 1712 |
| `d3d12_video_dec.cpp` | `ResourceBarrier` | 513, 644, 778, 1062 | 513, 644, 778, 1062 |
| `d3d12_video_dec.cpp` | `SetEventOnCompletion` | 1650 | 1650 |
| `d3d12_video_dec.cpp` | `Signal` | 794 | 794 |
| `d3d12_video_dec.cpp` | `Wait` | 701, 792 | 701, 792 |
| `d3d12_video_dec_av1.cpp` | `ResourceBarrier` | 95 | 95 |
| `d3d12_video_dec_h264.cpp` | `ResourceBarrier` | 120 | 120 |
| `d3d12_video_dec_hevc.cpp` | `ResourceBarrier` | 112 | 112 |
| `d3d12_video_dec_vp9.cpp` | `ResourceBarrier` | 98 | 98 |
| `d3d12_video_enc.cpp` | `Close` | 144 | 144 |
| `d3d12_video_enc.cpp` | `CreateCommandAllocator` | 1508 | 1511 |
| `d3d12_video_enc.cpp` | `CreateCommandList1` | 1527 | 1530 |
| `d3d12_video_enc.cpp` | `CreateCommandQueue` | 1487 | 1490 |
| `d3d12_video_enc.cpp` | `CreateCommittedResource` | 1648, 1669, 1985 | 1651, 1672, 1988 |
| `d3d12_video_enc.cpp` | `CreateFence` | 1497 | 1500 |
| `d3d12_video_enc.cpp` | `CreateVideoEncoder` | 455 | 458 |
| `d3d12_video_enc.cpp` | `CreateVideoEncoderHeap` | 504 | 507 |
| `d3d12_video_enc.cpp` | `EncodeFrame` | 2188 | 2191 |
| `d3d12_video_enc.cpp` | `ExecuteCommandLists` | 151 | 151 |
| `d3d12_video_enc.cpp` | `Reset` | 236, 1753 | 239, 1756 |
| `d3d12_video_enc.cpp` | `ResolveEncoderOutputMetadata` | 2224 | 2227 |
| `d3d12_video_enc.cpp` | `ResourceBarrier` | 139, 2029, 2124, 2208, 2244, 2258 | 139, 2032, 2127, 2211, 2247, 2261 |
| `d3d12_video_enc.cpp` | `SetEventOnCompletion` | 193 | 193 |
| `d3d12_video_enc.cpp` | `Signal` | 152 | 152 |
| `d3d12_video_enc.cpp` | `Wait` | 115, 120 | 115, 120 |
| `d3d12_video_proc.cpp` | `Close` | 355 | 355 |
| `d3d12_video_proc.cpp` | `CreateCommandAllocator` | 674 | 674 |
| `d3d12_video_proc.cpp` | `CreateCommandList1` | 694 | 694 |
| `d3d12_video_proc.cpp` | `CreateCommandQueue` | 650 | 650 |
| `d3d12_video_proc.cpp` | `CreateFence` | 661 | 661 |
| `d3d12_video_proc.cpp` | `CreateVideoProcessor` | 629 | 629 |
| `d3d12_video_proc.cpp` | `ExecuteCommandLists` | 373 | 373 |
| `d3d12_video_proc.cpp` | `ProcessFrames1` | 173 | 173 |
| `d3d12_video_proc.cpp` | `Reset` | 54, 818 | 54, 820 |
| `d3d12_video_proc.cpp` | `ResourceBarrier` | 169, 180, 350 | 169, 180, 350 |
| `d3d12_video_proc.cpp` | `SetEventOnCompletion` | 774 | 774 |
| `d3d12_video_proc.cpp` | `Signal` | 374 | 374 |
| `d3d12_video_proc.cpp` | `Wait` | 365, 370 | 365, 370 |
| `d3d12_video_texture_array_dpb_manager.cpp` | `CreateCommittedResource` | 53 | 53 |
| `nxbox_sync_batch.h` | `ClearDepthStencilView` | — | 264 |
| `nxbox_sync_batch.h` | `ClearRenderTargetView` | — | 249 |
| `nxbox_sync_batch.h` | `CopyBufferRegion` | — | 166 |
| `nxbox_sync_batch.h` | `CopyResource` | — | 235 |
| `nxbox_sync_batch.h` | `CopyTextureRegion` | — | 226 |
| `nxbox_sync_batch.h` | `Dispatch` | — | 326 |
| `nxbox_sync_batch.h` | `DrawIndexedInstanced` | — | 318 |
| `nxbox_sync_batch.h` | `DrawInstanced` | — | 307 |
| `nxbox_sync_batch.h` | `ExecuteBundle` | — | 331 |
| `nxbox_sync_batch.h` | `ExecuteIndirect` | — | 345 |
| `nxbox_sync_batch.h` | `ResolveSubresource` | — | 273 |
| `nxbox_sync_batch.h` | `ResourceBarrier` | — | 153 |
| `nxbox_sync_batch.h` | `SetComputeRootSignature` | — | 296 |
| `nxbox_sync_batch.h` | `SetGraphicsRootSignature` | — | 288 |
| `nxbox_sync_batch.h` | `SetPipelineState` | — | 280 |
