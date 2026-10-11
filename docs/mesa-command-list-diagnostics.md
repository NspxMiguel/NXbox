# Mesa command-list recording diagnostics

The pipe6-c4954ca26 console logs report `Close` returning `0x80070057` while
`GetDeviceRemovedReason()` is still `S_OK`. These are command-recording failures,
not evidence of exhausted CUSTOM heaps. The recorded process-lifetime totals are:

| Game | API sequence | Successful CUSTOM creations | Allocation bytes |
| --- | ---: | ---: | ---: |
| MK8D | 143327 | 31 | 86,048,768 |
| BotW | 88322 | 257 | 207,028,224 |

These counters are cumulative allocation totals, not live memory or residency.
The heap policy and existing counter accounting are unchanged.

## Next console run

The per-list ring runs independently of `NXBOX_SYNC_BATCH`. Every graphics
recording method used by the pinned Mesa driver is wrapped, including calls
inside the existing journal helpers, versioned graphics lists, fixup lists and
video lists. A command snapshots its arguments into fixed-size binary records; resources are
inspected before the call and no resource references are retained. There is no
per-command heap allocation or text formatting in the list capture. Text is
formatted only on Close/Reset failure. Each list owns its last 128 record
fragments through COM private data. Successful `Reset` starts a
new generation; failed `Reset` preserves the preceding commands plus the failed
attempt. Destruction releases the journal, including lists managed by `ComPtr`.
Creation records contain the list pointer/type, device, site, timestamp/thread,
node mask, allocator and initial PSO (or creation flags for `CreateCommandList1`).

Optional environment switches, set **before Mesa device initialization**:

- `NXBOX_D3D12_LIST_FULL=1`: retain the last **4096 fixed 512-byte records** in the
  current reset generation (2 MiB per list), independently of `NXBOX_SYNC_BATCH`.
  Default off keeps 128 records (64 KiB). The legacy heap-formatted batch journal
  is disabled in full mode even with `NXBOX_JOURNAL=1`, avoiding duplicate,
  unbounded string storage. Successful Reset starts a fresh ring using the same
  allocation; failed Reset preserves history. Large argument arrays span record
  fragments with the same command `seq`, `site`, and increasing `fragment`.
  `recorded` counts calls; `retained`, `dropped`, and `fragments` count fixed
  records. `capacity` and `record_bytes` make the memory bound explicit. When
  wrapping cuts into a command, its first retained `fragment` is greater than 0.
- `NXBOX_D3D12_VALIDATE=1`: enable cheap static validation at recording time,
  independent of API-ring/journal flags and capture allocation success. Publish
  only the first offender as `D3D12_VALIDATE_FIRST`, with call, Gallium site,
  barrier element, and reason. Checks include transitions to/from UAV/RT/depth
  states without required resource flags, equal transition states, incompatible
  texture format families, source/destination bounds (including mip and BC block
  extents), partial depth/stencil copies, and buffer bounds with overflow-safe
  arithmetic. Legal null aliasing/UAV barriers and compatible typeless or
  BC reinterpret-copy formats are preserved. Validation observes commands;
  it does not repair barriers, skip texture copies, or change submission.
- `NXBOX_D3D12_DEBUG=1`: default off. Resolve `D3D12GetDebugInterface`, enable
  `ID3D12Debug` before device creation, and configure `ID3D12InfoQueue` with empty
  storage/retrieval filters and a 4096-message limit. Failure capture retains
  ID, severity, category and description, bounded to 4096 messages, 64 KiB per
  message and roughly 1 MiB total description text. Oversized messages and
  count/byte truncation are reported explicitly.
  Debug mode uses the module's device creation path rather than an isolated
  device factory. Unavailable interfaces produce `D3D12_DEBUG_UNAVAILABLE hr=...`.
  `D3D12_INFOQUEUE` also reports discarded/denied counts and read failures.
- `NXBOX_D3D12_DUMP_EVERY_CLOSE=1`: retain up to five real Close/Reset failures
  under distinct immutable capture IDs. This does not resume recording, reset a
  poisoned list, or submit it. After the stop latch, only additional Close calls
  are allowed for this diagnostic option; ordinary recording remains stopped.
  Consequently a cleanly stopped game normally produces only one capture.

Run this against the next downloaded diagnostic file:

```sh
rg '^D3D12_(LIST_RING|INFOQUEUE|DEBUG_STATUS|DEBUG_UNAVAILABLE|LIST_ERROR|API_FIRST|SYNC_ERROR)|GAME_FAIL' game-diag.txt
```

`D3D12_LIST_RING` starts with the failure and CUSTOM totals, followed by creation
information and the list's ordered commands. `seq` is local to a reset generation.
Argument names describe root parameters, handles, copy endpoints, query ranges,
draw counts and PSO/root signatures. Barriers include every element in order,
not just the first element. Resource descriptions include dimension, format,
flags and named ALLOW_UNORDERED_ACCESS/RENDER_TARGET/DEPTH_STENCIL/
SIMULTANEOUS_ACCESS bits. Copy endpoints retain subresources, placed footprints,
source boxes and destination offsets. Clear and Discard retain all rectangles;
RTV/DSV resource descriptions come from snapshots taken when views are created,
with descriptor-slot reuse and heap destruction accounted for. Unknown handles
and null resources/arrays are explicit. Failure formatting never dereferences
resources, view pointers or caller-owned arrays, including those from other
contexts. RTV/DSV handles report both the expected type and
the actual type/alignment found in the live descriptor heap registry; an
unregistered handle is explicitly `heap_type=unknown`.

A long command or InfoQueue description is split into parts of at most 3500 text
bytes. Preserve **all** lines: `capture`, `part` and `offset` identify continuation
chunks. Manifests are committed after the chunks and before the terminal command
failure notification. Both polling and exception/device-loss draining read every
committed capture. Neither the 8192-byte frontend read buffer nor the former
three-message batch summary truncates these reports.

The ring narrows the recorded list; it cannot by itself prove which valid-looking
command the Xbox runtime rejected, particularly if it predates the last 128.
The debug queue may be unavailable on Xbox UWP. Only a subsequent console run
can establish the command responsible for the two reported failures.

## Static audit of the pin and final patch chain

### BotW calibrated-clock failure (2026-10-10)

The capture in `/tmp/botwfail/diag.txt` identifies list `000001D84FE25B50`,
context `000001D84DA02040`, batch 5, generation 397. Close returns `E_INVALIDARG`
with device removal still `S_OK`. Only sequences 1386 through 1513 survive:
1,386 earlier records are missing. `journal=allocation-failed` in the old batch
manifest also covered intentionally disabled journaling; the manifest now
distinguishes `disabled` from `allocation-failed`.

- Sequences 1422/1423 and subsequent pairs are null/null ALIASING (type 1) and
  null UAV (type 2) barriers. Both permit null resources under the documented
  [ResourceBarrier contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-resourcebarrier).
- Sequence 1420 changes `0x880` (COPY_SOURCE | PIXEL_SHADER_RESOURCE, both reads)
  to `0x4` (RENDER_TARGET) on a texture with flags `0x1` (ALLOW_RENDER_TARGET).
  Sequence 1428 changes that texture to COPY_SOURCE and its scratch buffer from
  COPY_SOURCE to COPY_DEST. Visible transitions have different before/after
  states, no read/write unions, and no UNORDERED_ACCESS state on a non-UAV resource.
- Sequences 1429/1431 copy RGBA8 (format 28) to a buffer footprint of format 28,
  then a format-24 footprint to a format-24 texture. This is the intentional
  byte reinterpretation through a buffer, not a direct incompatible texture copy.
  For 1280x720 the pitch is 5120 and the buffer is 3,686,400 bytes. Repeated
  320x180 copies use pitch 1280 and 230,400-byte buffers. Offsets are zero,
  dimensions agree, and pitches are 256-byte aligned.
- Sequence 1509 uploads format 26 to a matching 16x8x1 format-26 3D texture,
  pitch 256, from a 65,536-byte buffer. No visible format or bounds violation.
- Sequences 1511/1512 end and resolve occlusion query 15 at byte offset 120 in
  a 65,536-byte buffer. The preceding BeginQuery and initial destination state
  are outside the retained window, so query pairing/state cannot be established.
- Recording changes from thread 6560 to 6228 at sequence 1428. This demonstrates
  thread migration, not simultaneous recording or a cross-context state race.
  The list contains draws/clears; its different context pointer alone does not
  establish that it is a shader-only worker context.

No retained command proves the root cause. Enable `NXBOX_D3D12_LIST_FULL=1` for
the next run (optionally `NXBOX_D3D12_DEBUG=1` where supported) and preserve all
capture parts. The bounded window can recover initial state/heap/vertex/index bindings and
query begins when the generation fits in 4096 record fragments; check `dropped`
before assuming it contains every command. Use `NXBOX_D3D12_VALIDATE=1` as well:
the first static offender is retained even if its history record later wraps. Async submission is unchanged:
restoring a drain would not establish which recording argument was rejected.

| Candidate | Finding / action |
| --- | --- |
| Equal-state transitions | `transition_required` rejects exact matches and subset read states in both normal/fixup paths; existing promotion repair avoids read/write state unions. No blanket barrier suppression added. |
| COPY queue / COMMON | Graphics and fixup allocators/lists use DIRECT. No COPY queue is created in this driver pin. CUSTOM mapping still starts in COMMON. |
| Split flags / duplicate transitions | Graphics barriers are zero-initialized, with no BEGIN/END split usage in this pin. Pending BO state resolution already coalesces graphics transitions. The new ring preserves the complete array/order for runtime confirmation, including video arrays. Repeated subresources can legitimately form a sequence and are not blindly deduplicated. |
| Copy bounds and BC | Existing buffer bounds guard, BC allocation rounding/edge normalization and planar/array copy fixes remain. Every resource desc, placed footprint, destination offset and source box now survives in the per-list capture. No speculative clipping of uncompressed or reinterpret copies. |
| Clear rect pointer | Pinned clear call sites pass one stack rect. Hardened the old journal's formatting against a null rect pointer so diagnostic formatting cannot crash before the underlying API and new ring observe it. |
| ResolveSubresource | **Fixed:** the native fast path ignored array-layer offsets and depth. It now resolves each selected layer using its own source/destination mip stride. Offset rectangles, invalid/mismatched layer ranges and 3D slices use the existing fallback. Sample-count/format/full-size checks remain. This is a proven addressing bug, not a proven cause of the reported Close failures. |
| Bundles / PSO type | No bundle recording/submission call in the pin. Graphics/compute PSOs remain distinct; no PSO substitution added. |
| RTV/DSV / descriptor heaps | Batch heaps are CBV_SRV_UAV plus SAMPLER; the existing patch already handles either heap allocation failing. OM uses non-contiguous CPU handles. Live heap ranges now expose actual type and handle alignment. |
| Indexed draw without IBV | Direct indexed path constructs/binds IBV; batch start dirties the IBV state. No evidence justifies suppressing indexed draws. |
| RTV format mismatch | Existing format safety remains; this audit does not label shader/RTV mismatch as a proven Close validation failure. |

Microsoft documents runtime validation at
[ResourceBarrier](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-resourcebarrier)
and the [command-list validation boundary](https://microsoft.github.io/DirectX-Specs/d3d/CPUEfficiency.html).
These distinguish recording validation from later GPU/device failures; they do
not identify the rejected command in these particular console runs.

## Host validation

```sh
NXBOX_MESA_SRC=/tmp/mesa-pin python3 -m unittest discover -s tests/port
```

The scope checker independently reverses every wrapper to verify receiver,
arguments, lexical scope and include order. The full-chain test applies the
patches to pristine LF and CRLF trees and rejects repeat application. New host
tests cover list isolation, wraparound, reset success/failure, COM-owned cleanup,
long arrays/messages, five immutable captures, actual heap types, debug opt-in,
bounded 100,000-call capture with allocation rejection, null/destroyed resource
snapshots, Clear/Discard rectangles, first-offender validation and valid format
families,
terminal frontend draining and array-layer resolves. The real DirectX-Headers
syntax test instantiates graphics and video wrappers with clang; set
`NXBOX_DIRECTX_HEADERS` to their include directory if no local Mesa artifact
provides them. These checks do not replace a Windows/UWP build or console run.

### BotW full-generation failure: concurrent SRV maintenance recording

`/tmp/botwfail/diag_capture.txt` contains generation 1690 of list
`00000146A3EA11A0`, context `00000146A1A02040`, batch 2. Reassembly deduplicates
three repeated capture blocks into **1,622 calls / 1,945 fragments**, matching
`retained=1945 dropped=0`. Reproduce the static audit with:

```sh
python3 tools/nxbox/analyze_mesa_capture.py /tmp/botwfail/diag_capture.txt \
  --json /tmp/botwfail/audited.json
```

The invalid commands are **ResourceBarrier sequences 759 and 760**, both at
`d3d12_resource_state.cpp:598` (`d3d12_apply_resource_states`). Both contain
`barrier[2]={type=1826445160 flags=0x7ff6 }`. Type `0x6cdd5768` is outside
TRANSITION/ALIASING/UAV (0/1/2), and the flags contain reserved bits. Microsoft's
[ResourceBarrier runtime-validation contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-resourcebarrier)
explicitly checks the type enum and states that failure makes Close return
E_INVALIDARG. The old validator skipped every non-transition type, including
corrupt ones. It now checks type and flags before inspecting the union; legal
null aliasing/UAV barriers remain accepted.

The patch-induced recording race explains the corruption. `patch_view_cast`
inserted `nxbox_refresh_srv_shadow` into `d3d12_create_sampler_view`, which the
pinned `tc_create_sampler_view` calls directly on the frontend without draining
the driver worker. That refresh calls `copy_texture_region`, mutating the same
context's resource-state tables, `barrier_scratch` dynarray and command list as
the worker. The capture shows frontend thread 2672 doing shadow copies at
750/758 while worker 1900 records draw/dispatch at 747/755. At 759 the worker
records four barriers; at 760 the frontend records five, preserving the same
corrupt third entry and preceding transitions. This is concrete unsafe
concurrent context use, rather than resource creation on another context being
intrinsically invalid. The [D3D12 threading contract](https://microsoft.github.io/DirectX-Specs/d3d/CPUEfficiency.html)
prohibits concurrent calls on the same command list. The exact CPU memory
overwrite cannot be reconstructed from a command capture alone.

The smallest fix removes only the refresh from view creation. Shadow allocation
and descriptor initialization remain there; queued `set_sampler_views` and
pre-draw/pre-dispatch refreshes still initialize and update the shadow before
use. Adding a mutex only around the frontend copy would leave the worker
unprotected. Zeroing or filtering the corrupted barrier would hide the race.

The script checks all 64 texture copies and seven buffer copies for format
families/reinterpret pairs, extents, full depth/MSAA copies, identical texture
subresources, footprint alignment/row size/buffer capacity and buffer overlaps.
It also checks captured barrier flag/state requirements and read/write unions.
None adds another static offender. There are no ResolveSubresource or
ClearUnorderedAccessView calls in this generation. There is no captured UAV
transition on an RT-only texture; the earlier `nxbox_uav_capable` guard remains
applicable and unchanged. DEPTH_READ/read-state unions are legal; tracked-state
mismatches alone are not the runtime validation violation identified here.
The capture does not expose every descriptor or resource's creating context,
so it cannot certify all descriptor contents or cross-list synchronization.

Host regression coverage checks reassembly, duplicate/conflicting fragments,
corrupt barrier type/flags without dereferencing the union, legal null barriers,
and that patched sampler-view creation cannot record maintenance copies while
bind/draw/dispatch refreshes remain installed. A fresh Xbox run is still needed
to verify the corrected patch chain on the runtime.
