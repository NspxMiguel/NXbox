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
