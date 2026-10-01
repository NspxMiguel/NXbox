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
