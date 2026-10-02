// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace EdenXbox {

// Parses an "nxbox://play?title=<16 hex digits>" URI (what a per-game launcher tile sends) and
// returns the title ID in upper case, or an empty string when the URI is anything else. Pure and
// standard-library only, so it is unit-tested on every host (tests/port/protocol_uri.cpp).
inline std::string ParsePlayTitleUri(std::string_view uri) {
    const auto lower = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    constexpr std::string_view prefix = "nxbox://play";
    if (uri.size() < prefix.size()) {
        return {};
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (lower(uri[i]) != prefix[i]) {
            return {};
        }
    }
    uri.remove_prefix(prefix.size());
    // Windows may normalize the URI to "nxbox://play/?title=...".
    if (!uri.empty() && uri.front() == '/') {
        uri.remove_prefix(1);
    }
    if (uri.empty() || uri.front() != '?') {
        return {};
    }
    uri.remove_prefix(1);
    if (const auto fragment = uri.find('#'); fragment != std::string_view::npos) {
        uri = uri.substr(0, fragment);
    }
    while (!uri.empty()) {
        const auto amp = uri.find('&');
        const std::string_view pair = uri.substr(0, amp);
        uri = amp == std::string_view::npos ? std::string_view{} : uri.substr(amp + 1);
        constexpr std::string_view key = "title=";
        if (pair.size() != key.size() + 16) {
            continue;
        }
        bool key_matches = true;
        for (std::size_t i = 0; i < key.size(); ++i) {
            key_matches = key_matches && lower(pair[i]) == key[i];
        }
        if (!key_matches) {
            continue;
        }
        std::string id(pair.substr(key.size()));
        for (char& c : id) {
            if (!std::isxdigit(static_cast<unsigned char>(c))) {
                return {};
            }
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return id;
    }
    return {};
}

} // namespace EdenXbox
