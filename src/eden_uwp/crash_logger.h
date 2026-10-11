// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string_view>

namespace EdenXbox {
void InstallCrashHandlers() noexcept;
void OpenCrashLog();
void ObserveCrashLifecycle();
void CrashEvent(std::string_view event, std::uint64_t usage = 0, std::uint64_t limit = 0,
                std::uint64_t next_limit = 0) noexcept;
void CrashGpuError(const char* stage, std::int32_t result, std::int32_t removed) noexcept;
} // namespace EdenXbox
