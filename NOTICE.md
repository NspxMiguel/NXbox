# NOTICE

NXbox is a fork of [Eden](https://git.eden-emu.dev/eden-emu/eden) and is distributed under the GNU
GPL version 3 or later; see [LICENSE.txt](LICENSE.txt). Existing source notices and the licenses of
third-party libraries are kept as they are. This file adds the credits for the parts of NXbox that
were inspired by, or fetch data from, other projects.

## Credits and third-party notices

### Cheats: nx-cheats-db, CNX Updater and the projects behind it

The **Cheats** screen of the mod store (Menu button) and the way it finds a game's cheats follow what
the Nintendo Switch homebrew updater CNX Updater offers. NXbox takes the same data from the same
place, and credits the people who built it.

- **nx-cheats-db** by **sthetix** (<https://github.com/sthetix/nx-cheats-db>) is the cheats
  database. NXbox does not ship it. It downloads `titles.zip` (with `contents.zip` as a fallback)
  from that project's latest GitHub release when the player opens the Cheats screen of a game, keeps
  a copy for 24 hours in the app's local storage, and writes only the cheats the player turns on
  into the game's `NXboxCheats` mod folder. The cheats in the database belong to their authors and
  to the nx-cheats-db project; NXbox claims no rights over them.
- **CNX Updater** by **CostelaCNX** (<https://github.com/CostelaCNX/CNX-Updater>, GPL-3.0) is the
  updater whose feed points at nx-cheats-db (`cheats_config` with `url_titles` and `url_contents`).
  NXbox uses the same two addresses. No CNX Updater source code is copied; the feature set and the
  data source are the inspiration.
- **AIO-Switch-Updater** by **HamletDuFromage** (<https://github.com/HamletDuFromage/aio-switch-updater>,
  GPL-3.0) is the project CNX Updater is a fork of.
- **Borealis** by **natinusala** (<https://github.com/natinusala/borealis>) is the UI library those
  updaters are built on. NXbox does not use it; it is credited because the updaters' feature set,
  which inspired the downloads and cheats of NXbox, comes from that line of work.

NXbox is an independent project. It is not affiliated with or endorsed by any of the projects above.

The credit line "Cheats database: nx-cheats-db by sthetix, via CNX Updater by CostelaCNX (GPL-3.0)"
is shown on the Cheats screen, and Settings > Credits lists the same names.

### Emulator and platform

- **Eden** (<https://git.eden-emu.dev/eden-emu/eden>), the emulator NXbox is forked from, and the
  **yuzu** and **Sudachi** projects it derives from.
- **juanresendiz813/eden-xbox** (<https://github.com/juanresendiz813/eden-xbox>), which provided the
  starting Xbox/UWP work.
- **aerisarn/mesa-uwp** (<https://github.com/aerisarn/mesa-uwp>), which provides Mesa for UWP.

NXbox ships no games, firmware or keys.
