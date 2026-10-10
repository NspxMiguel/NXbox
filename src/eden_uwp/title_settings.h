// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace EdenXbox::TitleSettings {

// The numeric values of Eden's ResolutionSetup, not the multipliers themselves.
inline constexpr std::array<int, 4> kResolutionValues{3, 5, 6, 7};
inline constexpr int kDefaultResolution = kResolutionValues[0];

struct Entry {
    std::string label;
    std::string value;
};

inline std::string_view Trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

inline std::optional<Entry> ParseLine(std::string_view line) {
    line = Trim(line);
    const auto equals = line.find('=');
    if (line.empty() || line.front() == '#' || equals == std::string_view::npos) {
        return std::nullopt;
    }
    const auto label = Trim(line.substr(0, equals));
    if (label.empty()) {
        return std::nullopt;
    }
    return Entry{std::string(label), std::string(Trim(line.substr(equals + 1)))};
}

inline std::optional<int> ParseResolution(std::string_view value) {
    value = Trim(value);
    // ResolutionSetup currently has thirteen values (0..12).
    int result = 0;
    if (value.empty() || value.size() > 2) {
        return std::nullopt;
    }
    for (const char digit : value) {
        if (digit < '0' || digit > '9') {
            return std::nullopt;
        }
        result = result * 10 + digit - '0';
    }
    return result <= 12 ? std::optional<int>(result) : std::nullopt;
}

inline int ReadResolution(std::istream& in, int fallback = kDefaultResolution) {
    std::string line;
    while (std::getline(in, line)) {
        const auto entry = ParseLine(line);
        if (entry && entry->label == "resolution_setup") {
            if (const auto value = ParseResolution(entry->value)) {
                fallback = *value;
            }
        }
    }
    return fallback;
}

inline std::filesystem::path File(const std::filesystem::path& local, std::string_view title_id) {
    if (title_id.size() != 16) {
        return {};
    }
    std::string normalized(title_id);
    for (char& digit : normalized) {
        if (digit >= 'a' && digit <= 'f') {
            digit -= 'a' - 'A';
        }
        if (!((digit >= '0' && digit <= '9') || (digit >= 'A' && digit <= 'F'))) {
            return {};
        }
    }
    return local / "settings" / (normalized + ".txt");
}

// Replace all resolution entries while retaining comments and other settings verbatim.
inline std::string WithResolution(const std::string& content, int value) {
    std::istringstream in(content);
    std::string result;
    std::string line;
    while (std::getline(in, line)) {
        const auto entry = ParseLine(line);
        if (!entry || entry->label != "resolution_setup") {
            result += line + '\n';
        }
    }
    return result + "resolution_setup=" + std::to_string(value) + '\n';
}

inline bool SaveResolution(const std::filesystem::path& file, int value) {
    if (file.empty()) {
        return false;
    }
    bool supported = false;
    for (const int choice : kResolutionValues) {
        supported |= choice == value;
    }
    if (!supported) {
        return false;
    }
    std::error_code error;
    const bool exists = std::filesystem::exists(file, error);
    if (error) {
        return false;
    }
    std::string content;
    if (exists) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            return false;
        }
        content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (in.bad()) {
            return false;
        }
    }
    std::filesystem::create_directories(file.parent_path(), error);
    if (error) {
        return false;
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << WithResolution(content, value);
    out.close();
    return !out.fail();
}

} // namespace EdenXbox::TitleSettings
