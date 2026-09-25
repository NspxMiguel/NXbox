# NXbox: Xbox Series X port workbench

This is an experimental continuation of Eden's UWP work. Signed CPU and graphics diagnostics have
been installed and executed on the Xbox Series X. **No playable guest game or commercial-game FPS
has been demonstrated yet.** The gameplay frontend is undergoing its first Windows build.

## Console evidence (2026-09-23)

- CPU package `0.1.2.0`, built from emulator run `35920737218` and packaged by `35926296025`,
  completed `RunHeadlessBoot` with status 0. That status requires observing `EDEN_XBOX_JIT_ALIVE`
  from the guest through the SVC observer, followed by shutdown.
- Graphics package `0.1.6.0`, run `35926298987`, passed clear-color pixel readback, first
  presentation and all 64 compute-shader storage-buffer results. Mesa reported OpenGL 4.6 on
  `D3D12 (SraKmd_arden)`; the measured application memory limit was 5 GiB. It completed 3,590
  presentations in 60.009536 seconds. **This is a clear-screen driver test, not gameplay FPS.**
- Adding the packaged DXIL validator resolved the initial pipeline-creation crash. A later
  shared-context test (`0.1.7.0`) exited while activating the context on a worker thread. The pinned
  Mesa UWP shim calls `CoreWindow::GetForCurrentThread()` for window lookup; the worker-safe source
  patch and build are under validation.
- An earlier CPU crash reached `Kernel::KPageTableBase::SetHeapSize` while zeroing the guest heap.
  Windows eagerly committed the 4 GiB host page table. The new UWP sparse allocator reserves its
  address range and commits touched chunks; Windows integration CI verifies a 4 GiB reservation with
  only three 64 KiB chunks committed, including concurrent first touches. Standalone Xbox package
  `0.1.8.0` also passed: reported usage increased from 4,460,544 to 4,677,632 bytes while reserving
  4 GiB and touching the three regions. Full-core validation of the allocator remains in progress.
- The JIT uses one reserved code cache with protection transitions; it does not require duplicate
  writable/executable cache buffers. The temporary cache cap was reverted.

## Console evidence (2026-09-24)

- The patched Mesa (`0.1.9.0`) passed the worker-context compute test, which fixes the worker-thread
  context activation problem from 2026-09-23.
- The first game package crashed with fast-fail `0xC0000409` inside `libgallium_wgl.dll` while
  `OpenGL::UtilShaders` linked its separable compute programs. Probe `0.1.11.0` linked Eden's OpenGL
  startup shaders one by one on the worker context. Twelve passed: ASTC, unswizzle, BC4, S8D24,
  local-memory warmup, blit and present. Both `image2DMSArray` conversion programs abort Mesa's
  D3D12 backend, because D3D12 has no multisampled UAVs. WindowsStore builds define
  `NXBOX_NO_MSAA_STORAGE_IMAGES`: they skip those programs, and MSAA image copies are skipped with a
  warning.
- With that change the game package loads the homebrew and reaches `GAME_RUNNING` without crashing.
  The guest then panicked because `IpcController` 3, `set:sys` 3 and `IWindowController` 1 were
  reported as unknown. A lookup-miss diagnostic showed tables with pointer halves as command ids
  (for example, `set:sys` contained 1803350624). Only entries with a null handler kept their command
  id: `IWindowController` registered only id 0. MSVC constant-initialized the static `FunctionInfo`
  tables through the `constexpr FunctionInfoTyped` constructor and wrote the converted member
  pointers incorrectly. Pinning `ServiceFrameworkBase` to `__multiple_inheritance` changed nothing,
  so it was reverted. Removing `constexpr` makes the tables initialize at run time. Local package
  `0.1.16.2` then reached `NXBOX_PADDLE_READY` on the Xbox with no lookup misses.
- With the service tables fixed, the first present still crashed: `0xC00000FD` (stack overflow) at a
  return address inside `libgallium_wgl.dll`, with 936 repeats of the same six-frame cycle under
  `wglSwapBuffers`. The Xbox executable's default thread stack is 1 MiB; `/STACK:16777216` on
  `eden-uwp.exe` fixed that crash.
- The guest's `Controller type 0 is not supported` on every `Connect()` call was a separate bug:
  `Settings::values.players[0].controller_type` only becomes the guest's actual `NpadStyleIndex`
  through `HIDCore::ReloadInputDevices()`, which desktop frontends call from their Qt/Android
  settings UI and which the guest's own resource manager also calls, but lazily, on its first HID
  service call. The gamepad poll loop runs immediately on the host thread and raced ahead of that,
  so the controller stayed at its construction default (`NpadStyleIndex::None`). Calling
  `ReloadInputDevices()` once before the poll loop fixed it.
- With both of those fixed, the local build pipeline (build+package+deploy in under a minute on the
  Windows PC) let this be reproduced many times. The game now reliably reaches `NXBOX_PADDLE_READY`,
  connects the emulated controller, and **presents real frames** — every successful run measured
  exactly 12 presented frames (`GAME_PRESENT frames=12`), over a varying wall-clock window (5.5 to
  10.7 seconds across runs), before crashing. That fixed frame count, constant across very different
  elapsed times, rules out a time-based or purely-recursive-until-stack-exhaustion explanation: it
  is bounded by something that accumulates once per frame and hits a limit at 12.
- The crash itself: `0xC00000FD` (stack overflow) at the same return address inside
  `umd12ddi_arden.dll` (the Xbox kernel-mode D3D12 driver's user-mode component) in every
  reproduction, in a deep repeating cycle through `libgallium_wgl.dll` → `umd12ddi_arden.dll` →
  `D3D12Core.dll` → `libgallium_wgl.dll`. Raising the executable's default thread stack from 1 MiB
  to 16 MiB, then to 64 MiB, made **no difference to the crash address or the 12-frame count** —
  only to whether the crash could occur on the very first present (1 MiB) or only after 12 real
  frames (16/64 MiB). This is conclusive: it is not primarily a stack-size problem, and no further
  `/STACK` increase should be tried without new evidence. The `GAME_SUSPENDED` diagnostic logged
  just before each crash is very likely an artifact of Windows Error Reporting intercepting the
  exception (`Faultrep.dll` appears on the same crashing stack), not a real foreground app switch —
  three separate reproductions crashed the same way with no other app on the console.
- Getting a symbolized stack for this failed after real effort and should not be re-attempted the
  same way: the release Mesa build ships no PDB; the pinned UWP `aerisarn/meson` fork links the
  debug CRT (`ucrtbased.dll`, `VCRUNTIME140D_APP.dll`) into any build with `debug=true` regardless
  of `buildtype` or an explicit `-Db_vscrt=md`, including `buildtype=debugoptimized` — three
  separate CI builds all produced a Mesa DLL that fails `LoadPackagedLibrary` on the Xbox devkit
  (`ERROR_MOD_NOT_FOUND`, 126) before it can even be tested; and the minidump's module record for
  `umd12ddi_arden.dll` has no CodeView entry (`cv_size=0`), so its PDB signature cannot be recovered
  from the dump to query Microsoft's public symbol server either. `tools/nxbox/build-mesa.cmd` is
  back to the known-working release config. Symbolizing this driver-internal crash needs either a
  genuine Windows debugging session (WinDbg with the Xbox devkit's own symbol path) or Microsoft's
  own tools, neither available here.
- Tested and ruled out: `d3d12_wgl_framebuffer_present` unconditionally set
  `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` and called `Present(0, DXGI_PRESENT_ALLOW_TEARING)` whenever
  `interval < 1`. `patch_mesa_uwp.py` now also patches that call to always
  `Present(interval < 1 ? 1 : interval, 0)`, on the reasoning that Xbox's fixed-refresh compositor
  may not support tearing presents. A fresh Mesa build with this patch was deployed and tested: the
  crash is identical (`0xC00000FD` at `umd12ddi_arden.dll+0xab739`, one instruction from the earlier
  `+0xab74f`) after the same 12 presented frames. The patch is kept anyway (vsync-always is the more
  correct choice on a fixed-refresh console regardless), but it is not the cause and no further
  present-flag changes should be tried on this theory.
- A black screen in the other agent's app coincided with NXbox running. Two apps in the foreground
  on one console suspend each other. Test one app at a time.
- Using sccache with embedded debug info reduced a full CI rebuild from about 60 to 18 minutes.
  Successful builds are now packaged automatically as the game payload with Mesa run `35928958265`.

## Source provenance

- Base: [juanresendiz813/eden-xbox](https://github.com/juanresendiz813/eden-xbox), `development`,
  commit `33969e85769916535d02f124705ff7aa02086775`.
- The `src/eden_uwp` frontend was imported from the same repository's `feature/uwp-boot-appx`,
  commit `5b146f9a5ec3cffad462cf986c6c284df58bf316`. It was missing from the development snapshot.
  Original copyright and GPL notices are retained.
- Local branch: `port/xbox-series-x`. This checkout is independent of `../XboxDev`; console
  diagnostics use separate NXbox package identities and do not modify Nativra.

## Product requirements

Aim for mature Yuzu-class compatibility with minimal user intervention. This is an acceptance
target, **not a compatibility claim or a promise to run every game**.

- One-time setup: import user-provided content and any required keys/firmware through a guided flow;
  validate before copying and retain working configuration.
- No keys are needed for the homebrew CPU smoke test. Reimplementing system services can reduce
  firmware dependencies; it cannot decrypt encrypted content without the corresponding key material.
- Discover the selected game folders, present a library, and launch with the Xbox controller. Avoid
  mandatory per-game settings.
- Provide automatic controller mapping, conservative graphics defaults and versioned compatibility
  profiles where necessary.
- Manage saves within app-owned storage; preserve saves and imported configuration across updates.
  Uninstalling is not an update strategy.
- Keep a last-known-good configuration and explain missing requirements with one actionable next
  step.
- Target minimal maintenance **for the user**. Emulator fixes, compatibility testing and platform
  updates still require project maintenance.
- Do not bundle or fetch proprietary keys, firmware, or games.

## First implementation increment

1. Wire the imported `eden-uwp` IFrameworkView frontend into WindowsStore builds. It selects null
   video/audio and disables fastmem, then waits for a homebrew to emit `EDEN_XBOX_JIT_ALIVE` through
   `svcOutputDebugString`.
2. Initialize the worker's WinRT apartment, synchronize the condition-variable predicate, and detach
   the observer on exceptional exit as well as normal shutdown.
3. Fix UWP demand-commit dispatch: reject malformed records and execute/unknown access kinds;
   calculate chunks with integer bounds and overflow checks. Previously an execute fault inside the
   data backing could repeatedly commit read/write pages and retry an instruction that still could
   not execute.
4. Add standalone regression tests without downloading the emulator dependency graph, plus a
   portable CI workflow. The Windows emulator build and portable tests have passed; renderer
   integration is ongoing.

The policy tests cover first/last pages, chunk boundaries, out-of-range access, execute access,
missing reservations, overflow and every byte in a 132 KiB sample reservation. A separate
Windows-only integration executable exercises real reservations, commits, concurrent first-touch
faults and release. Neither test suite alone proves Xbox runtime compatibility.

## Reproduce local validation

```sh
cmake -S tests/port -B build-port-tests
cmake --build build-port-tests --config Release --parallel 2
ctest --test-dir build-port-tests -C Release --output-on-failure
```

A direct Clang sanitizer run is also possible:

```sh
mkdir -p build-port-tests
clang++ -std=c++20 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -Isrc tests/port/demand_commit.cpp \
  -o build-port-tests/demand-commit-tests
./build-port-tests/demand-commit-tests
```

## Next gates, in order

1. **Windows compile/link:** passed for the CPU frontend; repeat with the OpenGL gameplay frontend.
   Use CMake 3.31 or newer and the documented UWP compiler environment.
2. **Package and boot:** passed for the signed CPU and standalone graphics diagnostics. Preserve
   identity and user data when updating a future launcher.
3. **CPU and memory:** the JIT guest sentinel passed. Validate sparse page-table storage on-console
   before measuring the larger guest workload; the measured application budget is 5 GiB.
4. **Graphics:** implement and validate the Switch GPU backend for Direct3D, including shader
   translation, synchronization, texture formats and presentation. The base currently contains
   Vulkan/OpenGL/null renderers, not a working D3D12 renderer. A triangle alone will not establish
   game compatibility.
5. **Audio, input, storage and lifecycle:** exercise suspend/resume, reconnecting controllers,
   storage removal, and saves before a general launcher.
6. **Compatibility:** run a repeatable title-by-title matrix, recording boot, visuals, input, audio,
   saves, performance and regressions. Mark unsupported titles honestly.

Known inherited risks still need investigation: read/write protection faults on already committed
backing pages, global demand-commit state lifetime, static dependency CRT/import compatibility, and
timeout/shutdown behavior when a guest fails to stop. The initial execute-fault fix does not resolve
those separately.

## Performance acceptance

The requested minimum is 30 FPS, with 60 FPS or higher as the target where the game supports it.
This is a requirement to measure, not an achieved result. Record the title/update, emulator
revision, resolution, mods, upscaler, shader-cache state, measurement duration, average FPS, 1% low
and frame-time distribution. Use completed guest frames per host wall-clock second. Do not
substitute UI refresh rate, a clear-screen benchmark, the guest clock, or generated frames. Validate
input, audio, saving/loading and sustained play alongside performance.

## Standalone graphics investigation

`tests/xbox-graphics` builds independently of the emulator. It probes the
[aerisarn Mesa UWP port](https://github.com/aerisarn/mesa-uwp), inspired by the
[worleydl UWP GL sample](https://github.com/worleydl/uwp_gl_sample). The pinned release is
`alpha-2-resfix`, source revision `15acdd7ea2b9dcdd62f26fe86b88280d79efc46b`; its archive checksum
is checked by `tools/nxbox/prepare_mesa.py` and its license accompanies the package.

The probe checks an OpenGL 4.6 core context, compute-shader storage-buffer readback, clear-color
readback and presentation. It uses a separate package identity, `NSPX.NXbox.GraphicsProbe`, and logs
to `LocalState/graphics-probe.txt`. Passing it would establish a driver path only, not working
Switch GPU emulation.

The initial signed probe installed on Series X, but activation returned `0x8027025B` before any app
log or crash dump appeared. The MTA apartment initialization and activation handler fixed startup.
Packaged DXIL fixed the subsequent pipeline crash; the validated results and remaining
worker-context issue are listed above.

`homebrew/paddle-test` supplies an original interactive NRO for subsequent guest rendering and
controller tests without keys. Its source compiles with devkitA64; it has not yet been played
through NXbox.

## Booting a game from LocalState (2026-09-25)

`RunGame` now resolves what to boot from the app's `LocalState`:

- `game.txt` holds the path of the file to boot, relative to `LocalState` (for example
  `games\p5r.nsp`). Without it the bundled homebrew boots, as before.
- `game.url`, when present, makes the app download that URL to the `game.txt` path first, with a
  resumable HTTP `Range` download (`src/eden_uwp/game_download.cpp`) that logs `DOWNLOAD ...`
  progress. The manifest gained the `internetClient` and `privateNetworkClientServer` capabilities
  for it.
- Keys go in `LocalState\eden\keys` (`prod.keys`, `title.keys`).

A commercial dump in `.nsz` must be converted to `.nsp` first (`nsz -D`); Eden does not read `.nsz`.
The file can be served from a LAN machine with any HTTP server that honors `Range`.

## Symbolizing the Mesa D3D12 crash

`tools/nxbox/build-mesa.cmd` keeps `buildtype=release` and passes `/Zi`, `/FS` and `/DEBUG:FULL`
explicitly. That produces a `libgallium_wgl.pdb` that matches the release DLL and keeps the release
CRT, unlike `debug=true` or `buildtype=debugoptimized`, which link the debug CRT into the UWP
target.

## Root cause of the 12-frame crash (2026-09-25)

The `0xC00000FD` that ended every run after 12 presented frames is **not inside Microsoft's driver**
and not a stack-size problem: it is unbounded recursion in Mesa's d3d12 query code. Symbolizing the
crashing thread's stack with a PDB built from the same release flags (`crash-report.py`, ~15,400
repeats of one cycle) gives:

    begin_subquery (d3d12_query.cpp:458)
      -> accumulate_subresult_gpu (:398)
        -> d3d12_restore_compute_transform_state
          -> d3d12_set_active_query_state
            -> d3d12_resume_queries (:646)
              -> begin_query -> begin_subquery   (same subquery again)

`begin_subquery` accumulates the query heap when `curr_query == num_queries`. Accumulation saves and
restores compute state, which suspends and resumes every active query, so `begin_subquery` is
re-entered for the same subquery while `curr_query` still equals `num_queries`. Eden begins a query
per frame; the heap fills at frame 12. `patch_mesa_uwp.py` now guards the subquery with an
`accumulating` flag. The earlier statements above that the crash is "inside the Xbox D3D12 driver"
and needs WinDbg were wrong: the driver frames on the stack are just callees of this cycle.
Validation on the console is pending.
