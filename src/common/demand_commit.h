// SPDX-FileCopyrightText: Copyright 2026 SwitchXbox contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace Common::DemandCommit {

struct Chunk {
    std::size_t offset;
    std::size_t size;
};

// Windows AV access kinds: 0 = read, 1 = write, 8 = execute. Guest DRAM is data,
// never host executable memory. Handling an execute fault by committing RW pages
// would retry the same instruction forever because MEM_COMMIT may succeed again.
constexpr std::optional<Chunk> GetChunk(std::uintptr_t base, std::size_t size,
                                        std::uintptr_t address, std::uintptr_t access) {
    constexpr std::size_t granularity = 64 * 1024;
    if ((access != 0 && access != 1) || base == 0 || size == 0 ||
        size - 1 > std::numeric_limits<std::uintptr_t>::max() - base || address < base) {
        return std::nullopt;
    }
    const auto offset = address - base;
    if (offset >= size) {
        return std::nullopt;
    }
    const auto chunk_offset = offset & ~(granularity - 1);
    return Chunk{chunk_offset, std::min(granularity, size - chunk_offset)};
}

} // namespace Common::DemandCommit
