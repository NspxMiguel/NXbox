// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace EdenXbox::Ui {

using UpdateVersion = std::array<std::uint16_t, 4>;

// The release workflow uses the same stable tag grammar. Compare all four package components.
inline std::optional<UpdateVersion> ParseUpdateVersion(std::wstring_view tag) {
    if (tag.empty() || tag.front() != L'v') {
        return std::nullopt;
    }
    tag.remove_prefix(1);
    UpdateVersion version{};
    for (std::size_t i = 0; i < version.size(); ++i) {
        const auto end = tag.find(L'.');
        const auto part = tag.substr(0, end);
        if (part.empty()) {
            return std::nullopt;
        }
        std::uint32_t number = 0;
        for (const wchar_t digit : part) {
            if (digit < L'0' || digit > L'9') {
                return std::nullopt;
            }
            number = number * 10 + static_cast<std::uint32_t>(digit - L'0');
            if (number > 65535) {
                return std::nullopt;
            }
        }
        version[i] = static_cast<std::uint16_t>(number);
        if (end == std::wstring_view::npos) {
            return i >= 2 && version[0] >= 1 ? std::optional{version} : std::nullopt;
        }
        tag.remove_prefix(end + 1);
    }
    return std::nullopt;
}

} // namespace EdenXbox::Ui
