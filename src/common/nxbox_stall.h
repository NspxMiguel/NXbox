// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

// Define to 0 for every target (including dynarmic) to compile out the profiler.
#ifndef NXBOX_STALL_PROFILE
#define NXBOX_STALL_PROFILE 1
#endif

// Attributes frame hitches to host work: each category accumulates its total and its longest
// single call, and the frontend reads and resets them once per pacing window.
namespace NxboxStall {

enum class Kind {
    Shader,
    Upload,
    Convert,
    GarbageCollect,
    Video,
    Jit,
    JitFlush,
    JitProtect,
    JitTranslate,
    JitOptimize,
    JitEmit,
    JitInvalidate,
    Io,
    Aes,
    GpuBusy,
    GlSync,
    GlFinish,
    Readback,
    Query,
    Decommit,
    Present,
    Count
};

struct Counter {
    std::atomic<std::uint64_t> total_us{0};
    std::atomic<std::uint64_t> max_us{0};
    std::atomic<std::uint32_t> calls{0};
};

inline std::array<Counter, static_cast<std::size_t>(Kind::Count)>& Counters() {
    static std::array<Counter, static_cast<std::size_t>(Kind::Count)> counters;
    return counters;
}

inline void Record([[maybe_unused]] Kind kind, [[maybe_unused]] std::uint64_t us) {
#if NXBOX_STALL_PROFILE
    Counter& counter = Counters()[static_cast<std::size_t>(kind)];
    counter.total_us.fetch_add(us, std::memory_order_relaxed);
    counter.calls.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t seen = counter.max_us.load(std::memory_order_relaxed);
    while (us > seen && !counter.max_us.compare_exchange_weak(seen, us, std::memory_order_relaxed)) {
    }
#endif
}

class Scope {
#if NXBOX_STALL_PROFILE
public:
    explicit Scope(Kind kind_) : kind{kind_}, start{std::chrono::steady_clock::now()} {}
    ~Scope() {
        const auto elapsed = std::chrono::steady_clock::now() - start;
        Record(kind, static_cast<std::uint64_t>(
                         std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()));
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Kind kind;
    std::chrono::steady_clock::time_point start;
#else
public:
    explicit Scope(Kind) {}
#endif
};

struct Snapshot {
    std::uint64_t total_us;
    std::uint64_t max_us;
    std::uint32_t calls;
};

inline Snapshot Take(Kind kind) {
    Counter& counter = Counters()[static_cast<std::size_t>(kind)];
    return {counter.total_us.exchange(0, std::memory_order_relaxed),
            counter.max_us.exchange(0, std::memory_order_relaxed),
            counter.calls.exchange(0, std::memory_order_relaxed)};
}

// Event counters are raw counts/bytes, never durations. Window deltas except PcCompileMax.
enum class JitEvent {
    RangeCalls,
    RangeBytes,
    ClearRequests,
    CacheClears,
    ClearBlocks,
    RangeBlocks,
    EmptyInvalidations,
    NewPc,
    RepeatPc,
    UnknownPc,
    PcCompileMax,
    NewKey,
    RepeatKey,
    UnknownKey,
    InstructionInvalidations,
    PageTableInvalidations,
    ProtectBytes,
    Count
};

inline auto& JitEvents() {
    static std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(JitEvent::Count)>
        events{};
    return events;
}

inline void AddJit([[maybe_unused]] JitEvent event, [[maybe_unused]] std::uint64_t value = 1) {
#if NXBOX_STALL_PROFILE
    JitEvents()[static_cast<std::size_t>(event)].fetch_add(value, std::memory_order_relaxed);
#endif
}

// Maximum lifetime compile count of a tracked PC observed during this window.
inline void MaxJit([[maybe_unused]] JitEvent event, [[maybe_unused]] std::uint64_t value) {
#if NXBOX_STALL_PROFILE
    auto& counter = JitEvents()[static_cast<std::size_t>(event)];
    auto seen = counter.load(std::memory_order_relaxed);
    while (value > seen && !counter.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
    }
#endif
}

inline std::uint64_t TakeJit(JitEvent event) {
    return JitEvents()[static_cast<std::size_t>(event)].exchange(0, std::memory_order_relaxed);
}

// Latest sample per processor ID, in bytes, including the prelude. Not reset per window.
// Multiple processes on a core share a slot; this is not a sum of their allocations.
struct JitCache {
    std::atomic<std::uint64_t> used{0};
    std::atomic<std::uint64_t> capacity{0};
};

inline auto& JitCaches() {
    static std::array<JitCache, 4> caches;
    return caches;
}

inline void SampleJitCache([[maybe_unused]] std::size_t core, [[maybe_unused]] std::uint64_t used,
                           [[maybe_unused]] std::uint64_t capacity) {
#if NXBOX_STALL_PROFILE
    if (core < JitCaches().size()) {
        JitCaches()[core].used.store(used, std::memory_order_relaxed);
        JitCaches()[core].capacity.store(capacity, std::memory_order_relaxed);
    }
#endif
}

} // namespace NxboxStall
