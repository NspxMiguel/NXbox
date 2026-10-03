// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <set>
#include <vector>
#include <openssl/evp.h>

#include "common/hex_util.h"
#include "common/logging.h"
#include "core/file_sys/fssystem/fssystem_aes_ctr_counter_extended_storage.h"
#include "core/file_sys/fssystem/fssystem_indirect_storage.h"
#include "core/file_sys/romfs_read_diagnostics.h"
#include "core/file_sys/romfs_verify.h"

namespace FileSys {
namespace {
using Digest = std::array<u8, 32>;
constexpr size_t BatchSize = 1 << 20;
constexpr u64 DetailLimit = 16;
std::mutex scan_mutex;
std::set<std::string> verified;

bool HashBlock(const u8* data, size_t size, Digest& digest) {
    unsigned int written = 0;
    return EVP_Digest(data, size, digest.data(), &written, EVP_sha256(), nullptr) == 1 &&
           written == digest.size();
}

bool InRange(s64 offset, s64 size, u64 total) {
    return offset >= 0 && size > 0 && static_cast<u64>(offset) <= total &&
           static_cast<u64>(size) <= total - static_cast<u64>(offset);
}
} // namespace

void ResetRomfsVerification() {
    const std::scoped_lock lock{scan_mutex};
    verified.clear();
}

RomfsVerification::RomfsVerification(VirtualFile romfs, NcaFsHeader header,
                                     NcaFileSystemDriver::StorageContext context, Hash header_hash,
                                     u64 title_id, std::string identity)
    : m_romfs(std::move(romfs)), m_header(header), m_context(std::move(context)),
      m_header_hash(header_hash), m_title_id(title_id), m_identity(std::move(identity)) {}

void RomfsVerification::LogCounters(std::string_view phase) const {
    if (m_context.indirect_storage) {
        m_context.indirect_storage->LogReadDiagnostics(phase);
    }
    if (m_context.aes_ctr_ex_storage) {
        m_context.aes_ctr_ex_storage->LogReadDiagnostics(phase);
    }
}

void RomfsVerification::Trace(std::string_view role, u64 offset, u64 size) const {
    const auto& indirect = m_context.indirect_storage;
    if (!indirect) {
        LOG_INFO(Loader, "NXBOX VERIFY_ROMFS SOURCE role={} source=base section_offset={:#x}", role,
                 offset);
        return;
    }
    // Include the next entry to calculate the end of each mapped span. Bound the
    // detail output even for an unusually fragmented or damaged table.
    std::array<IndirectStorage::Entry, 9> entries{};
    s32 count = 0;
    const Result result = indirect->GetEntryList(entries.data(), &count,
                                                 static_cast<s32>(entries.size()), offset, size);
    LOG_INFO(Loader, "NXBOX VERIFY_ROMFS MAP role={} result={:#x} entries={} truncated={}", role,
             result.raw, count, count == static_cast<s32>(entries.size()));
    if (R_FAILED(result)) {
        return;
    }
    for (s32 i = 0; i < std::min<s32>(count, 8); ++i) {
        const auto& entry = entries[i];
        const u64 start = std::max<u64>(offset, entry.GetVirtualOffset());
        const u64 end = i + 1 < count
                            ? std::min<u64>(offset + size, entries[i + 1].GetVirtualOffset())
                            : offset + size;
        const u64 physical = entry.GetPhysicalOffset() + start - entry.GetVirtualOffset();
        LOG_INFO(Loader,
                 "NXBOX VERIFY_ROMFS SOURCE role={} source={} virtual={:#x} physical={:#x} "
                 "size={:#x} relocation_start={:#x}",
                 role, entry.storage_index == 0 ? "base" : "update", start, physical, end - start,
                 entry.GetVirtualOffset());
        if (entry.storage_index != 1 || !m_context.aes_ctr_ex_storage) {
            continue;
        }
        std::array<AesCtrCounterExtendedStorage::Entry, 8> subsections{};
        s32 subsection_count = 0;
        const auto& ctr = m_context.aes_ctr_ex_storage;
        const Result ctr_result =
            ctr->GetEntryList(subsections.data(), &subsection_count,
                              static_cast<s32>(subsections.size()), physical, end - start);
        if (R_FAILED(ctr_result)) {
            LOG_ERROR(Loader, "NXBOX VERIFY_ROMFS CTR_MAP result={:#x}", ctr_result.raw);
            continue;
        }
        for (s32 j = 0; j < subsection_count; ++j) {
            const auto& subsection = subsections[j];
            const auto position = std::max<u64>(physical, subsection.GetOffset());
            LOG_INFO(Loader,
                     "NXBOX VERIFY_ROMFS CTR subsection_start={:#x} generation={:#x} "
                     "encrypted={} nca_offset={:#x} counter_low={:#x}",
                     subsection.GetOffset(), static_cast<u32>(subsection.generation),
                     subsection.encryption_value ==
                         AesCtrCounterExtendedStorage::Entry::Encryption::Encrypted,
                     ctr->GetCounterOffset() + position, (ctr->GetCounterOffset() + position) / 16);
        }
    }
}

void RomfsVerification::Verify() const {
    if (!IsRomfsVerificationEnabled()) {
        return;
    }
    const auto& raw = m_context.raw_storage;
    const auto& meta = m_header.hash_data.integrity_meta_info;
    const auto& tree = meta.level_hash_info;
    if (!raw || m_header.hash_type != NcaFsHeader::HashType::HierarchicalIntegrityHash ||
        (m_header.compression_info.bucket.offset != 0 &&
         m_header.compression_info.bucket.size != 0)) {
        LOG_WARNING(Loader,
                    "NXBOX VERIFY_ROMFS SKIP title={:016X} reason=unsupported_layout identity={}",
                    m_title_id, m_identity);
        return;
    }

    Digest header_digest{};
    const bool header_hash_ok =
        HashBlock(reinterpret_cast<const u8*>(&m_header), sizeof(m_header), header_digest) &&
        header_digest == m_header_hash.value;

    // PatchRomFS is called several times while booting. Scan each selected NCA/base
    // pair once per launch, but keep counters on every mount. Never cache a failure.
    const std::scoped_lock lock{scan_mutex};
    const auto key =
        fmt::format("{}:{}:{}", m_title_id, m_identity, Common::HexToString(header_digest));
    if (header_hash_ok && verified.contains(key)) {
        LOG_INFO(Loader, "NXBOX VERIFY_ROMFS CACHED title={:016X} identity={}", m_title_id,
                 m_identity);
        LogCounters("cached_mount");
        return;
    }

    const auto start_time = std::chrono::steady_clock::now();
    LOG_INFO(Loader,
             "NXBOX VERIFY_ROMFS BEGIN title={:016X} identity={} raw_size={:#x} romfs_size={:#x} "
             "layers={} detail_limit={} scan=full batch_size={:#x} fs_header_hash_ok={}",
             m_title_id, m_identity, raw->GetSize(), m_romfs->GetSize(), tree.max_layers,
             DetailLimit, BatchSize, header_hash_ok);
    LogCounters("verify_begin");
    if (meta.magic != Common::MakeMagic('I', 'V', 'F', 'C') || meta.version != 0x20000 ||
        meta.master_hash_size != Digest{}.size() || tree.max_layers < 2 ||
        tree.max_layers > tree.info.size() + 1) {
        LOG_ERROR(Loader, "NXBOX VERIFY_ROMFS ABORT reason=invalid_ivfc_header");
        return;
    }
    const u32 levels = tree.max_layers - 1;
    for (u32 level = 0; level < levels; ++level) {
        const auto& info = tree.info[level];
        if (!InRange(info.offset, info.size, raw->GetSize()) || info.block_order < 5 ||
            info.block_order > 20) {
            LOG_ERROR(Loader, "NXBOX VERIFY_ROMFS ABORT reason=invalid_level level={}", level + 1);
            return;
        }
        const u64 block_size = u64{1} << info.block_order;
        const u64 blocks = (static_cast<u64>(info.size.Get()) + block_size - 1) / block_size;
        const u64 parent_size =
            level == 0 ? meta.master_hash_size : tree.info[level - 1].size.Get();
        if (blocks > parent_size / Digest{}.size()) {
            LOG_ERROR(Loader, "NXBOX VERIFY_ROMFS ABORT reason=parent_too_small level={}",
                      level + 1);
            return;
        }
    }
    if (m_romfs->GetSize() != static_cast<u64>(tree.info[levels - 1].size.Get())) {
        LOG_ERROR(Loader, "NXBOX VERIFY_ROMFS ABORT reason=romfs_size_mismatch");
        return;
    }

    u64 total_blocks = 0, mismatches = 0, read_errors = 0, shape_mismatches = 0;
    bool ancestors_ok = header_hash_ok;
    std::vector<u8> data(BatchSize);
    std::vector<u8> expected(BatchSize);
    std::vector<u8> unaligned(BatchSize);
    auto last_progress = start_time;
    for (u32 level = 0; level < levels; ++level) {
        const auto& info = tree.info[level];
        const u64 level_size = info.size.Get();
        const u64 section_start = info.offset.Get();
        const size_t block_size = size_t{1} << info.block_order;
        const bool leaf = level + 1 == levels;
        u64 level_mismatches = 0, level_read_errors = 0;
        LOG_INFO(Loader,
                 "NXBOX VERIFY_ROMFS LEVEL_BEGIN level={} data={} section_offset={:#x} size={:#x} "
                 "block_size={:#x} ancestors_ok={}",
                 level + 1, leaf, section_start, level_size, block_size, ancestors_ok);
        for (u64 position = 0; position < level_size; position += BatchSize) {
            const size_t requested = std::min<u64>(BatchSize, level_size - position);
            const size_t blocks = (requested + block_size - 1) / block_size;
            std::fill(data.begin(), data.end(), 0);
            const auto got = leaf ? m_romfs->Read(data.data(), requested, position)
                                  : raw->Read(data.data(), requested, section_start + position);
            const u64 hash_offset = (position / block_size) * Digest{}.size();
            const size_t hash_bytes = blocks * Digest{}.size();
            std::fill(expected.begin(), expected.begin() + hash_bytes, 0);
            size_t hash_got = 0;
            if (level == 0) {
                std::copy(meta.master_hash.value.begin(), meta.master_hash.value.end(),
                          expected.begin());
                hash_got = Digest{}.size();
            } else {
                hash_got = raw->Read(expected.data(), hash_bytes,
                                     tree.info[level - 1].offset.Get() + hash_offset);
            }
            for (size_t block = 0; block < blocks; ++block) {
                ++total_blocks;
                const size_t in_batch = block * block_size;
                const u64 block_offset = position + in_batch;
                const size_t valid_size = std::min<u64>(block_size, level_size - block_offset);
                const bool read_ok =
                    got >= in_batch + valid_size && hash_got >= (block + 1) * Digest{}.size();
                Digest actual{};
                if (!HashBlock(data.data() + in_batch, block_size, actual)) {
                    LOG_ERROR(Loader, "NXBOX VERIFY_ROMFS ABORT reason=sha256_failed");
                    return;
                }
                if (read_ok && std::equal(actual.begin(), actual.end(),
                                          expected.begin() + block * actual.size())) {
                    continue;
                }
                ++mismatches;
                ++level_mismatches;
                read_errors += !read_ok;
                level_read_errors += !read_ok;
                if (mismatches <= DetailLimit) {
                    Digest wanted{};
                    std::copy_n(expected.data() + block * wanted.size(), wanted.size(),
                                wanted.data());
                    // Retry through the raw section, then bypass BKTR's coalescing.
                    std::vector<u8> retry(block_size, 0);
                    Digest retry_hash{}, unmerged_hash{};
                    const u64 section_offset = section_start + block_offset;
                    const bool retry_ok =
                        raw->Read(retry.data(), valid_size, section_offset) == valid_size &&
                        HashBlock(retry.data(), retry.size(), retry_hash);
                    std::fill(retry.begin(), retry.end(), 0);
                    const bool unmerged_ok =
                        m_context.indirect_storage &&
                        m_context.indirect_storage->ReadUnmerged(retry.data(), valid_size,
                                                                 section_offset) == valid_size &&
                        HashBlock(retry.data(), retry.size(), unmerged_hash);
                    LOG_ERROR(Loader,
                              "NXBOX VERIFY_ROMFS MISMATCH level={} data={} block={} "
                              "level_offset={:#x} section_offset={:#x} read_ok={} ancestors_ok={} "
                              "expected={} actual={} raw_retry_matches={} unmerged_matches={} "
                              "raw_retry_same={}",
                              level + 1, leaf, block_offset / block_size, block_offset,
                              section_offset, read_ok, ancestors_ok, Common::HexToString(wanted),
                              Common::HexToString(actual), retry_ok && retry_hash == wanted,
                              unmerged_ok && unmerged_hash == wanted,
                              retry_ok && retry_hash == actual);
                    Trace("data", section_offset, valid_size);
                    if (level != 0) {
                        Trace("hash",
                              tree.info[level - 1].offset.Get() + hash_offset +
                                  block * wanted.size(),
                              wanted.size());
                    }
                }
            }
            // Exercise the same bytes with an unaligned request as well. An aligned
            // sweep alone cannot detect a head/tail or read-shape-dependent BKTR bug.
            if (leaf && requested > 2 && got == requested) {
                const auto shape_size = requested - 2;
                const auto shape_got = m_romfs->Read(unaligned.data(), shape_size, position + 1);
                const auto difference = std::mismatch(
                    data.begin() + 1, data.begin() + 1 + shape_size, unaligned.begin());
                if (shape_got != shape_size || difference.first != data.begin() + 1 + shape_size) {
                    ++shape_mismatches;
                    if (shape_mismatches <= DetailLimit) {
                        const auto first = shape_got == shape_size
                                               ? static_cast<u64>(difference.first - data.begin())
                                               : u64{1};
                        LOG_ERROR(Loader,
                                  "NXBOX VERIFY_ROMFS SHAPE_MISMATCH level_offset={:#x} "
                                  "section_offset={:#x} requested={:#x} actual={:#x}",
                                  position + first, section_start + position + first, shape_size,
                                  shape_got);
                        Trace("shape", section_start + position + first, 1);
                    }
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - last_progress >= std::chrono::seconds(5)) {
                LOG_INFO(
                    Loader,
                    "NXBOX VERIFY_ROMFS PROGRESS level={} offset={:#x} size={:#x} mismatches={}",
                    level + 1, position + requested, level_size, mismatches);
                last_progress = now;
            }
        }
        LOG_INFO(Loader, "NXBOX VERIFY_ROMFS LEVEL_END level={} mismatches={} read_errors={}",
                 level + 1, level_mismatches, level_read_errors);
        ancestors_ok &= level_mismatches == 0;
    }
    LogCounters("verify_end");
    LOG_INFO(Loader,
             "NXBOX VERIFY_ROMFS END title={:016X} complete=true blocks={} mismatches={} "
             "read_errors={} shape_mismatches={} fs_header_hash_ok={} elapsed_ms={}",
             m_title_id, total_blocks, mismatches, read_errors, shape_mismatches, header_hash_ok,
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - start_time)
                 .count());
    if (mismatches == 0 && shape_mismatches == 0 && header_hash_ok) {
        verified.insert(key);
    }
}

} // namespace FileSys
