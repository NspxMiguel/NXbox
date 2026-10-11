// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <fstream>
namespace Common::FS {
inline bool CreateDirs(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return !error;
}
inline bool Exists(const std::filesystem::path& path) {
    return std::filesystem::exists(path);
}
inline auto GetSize(const std::filesystem::path& path) {
    return std::filesystem::file_size(path);
}
inline bool RemoveFile(const std::filesystem::path& path) {
    return std::filesystem::remove(path);
}
inline bool NewFile(const std::filesystem::path& path) {
    return std::ofstream(path).good();
}
} // namespace Common::FS
