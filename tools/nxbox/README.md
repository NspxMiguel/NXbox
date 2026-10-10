# NXbox tools

Run these scripts from the repository root unless a command says otherwise. Windows build/package
operations use the Visual Studio x64 UWP toolchain and Windows SDK. Python inspection and policy
checks can run on the host. The [maintenance map](../../docs/MAINTENANCE.md) records the complete
patch order, runtime switches, CI artifacts and console loop.

## Scripts

| Script                                         | Responsibility                                                                                                                            | Invocation / inputs                                                                                                                                                                                                                  |
| ---------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| [build-graphics.cmd](build-graphics.cmd)       | Selects the Visual Studio x64 UWP toolchain and builds the standalone graphics probe in build-graphics.                                   | `tools\nxbox\build-graphics.cmd` → build-graphics/nxbox-graphics.exe.                                                                                                                                                                |
| [build-mesa.cmd](build-mesa.cmd)               | Builds the prepared Mesa checkout with the pinned Meson fork and installs the d3d12/WGL runtime into mesa-install.                        | `tools\nxbox\build-mesa.cmd`; requires patched .cache/mesa, pinned .cache/meson and generator dependencies from mesa-uwp.yml.                                                                                                        |
| [build-uwp.cmd](build-uwp.cmd)                 | Configures the uwp-x64 preset, compiles frontend objects first, then builds eden-uwp; the frontend argument stops after the object check. | `tools\nxbox\build-uwp.cmd [frontend]`; prepare_host_tools.py first; NXBOX_JOBS defaults to 3.                                                                                                                                       |
| [fetch_mesa_build.py](fetch_mesa_build.py)     | Validates a successful main-branch workflow_dispatch Mesa run and stages its runtime DLLs and license using gh.                           | `python tools/nxbox/fetch_mesa_build.py --run <run-id> --output <directory>`; gh authentication and actions read access.                                                                                                             |
| [inspect_dxil.py](inspect_dxil.py)             | Decodes DXBC/DXIL container metadata and signatures from a binary or JSON hex capture; it does not validate shader instructions.          | `python tools/nxbox/inspect_dxil.py <container-or-json>`; prints JSON metadata to standard output.                                                                                                                                   |
| [package_launcher.py](package_launcher.py)     | Stages and signs a per-game launcher tile with title ID, display name and eShop artwork.                                                  | `python tools/nxbox/package_launcher.py --exe <launcher.exe> --title-id <16-hex> --name <display-name> --version <four-part> --output <directory>`; Pillow, SDK signing tools and NXBOX_PFX_BASE64/NXBOX_PFX_PASSWORD.               |
| [package_uwp.py](package_uwp.py)               | Validates an x64 PE payload, stages cpu/graphics/game UWP packages and signs the Appx with the configured certificate.                    | `python tools/nxbox/package_uwp.py --exe <eden-uwp.exe> --kind game --version <four-part> --output <directory>`; kinds cpu/graphics/game; stage corresponding runtime DLLs/notices beside the executable and supply signing secrets. |
| [patch_mesa_uwp.py](patch_mesa_uwp.py)         | Applies the ordered, anchor-checked NXbox changes to the pinned Mesa source tree.                                                         | `python tools/nxbox/patch_mesa_uwp.py <pristine-mesa-root>`; optional patch-time NXBOX_MESA_SKIP.                                                                                                                                    |
| [prepare_dxil.py](prepare_dxil.py)             | Fetches checksum-pinned Microsoft DXC files and stages shader validation/compiler DLLs and distribution notices.                          | `python tools/nxbox/prepare_dxil.py --output <directory>`; checksum-pinned release download.                                                                                                                                         |
| [prepare_host_tools.py](prepare_host_tools.py) | Fetches the checksum-pinned Windows glslang host compiler into build-host-tools/glslang.                                                  | `python tools/nxbox/prepare_host_tools.py`; stages build-host-tools/glslang.                                                                                                                                                         |
| [prepare_mesa.py](prepare_mesa.py)             | Stages the checksum-pinned upstream alpha-2-resfix Mesa runtime and license for the graphics probe.                                       | `python tools/nxbox/prepare_mesa.py --output <directory>`; upstream probe runtime, distinct from a patched CI build.                                                                                                                 |
| [release_version.py](release_version.py)       | Converts stable tags into four-part Appx versions and rejects versions below or equal to published stable releases.                       | `python tools/nxbox/release_version.py <vMAJOR.MINOR.BUILD[.REVISION]> <releases.json>`; input JSON is gh api --paginate --slurp release pages; emits package_version.                                                               |
| [run_logged.py](run_logged.py)                 | Runs a command, streams merged output to CI and saves the complete log while preserving the exit status.                                  | `python tools/nxbox/run_logged.py <log-path> <command> [arguments...]`.                                                                                                                                                              |

The full workflow commands and prerequisites are in `.github/workflows/` and
[uwp_build.md](../../docs/uwp_build.md). Packaging game mode requires a patched Mesa runtime plus
DXIL/compiler files and notices; a successful emulator compilation does not produce an installable
Appx by itself. Signing certificates are supplied by CI secrets, not generated by these preparation
scripts.

prepare_dxil.py prefers an installed x64 Windows SDK validator when available; set the
**preparation-time** variable `NXBOX_DXIL_SDK=0` to keep the pinned DXC validator instead. It also
stages Visual C++ runtime DLLs for the optional dxcompiler.dll diagnostic path when the Visual
Studio redistributables are present. This setting is not read by the console app.

## Mesa helper inputs

These headers are patch inputs installed or spliced into the pinned driver by patch_mesa_uwp.py.
Edit the original helper here when changing that instrumentation; generated copies in a build
checkout are not the maintained source.

- `mesa_api_ring.h` — Records a process-wide ring of D3D12 API completions and resource counters.
- `mesa_batch_reuse.h` — Guards batch-storage reuse until GPU completion and stops poisoned
  recording.
- `mesa_dred.h` — Configures DRED and captures the first device-removal breadcrumbs/page-fault
  report.
- `mesa_gpu_profile.h` — Adds per-command GPU timestamps and latency summaries to the batch journal.
- `mesa_heap_policy.h` — Selects abstract GPU heap properties and feature-gated residency flags.
- `mesa_lifetime.h` — Reports lifetime/API failures and supports bounded PSO ownership.
- `mesa_list_ring.h` — Tracks command-list/descriptor history and captures debug info-queue
  messages.
- `mesa_pso_first.h` — Retains the first rejected PSO description and shader-bytecode capture.
- `mesa_pso_guard.h` — Defines the opt-in conservative PSO shape guard.
- `mesa_pso_input.h` — Parses input signatures, normalizes PSO input/blend state and implements
  opt-in known-pair quarantine.
- `mesa_view_cast.h` — Counts view-cast paths and bounded distinct format pairs.
- `mesa_view_cast_copy.h` — Caches SRV shadows and reinterprets texels using GPU buffer copies.
- `mesa_query_wait.h` — Waits for query completion with device-loss and unsubmitted-fence checks.
- `mesa_sync_batch.h` — Implements synchronous submission helpers, command journaling and copy/clear
  safety wrappers.

The tracked `__pycache__/patch_mesa_uwp.cpython-314.pyc` is generated bytecode, not another script
entry point. Use patch_mesa_uwp.py as the source of truth. Patch order and source pin are documented
in [MAINTENANCE.md](../../docs/MAINTENANCE.md#mesa-patch-chain).
