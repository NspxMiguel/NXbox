// SPDX-License-Identifier: GPL-3.0-or-later
// Original, unencrypted guest game for display, timing and controller
// validation.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

// Keep this diagnostic independent of the guest loader's maximum heap
// allowance.
size_t __nx_heap_size = 128 * 1024 * 1024;

enum {
  WIDTH = 640,
  HEIGHT = 360,
  PADDLE_HEIGHT = 64,
  PADDLE_WIDTH = 8,
  BALL_SIZE = 8
};
static const u32 BACKGROUND = RGBA8_MAXALPHA(0, 0, 0);
static const u32 FOREGROUND = RGBA8_MAXALPHA(245, 245, 245);
static const u32 ACCENT = RGBA8_MAXALPHA(78, 208, 127);
static const u32 DIVIDER = RGBA8_MAXALPHA(36, 36, 36);

static float clamp(float value, float low, float high) {
  return fminf(high, fmaxf(low, value));
}

static void rectangle(u32 *pixels, u32 pitch, int x, int y, int width,
                      int height, u32 color) {
  for (int row = y < 0 ? 0 : y; row < y + height && row < HEIGHT; ++row) {
    for (int column = x < 0 ? 0 : x; column < x + width && column < WIDTH;
         ++column) {
      pixels[row * pitch + column] = color;
    }
  }
}

int main(void) {
  Framebuffer framebuffer;
  Result result = framebufferCreate(&framebuffer, nwindowGetDefault(), WIDTH,
                                    HEIGHT, PIXEL_FORMAT_RGBA_8888, 2);
  if (R_FAILED(result)) {
    const char *error = "NXBOX_PADDLE_FRAMEBUFFER_FAILED";
    svcOutputDebugString(error, strlen(error));
    return 1;
  }
  result = framebufferMakeLinear(&framebuffer);
  if (R_FAILED(result)) {
    framebufferClose(&framebuffer);
    return 2;
  }
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);
  float player_y = (HEIGHT - PADDLE_HEIGHT) / 2.0f;
  float opponent_y = player_y;
  float ball_x = WIDTH / 2.0f, ball_y = HEIGHT / 2.0f;
  float velocity_x = -220.0f, velocity_y = 95.0f;
  bool playing = false;
  unsigned player_score = 0, opponent_score = 0, frames = 0;
  u64 previous = armGetSystemTick(), sample_start = previous;
  const char *ready =
      "NXBOX_PADDLE_READY: A starts; D-pad or left stick moves; Plus exits";
  svcOutputDebugString(ready, strlen(ready));

  while (appletMainLoop()) {
    const u64 now = armGetSystemTick();
    const float delta =
        clamp((float)armTicksToNs(now - previous) / 1.0e9f, 0.0f, 1.0f / 15.0f);
    previous = now;
    padUpdate(&pad);
    const u64 pressed = padGetButtonsDown(&pad);
    const u64 held = padGetButtons(&pad);
    if (pressed & HidNpadButton_Plus) {
      break;
    }
    if (pressed & HidNpadButton_A) {
      playing = true;
      const char *input = "NXBOX_PADDLE_INPUT_A";
      svcOutputDebugString(input, strlen(input));
    }
    const HidAnalogStickState stick = padGetStickPos(&pad, 0);
    float movement = -(float)stick.y / 32767.0f;
    if (held & HidNpadButton_Up) {
      movement = -1.0f;
    } else if (held & HidNpadButton_Down) {
      movement = 1.0f;
    }
    if (fabsf(movement) < 0.15f) {
      movement = 0.0f;
    }
    player_y = clamp(player_y + movement * 300.0f * delta, 0.0f,
                     HEIGHT - PADDLE_HEIGHT);
    if (playing) {
      const float distance = ball_y - (opponent_y + PADDLE_HEIGHT / 2.0f);
      opponent_y =
          clamp(opponent_y + clamp(distance, -165.0f * delta, 165.0f * delta),
                0.0f, HEIGHT - PADDLE_HEIGHT);
      ball_x += velocity_x * delta;
      ball_y += velocity_y * delta;
      if (ball_y < 0 || ball_y > HEIGHT - BALL_SIZE) {
        ball_y = clamp(ball_y, 0.0f, HEIGHT - BALL_SIZE);
        velocity_y = -velocity_y;
      }
      if (velocity_x < 0 && ball_x <= 32 && ball_x >= 24 - BALL_SIZE &&
          ball_y + BALL_SIZE >= player_y &&
          ball_y <= player_y + PADDLE_HEIGHT) {
        ball_x = 32;
        velocity_x = fminf(-velocity_x * 1.04f, 480.0f);
        velocity_y = (ball_y - player_y - PADDLE_HEIGHT / 2.0f) * 6.0f;
      }
      if (velocity_x > 0 && ball_x + BALL_SIZE >= WIDTH - 32 &&
          ball_x <= WIDTH - 24 && ball_y + BALL_SIZE >= opponent_y &&
          ball_y <= opponent_y + PADDLE_HEIGHT) {
        ball_x = WIDTH - 32 - BALL_SIZE;
        velocity_x = -fminf(velocity_x * 1.04f, 480.0f);
      }
      if (ball_x < -BALL_SIZE || ball_x > WIDTH) {
        if (ball_x < 0) {
          ++opponent_score;
        } else {
          ++player_score;
        }
        ball_x = WIDTH / 2.0f;
        ball_y = HEIGHT / 2.0f;
        velocity_x = -220.0f;
        velocity_y = 95.0f;
        playing = false;
      }
    }

    u32 stride = 0;
    u32 *pixels = framebufferBegin(&framebuffer, &stride);
    const u32 pitch = stride / sizeof(u32);
    for (unsigned row = 0; row < HEIGHT; ++row) {
      for (unsigned column = 0; column < WIDTH; ++column) {
        pixels[row * pitch + column] = BACKGROUND;
      }
    }
    for (int y = 12; y < HEIGHT; y += 20) {
      rectangle(pixels, pitch, WIDTH / 2 - 1, y, 2, 10, DIVIDER);
    }
    rectangle(pixels, pitch, 24, (int)player_y, PADDLE_WIDTH, PADDLE_HEIGHT,
              ACCENT);
    rectangle(pixels, pitch, WIDTH - 32, (int)opponent_y, PADDLE_WIDTH,
              PADDLE_HEIGHT, FOREGROUND);
    rectangle(pixels, pitch, (int)ball_x, (int)ball_y, BALL_SIZE, BALL_SIZE,
              playing ? FOREGROUND : ACCENT);
    for (unsigned i = 0; i < player_score % 10; ++i) {
      rectangle(pixels, pitch, WIDTH / 2 - 24 - (int)i * 12, 16, 6, 12, ACCENT);
    }
    for (unsigned i = 0; i < opponent_score % 10; ++i) {
      rectangle(pixels, pitch, WIDTH / 2 + 18 + (int)i * 12, 16, 6, 12,
                FOREGROUND);
    }
    framebufferEnd(&framebuffer);
    if (++frames == 300) {
      const double seconds =
          (double)armTicksToNs(armGetSystemTick() - sample_start) / 1.0e9;
      char message[128];
      const int size = snprintf(
          message, sizeof(message),
          "NXBOX_PADDLE_GUEST frames=300 guest_seconds=%.3f score=%u:%u",
          seconds, player_score, opponent_score);
      svcOutputDebugString(message, (u64)size);
      frames = 0;
      sample_start = armGetSystemTick();
    }
  }
  framebufferClose(&framebuffer);
  return 0;
}
