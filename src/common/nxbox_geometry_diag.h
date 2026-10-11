// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// Shared by Eden and the patched Mesa DLL. Each module publishes its own report.
inline bool nxbox_geometry_diag_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("NXBOX_GEOMETRY_DIAG");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

struct NxboxGeometrySample {
    unsigned vertex_offset = 0;
    unsigned vertex_stride = 0;
    unsigned element_offset = 0;
    unsigned unknown_format = 0;
    unsigned emulated_format = 0;
    unsigned cbv_offset = 0;
    unsigned cbv_size = 0;
    unsigned stream_output = 0;
};

inline void nxbox_record_geometry(const NxboxGeometrySample& sample) {
    if (!nxbox_geometry_diag_enabled())
        return;
    static std::atomic<uint64_t> draws{0};
    static std::atomic<uint64_t> counts[8]{};
    const unsigned values[]{sample.vertex_offset,  sample.vertex_stride,   sample.element_offset,
                            sample.unknown_format, sample.emulated_format, sample.cbv_offset,
                            sample.cbv_size,       sample.stream_output};
    for (unsigned i = 0; i < 8; ++i)
        counts[i].fetch_add(values[i] != 0, std::memory_order_relaxed);
    const auto draw = draws.fetch_add(1, std::memory_order_relaxed) + 1;
    if (draw > 16 && draw % 4096)
        return;
    char text[512];
    std::snprintf(text, sizeof(text),
                  "draws=%llu vb_offset=%llu vb_stride=%llu element_offset=%llu "
                  "unknown_format=%llu emulated_format=%llu cbv_offset=%llu cbv_size=%llu so=%llu",
                  (unsigned long long)draw,
                  (unsigned long long)counts[0].load(std::memory_order_relaxed),
                  (unsigned long long)counts[1].load(std::memory_order_relaxed),
                  (unsigned long long)counts[2].load(std::memory_order_relaxed),
                  (unsigned long long)counts[3].load(std::memory_order_relaxed),
                  (unsigned long long)counts[4].load(std::memory_order_relaxed),
                  (unsigned long long)counts[5].load(std::memory_order_relaxed),
                  (unsigned long long)counts[6].load(std::memory_order_relaxed),
                  (unsigned long long)counts[7].load(std::memory_order_relaxed));
    SetEnvironmentVariableA("NXBOX_D3D12_GEOMETRY", text);
}

inline void nxbox_record_uniform_binding(unsigned stage, unsigned slot, uint64_t offset,
                                         uint64_t size, uint64_t alignment) {
    if (!nxbox_geometry_diag_enabled())
        return;
    static std::atomic<uint64_t> bindings{0}, misaligned{0}, oversized{0};
    const bool bad_offset = alignment > 1 && offset % alignment != 0;
    const bool bad_size = size > 65536;
    const auto total = bindings.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto bad = misaligned.fetch_add(bad_offset, std::memory_order_relaxed) + bad_offset;
    const auto large = oversized.fetch_add(bad_size, std::memory_order_relaxed) + bad_size;
    // Preserve the first suspect range; later healthy draws must not erase it.
    static std::atomic<bool> first{false};
    char text[256];
    const bool first_bad =
        (bad_offset || bad_size) && !first.exchange(true, std::memory_order_relaxed);
    if (first_bad) {
        std::snprintf(text, sizeof(text), "stage=%u slot=%u offset=%llu size=%llu alignment=%llu",
                      stage, slot, (unsigned long long)offset, (unsigned long long)size,
                      (unsigned long long)alignment);
        SetEnvironmentVariableA("NXBOX_GL_UBO_FIRST", text);
    }
    if (total > 16 && total % 4096 && !first_bad)
        return;
    std::snprintf(text, sizeof(text), "bindings=%llu misaligned=%llu oversized=%llu",
                  (unsigned long long)total, (unsigned long long)bad, (unsigned long long)large);
    SetEnvironmentVariableA("NXBOX_GL_UBO_GEOMETRY", text);
}
