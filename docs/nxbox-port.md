# NXbox: Xbox Series X port workbench

This is an experimental continuation of Eden's UWP work. **No Switch game has been booted on
Miguel's Xbox by this project. No installable package has been produced.** The null-renderer
frontend cannot display gameplay.

## Source provenance

- Base: [juanresendiz813/eden-xbox](https://github.com/juanresendiz813/eden-xbox), `development`,
  commit `33969e85769916535d02f124705ff7aa02086775`.
- The `src/eden_uwp` frontend was imported from the same repository's `feature/uwp-boot-appx`,
  commit `5b146f9a5ec3cffad462cf986c6c284df58bf316`. It was missing from the development snapshot.
  Original copyright and GPL notices are retained.
- Local branch: `port/xbox-series-x`. This checkout is independent of `../XboxDev`; no console
  installations or changes to that project have been made.

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
   portable CI workflow. The workflow is published on GitHub; the Windows build is still being
   brought up.

The policy tests cover first/last pages, chunk boundaries, out-of-range access, execute access,
missing reservations, overflow and every byte in a 132 KiB sample reservation. They do **not**
exercise Windows virtual memory, the exception handler, Dynarmic or the Xbox runtime.

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

1. **Windows compile/link:** follow [UWP build setup](uwp_build.md), but use CMake **3.31 or newer**
   (the root build requires it). Build `eden-uwp`, not just `core`. The preset and older upstream
   guide currently advertise 3.25; the root requirement wins. Windows CI is running on GitHub; a
   successful emulator build is not yet established.
2. **Package and boot:** add a separate package identity, codeGeneration capability, assets and a
   reproducible homebrew NRO that emits the sentinel; sign and inspect imports. The imported branch
   contains the frontend but no complete packaging pipeline. Coordinate console time before
   launching it while XboxDev is being tested.
3. **Measure CPU and memory on the Series X:** capture the guest sentinel and actual application
   memory limit. XboxDev's local `docs/VEREDITO.md` reports a successful RW-to-RX JIT probe
   returning 42 on this console on 2026-09-19. That is useful prior evidence, not proof that
   Dynarmic works. The
   [Microsoft resource guide](https://learn.microsoft.com/en-us/previous-versions/windows/uwp/xbox-apps/system-resource-allocation)
   distinguishes app and game budgets; measure with MemoryManager instead of assuming all physical
   RAM is available.
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
