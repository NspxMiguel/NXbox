// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

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

inline void Record(Kind kind, std::uint64_t us) {
    Counter& counter = Counters()[static_cast<std::size_t>(kind)];
    counter.total_us.fetch_add(us, std::memory_order_relaxed);
    counter.calls.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t seen = counter.max_us.load(std::memory_order_relaxed);
    while (us > seen && !counter.max_us.compare_exchange_weak(seen, us, std::memory_order_relaxed)) {
    }
}

class Scope {
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

} // namespace NxboxStall
