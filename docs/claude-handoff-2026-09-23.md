# NXbox continuation checkpoint — 2026-09-23

Miguel requested a handoff to Claude for tomorrow because the Codex plan was nearly exhausted.
Resume implementation and console validation; do not start over. No guest game is playable yet.

## Goal and authorization

Make NXbox usable on Xbox Series X Dev Mode, with actual guest rendering and controller input, then
audio, storage, a controller-first library, mods and upscaling. Target measured gameplay 30 FPS
minimum and 60+ where feasible. Full compatibility is a goal, never a verified claim. Miguel
authorized code changes, public GitHub publication, signing, console installation and tests. Do not
ask again for these routine steps. Preserve other projects and the other agent's Nativra app. Do not
create CLAUDE.md anywhere inside projects. Personal instructions belong outside repositories. Read
the global AGENTS instructions and the user's nspx-design skill before any new UI work.

## Checkout and tools

- Checkout: `/Users/miguel/Documents/Claude/Projetos/NXbox`.
- Local branch: `port/xbox-series-x`; origin is `NspxMiguel/NXbox`, public, remote default `main`.
- Push: `git push origin HEAD:main`.
- Always specify `-R NspxMiguel/NXbox` with `gh`: otherwise it sometimes selects upstream Eden!
- Last implementation commit before this document: `f4139015d5` (guest exit and error cleanup).
- No local heavy builds are running. Windows builds run in GitHub Actions.
- `cmake` is not directly on PATH; local portable tests can use `uvx --from cmake cmake` and
  `uvx --from cmake ctest`. `uvx clang-format`, `uvx ruff format` are available.
- devkitA64 and libnx are installed at `/opt/devkitpro`; `make -C homebrew/paddle-test` works.
- Keep keys, dumps and console credentials out of Git and public artifacts.

## Proven on the actual Series X

1. CPU package 0.1.2.0 executed the original Switch NRO through Dynarmic. The diagnostic recorded
   `RunHeadlessBoot returned 0`, which requires receiving the guest `EDEN_XBOX_JIT_ALIVE` sentinel
   and completing shutdown. It is not merely successful process activation.
2. The graphics probe with packaged DXIL passed clear pixel readback, presentation, and all 64
   compute-shader results. The driver reports OpenGL 4.6 / Mesa 24.1.0-devel on
   `D3D12 (SraKmd_arden)`; app memory budget is 5 GiB.
3. Original driver probe 0.1.6.0 presented 3,590 clear frames over 60.009536 seconds. This is
   **driver presentation rate, not gameplay FPS**.
4. Sparse memory probe 0.1.8.0 reserved 4 GiB and touched three regions; app usage increased from
   4,460,544 to 4,677,632 bytes. Zero fill, writes and release passed on Xbox.
5. **Latest result at handoff:** patched Mesa probe 0.1.9.0 (run 35929432669) is installed and ran.
   Its log contains `worker: shared context active`, a second `COMPUTE_READBACK_PASS count=64`, and
   `WORKER_CONTEXT_PASS`. This resolves the worker-context blocker in the standalone probe. Collect
   its final presentation/cleanup result if needed. No actual game has been displayed yet.

Private evidence lives under `~/.local/share/nxbox/artifacts/`:

- `cpu-35926296025/eden_uwp_diag.txt`
- `graphics-35926298987/graphics-probe.txt` and `screen.png` (green clear screen)
- `graphics-35928748838/graphics-probe.txt` (sparse storage)
- `graphics-35929432669/graphics-probe.txt` (patched worker context, most recent)
- `mesa-35928958265/nxbox-mesa-uwp/` (new DLLs, sources, build logs)

## Console access without exposing or changing secrets

Actual portal: **192.168.68.132:11443**, HTTPS. Old 10.0.0.43 is stale. A private wrapper reads
existing XboxDev credentials from the specific keychain item and overrides host/port only in the
child environment. Do not print the credential or alter the keychain.

```sh
python3 ~/.local/share/nxbox/console.py apps
python3 ~/.local/share/nxbox/console.py install /absolute/package.appx
python3 ~/.local/share/nxbox/console.py launch NXbox
python3 ~/.local/share/nxbox/console.py launch 'NXbox Graphics Probe'
python3 ~/.local/share/nxbox/console.py pull NXbox eden_uwp_diag.txt LocalState
python3 ~/.local/share/nxbox/console.py pull 'NXbox Graphics Probe' graphics-probe.txt LocalState
python3 ~/.local/share/nxbox/console.py shot /absolute/private/screenshot.png
```

The wrapper calls the neighboring XboxDev CLI as a tool. Do not copy its differently licensed
implementation into NXbox or edit its project. `pull` writes into the **XboxDev working directory**;
move retrieved files promptly to the NXbox private artifact directory.

Installed app identities: `NSPX.NXbox` (display `NXbox`) and `NSPX.NXbox.GraphicsProbe` (display
`NXbox Graphics Probe`). Use display names with the CLI. Update-in-place sometimes misbehaves on
this console; only these disposable diagnostics may be uninstalled after preserving logs. Do not
carry that workaround into a product that contains saves. Never uninstall Nativra.

Other private helper scripts include `inspect-console.ts`, `inspect-cpu.ts`, `enable-dump.ts`,
`enable-cpu-dump.ts`; run with Bun. They use the same specific credential and corrected address. CPU
crash dumps can be 5 GiB: use HTTP Range requests, not whole-dump downloads. Range retrieval scripts
and parsed headers/stacks already exist privately. Do not collect unrelated process data.

The other Codex thread is “Continue XboxDev work”, id `01a0cefe-cded-7661-9f76-51d6cc949d3d`. Miguel
authorized coordination. A relay is available:
`team relay nxbox codex --dir /Users/miguel/Documents/Claude/Projetos/XboxDev 'message'`. It queues
a note for the agent's next brief; it does not guarantee immediate acknowledgement. Coordinate
foreground console use. Do not operate the Codex application through computer-use; that tool
explicitly denied it in this session.

## Builds to inspect FIRST tomorrow

They may have finished by then. Check status and logs before launching duplicate builds.

| Run         | Purpose                                                           | Status at handoff                            |
| ----------- | ----------------------------------------------------------------- | -------------------------------------------- |
| 35920737218 | Original complete UWP core build                                  | Passed; executable and PDB downloaded        |
| 35927224925 | Core with sparse allocation, before gameplay frontend             | Running                                      |
| 35928265984 | First OpenGL/gamepad/game-session core build, revision bb105a1a3d | Running                                      |
| 35929224739 | Early frontend object compile, revision 1d8b5c26b6                | Running                                      |
| 35928958265 | Patched Mesa runtime                                              | Passed, downloaded and tested on Xbox        |
| 35929432669 | Probe using patched Mesa runtime                                  | Passed build; worker GPU test passed on Xbox |
| 35929355703 | Portable tests plus Windows sparse/SEH protection tests           | Passed on all three OSes                     |

The full-core workflows automatically trigger CPU diagnostic packaging on success. That automatic
package does not include the graphical guest payload. To make a game package from a successful
OpenGL core build:

```sh
gh workflow run package-nxbox.yml -R NspxMiguel/NXbox --ref main \
  -f build_run=SUCCESSFUL_OPENGL_BUILD_RUN -f kind=game -f mesa_run=35928958265
```

`fetch_mesa_build.py` verifies the source workflow, branch, repository and success before copying
runtime DLLs; `prepare_dxil.py` supplies the validator and license notices. Package secrets are
already configured in GitHub. There is no need to touch Apple accounts or certificates.

Current build workflow enables sccache, streams logs and compiles frontend objects before the full
core. Earlier running builds predate those improvements and may still take about 45 minutes. Use
`[skip ci]` commits when changing unrelated packaging/docs to avoid unnecessary full builds; manual
dispatch is available. Full build concurrency now includes the commit SHA.

## Implementation and important root causes

### Memory

`PageEntryData` is 32 bytes; the 39-bit guest page table therefore reserves 4 GiB. The inherited
Windows `VirtualBuffer` eagerly committed it, consuming most of the Xbox 5 GiB budget. A guest libnx
heap request then crashed inside `KPageTableBase::SetHeapSize` while zeroing about 1 GiB. The CPU
dump was symbolized using the public PDB and only selected dump ranges.

`src/common/sparse_memory.{h,cpp}` now reserves data and commits touched 64 KiB chunks via a
bounded, allocation-free VEH registry. `VirtualAllocFromApp`, `VirtualQuery` and VEH registration
are resolved dynamically from KernelBase, avoiding unsupported Xbox API-set imports. Only UWP
`virtual_buffer.cpp` uses this allocator. Protection faults on already committed non-RW pages and
execute faults propagate. Desktop memory behavior is unchanged. Windows integration tests cover
sparse footprint, concurrent first touches, release, read-only and execute faults.

The old claim that JIT W^X doubles its cache allocation was incorrect. Dynarmic has one reservation
with protection transitions; the temporary 128 MiB cache cap was reverted. Do not repeat that claim.

CPU homebrew uses a static 64 KiB heap. The original paddle game uses an explicit 128 MiB libnx
heap, compiled into the tracked `homebrew/paddle-test/fixtures/game.nro` (176,128 bytes).

### Graphics

Upstream Mesa UWP `alpha-2-resfix`, revision `15acdd7ea2b9dcdd62f26fe86b88280d79efc46b`, originally
crashed at a null pipeline object in `libgallium_wgl.dll`. Packaged DXIL resolved it. The script
prefers the Windows SDK's redist DXIL and retains the official DXC license notices.

A separate crash occurred when a worker called `wglMakeCurrent`: upstream `WindowFromDC` ignores its
HDC and asks for a thread-local CoreWindow, absent on workers. `patch_mesa_uwp.py` returns the
retained CoreWindow ABI pointer supplied as HDC, removes an unused off-thread display query, and
replaces volatile completion flags with atomics. **This patch passed the worker compute test.**

Mesa is built with pinned aerisarn Meson fork `d0a111134f032dcbfbc7b743a4bf811f99909868`, VS2022
backend and `--uwp`. Use the **desktop** vcvars x64 environment for runnable Meson compiler probes;
the generated projects target UWP. `c_winlibs` and `cpp_winlibs` require `WindowsApp.lib` with the
suffix. Earlier failed runs are superseded by successful run 35928958265.

### Gameplay frontend (NOT validated yet)

- `src/eden_uwp/mesa_window.{h,cpp}`: OpenGL loader, shared contexts created on the UI dispatcher,
  context activation/presentation on workers, actual presented-frame counter.
- `gamepad.h`: Windows.Gaming.Input adapter through the existing input engine/factory, buttons,
  triggers and sticks for player 1; disconnect/reconnect path written but not console-tested.
- `game_session.cpp`: boots with OpenGL GLSL and null audio, polls input, records presented guest
  frames per host wall-clock interval and memory usage, observes homebrew debug markers.
- `uwp_boot.cpp`: bundled `game.nro` selects the graphical path; otherwise original CPU smoke path.
- `CMakePresets.json`: OpenGL enabled. No direct Switch-to-D3D renderer was written; this uses
  inherited OpenGL translation on the verified Mesa D3D12 path.
- Latest f4139015d5 adds the guest exit callback and scope-based shutdown on errors; it is newer
  than the running full builds and has not yet been compiled.
- 7d35357a2b fixes fresh-install logging by recursively creating the log directory. It is also newer
  than the running first gameplay full build. If that older build has no Eden log, this is why.
- Inspect cleanup: older graphics probes reached `PRESENTATION_PASS` without immediately recording
  `END`; driver teardown on the UI thread may require dispatch. The game frontend resets its
  graphics on a worker while the UI keeps pumping. This still needs sustained validation.

## Next sequence

1. Inspect running build outcomes; fix early frontend compiler errors first. Do not assume the new
   C++ frontend compiles until the workflow proves it.
2. Package a successful OpenGL build with `kind=game` and Mesa run 35928958265; install it as NXbox.
3. Collect `eden_uwp_diag.txt`, `LocalState/eden/log` if available, screenshot and any own-app crash
   evidence. The first goal is the paddle framebuffer visible on Xbox, not commercial content.
4. Verify A serves, D-pad/stick moves the paddle, Plus exits cleanly, reconnect works. XboxDev CLI
   has `press`; inspect its supported inputs before using it. Device Portal remote navigation may
   differ from a physical gamepad, so distinguish those tests.
5. Measure sustained completed guest frames and frame times on the console. No game FPS has been
   achieved or measured yet. Then add audio, launcher/import flow, saves, mods and upscaling.
6. Update README status: it still has outdated “console execution pending” text. Detailed port notes
   and this checkpoint contain newer evidence. Keep fork/GPL credits and existing notices.

Do not claim 100% compatibility, newer FSR, a playable release or measured game FPS based on these
component probes. Keys are unnecessary for current tests. No commercial title was booted in this
session. User-owned content can be considered after the original homebrew path works.
