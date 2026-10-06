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
video lists. A command owns a text snapshot of its arguments; resources are
inspected before the call and no resource references are retained. Each list
owns its last 128 commands through COM private data. Successful `Reset` starts a
new generation; failed `Reset` preserves the preceding commands plus the failed
attempt. Destruction releases the journal, including lists managed by `ComPtr`.
Creation records contain the list pointer/type, device, site, timestamp/thread,
node mask, allocator and initial PSO (or creation flags for `CreateCommandList1`).

Optional environment switches, set **before Mesa device initialization**:

- `NXBOX_D3D12_DEBUG=1`: default off. Resolve `D3D12GetDebugInterface`, enable
  `ID3D12Debug` before device creation, and configure `ID3D12InfoQueue` with empty
  storage/retrieval filters and `UINT64_MAX` message limit. All stored messages
  are retained, including ID, severity, category and complete description.
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
not just the first element. RTV/DSV handles report both the expected type and
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
terminal frontend draining and array-layer resolves. The real DirectX-Headers
syntax test instantiates graphics and video wrappers with clang; set
`NXBOX_DIRECTX_HEADERS` to their include directory if no local Mesa artifact
provides them. These checks do not replace a Windows/UWP build or console run.
