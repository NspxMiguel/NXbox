// SPDX-FileCopyrightText: Copyright 2026 SwitchXbox contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <iostream>
#include <limits>
#include "common/demand_commit.h"

using Common::DemandCommit::GetChunk;

int main() {
    unsigned checks = 0;
    const auto check = [&](bool passed, const char* name) {
        ++checks;
        if (!passed) {
            std::cerr << "FAIL: " << name << '\n';
            std::exit(1);
        }
    };
    constexpr std::uintptr_t base = 0x100000;
    constexpr std::size_t size = 0x21000;
    auto chunk = GetChunk(base, size, base, 0);
    check(chunk && chunk->offset == 0 && chunk->size == 0x10000, "first read");
    chunk = GetChunk(base, size, base + 0x10000, 1);
    check(chunk && chunk->offset == 0x10000 && chunk->size == 0x10000, "boundary write");
    chunk = GetChunk(base, size, base + size - 1, 1);
    check(chunk && chunk->offset == 0x20000 && chunk->size == 0x1000, "partial last chunk");
    check(!GetChunk(base, size, base - 1, 0), "below backing");
    check(!GetChunk(base, size, base + size, 1), "exclusive upper bound");
    check(!GetChunk(base, size, base, 8), "execute fault must propagate");
    check(!GetChunk(base, size, base, 2), "unknown access must propagate");
    check(!GetChunk(0, size, 1, 0), "missing backing");
    check(!GetChunk(base, 0, base, 0), "empty backing");
    constexpr auto max = std::numeric_limits<std::uintptr_t>::max();
    check(!GetChunk(max - 0x1000, 0x2000, max, 0), "overflowing backing");
    chunk = GetChunk(max - 0xfff, 0x1000, max, 0);
    check(chunk && chunk->offset == 0 && chunk->size == 0x1000, "highest valid address");
    // Validate every byte near both chunk boundaries, for reads and writes.
    for (std::uintptr_t access : {0, 1}) {
        for (std::size_t offset = 0; offset < size; ++offset) {
            chunk = GetChunk(base, size, base + offset, access);
            check(chunk && chunk->offset <= offset && offset - chunk->offset < chunk->size &&
                      chunk->offset + chunk->size <= size && chunk->offset % 0x10000 == 0,
                  "chunk contains fault without crossing reservation");
        }
    }
    std::cout << checks << " demand-commit checks passed\n";
}
