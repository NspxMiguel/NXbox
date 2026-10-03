# Recompiling games instead of emulating them

The request: a tool that turns a Switch game into a native Xbox program, so it shows up as a game and
runs without the emulator in between. This note records what is feasible and what NXbox does about it.

## What "recompilation" means here

Static recompilation (N64Recomp, XenonRecomp and similar projects) translates a game's machine code to C++
ahead of time and builds that C++ for the target. It removes the CPU emulator. It does not remove the
rest: a Switch game also calls the Horizon OS (kernel SVCs and about 100 system services) and the NVN
graphics API with Maxwell shaders. A recompiled Switch game still needs all of that provided natively.
Those projects work game by game, with hand-written replacements for the system and graphics layers.

On the Xbox there is a second limit: a UWP app cannot generate and run code except through the JIT
route NXbox already uses, so shipping the game's code as ordinary compiled C++ is the only static path.

## What is realistic

1. **Per-game dashboard tiles (done).** `package_launcher.py` and `package-game-tile.yml` make a package per game
   with its own name and art. Opening it starts NXbox directly in that game (`nxbox://play?title=...`),
   so the game appears on the dashboard as a game.
2. **Persistent translation cache (planned, task 83).** Keep each game's translated code between runs, the
   closest generic equivalent of recompiling: no CPU translation work after the first run, no emulator
   warm-up stalls. The cost today is JIT time at scene loads (see `jit-stall-investigation.md`).
3. **A real recompiler (a project of its own).** AArch64 to C++ for one game, with NXbox's HLE services and the
   OpenGL translation reused as the runtime. It is worth starting only for a game that is already stable under
   the emulator, because the emulator is the reference to compare against. Not started.
