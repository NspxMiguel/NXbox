// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string_view>

namespace Common::NativeClockPolicy {

constexpr bool Enabled(const char* override_value) {
    return override_value == nullptr || std::string_view{override_value} != "0";
}

constexpr bool ValidFrequency(std::uint64_t frequency) {
    return frequency > 1'000'000'000 && frequency <= 10'000'000'000;
}

// Use measured elapsed time, not the requested sleep duration or a wall-time clock.
inline std::uint64_t SampleFrequency(std::uint64_t ticks, std::int64_t elapsed_ns) {
    if (elapsed_ns < 50'000'000 || elapsed_ns > 5'000'000'000) {
        return 0;
    }
    const long double frequency = static_cast<long double>(ticks) * 1'000'000'000 / elapsed_ns;
    if (frequency <= 1'000'000'000 || frequency > 10'000'000'000) {
        return 0;
    }
    return static_cast<std::uint64_t>(frequency);
}

constexpr std::uint64_t StableFrequency(std::uint64_t first, std::uint64_t second) {
    if (!ValidFrequency(first) || !ValidFrequency(second)) {
        return 0;
    }
    const auto difference = first > second ? first - second : second - first;
    if (difference > first / 200) {
        return 0;
    }
    return ((first + second) / 2 + 50'000) / 100'000 * 100'000;
}

template <typename Estimate>
std::uint64_t ResolveFrequency(bool invariant, std::uint64_t reported, bool enabled,
                               Estimate estimate) {
    if (!enabled || !invariant || reported > 1'000'000'000) {
        return reported;
    }
    const auto measured = estimate();
    return ValidFrequency(measured) ? measured : reported;
}

} // namespace Common::NativeClockPolicy
