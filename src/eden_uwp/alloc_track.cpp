// SPDX-License-Identifier: GPL-3.0-or-later

#include "eden_uwp/alloc_track.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <new>
#include <vector>

#include <intrin.h>

#include <fmt/format.h>

extern "C" __declspec(dllimport) void* __stdcall GetModuleHandleW(const wchar_t*);

namespace {

constexpr std::size_t kMinTracked = 2 * 1024;
constexpr std::size_t kSlots = 1 << 21;

struct Entry {
    void* pointer;
    std::size_t bytes;
    void* site;
};

Entry g_table[kSlots];
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
std::atomic<bool> g_enabled{false};
std::atomic<std::size_t> g_live_entries{0};

struct Guard {
    Guard() {
        while (g_lock.test_and_set(std::memory_order_acquire)) {
        }
    }
    ~Guard() {
        g_lock.clear(std::memory_order_release);
    }
};

std::size_t Slot(void* pointer) {
    return (reinterpret_cast<std::uintptr_t>(pointer) >> 4) & (kSlots - 1);
}

void Track(void* pointer, std::size_t bytes, void* site) {
    if (pointer == nullptr || bytes < kMinTracked || !g_enabled.load(std::memory_order_relaxed)) {
        return;
    }
    const Guard guard;
    for (std::size_t i = 0; i < kSlots; ++i) {
        Entry& entry = g_table[(Slot(pointer) + i) & (kSlots - 1)];
        if (entry.pointer == nullptr) {
            entry = {pointer, bytes, site};
            g_live_entries.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

void Untrack(void* pointer) {
    if (pointer == nullptr || g_live_entries.load(std::memory_order_relaxed) == 0) {
        return;
    }
    const Guard guard;
    for (std::size_t i = 0; i < kSlots; ++i) {
        Entry& entry = g_table[(Slot(pointer) + i) & (kSlots - 1)];
        if (entry.pointer == pointer) {
            // Keep the probe chain intact: mark deleted with a tombstone that never matches.
            entry.pointer = reinterpret_cast<void*>(1);
            entry.bytes = 0;
            g_live_entries.fetch_sub(1, std::memory_order_relaxed);
            return;
        }
        if (entry.pointer == nullptr) {
            return;
        }
    }
}

void* Allocate(std::size_t bytes, void* site) {
    void* pointer = std::malloc(bytes != 0 ? bytes : 1);
    Track(pointer, bytes, site);
    return pointer;
}

void* AllocateAligned(std::size_t bytes, std::size_t alignment, void* site) {
    void* pointer = _aligned_malloc(bytes != 0 ? bytes : 1, alignment);
    Track(pointer, bytes, site);
    return pointer;
}

} // namespace

void NxboxAllocTrackEnable() {
    g_enabled.store(true);
}

std::string NxboxAllocTrackReport() {
    std::map<void*, std::pair<std::size_t, std::size_t>> sites;
    std::size_t total = 0;
    {
        const Guard guard;
        for (const Entry& entry : g_table) {
            if (entry.pointer != nullptr && entry.pointer != reinterpret_cast<void*>(1)) {
                auto& site = sites[entry.site];
                site.first += entry.bytes;
                ++site.second;
                total += entry.bytes;
            }
        }
    }
    std::vector<std::pair<std::size_t, std::pair<void*, std::size_t>>> sorted;
    for (const auto& [site, usage] : sites) {
        sorted.push_back({usage.first, {site, usage.second}});
    }
    std::sort(sorted.rbegin(), sorted.rend());
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::string out = fmt::format("total={}MiB sites={}", total >> 20, sites.size());
    for (std::size_t i = 0; i < sorted.size() && i < 12; ++i) {
        out += fmt::format(" site={:x} bytes={}MiB count={}",
                           reinterpret_cast<std::uintptr_t>(sorted[i].second.first) - base,
                           sorted[i].first >> 20, sorted[i].second.second);
    }
    return out;
}

void* operator new(std::size_t bytes) {
    if (void* pointer = Allocate(bytes, _ReturnAddress())) {
        return pointer;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) {
    if (void* pointer = Allocate(bytes, _ReturnAddress())) {
        return pointer;
    }
    throw std::bad_alloc();
}
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    return Allocate(bytes, _ReturnAddress());
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
    return Allocate(bytes, _ReturnAddress());
}
void* operator new(std::size_t bytes, std::align_val_t alignment) {
    if (void* pointer = AllocateAligned(bytes, static_cast<std::size_t>(alignment), _ReturnAddress())) {
        return pointer;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    if (void* pointer = AllocateAligned(bytes, static_cast<std::size_t>(alignment), _ReturnAddress())) {
        return pointer;
    }
    throw std::bad_alloc();
}
void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return AllocateAligned(bytes, static_cast<std::size_t>(alignment), _ReturnAddress());
}
void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return AllocateAligned(bytes, static_cast<std::size_t>(alignment), _ReturnAddress());
}

void operator delete(void* pointer) noexcept {
    Untrack(pointer);
    std::free(pointer);
}
void operator delete[](void* pointer) noexcept {
    Untrack(pointer);
    std::free(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept {
    Untrack(pointer);
    std::free(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
    Untrack(pointer);
    std::free(pointer);
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept {
    Untrack(pointer);
    std::free(pointer);
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
    Untrack(pointer);
    std::free(pointer);
}
void operator delete(void* pointer, std::align_val_t) noexcept {
    Untrack(pointer);
    _aligned_free(pointer);
}
void operator delete[](void* pointer, std::align_val_t) noexcept {
    Untrack(pointer);
    _aligned_free(pointer);
}
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
    Untrack(pointer);
    _aligned_free(pointer);
}
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
    Untrack(pointer);
    _aligned_free(pointer);
}
void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
    Untrack(pointer);
    _aligned_free(pointer);
}
void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
    Untrack(pointer);
    _aligned_free(pointer);
}
