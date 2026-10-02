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

## Homebrew validated on the console (2026-09-25)

Package `0.1.32.19` (Mesa with the query-recursion and null-fence patches), Xbox OS `26100.9608`:

- The bundled Paddle ran for two minutes with 24 `GAME_PRESENT` samples: 44.8 to 52.7 FPS and a
  final sample of 59.56 FPS (298 frames in 5 s), memory flat at 1.06 GB, no crash.
- A remote A press produced `NXBOX_PADDLE_INPUT_A` in the guest and the score moved `0:0` to `0:1`.
- Plus produced `GAME_STOPPED` and the process exited.
- The second Mesa crash after the recursion fix was an access violation in `d3d12_fence_finish`
  (`d3d12_fence.cpp:113`, read of offset 0x28 of a null fence, called from `fence_finish`). The
  patch treats a null fence as complete.
- Limits: the Device Portal screenshot returns a black frame, so there is no visual capture of the
  Paddle; the evidence is the guest markers and frame counters.

## Commercial game boot (2026-09-25)

A user-owned Persona 5 Royal dump (`.nsz` converted to `.nsp`, all NCAs hash-verified) was
downloaded to `LocalState\games` over the LAN at about 110 MiB/s (14.2 GB in about 2 minutes). Eden
recognized title `01005CA01580E000`, patched the ExeFS and applied game settings, so the keys work.
The guest then stops calling services about 4.6 s after start (last call: applet `ReceiveMessage`,
message 1), never presents a frame (`frames=0`) and the host CPU stays low; the process stays alive.
Cause not found yet. Next steps: `log_filter.txt` with `*:Trace` and the `flush` line, and find what
the guest waits for after `GetEventHandle`/`ReceiveMessage`.

Operational notes: an Xbox system update wipes Device Portal credentials and can remove sideloaded
apps; `game.txt` set to `none` (or empty) boots the bundled homebrew; installing a new package
without uninstalling keeps `LocalState` (and the downloaded game), but an uninstall wipes it. The
app's storage filled once when a stray download was written next to the 14.5 GB game.

## Commercial game boot: root cause of the stall (measured)

The frontend passed zeroed `FrontendAppletParameters`, so the applet manager created the game's
applet with `is_application = false`. The guest then received `ChangeIntoForeground` (1) instead of
`FocusStateChanged` (15) and both guest threads waited forever on the AM message event
(`WaitSynchronization` on a `KReadableEvent`). Booting with `applet_id = Application` and
`applet_type = Application`, as the desktop frontend does, lets the game run: Persona 5 Royal
presents 150 frames per 5 s (30 FPS, `speed=100%`) with about 4.8 GB of memory in use. Only a short
run was measured; audio is null and nothing beyond the boot was exercised. The stall diagnostics
(`GUEST_THREAD` lines in the diagnostic file while no frame has been presented) remain available.

## Black screen with frames presented (open, measured)

Persona 5 Royal and the homebrew present frames at a steady rate, but the TV stays black. Facts:

- The window framebuffer and swap chain are fine: a solid red clear right before `wglSwapBuffers`
  shows on the TV.
- The Eden presentation pass drew into a core profile context without a vertex array object
  (`No array object bound` on every frame); each context now binds one. The picture stayed black.
- The frame's alpha is forced opaque before swapping; it stayed black.
- Plain GL works in the app's own contexts before the emulator runs: clears and draws on RGBA8,
  RGB10_A2, RGBA16F, sRGB and R11G11B10F targets, DSA textures and texture views, both on the
  bootstrap context and on a shared worker context, and quad draws into the window framebuffer
  (sample counts from occlusion queries match the expected pixel counts).
- A full-screen quad drawn late in a running session, after Eden's presentation, passed 0 samples
  (occlusion query), so draws in the emulator's context stop producing pixels at some point.
  That query result is not fully trustworthy: Eden may already hold a `GL_SAMPLES_PASSED` query
  open, which would make a second one fail.
- glReadPixels and glGetTextureImage return stale data once the emulator runs (values from
  earlier readbacks appear in unrelated reads), so pixel readback cannot be used as evidence here;
  the peak/PPM numbers from the earlier diagnostics are invalid. Neither the pack buffer binding
  nor pack row/skip state nor `glFinish` changes that.
- Device Portal screenshots of NXbox are always black, including while the red test was on the TV.

Suspects, in order: the Mesa null-fence and query-reentrancy patches (`tools/nxbox/patch_mesa_uwp.py`)
changing synchronization in the d3d12 driver; a query or predication state the emulator leaves
active; the emulator's GL state after its first clear. Every diagnostic used is kept on the
`diag/black-screen` branch (probes, self tests, PPM dumps, the green draw test); `main` carries
only the fixes.


## Persona 5 Royal: found the loop that blocks the menu (measured, root cause not yet fixed)

The game never gets past a black screen because it never leaves the controller-configuration
applet. `applet_controller.cpp`'s `Initialize`/`ReconfigureControllers` cycle repeats every ~5.13
seconds, forever (measured: `Initializing Controller Applet` at 144.7, 149.8, 154.9, 160.1, 165.2,
170.4, 175.5, 180.7, 185.9s, a run of nine straight cycles). Our side reports success every time
(logged: `is_success=true player_count=1 selected_id=0x0 result=0`), so the game is rejecting a
response that looks correct by every field we control — the bug is in the handshake back to the
guest, not in the controller state itself. `FrontendApplet::Exit()` sets `is_completed` and signals
`state_changed_event`; `ILibraryAppletAccessor::GetResult()` separately returns `terminate_result`,
which nothing in the applet code ever sets explicitly (it stays at its zero/Success default, so this
was ruled out as the cause, not confirmed as it).

This loop is also what exhausts kernel events: each cycle leaks the generic `Service::Event` wrapper
(`core/hle/service/os/event.cpp`, name `"Event"`) at the controller applet's retry rate — measured
766 of 800 total `KEvent` creations were bucketed under `"Event"` in one run. The stock slab of 900
(`SlabCountKEvent` in `init_slab_setup.cpp`) is exhausted in ~216s; raised to 20000 on this port only
as a stopgap (`#ifdef _WIN32`) so longer runs are possible while this is tracked down, but the
process still eventually hits the guest's own per-process `EventCountMax` resource limit (~247s in
two independent runs, suspiciously exact) since raising the slab does not raise that.

**Root cause found and fixed.** `eden_uwp/gamepad.h`'s `Poll()` disconnected the emulated controller
the instant a poll saw no WGI gamepad (`Gamepad::Gamepads()`) and no remote-input key had ever been
seen yet — every 8 ms. The controller applet's `Connect(true)` was undone by the very next poll
tick, so from the game's continuous connection-monitoring the pad connected and instantly vanished
again, and it re-showed the applet on its own ~5.13 s cooldown, forever. Fixed by debouncing the
disconnect (half a second of consecutive empty polls, not one) — confirmed on Xbox: zero
`Initializing Controller Applet` re-entries in a run that previously hit it every ~5 s without stop.
This was most likely specific to testing over Device Portal remote input rather than a real paired
controller (a real controller should stay enumerated in WGI continuously), but the debounce also
protects a real controller against any transient WGI enumeration gap, so it is the right fix either
way.

**Correction after further testing: this was not the (sole) cause of the black screen.** With
continuous remote input sent throughout boot (`hold-input.ts`, keeping the debounce's `keys_seen`
flag true from the start), the controller-applet loop is confirmed gone — zero retries through a
262-second run — and the game runs the whole time at a steady ~30 FPS with no crash. The screen is
still black: every thumbnail sample in that run (48 of them, 5 seconds apart, the same proven
`window_adapt_pass.cpp` readback used earlier) came back `peak=0`. The controller loop was a real,
now-fixed bug, but whatever actually keeps the screen black is still open.

## Motion controls (planned, not implemented)

The Xbox Series X controller and Xbox Elite controller have no gyroscope. Eden's motion pipeline
(`hid_core/frontend/emulated_controller.h`'s `SetMotion`) reads through the same `Common::Input`
engine callback mechanism as buttons and sticks, so no new core plumbing is needed — only a new
source. Plan: map the right stick's tilt as a motion proxy (pitch/yaw) by default, which covers
aim-assist-style motion controls without new infrastructure. Stretch idea for later: a phone as a
companion gyroscope over the LAN, reusing the remote-input bridge already built for testing.


## Ruled out: the null-fence patch is not the black-screen cause either

Tested `d3d12_fence.cpp`'s null-fence guard (patch_mesa_uwp.py's `patch_fence`) in isolation, removed
via a CI build with `mesa_skip=fence`. Result: the game is noticeably less stable without it (crashed
on the first three launch attempts, before even reaching the point the loop-fix build always survives
to), and on the one run that did stay up, the picture is still black — 4 samples, `peak=0`, through
t=67.9s. The guard is necessary for basic stability and is not itself hiding real rendered content;
restored to the full-patch (query + fence) Mesa build after this test.

Ruled out so far, in full: both Mesa patches together, either alone, the controller-applet retry
loop (fixed, confirmed not the cause with 4+ minutes of continuous input), the disabled MSAA resolve
(fixed with glBlitFramebuffer, confirmed not the cause), multiple display layers (P5R uses exactly
one), and the managed layer's default visibility (defaults to visible, unrelated code path). The
presented texture the game itself writes to remains genuinely empty through several minutes of
active ~30 FPS rendering with zero GL/driver errors.


## 15-minute conclusive test: the black screen is not a loading phase

With the full-patch (query + fence) Mesa restored, the controller loop fixed, and continuous remote
input sent the whole time (ruling out any input-related stall), P5R ran for 887.8 seconds (14.8
minutes) at a steady ~30 FPS with zero crashes and zero controller-applet retries. 173 thumbnail
samples, 5 seconds apart, spanning the whole run: every single one `peak=0`. This rules out "it just
needs more time to load" — a stable, active, crash-free 15-minute run with genuinely empty output is
not a loading screen.

**Where this investigation stands**: every hypothesis generated from reading Eden's and this port's
own code has been tested and ruled out (see the sections above). The remaining possibilities are
either something in Eden's rendering pipeline this port does not yet understand well enough to
suspect, or something at the Xbox compositor/swapchain level outside Eden's own code entirely —
notably, Device Portal screenshots of NXbox have been black in every test all session, including
when a manual full-screen red clear was confirmed visible on the TV, which was never explained
either. Continuing this needs either a working comparison (the same P5R dump running visibly on
desktop Eden, to confirm the assumption that it should work at all) or someone who knows Eden's
render pipeline well enough to suggest a lead this session did not find.


## Confirmed via desktop Eden: the dump, keys and Eden itself are all fine

Built desktop Eden (`cmake -S . -B build-desktop` with the Qt frontend, target `yuzu`, output
`eden.exe`) on the PC with a real NVIDIA GPU, using the exact same P5R dump and keys copied
straight into `%APPDATA%\eden\keys`. It boots and renders correctly: the photosensitivity warning
screen (real text) and the "Thieves Guild" network-connection prompt (a real logo image, not just
text) both display properly. This rules out the dump, the keys, and Eden's own P5R support as
possible causes — the same title works when NOT going through this port's Mesa D3D12 backend.

This narrows the black screen to something specific to `libgallium_wgl.dll` (the pinned
`aerisarn/mesa-uwp` fork) rendering through D3D12 on the Xbox, as opposed to a native driver
(NVIDIA here) rendering OpenGL directly. Screenshots kept in
`~/.local/share/nxbox/artifacts/p5r-desktop-proof/` (not committed — real box art/game screens).

**Correction (2026-09-28):** that run used the **Vulkan** backend (`backend=1` in
`qt-config.ini`, Eden's desktop default), so it did not exercise the OpenGL path this port uses.
Re-ran the same build with `backend=0` (`OpenGL_GLSL`, the exact backend `game_session.cpp` forces)
on the NVIDIA OpenGL driver (`nvoglv64.dll` confirmed loaded): the same diagnostic in
`window_adapt_pass.cpp` reports `peak=183` at 17.7s, reading the same framebuffer addresses
(`0x9ae71000` / `0x9a601000`) the Xbox resolves. Eden's OpenGL GLSL path renders P5R; the black
screen is specific to Mesa D3D12.

Loading the UWP Mesa build into desktop Eden (`QT_OPENGL_DLL` pointing at its `opengl32.dll`) does
load it, but Qt aborts with "OpenGL shared contexts are not supported", so reproducing on desktop
needs the share-context path working in this Mesa build first.

Practical note for next time: on this PC, GUI apps launched over SSH run in Session 0 (services,
no real display) and screenshots there fail with "the handle is invalid". A `schtasks /Create ...
/IT /RL HIGHEST` (interactive) run against the active console session (`query session` shows it)
is what actually reaches the real screen for both the app and screenshot capture.

## D3D12 readback synchronization audit (2026-09-28)

Audited the exact pinned `aerisarn/mesa-uwp` revision `15acdd7ea2b9dcdd62f26fe86b88280d79efc46b` because the black-screen samples read the same texture that Eden's display path consumes. The ordinary Gallium texture readback path does **not** appear to omit either the D3D12 state transition or a CPU-visible completion wait:

- `d3d12_resource.cpp::transfer_image_part_to_buf` delegates to `copy_texture_region`; that helper retains source/destination in the current batch, transitions the source to `D3D12_RESOURCE_STATE_COPY_SOURCE` and staging buffer to `COPY_DEST`, applies resource states, then records `CopyTextureRegion`.
- `d3d12_transfer_map` calls `transfer_image_to_buf`, then `d3d12_flush_cmdlist_and_wait`, before mapping the staging buffer. The flush ends/submits the batch and `d3d12_reset_batch(..., OS_TIMEOUT_INFINITE)` waits for its fence. This is stronger than the `glFinish()` in the NXbox diagnostic guard.
- Mesa's generic state tracker therefore should make an extra GL-side RT-to-SRV barrier unnecessary for `glGetTextureSubImage`/texture readback. The relevant readback transition is RT/current-state to COPY_SOURCE, not shader-resource, because the copy is texture-to-staging-buffer.
- The pinned `patch_fence` only makes a null `fence_finish` return success; it does not bypass the non-null batch fence used by `d3d12_flush_cmdlist_and_wait`. The null fence patch is already independently ruled out by the Xbox test in the section above.

This makes a missing generic readback barrier or missing readback fence wait an unlikely explanation for a stable all-black texture. It is not proof that Xbox's D3D12 implementation honors Mesa's command-list/fence behavior correctly, nor that the tracked state is correct for every subresource/alias. If the sampled object truly is the display source, the next discriminating test belongs in the external Mesa fork: instrument resource identity, subresource, state-before/state-after, batch/fence values, and a known non-black clear followed by readback on that *same* resource; compare a direct `CopyTextureRegion` staging readback with a shader sampling that texture into a known-good render target. Also inspect whether the rendering commands target the same underlying `ID3D12Resource` (and layer/mip) whose view is sampled by presentation. A transition should only be patched if that trace shows the state tracker emitting the wrong resource/subresource state or failing to submit/wait; adding an unconditional barrier in `patch_mesa_uwp.py` would duplicate the existing Gallium path and is not currently justified.

Evidence: pinned source [`d3d12_resource.cpp`](https://github.com/aerisarn/mesa-uwp/blob/15acdd7ea2b9dcdd62f26fe86b88280d79efc46b/src/gallium/drivers/d3d12/d3d12_resource.cpp) (`copy_texture_region`, `d3d12_transfer_map`) and [`d3d12_context.cpp`](https://github.com/aerisarn/mesa-uwp/blob/15acdd7ea2b9dcdd62f26fe86b88280d79efc46b/src/gallium/drivers/d3d12/d3d12_context.cpp) (`d3d12_flush_cmdlist_and_wait`).

## Black screen: driver-side measurements (28/09/2026)

Instrumented the pinned Mesa d3d12 driver (`tools/nxbox/patch_mesa_uwp.py`, reported through
process environment variables and logged by the presentation pass):

- Graphics PSO creation: 16 created, 0 failed.
- DXIL.dll loads (validator 1.8, Windows SDK build). Every shader fails validation with
  `0x80AA0013` (DXC_E_LLVM_UNREACHABLE) at shader model 6.8 and at 6.7, so no shader is signed;
  the Xbox runtime still accepts them. The DXC release validator cannot load in the package
  (error 126, it imports the desktop CRT).
- `d3d12_draw_vbo`: about 96k draws, none dropped early (no zero-count, cull front-and-back,
  or stream-output failures). Issued draws have no predication, no rasterizer discard, no clip
  planes, full sample mask and full color write mask. The presentation quad was issued with
  back-face culling; it is now drawn with culling off.
- **Readback self test**: clearing a fresh 4x4 RGBA8 texture to 37,99,201 and reading it back
  returns 0,0,0 in the running session. GPU work is not executing at all, which is why every
  earlier pixel sample was zero.
- Suspect: `ID3D12GraphicsCommandList::Close()` failing. Mesa then skips `ExecuteCommandLists`
  and leaves the batch without a fence, which the null-fence patch treats as complete. Now
  reporting close failures, the device removed reason and the last debug layer message.

`LocalState\nxbox_env.txt` (KEY=VALUE lines) sets environment variables before Mesa loads, e.g.
`D3D12_DEBUG=debuglayer` or `NXBOX_D3D12_MAX_SM=7`, without a rebuild.

## Persona 5 Royal is playable on Xbox Series X (28/09/2026)

Measured on the console with remote input:

- Boot → Thieves Guild network prompt → title screen → New Game → voice language → the opening
  movie → the first playable scene ("Escape from the casino"). The character walks with the left
  stick. Steady ~28-30 FPS; app memory about 4.0 GB of the 5 GiB budget.

What it took, in order:

1. **Black screen**: in Mesa d3d12, accumulating a full query heap resumes every active query, and
   that began the other subquery of PRIMITIVES_GENERATED a second time. BeginQuery twice, then
   `Close()` fails with "queries outstanding", and the reused list fails every later batch, so no
   GPU work ran. Fix: `begin_subquery` returns early when the subquery is already active
   (`tools/nxbox/patch_mesa_uwp.py`, `patch_query`).
2. **Freeze after declining the network**: the default error applet never invoked its finished
   callback. It now finishes at once (`src/core/frontend/applets/error.cpp`).
3. **Suspended at the intro (5 GiB budget)**: the kernel's `memset(0)` of every guest allocation
   committed the whole heap. On UWP the backing is demand-committed, so zero-filling now
   decommits (`HostMemory::ClearBackingRegion`); peak memory fell from 5.36 GB to 3.6-4.0 GB. The
   JIT code cache bound (128 MiB per core) and smaller texture-cache thresholds also apply now,
   through a `NXBOX_UWP` definition in core and video_core.
4. **Audio**: new XAudio2 sink (`src/audio_core/sink/xaudio2_sink.cpp`), 48 kHz stereo;
   `NXBOX_AUDIO=null` in `LocalState\nxbox_env.txt` restores the silent sink. It initialises on the
   console; sound on the TV has not been confirmed by ear yet.

Still open: listening test for audio, a real paired controller (the tests used Device Portal
remote input), longer play sessions, and removing the Mesa diagnostic counters once they are no
longer needed.

### Performance and glitch notes (28/09/2026, night)

- Pacing measured with the per-window `worst_gap_ms`/`hitches` fields: 60-70 gaps over 100 ms per
  run, some of 3-6 s. Most fall on scene transitions, when shaders compile synchronously.
- `use_asynchronous_shaders=true` (through `LocalState\eden_settings.txt`) hangs P5R during
  `system.Load` on Mesa d3d12 (the log stops at `MEM before_load`); leave it off.
- `NXBOX_PRESENT_INTERVAL=0` (Present(0, 0)) changed nothing in the menu (21.3 vs 21.0 FPS).
- Screenshots show heavy aliasing and moiré on distant textures, which looks like missing or
  garbage mip levels. Being tested: `accelerate_astc=0` (CPU ASTC decoding).
- The console's internal storage was 99.9% full from old crash dumps and diagnostic dumps;
  they were archived to the PC (`/srv/nxbox-archive`) and removed from the console.
- **Fixed: texture glitches.** With `accelerate_astc=0` (CPU ASTC decoding) the casino renders
  cleanly: no sparkling dots, no moiré, smooth arches and panels. The GPU ASTC compute decoder
  leaves garbage in the lower mip levels on Mesa d3d12. CPU decoding is now the default
  (`game_session.cpp`). Worth trying later: `CpuAsynchronous` (2), to take decode stalls off the
  emulation thread.
- **Fixed: frozen intro movie.** GPU video decoding (`d3d11va`) froze the casino movie on one
  frame while the game kept presenting at 30 FPS. `nvdec_emulation=1` (CPU) plays it through,
  and it is now the default.
- The Plus button does not skip P5R movies over remote input; this is not investigated yet.
- While the CPU decoded video and ASTC, the Device Portal stopped answering (remote input,
  screenshots, file API all timed out). Being investigated.

### Shader cache (29/09/2026)

- The UWP session never called `LoadDiskResources`, so the OpenGL shader cache was neither saved
  nor precompiled. A second GL context cannot be made current on the Xbox's Mesa WGL: surfaceless
  `wglMakeCurrent` fails, and the window DC would need a second swap chain. The precompile
  therefore runs on the renderer's own context before the GPU thread starts
  (`EmuWindow::PrecompileOnCurrentContext`, `NXBOX_SHADER_CACHE=0` disables it).
- Measured on P5R to the casino: with an empty cache, gameplay gaps of up to 7.8 s; with the cache
  (971 KB after one run), every 5 s window in the casino has a worst frame gap of 36-64 ms and no
  hitch over 100 ms. The remaining stalls (up to 3.7 s) happen during the intro, the logos and the
  menu load.
- Sharing between consoles: `ShaderCache::MergeCacheFiles` merges entries byte for byte. With
  `NXBOX_SHADER_SHARE_URL` set, the session downloads and merges before the precompile and uploads
  on suspend. The server is `tools/shader-share/server.ts` (Bun, one cache per title, keeps the
  larger valid upload). A cache from a shader-derived artifact is independent of the GPU; a future
  compiled-DXIL cache would need a Series S/X + OS + Mesa key.

### Stall attribution and precompile guard (30/09/2026)

With the shader cache in place, the remaining hitches (up to 3.8 s, about 70 per run) all fall in
loading phases: the intro, the logos and the menu. In every such window the app memory grows, which
points at new content being loaded rather than at shaders. To tell the causes apart, every 5 s pacing
window now writes a `GAME_STALL` line with total, longest and count, in milliseconds, for:

- `shader`: `CreateGraphicsPipeline` and `CreateComputePipeline`
- `upload`: `TextureCache::UploadImageContents`
- `convert`: CPU ASTC/BCn conversion in `ConvertImage`
- `gc`: texture cache garbage collection
- `video`: FFmpeg `SendPacket`

The counters live in the header-only `src/common/nxbox_stall.h`.

The first run of that build after the console lost power never got past `SHADER_CACHE loading`.
The process vanished with no crash dump, and the Device Portal then stopped answering, which meant
a manual reboot. The 1,036,492-byte cache that was loading parses cleanly: 264 well-formed entries,
checked with a standalone parser of the `yuzucach` v15 layout. So the cache file is not corrupt.
Two changes came out of this:

- The precompile writes `SHADER_CACHE built n/total` to the flushed diagnostic file every 32
  pipelines. The emulator log is buffered and loses its tail when the process dies.
- A marker file, `opengl.loading`, sits next to the cache during the precompile. If it is still
  there at the next launch, the cache is renamed to `opengl.crashed.bin` and that session neither
  loads it nor downloads the shared copy. A precompile that kills the process can no longer keep
  the game from starting.

Warm-cache measurement (01/10/2026, package 0.3.168, the run that started with 186 precompiled
pipelines):
- Shader time is zero in every hitch window. The precompiled cache works, and the remaining
  shader time is the boot precompile.
- Persona 5 Royal never converts an ASTC texture (`convert` stays 0), so ASTC decoding is not part
  of its stalls. Its textures are BCn, which D3D12 samples natively. The GPU ASTC decoder still
  matters for other games, and the device reports `NXBOX_D3D12_RELAXED_CAST=0`: sRGB textures get
  no UAV, which is why that decoder fails.
- The hitches that remain (3.8 s, 2.6 s, 1.7 s, 1.2 s) are nearly unattributed: at most 0.24 s of
  texture upload, no shader, no video.
- The same session froze on the casino movie and took the Device Portal down. Both fit priority
  inversion: the guest CPU threads and the GPU thread ran at TIME_CRITICAL and spun on fences that
  Mesa's normal-priority threads had to signal. Every emulator thread now runs at normal priority
  on Xbox (`common/thread.cpp`). The next build also attributes time to JIT compiles, code-cache
  evacuations, file reads, AES and GPU-thread busy time.

### ProsperoEden comparison and bounded JIT changes (01/10/2026)

The PS5 homebrew port is **ProsperoEden**. Its [v1.000.030 release](https://github.com/blackbearreloaded/ProsperoEden/releases/tag/v1.000.030)
and tagged source are now public; older search results saying source is unavailable are stale.
The release claims shared compiled CPU code and roughly 40% less compilation CPU time. These are
the author's results, not a P5R comparison or a guarantee that demanding games run smoothly. The
published build overlays inspected below still describe per-core caches; the release claim alone
is insufficient to reconstruct a safe cross-core sharing implementation for NXbox.

What the inspected sources establish:

- **JIT:** Dynarmic on x86-64, with [separate writable/executable aliases](https://github.com/blackbearreloaded/ProsperoEden/blob/v1.000.030/headless/jit-alias.cmake)
  and [bounded compilation of unconditional chains](https://github.com/blackbearreloaded/ProsperoEden/blob/v1.000.030/headless/jit-compile-batch.inc).
  [Successful assertions stay inline](https://github.com/blackbearreloaded/ProsperoEden/blob/v1.000.030/headless/jit_assert.inc).
  The [build overlays](https://github.com/blackbearreloaded/ProsperoEden/blob/v1.000.030/headless/CMakeLists.txt)
  also enable Dynarmic ThinLTO, avoid empty patch records and unused perf names, and budget A64 code
  at 256/192/192/16 MiB for cores 0/1/2/3. NXbox uses 128 MiB per core, page-table memory access
  (fastmem is disabled in the session), and its own Windows AppContainer JIT allocation path.
- **GPU:** the [release build](https://github.com/blackbearreloaded/ProsperoEden/blob/v1.000.030/tools/build-package.sh)
  enables Vulkan/RADV; OpenGL remains available. [Pinned dependencies](https://github.com/blackbearreloaded/ProsperoEden/blob/v1.000.030/tools/deps.json)
  include PS5_Mesa, PS5_Vulkan and the PS5 OpenGL 4.6 SDK. This is a native PS5 driver stack, not
  the OpenGL-to-D3D12 path NXbox must use. Switching an Eden setting cannot supply that driver on Xbox.
- **Shaders:** [startup code](https://github.com/blackbearreloaded/ProsperoEden/blob/v1.000.030/headless/main.cpp)
  configures RADV's disk cache, a native OpenGL compiler cache and `PS5_GLTHREAD=1`. These are
  driver-specific facilities. NXbox already precompiles Eden's disk cache on the renderer context;
  enabling asynchronous shaders previously hung Mesa WGL. Keep that working configuration.
- **Threading:** [topology tests](https://github.com/blackbearreloaded/ProsperoEden/blob/v1.000.030/tools/check-worker-affinity.py)
  exercise placement of four CPU workers and the GPU on separate physical cores, excluding SMT
  siblings and falling back when topology is unavailable. NXbox already lowered emulator priorities
  to normal. PS5 affinity masks are not transferable to the Xbox UWP CPU allocation.
- **Memory:** [per-thread allocation arenas](https://github.com/blackbearreloaded/ProsperoEden/blob/v1.000.030/headless/heap_arenas.inc)
  reduce contention on Sony's mspace allocator. The build reserves a 3 GiB heap and describes 12 GiB
  of direct memory. NXbox has a 5 GiB process budget and demand-committed guest backing; replacing
  the Windows allocator or copying those reservations is not justified.

Applied only to the UWP Dynarmic target:

1. `EmitX64::Patch` uses `find` and returns when no incoming patch sites exist. Previously
   `operator[]` retained an empty four-vector record for every such compiled block. Existing
   sites remain intact for unlinking and relinking after invalidation.
2. `RegisterBlock` skips friendly-name formatting for the Windows no-op perf-map writer.
3. `ASSERT_MSG` keeps the successful condition inline and outlines only the logging/failure path.
   Conditions still execute once; failed assertions still log and invoke `AssertFailSoftImpl`.
   The extra `NXBOX_INLINE_JIT_ASSERTS` definition is private to Dynarmic, so assertions in the
   other libraries are unchanged. `NXBOX_UWP` guards the emitter changes.

These reduce JIT bookkeeping/allocation overhead; they do not establish the cause of the 1-4 s
gaps. No shared JIT, larger code cache, affinity change, shader-thread change or new GPU backend
is included. The fence flush fix remains intact.

Validation: `python3 tests/port/test_jit_hot_paths.py` passes two tests with five compiled host
variants. It exercises the production Patch/Unpatch/RegisterBlock methods with a mock code writer:
100,000 absent targets, all four patch types, cursor restoration, unlink/relink and the unchanged
desktop path. It also includes the real assertion header with a logging stub and checks condition
and message side effects plus failure handling. This is not an x86 emitter or Windows SDK build.

Console verification:

1. Build baseline and candidate from the same base, including the fence flush fix, using
   `tools\nxbox\build-uwp.cmd` on Windows. Verify the candidate Dynarmic compile command contains
   `NXBOX_UWP=1` and `NXBOX_INLINE_JIT_ASSERTS=1`. Package/install using the existing UWP workflow.
2. Preserve the same P5R save, settings and warmed shader-cache snapshot for both packages. Keep
   async shaders off, normal priorities and the 128 MiB caches; remove verbose/per-line logging
   overrides for both runs. Restart the app for each trial: CPU JIT caches are session-local even
   with a warm shader cache. Alternate baseline/candidate at least three times each.
3. Follow the same sequence through logos, menu, movie and casino traversal. Collect
   `LocalState\eden_uwp_diag.txt` after each run. Compare equal phases using `GAME_PRESENT`
   `worst_gap_ms`/`hitches`, `GAME_STALL jit=total_ms/max_ms/count`, and process memory. Sum
   `jit` totals and counts across the phase before computing time per compile. Also record
   `jitflush` counts (its current times are always zero), `shader`, `glsync`, `readback`, `io`
   and `decommit`; lower JIT cost cannot explain a window with no JIT work.
4. Check movie playback, audio, controls, a repeated area transition and a 15-minute gameplay
   session for crashes, visual regressions or memory growth. Report loading and gameplay results
   separately. Profiler categories can overlap across threads/nested scopes, and scopes record
   on completion; their totals are not an additive decomposition of a frame gap.

No Xbox run or complete UWP compilation was performed for this change on the macOS host.

## Storage, stale Mesa and the first library build on the console (01/10/2026)

- **Disk.** The devkit's internal storage left for apps is about 5 GB beyond the installed
  games. A stale 5 GB crash dump filled it: a game download then failed at the same byte twenty
  times, and banner caching and the diagnostic log stopped silently. Downloads now check free
  space first (`DOWNLOAD_NO_SPACE`), the log rotates past 4 MiB, and games can live on a
  removable drive in `<drive>\NXbox\games` (read, downloaded and converted in place through
  the `...FromApp` Win32 calls; plain Win32 and `std::filesystem` are refused there).
- **Stale Mesa in automatic packages.** `package-nxbox.yml` defaulted to a Mesa run pinned
  weeks ago, without the d3d12 query reentrancy and double-begin fixes. 0.3.183 then died about
  five seconds into Persona 5 Royal: the dump shows `0xc00000fd` with 15,391 nested
  `begin_subquery` → `accumulate_subresult_gpu` → `d3d12_set_active_query_state` cycles. The
  package now takes the newest successful `mesa-uwp.yml` run on main unless one is given.
- **Pacing on 0.3.184 (correct Mesa, cold shader cache).** To the casino: 79 windows of 5 s,
  average 26.1 FPS, 21 windows with a gap of 200 ms or more, worst 5.3 s. The worst window is a
  single 4.9 s GPU-thread job; shader builds (0.95 s) and query waits (0.55 s) explain only part
  of it. The cache was cold because the precompile guard discarded it after the 0.3.183 crashes.
- **Large games on the removable drive (02/10/2026).** Downloads land on `E:\NXbox\games` when
  LocalState is short (`DOWNLOAD_TARGET E:\...`). The drive writes at about 2-5 MiB/s through
  the `...FromApp` path (internal storage took about 100 MiB/s), so it is slow but works. A
  remote `.nsz` named by `game.url` is converted straight into an `.nsp` over HTTP range
  requests (`NSZ_STREAM ...`), because a 9.6 GB NSZ and its 13.5 GB NSP did not fit side by
  side; a partial `.nsz` of the same game counts as reclaimable space.
- **Game boot tests (02/10/2026, 0.3.197-0.3.199).**
  - *Super Mario 3D World + Bowser's Fury* boots to its title screen at a steady 60 FPS.
  - *Breath of the Wild* boots from the removable drive and runs at up to 30 FPS for about
    80 s, then `CreatePipelineState` starts failing with `E_INVALIDARG` (68 created, 80
    failed). Before the null-PSO patch the driver AddRef()ed the null PSO and crashed;
    now the process lives but presents no more frames. The reason for the rejection needs the
    D3D12 debug layer, which the console does not have, so the next step is reproducing it
    with the same Mesa DLLs in a desktop process.
  - Both games had looped on the controller-support applet (43 launches, leaking events until
    `CreateEvent` hit the resource limit) while player 1 was disconnected without an Xbox pad;
    player 1 now stays connected.
  - *Mario Kart 8 Deluxe* has no room to convert: about 10 GB of NSP for the base game and
    update, with the internal storage and the removable drive both full.
