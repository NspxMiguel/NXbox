# Paddle guest test

Original, unencrypted Nintendo Switch homebrew for the NXbox validation path. No firmware, keys, or
commercial content is bundled. This is a test game, not proof of commercial-game compatibility or
performance.

Build with devkitA64 and libnx:

```sh
make -C homebrew/paddle-test
```

The output is `build/paddle.nro`. Press A to serve, move the left paddle with the D-pad or left
stick, and press Plus to exit. Each point waits for a new serve. The right paddle is
computer-controlled. Score marks appear along the top.

Guest debug messages confirm startup, A-button input and batches of 300 submitted frames.
Guest-clock timing is diagnostic only: acceptance measurements must use the host's actual completed
guest frames per wall-clock second, not this timer or the Xbox display refresh rate.

The build links libnx; retain its [license](../jit-smoke/LICENSE.libnx.md) when distributing the
binary. Source code is GPL-3.0-or-later.
