# Submit journal: package 0.3.259 follow-up

The local `pipe6-6bf5c623b/mk8-diag.txt` contains all 13 entries, numbered
0 through 12. The producer puts newline-separated entries into environment
chunks, and the frontend previously prefixed the entire chunk only once.
`Diagnostic` preserves newlines. Filtering for `D3D12_BATCH_JOURNAL` therefore
kept only entry 0 in `mk8-journal.txt`. This capture was not truncated by the
8192-byte reader or the 4096-byte producer chunk boundary.

The requested `pipe6-6bf5c623b/botw-diag.txt` is absent locally. Captures from
other commits cannot establish what BOTW did with this package.

## What MK8 establishes

- Removal is `0x887a0001`, first noticed at `fence-before-wait`. The snapshot
  names submit 21474838111, batch slot 6, fence 1642, already marked completed.
  This is a last checked submission, not proof of the causative batch.
- Sampled batch reports show successful Close and no removal; PSO counters
  reach 24 created, zero failures. This does not establish draw-time validity.
- DRED queries succeed but return zero breadcrumb nodes and no page-fault VA
  or allocation records. There is no DRED command-level attribution.
- Entry 0 only binds a graphics root signature. Later entries bind two PSOs
  and draw with matching reported PSO/bound RTV formats (61 and 24), no DSV,
  and one sample. Nothing here proves a root-signature or RTV mismatch.
- Entry 11 is a full texture copy: destination format 83, 27x27, versus a
  28x28 placed footprint, row pitch 256. This is a concrete upload geometry
  lead to check against block-compressed edge-copy rules, not a proven cause.
  Entry 10 transitions that destination from COMMON to COPY_DEST; entry 12
  is a global UAV barrier. The journal is execution order, not GPU progress.

## Next console run

Look for `sync=1 journal=3`. Every physical entry line now starts with
`D3D12_BATCH_JOURNAL_<part>`; preceding checked-good entries use
`D3D12_BATCH_JOURNAL_PREVIOUS_<part>`. Keep both manifests:
`D3D12_FIRST_BAD_BATCH` and `D3D12_PREVIOUS_GOOD_BATCH`. Compare their submit
IDs, fence values, `parts`, `entries`, and `dropped=0`. Parts are read from the
manifest, not a fixed eight-part limit. Sync journals retain every recorded
entry rather than only the last 64; individual entry text retains its existing
480-byte bound. This increases diagnostic-mode memory use for large batches.

A dedicated queue Signal, fence wait, and post-wait GetDeviceRemovedReason
now run immediately after each graphics ExecuteCommandLists, while holding
submit_mutex, before other observers and normal batch-fence allocation.
`DRED at=sync-submit-fence` with `completed=0 attribution=in-flight-submit`
identifies the submit under that check. A removal first observed after a
successful check remains `attribution=delayed-after-checked-submit`; neither a
fence nor this journal proves which individual GPU command caused removal.
The extra drain is disabled by `NXBOX_SYNC_BATCH=0`.

Filter all journal-prefixed lines, including PREVIOUS, and inspect the draws,
barriers, and copy geometry surrounding the failure. Host tests validate
publication, formatting, snapshot retention, wait/check ordering, and opt-out;
console execution is still required to validate the driver behavior.

## Host validation

- All 63 Python tests in `tests/port` pass, including the complete patch chain
  against `/tmp/mesa-pin` with LF and CRLF inputs and repeat rejection.
- All four portable C++ tests pass when compiled directly with clang++ C++20,
  `-Wall -Wextra -Werror -pedantic` (cheats parser linked with zlib).
- CMake/CTest is unavailable on this host; fetching CMake was blocked by network
  DNS restrictions. The Windows-only sparse-memory integration test cannot run
  on this Mac. No console test or Xbox build was performed.
