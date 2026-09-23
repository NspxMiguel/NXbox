// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <windows.h>

#include "common/sparse_memory.h"

namespace {
void Require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}
std::size_t Committed(void *base, std::size_t size) {
  std::size_t total = 0;
  auto *cursor = static_cast<unsigned char *>(base);
  const auto *end = cursor + size;
  while (cursor < end) {
    MEMORY_BASIC_INFORMATION info{};
    Require(VirtualQuery(cursor, &info, sizeof(info)) == sizeof(info),
            "Query failed");
    if (info.State == MEM_COMMIT) {
      total += info.RegionSize;
    }
    cursor += info.RegionSize;
  }
  return total;
}
} // namespace

int main() {
  constexpr std::size_t size = std::size_t{4} << 30;
  auto *allocation =
      static_cast<unsigned char *>(Common::SparseMemory::Allocate(size));
  Require(allocation != nullptr, "Sparse reservation failed");
  Require(Committed(allocation, size) == 0,
          "Reservation consumed commit budget");
  volatile unsigned char *bytes = allocation;
  Require(bytes[0] == 0 && bytes[size - 1] == 0,
          "New pages were not zero filled");
  bytes[0] = 17;
  bytes[size - 1] = 29;
  std::array<std::thread, 8> workers;
  for (std::size_t i = 0; i < workers.size(); ++i) {
    workers[i] = std::thread([=] {
      // Independent bytes share a previously untouched chunk.
      bytes[size / 2 + i] = static_cast<unsigned char>(i + 1);
    });
  }
  for (auto &worker : workers) {
    worker.join();
  }
  Require(bytes[0] == 17 && bytes[size - 1] == 29,
          "Commit destroyed existing data");
  for (std::size_t i = 0; i < workers.size(); ++i) {
    Require(bytes[size / 2 + i] == i + 1, "Concurrent first touch lost data");
  }
  Require(Committed(allocation, size) == 3 * 65536,
          "Sparse commit footprint is incorrect");
  auto *second =
      static_cast<unsigned char *>(Common::SparseMemory::Allocate(1));
  Require(second != nullptr, "Second reservation failed");
  *static_cast<volatile unsigned char *>(second) = 31;
  Require(Common::SparseMemory::Free(second), "Second release failed");
  Require(Common::SparseMemory::Free(allocation), "Release failed");
  Require(Common::SparseMemory::Allocate(0) == nullptr,
          "Zero size allocation succeeded");
  Require(Common::SparseMemory::Free(nullptr), "Null release failed");
  std::puts("Sparse memory integration checks passed");
}
