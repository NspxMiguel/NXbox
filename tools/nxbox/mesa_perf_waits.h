/* SPDX-License-Identifier: MIT */
#pragma once

#include <atomic>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static inline bool nxbox_async_submit_enabled() {
  static const bool enabled = [] {
    const char *value = getenv("NXBOX_ASYNC_SUBMIT");
    const char *profile = getenv("NXBOX_GPU_PROFILE");
    // The profiler reads timestamps immediately after Execute; preserve its
    // completion proof.
    return !(value && value[0] == '0') && !(profile && profile[0] == '1');
  }();
  return enabled;
}

static inline bool nxbox_so_no_wait_enabled() {
  static const bool enabled = [] {
    const char *value = getenv("NXBOX_SO_NO_WAIT");
    return !(value && value[0] == '0');
  }();
  return enabled;
}

enum class NxboxPerfWait { AsyncSubmit, SubmitDrain, SoEnable, SoDisable };

inline void nxbox_count_perf_wait(NxboxPerfWait kind) {
  // One set of counters across translation units and contexts. Report the first
  // 16 events, then every 64, to keep environment publication out of most
  // hot-path submissions.
  static std::atomic<uint64_t> counts[4]{};
  const auto count = counts[static_cast<unsigned>(kind)].fetch_add(
                         1, std::memory_order_relaxed) +
                     1;
  if (count > 16 && count % 64)
    return;
  char text[256];
  snprintf(text, sizeof(text),
           "async_submit=%llu submit_drain=%llu so_enable=%llu so_disable=%llu",
           (unsigned long long)counts[0].load(std::memory_order_relaxed),
           (unsigned long long)counts[1].load(std::memory_order_relaxed),
           (unsigned long long)counts[2].load(std::memory_order_relaxed),
           (unsigned long long)counts[3].load(std::memory_order_relaxed));
  SetEnvironmentVariableA("NXBOX_D3D12_PERF_WAITS", text);
}
