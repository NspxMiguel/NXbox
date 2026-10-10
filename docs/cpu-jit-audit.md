# Guest CPU / JIT audit

Audited 2026-10-10 at `c2fb1b113f`, using the maintenance map and
[GPU wait audit](perf-waits.md). Comparison is this checkout's `uwp-x64` preset
(`WindowsStore`, MSVC x64) against its Windows x64 desktop defaults, with the same guest
and CPU accuracy. This is a source audit, not a measurement of BotW on Xbox. The reported
6–10 FPS and 5120 MiB process limit are the investigation context; no FPS gain is claimed.

The UWP memory path deliberately disables fastmem. Placeholder symbol resolution is already
present, but its result is not what selects the private backing. The most actionable timing
issue is incomplete TSC frequency detection. A second issue is that the Windows timed wait
uses raw TSC deadlines even when the clock has fallen back to nanoseconds. Both have guarded,
reversible changes below. No GPU wait policy was changed.

## Ranked comparison

Rank prioritizes changes that can be made safely now, followed by potentially larger costs
that need console evidence or a new memory design. Values are defaults unless stated otherwise;
`eden_settings.txt` can override CPU accuracy and multicore, so use the emitted `CPU_JIT` line.
Impact is qualitative and conditional, not an FPS prediction. The common/core/frontend
AppContainer definitions are explicit because the Store CRT build can still report the desktop
WINAPI partition. Dynarmic has its own DYNARMIC_UWP_APPCONTAINER option. No _GAMING_XBOX
branch was found in the audited CPU/JIT/common source; that GDK macro is not a substitute for
these UWP definitions. The only WINAPI_FAMILY fast-path selection here is HostMemory.

| Rank | Setting / path | UWP value / behavior | Windows x64 desktop value / behavior | Expected performance impact | Safe to change? |
| --- | --- | --- | --- | --- | --- |
| 1 | Host clock, `cpu_features.cpp`, `GetWallClock()` | Previously native only if invariant CPUID bit **and** detected frequency >1 GHz; missing leaf 0x15 or zero crystal can leave frequency zero. Now calibrates those failures against monotonic QPC, with validation. | Original detection unchanged: estimate only inside the leaf-0x15 branch when denominator is zero. Same failure possible on a desktop CPU exposing incomplete leaves. | Removes repeated `steady_clock` reads/conversions in timing and guest CNTPCT callbacks if detection was broken. QPC can already be inexpensive; gain unknown. | **Implemented**, `NXBOX_NATIVE_CLOCK=0` restores original selection. Never forces invariant support. |
| 2 | `Common::Event::WaitFor`, `HostTiming` | Previously raw TSC + MWAITX/UMWAIT or `NtDelayExecution` tick polling. Now predicate-based condition-variable timed wait with notification. | Original raw-TSC wait branches. | Correct deadline units even when TSC detection fails; scheduled-event notifications wake immediately rather than awaiting the next polling tick. May reduce redundant wakeups; CRT timeout granularity may be worse than a hardware wait for very short deadlines. | **Implemented**, `NXBOX_EVENT_WAIT=0` restores legacy wait. A/B Xbox timing required. |
| 3 | Fastmem / `HostMemory` virtual base | `Settings::IsFastmemEnabled()` unconditionally false under `YUZU_UWP_APPCONTAINER`; private demand-committed DRAM, `virtual_base=nullptr`; frontend also sets both fastmem settings false. | Normally enabled; shared section backing plus 512 GiB arena reservation (+alignment padding), or null arena if initialization fails. | Potentially large: each eligible guest load/store uses address translation rather than a base-plus-offset access. | **No simple safe switch.** Private backing cannot be aliased with `MapViewOfFile3FromApp`; eager shared-section commit competes with the 5 GiB limit. |
| 4 | A64 `collect_memory_access_context` | Always true under `NXBOX_UWP`: two immediate PC stores before each memory IR instruction; register/SP/NZCV/vector stores cannot be eliminated across memory operations. | False by default. | A direct steady-state instruction/cache cost; potentially substantial in memory-heavy code even with warm JIT. | Keep precise fault context. A future opt-out could trade diagnostic fidelity for speed, but this audit does not establish all callback/abort behavior without it. |
| 5 | Code cache (`ArmDynarmic32/64::MakeJit`) | 128 MiB per active JIT; null JIT 8 MiB. Lazy private commit, single RW/RX allocation. | 512 MiB per active x64 JIT; null JIT 8 MiB. | More capacity flushes/recompilation if a game's working set exceeds 128 MiB. Four active caches have a nominal 512 MiB capacity instead of 2 GiB. | Do not raise unconditionally. Check `jit_cacheN`, clear counts, memory peaks; four 512 MiB caches add 1536 MiB of possible commit. |
| 6 | A64 fast-dispatch table | 65,536 tagged entries, 1 MiB/core. | 1,048,576 tagged entries, 16 MiB/core. | More collisions can enter `LookupBlock`; smaller cache saves 60 MiB across four cores and improves cache locality. FastDispatch itself remains enabled. | Semantically safe to enlarge, but performance direction and memory headroom are unmeasured. ARM32 retains its original table size. |
| 7 | JIT page protection / instruction cache | Forced `DYNARMIC_UWP_APPCONTAINER`, W^X; `VirtualAllocFromApp`, page-granular `VirtualProtectFromApp`, explicit instruction-cache flush. | RWX by default; optional no-execute mode flips protections (whole committed region on Windows). | Compile/link/invalidate overhead; does not add a permission syscall to every execution of a warmed block. | W^X is required. Page-granular writes and immutable prelude handling are already optimized; never replace with RWX. |
| 8 | Core thread priorities | `CpuManager` requests Critical; common layer clamps to Normal. HostTiming requests VeryHigh, also becomes Normal. GPU shares this clamp. | TIME_CRITICAL CPU cores, HIGHEST HostTiming (subject to OS acceptance). | Can lose scheduling share, but raising priority can starve Mesa and system work needed to unblock emulation. | **No** blanket increase: source records console freezes and Device Portal starvation. |
| 9 | Page-table / slow callbacks | Enabled; packed absolute-offset pointers, attribute masking, bounds/tag checks and boundary-only misalignment checks. Invalid/nonordinary/split accesses call callbacks. Table storage uses sparse private commitment. | Same page-table configuration as fallback; eligible application memory generally uses fastmem first. | Translation costs and exceptional callbacks remain; this is not an interpreter or callback for every ordinary load. First touches cost VEH commit; subsequent touches do not. | Keep bounds, special mappings and invalid-access handling. No evidence that disabling checks is safe. |
| 10 | Exclusives / global monitor | `fastmem_exclusive_access=false` because arena is absent. Global Dynarmic monitor supplied for four cores; callback/page-table paths remain. | Fastmem exclusives on with an arena; recompiles failed exclusive fastmem accesses. Same global monitor. | Extra callback/monitor work in synchronization-heavy guest code. | Cannot enable inline fastmem exclusives without an arena. Ignoring more monitor semantics is not a safe general optimization. |
| 11 | `cpu_accuracy` and unsafe flags | Auto by default, same as desktop; settings file can change it. All unsafe setting defaults true, but Auto selects its own curated subset. | Same. | No default UWP handicap from accuracy selection; changing Auto to Unsafe adds approximations, not guaranteed speed. | Keep Auto; verify effective setting rather than forcing Unsafe. |
| 12 | `cpuopt_*` safe optimizer toggles | All default true except the frontend's two fastmem assignments. Block linking, RSB, FastDispatch, GetSetElimination, ConstProp, MiscIR remain on. | All default true. | No UWP-wide disabling of these optimizers. Context preservation is a separate limit on GetSetElimination (rank 4). | Already enabled; toggles are consulted primarily in Debugging accuracy. |
| 13 | `use_multi_core` / cycle counting | True; four guest-core host threads; wall-clock CNTPCT enabled and JIT cycle counting disabled. | Same. Single-core mode uses cycle counting and core rotation. | No default single-thread bottleneck introduced by the UWP port. | Keep enabled; log settings-file overrides. Xbox's eight physical cores do not imply eight guest cores. |
| 14 | Fiber stacks (`fiber.cpp`) | 512 KiB stack + 512 KiB rewind stack per FiberImpl. Uninitialized arrays avoid eager zero touches. | 2 MiB + 2 MiB. Same Boost.Context switches, uninitialized storage in this checkout. | Saves allocation/commit pressure; no extra operation per fiber switch. | Keep; further shrink needs stack-depth evidence. Canary checks do not substitute for guard pages. |
| 15 | `patch_information` / linking metadata | One vector of typed sites per target; lookup uses `find`, no empty entry insertion; full cache clear releases bucket capacity. Incoming sites survive target invalidation. | Four `small_vector<CodePtr,4>` fields; Patch can insert an empty target record. | Saves substantial metadata and avoids useless allocations. Heap growth/type-switch work is a compile/link cost, not loss of block linking. | Already improved; do not delete sites on target invalidation, which would break relinking. |
| 16 | Spin locks / assertions / host ISA | Host SpinLock uses Interlocked exchange + pause; emitted guest lock unchanged. Inline successful JIT assertions, no block-friendly-name formatting for the no-op Windows perf map. ISA detection ON. | Host SpinLock uses generated xchg/pause and call_once; ordinary assertions/perf-map call sites. ISA detection ON. | Existing UWP bookkeeping savings. AVX/AVX2/FMA/AES remain detected; FastBMI2 intentionally excluded for pre-Zen-3 AMD. | Keep sandbox-safe lock and ISA checks. Do not force costly Zen-2 PDEP/PEXT paths. |
| 17 | 1 ms timer request | `RunGame` dynamically tries `NtSetTimerResolution(10000, TRUE)` and logs status/actual in 100 ns units. No matched release here. | Qt/CLI request maximum resolution and high QoS through `Common::Windows`, rather than a fixed 1 ms target. | Affects timed sleep expiry; not JIT execution speed or QPC precision. Xbox acceptance is unknown without the diagnostic. | Not replaced by `timeBeginPeriod` (desktop-only requirement). Treat existing NT request as best effort, not a documented UWP guarantee. |

## Settings and exclusive paths

`MakeJit` starts with Dynarmic's safe optimization mask. Debugging selectively clears flags:
`cpuopt_page_tables`, `cpuopt_block_linking`, `cpuopt_return_stack_buffer`,
`cpuopt_fast_dispatcher`, `cpuopt_context_elimination`, `cpuopt_const_prop`, `cpuopt_misc_ir`,
`cpuopt_reduce_misalign_checks`, `cpuopt_fastmem`, `cpuopt_fastmem_exclusives`,
`cpuopt_recompile_exclusives`, `cpuopt_ignore_memory_aborts`. Accurate retains safe defaults;
Paranoid clears optimizations. Neither overrides UWP's final fastmem veto.

A64 Auto adds UnfuseFMA and IgnoreGlobalMonitor, permits unsafe optimizations and sets fastmem
address-space bits to 64 (irrelevant with a null fastmem pointer). A64 Unsafe follows the
settings for host MMU, unfused FMA, reduced FP error, inaccurate NaNs, fastmem address checking
and ignore-global-monitor; `cpuopt_unsafe_ignore_standard_fpcr` is not consumed by A64 MakeJit.
A32 Auto additionally selects IgnoreStandardFPCRValue and InaccurateNaN; A32 Unsafe consumes
ignore-standard-FPCR but not the A64 fastmem-address-width setting. These differences are between
guest architectures, not UWP and desktop. BotW is A64. No unsafe setting was changed here.

The monitor is not absent in UWP: `config.global_monitor` and `processor_id` are still provided,
and RunThread clears local exclusive state. Inline emitters can skip monitor operations under
IgnoreGlobalMonitor, while callback implementations still use the supplied monitor where called.
The no-arena path chooses `EmitExclusiveRead/WriteMemory`, not the fastmem-inline variants.
Do not infer that Auto removes every exclusive lock or callback.

## Why fastmem remains disabled

Relevant source: `host_memory.cpp:Impl::Init`, `DeviceMemory`, `settings.cpp:IsFastmemEnabled`,
`memory.cpp:SetCurrentPageTable`, and both ARM Dynarmic MakeJit implementations.

The AppContainer branch resolves `VirtualAlloc2FromApp` and `MapViewOfFile3FromApp`, but does not
use them to reserve or map a virtual arena. It instead reserves private DRAM with
`VirtualAllocFromApp`; a VEH commits 64 KiB chunks on first touch. Init returns success with a
null virtual base. This is intentional, not a failed placeholder allocation triggering the
ordinary `HostMemory` fallback. Desktop does require its section/placeholder exports and can
fall back to a linear allocation if initialization fails.

A 512 GiB virtual reservation does not itself require 512 GiB of physical memory; the old
settings comment about an arena exceeding the memory budget is not a sufficient diagnosis.
The blocking issue is coherent aliasable backing with a proven commit policy. The source records
an on-console SEC_RESERVE section failure: committing its mapped pages through the private-memory
API access-violated. Switching to SEC_COMMIT would reserve the full guest DRAM's commitment plus
JIT/graphics/frontend overhead under 5120 MiB. Copying private pages into a second arena would
break alias coherence and exclusive semantics.

The existing x64 Windows Dynarmic exception handler registers unwind tables and reports fastmem
support when registered; it has no separate UWP veto. That does not prove fault/recompile behavior
with a new Xbox arena. Re-enabling requires a tested shared backing design, accounting of unique
physical commit, placeholder splitting/unmapping, permission changes and fault fallback. It is
not achieved by removing the settings guard alone.

The documented placeholder APIs are not categorically unavailable to Store JIT apps, but their
Microsoft requirement tables are inconsistent with their Store-app remarks. Both
[VirtualAlloc2FromApp](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc2fromapp)
and [MapViewOfFile3FromApp](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile3fromapp)
list Windows 10 / Windows Server 2016, `memoryapi.h`, `WindowsApp.lib` and `Kernel32.dll`;
the tables say desktop-only while the remarks discuss Store/codeGeneration use. Export presence
on the Xbox OS must be measured. This change adds no use of either API.

## Implemented timing changes and reversion

This checkout no longer contains a separate `NativeClock` class: `Common::WallClock` in
`cpu_features.cpp` incorporates the native/fallback paths. Invariant detection uses CPUID
0x80000007 EDX bit 8. The old frequency estimate runs only if leaf 0x15 exists and its
denominator is zero; it also measures RealTimeClock rather than a monotonic clock.

`GetWallClock()` leaves the original global clock and desktop selection intact. On UWP x64 its
immutable function-local clock is selected once, after `nxbox_env.txt` has been loaded. RunGame
initializes and logs it before guest timing starts. CoreTiming, Event deadlines and presentation
timestamps all use this accessor, so clocks with different epochs are not mixed.

When invariant support is present but reported frequency is <=1 GHz, calibration takes two
100 ms samples using the existing fenced RDTSC and `Common::SteadyClock` (QPC on Windows).
Actual elapsed time is used, including oversleep. Samples outside 50 ms–5 s, invalid/reversed
counters, rates outside (1, 10] GHz or disagreement above 0.5% are rejected. Accepted rates are
rounded to 100 kHz as in the existing estimator. Valid existing frequencies are retained; failed
calibration and non-invariant CPUs retain the original fallback. This does not assume a fixed
Xbox frequency, force support bits, or use CPUID base/boost MHz as the TSC frequency.

Microsoft recommends QPC for general portability; invariant detection and local calibration do
not prove cross-core synchronization on every possible machine or VM. This UWP change follows
Eden's existing native-clock contract on the fixed Xbox target. Check monotonicity after thread
migration and suspend/resume on console. The fallback is not automatically a kernel call or a
slow path: Windows may implement QPC using a synchronized TSC.
[Microsoft QPC/TSC guidance](https://learn.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps).

The old Windows Event wait converts nanoseconds with `NsToTicks` and then compares against raw
RDTSC unconditionally. A fallback WallClock supplies an identity nanosecond conversion, so an
undetected 3.5 GHz clock would wait roughly 1/3.5 of the requested interval. The new UWP path
uses the existing condition variable, mutex and atomic signal: it releases the mutex while
waiting, rechecks the predicate, consumes a signal under the same mutex as Set, and returns false
on timeout. It avoids legacy NtDelayExecution polling without introducing a spin loop. It does
not guarantee submillisecond expiry precision or grant a 1 ms timer resolution.

Use `LocalState\\nxbox_env.txt`, then restart the entire process:

```text
# Restore original clock selection (including any original detection failure).
NXBOX_NATIVE_CLOCK=0
# Restore original Event timed-wait implementation.
NXBOX_EVENT_WAIT=0
```

Remove either line to enable its new default. Overrides are exact `0`, cached per process.
`CPU_CLOCK` reports native/fallback, invariant bit, reported and selected Hz, and whether
calibration is enabled. `CPU_TIMED_WAIT` reports `notified-condition-variable` or
`legacy-tsc-poll`. `CPU_JIT` reports effective accuracy/multicore, the fastmem veto, backing,
cache/stack limits, priority and absence of affinity. Existing `NXBOX FAULT CONFIG` Eden logs
retain optimizer/context details; existing `STALL`/`jit_cacheN` lines report JIT work/capacity.

## MSVC x64 / AppContainer API requirements

The new production code adds no direct Windows imports: calibration reuses the existing clock
wrapper and fenced intrinsic; notified waits use the standard UWP CRT synchronization already
used by `Event::Wait` and `Set`. No manifest capability or linker library was added.

| API / facility used by the changes | Microsoft requirement / source |
| --- | --- |
| `QueryPerformanceCounter` through SteadyClock | Windows 2000 Professional / Windows 2000 Server, explicitly desktop and UWP; `profileapi.h` (Windows.h), Kernel32.lib / Kernel32.dll. [Requirements](https://learn.microsoft.com/en-us/windows/win32/api/profileapi/nf-profileapi-queryperformancecounter). |
| `QueryPerformanceFrequency` through SteadyClock | Same desktop/UWP minimums and header/library; cached frequency is fixed at boot. [Requirements](https://learn.microsoft.com/en-us/windows/win32/api/profileapi/nf-profileapi-queryperformancefrequency). |
| `__rdtsc` in existing FencedRDTSC | MSVC x86/x64 intrinsic in `<intrin.h>`; emits an instruction, not a Windows API import. Existing lfence/compiler barriers retained. [Requirements](https://learn.microsoft.com/en-us/cpp/intrinsics/rdtsc). |
| `std::this_thread::sleep_for`, `std::condition_variable::wait_for` | Existing standard CRT facilities, not manually resolved undocumented NT entry points. The Windows synchronization primitive `SleepConditionVariableSRW` explicitly supports desktop/UWP since Vista / Server 2008, with synchapi.h and Kernel32.lib / Kernel32.dll. The CRT sleep uses the Windows Sleep facility, documented for desktop/UWP (XP / Server 2003 minimums; Store support from Windows 8.1), synchapi.h and Kernel32.lib / Kernel32.dll. [Sleep requirements](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-sleep). Predicate handles spurious/stolen wakes. [Requirements](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-sleepconditionvariablesrw). |

Existing memory/JIT APIs are unchanged. `VirtualProtectFromApp` disallows RWX, permits RX with
codeGeneration, and requires instruction-cache coherence; this is why the desktop RWX path
cannot simply be reused. [Microsoft contract](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualprotectfromapp).

The pre-existing 1 ms NT request is not evidence of documented UWP support. Only a successful
`TIMER_RESOLUTION` status plus actual <=10000 indicates the request reported <=1 ms for that
run; wake latency still needs measurement. The fallback result `status=-1, actual=0` is failure,
not zero resolution. `timeBeginPeriod` is documented desktop-only and does not improve QPC
precision, so it was not introduced as a substitute.
[Microsoft timer requirements](https://learn.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timebeginperiod).
Legacy NT imports in `windows/timer_resolution.cpp` predate this audit and remain a separate
packaging/compatibility issue; the default new Event path does not call NtDelayExecution.

## Validation and console follow-up

`tests/port/test_cpu_jit_policy.py` compiles the std-only policy and the production notified Event
method with a host compiler. It checks missing frequency, override/non-invariant behavior,
preservation of valid rates, measured oversleep, invalid intervals/rates and unstable samples;
it also checks real timeout, pre-signaled zero timeout, notification and signal consumption.
The `=0` host wait case uses the pre-existing POSIX condition-variable fallback, not a simulation
of Windows MWAITX/NT behavior. Windows/Xbox integration is still required.

Run the acceptance suite:

```sh
cd tests/port
python3 -m unittest discover -s . -p 'test_*.py'
```

Acceptance passed on this Mac: **139 tests in 83.264 s, OK**. `git diff --check` also
passed. C++ additions/changed ranges were run through clang-format and the Python test through
ruff format; no unrelated full-file reformat was applied.

No MSVC toolchain or Xbox runtime was used on this Mac. Compilation claims are limited to source
compatibility and documented API usage, not a completed UWP binary build. No commit or push.
Console A/B: same BotW save, camera and open-world route after shader/JIT warm-up; four runs with
defaults, NATIVE_CLOCK=0, EVENT_WAIT=0, and both zero. Retain CPU_CLOCK, CPU_TIMED_WAIT,
TIMER_RESOLUTION, CPU_JIT, SAMPLER, STALL, cache occupancy and memory peak. Compare wake latency,
FPS, guest-clock progression, rendering, audio, suspend/resume and faults. Keep GPU switches
identical and GPU profiling off, since profiling can restore submission drains independently.
