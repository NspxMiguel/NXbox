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
