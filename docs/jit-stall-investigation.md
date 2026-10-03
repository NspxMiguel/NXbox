# P5R JIT stall investigation

The supplied capture does not establish continuous steady-state recompilation.
`~/.local/share/nxbox/artifacts/stall-0.3.184/eden_uwp_diag.txt` contains 79 windows,
128,826 compilation calls and 67,075 ms of JIT host time (0.521 ms/call overall).
23 windows have no compilation; the last 20 contain only 760 calls / 889 ms,
with at most 139 calls in one window. The three example triples in the task are
absent from this file. They may describe another capture. No runtime engine
(Unity, IL2CPP, Mono, etc.) is assumed from the game's name.

Findings, ranked by evidence and diagnostic priority:

1. **Whole-region W^X is real; its share of compile time is unmeasured.**
   `src/dynarmic/src/dynarmic/backend/x64/a64_emit_x64.cpp:80` and `:166` switch
   protection for each emitted block. `block_of_code.cpp:227` and `:237` protect
   all committed bytes, initially committed in a 16 MiB chunk, rather than just
   the emitted block. Invalidation also switches protections, including twice
   per unpatched descriptor to call the generated dispatch lookup
   (`a64_emit_x64.cpp:797`). This explains a possible cost multiplier, not extra
   compilation calls. There is one RW/RX allocation, not a separate writable view.
   Protecting only newly emitted pages is unsafe without tracking old incoming
   patch sites (`emit_x64.cpp:367`); that redesign is intentionally deferred.
2. **Invalidation-driven recompilation remains unmeasured.**
   `a64_interface.cpp:259` starts the JIT timer only after the authoritative block
   map misses. `jitflush` counts only the less-than-1-MiB capacity evacuation;
   explicit clears and range removals previously had no counters
   (`a64_interface.cpp:112`, `:121`, `:311`). The page-table helper broadcasts to
   each CPU, but its callers are code unmapping, executable permission changes,
   and debug writes (`src/core/hle/kernel/k_page_table_base.cpp:1349`, `:2087`,
   `:3332`), not every ordinary MapMemory/UnmapMemory or IPC copy. Guest IC
   instructions have a separate route (`src/core/arm/dynarmic/arm_dynarmic_64.cpp:91`).
   Data-cache callbacks are disabled by default (`src/dynarmic/src/dynarmic/interface/A64/config.h:234`).
3. **Capacity thrashing is not supported by this capture.**
   UWP uses 128 MiB per JIT versus desktop's 512 MiB
   (`src/core/arm/dynarmic/arm_dynarmic_64.cpp:253`); all 79 `jitflush` counts are
   zero. Range invalidations remove descriptors without reclaiming emitted code;
   only a whole-cache clear rewinds the allocation. The budget is unchanged.
4. **No lookup-key defect or UWP optimization disablement was found.**
   Fast-dispatch misses still consult the block map (`emit_x64.cpp:55`). Its key
   contains PC, masked FPCR and single-step state, not FPSR
   (`src/dynarmic/src/dynarmic/frontend/A64/a64_location_descriptor.h:31`,
   `src/dynarmic/src/dynarmic/backend/x64/a64_jitstate.h:77`). Normal Run does not
   introduce single-step state. `MemoryReadCode` supplies instruction bytes,
   independently of fastmem. UWP forces fastmem off (`src/common/settings.cpp:182`)
   but retains page-table accesses and the Auto optimization path
   (`arm_dynarmic_64.cpp:336`, `:351`); safe passes default on and run through
   `src/dynarmic/src/dynarmic/ir/opt_passes.cpp:1464`. Settings overrides/debugging
   can still change this. Disabling passes or changing FP semantics is not justified.

The behavioral fix is an early return for an empty descriptor set in
`src/dynarmic/src/dynarmic/backend/x64/emit_x64.cpp:420`. It avoids two whole-cache
protection calls when there is nothing to unpatch or remove. Live invalidations,
RSB handling and guest instruction-cache behavior are unchanged. A misleading
comment about a separate writable view was also corrected.

The next console run prints these fields on `GAME_STALL`
(`src/eden_uwp/game_session.cpp:609`; storage in `src/common/nxbox_stall.h:100`):

| Fields | Meaning / confirming observation |
| --- | --- |
| `jit_inv_calls`, `jit_inv_bytes` | A64 range requests and requested bytes, summed before coalescing and across cores. High requests alone do not prove eviction. |
| `jit_range_blocks`, `jit_inv_empty` | Actual descriptors removed by the shared x64 emitter; empty descriptor sets skipped by the fix. High removals plus repeated keys support invalidation churn. Historical range entries can yield nonempty sets with no live blocks, so empty counts are not all ineffective requests. |
| `jit_ic`, `jit_pt` | A64 IC callbacks (including ignored operations), and page-table invalidations forwarded to CPU interfaces. These distinguish guest cache maintenance from kernel code-mapping/debug-write routes. |
| `jit_clear_req`, `jit_clears`, `jit_clear_blocks` | Explicit A64 clear requests, executed A64 clears, and descriptors removed by shared x64 full clears. Requests can coalesce. Clears with `jitflush=0` reveal non-capacity flushing. |
| `jit_cache0` ... `jit_cache3` | Latest used/capacity bytes per processor ID, including prelude, sampled after compilation/clear. Capacity pressure needs `jitflush>0`; a clear may hide the preceding peak fill. Multiple processes share a slot; these are not allocation totals or committed-memory readings. |
| `jit_pc_new`, `jit_pc_repeat`, `jit_pc_max` | First-seen PCs, compilations of previously seen PCs, and maximum lifetime compile count of any tracked PC observed this window. |
| `jit_key_new`, `jit_key_repeat` | First-seen full descriptors versus descriptors compiled again. PC repeats with new keys suggest FPCR/single-step variants; repeated keys plus evictions support recompilation. Repeated keys without corresponding removals/clears merit lookup investigation. Mostly new PCs supports continued code discovery. |
| `jit_pc_unknown`, `jit_key_unknown` | History saturation; absent values cannot be classified after the tracking limit. Do not treat them as new PCs/keys. |
| `jittranslate`, `jitoptimize`, `jitemit`, `jitprotect`, `jitinvalidate` | Timed total-ms/max-ms/call-count triples. High `jitprotect` relative to `jitemit` implicates W^X; high `jitoptimize` or `jittranslate` identifies another compile stage. Timings nest: do not sum them into JIT time. Protection also occurs during initialization and invalidation. |
| `jit_protect_bytes` | Sum of requested protection spans, including repeated flips; not memory allocated. |

Raw counters use brackets, e.g. `jit_inv_calls=[42]`, and cache gauges use
`jit_cache0=[16777216/134217728]`. This lets the existing duration-only
`stall-report.py` ignore them without misinterpreting bytes as milliseconds.
All event fields are window deltas except `jit_pc_max`; gauges persist. Independent
relaxed atomics mean adjacent-window totals can straddle a sampling boundary.
Histories are per JIT, survive cache clears, and are capped at 65,536 PCs plus
65,536 keys each. Migration to another CPU is first-seen there; histories reset
when a JIT is recreated. No successful block-lookup path gains profiling work.
Compile **every target**, including dynarmic, with `NXBOX_STALL_PROFILE=0` to
remove timers, event recording, histories and `GAME_STALL` formatting. The default
remains enabled, matching the existing profiler.

The GPU hitch is separate: capture line 71 has `gpu=6816/4898/59`, so 6.816 s is
summed work and 4.898 s the longest command. The scope measures host GPU-thread
command processing, including waits, not hardware GPU execution
(`src/video_core/gpu_thread.cpp:44`). JIT counters cannot assign that hitch's cause.

Validation: source/control-flow review, the supplied log summarizer, independent
capture aggregation, clang-format on changed lines, and `git diff --check`.
No build, emulator execution, console test, commit or push was performed. The
cause of the reported persistent high-compilation windows remains unproven until
the new identity and invalidation counters are captured in that scene.
