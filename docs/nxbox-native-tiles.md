# Native dashboard tiles: launch path and full-package decision

This change implements the direct-launch presentation path. It does not turn emulation into a
native port, change dashboard “now playing” identity, or eliminate the OS transition between two
packages. Full packages are feasible in principle but **not safe to ship with a path-only refactor**.
No `--full`, workflow switch, publisher-cache declaration, or partial `DataRoot()` implementation is
included. The main package and its existing data remain authoritative.

## Direct launch

A cold `nxbox://play?title=<ID>` activation takes priority over `skip_library.txt`, `move.txt`,
`usb_scan_test.txt`, `game.txt`, and `game.url`. It scans without NSZ conversion, validates the title,
and passes the selected path directly to `RunGame`. It does not run the library, USB detection UI,
setup, OTA UI, pending sync, boot sync, or post-game SaveSync. Existing sync markers are left alone;
cloud saves must be reconciled from a normal NXbox launch. Silently choosing a cloud conflict winner
would risk overwriting the player's save.

The splash reads the cached title name and banner/icon from `LocalState/library`. A cache miss uses
the title ID until the scan supplies the name. The existing art worker downloads missing eShop art;
network availability never gates the boot. Without cached art while offline, the fallback is the
name/ID and phase on black. A missing game, disconnected drive, missing keys, or load failure keeps
a localized game-specific error screen with B to exit, rather than opening the library or booting
the previously selected game. A game with only an NSZ must first be prepared in the normal library.

Rendering has two owners, in sequence:

1. Direct2D renders directly on the CoreWindow during lookup. The keys-phase frame is captured
   **before** `Present1`, then all Direct2D window resources are released.
2. The frontend's `MesaWindow` presents that captured frame using its bootstrap context. During
   `RunGame`, Direct2D renders into an offscreen bitmap; the existing renderer context blits its
   pixels onto the game's swap chain. Shader callbacks update atomic phase/count fields. Readback
   and presentation are limited to four times per second plus phase changes. The GL bindings,
   unpack state, scissor, and sRGB state are restored. No Mesa driver or `video_core` changes.
3. The final “Starting” image remains presented while the GPU starts. Splash graphics and CPU pixel
   storage are released before emulation; the first guest present replaces that image. There is
   no intentional blank clear or extra window. A compositor gap during swap-chain replacement
   remains a console validation item, not a claimed measured result.

The line represents launch phases, not elapsed-time percentage. Inline shader compilation does
not know its total until the final callback; the label shows the real built count, adding a total
when available. Keys/loading retain the last frame during synchronous initialization. A single long
shader compilation retains the last count until its callback returns. Each 1920x1080 BGRA bitmap
is 8,294,400 bytes (7.91 MiB); readback, upload, and graphics allocations add transient memory and
must be checked on the console near the UWP memory limit.

The launcher paints its packaged `Assets/SplashScreen.png`, preserving aspect ratio, while awaiting
`LaunchUriAsync`. It does no network or library work, exits immediately after acceptance, and cancels
a 20-second timeout. The OS splash uses the same asset and black background. No callback captures
stack state after the timeout. This is the same game's artwork, not a pixel-identical cross-process
animation: the manifest splash has a different aspect ratio and contains no live progress.

Existing warm-activation semantics remain: a second activation in an already running NXbox process
does not replace its game/library session. Test cold launches by terminating NXbox first. Safely
replacing a running session requires a separate stop/join/restart state machine, including save
reconciliation. The two-package launcher cannot claim native dashboard ownership.

## What a full package must contain

Stage the same runtime as `.github/workflows/package-nxbox.yml`, not just a renamed executable:

| Payload | Source and treatment |
| --- | --- |
| `nxbox.exe` | Exactly one `eden-uwp.exe` from trusted successful main-branch `nxbox-uwp-build`; `package_uwp.py` renames it. |
| Runtime DLLs | All DLLs adjacent to that executable, plus the patched `nxbox-mesa-uwp` artifact: at least `opengl32.dll`, `libgallium_wgl.dll`, `libglapi.dll`. Use `fetch_mesa_build.py` validation; do not use a desktop Mesa binary. |
| DXIL/DXC | `prepare_dxil.py`: SDK x64 `dxil.dll` preferred; pinned DXC fallback, optional `dxcompiler.dll`, app-local `msvcp140.dll`, `vcruntime140.dll`, `vcruntime140_1.dll` when available. Preserve the checksums and notices. |
| Package assets | Per-title icons, wide tile, splash, validated `title.txt`; display name and identity `NSPX.NXbox.Game.<ID>`. Keep `game.nro` until `BootView::Run` no longer uses its presence to select the graphical frontend. |
| Manifest | Same UWP target family, x64, VCLibs dependency, JIT `codeGeneration`, network/removable capabilities, matching signer/publisher. Only main NXbox owns `nxbox://`; full packages must omit that registration. |
| Notices | GPL, Mesa, DXC Microsoft/LLVM, and libnx while bundling the fixture. Distribute corresponding source for the shipped revisions. |

Every full tile carries the emulator/driver runtime. It need not carry any game bytes, keys, saves,
or shader cache inside the signed package. Different package identities do not justify assuming
runtime DLL deduplication for storage budgeting.

### Size accounting

No built Windows executable or Mesa artifact is in this worktree. The read-only GitHub artifact
query failed with `error connecting to api.github.com`; **binary and signed-package sizes are
unmeasured**, not zero and not inferred from an artifact ZIP containing symbols. Measured repository
payload: existing three main-app PNGs total 341,725 bytes; `game.nro` is 176,128 bytes. Launcher PNG
sizes depend on each image's compression. Full installed size is the sum of executable, staged
DLLs, generated PNGs, fixture, notices, and package metadata. N titles multiply that runtime cost;
the 15 GB game stays in one shared location.

Before enabling a full workflow, record per-file bytes and total from the final Windows staging
directory, separately from the compressed `.appx` and CI artifact ZIP. For example, after staging:

```powershell
Get-ChildItem out/package -Recurse -File | Select-Object FullName, Length
(Get-ChildItem out/package -Recurse -File | Measure-Object Length -Sum).Sum
Get-ChildItem out/*.appx | Select-Object Name, Length
```

Preserve emulator revision, Mesa run/revision, packaging revision and DXIL hash in build metadata.
“Latest successful” must still validate repository, workflow, event, branch and conclusion as the
current main workflow does. Pin the selected runs for reproducible re-packaging.

## One shared root: scope and blockers

Proposed API: `Common::FS::DataRoot()` returning a filesystem path, implemented in the common UWP
filesystem layer so both `PathManager` and the frontend can use it without a dependency cycle. Its
UWP implementation obtains `ApplicationData::Current().GetPublisherCacheFolder(L"NXboxData")`
when declared and migration is complete; otherwise it returns `LocalFolder().Path()`. Resolve it
once per process, on an initialized WinRT apartment, before Eden path initialization. Do not cache
an early failure before apartment initialization. Distinguish an undeclared feature from a missing
migration marker; a full tile must fail closed when shared data was expected but disappeared,
rather than creating fresh saves in a private fallback and later merging them blindly.

The manifest extension is **package-level, in the foundation namespace**, not a `uap:` application
extension:

```xml
<Extensions>
  <Extension Category="windows.publisherCacheFolders">
    <PublisherCacheFolders><Folder Name="NXboxData" /></PublisherCacheFolders>
  </Extension>
</Extensions>
```

Microsoft documents sharing for the same publisher and user, access only to declared subfolders,
and user-clearable storage without backup/roaming. The last app's uninstall removes it. This makes
an unbacked sole copy of saves unsuitable there. See [GetPublisherCacheFolder](https://learn.microsoft.com/en-us/uwp/api/windows.storage.applicationdata.getpublishercachefolder)
and the [manifest schema](https://learn.microsoft.com/en-us/uwp/schemas/appxpackage/uapmanifestschema/element-f-publishercachefolders).
Xbox Dev Mode support, path-based `CreateFileFromAppW` access, quotas, and reset/uninstall behavior
must be proven with two signed identities on the actual console; the API contract alone is not
that proof.

### Path audit

`rg -n 'LocalFolder\(' src --glob '*.{cpp,h}'` finds **18 calls across 10 files**. This counts actual
calls, not every downstream derived path:

| File | Calls | Intended handling |
| --- | ---: | --- |
| `src/common/fs/path_util.cpp` | 2 | Eden root and UWP roaming fallback. Central root change affects keys, NAND, profiles, load/mods, config, cache, shaders and logs. Override logs back to a private location. |
| `src/eden_uwp/game_session.cpp` | 7 | Game resolution, settings, log filter, disabled mods, content directories, USB diagnostic trigger, session root. Share content/settings; keep triggers/control files private. |
| `src/eden_uwp/mesa_window.cpp` | 2 | Environment overrides and present diagnostic trigger: private per identity, with deliberately chosen shared settings if needed. |
| `src/eden_uwp/usb_library.cpp` | 1 | Import target is a **StorageFolder**, not only a path; obtain the selected root folder with the broker before creating children. |
| `src/eden_uwp/save_sync.cpp` | 1 | Root feeds OAuth files, sync state/staging/backup. Needs account ownership and cross-process serialization. |
| `src/eden_uwp/ui/library_screen.cpp` | 1 | Root is passed to library/art/mods/import/screens; route those content services consistently. |
| `src/eden_uwp/ui/strings.cpp` | 1 | Language environment-file preference: choose shared vs per-package policy explicitly. |
| `src/eden_uwp/ui/updater.cpp` | 1 | Device Portal credential file: keep private; do not share as a side effect of content migration. |
| `src/eden_uwp/diagnostic.h` | 1 | Keep `diagnostic.txt` private so two processes cannot truncate each other's evidence. |
| `src/eden_uwp/uwp_boot.cpp` | 1 | Boot diagnostic output remains private. |

`ui/art.cpp`, `ui/library.cpp`, `ui/mods.cpp`, and `ui/usb_import_screen.cpp` take roots as arguments;
zero direct calls does not mean no work. `shader_share.cpp` derives its files from Eden's ShaderDir.
`save_sync.cpp` also derives NAND paths from Eden. A frontend-only replacement would split data
between roots and can make existing saves appear lost.

Shared logical content: `games` plus updates/DLC (or one external game directory), `eden/keys`,
`eden/nand` including saves and user profiles, `eden/load` mods/cheats and their enable state,
`eden/cache/shader` caches plus crash markers, library JSON/JPEG/eShop index, and the settings that select
the same emulated user. Sync state/credentials need an explicit account and locking design; do not
implicitly share `portal.json`, logs, crash dumps, `game.txt`, `game.url`, `move.txt`, or package-update
staging. Once full mode exists, `title.txt` must take precedence over all private remembered titles.

### Migration and zero-copy games

Only the main NXbox package can reliably read its old sandbox. Migration must be implemented there,
not in a newly installed tile. With all game sessions stopped, acquire a cross-package exclusive
lease; write a versioned journal and inventory; move large internal games by rename only after
proving same-volume AppContainer/broker permissions. If rename is unsupported, offer the existing
move-to-drive flow from the main app, with free-space checks. Do not quietly copy 15 GB or delete
the original after a partial move. External `NXbox/games` already provides one-copy game storage;
each full package must enumerate and authorize that drive independently. Drive letters can change.
A missing drive is an unavailable game, not permission to boot another title.

Copy and verify the small critical keys/profile/save data with a durable backup outside the cache;
never overwrite a divergent destination save. Rewrite cached absolute paths, retain external paths
only after revalidation, and rebuild library metadata if needed. Publish the migration-complete
marker last using atomic replacement. Test power loss at each journal step. Retain rollback and
backup until console verification succeeds. The old main version must not continue writing a
second copy after migration. Save/cloud upload, mod installs, shader writes and migration all need
cross-process exclusion, not the existing in-process mutexes alone.

Recommendation: use external `NXbox/games` as the first full-package game's storage option, but do
not limit or migrate the existing main app automatically. Use a shared root only after recovery and
backup are implemented for small mutable data. A drive-only game policy solves duplication of
ROMs, not identity, keys, profiles, saves, and cache coordination.

## What breaks under a second identity

- **Activation/lifecycle:** a separate package means a separate sandbox/process, not another view
  of the same NXbox singleton. The old process may be suspended while still owning files. A stale
  lease needs crash recovery; do not assume switching Home terminated it. The bundled title must
  be handled for normal Launch activation, independent of `game.nro` and protocol registration.
- **Drive broker:** retain removableStorage and independently enumerate KnownFolders.RemovableDevices.
  The Xbox WinRT/from-app fallback must be exercised under both identities; a path or token from
  the main package is not evidence of access by the other package. Read-only games can be shared;
  disappearing media during a save/move requires explicit failure handling.
- **Dumps/logs:** Device Portal retrieval is package-specific. Keep diagnostic and Eden logs private,
  associate dumps with AUMID, build revision and title, and preserve matching symbols per build.
- **OTA:** the current updater targets the main NXbox release/identity. A full tile cannot install
  that package as its own update or relaunch the main package and still claim to be the game.
  Initially disable OTA in full tiles and update through Device Portal; later publish per-identity
  monotonic versions from one trusted runtime build. Do not expose OTA prompts during game launch.
- **Signing:** use the existing stable NXbox signing secrets and exactly matching publisher. Never
  manufacture a new certificate per title. VCLibs must be installed. A tiny launcher and full app
  with the same identity would be an in-place upgrade: define migration, version ordering and rollback
  before switching modes. A failed full upgrade must not strand its shared data.
- **Data consistency:** NAND profile identity, SaveSync scratch names, shader crash markers, mod
  toggles, and cache formats were designed for one process. Two suspended/live copies and differently
  versioned runtimes can corrupt them without versioning and exclusion.

## Recommendation and effort

Ship and console-validate the splash path first. Full packaging is a medium-sized follow-up with
high data-loss risk if reduced to manifest edits. Planning estimate (not a delivery guarantee):
1–2 days for a disposable two-identity console storage/activation probe, 3–5 days for root routing,
locking/migration/recovery and tests, 1–2 days for trusted full packaging and identity-aware updates,
and 2–3 days for crash/uninstall/offline/USB/upgrade regression on console. Roughly 7–12 engineering
days, contingent on publisher storage and rename behavior. Stop the full approach if durable shared
saves cannot be demonstrated; retain the improved launcher path. Repackaging preserves emulation
and its performance limits even when dashboard ownership changes.

## Console acceptance checklist

Install CI builds of main NXbox and the tiny launcher; no local MSVC build was possible on macOS.
Terminate NXbox before each cold-launch test. Pull the main package's `LocalState/diagnostic.txt`.

1. Cached game/art, PT and EN: see art/name immediately, lookup → keys → shaders → starting,
   then the game. Expected order (existing memory/content lines may interleave):
   `PROTOCOL_SPLASH <ID>`, `PROTOCOL_PHASE lookup`, `PROTOCOL_LAUNCH <ID>`,
   `PROTOCOL_PHASE keys`, `UI renderer released`, `GAME_BEGIN`, `GAME_LOADING`,
   `PROTOCOL_PHASE shaders`, `SHADER_CACHE loading`, `SHADER_CACHE built N ...`,
   `SHADER_CACHE total N`, `SHADER_CACHE ready`, `PROTOCOL_PHASE starting`,
   `PROTOCOL_HANDOFF`, `GAME_RUNNING`, `GAME_FIRST_FRAME`, `GAME_PRESENT ...`.
   An empty/disabled cache can omit build lines or the entire shader phase. First-frame and running
   log ordering can race once emulation starts. Launcher debugger: `protocol accepted; exiting`.
2. Uncached art online: expect `UI art icon <ID> cached` and/or `UI art banner <ID> cached ...`;
   remove only cached art for this test. Offline with cached art must still show it; offline without
   art must still boot with name/ID and phase. Corrupt cached metadata must not block the game.
3. Missing title/drive/keys: expect `PROTOCOL_GAME_NOT_FOUND <ID>` or `GAME_LOAD_FAILED status=...`
   / `GAME_FAIL ...`, a localized error and B to exit. Never `UI library screen open`, a setup,
   update, USB or sync prompt, or `GAME_RUNNING` for a previously remembered title.
4. Seed `skip_library.txt`, `usb_scan_test.txt`, `move.txt`, `game.url`, and a pending sync marker
   with a different remembered game. The protocol must still launch only the requested title;
   maintenance files remain untouched. Verify local saves persist across these launches. Verify
   cloud reconciliation by opening NXbox normally; protocol sessions intentionally do not sync.
5. Large shader cache: counts advance without a second swap chain, black/transparent image, upside-down
   artwork or distorted first guest frame. Watch `MEM` commit, not just FPS. Close/Home during lookup
   and shaders; return/relaunch and inspect crash recovery. The dispatcher must remain responsive.
6. Ordinary NXbox launch still offers its existing library/setup/USB/SaveSync/update behavior. Test
   launcher URI failure with main NXbox absent; it must terminate, including the bounded timeout.

The highest-risk unverified parts are D2D readback support and the GL bootstrap/render-context swap
on the Xbox driver, SDK overload resolution, and console memory pressure. The source uses existing
Windows libraries, `.as<T>()`/COM `As`, explicit collection headers, and no new third-party dependency.

## Validation performed for this patch

- `python3 -m unittest discover -s tests/port -p test_package_launcher.py -v`: 9 passed,
  including black manifest backgrounds and the packaged splash actually using the game's banner.
- `python3 -m unittest discover -s tests/port -p test_package.py -v`: 8 passed.
- `git diff --check`: clean. C++ edits passed through clang-format; Python edits through ruff format.
  Unrelated existing formatting was retained, especially the translation table.
- No MSVC build, UWP execution, console deployment, commit, or push. Python staging tests are not
  evidence that the C++ compiles or that the presentation transition works on Xbox.

Concrete MSVC/console review locations (line numbers in this patch):

| Location | Risk / required check |
| --- | --- |
| `src/eden_uwp/ui/renderer.cpp:156` | New offscreen `CreateBitmap` branch: SDK overload/types and hardware target support. |
| `src/eden_uwp/ui/renderer.cpp:302` | `CopyFromBitmap` → CPU_READ bitmap → Map/Unmap readback; matching formats, row pitch, alpha and Xbox driver support. Initial capture happens before Present. |
| `src/eden_uwp/mesa_window.cpp:313` | Frontend blit into the existing GL surface: bootstrap/renderer ownership, complete framebuffer, BGRA orientation, restored state, transient memory and no extra swap chain. |
| `src/eden_uwp/game_session.cpp:545` | Shader callback captures / C++20 stop source; rendering is safe only because this frontend precompiles inline on the current context. Keep that invariant if parallel shader loading changes. |
| `src/eden_uwp/ui/launch_screen.cpp:142` | Renderer recreated on the worker after being released on the UI thread; apartment lifetime and release on every failure path. |
| `src/nxbox_launcher/main.cpp:61` | Standalone launcher now links D3D11/D2D/DXGI/WIC Windows libraries; check CoreWindow activation and PNG decoding with the UWP SDK. |
| `src/nxbox_launcher/main.cpp:168` | Async operation status polling and immediate CoreApplication exit after acceptance, cancellation timeout, unavailable protocol. |
