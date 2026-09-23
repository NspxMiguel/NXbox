// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

namespace Common::SparseMemory {
// Reserve zero-filled data storage and commit only touched 64 KiB chunks.
// The owner must synchronize all accesses before releasing a reservation.
void* Allocate(std::size_t size) noexcept;
bool Free(void* base) noexcept;
} // namespace Common::SparseMemory
