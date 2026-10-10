// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace Common::SparseMemory {
// Reserve zero-filled data storage and commit only touched 64 KiB chunks.
// The owner must synchronize all accesses before releasing a reservation.
void* Allocate(std::size_t size) noexcept;
// Bytes committed so far by demand faults (diagnostics).
std::uint64_t CommittedBytes() noexcept;
bool Free(void* base) noexcept;
} // namespace Common::SparseMemory
