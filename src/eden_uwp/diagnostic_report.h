// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <charconv>
#include <initializer_list>
#include <string>
#include <string_view>

namespace EdenXbox {
// Prefix every physical line, including continuation lines in Mesa environment chunks.
template <typename Emit>
void DiagnosticReportLines(std::string_view name, std::string_view value, Emit emit) {
    while (!value.empty()) {
        const auto end = value.find_first_of("\r\n");
        const auto line = value.substr(0, end);
        if (!line.empty()) {
            emit(std::string(name.substr(6)) + " " + std::string(line));
        }
        if (end == std::string_view::npos) {
            break;
        }
        value.remove_prefix(end + 1);
    }
}

// Read only committed manifests. Each producer chunk is <4096 bytes; the number
// of parts is dynamic so a large batch cannot be silently capped at eight parts.
template <typename Read, typename Emit>
void CollectBatchJournals(Read read, Emit emit) {
    for (const bool previous : {false, true}) {
        const std::string name =
            previous ? "NXBOX_D3D12_PREVIOUS_GOOD_BATCH" : "NXBOX_D3D12_FIRST_BAD_BATCH";
        const std::string manifest = read(name.c_str());
        if (manifest.empty()) {
            continue;
        }
        emit(name.c_str(), manifest);
        const auto offset = manifest.find(" parts=");
        if (offset == std::string::npos) {
            continue;
        }
        unsigned parts = 0;
        const auto first = manifest.data() + offset + 7;
        const auto last = manifest.data() + manifest.size();
        const auto result = std::from_chars(first, last, parts);
        if (result.ec != std::errc{} || (result.ptr != last && *result.ptr != ' ')) {
            continue;
        }
        const std::string prefix =
            previous ? "NXBOX_D3D12_BATCH_JOURNAL_PREVIOUS_" : "NXBOX_D3D12_BATCH_JOURNAL_";
        for (unsigned part = 0; part < parts; ++part) {
            const auto key = prefix + std::to_string(part);
            const auto value = read(key.c_str());
            if (value.empty()) {
                emit("NXBOX_D3D12_BATCH_JOURNAL_ERROR", "missing part=" + key);
            } else {
                emit(key.c_str(), value);
            }
        }
    }
}
} // namespace EdenXbox
