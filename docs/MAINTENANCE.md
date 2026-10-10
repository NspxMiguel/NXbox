# NXbox maintenance map

This guide maps the current checkout of Eden's Xbox UWP port. Keep source and build paths stable:
CMake, patch anchors and CI refer to them directly. Historical console investigations remain in
[nxbox-port.md](nxbox-port.md); build prerequisites are in [uwp_build.md](uwp_build.md). Script
details are in [tools/nxbox/README.md](../tools/nxbox/README.md).

## Directory map

The frontend owns UWP activation, storage, input and UI. Guarded Eden changes own emulation policy
and diagnostics. Mesa changes are applied to a separate pinned checkout, rather than copied into
Eden. Each entry below describes one file; generated caches are identified separately.

### Xbox frontend: src/eden_uwp

- `src/eden_uwp/CMakeLists.txt` — Defines the eden-uwp executable, frontend sources and UWP link
  dependencies.
- `src/eden_uwp/alloc_track.cpp` — Implements opt-in global allocation tracking and call-site
  summaries.
- `src/eden_uwp/alloc_track.h` — Declares allocation tracker activation and reporting.
- `src/eden_uwp/await_bounded.h` — Bounds blocking WinRT worker waits while retaining timed-out
  operations.
- `src/eden_uwp/diagnostic.h` — Writes the LocalState diagnostic log and rotates it past 4 MiB.
- `src/eden_uwp/diagnostic_report.h` — Collects chunked Mesa reports and prefixes every physical log
  line.
- `src/eden_uwp/game_download.cpp` — Implements resumable downloads and storage-aware destination
  selection.
- `src/eden_uwp/game_download.h` — Declares the worker-thread download interface and progress
  callbacks.
- `src/eden_uwp/game_session.cpp` — Owns game selection, settings, core lifecycle, shader warm-up
  and runtime diagnostics.
- `src/eden_uwp/game_session.h` — Declares game-view entry and per-game protocol activation handoff.
- `src/eden_uwp/gamepad.h` — Maps Xbox input to the emulated controller and supplies null
  unsupported input engines.
- `src/eden_uwp/headless_emu_window.h` — Provides the SDL-free window for the null-renderer CPU boot
  diagnostic.
- `src/eden_uwp/http_vfs_file.cpp` — Implements bounded block caching over HTTP range reads.
- `src/eden_uwp/http_vfs_file.h` — Declares remote read-only VFS access for streaming NSZ
  conversion.
- `src/eden_uwp/mesa_window.cpp` — Loads Mesa/DXIL, applies nxbox_env.txt, manages WGL contexts and
  presentation.
- `src/eden_uwp/mesa_window.h` — Declares the Mesa-backed emulation window and launch-frame handoff.
- `src/eden_uwp/protocol_uri.h` — Parses nxbox://play title IDs without WinRT dependencies.
- `src/eden_uwp/save_sync.cpp` — Implements SwitchSaveSync-compatible Google Drive authentication
  and save transfers.
- `src/eden_uwp/save_sync.h` — Declares blocking save-sync operations, state and progress types.
- `src/eden_uwp/setup_ui.cpp` — Draws the standalone Direct2D setup screen.
- `src/eden_uwp/setup_ui.h` — Declares the setup-screen entry point.
- `src/eden_uwp/shader_share.cpp` — Downloads/uploads per-title shader caches and handles
  interrupted cache loads.
- `src/eden_uwp/shader_share.h` — Declares shared-cache transfer and crash-marker operations.
- `src/eden_uwp/ui/anim.h` — Defines UI timing, easing and tween primitives.
- `src/eden_uwp/ui/art.cpp` — Downloads and caches eShop artwork and its title index. Implements its
  operations.
- `src/eden_uwp/ui/art.h` — Downloads and caches eShop artwork and its title index; declares its
  interface and state types.
- `src/eden_uwp/ui/cheats.cpp` — Fetches the community cheat database and manages per-title cheat
  state. Implements its operations.
- `src/eden_uwp/ui/cheats.h` — Fetches the community cheat database and manages per-title cheat
  state; declares its interface and state types.
- `src/eden_uwp/ui/cheats_parse.h` — Provides portable ZIP and Atmosphere cheat parsing/writing.
- `src/eden_uwp/ui/cheats_screen.cpp` — Draws cheat selection and credit screens. Implements its
  operations.
- `src/eden_uwp/ui/cheats_screen.h` — Draws cheat selection and credit screens; declares its
  interface and state types.
- `src/eden_uwp/ui/input.cpp` — Polls screen input with stick hysteresis and held-direction repeats.
  Implements its operations.
- `src/eden_uwp/ui/input.h` — Polls screen input with stick hysteresis and held-direction repeats;
  declares its interface and state types.
- `src/eden_uwp/ui/launch_screen.cpp` — Draws boot/shader progress and prepares offscreen launch
  frames. Implements its operations.
- `src/eden_uwp/ui/launch_screen.h` — Draws boot/shader progress and prepares offscreen launch
  frames; declares its interface and state types.
- `src/eden_uwp/ui/library.cpp` — Scans game packages and caches title metadata. Implements its
  operations.
- `src/eden_uwp/ui/library.h` — Scans game packages and caches title metadata; declares its
  interface and state types.
- `src/eden_uwp/ui/library_screen.cpp` — Runs the library, selection and settings navigation loop.
  Implements its operations.
- `src/eden_uwp/ui/library_screen.h` — Runs the library, selection and settings navigation loop;
  declares its interface and state types.
- `src/eden_uwp/ui/mods.cpp` — Fetches GameBanana listings and installs/enables per-title mods.
  Implements its operations.
- `src/eden_uwp/ui/mods.h` — Fetches GameBanana listings and installs/enables per-title mods;
  declares its interface and state types.
- `src/eden_uwp/ui/mods_screen.cpp` — Draws the mod store and its detail/filter interactions.
  Implements its operations.
- `src/eden_uwp/ui/mods_screen.h` — Draws the mod store and its detail/filter interactions; declares
  its interface and state types.
- `src/eden_uwp/ui/renderer.cpp` — Provides the Direct2D/DirectWrite UI renderer, images and frame
  capture. Implements its operations.
- `src/eden_uwp/ui/renderer.h` — Provides the Direct2D/DirectWrite UI renderer, images and frame
  capture; declares its interface and state types.
- `src/eden_uwp/ui/savesync_ui.cpp` — Runs save-sync sign-in, progress and settings screens.
  Implements its operations.
- `src/eden_uwp/ui/savesync_ui.h` — Runs save-sync sign-in, progress and settings screens; declares
  its interface and state types.
- `src/eden_uwp/ui/sources_screen.cpp` — Manages user-supplied Tinfoil-format sources and download
  browsing. Implements its operations.
- `src/eden_uwp/ui/sources_screen.h` — Manages user-supplied Tinfoil-format sources and download
  browsing; declares its interface and state types.
- `src/eden_uwp/ui/strings.cpp` — Owns translated UI strings and language selection. Implements its
  operations.
- `src/eden_uwp/ui/strings.h` — Owns translated UI strings and language selection; declares its
  interface and state types.
- `src/eden_uwp/ui/text_entry.cpp` — Runs controller-accessible text entry. Implements its
  operations.
- `src/eden_uwp/ui/text_entry.h` — Runs controller-accessible text entry; declares its interface and
  state types.
- `src/eden_uwp/ui/theme.h` — Defines shared UI colors and layout tokens.
- `src/eden_uwp/ui/update_version.h` — Parses and compares update/package versions on any host.
- `src/eden_uwp/ui/updater.cpp` — Checks releases and deploys updates through Device Portal.
  Implements its operations.
- `src/eden_uwp/ui/updater.h` — Checks releases and deploys updates through Device Portal; declares
  its interface and state types.
- `src/eden_uwp/ui/usb_import_screen.cpp` — Draws USB game/key import selection and progress.
  Implements its operations.
- `src/eden_uwp/ui/usb_import_screen.h` — Draws USB game/key import selection and progress; declares
  its interface and state types.
- `src/eden_uwp/ui/widgets.cpp` — Provides shared pills, hint bars and other screen drawing
  primitives. Implements its operations.
- `src/eden_uwp/ui/widgets.h` — Provides shared pills, hint bars and other screen drawing
  primitives; declares its interface and state types.
- `src/eden_uwp/title_settings.h` — Provides portable label/value parsing, resolution validation
  and per-title settings paths/persistence.
- `src/eden_uwp/usb_library.cpp` — Finds removable game folders and imports games/keys from USB
  storage.
- `src/eden_uwp/usb_library.h` — Declares removable-library discovery and import operations.
- `src/eden_uwp/uwp_boot.cpp` — Owns UWP activation and the boot/CPU diagnostic entry points.
- `src/eden_uwp/vma_impl.cpp` — Provides the Vulkan Memory Allocator implementation translation
  unit.

### Build and Mesa tooling: tools/nxbox

- `tools/nxbox/build-graphics.cmd` — Selects the Visual Studio x64 UWP toolchain and builds the
  standalone graphics probe in build-graphics.
- `tools/nxbox/build-mesa.cmd` — Builds the prepared Mesa checkout with the pinned Meson fork and
  installs the d3d12/WGL runtime into mesa-install.
- `tools/nxbox/build-uwp.cmd` — Configures the uwp-x64 preset, compiles frontend objects first, then
  builds eden-uwp; the frontend argument stops after the object check.
- `tools/nxbox/fetch_mesa_build.py` — Validates a successful main-branch workflow_dispatch Mesa run
  and stages its runtime DLLs and license using gh.
- `tools/nxbox/inspect_dxil.py` — Decodes DXBC/DXIL container metadata and signatures from a binary
  or JSON hex capture; it does not validate shader instructions.
- `tools/nxbox/package_launcher.py` — Stages and signs a per-game launcher tile with title ID,
  display name and eShop artwork.
- `tools/nxbox/package_uwp.py` — Validates an x64 PE payload, stages cpu/graphics/game UWP packages
  and signs the Appx with the configured certificate.
- `tools/nxbox/patch_mesa_uwp.py` — Applies the ordered, anchor-checked NXbox changes to the pinned
  Mesa source tree.
- `tools/nxbox/prepare_dxil.py` — Fetches checksum-pinned Microsoft DXC files and stages shader
  validation/compiler DLLs and distribution notices.
- `tools/nxbox/prepare_host_tools.py` — Fetches the checksum-pinned Windows glslang host compiler
  into build-host-tools/glslang.
- `tools/nxbox/prepare_mesa.py` — Stages the checksum-pinned upstream alpha-2-resfix Mesa runtime
  and license for the graphics probe.
- `tools/nxbox/release_version.py` — Converts stable tags into four-part Appx versions and rejects
  versions below or equal to published stable releases.
- `tools/nxbox/run_logged.py` — Runs a command, streams merged output to CI and saves the complete
  log while preserving the exit status.
- `tools/nxbox/mesa_api_ring.h` — Records a process-wide ring of D3D12 API completions and resource
  counters.
- `tools/nxbox/mesa_batch_reuse.h` — Guards batch-storage reuse until GPU completion and stops
  poisoned recording.
- `tools/nxbox/mesa_dred.h` — Configures DRED and captures the first device-removal
  breadcrumbs/page-fault report.
- `tools/nxbox/mesa_gpu_profile.h` — Adds per-command GPU timestamps and latency summaries to the
  batch journal.
- `tools/nxbox/mesa_heap_policy.h` — Selects abstract GPU heap properties and feature-gated
  residency flags.
- `tools/nxbox/mesa_lifetime.h` — Reports lifetime/API failures and supports bounded PSO ownership.
- `tools/nxbox/mesa_list_ring.h` — Tracks command-list/descriptor history and captures debug
  info-queue messages.
- `tools/nxbox/mesa_pso_first.h` — Retains the first rejected PSO description and shader-bytecode
  capture.
- `tools/nxbox/mesa_pso_guard.h` — Defines the opt-in conservative PSO shape guard.
- `tools/nxbox/mesa_pso_input.h` — Parses input signatures, normalizes PSO input/blend state and
  implements opt-in known-pair quarantine.
- `tools/nxbox/mesa_query_wait.h` — Waits for query completion with device-loss and
  unsubmitted-fence checks.
- `tools/nxbox/mesa_sync_batch.h` — Implements synchronous submission helpers, command journaling
  and copy/clear safety wrappers.
- `tools/nxbox/__pycache__/patch_mesa_uwp.cpython-314.pyc` — Tracked generated Python bytecode; the
  maintained patch source is patch_mesa_uwp.py.
- `tools/nxbox/mesa_view_cast.h` — Counts view-cast paths and bounded distinct format pairs.
- `tools/nxbox/mesa_view_cast_copy.h` — Owns SRV shadows and copies raw texels through a GPU buffer.
- `tools/nxbox/README.md` — Script usage and helper ownership index.

### CI: .github/workflows

- `.github/workflows/build-nxbox.yml` — Windows 2025 UWP build and C++ port checks; uploads
  nxbox-uwp-build (binaries, configuration and build logs).
- `.github/workflows/frontend-check.yml` — Manual Windows 2025 frontend-object compilation; uploads
  nxbox-frontend-check (configure/frontend logs).
- `.github/workflows/graphics-probe.yml` — Builds and signs the standalone graphics probe; uploads
  nxbox-graphics-probe (.appx).
- `.github/workflows/mesa-uwp.yml` — Manual Windows 2022 pinned Mesa/Meson build; uploads
  nxbox-mesa-uwp (installed runtime, license, source evidence, logs and PDBs).
- `.github/workflows/package-game-tile.yml` — Manual per-title launcher build/signing; uploads
  nxbox-game-tile-<title_id> (.appx).
- `.github/workflows/package-nxbox.yml` — Packages a trusted emulator build plus patched Mesa/DXIL
  for game mode; uploads nxbox-diagnostic or nxbox-release (.appx and build-info.json).
- `.github/workflows/port-tests.yml` — Runs standalone CMake/CTest port checks on macOS, Windows and
  Linux; produces test results in the job log.
- `.github/workflows/release-nxbox.yml` — On v* tags, validates increasing versions, calls
  build/package workflows and publishes signed Appx/build-info.json release assets.

### Host regression checks: tests/port

- `tests/port/CMakeLists.txt` — Defines portable C++ tests plus Windows sparse integration and
  optional zlib cheat parsing.
- `tests/port/cheats_parse.cpp` — Checks ZIP/Atmosphere cheat parsing and writing.
- `tests/port/demand_commit.cpp` — Checks demand-commit chunk policy.
- `tests/port/fixtures/botw-pipe6-dxil.json` — Retains captured BotW shader containers for
  signature/hash regressions.
- `tests/port/fixtures/list_ring/mock.h` — Supplies mock D3D12/COM APIs for list-ring host checks.
- `tests/port/fixtures/list_ring/syntax.cpp` — Instantiates list wrappers for real DirectX-Headers
  syntax checking.
- `tests/port/fixtures/mesa-15acdd7/README.md` — Records pinned Mesa fixture provenance and
  checksums.
- `tests/port/fixtures/mesa-15acdd7/d3d12_batch.cpp` — Retains pinned d3d12_batch source anchors for
  patch regression checks.
- `tests/port/fixtures/mesa-15acdd7/d3d12_blit.cpp` — Retains pinned d3d12_blit source anchors for
  patch regression checks.
- `tests/port/fixtures/mesa-15acdd7/d3d12_context.cpp` — Retains pinned d3d12_context source anchors
  for patch regression checks.
- `tests/port/fixtures/mesa-15acdd7/d3d12_draw.cpp` — Retains pinned d3d12_draw source anchors for
  patch regression checks.
- `tests/port/fixtures/mesa-15acdd7/d3d12_pipeline_state.cpp` — Retains pinned d3d12_pipeline_state
  source anchors for patch regression checks.
- `tests/port/fixtures/mesa-15acdd7/d3d12_query.cpp` — Retains pinned d3d12_query source anchors for
  patch regression checks.
- `tests/port/fixtures/mesa-15acdd7/d3d12_root_signature.cpp` — Retains pinned d3d12_root_signature
  source anchors for patch regression checks.
- `tests/port/fixtures/mesa-15acdd7/d3d12_screen.cpp` — Retains pinned d3d12_screen source anchors
  for patch regression checks.
- `tests/port/protocol_uri.cpp` — Checks nxbox protocol title parsing.
- `tests/port/sparse_memory.cpp` — Checks Windows sparse reservation, concurrent first touches and
  committed-byte accounting.
- `tests/port/test_dxil_inspector.py` — Checks captured DXIL container metadata/signature
  inspection.
- `tests/port/test_image_slot_patch.py` — Checks shader-image format-emulation indexing at nonzero
  start slots.
- `tests/port/test_jit_hot_paths.py` — Checks JIT assertion/hot-path source invariants.
- `tests/port/test_mesa_api_ring.py` — Checks process-wide API-ring generation and reporting with
  host mocks.
- `tests/port/test_mesa_api_ring_scopes.py` — Checks API-call coverage and generated instrumentation
  scopes on pinned sources.
- `tests/port/test_mesa_dred.py` — Checks fixture integrity and DRED patch integration.
- `tests/port/test_mesa_first_bad_batch.py` — Checks BC/view/barrier fixes and complete-source patch
  integration.
- `tests/port/test_mesa_full_chain.py` — Checks the whole patch chain, including pristine pinned
  sources and CRLF handling.
- `tests/port/test_mesa_heap_policy.py` — Checks heap properties and feature-gated residency policy.
- `tests/port/test_mesa_invalid_commands.py` — Checks descriptor capacity, state promotions and
  planar-copy fixes.
- `tests/port/test_mesa_journal.py` — Checks command journaling, synchronous submission and copy
  safety.
- `tests/port/test_mesa_list_ring.py` — Checks list history/debug captures and optional real
  DirectX-Headers syntax.
- `tests/port/test_mesa_pso.py` — Checks fixture integrity and PSO fallback generation/cache
  integration.
- `tests/port/test_mesa_pso_guard.py` — Checks conservative guards and known-bytecode quarantine
  behavior.
- `tests/port/test_mesa_view_cast.py` — Checks SRV shadow copies, lifetime, format filters, diagnostics and full-chain integration.
- `tests/port/test_mesa_query_policy.py` — Checks software versus hardware query modes.
- `tests/port/test_mesa_render_safety.py` — Checks subresource/copy/query safety and frontend
  device-loss exit integration.
- `tests/port/test_package.py` — Checks UWP payload validation and package staging.
- `tests/port/test_package_launcher.py` — Checks per-game launcher staging and manifest/art
  generation.
- `tests/port/test_title_settings.py` — Checks settings parsing, override precedence, title paths,
  resolution enum values and per-title persistence on the host.
- `tests/port/test_release_version.py` — Checks stable release-tag parsing and increasing package
  versions.
- `tests/port/update_version.cpp` — Checks portable update-version parsing and ordering.

### NXbox changes inside Eden

Located with `rg -l 'NXBOX|YUZU_UWP_APPCONTAINER|NxboxStall' src/common src/core src/video_core`.
This includes unguarded diagnostics bearing NXbox markers, not just preprocessor branches. NXBOX_UWP
and YUZU_UWP_APPCONTAINER are compile definitions, not environment switches.

- `src/common/CMakeLists.txt` — Enables AppContainer code and sparse-memory sources for
  WindowsStore.
- `src/common/assert.h` — Provides the optional inline JIT assertion fast path.
- `src/common/fiber.cpp` — Reduces AppContainer fiber stacks to 512 KiB.
- `src/common/fs/file.cpp` — Uses FromApp file-opening fallback and handle-based size queries.
- `src/common/fs/fs.cpp` — Uses FromApp file/directory operations and enumeration fallbacks.
- `src/common/fs/fs_uwp.h` — Wraps FromApp directory enumeration.
- `src/common/fs/path_util.cpp` — Resolves writable paths to LocalState without shell32 imports.
- `src/common/host_memory.cpp` — Implements AppContainer memory/protection handling, demand
  commitment and decommit attribution.
- `src/common/host_memory.h` — Exposes committed guest-backing byte diagnostics.
- `src/common/nxbox_stall.h` — Owns shared stall/JIT counters and sampler thread IDs.
- `src/common/settings.cpp` — Disables fastmem in the AppContainer configuration.
- `src/common/sparse_large_vector.cpp` — Uses the sparse allocator for AppContainer reservations and
  touched-page commitment.
- `src/common/thread.cpp` — Caps AppContainer host thread priority at normal.
- `src/core/CMakeLists.txt` — Enables NXBOX_UWP and YUZU_UWP_APPCONTAINER privately for core.
- `src/core/arm/dynarmic/arm_dynarmic_32.cpp` — Caps the UWP ARM32 JIT cache at 128 MiB.
- `src/core/arm/dynarmic/arm_dynarmic_64.cpp` — Caps the UWP ARM64 JIT cache and records
  memory-access context and JIT events.
- `src/core/arm/dynarmic/arm_dynarmic_64.h` — Retains the currently running guest thread for fault
  context.
- `src/core/arm/nxbox_fault.h` — Defines thread-local memory-access context and SVC history records.
- `src/core/cpu_manager.cpp` — Publishes emulated CPU host thread IDs for stack sampling.
- `src/core/crypto/aes_util.cpp` — Attributes AES work to the stall profiler.
- `src/core/file_sys/content_archive.cpp` — Validates base/update NCA pairing and reports RomFS
  mount/verification failures.
- `src/core/file_sys/patch_manager.cpp` — Reports selected update ExeFS and layered executable
  provenance.
- `src/core/file_sys/romfs_factory.cpp` — Reports whether the base RomFS came from the loader or
  content provider.
- `src/core/file_sys/romfs_read_diagnostics.h` — Controls RomFS verification and bounded BKTR read
  diagnostics.
- `src/core/file_sys/romfs_verify.cpp` — Verifies RomFS integrity and reports base/update relocation
  and encryption provenance.
- `src/core/file_sys/vfs/vfs_real.cpp` — Uses FromApp enumeration/attributes and attributes file
  reads to stalls.
- `src/core/hle/kernel/k_page_table_base.cpp` — Counts JIT invalidations caused by page-table
  changes.
- `src/core/hle/kernel/k_process.cpp` — Reports guest code, heap, alias and stack memory layout.
- `src/core/hle/kernel/k_thread.h` — Stores per-thread SVC diagnostic history.
- `src/core/hle/kernel/physical_core.cpp` — Records SVC inputs/completion outputs across core
  migration.
- `src/core/hle/kernel/svc/svc_memory.cpp` — Reports failed guest MapMemory operations.
- `src/core/hle/kernel/svc/svc_physical_memory.cpp` — Reports heap-size and physical-memory mapping
  failures with resource limits.
- `src/core/hle/service/am/frontend/applet_controller.cpp` — Reports controller-support applet
  completion on Windows.
- `src/core/hle/service/kernel_helpers.cpp` — Reports service event creation counts for
  resource-exhaustion diagnosis.
- `src/core/hle/service/vi/application_display_service.cpp` — Reports display-vsync event requests
  and event-map size.
- `src/core/loader/deconstructed_rom_directory.cpp` — Reports NPDM title and guest process
  configuration.
- `src/core/loader/nso.cpp` — Hashes loaded executable sections and reports final patched NSO
  provenance.
- `src/core/memory.cpp` — Reports unmapped/split/exclusive guest accesses with fault context.
- `src/video_core/CMakeLists.txt` — Enables UWP limits and the no-MSAA-storage-image fallback.
- `src/video_core/gpu_thread.cpp` — Publishes the GPU host thread ID and attributes GPU-thread busy
  time.
- `src/video_core/host1x/ffmpeg.cpp` — Caps UWP movie decoding at two threads and attributes decode
  time.
- `src/video_core/renderer_opengl/gl_buffer_cache.cpp` — Attributes buffer readback and glFinish
  stalls.
- `src/video_core/renderer_opengl/gl_compute_pipeline.cpp` — Builds UWP compute programs through
  GLSL even under the SPIR-V renderer setting.
- `src/video_core/renderer_opengl/gl_fence_manager.cpp` — Flushes pending commands during UWP GL
  fence waits and profiles them.
- `src/video_core/renderer_opengl/gl_query_cache.cpp` — Attributes blocking query-result reads.
- `src/video_core/renderer_opengl/gl_rasterizer.cpp` — Reports display acceleration and scaling
  decisions.
- `src/video_core/renderer_opengl/gl_shader_cache.cpp` — Profiles shader construction and emits GLSL
  for UWP compute shaders.
- `src/video_core/renderer_opengl/gl_staging_buffer_pool.cpp` — Flushes UWP GL staging-buffer fence
  waits and profiles them.
- `src/video_core/renderer_opengl/gl_texture_cache.cpp` — Applies texture budgets, consumes Mesa
  residency usage, compares ASTC and profiles readbacks.
- `src/video_core/renderer_opengl/gl_texture_cache.h` — Declares texture-budget overrides and ASTC
  comparison hooks.
- `src/video_core/renderer_opengl/present/layer.cpp` — Reports CPU framebuffer fallback contents.
- `src/video_core/renderer_opengl/present/window_adapt_pass.cpp` — Runs opt-in layer
  thumbnails/readback probes and consumes Mesa reports.
- `src/video_core/renderer_opengl/renderer_opengl.cpp` — Writes opt-in composed-frame PPM probes.
- `src/video_core/renderer_opengl/util_shaders.cpp` — Implements the MSAA blit fallback and optional
  ASTC sRGB decode via UNORM.
- `src/video_core/renderer_opengl/util_shaders.h` — Owns framebuffer objects for the MSAA fallback.
- `src/video_core/texture_cache/texture_cache.h` — Applies runtime budgets, profiles
  upload/collection and triggers ASTC comparisons.
- `src/video_core/texture_cache/texture_cache_base.h` — Sets UWP texture-cache thresholds to 512 MiB
  expected and 768 MiB critical.
- `src/video_core/texture_cache/util.cpp` — Attributes texture conversion time.

Related support files used by these paths (without those search markers):

- `src/common/demand_commit.h` — Defines portable demand-commit policy exercised by host tests.
- `src/common/nxbox_gl_readback.h` — Preserves pixel-pack-buffer state around diagnostic GL
  readback.
- `src/common/sparse_memory.cpp` — Implements Windows sparse reservation and first-touch chunk
  commitment.
- `src/common/sparse_memory.h` — Declares sparse allocation/free and committed-byte reporting.

## Mesa patch chain

`tools/nxbox/patch_mesa_uwp.py` operates on aerisarn/mesa-uwp commit
`15acdd7ea2b9dcdd62f26fe86b88280d79efc46b`. Run it on a pristine disposable checkout: it mutates
source and checks exact anchors, rather than acting as an idempotent installer. `patch(root)` first
fixes worker CoreWindow lookup, atomic completion, cached display size and present interval. It then
calls every `patch_*` function in the order below. Helper destinations are under Mesa's
`src/gallium/drivers/d3d12/`.

| Order | Function                      | Responsibility                                                                                       | Installed helper input → destination                                                   |
| ----- | ----------------------------- | ---------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------- |
| 1     | `patch_query`                 | Prevents query-accumulation reentrancy and duplicate query begin operations.                         | —                                                                                      |
| 2     | `patch_pso`                   | Reports rejected PSOs, normalizes input/blend state and retries bounded graphics fallbacks.          | mesa_pso_guard.h → nxbox_pso_guard.h; mesa_pso_input.h → nxbox_pso_input.h             |
| 3     | `patch_dxil`                  | Loads packaged DXIL validation and publishes compiler/validator errors.                              | —                                                                                      |
| 4     | `patch_shader_model`          | Adds a runtime cap on the detected shader-model minor version.                                       | —                                                                                      |
| 5     | `patch_format_cast_report`    | Publishes relaxed-format-casting support.                                                            | —                                                                                      |
| 6     | `patch_draw`                  | Counts silent draw exits and snapshots issued draw state.                                            | —                                                                                      |
| 7     | `patch_null_pso`              | Drops draws/dispatches when pipeline creation returned null.                                         | —                                                                                      |
| 8     | `patch_bisect_switches`       | Adds draw/compute/stream-output bisection, hash buckets and CPU draw timing.                         | —                                                                                      |
| 9     | `patch_image_slot`            | Indexes image-format emulation by the actual shader-image slot.                                      | —                                                                                      |
| 10    | `patch_root_signature_report` | Publishes root-signature serialization and creation failures.                                        | —                                                                                      |
| 11    | `patch_batch`                 | Reports Close/Reset failure, removal reason and debug messages, with an optional recovery branch.    | —                                                                                      |
| 12    | `patch_dred`                  | Configures DRED before device creation and latches first-removal evidence.                           | mesa_dred.h → nxbox_dred.h                                                             |
| 13    | `patch_fence`                 | Treats a null fence as completed work.                                                               | —                                                                                      |
| 14    | `patch_lifetime`              | Reports API-boundary device loss and bounds context-owned PSO caches.                                | mesa_lifetime.h → nxbox_lifetime.h; mesa_pso_first.h → nxbox_pso_first.h               |
| 15    | `patch_sync_batch`            | Serializes submissions by default and wraps recorded commands with CPU journaling and GPU profiling. | mesa_sync_batch.h → nxbox_sync_batch.h; mesa_gpu_profile.h is spliced into that header |
| 16    | `patch_batch_reuse`           | Requires completion before recycling batch-owned storage.                                            | mesa_batch_reuse.h → nxbox_batch_reuse.h                                               |
| 17    | `patch_render_safety`         | Fixes copy/subresource ranges and ends lost-device or unsubmitted query waits.                       | mesa_query_wait.h → nxbox_query_wait.h                                                 |
| 18    | `patch_invalid_commands`      | Fixes descriptor-table capacity, read-only promotions and planar-copy array layers.                  | —                                                                                      |
| 19    | `patch_first_bad_batch`       | Aligns BC allocations/copies, adjusts texture feedback barriers and validates RTV/resource state.    | —                                                                                      |
| 20    | `patch_heap_policy`           | Uses abstract GPU heaps and feature-gated residency on UWP.                                          | mesa_heap_policy.h → nxbox_heap_policy.h                                               |
| 21    | `patch_query_policy`          | Replaces pipeline/stream-output statistics queries with software results by default.                 | —                                                                                      |
| 22    | `patch_device_api_ring`       | Instruments D3D12 calls, list histories, descriptor validation and optional debug-layer setup.       | mesa_api_ring.h → nxbox_api_ring.h; mesa_list_ring.h → nxbox_list_ring.h               |
| 23    | `patch_bo_counters`           | Tracks BO lifetime/byte counters and adds the resource-leak experiment.                              | —                                                                                      |
| 24    | `patch_vidmem_report`         | Publishes residency-manager video-memory budget and usage.                                           | —                                                                                      |
| 25    | `patch_no_evict`              | Adds an eviction bypass and eviction/residency failure counters.                                     | —                                                                                      |
| 26    | `patch_resident_create`       | Adds an override that disables non-resident resource creation.                                       | —                                                                                      |
| 27    | `patch_view_cast`             | Reinterprets equal-size color SRVs via GPU buffers; guards other invalid views.                       | mesa_view_cast.h → nxbox_view_cast.h; mesa_view_cast_copy.h → nxbox_view_cast_copy.h                          |
| 28    | `patch_buffer_staging`        | Routes eligible busy-buffer discard writes through upload staging instead of waiting.                | —                                                                                      |

`NXBOX_MESA_SKIP` is a **patch-time** comma-separated setting, not a console switch. The code
supports `query`, `pso`, `dxil` and `fence`; the CI input description advertises only query/fence.
Later patches depend on the chain and its checked anchors, so an omitted patch is a bisection
experiment. `fast_poll()` rewrites helper `Sleep(1)` calls to SwitchToThread/YieldProcessor polling.

## Runtime environment switches

`MesaRuntime::Initialize` in `src/eden_uwp/mesa_window.cpp` loads `LocalState\nxbox_env.txt` before
loading Mesa. Use `NAME=value`, one per line; empty lines and lines beginning with `#` are ignored.
Restart the process after editing: many readers cache their first value. Defaults below describe
NXbox with no override. **Diagnostic** means instrumentation or an experimental
bisection/workaround; such switches can alter output, timing or memory use.

Readers under `tools/nxbox/` become code in the patched Mesa DLLs. `patch_mesa_uwp.py:patch_*`
identifies the generator function and names the generated Mesa reader where useful.

| Switch                         | Default                                               | Effect / accepted value                                                                                                                                  | Diagnostic? | Where read                                                                                                  |
| ------------------------------ | ----------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------- | ----------------------------------------------------------------------------------------------------------- |
| `NXBOX_ALLOC_TRACK`            | Off                                                   | Leading `1` enables allocation call-site tracking; periodic ALLOCS output also needs VMMAP.                                                              | Yes         | src/eden_uwp/game_session.cpp:RunGame                                                                       |
| `NXBOX_API_RING`               | Off in NXbox                                          | `1` requests per-call history; helper disables on exact `0` or SYNC_BATCH=0 (otherwise standalone helper defaults on).                                   | Yes         | tools/nxbox/mesa_api_ring.h:nxbox_api_enabled; frontend default in mesa_window.cpp                          |
| `NXBOX_ASTC_CHECK`             | Off                                                   | Presence, including `0`, requests bounded GPU-versus-CPU ASTC comparisons.                                                                               | Yes         | src/video_core/renderer_opengl/gl_texture_cache.cpp:NxboxAstcCheckWanted                                    |
| `NXBOX_ASTC_SRGB_VIA_UNORM`    | Off                                                   | Leading `1` decodes sRGB ASTC into a UNORM scratch texture before copying.                                                                               | Yes         | src/video_core/renderer_opengl/util_shaders.cpp:ASTCDecode                                                  |
| `NXBOX_AUDIO`                  | XAudio2                                               | Exact `null` selects silence.                                                                                                                            | Yes         | src/eden_uwp/game_session.cpp:RunGame                                                                       |
| `NXBOX_BUFFER_STAGING`         | On                                                    | Leading `0` disables eligible busy-buffer upload staging.                                                                                                | Yes         | patch_mesa_uwp.py:patch_buffer_staging → d3d12_resource.cpp:nxbox_use_staging                               |
| `NXBOX_CTX_DIRECT`             | Off                                                   | Leading `1` creates later shared contexts on the caller instead of UI dispatch.                                                                          | Yes         | src/eden_uwp/mesa_window.cpp:CreateSharedContext                                                            |
| `NXBOX_D3D12_DEBUG`            | Off                                                   | Exact `1` requests the debug layer and unfiltered info queue when available.                                                                             | Yes         | tools/nxbox/mesa_list_ring.h:nxbox_debug_enabled; patch_device_api_ring → d3d12_screen.cpp                  |
| `NXBOX_D3D12_DUMP_EVERY_CLOSE` | Off                                                   | Exact `1` captures successful Close calls too and raises capture retention from one to five.                                                             | Yes         | tools/nxbox/mesa_api_ring.h; mesa_list_ring.h                                                               |
| `NXBOX_D3D12_GBV`              | Off                                                   | Exact `1` requests GPU-based and synchronized-queue validation under D3D12_DEBUG.                                                                        | Yes         | patch_mesa_uwp.py:patch_device_api_ring → d3d12_screen.cpp                                                  |
| `NXBOX_D3D12_MAX_SM`           | Device maximum                                        | Numeric minor version caps shader model 6.x (e.g. `7` for 6.7).                                                                                          | Yes         | patch_mesa_uwp.py:patch_shader_model → d3d12_screen.cpp                                                     |
| `NXBOX_D3D12_QUERIES`          | Safe mode                                             | `full` uses hardware queries; `none` makes all non-timestamp queries software; otherwise pipeline/SO statistics are software (occlusion stays hardware). | Yes         | patch_mesa_uwp.py:patch_query_policy → d3d12_query.cpp:nxbox_soft_subquery                                  |
| `NXBOX_D3D12_RECOVER`          | Off                                                   | Presence requests the legacy Close-failure drop/reset branch; final-chain poisoned-list guards still stop failed recording.                              | Yes         | patch_mesa_uwp.py:patch_batch → d3d12_batch.cpp                                                             |
| `NXBOX_DIAG`                   | Off                                                   | Presence, including `0`, enables periodic layer thumbnails, blocking readback and known-color probes.                                                    | Yes         | src/video_core/renderer_opengl/present/window_adapt_pass.cpp                                                |
| `NXBOX_DISABLE_DLC`            | Unset                                                 | Hex title ID appends DLC to that title’s disabled add-ons.                                                                                               | Yes         | src/eden_uwp/game_session.cpp:RunGame                                                                       |
| `NXBOX_DRAW_BUCKETS`           | Off                                                   | `n,k` drops draws whose combined VS/PS hash is k modulo n; n must be at least 2.                                                                         | Yes         | patch_mesa_uwp.py:patch_bisect_switches → d3d12_draw.cpp:nxbox_draw_bucket_skip                             |
| `NXBOX_DRAW_BUCKET_LOG`        | Unset                                                 | File for bounded skipped-hash reports; frontend sets LocalState\bucket.txt and removes its previous copy when DRAW_BUCKETS is present.                   | Yes         | patch_bisect_switches → d3d12_draw.cpp; src/eden_uwp/mesa_window.cpp                                        |
| `NXBOX_FRAME_DUMP`             | Off                                                   | Leading `1` sets FRAME_DUMP_DIR to LocalState\framedump.                                                                                                 | Yes         | src/eden_uwp/mesa_window.cpp:MesaRuntime::Initialize                                                        |
| `NXBOX_FRAME_DUMP_DIR`         | Unset                                                 | Nonempty directory writes composed-frame PPM probes every 150 frames.                                                                                    | Yes         | src/video_core/renderer_opengl/renderer_opengl.cpp:DumpProbeFrame                                           |
| `NXBOX_GPU_PROFILE`            | Off                                                   | Exact `1` adds GPU timestamps to journaled main-list commands; needs SYNC_BATCH enabled and JOURNAL=1 for useful command attribution.                    | Yes         | tools/nxbox/mesa_gpu_profile.h:nxbox_profile_on                                                             |
| `NXBOX_JOURNAL`                | Off in NXbox                                          | `1` requests command journaling; helper disables on exact `0` and requires SYNC_BATCH (standalone helper otherwise defaults on).                         | Yes         | tools/nxbox/mesa_sync_batch.h:nxbox_journal_reset; frontend default in mesa_window.cpp                      |
| `NXBOX_LANG`                   | System language, then supported fallback              | Case-insensitive `pt`/`en` prefixes force UI language; read directly from nxbox_env.txt before Mesa starts too.                                          | No          | src/eden_uwp/ui/strings.cpp:ForcedLanguage/ResolveLanguage                                                  |
| `NXBOX_LEAK_RESOURCES`         | Off                                                   | Leading `1` bypasses BO destruction to test premature release; grows memory usage.                                                                       | Yes         | patch_mesa_uwp.py:patch_bo_counters → d3d12_bufmgr.cpp                                                      |
| `NXBOX_NO_EVICT`               | Off                                                   | Leading `1` bypasses residency eviction.                                                                                                                 | Yes         | patch_mesa_uwp.py:patch_no_evict → d3d12_residency.cpp                                                      |
| `NXBOX_PRESENT_INTERVAL`       | At least 1 from requested interval                    | Nonempty integer overrides DXGI sync interval; `0` queues without waiting, with no tearing flag.                                                         | Yes         | patch_mesa_uwp.py:patch → d3d12_wgl_framebuffer_uwp.cpp                                                     |
| `NXBOX_PSO_FIX`                | On                                                    | Exact `0` disables input/blend normalization; quarantine is controlled separately.                                                                       | Yes         | tools/nxbox/mesa_pso_input.h                                                                                |
| `NXBOX_PSO_GUARD`              | Off                                                   | Exact `1` enables the conservative shape-based PSO rejection guard.                                                                                      | Yes         | tools/nxbox/mesa_pso_guard.h:nxbox_pso_guard_enabled                                                        |
| `NXBOX_PSO_QUARANTINE`         | Off                                                   | Exact `1` skips two known VS/PS bytecode pairs before PSO creation.                                                                                      | Yes         | tools/nxbox/mesa_pso_input.h:nxbox_pso_quarantine_enabled                                                   |
| `NXBOX_READ_CHECK`             | Off                                                   | Leading `1` reads random game-file chunks before boot and logs results.                                                                                  | Yes         | src/eden_uwp/game_session.cpp:RunGame/CheckGameReads                                                        |
| `NXBOX_RESIDENT_CREATE`        | Off                                                   | Exact `1` disables CREATE_NOT_RESIDENT resource creation.                                                                                                | Yes         | patch_mesa_uwp.py:patch_resident_create → d3d12_screen.cpp                                                  |
| `NXBOX_SAMPLER`                | Off                                                   | Leading `1` starts the host stack sampler for emulator/driver threads.                                                                                   | Yes         | src/eden_uwp/game_session.cpp:RunGame/RunStackSampler                                                       |
| `NXBOX_SHADER_BACKEND`         | spirv                                                 | Case-insensitive `glsl` restores graphics GLSL; compute remains GLSL on UWP in either mode.                                                              | No          | src/eden_uwp/game_session.cpp:RunGame; gl_shader_cache.cpp/gl_compute_pipeline.cpp                          |
| `NXBOX_SHADER_CACHE`           | On if Eden disk cache is enabled                      | Exact `0` skips preloading/precompiling the per-title disk/shared cache.                                                                                 | Yes         | src/eden_uwp/game_session.cpp:RunGame                                                                       |
| `NXBOX_SHADER_SHARE_URL`       | Unset; sharing off                                    | Server base URL for per-title cache download/upload.                                                                                                     | No          | src/eden_uwp/shader_share.cpp:ShareUrl                                                                      |
| `NXBOX_SKIP_CLEAR`             | Off                                                   | Exact `1` drops wrapped clear commands.                                                                                                                  | Yes         | tools/nxbox/mesa_sync_batch.h:NxboxJournalCommands via nxbox_skip_class                                     |
| `NXBOX_SKIP_COMPUTE`           | Off                                                   | Leading `1` drops compute dispatches.                                                                                                                    | Yes         | patch_mesa_uwp.py:patch_bisect_switches → d3d12_draw.cpp                                                    |
| `NXBOX_SKIP_COPY`              | Off                                                   | Exact `1` drops wrapped copy commands.                                                                                                                   | Yes         | tools/nxbox/mesa_sync_batch.h:NxboxJournalCommands via nxbox_skip_class                                     |
| `NXBOX_SKIP_DRAW`              | Off                                                   | Leading `1` drops all graphics draws.                                                                                                                    | Yes         | patch_mesa_uwp.py:patch_bisect_switches → d3d12_draw.cpp                                                    |
| `NXBOX_SKIP_SO`                | Off                                                   | Leading `1` drops draws with stream-output targets.                                                                                                      | Yes         | patch_mesa_uwp.py:patch_bisect_switches → d3d12_draw.cpp                                                    |
| `NXBOX_SYNC_BATCH`             | On                                                    | Exact `0` disables synchronous completion waits and the dependent API ring/journal paths.                                                                | Yes         | tools/nxbox/mesa_sync_batch.h:nxbox_sync_batch_enabled; mesa_api_ring.h                                     |
| `NXBOX_TEXCACHE_MB`            | No override; 512/768 MiB expected/critical thresholds | Positive MiB sets expected cache threshold and critical to 1.25× that value; this is a collection threshold, not a hard allocation cap.                  | Yes         | src/video_core/renderer_opengl/gl_texture_cache.cpp:ApplyMemoryBudgetOverride                               |
| `NXBOX_TEXTURE_BARRIER`        | Aliasing barrier                                      | Value beginning with `w` (normally `wait`) restores submit-and-wait texture barriers.                                                                    | Yes         | patch_mesa_uwp.py:patch_first_bad_batch → d3d12_context.cpp                                                 |
| `NXBOX_UPDATES`                | On                                                    | Leading `0` disables external content directories used for updates/DLC.                                                                                  | Yes         | src/eden_uwp/game_session.cpp:RunGame                                                                       |
| `NXBOX_VERIFY_ROMFS`           | Off                                                   | Leading `1` enables RomFS verification and BKTR read diagnostics.                                                                                        | Yes         | src/core/file_sys/romfs_read_diagnostics.h:IsRomfsVerificationEnabled; content_archive.cpp/romfs_verify.cpp |
| `NXBOX_VIEW_CAST_COPY` | On | Exact `0` disables GPU SRV reinterpretation and restores resource-format fallback. Read once. Shadows are refreshed on bind and before draw/dispatch, with two GPU copies per mip/layer. | Yes | tools/nxbox/mesa_view_cast.h:nxbox_view_cast_copy_enabled |
| `NXBOX_VMMAP`                  | Off                                                   | Leading `1` logs committed virtual memory and guest accounting every 30 seconds.                                                                         | Yes         | src/eden_uwp/game_session.cpp:RunGame/LogVirtualMemoryMap                                                   |
| `GALLIUM_THREAD`               | true                                                  | Mesa boolean option; `0` returns the underlying pipe context without the threaded-context wrapper.                                                       | Yes         | Pinned Mesa src/gallium/auxiliary/util/u_threaded_context.c:threaded_context_create                         |

### Names that are not user switches

The broad `NXBOX_[A-Z0-9_]*` search also finds **outputs**, compile definitions and patch markers.
Do not put report text into nxbox_env.txt. `NXBOX_D3D12_DRED` is a setup/removal report, not a DRED
enable flag: patch_dred requests configuration automatically when supported. `NXBOX_D3D12_LIST_RING`
is likewise an emitted frontend label, not a list-ring toggle.

`NXBOX_UWP`, `NXBOX_INLINE_JIT_ASSERTS`, `NXBOX_NO_MSAA_STORAGE_IMAGES`, `NXBOX_STALL_PROFILE`,
`NXBOX_API_RING_CORE_ONLY`, `NXBOX_API_RING_NO_LIST`, `NXBOX_DRED_IMPLEMENTATION`,
`NXBOX_API_METHOD`, `NXBOX_DRED_OP`, `NXBOX_POLL_PAUSE` and `NXBOX_PROFILE_*` are compile-time
macros. `NXBOX_GPU_PROFILE_HOOK` is a splice marker; `NXBOX_PADDLE_*` is a guest debug-string
prefix. `NXBOX_MESA_SRC` and `NXBOX_DIRECTX_HEADERS` configure host tests, not the app.

Mesa writes the following diagnostic/status names or chunk prefixes; the frontend consumes them
through `diagnostic_report.h`, `game_session.cpp`, `mesa_window.cpp` and the presentation/readback
paths. DEVICE_LOST and COMMAND_FAILURE also stop the session; VIDMEM feeds texture-cache accounting.
Names generated with a numeric suffix are report chunks.

- `NXBOX_D3D12_API_FIRST`
- `NXBOX_D3D12_API_RING`
- `NXBOX_D3D12_API_RING_`
- `NXBOX_D3D12_API_RING_ERROR`
- `NXBOX_D3D12_BATCH`
- `NXBOX_D3D12_BATCH_JOURNAL`
- `NXBOX_D3D12_BATCH_JOURNAL_`
- `NXBOX_D3D12_BATCH_JOURNAL_ERROR`
- `NXBOX_D3D12_BATCH_JOURNAL_PREVIOUS`
- `NXBOX_D3D12_BATCH_JOURNAL_PREVIOUS_`
- `NXBOX_D3D12_BATCH_TIME`
- `NXBOX_D3D12_CACHE`
- `NXBOX_D3D12_CALLS`
- `NXBOX_D3D12_COMMAND_FAILURE`
- `NXBOX_D3D12_DEBUG_STATUS`
- `NXBOX_D3D12_DEBUG_UNAVAILABLE`
- `NXBOX_D3D12_DESCRIPTOR_FAIL`
- `NXBOX_D3D12_DEVICE_LOST`
- `NXBOX_D3D12_DRAW`
- `NXBOX_D3D12_DRAWTIME`
- `NXBOX_D3D12_DRED`
- `NXBOX_D3D12_DRED2`
- `NXBOX_D3D12_DRIVER_TID`
- `NXBOX_D3D12_EVICT`
- `NXBOX_D3D12_FIRST_BAD_BATCH`
- `NXBOX_D3D12_FIRST_FAILURE`
- `NXBOX_D3D12_FIRST_MESSAGE`
- `NXBOX_D3D12_GBV_STATUS`
- `NXBOX_D3D12_GPUPROF`
- `NXBOX_D3D12_HEAP_POLICY`
- `NXBOX_D3D12_INFOQUEUE`
- `NXBOX_D3D12_LIST_`
- `NXBOX_D3D12_LIST_CAPTURES`
- `NXBOX_D3D12_LIST_CAPTURE_`
- `NXBOX_D3D12_LIST_ERROR`
- `NXBOX_D3D12_LIST_RING`
- `NXBOX_D3D12_MESSAGE`
- `NXBOX_D3D12_PREVIOUS_GOOD_BATCH`
- `NXBOX_D3D12_PSO`
- `NXBOX_D3D12_PSO_DXIL`
- `NXBOX_D3D12_PSO_DXIL_`
- `NXBOX_D3D12_PSO_DXIL_ERROR`
- `NXBOX_D3D12_PSO_FAIL`
- `NXBOX_D3D12_PSO_FAIL2`
- `NXBOX_D3D12_PSO_FAIL_FIRST`
- `NXBOX_D3D12_PSO_FALLBACK`
- `NXBOX_D3D12_PSO_FIX`
- `NXBOX_D3D12_QUAD`
- `NXBOX_D3D12_QUEUE`
- `NXBOX_D3D12_RELAXED_CAST`
- `NXBOX_D3D12_REMOVED`
- `NXBOX_D3D12_RESET`
- `NXBOX_D3D12_RESOURCES`
- `NXBOX_D3D12_RESOURCES_FIRST`
- `NXBOX_D3D12_ROOTSIG`
- `NXBOX_D3D12_SM`
- `NXBOX_D3D12_SYNC_ERROR`
- `NXBOX_D3D12_VIDMEM`
- `NXBOX_D3D12_VIEW_CAST`
- `NXBOX_D3D12_VIEW_CAST_PAIRS`
- `NXBOX_DXIL`
- `NXBOX_DXIL_ERROR`
- `NXBOX_DXIL_VALIDATE`

## Build and validation loop

### CI and local Windows builds

The workflow directory map above lists every workflow and artifact. For a game package, run
`mesa-uwp.yml` to build the patched runtime, run `build-nxbox.yml` for Eden, then use
`package-nxbox.yml` with the trusted build run and a successful Mesa run. Packaging selects the
latest successful main-branch Mesa run when none is supplied, and `fetch_mesa_build.py` verifies its
provenance. The automatic successful-build path packages game mode too. CPU mode does not need the
graphics runtime. Signing uses `NXBOX_PFX_BASE64` and `NXBOX_PFX_PASSWORD` CI secrets.

For a narrow frontend check, use `frontend-check.yml`. For an isolated driver test, use
`graphics-probe.yml`: absent mesa_run it stages the checksum-pinned upstream binary release;
supplying mesa_run tests the patched runtime. `package-game-tile.yml` produces a separate dashboard
tile that activates NXbox by protocol. Stable v* tags use `release-nxbox.yml` and the
increasing-version policy; do not use that publication workflow for a routine console experiment.

The full emulator build uses Windows/Visual Studio's x64 UWP environment and CMake 3.31 or newer.
From the repository root, after installing the prerequisites in [uwp_build.md](uwp_build.md), the
workflow's local entry points are:

```bat
python tools\nxbox\prepare_host_tools.py
tools\nxbox\build-uwp.cmd frontend
tools\nxbox\build-uwp.cmd
```

`build-mesa.cmd` expects prepared `.cache/mesa` and `.cache/meson` trees plus the generator
dependencies listed in `mesa-uwp.yml`. Mesa is pinned at `15acdd7ea2b9dcdd62f26fe86b88280d79efc46b`;
the aerisarn/meson fork is pinned at `d0a111134f032dcbfbc7b743a4bf811f99909868`. Apply the patch
script before building. Build outputs alone are not signed/installable packages.

### Host Python tests

Several checks use checked-in fixtures, but complete patch composition and some source checks
require a **pristine** aerisarn/mesa-uwp checkout at `/tmp/mesa-pin`. In a new location, fetch just
the pinned commit:

```sh
git init /tmp/mesa-pin
git -C /tmp/mesa-pin remote add origin https://github.com/aerisarn/mesa-uwp.git
git -C /tmp/mesa-pin fetch --depth 1 origin 15acdd7ea2b9dcdd62f26fe86b88280d79efc46b
git -C /tmp/mesa-pin checkout --detach FETCH_HEAD
# From the NXbox repository root:
cd tests/port && python3 -m unittest discover -s . -p "test_*.py"
```

If `/tmp/mesa-pin` already exists, check its revision and cleanliness rather than overwriting it.
`NXBOX_MESA_SRC` can point to another pristine checkout. The tests copy sources into temporary trees
before patching. Without the Mesa checkout, some tests skip; that is not complete chain validation.
`clang++` on PATH is needed for mock helper checks. Launcher checks skip when Pillow is missing. The
optional DirectX-Headers syntax check uses `NXBOX_DIRECTX_HEADERS` and may skip when headers are
unavailable. Launcher packaging tests use Pillow.

The separate C++ suite (CMake 3.20 minimum) runs from the repository root:

```sh
cmake -S tests/port -B build-port-tests
cmake --build build-port-tests --config Release --parallel 2
ctest --test-dir build-port-tests -C Release --output-on-failure
```

Windows adds sparse-memory integration; cheat parsing is built when zlib is found. `port-tests.yml`
currently runs this C++ suite, not the complete Python discovery command. Host checks establish
patch/source/policy behavior; Xbox GPU behavior still needs console validation.

### Console test loop

1. Install a signed candidate Appx through Xbox Device Portal; record emulator revision, Mesa run,
   package version, game version and content configuration.
2. Edit `LocalState\nxbox_env.txt` for environment overrides and `LocalState\eden_settings.txt` for
   Eden setting labels (`label=value`). The latter matches registered setting labels and logs
   applied/unknown entries; it is applied over boot defaults. `LocalState\settings\<TITLEID>.txt`
   uses the same format and is applied afterwards for the title read from the boot package, before
   core initialization. The library's Resolution action saves 1x, 1.5x, 2x or 3x per game
   (`resolution_setup=3`, `5`, `6` or `7`, respectively), preserving other entries. With no override,
   resolution defaults to 1x; 2x/3x show a memory warning. `RESOLUTION` in the diagnostic log records
   the title and effective enum value at boot. Keep each experiment small and restart the app.
3. Launch a library game (or a per-game protocol tile), reproduce the target scene, and check frame
   progression and visual output. Suspend/resume is part of the game-session lifecycle.
4. Retrieve `LocalState\eden_uwp_diag.txt` plus Eden's `LocalState\eden\log` output. The frontend
   log rotates to eden_uwp_diag.old.txt after exceeding 4 MiB at process start. Preserve the first
   failure, chunked reports and build-info.json with the run.
5. For image evidence set `NXBOX_FRAME_DUMP=1`; inspect the PPM files under `LocalState\framedump`,
   produced every 150 composed frames, and their brightness diagnostics. Keep the dump with the logs
   before the next run. Layer probes under NXBOX_DIAG are a separate, blocking readback diagnostic.
6. Compare the same scene with the baseline and remove experimental overrides once the question is
   answered. A host patch test or advancing frame counter alone does not establish correct game
   rendering.

## Where to look when

### Device removed or Close failed

Set `NXBOX_JOURNAL=1` and `NXBOX_API_RING=1` in nxbox_env.txt, keeping synchronous batches enabled
(the default). Read the earliest `D3D12_FIRST_FAILURE`, `D3D12_BATCH`, `D3D12_REMOVED`, DRED/DRED2,
first PSO rejection, API-ring/list captures and current/previous batch journal chunks in
eden_uwp_diag.txt. The frontend drops the NXBOX_ prefix in report labels. Follow command-list
identity, batch/submission and fence values through `mesa_api_ring.h`, `mesa_list_ring.h`,
`mesa_sync_batch.h` and patch_dred. DRED/debug availability is reported; unavailable output does not
prove the command was valid. Final-chain Close/Reset failures and device loss enter the terminal
session path rather than providing device recovery.

### Slow frames

Set `NXBOX_SAMPLER=1` and `NXBOX_GPU_PROFILE=1`; also set `NXBOX_JOURNAL=1` for GPU command
attribution and retain default synchronous submission. Compare GAME_PRESENT frame gaps, STALL/JIT
categories, SAMPLER stacks, D3D12_DRAWTIME, D3D12_BATCH_TIME and D3D12_GPUPROF. Look at
`game_session.cpp`, `common/nxbox_stall.h`, shader/cache/readback wait sites, and
`mesa_gpu_profile.h`. Instrumentation changes timing; compare an uninstrumented run afterward. Check
shader warm-up, repeated texture barriers and busy-buffer uploads before changing scheduling policy.

### Memory limit

The Xbox app budget is **5 GiB**, shared by guest memory, JIT, renderer and frontend. Set
`NXBOX_VMMAP=1` and `NXBOX_ALLOC_TRACK=1`, then compare MEM/MEMORY_LIMIT, VMMAP, ALLOCS, SPARSE,
GUEST_MEMORY and Mesa VIDMEM/resource counters. `RunGame` attempts larger limits (10, 8, 7, 6 GiB)
and logs whether the shell grants them; use the reported actual limit. `alloc_track.cpp` currently
tracks allocations of at least 2 KiB, despite the older 64 KiB comment in its header. Start with
`common/host_memory.cpp`, `common/sparse_memory.cpp`, `common/sparse_large_vector.cpp`, the Dynarmic
cache caps, `gl_texture_cache.cpp` and `mesa_heap_policy.h`. The texture-cache thresholds control
collection, not total process memory; BO creation totals are not live resident bytes.
