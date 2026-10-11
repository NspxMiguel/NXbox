// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
namespace Common::FS {
enum class EdenPath { NANDDir };
inline std::filesystem::path nand_path;
inline const std::filesystem::path& GetEdenPath(EdenPath) {
    return nand_path;
}
} // namespace Common::FS
