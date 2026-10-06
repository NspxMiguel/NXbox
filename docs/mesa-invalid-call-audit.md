# Mesa invalid-call audit and journal repair

Host-only investigation of NXbox commit `97d076c7c3`, package 0.3.256, using Mesa
`15acdd7ea2b9dcdd62f26fe86b88280d79efc46b` at `/tmp/mesa-pin`. No console execution,
Windows driver build, deployment, or push was performed. The fixes below remove
reproducible invalid paths; none is yet proven to be the console's triggering call.

## Evidence

Read both complete diagnostics in
`~/.local/share/nxbox/artifacts/pipe6-97d076c7c/`, with the accompanying Eden logs
and comparisons against `pipe5-4932c691e/` and `pipe6-6a7affd1e/`.

- **MK8D:** last sampled batch is 1280, submit `21474837749`, fence target 1280,
  `close_failed=0`, removed reason still zero. Last PSO sample is 16 created,
  zero failed. Last stall sample reports 426 uploads, 20 ms total / 1 ms longest;
  GPU work is 2203/24/353 and presentation 1900/24/118. Removal subsequently
  appears at `fence-before-wait`, `0x887a0001`. DRED has zero breadcrumb nodes
  and page-fault VA zero. This favors investigating resource/copy/draw commands
  during the scene change, but does not identify one. The last sampled batch is
  **not** necessarily the failing batch: successful reports are throttled.
- **BotW:** removal persists with the quarantine patch present in this build.
  Last sampled batch is 5120, submit `21474841589`, target 5120; PSOs reach
  144 created / zero failed. The last presentation sample still has 150 frames
  in 5.005 seconds. Then DRED reports `0x887a0001` at `batch-reuse`, followed by
  `GAME_STOPPED` and `GAME_FAIL`. The file does not contain a later zero-present
  interval because the frontend now exits. The Eden shutdown finishes at
  approximately 86.9 seconds on its own clock; MK8D finishes at 26.8 seconds.
  These timestamps do not establish the roughly two-minute/45-second wall times
  measured outside the application.
- **Older BotW:** `pipe6-6a7affd1e` records the rejected PSO with VS
  `2492/fee40f01e55da8b0`, PS `2192/944713566752175e`, followed by loss at `gfx-pso`.
  Its final presentation interval has zero frames. The new capture contains no
  PSO quarantine diagnostic, so it does not prove that the blocked pair was
  actually encountered, or that PSO creation cannot be involved in the new loss.
- `fence_ok=1` only tested whether a fence object existed. It was not proof of
  GPU completion. New reports call it `fence_present` and add `sync=1 journal=2`.

## Why the journal disappeared

The normal five-second frontend collector already knew the journal keys. The
immediate device-loss branch ran before that collector, logged only four fields,
and threw. Neither the manifest nor the eight parts was collected there.
Additionally, DRED set `NXBOX_D3D12_DEVICE_LOST` before publishing its reports.

The producer had a separate limitation: it published only if the synchronous
submission wait observed removal. A fence/PSO/reuse observer could notice loss
later, after that wait returned and after the context journal was reset. Both
capture locations in the new evidence are outside the journal publisher.

The repaired producer snapshots the journal immediately before Execute, retains
it through completion and context reset, and lets any DRED observer freeze that
snapshot once per device. Publication precedes the frontend exit notification.
The loss branch now drains the manifest, all eight parts, batch report, and sync
error. No environment opt-in is required; only `NXBOX_SYNC_BATCH=0` disables it.

The retained tail contains at most 64 records, in execution order: fixup then
main. Separate rings prevent a large fixup, recorded later on the CPU, from
wiping out the main command tail. The manifest distinguishes an in-flight
submission from a last-completed submission. The latter is useful evidence for
late or non-submission loss, **not proof that a completed batch caused it**.

Records include barrier before/after states and resource descriptions, buffer
and texture copies, clears, resolves, draw/dispatch/indirect calls, PSO pointer
identity, root signatures, and expected/bound RTV/DSV formats. The wrapper also
records `ExecuteBundle`; this pin has no graphics bundle calls, and the patch
rejects uninstrumented command sites if the source changes. No COM references
are retained by the snapshot. Video queues are outside this graphics journal.

## Audited fixes

| Path | Defect and resulting change | Evidence strength |
| --- | --- | --- |
| Array texture copies | Gallium's multi-layer `box.depth` reached one D3D12 subresource copy. Split layers before state transitions and copies; preserve volumetric copies when neither endpoint is an array. | Reproduced with a three-layer host test; concrete out-of-bounds region risk. |
| Depth/stencil planes | Normalizing array Z for plane zero destroyed the layer index for plane one; the source-format condition also incorrectly inspected the destination for D32+S8. Restore layer coordinates for each plane and inspect the source format. | Host test verifies source/destination indices 13/21 and 45/53 for mip 1, layers 3/5, eight layers. |
| Full depth/MSAA layer copies | Full-copy selection tested the array index instead of subresource-local Z, producing a box for a full nonzero layer. Test normalized Z and emit the null box. | Host test verifies the full-copy path for both depth/stencil planes. |
| Depth-only format copies | D32 and D32+S8 were accepted solely because one is the depth-only counterpart of the other, despite different DXGI type groups. Require matching typeless groups; otherwise the existing blit fallback handles it. D24/S8 to D24 remains accepted. | Executable compatibility tests; no new shader conversion introduced. |
| Promotion bookkeeping | The first exact promotion failed to retain `is_promoted`; the accumulated-promotion branch lacked a read-only destination check. Retain the flag and require read-only states on both sides, so a subsequent write takes the explicit barrier path. | State-machine tests; the illegal-union branch was latent, not demonstrated in either game capture. |
| Root descriptor tables | The temporary arrays allowed four tables per graphics stage, while CBV, SRV, sampler, SSBO and image tables require five. Reserve all five. | Host capacity test fills all 25 tables; actual shader usage in the failing batch remains unknown. |

The copy changes follow Microsoft's [CopyTextureRegion contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copytextureregion): compatible type groups, valid per-subresource coordinates, and full depth/stencil or MSAA rectangles. Promotion changes follow the [resource-state rules](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12), which do not permit read/write unions.

## Other audit results and remaining uncertainty

- **Typed UAVs:** texture format advertisement and resource creation query typed
  load/store support; the existing image-slot and 3D-UAV transition fixes remain.
  Buffer views and format emulation still need actual failing format evidence.
  No blanket format blacklist or forced UAV flag was introduced.
- **RTV/PSO and depth states:** the pin derives PSO formats through
  `d3d12_rtv_format`, including logic-op surface conversion. DSVs use flags NONE
  and draws request DEPTH_WRITE. No read-only DSV mismatch is proven; simultaneous
  depth sampling remains a candidate requiring the actual resources/states.
  New draw records expose expected versus bound formats.
- **Descriptor heaps:** the driver counts view/sampler demand before flushing;
  existing batch-reuse guards protect live storage. The concrete capacity defect
  found was in the temporary root-table arrays, not a measured heap exhaustion.
- **Promotion/decay and queues:** preserve submission-time global fixups and
  reset of simultaneous-access resource tracking at submission. Graphics uses
  one direct queue; fixup and main execute together. Do not force COMMON between
  these lists or disable decay globally.
- **Buffer copies, 16-bit and MSAA:** buffer copies account for suballocation
  offsets; staging uses D3D12 footprints. No blanket four-byte rounding was
  added to byte copies. Index formats are restricted to R16/R32, with primitive
  conversion for unsupported cases. Texture MSAA support is feature-queried;
  existing full-rectangle and stencil-resolve fixes remain. The logs supply no
  concrete misaligned copy or unsupported sample count.
- Successful PSO creation and Close do not identify the later invalid command;
  empty DRED and VA zero do not prove the absence of every resource-lifetime bug.

## Host validation

- `python3 -m unittest discover -s tests/port -p 'test_*.py'`: **62 passed**.
- Includes `test_mesa_full_chain.py` against the real pinned tree with LF and
  CRLF, identical patched output, and repeat-application rejection without writes.
- New C++ mocks compile the complete journal helper and execute snapshot
  retention after context reset/destruction, cross-thread capture, capture-once,
  execution ordering, truncation, eight bounded chunks, immediate frontend
  collection, and the copy/state cases above.
- All four portable C++ targets in `tests/port/CMakeLists.txt` were compiled with
  host `clang++` and passed: demand commit (270347 checks), update version,
  protocol URI, and cheats parsing with zlib. CMake was unavailable, so these
  targets were built directly. The Windows-only sparse-memory target cannot run
  on this macOS host.

## Next console run

1. Rebuild both the patched Mesa DLL and the frontend. Verify a batch report
   contains `sync=1 journal=2`; package version alone does not prove DLL contents.
2. Reproduce MK8D's scene transition and let BotW run beyond its previous freeze.
3. On removal, expect `D3D12_FIRST_BAD_BATCH` and `D3D12_BATCH_JOURNAL_0` through
   `parts-1` before `GAME_FAIL`. Check `entries`, `dropped`, `completed`, and
   `attribution`; never infer guilt from `last-completed-submit` alone.
4. Inspect transitions for the copied/drawn resources, both copy formats and
   subresource indices, null versus partial boxes, expected/bound target formats,
   PSO/root identities, and the last draw/dispatch. With `completed=0`, the
   serialized batch is the first in-flight submission observed with loss.
5. If either game survives, retain the diagnostic beyond the old failure point;
   that supports the fixes but does not isolate which one resolved the failure.
