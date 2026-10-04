// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <vector>
#include <openssl/evp.h>

#include "common/hex_util.h"
#include "common/logging.h"
#include "common/scope_exit.h"
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
std::set<std::string> checked_mounts;

bool HashBlock(const u8* data, size_t size, Digest& digest) {
    unsigned int written = 0;
    return EVP_Digest(data, size, digest.data(), &written, EVP_sha256(), nullptr) == 1 &&
           written == digest.size();
}

bool InRange(s64 offset, s64 size, u64 total) {
    return offset >= 0 && size > 0 && static_cast<u64>(offset) <= total &&
           static_cast<u64>(size) <= total - static_cast<u64>(offset);
}
// Decode entry sets sequentially, without BucketTree::Find/GetEntryList or its
// binary-search nodes. The last set header supplies the exclusive end sentinel.
template <typename Entry, typename Offset>
bool ReadFlatTable(VirtualFile meta, const std::array<u8, 0x10>& header_bytes,
                   std::vector<Entry>& entries, u64& end, Offset get_offset) {
    BucketTree::Header header{};
    std::memcpy(&header, header_bytes.data(), sizeof(header));
    if (!meta || header.magic != BucketTree::Magic || header.version != 1 ||
        header.entry_count <= 0) {
        return false;
    }
    constexpr size_t NodeSize = 0x4000;
    constexpr size_t Capacity = (NodeSize - sizeof(BucketTree::NodeHeader)) / sizeof(Entry);
    const u64 node_bytes =
        BucketTree::QueryNodeStorageSize(NodeSize, sizeof(Entry), header.entry_count);
    const u64 sets = (static_cast<u64>(header.entry_count) + Capacity - 1) / Capacity;
    if (node_bytes > meta->GetSize() || sets > (meta->GetSize() - node_bytes) / NodeSize) {
        return false;
    }
    std::array<u8, NodeSize> bytes{};
    s64 previous = -1;
    for (u64 set = 0; set < sets; ++set) {
        if (meta->Read(bytes.data(), bytes.size(), node_bytes + set * NodeSize) != bytes.size()) {
            return false;
        }
        BucketTree::NodeHeader node{};
        std::memcpy(&node, bytes.data(), sizeof(node));
        const auto count = std::min<u64>(Capacity, header.entry_count - entries.size());
        if (node.index != static_cast<s64>(set) || node.count != static_cast<s64>(count) ||
            node.offset <= 0) {
            return false;
        }
        for (u64 i = 0; i < count; ++i) {
            Entry entry{};
            std::memcpy(&entry, bytes.data() + sizeof(node) + i * sizeof(Entry), sizeof(entry));
            const s64 offset = get_offset(entry);
            if (offset <= previous || offset >= node.offset ||
                (i == 0 && set != 0 && static_cast<u64>(offset) != end)) {
                return false;
            }
            previous = offset;
            entries.push_back(entry);
        }
        end = node.offset;
    }
    return entries.size() == static_cast<size_t>(header.entry_count);
}

struct ReferenceReader {
    const NcaFileSystemDriver::StorageContext& context;
    std::vector<IndirectStorage::Entry> relocations;
    std::vector<AesCtrCounterExtendedStorage::Entry> subsections;
    u64 relocation_end{}, subsection_end{};
    u64 base_spans{}, update_spans{};

    bool Initialize(const NcaPatchInfo& patch) {
        return (!context.indirect_storage ||
                ReadFlatTable(context.indirect_storage_meta_storage, patch.indirect_header,
                              relocations, relocation_end,
                              [](const auto& entry) { return entry.GetVirtualOffset(); })) &&
               (!context.aes_ctr_ex_storage ||
                ReadFlatTable(context.aes_ctr_ex_storage_meta_storage, patch.aes_ctr_ex_header,
                              subsections, subsection_end,
                              [](const auto& entry) { return entry.GetOffset(); }));
    }

    size_t Read(u8* output, size_t size, u64 offset) {
        if (!context.indirect_storage) {
            return context.aes_ctr_ex_storage ? ReadPatch(output, size, offset)
                                              : context.raw_storage->Read(output, size, offset);
        }
        if (offset > relocation_end || size > relocation_end - offset) {
            return 0;
        }
        size_t done = 0;
        // Intentionally linear lookup, independent of production tree searches.
        for (size_t i = 0; i < relocations.size() && done < size; ++i) {
            const auto& entry = relocations[i];
            const u64 start = entry.GetVirtualOffset();
            const u64 end =
                i + 1 < relocations.size() ? relocations[i + 1].GetVirtualOffset() : relocation_end;
            const u64 position = offset + done;
            if (position < start || position >= end) {
                continue;
            }
            if (entry.GetPhysicalOffset() < 0) {
                return 0;
            }
            const u64 physical = static_cast<u64>(entry.GetPhysicalOffset()) + position - start;
            const size_t chunk = std::min<u64>(size - done, end - position);
            if (entry.storage_index == 0) {
                ++base_spans;
                const auto& base = context.original_indirectable_storage;
                if (!base || physical > base->GetSize() || chunk > base->GetSize() - physical ||
                    base->Read(output + done, chunk, physical) != chunk) {
                    return 0;
                }
            } else if (entry.storage_index == 1) {
                ++update_spans;
                if (ReadPatch(output + done, chunk, physical) != chunk) {
                    return 0;
                }
            } else {
                return 0;
            }
            done += chunk;
        }
        return done;
    }

    size_t ReadPatch(u8* output, size_t size, u64 offset) const {
        if (!context.aes_ctr_ex_storage) {
            return context.fs_data_storage->Read(output, size, offset);
        }
        if (offset > subsection_end || size > subsection_end - offset) {
            return 0;
        }
        size_t done = 0;
        for (size_t i = 0; i < subsections.size() && done < size; ++i) {
            const auto& entry = subsections[i];
            const u64 start = entry.GetOffset();
            const u64 end =
                i + 1 < subsections.size() ? subsections[i + 1].GetOffset() : subsection_end;
            const u64 position = offset + done;
            if (position < start || position >= end) {
                continue;
            }
            using Encryption = AesCtrCounterExtendedStorage::Entry::Encryption;
            if (entry.encryption_value != Encryption::Encrypted &&
                entry.encryption_value != Encryption::NotEncrypted) {
                return 0;
            }
            const size_t chunk = std::min<u64>(size - done, end - position);
            if (context.aes_ctr_ex_storage->ReadReference(
                    output + done, chunk, position, static_cast<u32>(entry.generation),
                    entry.encryption_value == Encryption::Encrypted) != chunk) {
                return 0;
            }
            done += chunk;
        }
        return done;
    }
};
} // namespace

void ResetRomfsVerification() {
    const std::scoped_lock lock{scan_mutex};
    checked_mounts.clear();
}

RomfsVerification::RomfsVerification(VirtualFile romfs, NcaFsHeader header,
                                     NcaFileSystemDriver::StorageContext context, Hash header_hash,
                                     u64 title_id, std::string identity, bool is_update)
    : m_romfs(std::move(romfs)), m_header(header), m_context(std::move(context)),
      m_header_hash(header_hash), m_title_id(title_id), m_identity(std::move(identity)),
      m_is_update(is_update) {}

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

void RomfsVerification::Verify(VirtualFile received, bool full) const {
    // Emit the bounded check before an optional, potentially multi-minute walk.
    if (full) {
        Verify(received, false);
    }
    const auto& raw = m_context.raw_storage;
    const auto& meta = m_header.hash_data.integrity_meta_info;
    const auto& tree = meta.level_hash_info;
    Digest header_digest{};
    const bool header_hash_ok =
        HashBlock(reinterpret_cast<const u8*>(&m_header), sizeof(m_header), header_digest) &&
        header_digest == m_header_hash.value;

    const std::scoped_lock lock{scan_mutex};
    const auto key = fmt::format("{}:{}:{}:{}:{}", m_title_id, m_identity,
                                 Common::HexToString(header_digest), full, received == m_romfs);
    // One report per selected mount identity per launch, including failures.
    // ResetRomfsVerification clears attempts before each UWP game launch.
    if (!checked_mounts.insert(key).second) {
        LogCounters("cached_mount");
        return;
    }
    u64 total_blocks = 0, mismatches = 0, read_errors = 0, shape_mismatches = 0;
    u64 reference_blocks = 0, reference_bad = 0;
    std::optional<u64> first_bad_offset;
    u32 first_bad_level = 0;
    bool complete = false;
    const char* reason = "incomplete";
    const char* source = m_context.indirect_storage ? "relocated" : m_is_update ? "update" : "base";
    SCOPE_EXIT {
        if (!full) {
            const bool ok = complete && header_hash_ok && mismatches == 0 &&
                            shape_mismatches == 0 && reference_bad == 0;
            LOG_INFO(Loader,
                     "NXBOX ROMFS_CHECK {} blocks={} bad={} first_bad_offset={} source={} "
                     "title={:016X} first_bad_level={} read_errors={} shape_bad={} "
                     "reference_blocks={} reference_bad={} fs_header_hash_ok={} reason={} "
                     "scope={} scan=sample identity={}",
                     ok ? "ok" : "FAIL", total_blocks,
                     mismatches + shape_mismatches + reference_bad + !header_hash_ok + !complete,
                     first_bad_offset ? fmt::format("{:#x}", *first_bad_offset) : "none", source,
                     m_title_id, first_bad_level, read_errors, shape_mismatches, reference_blocks,
                     reference_bad, header_hash_ok, reason,
                     received == m_romfs ? "received" : "unverified_received", m_identity);
        }
    };
    if (!raw || !received ||
        m_header.hash_type != NcaFsHeader::HashType::HierarchicalIntegrityHash ||
        m_context.current_sparse_storage ||
        (m_header.compression_info.bucket.offset != 0 &&
         m_header.compression_info.bucket.size != 0)) {
        reason = "unsupported_layout";
        LOG_WARNING(Loader, "NXBOX VERIFY_ROMFS SKIP title={:016X} reason={} identity={}",
                    m_title_id, reason, m_identity);
        return;
    }
    if (received != m_romfs) {
        // A rebuilt LayeredFS has different offsets and no matching signed tree.
        // Do not authenticate the underlying mount and label the result a pass.
        reason = "received_file_changed";
        return;
    }
    const auto start_time = std::chrono::steady_clock::now();
    LOG_INFO(Loader,
             "NXBOX VERIFY_ROMFS BEGIN title={:016X} identity={} raw_size={:#x} romfs_size={:#x} "
             "layers={} detail_limit={} scan={} batch_size={:#x} fs_header_hash_ok={}",
             m_title_id, m_identity, raw->GetSize(), m_romfs->GetSize(), tree.max_layers,
             DetailLimit, full ? "full" : "sample", BatchSize, header_hash_ok);
    LogCounters("verify_begin");
    if (meta.magic != Common::MakeMagic('I', 'V', 'F', 'C') || meta.version != 0x20000 ||
        meta.master_hash_size != Digest{}.size() || tree.max_layers < 2 ||
        tree.max_layers > tree.info.size() + 1) {
        reason = "invalid_ivfc_header";
        LOG_ERROR(Loader, "NXBOX VERIFY_ROMFS ABORT reason=invalid_ivfc_header");
        return;
    }
    const u32 levels = tree.max_layers - 1;
    for (u32 level = 0; level < levels; ++level) {
        const auto& info = tree.info[level];
        if (!InRange(info.offset, info.size, raw->GetSize()) || info.block_order < 5 ||
            info.block_order > 20) {
            reason = "invalid_level";
            LOG_ERROR(Loader, "NXBOX VERIFY_ROMFS ABORT reason=invalid_level level={}", level + 1);
            return;
        }
        const u64 block_size = u64{1} << info.block_order;
        const u64 blocks = (static_cast<u64>(info.size.Get()) + block_size - 1) / block_size;
        const u64 parent_size =
            level == 0 ? meta.master_hash_size : tree.info[level - 1].size.Get();
        if (blocks > parent_size / Digest{}.size()) {
            reason = "parent_too_small";
            LOG_ERROR(Loader, "NXBOX VERIFY_ROMFS ABORT reason=parent_too_small level={}",
                      level + 1);
            return;
        }
    }
    if (m_romfs->GetSize() != static_cast<u64>(tree.info[levels - 1].size.Get())) {
        reason = "romfs_size_mismatch";
        LOG_ERROR(Loader, "NXBOX VERIFY_ROMFS ABORT reason=romfs_size_mismatch");
        return;
    }

    ReferenceReader reference{m_context};
    const bool reference_ready =
        reference.Initialize(m_header.patch_info) &&
        (!m_context.indirect_storage ||
         reference.relocation_end == m_context.indirect_storage->GetSize()) &&
        (!m_context.aes_ctr_ex_storage ||
         reference.subsection_end == m_context.aes_ctr_ex_storage->GetSize());
    if (!reference_ready) {
        ++reference_bad;
        LOG_ERROR(Loader, "NXBOX ROMFS_REFERENCE FAIL reason=invalid_flat_tables title={:016X}",
                  m_title_id);
    }
    if (!full && reference_ready) {
        const u64 leaf_start = tree.info[levels - 1].offset.Get();
        const u64 leaf_size = tree.info[levels - 1].size.Get();
        std::set<u64> probes{0, leaf_size > 4096 ? leaf_size - 4096 : 0};
        std::mt19937_64 random{m_title_id ^ leaf_size};
        for (size_t i = 0; i < 16; ++i) {
            probes.insert((random() % ((leaf_size + 4095) / 4096)) * 4096);
        }
        // Include both storage sources and requests crossing relocation boundaries,
        // not just random blocks that may all land in the original storage.
        std::array<u32, 2> source_count{};
        for (const auto& entry : reference.relocations) {
            const u64 start = entry.GetVirtualOffset();
            if (start >= leaf_start && start < leaf_start + leaf_size && entry.storage_index >= 0 &&
                entry.storage_index < 2 && source_count[entry.storage_index]++ < 8) {
                probes.insert(start - leaf_start);
                if (start > leaf_start) {
                    probes.insert(start - leaf_start - 1);
                }
            }
        }
        size_t subsection_probes = 0;
        for (const auto& subsection : reference.subsections) {
            if (subsection_probes >= 8) {
                break;
            }
            const u64 physical = subsection.GetOffset();
            for (size_t i = 0; i < reference.relocations.size(); ++i) {
                const auto& entry = reference.relocations[i];
                if (entry.storage_index != 1 || entry.GetPhysicalOffset() < 0) {
                    continue;
                }
                const u64 start = entry.GetVirtualOffset();
                const u64 end = i + 1 < reference.relocations.size()
                                    ? reference.relocations[i + 1].GetVirtualOffset()
                                    : reference.relocation_end;
                const u64 storage_start = entry.GetPhysicalOffset();
                if (physical >= storage_start && physical - storage_start < end - start) {
                    const u64 mapped = start + physical - storage_start;
                    if (mapped >= leaf_start && mapped < leaf_start + leaf_size) {
                        probes.insert(mapped - leaf_start);
                        if (mapped > leaf_start) {
                            probes.insert(mapped - leaf_start - 1);
                        }
                        ++subsection_probes;
                        break;
                    }
                }
            }
        }
        for (u64 boundary = u64{1} << 32; boundary < leaf_start + leaf_size;
             boundary += u64{1} << 32) {
            if (boundary > leaf_start) {
                probes.insert(boundary - leaf_start - 1);
            }
        }
        std::array<u8, 4096> actual{}, manual{}, raw_bytes{};
        for (u64 position : probes) {
            const size_t size = std::min<u64>(actual.size(), leaf_size - position);
            ++reference_blocks;
            const bool vfs_ok = received->Read(actual.data(), size, position) == size;
            const bool manual_ok =
                reference.Read(manual.data(), size, leaf_start + position) == size;
            const bool raw_ok = raw->Read(raw_bytes.data(), size, leaf_start + position) == size;
            const bool same = vfs_ok && manual_ok &&
                              std::equal(actual.begin(), actual.begin() + size, manual.begin());
            const bool raw_same =
                vfs_ok && raw_ok &&
                std::equal(actual.begin(), actual.begin() + size, raw_bytes.begin());
            if (!same || !raw_same) {
                ++reference_bad;
                if (!first_bad_offset) {
                    first_bad_offset = leaf_start + position;
                    first_bad_level = levels;
                }
                if (reference_bad <= DetailLimit) {
                    LOG_ERROR(
                        Loader,
                        "NXBOX ROMFS_REFERENCE DIFF romfs_offset={:#x} section_offset={:#x} "
                        "size={:#x} vfs_ok={} manual_ok={} raw_ok={} manual_same={} raw_same={}",
                        position, leaf_start + position, size, vfs_ok, manual_ok, raw_ok, same,
                        raw_same);
                    Trace("reference", leaf_start + position, size);
                }
            }
        }
        LOG_INFO(Loader,
                 "NXBOX ROMFS_REFERENCE {} title={:016X} blocks={} bad={} base_spans={} "
                 "update_spans={} relocation_entries={} relocation_end={:#x} "
                 "subsection_entries={} subsection_end={:#x} romfs_section_offset={:#x}",
                 reference_bad == 0 ? "ok" : "FAIL", m_title_id, reference_blocks, reference_bad,
                 reference.base_spans, reference.update_spans, reference.relocations.size(),
                 reference.relocation_end, reference.subsections.size(), reference.subsection_end,
                 leaf_start);
    }

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
        std::set<u64> samples;
        if (leaf && !full) {
            const u64 block_count = (level_size + block_size - 1) / block_size;
            const u64 edge_blocks = (2 * 1024 * 1024 + block_size - 1) / block_size;
            for (u64 i = 0; i < std::min(block_count, edge_blocks); ++i) {
                samples.insert(i * block_size);
                samples.insert((block_count - 1 - i) * block_size);
            }
            // Sample 512 distinct 4 KiB locations, then expand to their complete
            // IVFC blocks. Hashing only 4 KiB would be wrong for larger block orders.
            std::mt19937_64 random{m_title_id ^ level_size};
            const u64 pages = (level_size + 4095) / 4096;
            std::set<u64> random_pages;
            while (random_pages.size() < std::min<u64>(512, pages)) {
                random_pages.insert(random() % pages);
            }
            for (u64 page : random_pages) {
                samples.insert(((page * 4096) / block_size) * block_size);
            }
            // Explicitly exercise section-relative 4 GiB boundaries as well.
            for (u64 boundary = u64{1} << 32; boundary < section_start + level_size;
                 boundary += u64{1} << 32) {
                if (boundary > section_start) {
                    samples.insert(((boundary - section_start - 1) / block_size) * block_size);
                    samples.insert(((boundary - section_start) / block_size) * block_size);
                }
            }
        }
        auto sample = samples.begin();
        for (u64 position = samples.empty() ? 0 : *sample; position < level_size;) {
            const size_t requested =
                std::min<u64>(leaf && !full ? block_size : BatchSize, level_size - position);
            const size_t blocks = (requested + block_size - 1) / block_size;
            std::fill(data.begin(), data.begin() + blocks * block_size, u8{0});
            const auto got = leaf ? m_romfs->Read(data.data(), requested, position)
                                  : raw->Read(data.data(), requested, section_start + position);
            const u64 hash_offset = (position / block_size) * Digest{}.size();
            const size_t hash_bytes = blocks * Digest{}.size();
            std::fill(expected.begin(), expected.begin() + hash_bytes, u8{0});
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
                    reason = "sha256_failed";
                    LOG_ERROR(Loader, "NXBOX VERIFY_ROMFS ABORT reason=sha256_failed");
                    return;
                }
                if (read_ok && std::equal(actual.begin(), actual.end(),
                                          expected.begin() + block * actual.size())) {
                    continue;
                }
                ++mismatches;
                if (!first_bad_offset) {
                    first_bad_offset = section_start + block_offset;
                    first_bad_level = level + 1;
                }
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
                    std::fill(retry.begin(), retry.end(), u8{0});
                    const bool unmerged_ok =
                        m_context.indirect_storage &&
                        m_context.indirect_storage->ReadUnmerged(retry.data(), valid_size,
                                                                 section_offset) == valid_size &&
                        HashBlock(retry.data(), retry.size(), unmerged_hash);
                    std::fill(retry.begin(), retry.end(), u8{0});
                    Digest reference_hash{};
                    const bool reference_ok =
                        reference_ready &&
                        reference.Read(retry.data(), valid_size, section_offset) == valid_size &&
                        HashBlock(retry.data(), retry.size(), reference_hash);
                    LOG_ERROR(
                        Loader,
                        "NXBOX VERIFY_ROMFS MISMATCH level={} data={} block={} "
                        "level_offset={:#x} section_offset={:#x} read_ok={} ancestors_ok={} "
                        "expected={} actual={} raw_retry_matches={} unmerged_matches={} "
                        "raw_retry_same={} reference_ok={} reference_matches={} reference_same={}",
                        level + 1, leaf, block_offset / block_size, block_offset, section_offset,
                        read_ok, ancestors_ok, Common::HexToString(wanted),
                        Common::HexToString(actual), retry_ok && retry_hash == wanted,
                        unmerged_ok && unmerged_hash == wanted, retry_ok && retry_hash == actual,
                        reference_ok, reference_ok && reference_hash == wanted,
                        reference_ok && reference_hash == actual);
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
                    if (!first_bad_offset) {
                        first_bad_offset = section_start + position + 1;
                        first_bad_level = level + 1;
                    }
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
            if (leaf && !full) {
                ++sample;
                position = sample == samples.end() ? level_size : *sample;
            } else {
                position += requested;
            }
        }
        LOG_INFO(Loader, "NXBOX VERIFY_ROMFS LEVEL_END level={} mismatches={} read_errors={}",
                 level + 1, level_mismatches, level_read_errors);
        ancestors_ok &= level_mismatches == 0;
    }
    complete = true;
    reason = "complete";
    LogCounters("verify_end");
    LOG_INFO(Loader,
             "NXBOX VERIFY_ROMFS END title={:016X} complete=true blocks={} mismatches={} "
             "read_errors={} shape_mismatches={} fs_header_hash_ok={} elapsed_ms={} scan={}",
             m_title_id, total_blocks, mismatches, read_errors, shape_mismatches, header_hash_ok,
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - start_time)
                 .count(),
             full ? "full" : "sample");
}

} // namespace FileSys
