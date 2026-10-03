// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdlib>
#include <string_view>

#include "common/common_types.h"
#include "common/logging.h"

namespace FileSys {

inline bool IsRomfsVerificationEnabled() {
    const char* value = std::getenv("NXBOX_VERIFY_ROMFS");
    return value != nullptr && std::string_view{value} == "1";
}

// Capture the switch at mount time, not on every read. Do not cache getenv globally:
// the frontend loads nxbox_env.txt after its initial content scan.
class BktrReadDiagnostics {
public:
    explicit BktrReadDiagnostics(std::string_view kind) : m_kind(kind) {}

    void Initialize(s32 entries) {
        m_enabled = IsRomfsVerificationEnabled();
        m_entries = entries;
        Log("mount");
    }

    bool Enabled() const {
        return m_enabled;
    }

    void Record(u64 entries, bool failed) const {
        if (!m_enabled) {
            return;
        }
        m_crossing_reads.fetch_add(entries > 1, std::memory_order_relaxed);
        m_failures.fetch_add(failed, std::memory_order_relaxed);
        const auto reads = m_reads.fetch_add(1, std::memory_order_relaxed) + 1;
        if (reads >= 1024 && (reads & (reads - 1)) == 0) {
            Log("progress");
        }
    }

    void Failure(u64 offset, u64 size, u32 result) const {
        if (m_enabled && m_error_details.fetch_add(1, std::memory_order_relaxed) < 16) {
            LOG_ERROR(Loader,
                      "NXBOX BKTR READ_ERROR kind={} id={} offset={:#x} size={:#x} result={:#x}",
                      m_kind, fmt::ptr(this), offset, size, result);
        }
    }

    void Log(std::string_view phase) const {
        if (m_enabled) {
            LOG_INFO(Loader,
                     "NXBOX BKTR kind={} id={} phase={} entries={} reads={} crossing_reads={} "
                     "failed_reads={}",
                     m_kind, fmt::ptr(this), phase, m_entries,
                     m_reads.load(std::memory_order_relaxed),
                     m_crossing_reads.load(std::memory_order_relaxed),
                     m_failures.load(std::memory_order_relaxed));
        }
    }

private:
    std::string_view m_kind;
    bool m_enabled{};
    s32 m_entries{};
    mutable std::atomic<u64> m_reads{};
    mutable std::atomic<u64> m_crossing_reads{};
    mutable std::atomic<u64> m_failures{};
    mutable std::atomic<u32> m_error_details{};
};

} // namespace FileSys
