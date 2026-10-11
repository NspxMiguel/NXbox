// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace Service::Account::Offline {

constexpr bool FakeLinkEnabled(const char* value, bool default_enabled) {
    return value ? std::string_view{value} != "0" : default_enabled;
}

inline bool IsFakeLinkEnabled() {
#ifdef NXBOX_UWP
    constexpr bool default_enabled = true;
#else
    constexpr bool default_enabled = false;
#endif
    return FakeLinkEnabled(std::getenv("NXBOX_FAKE_NA_LINK"), default_enabled);
}

// ProfileManager loads valid profiles contiguously. Always write the resolved index back so
// account services, the profile applet and save sync address the same UUID.
constexpr std::size_t SelectProfileIndex(int requested, std::size_t count) {
    const auto index = static_cast<std::size_t>(std::clamp(requested, 0, 7));
    return index < count ? index : 0;
}

constexpr std::uint64_t AccountId(std::uint64_t hash) {
    return hash ? hash : 1;
}

} // namespace Service::Account::Offline
