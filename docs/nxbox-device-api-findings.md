# NXbox 0.3.270 device API findings

Host investigation of `~/.local/share/nxbox/artifacts/pipe6-7ac6b4643/{mk8,botw}-diag.txt`, using pristine `/tmp/mesa-pin` and the complete NXbox patch chain. No console execution was available. Line references below refer to the original diagnostic files, not regenerated Mesa source.

## What the logs establish

| Evidence | MK8D | BotW |
| --- | --- | --- |
| First observed removal | line 86: seq 149404, `CreateRenderTargetView`, `d3d12_surface.cpp:226`, ms 285459000 | line 125: seq 241978, `CreateShaderResourceView`, `d3d12_context.cpp:981`, ms 285946093 |
| Removal reason | `0x887a0001` | `0x887a0001` |
| Unique retained sequences | 149149–149404, 256 entries, zero gaps | 241723–241978, 256 entries, zero gaps |
| Repeated copies of the same ring | 4 | 2 |
| Failed command-list resets | 19 distinct; 76 printed entries | 22 distinct; 44 printed entries (not 46) |
| Successful CUSTOM committed creations in retained window | 6 | 12 |
| Texture payload bytes in retained window | 327,504 | 118,320 |
| Last `GAME_PRESENT` app memory | 3,280,281,600 bytes (3.055 GiB), line 54 | 4,226,531,328 bytes (3.936 GiB), line 99 |

The MK8D creation at seq 149390 (line 328) is BC5_UNORM (83), 28×28, with CUSTOM / NOT_AVAILABLE / L0 / heap flags 2048. The following 14 calls include **one** `CreateGraphicsPipelineState`, not two; it returns S_OK. They also include Map, Unmap, barriers, a texture copy, a failed Reset, and descriptor creation. `ResourceBarrier` and `CopyTextureRegion` in `nxbox_sync_batch.h` are **recording calls**, not queue submissions.

BotW seq 241974 (line 377) creates a 46×46 RGBA8_UNORM (28) resource with `ALLOW_RENDER_TARGET`, using the same heap properties. Three BeginQuery calls and a failed Reset precede it. Neither retained ring contains a Close, ExecuteCommandLists, or Signal. MK8D's last successful removal observation is 16 ms before the first failed observation; BotW's is in the same millisecond. These intervals do not measure time since a fence completed.

The `completed=1` manifests identify the **last checked-good submission**, not the point of failure. In BotW it is batch 7 / submit 21474838896 / fence 4853 (lines 105, 110). Line 72 already reports the *next* batch's Close failure: batch 0 / submit 21474838897, `close_failed=1`, `close_hr=0x80070057`, `removed=0`, no fence. Subsequent reports increase `list_reset_failed` from 1,816 to 22,662 while `alloc_reset_failed=0` (lines 73–100). The CPU continues issuing calls long after submission has stopped. The retained rings cannot prove the initiating invalid recorded command, nor whether the later removal originated on the GPU or in a CPU call.

## Reset diagnosis and correction

The generated API inventory resolves `d3d12_batch.cpp:329` to `ctx->cmdlist->Reset(batch->cmdalloc, nullptr)` in `d3d12_start_batch`. Line 278 is the separate allocator Reset, which succeeds in these rings. The batch reuse patch already checks the real fence completion value before allocator reuse.

Microsoft documents that a list whose Close failed cannot be reset successfully: subsequent Reset returns the same error. E_INVALIDARG can also indicate an allocator used by another recording list or a wrong allocator type; E_FAIL is the documented result for resetting an open list. Thus E_INVALIDARG alone does not establish “Reset while recording” or an in-flight allocator reset. BotW's earlier failed Close directly supports the poisoned-list cascade. MK8D's original Close failure is outside the retained evidence.

The old start path marked `batch->has_errors` but did not stop the context. End skipped Close for errored batches; flush advanced the batch index anyway. Query/copy operations could keep using the invalid global list and later reuse batch storage. This is a real failure-handling bug even when `removed=0`.

The ring now captures the first failed Close/Reset with `attribution=command-api-failure` and preserves its actual removal status. A shared terminal latch blocks subsequent command-list calls, allocator resets and queue execution, including with `NXBOX_SYNC_BATCH=0`. Signals and waits remain available to drain previously submitted work. Batch cleanup retains resources while a poisoned list may reference them; query waits retire without reading unsubmitted results. The frontend drains diagnostics and exits through its existing error path on `NXBOX_D3D12_COMMAND_FAILURE`. This prevents the cascade; it does not claim to repair the original invalid command or allow gameplay to continue.

## Heap policy and capability checks

Mesa already queries `D3D12_FEATURE_ARCHITECTURE` during screen initialization and fails initialization if it cannot obtain it. Both texture committed-creation branches call `GetCustomHeapProperties(DEFAULT)`; the buffer manager chooses DEFAULT, UPLOAD or READBACK from CPU-access usage and then calls the same helper. GetCustomHeapProperties always returns a CUSTOM heap, regardless of whether UMA is true.

Contrary to the initial hypothesis, **NOT_AVAILABLE + L0 is documented for DEFAULT-equivalent heaps on UMA**, both coherent and noncoherent. L1 is the DEFAULT mapping on discrete adapters. The logs do not prove an invalid heap combination. As requested, the UWP patch now uses an abstract DEFAULT heap with UNKNOWN CPU page/pool properties for GPU-only textures, render targets and buffers. CPU-mapped buffers retain CUSTOM/L0 with WRITE_BACK for readback or cache-coherent UMA upload; other uploads use WRITE_COMBINE. COMMON initial states and CPU mapping remain compatible with Mesa's custom mapped-buffer path. Node masks are 1.

`2048 == 0x800 == D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT`. `CREATE_NOT_ZEROED` is **0x1000** and is not enabled here. Upstream already gated NOT_RESIDENT on successful ID3D12Device8 QI, which Microsoft documents as sufficient support evidence. The UWP patch conservatively also requires a successful OPTIONS7 query. There is no separate boolean support field for these flags in OPTIONS7. Failure leaves creation resident and the initial BO residency state consistent with the flags; existing residency management remains active.

`NXBOX_D3D12_HEAP_POLICY` reports UMA/coherence and the selected policy. Ring entries retain type/page/pool, show heap flags in decimal and explicit hex, and decode the two creation bits. `custom_created`, `custom_bytes`, and `custom_unknown` are **process-wide cumulative successful committed CUSTOM creation totals**, not live allocations or a per-device residency budget. `custom_bytes` sums `GetResourceAllocationInfo().SizeInBytes`; failed size queries increment `custom_unknown`. Failed creations and S_FALSE validation calls do not contribute. Counters survive ring wraparound; concurrent snapshots may straddle another worker's update. The diagnostic opt-out disables these counters.

## Memory interpretation

The old rings contain dimensions, formats, one mip and one sample, but no allocation sizes or destruction history. Payload calculations use 4 bytes/texel for format 28, 1 byte/texel for format 61, and 16 bytes per 4×4 block for BC5. They exclude alignment, tiling and metadata and are **not allocated heap bytes**. They are only retained-window totals; neither session's full creation count, total allocation bytes, nor live CUSTOM usage can be reconstructed.

`GAME_PRESENT memory` is `MemoryManager::AppMemoryUsage()`, not a Mesa heap counter. MK8D rises from 2.055 to 3.055 GiB over the short capture. BotW rises from 1.967 GiB and then remains around 3.92–3.94 GiB for its last several samples, including after the Close failure. This does not establish an unbounded leak or hitting the app limit. The new log adds `MemoryManager::AppMemoryUsageLimit()` alongside usage so the next run can measure actual headroom.

## Next console capture

- `D3D12_HEAP_POLICY`: verify `gpu=DEFAULT`, UMA/coherence and `create_not_resident`; `create_not_zeroed=0`.
- Resource ring entries: GPU textures should show `heap_type=1 cpu_page=0 pool=0`; mapped buffers should show `heap_type=4 pool=1 cpu_page=2` (WRITE_COMBINE) or `3` (WRITE_BACK). Heap flags may remain 0x800 only when the capability gate succeeds.
- `D3D12_API_FIRST` / `D3D12_API_RING`: a recording failure should now capture the *first* failing Close/Reset, with the preceding recorded commands, without thousands of retries. `removed=0` must remain distinguishable from actual removal. Successful void APIs use a synthetic S_OK, not an API-returned HRESULT.
- `D3D12_SYNC_ERROR` and the frontend “command recording failed” error: identify the failing API/site/HRESULT. A terminal command failure intentionally stops the run earlier than the old delayed removal.
- `custom_created/custom_bytes/custom_unknown`: successful creation volume up to capture; do not compare cumulative bytes directly with live app usage.
- `GAME_PRESENT memory=... memory_limit=...`: compare app headroom over time. A live allocation/release counter would be needed to prove a Mesa resource leak.

References: [GetCustomHeapProperties mappings](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-getcustomheapproperties%28uint_d3d12_heap_type%29), [heap flag enum](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_heap_flags), [flag availability](https://devblogs.microsoft.com/directx/coming-to-directx-12-more-control-over-memory-allocation/), [command-list Reset](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-reset).

## Host validation

`python3 -m unittest discover -s tests/port`: **90 tests passed**, with no skips. This includes the pristine pinned Mesa full chain for LF and CRLF, repeat-application rejection, all 169 instrumented call sites across 28 files, and the scope checker. Clang C++17 compile/run tests cover heap architecture combinations, the actual generated Device8/OPTIONS7 gate, first failed Close and Reset with diagnostics on/off, creation counters across ring wraparound, stopped query waits, and frontend diagnostic draining. The scope checker also syntax-checks the extracted pinned command-signature function. `git diff --check` passes. Formatting used Ruff and Xcode clang-format, retaining formatter edits only in changed regions of existing files.

These are host tests with narrow D3D12/Windows mocks, not a full Windows/UWP SDK build or console validation. The initiating Close failure still needs the next console capture. No push was attempted. Git staging was blocked because the sandbox could not create the shared worktree `index.lock`; all changes remain in the worktree.
