# BotW 1.6 unmapped memory investigation

The patch fixes a demonstrated scalar memory-access bug and adds console diagnostics.
It is a **candidate fix for this hang**, not a confirmed root cause: the supplied logs
contain neither SVC results nor the mapping history. No build, emulator, or console
test was run. No commit or push was made.

## Evidence and address classification

- `botw-nodlc-eden.txt`: first fault at line 197, 16.542802 s,
  `Read8 @ 0x21c96c03f3`; 901,261 scalar unmapped accesses in total.
  The observed range is `0x21c6a00002` through `0x220e54c62e`.
- `botw-internal-eden.txt`: first fault at line 202, 16.843256 s,
  `Read8 @ 0x21daea12d3`; 901,202 scalar unmapped accesses. Moving storage does
  not remove the failure. The no-DLC run also fails.
- Frontend diagnostics report a 5120 MiB process limit, 1035 MiB committed at
  `MEM loaded`, and later `GAME_PRESENT memory` around 1.94 billion bytes.
  The latter is a different diagnostic field, not proof of peak commit usage.
- `0x21c96c03f3` is about **135 GiB of guest virtual address**, not 8.9 GiB
  of physical allocation. The guest thread PCs near `0x82d...` and stacks near
  `0x108...` fit the ordinary 39-bit layout with fastmem disabled.

`KProcess::LoadFromMetadata` does not set `EnableAslr` (the loader separately
adds a small code offset). With the 39-bit sizes in `k_address_space_info.cpp`,
`KPageTableBase::InitializeForProcess` places the regions after aligned code end C:

| Region | Start | Reserved virtual size |
| --- | --- | --- |
| Kernel map (including TLS/shared mappings) | C | 64 GiB |
| Stack | C + 64 GiB | 2 GiB |
| Alias | C + 66 GiB | 64 GiB |
| Heap | C + 130 GiB | 8 GiB |

With C near `0x83000000`, the heap begins near `0x2183000000`. The fault range
is therefore in the **heap reservation**. Its exact allocated extent is absent
from the old logs. `SetHeapSize` must map it; `MapPhysicalMemory` is restricted
to the alias region. NSO data/BSS are mapped with code, far below these addresses.
`MapMemory` aliases existing pages into the stack region; creating transfer
memory locks existing pages rather than extending the heap.

## Ranked hypotheses

1. **Earlier guest-data corruption through a page-crossing scalar callback.**
   `Memory::Impl::Read<T>` and `Write<T>` translated only the first byte and then
   copied `sizeof(T)` contiguous host bytes. Adjacent guest pages can have
   nonadjacent physical backing. A 64-bit access at page offset `0xffc`, for
   example, reads/writes four bytes of the wrong physical page. Dynarmic explicitly
   routes page-boundary misaligned accesses to callbacks. This is a definite
   upstream memory-path bug, also present before this worktree's upstream merge;
   NXbox's disabled fastmem exposes this path. Desktop fastmem does not establish
   correctness of these callbacks. **The existing logs do not prove BotW exercised
   such a crossing before its first fault.** The patch translates each byte of
   crossing accesses; accesses contained within a page retain the existing path.
2. **Failed heap extension or subsequent mapping/lifetime error.**
   `SetHeapSize` can reject the requested size, fail physical-resource reservation,
   fail page allocation, or fail memory-state/block allocation checks. These
   propagate failure results, not successful mappings. The patch records both
   successful heap extents and failures, including resource usage/limit. A guest
   accessing past a successful heap end remains possible; the old logs cannot
   distinguish it from lost mappings or pointer corruption.
3. **Sparse-vector bookkeeping or host backing failure.**
   No guest-address-offset cap was found in the UWP backing. `HostMemory` reserves
   physical DRAM separately; guest virtual addresses index page-table entries,
   not the DRAM allocation. `SparseMemory` has 256 reservation slots, size_t
   offsets, and 64 KiB demand commits. Its 4 KiB vector-page clearing preserves
   other pages sharing a committed chunk. Reservation failures assert; a failed
   VEH commit propagates a host exception rather than returning a guest SVC
   success with zero entries. The first-fault diagnostic compares the vector's
   bitmap-filtered entry with the raw entry Dynarmic sees, to detect disagreement.

No Xbox-specific heap, address-space, physical-resource, or thread-count clamp
was found. Default DRAM is 4 GiB, with a 3285 MiB application pool; 6/8 GiB modes
have 4916/6547 MiB pools. Those larger modes are not evidence-based fixes under a
5120 MiB app budget shared with JIT/GPU allocations. The Windows KEvent slab is
already increased to 20,000; the ordinary event resource limit remains 900.
Thread limits are 800 and transfer-memory count is 200. These do not silently
map or skip heap pages.

The loader's `Read heap size 0x6e6f69` is misleading: NPDM offset `0x28` lies
inside `application_name` (starts at `0x20`), not a heap-size field. The bytes
spell `ion` in little endian. That read only selects an IPC pointer-buffer size
(`0x8000` here); it does not set guest heap size. Left unchanged to keep this
patch focused. The actual process metadata comes from the patched ExeFS loader.

## Patch and console verification

| File | Change / diagnostic |
| --- | --- |
| `src/core/memory.cpp` | Correct page-crossing scalar reads/writes. `NXBOX MEM_SPLIT` reports the first crossing with noncontiguous or absent backing, its size/direction and whether each side is mapped. `NXBOX MEM_UNMAPPED` reports the first scalar fault and filtered/raw page entries, once per Memory instance. |
| `src/core/hle/kernel/k_process.cpp` | `NXBOX MEM_LAYOUT`: loaded code range, physical pool size, NPDM system-resource size, heap/alias/stack reservations. Ranges use start + size. |
| `src/core/hle/kernel/svc/svc_physical_memory.cpp` | `NXBOX MEM_SVC SetHeapSize`: successful base/size/end or failed size/result/resource usage/limit. `MapPhysicalMemory`: all failure returns, requested range, resource usage/limit and system-resource size. Both guest ABIs use these wrappers. |
| `src/core/hle/kernel/svc/svc_memory.cpp` | `NXBOX MEM_SVC MapMemory`: all failure returns, destination/source/size/result. |

Use an Info-level log filter (including Kernel, Kernel.SVC and HW.Memory), preserve
the same update/settings/save, and collect both Eden and frontend logs after boot:

```sh
grep -anE 'NXBOX MEM_(LAYOUT|SVC|SPLIT|UNMAPPED)' eden_log.txt
grep -anE 'NXBOX MEM_SVC .*result=0x[1-9a-fA-F]' eden_log.txt
grep -an -m 10 'Unmapped' eden_log.txt
grep -an 'GAME_PRESENT' eden_uwp_diag.txt
```

Confirmation requires passing the previous 16–17 s failure point, presenting
frames, and no recurring unmapped-access sweep. `MEM_SPLIT` with both sides mapped
before that point demonstrates an access for which the old code used the wrong
backing. No `MEM_SPLIT` before the hang weakens hypothesis 1. A remaining hang is
**not** fixed merely because the SVC diagnostics report success.

If it still fails, compare the first fault with the latest successful heap
`[base,end)` and preceding failures. Result `0x10801` means `LimitReached`;
`0xd001` means `OutOfMemory`; `0xce01` means `OutOfResource`. A nonzero raw backing
entry with zero filtered entry points to sparse-vector bookkeeping; both zero
means the entry really is absent at inspection time (including legitimate
unmapping). These are separate reads, so concurrent mapping changes remain
possible. Retest BotW 1.0, Mario 3D World and P5R for regressions; the later BotW
1.0 D3D12 removal remains outside this patch.

Static review and `git diff --check` passed. Clang-format was applied to changed
code, preserving unrelated formatting. Runtime regression case for the target:
map consecutive guest pages to separated physical pages, place sentinel bytes in
the physical gap, and read/write 16/32/64 bits across the boundary. Reads must
combine the two guest pages and writes must leave the gap intact; repeat with the
second guest page unmapped. This test and console validation remain unexecuted.
