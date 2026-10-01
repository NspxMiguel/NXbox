<!--
SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
SPDX-License-Identifier: GPL-3.0-or-later
-->

# NXbox setup UI

NXbox currently boots straight into a game with no on-screen UI at all: `eden_uwp/uwp_boot.cpp` is
a headless `IFrameworkView` that picks the game from `LocalState\game.txt` and renders the emulator
directly into the `CoreWindow`. This document plans the setup screens a real player needs: where the
games and keys live, controller confirmation, and a library to pick from.

## Visual language

A reference UI already exists on this machine and console: **Nativra** (the sibling Xbox project,
its app is named `Kiosk` — `XboxDev/uwp/Kiosk/`). It uses a dark navy background (`#0D1012`), a cyan
accent (`#3EFFC0`), Archivo type, and flat 12px-radius bordered cards. NXbox should not look like a
reskin of it. Differences, deliberate:

| | Nativra/Kiosk | NXbox |
| --- | --- | --- |
| Background | navy `#0D1012` | true black `#000000` |
| Accent | cyan `#3EFFC0` | Nintendo Switch red `#E60012` (the one color; thematically the product's own brand, not picked for taste) |
| Depth | flat surface + 1px border | liquid glass on floating panels (game detail, settings sheet); plain 6% white hairline elsewhere |
| Hero background | fixed color | gradient extracted from the selected game's box art, three stops (0% / 48% / 72%) with a scrim, so every game's page feels different |
| Signature gesture | — | "the cartridge lift": selecting a tile lifts and tilts it slightly toward the viewer before the detail panel slides in, echoing a Switch cartridge being pulled — one gesture, used only there |

Everything else follows the house rules: 44px minimum touch target, pill shape on anything tappable,
one accent color used only for focus/action, lists as text rather than stacked cards where the data
is a list (game folder contents, key files found), empty states that say the next step instead of
the error, PT-BR and EN from the first screen, dark only (a TV has no "light mode" concept here, but
the token file still separates color from usage so this isn't hardcoded per-component).

## Game and key discovery: USB, not Device Portal

Development so far has pushed games and keys through the Device Portal (`console.py push`), which is
fine for us but not for a real player — Dev Mode's portal needs a PC and credentials. Nativra proved
a working, fast path already on this same hardware: a USB drive shows up as a drive letter
(`E:\Nativra\games` in Nativra's own layout), reached from a UWP app via
`KnownFolders.RemovableDevices` + `IStorageFolderHandleAccess::Create` (interface
`DF19938F-5462-48A0-BE65-D2A3271A08D6`) for a real Win32 `HANDLE`, not the ~220ms-per-file broker
path a plain file open would take. NXbox's setup screen scans an inserted USB drive for:

- `switch/prod.keys` and `switch/title.keys` (or `prod.keys`/`title.keys` at the drive root) — the
  common SD-backup layout Lockpick and similar tools already produce, so a player who dumped their
  own Switch already has the right file in the right place without doing anything NXbox-specific.
- `switch/games/*.nsp` (or a `nxbox/games/` folder) — copies (not moves) matching files into
  `LocalState\games\` so the emulator's existing `ResolveGamePath()` keeps working unchanged.

Device Portal push remains available underneath for development; the setup screen is what a player
without a PC needs.

## Screens (first layer — open it, no decisions)

1. **Welcome / scan.** Full black, centered: "Insert a USB drive with your games and keys." Live
   status once a drive appears: keys found (or not — link to "where do I get these"), N games found.
   No jargon; no keys/games found is not an error state, it explains the next physical action.
2. **Controller confirm.** "Press A." Once pressed, one extra choice with two options, not a form:
   **Nintendo layout** (confirm is the bottom face button, matching a real Switch controller) or
   **Xbox layout** (confirm is A, where Xbox players expect it) — the one place Switch and Xbox
   controller conventions genuinely conflict, so it is worth surfacing once, in plain terms, instead
   of silently picking one.
3. **Library.** A horizontal shelf of the games found (box art if the NSP's icon can be read, a
   generic cartridge tile otherwise). Selecting one does the cartridge-lift and opens the detail
   panel (glass, box-art gradient) with Play / Delete from console. Empty state (no games yet) sends
   the player back to step 1's instructions rather than showing a blank shelf.

## Screens (second layer — behind "More", on the library page)

- Per-game log level (for reporting bugs, not a normal setting).
- Motion controls: right-stick-as-motion toggle (see `docs/nxbox-port.md`'s motion-controls section).
- Network / Device Portal status (for us, not really for a player, but it's honest to show it).

## Architecture note

Kiosk is a **C# / .NET UWP app** (`Kiosk.csproj`, `App.xaml.cs`) built with MSBuild's XAML
toolchain. NXbox is **C++/WinRT**, built with CMake + Ninja (`CMakePresets.json`'s `uwp-x64`
preset) — a different stack. CMake has no first-class XAML compiler step, and bolting MSBuild's
XAML precompile (`Microsoft.UI.Xaml.Markup.Compiler`, the `.g.h`/`.g.cpp` codegen) onto a
Ninja-generated build is a real project of its own, not a side task here. Kiosk's XAML pages are a
visual/UX reference, not a code template.

Plan: draw the setup UI with **Direct2D + DirectWrite** directly onto the `CoreWindow`, the same way
`MesaWindow` already owns that window for the emulator — no XAML dependency at all, and the existing
`CoreWindow`/`CoreDispatcher` event loop in `uwp_boot.cpp` and `game_session.cpp` already does the
input and lifecycle plumbing this needs. The window's content swaps between the setup UI's Direct2D
render target and the emulator's Mesa/OpenGL swap chain when a game launches, rather than running
two windows. `game.txt`/`game.url` stay as the interface between the UI and `ResolveGamePath()` —
the setup screen picks a game, writes `LocalState\game.txt`, and either re-launches the boot path or
(later) calls into the existing game-session code directly.

First implementable, testable-without-any-rendering slice: the USB scanner itself
(`KnownFolders.RemovableDevices` + `IStorageFolderHandleAccess`), which can prove it finds
`switch/prod.keys`/`switch/games/*.nsp` on a real USB drive and copies them into `LocalState`,
independent of whatever draws the screen on top of it.


## Status (measured on Xbox)

Both foundations proven working, independent of each other, on real hardware:

- **USB discovery**: `usb_library.cpp`'s `ScanUsbForGamesAndKeys()` gets the `removableStorage`
  capability, enumerates the console's USB drive via `KnownFolders::RemovableDevices()`, and
  correctly reports no `switch/`, `nxbox/games/`, `prod.keys` or `title.keys` on Nativra's drive
  (there genuinely is none there) — no crash, no capability denial.
- **Rendering**: `setup_ui.cpp`'s `ShowSetupScreen()` stands up D3D11 + a D2D device/context + a
  DXGI swap chain on the same `CoreWindow` Mesa later takes over, and draws with DirectWrite —
  6 seconds of `BeginDraw`/`DrawText`/`EndDraw`/`Present1` with zero failures logged
  (`SETUP_UI_BEGIN` through `SETUP_UI_END`, no `SETUP_UI_FAILED`).

Not yet built: the actual screens from the plan above (only a placeholder title + one status line
renders right now), input/focus navigation on the setup screen, `ImportFromUsb()`'s copy path has
not been exercised against a drive that actually has files on it, and neither piece is wired into
the real boot flow yet (both are behind the `usb_scan_test.txt` marker file for testing only).

## Implementation plan (2026-10-01)

Approved look: a Library screen and a per-game Mods screen, previewed at 1920×1080 (true black, eShop art,
nav pills top-left (Library, Settings; mods are reached from a game, never from the nav), status top-right, glass hint bar at the bottom). The NXbox name is not shown on
screen. The one accent is the brand ribbon, a green→red gradient (`#2FD07A` → `#E8343E`). It is
used only as the 3 px focus ring around whatever has focus. The focused tile also lifts: scale 1.08,
−16 px, −1.2°, 220 ms with `cubic-bezier(0.2,0.8,0.2,1)`. Primary actions are white pills with
black text, secondary ones are glass pills. Numbers use a monospaced face. Text is Segoe UI.

Rendering stays Direct2D/DirectWrite/WIC on the CoreWindow, before Mesa takes the window (proven
on hardware by `setup_ui.cpp`: `SETUP_UI_BEGIN`/`END`). The UI owns a D3D11 flip swap chain and
releases it fully before the game boots.

### Increment 1: library and launch

- `src/eden_uwp/ui/` holds a small framework:
  - `renderer`: device, swap chain, D2D context, DWrite and WIC factories; text, images, rounded
    rects, gradients.
  - `input`: Windows.Gaming.Input polling with edge detection and repeat for D-pad and stick; A, B,
    X, Y, LB, RB and View.
  - `anim`: time-based easing.
  - `strings`: pt-BR and en. The system language decides the default; `NXBOX_LANG=pt|en` in
    `nxbox_env.txt` forces one.
- Library data:
  - Scan `LocalState\games` for `.nsp` and `.xci`.
  - Read each game's title ID, name and the Switch's own square icon from its control NCA:
    `control.nacp` plus `icon_AmericanEnglish.dat`, via `FileSys::NSP` / `FileSys::XCI`,
    `ExtractRomFS` and `NACP`.
  - Cache the result in `LocalState\library\<titleid>.json` and `.jpg`, so later launches never
    reparse the files.
- Hero:
  - Offline: a three-stop gradient extracted from the icon (0% / 48% / 72%) with a scrim, and the
    icon itself.
  - Increment 3 adds the eShop banner.
- Selecting a game and pressing A boots it: the path is handed to the existing boot path in
  `RunGameView`. With no games, the screen explains how to add one (USB `switch/games`, or a
  source in Settings).
- Every step writes a `UI ...` line through `Diagnostic()`.

### Increment 2: mod store (GameBanana)

- Find the game: `apiv11/Util/Search/Results?_sModelName=Game&_sSearchString=<name>`, preferring
  the "(Switch)" entry.
- List its mods: `apiv11/Mod/Index?_aFilters[Generic_Game]=<id>&_sSort=Generic_MostDownloaded`.
- Download a mod's files: `apiv11/Mod/<id>?_csvProperties=_aFiles`.
- Install into `LocalState\eden\load\<TITLEID>\<mod name>\`, normalizing the archive to its
  `romfs` / `exefs` / `cheats` folders. zip first, then 7z.
- Enable and disable through Eden's disabled add-ons list.

### Increment 3: sources and art

- Settings → Sources (like browser extensions), one list:
  - built-in GameBanana for mods;
  - built-in titledb for eShop art by title ID (icon, banner);
  - user-added sources in the Tinfoil shop format (`{"files":[{"url","size"}],"directories":[...]}`),
    which download games, updates and DLC into `LocalState\games`.
- No source of games ships configured; the user adds their own.

Focus model (owner, 2026-10-01): three vertical layers, as on the Xbox dashboard.
1. The game rail is the default focus, and A on a tile launches that game.
2. Up from the rail moves focus to the hero actions (Jogar, Mods, Detalhes) of the focused game.
3. Up again moves focus to the top nav (Biblioteca, Configurações).

Down goes back one layer. X (Mods) and Y (Detalhes) also work directly from the rail.

### Increment 4: SwitchSaveSync (requested 2026-10-01)

The first-run setup asks "Sincronizar saves com o SwitchSaveSync?". The same toggle lives in
Settings. The goal is the same cloud layout the Switch homebrew uses, so a real Switch and NXbox
share one save:

- Google Drive through the OAuth device flow: the TV shows the code, the phone signs in. The
  scope is `drive.file`, which only shows files created by the same OAuth client, so NXbox must
  use SwitchSaveSync's client ID. WebDAV comes later.
- Layout: `Nintendo Switch Saves/<game folder>/<account folder>/`, loose files, same names as
  SwitchSaveSync (`core/drive.c`, `core/syncjob.h` in the SwitchSaveSync repo).
- Eden saves live in `nand/user/save/0000000000000000/<user>/<TITLEID>/`.
- Download before the game boots (with a progress bar) and upload after it closes.
- Conflicts use SwitchSaveSync's fingerprint rule: if both sides changed, ask; never overwrite
  silently.

### Increment 1 status (2026-10-01)

Written but not yet compiled on Windows or run: CI and the console are the first to see this code.
It was syntax-checked against stand-in headers and its logic (focus layers, drawing, the `.nsz`
flow) was run on a host with fakes.

- Code: `src/eden_uwp/ui/` (`renderer`, `input`, `anim`, `theme`, `strings`, `library`,
  `library_screen`). The entry point is `Ui::RunLibrary(window)`; `RunGameView` calls it before
  Mesa takes the window. It returns the chosen game's path, which `RunGameView` writes to
  `LocalState\game.txt`, or an empty string, which keeps the old boot (`game.txt`, then the
  bundled homebrew). Every step logs a `UI ...` line.
- Look: the approved 1920x1080 preview, with the icon-derived gradient standing in for the eShop
  banner. The nav is one dark glass capsule with the selected tab as a white pill; the hero
  (gradient and scrim) fills the top 760 px; the rail sits on black below it.
- Navigation, as the owner changed it after the plan above:
  - The nav has two tabs only, Library and Settings (LB/RB switch). Mods always belong to a game,
    so they are reached from the game's hero.
  - Focus moves through three layers: the game rail (default; A launches), the hero pills of the
    focused game (Play, Mods, Details), then the nav. Up goes one layer up, Down one down. The
    ribbon ring is drawn only on the focused element of the active layer. X (Mods) and Y
    (Details) work from the rail too.
  - The focused tile always stands at the left margin and the rail scrolls under it, so the name
    under the tile never meets the hint bar.
  - Mods only shows a notice until Increment 2. Details is a sheet with the package's title ID,
    format, size and file. A on the "add games" tile scans the folder again.
- `.nsz` packages: the scan converts each `.nsz` in `LocalState\games` to a `.nsp` next to it
  (`FileSys::ConvertNszToNsp`, `core/file_sys/nsz.h`) before reading any metadata, and the screen
  shows the game's name, the percentage and a progress bar meanwhile. The converter writes
  `<name>.nsp.partial`, renamed when complete, and the `.nsz` is deleted only after that. A failed
  or cancelled (B) conversion removes the partial file and keeps the `.nsz`; an `.nsz` that already
  has a `.nsp` is left alone. Log lines: `UI nsz convert <file> ok|fail <error>|skip ...`.
- Beyond the plan:
  - `LocalState\skip_library.txt` skips the screen, so unattended runs that boot from `game.txt`
    do not wait for A.
  - A game chosen in the library ignores `game.url`, which belongs to whatever `game.txt` named
    before: downloading it over the chosen file would replace the player's game.
  - The empty state says what works today (copy files with Device Portal, press A to scan); the USB
    import and the sources of Increment 3 are not wired to the screen yet. When the folder has
    games but none could be opened for want of keys, it says that instead of "no games yet".
- To check on the console first: that Mesa gets the window after the library's swap chain is
  released (`UI renderer released`, then the normal `GAME_*` lines); that the icons decode and the
  hero gradients look right; that Segoe UI, the monospaced face and the CJK fallback exist.

### Hero banner status (2026-10-01)

Written, syntax-checked against stand-in headers, not yet compiled on Windows or run on the console.

- The hero shows the official eShop banner full bleed instead of the icon gradient. The index is
  `dist/art/eshop-art.json` on `main` (`{"base","ext","titles":{"<TITLEID16>":["<banner>","<icon>"]}}`);
  `ui/art.cpp` downloads it once into `LocalState\library\eshop-art.json` and refreshes it weekly (a
  failed refresh keeps the old copy). Each banner (1920x1080 JPEG) is cached in
  `LocalState\library\<TITLEID>.banner.jpg`, so a banner seen once also works offline. One worker
  thread does all of it; the newest request is served first.
- The banner covers the top 760 px with object-fit cover at 50% / 18%, under the preview's scrim
  (0.5 at 0%, 0 at 16% and 52%, 0.72 at 76%, black at 98%). It fades in over 320 ms when it arrives
  and crossfades with the previous game's banner. With a banner there is no big title (the banner
  carries the logo) and no icon on the right; both fade out as the banner fades in. The meta line and
  the pills stay where they were.
- Fallback: offline, no banner for the title, or still downloading, the icon gradient, title and icon
  show as before. At most three decoded banners stay on the GPU (8 MB each).
- Log lines: `UI art index ...`, `UI art banner <id> cached`, `UI banner shown <id>`.

### Increment 2 status: the mod store (2026-10-01)

Written, syntax-checked against stand-in headers, not yet compiled on Windows or run on the console.
The zip reader, the archive search and the HTML clean-up were also run on a host against a real zip.

- Code: `ui/mods.cpp` (GameBanana client, installer, zip reader), `ui/mods_screen.cpp` (the screen),
  `ui/widgets.cpp` (the ring, pills, hint bar, tabs and sheets that the library and the new screens
  share; they moved out of `library_screen.cpp` unchanged). It opens from the Mods pill or X, on a
  game, and returns with B.
- The game is found with `Util/Search/Results`, preferring the "(Switch)" record, and its id is cached
  in `LocalState\library\<TITLEID>.mods.json`, together with the installed mods and the disabled
  ones. All networking runs on worker threads (a search/list thread, a thumbnail thread, an install
  thread); thumbnails are decoded on the render thread, two per frame.
- Chips: Mais baixados (GameBanana's own order), Gráficos, Interface, Jogabilidade, Instalados with
  its count. LB/RB move between them. GameBanana's categories differ from game to game, so the three
  middle chips filter what is loaded by keyword on the root category (prefix match on its words), and
  the screen keeps loading pages while such a chip shows fewer than five rows (at most eight pages).
  Instalados comes from the record file, so it works offline.
- Install: the mod's files come from `Mod/<id>?_csvProperties=_sName,_nDownloadCount,_aFiles,_sText`.
  The first `.zip` is downloaded (progress on the row: "Baixando 64%"), unpacked into
  `LocalState\mods_tmp\<id>\x` (stored and deflate entries, CRC checked, zip64 and encrypted archives
  refused, `..` paths refused), and the shallowest folder holding `romfs`, `exefs` or `cheats` (up to
  six levels down, so `atmosphere\contents\<titleid>\` works) is moved to
  `LocalState\eden\load\<TITLEID16>\<mod name>\`. An archive with no such folder fails with a log
  line. A mod that only has `.7z` or `.rar` files shows "Formato ainda não suportado" on its row.
- On/off: X toggles an installed mod. Eden's disabled add-ons list (`Settings::values.disabled_addons`,
  keyed by title ID and folder name) is not reachable from `eden_settings.txt`, so the disabled names
  are kept in the record file and `Ui::ApplyDisabledMods` puts them in that list at boot (called in
  `RunGame` after the settings file). Nothing is renamed.
- Y opens the details sheet (author, category, count, first file with its size, description as plain
  text, 700 characters at most).
- Left out: `.7z` and `.rar`; a download resumes nothing (a cancelled one starts over); the count on
  the rows is likes when the listing carries no download count (the sheet shows downloads once the
  details arrive); no "Feito pro Xbox" badge (no field says it).
- Log lines: `UI mods ...`.

### Increment 4 status: SwitchSaveSync (2026-10-01)

Written, syntax-checked against stand-in headers, not yet compiled on Windows or run on the console.
The backend is `save_sync.h/.cpp`; `ui/savesync_ui.cpp` is the part the player sees.

- First run: when `LocalState\setup_done.txt` is missing, `RunLibrary` asks "Sincronizar saves com o
  SwitchSaveSync?" (Sim / Agora não) before the library shows, and writes `setup_done.txt` at the
  end (not when the window was closed meanwhile). Sim opens the sign-in screen; without
  `savesync.json` it explains that the OAuth client file is missing and goes on.
- Sign-in: `StartDeviceLogin()` then `PollDeviceLogin()` on a worker thread; the screen shows the
  verification URL, the user code large, a countdown to its expiry and the result. B cancels. No QR
  code (it would need an encoder; the code and the URL are enough).
- Settings tab: a list with "SwitchSaveSync" (Desligado / Conectado / Não configurado; A signs in
  or out) and "Fontes" (a placeholder that says "em breve"; Increment 3 fills it). Sync is enabled
  exactly when the account is configured and signed in.
- Before boot (only for a game chosen in the library): after the library returns, with sync enabled,
  `RunBootSync` reconciles the save behind a progress screen. On a conflict it shows both sides (time,
  files, size) and asks "Manter o save do Xbox" / "Usar o da nuvem" (focus starts on the newer side;
  B plays without syncing). The name given to SwitchSaveSync is `GameNameFromNames` over the NACP
  names, which the library cache now stores (`nacp_names` in `<TITLEID>.json`; an older record is
  rebuilt on the next scan). `eden_settings.txt` is applied first so the active profile is right.
- After the game: `SyncAfterExit` runs on the game's worker thread right after `RunGame` returns,
  which is after the guest stopped and `ShutdownMainProcess` released the save files. No screen
  (nothing owns the window then). Home suspends the app and the system may end it, and the window
  can be closed, so there is no safe point in those paths: `savesync_pending.txt` (title ID and name)
  is written before the boot and removed once the upload succeeded, and a leftover one is finished
  at the next launch, behind a progress screen, before the library shows.
- A game launched through `game.txt` with `skip_library.txt` has no title ID known to the UI and is
  not synced.
- Log lines: `UI setup ...`, `UI savesync ...`.

### Over-the-air updates (2026-10-01)

The library checks the latest GitHub release on an MTA worker. A newer package
adds an update pill to the right of the top navigation; move right from Settings
to focus it. Only A on that pill begins the download and Device Portal install.
A modal shows download percentage, installation/restart status, or a retryable
error. The worker cannot launch an install from the background check, and the
modal blocks game launches while a deployment is pending. PT and EN strings live
in the existing table. See [NXbox updates](nxbox-updates.md) for the release/tag
contract, portal.json schema, CSRF protocol and real-console validation limits.
