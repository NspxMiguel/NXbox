// SPDX-FileCopyrightText: Copyright 2026 NXbox contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <switch.h>

// Avoid service initialization: this test exercises only guest instructions and
// SVCs.
void __appInit(void) {}
void __appExit(void) {}

// This fixture needs no guest heap service. Keep newlib's heap bounded instead
// of letting libnx request a gigabyte before the sentinel can be emitted.
void __libnx_initheap(void) {
  static unsigned char heap[64 * 1024];
  extern void *fake_heap_start;
  extern void *fake_heap_end;
  fake_heap_start = heap;
  fake_heap_end = heap + sizeof(heap);
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static const char sentinel[] = "EDEN_XBOX_JIT_ALIVE";
  volatile unsigned result = 0;
  for (unsigned i = 0; i < 100; ++i) {
    result += i;
  }
  if (result == 4950) {
    svcOutputDebugString(sentinel, sizeof(sentinel) - 1);
  }
  for (;;) {
    svcSleepThread(1000000000);
  }
}
