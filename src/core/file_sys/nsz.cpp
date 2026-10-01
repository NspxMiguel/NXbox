// SPDX-FileCopyrightText: Copyright 2026 NXbox contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <openssl/evp.h>
#include <zstd.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "core/crypto/aes_util.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/nsz.h"
#include "core/file_sys/vfs/vfs.h"

namespace FileSys {

namespace {

// PFS0 layout: a 0x10 byte header, `num_entries` entries of 0x18 bytes, a string table, and then
// the file data. Entry offsets are relative to the end of the string table.
constexpr u64 Pfs0HeaderSize = 0x10;
constexpr u64 Pfs0EntrySize = 0x18;
constexpr u64 MaxPfs0Entries = 0x10000;
constexpr u64 MaxPfs0MetadataSize = 0x4000000;
// Bytes allowed between the end of the string table and the first file. nsz keeps the input's
// padding, real NSPs have at most a few KiB of it.
constexpr u64 MaxPfs0DataPadding = 0x1000000;

// An NCZ starts with the first 0x4000 bytes of the original NCA, copied verbatim (NCA header, FS
// headers and the padding up to the first section). The "NCZSECTN" magic and the u64 section count
// follow, then `count` section entries of 0x40 bytes. An optional "NCZBLOCK" header comes after
// the table, then the compressed data.
constexpr u64 NczHeaderCopySize = 0x4000;
constexpr u64 NczSectionTableHeaderSize = 0x10;
constexpr u64 NczSectionEntrySize = 0x40;
// BKTR (update) NCAs get one section per subsection, so the count can be large.
constexpr u64 NczMaxSections = 0x100000;
constexpr u64 NczBlockHeaderSize = 0x18;
constexpr u32 NczMinBlockSizeExponent = 14;
constexpr u32 NczMaxBlockSizeExponent = 32;

// Section crypto types that nsz decrypts when it compresses, and so encrypts again when it
// decompresses. Every other type is stored as it is in the NCA.
constexpr u64 NczCryptoTypeCtr = 3;
constexpr u64 NczCryptoTypeBktr = 4;

// Size of the read, decompression and write buffers.
constexpr std::size_t IoChunkSize = 0x100000;
// Room kept in front of the data in the work buffer, see Converter::Encrypt.
constexpr std::size_t AesHeadroom = 0x10;
// The progress callback is called at least once per this many output bytes.
constexpr u64 ProgressStep = 0x800000;

// Checks every NCA that is rebuilt against the NCA ID in its name, like nsz does.
constexpr bool VerifyNcaIds = true;

static_assert(NczHeaderCopySize <= IoChunkSize, "The NCZ header copy must fit in one chunk");

u32 ReadLE32(const u8* data) {
    return static_cast<u32>(data[0]) | (static_cast<u32>(data[1]) << 8) |
           (static_cast<u32>(data[2]) << 16) | (static_cast<u32>(data[3]) << 24);
}

u64 ReadLE64(const u8* data) {
    return static_cast<u64>(ReadLE32(data)) | (static_cast<u64>(ReadLE32(data + 4)) << 32);
}

void WriteLE32(u8* data, u32 value) {
    for (std::size_t i = 0; i < sizeof(u32); ++i) {
        data[i] = static_cast<u8>(value & 0xFF);
        value >>= 8;
    }
}

void WriteLE64(u8* data, u64 value) {
    for (std::size_t i = 0; i < sizeof(u64); ++i) {
        data[i] = static_cast<u8>(value & 0xFF);
        value >>= 8;
    }
}

char ToLowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool IsNczName(const std::string& name) {
    constexpr std::string_view suffix = ".ncz";
    if (name.size() < suffix.size()) {
        return false;
    }
    const std::size_t start = name.size() - suffix.size();
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        if (ToLowerAscii(name[start + i]) != suffix[i]) {
            return false;
        }
    }
    return true;
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// An NCA entry is named after its NCA ID, the first 16 bytes of the SHA-256 of the file, in hex.
std::optional<std::array<u8, 0x10>> NcaIdFromName(const std::string& name) {
    constexpr std::size_t id_chars = 0x20;
    if (name.size() != id_chars + 4 || name.compare(id_chars, 4, ".nca") != 0) {
        return std::nullopt;
    }
    std::array<u8, 0x10> id{};
    for (std::size_t i = 0; i < id.size(); ++i) {
        const int high = HexValue(name[i * 2]);
        const int low = HexValue(name[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        id[i] = static_cast<u8>((high << 4) | low);
    }
    return id;
}

bool Fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

// Reads exactly `size` bytes at `offset`, without ever reading past `file_size`. Not every VfsFile
// is safe to read out of bounds.
bool ReadExact(const VirtualFile& file, u64 file_size, u8* dst, std::size_t size, u64 offset) {
    if (offset > file_size || size > file_size - offset) {
        return false;
    }
    std::size_t done = 0;
    while (done < size) {
        const std::size_t got =
            file->Read(dst + done, size - done, static_cast<std::size_t>(offset + done));
        if (got == 0) {
            return false;
        }
        done += got;
    }
    return true;
}

// Incremental SHA-256. It disables itself on any OpenSSL failure, as the hash is only a check.
class Sha256 {
public:
    Sha256() : context(EVP_MD_CTX_new()) {
        if (context != nullptr && EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
            Release();
        }
    }

    ~Sha256() {
        Release();
    }

    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    void Update(const u8* data, std::size_t size) {
        if (context != nullptr && EVP_DigestUpdate(context, data, size) != 1) {
            Release();
        }
    }

    bool Finish(std::array<u8, 0x20>* digest) {
        if (context == nullptr) {
            return false;
        }
        unsigned int length = 0;
        const bool ok = EVP_DigestFinal_ex(context, digest->data(), &length) == 1 &&
                        length == digest->size();
        Release();
        return ok;
    }

private:
    void Release() {
        EVP_MD_CTX_free(context);
        context = nullptr;
    }

    EVP_MD_CTX* context;
};

struct Pfs0Entry {
    std::string name;
    u64 offset = 0; // Absolute offset of the data in the file.
    u64 size = 0;
};

struct Pfs0Info {
    std::vector<Pfs0Entry> entries;
    u64 metadata_size = 0; // Header, entry table and string table.
    u64 string_table_size = 0;
    u64 data_padding = 0; // Offset of the first entry's data, relative to the end of the metadata.
};

// Parses the header, the entry table and the string table of a PFS0. With `check_data` it also
// requires the data of every entry to lie inside the file.
bool ParsePfs0(const VirtualFile& file, u64 file_size, bool check_data, Pfs0Info* info,
               std::string* error) {
    std::array<u8, Pfs0HeaderSize> header{};
    if (file_size < header.size() ||
        !ReadExact(file, file_size, header.data(), header.size(), 0)) {
        return Fail(error, "the file is too small to be a PFS0 container");
    }
    if (std::memcmp(header.data(), "HFS0", 4) == 0) {
        return Fail(error, "HFS0 containers (XCZ and XCI) are not supported");
    }
    if (std::memcmp(header.data(), "PFS0", 4) != 0) {
        return Fail(error, "the file is not a PFS0 container");
    }

    const u64 num_entries = ReadLE32(header.data() + 4);
    const u64 string_table_size = ReadLE32(header.data() + 8);
    if (num_entries == 0 || num_entries > MaxPfs0Entries) {
        return Fail(error, fmt::format("invalid PFS0 entry count {}", num_entries));
    }
    const u64 metadata_size = Pfs0HeaderSize + num_entries * Pfs0EntrySize + string_table_size;
    if (metadata_size > file_size || metadata_size > MaxPfs0MetadataSize) {
        return Fail(error, "the PFS0 header is truncated or too large");
    }

    std::vector<u8> metadata(static_cast<std::size_t>(metadata_size));
    if (!ReadExact(file, file_size, metadata.data(), metadata.size(), 0)) {
        return Fail(error, "cannot read the PFS0 header");
    }
    const u8* const entry_table = metadata.data() + Pfs0HeaderSize;
    const char* const string_table =
        reinterpret_cast<const char*>(entry_table + num_entries * Pfs0EntrySize);

    info->entries.clear();
    info->entries.reserve(static_cast<std::size_t>(num_entries));
    info->metadata_size = metadata_size;
    info->string_table_size = string_table_size;
    info->data_padding = 0;
    for (u64 i = 0; i < num_entries; ++i) {
        const u8* const raw = entry_table + i * Pfs0EntrySize;
        const u64 relative_offset = ReadLE64(raw);
        const u64 size = ReadLE64(raw + 8);
        const u64 name_offset = ReadLE32(raw + 16);

        if (name_offset >= string_table_size) {
            return Fail(error, fmt::format("entry {} has its name outside the string table", i));
        }
        // The name ends at its terminator, or at the end of the string table.
        const std::size_t max_name_size = static_cast<std::size_t>(string_table_size - name_offset);
        const char* const name = string_table + name_offset;
        const void* const terminator = std::memchr(name, 0, max_name_size);
        std::size_t name_size = max_name_size;
        if (terminator != nullptr) {
            name_size = static_cast<std::size_t>(static_cast<const char*>(terminator) - name);
        }

        Pfs0Entry entry;
        entry.name.assign(name, name_size);
        if (relative_offset > (std::numeric_limits<u64>::max)() - metadata_size) {
            return Fail(error, fmt::format("entry {} ({}) has an invalid offset", i, entry.name));
        }
        entry.offset = metadata_size + relative_offset;
        entry.size = size;
        if (check_data && (entry.offset > file_size || entry.size > file_size - entry.offset)) {
            return Fail(error,
                        fmt::format("entry {} ({}) lies outside the file, is it truncated?", i,
                                    entry.name));
        }
        if (i == 0) {
            info->data_padding = relative_offset;
        }
        info->entries.push_back(std::move(entry));
    }
    return true;
}

struct NczSection {
    u64 offset = 0; // Absolute offset in the NCA.
    u64 size = 0;
    u64 crypto_type = 0;
    std::array<u8, 0x10> key{};
    std::array<u8, 0x10> counter{};
};

struct NczInfo {
    // The crypto sections, in NCA order. They tile the NCA from the first section to its end.
    std::vector<NczSection> sections;
    // Size of the NCA that is rebuilt: the end of the last section.
    u64 nca_size = 0;

    // Compressed data: one zstd stream, or the independent zstd blocks of an NCZBLOCK header.
    u64 payload_offset = 0; // Absolute offset in the NSZ.
    u64 payload_size = 0;
    bool block_mode = false;
    u64 block_size = 0;
    u64 decompressed_size = 0;
    std::vector<u32> block_sizes; // Compressed size of every block.
};

// Reads the headers of the NCZ stored in `entry` and checks that they are consistent. The data is
// not touched.
bool ParseNcz(const VirtualFile& file, u64 file_size, const Pfs0Entry& entry, NczInfo* info,
              std::string* error) {
    constexpr u64 table_offset = NczHeaderCopySize + NczSectionTableHeaderSize;
    if (entry.size < table_offset) {
        return Fail(error, "too small to be an NCZ");
    }

    std::array<u8, NczSectionTableHeaderSize> table_header{};
    if (!ReadExact(file, file_size, table_header.data(), table_header.size(),
                   entry.offset + NczHeaderCopySize)) {
        return Fail(error, "cannot read the section table header");
    }
    if (std::memcmp(table_header.data(), "NCZSECTN", 8) != 0) {
        return Fail(error, "no NCZSECTN magic, is this really an NCZ?");
    }
    const u64 section_count = ReadLE64(table_header.data() + 8);
    if (section_count == 0 || section_count > NczMaxSections ||
        section_count > (entry.size - table_offset) / NczSectionEntrySize) {
        return Fail(error, fmt::format("invalid section count {}", section_count));
    }

    std::vector<u8> table(static_cast<std::size_t>(section_count * NczSectionEntrySize));
    if (!ReadExact(file, file_size, table.data(), table.size(), entry.offset + table_offset)) {
        return Fail(error, "cannot read the section table");
    }
    info->sections.clear();
    info->sections.reserve(static_cast<std::size_t>(section_count));
    for (u64 i = 0; i < section_count; ++i) {
        const u8* const raw = table.data() + i * NczSectionEntrySize;
        NczSection section;
        section.offset = ReadLE64(raw);
        section.size = ReadLE64(raw + 8);
        section.crypto_type = ReadLE64(raw + 16);
        // raw + 24: 8 bytes of padding.
        std::memcpy(section.key.data(), raw + 32, section.key.size());
        std::memcpy(section.counter.data(), raw + 48, section.counter.size());
        info->sections.push_back(section);
    }

    // nsz only compresses NCAs whose sections are contiguous and end at the end of the NCA, so the
    // NCA is as large as the end of its last section. The decompressed data holds every byte from
    // 0x4000 on: the gap between the header copy and the first section, if there is one, and then
    // the sections. When the first section starts inside the header copy, its first bytes are not
    // part of the data.
    const NczSection& first = info->sections.front();
    u64 end = first.offset;
    for (const NczSection& section : info->sections) {
        if (section.offset != end) {
            return Fail(error, "the sections are not contiguous");
        }
        if (section.size > (std::numeric_limits<u64>::max)() - end) {
            return Fail(error, "the section sizes overflow");
        }
        end += section.size;
    }
    const u64 skipped = first.offset < NczHeaderCopySize ? NczHeaderCopySize - first.offset : 0;
    if (skipped > first.size) {
        return Fail(error, "the first section ends inside the NCA header");
    }
    info->nca_size = end;
    const u64 stream_size = end - NczHeaderCopySize;

    const u64 payload_start = table_offset + section_count * NczSectionEntrySize;
    const u64 available = entry.size - payload_start;
    if (available == 0) {
        return Fail(error, "there is no compressed data");
    }

    info->block_mode = false;
    info->block_sizes.clear();
    std::array<u8, 8> block_magic{};
    if (available >= block_magic.size() &&
        ReadExact(file, file_size, block_magic.data(), block_magic.size(),
                  entry.offset + payload_start) &&
        std::memcmp(block_magic.data(), "NCZBLOCK", 8) == 0) {
        info->block_mode = true;
    }

    if (!info->block_mode) {
        info->payload_offset = entry.offset + payload_start;
        info->payload_size = available;
        return true;
    }

    // NCZBLOCK header: magic (8), version (1), type (1), unused (1), block size exponent (1),
    // block count (4), decompressed size (8), then one u32 compressed size per block. Block `i`
    // holds bytes [i << exponent, (i + 1) << exponent) of the decompressed data. A block whose
    // compressed size is not smaller than its decompressed size is stored uncompressed.
    if (available < NczBlockHeaderSize) {
        return Fail(error, "the NCZBLOCK header is truncated");
    }
    std::array<u8, NczBlockHeaderSize> block_header{};
    if (!ReadExact(file, file_size, block_header.data(), block_header.size(),
                   entry.offset + payload_start)) {
        return Fail(error, "cannot read the NCZBLOCK header");
    }
    const u32 exponent = block_header[11];
    const u64 block_count = ReadLE32(block_header.data() + 12);
    const u64 decompressed_size = ReadLE64(block_header.data() + 16);
    if (exponent < NczMinBlockSizeExponent || exponent > NczMaxBlockSizeExponent) {
        return Fail(error, fmt::format("invalid NCZBLOCK block size exponent {}", exponent));
    }
    const u64 block_size = u64{1} << exponent;
    if (decompressed_size != stream_size) {
        return Fail(error, "the NCZBLOCK decompressed size does not match the section table");
    }
    const u64 expected_blocks =
        decompressed_size / block_size + (decompressed_size % block_size != 0 ? u64{1} : u64{0});
    if (block_count != expected_blocks) {
        return Fail(error, "the NCZBLOCK block count does not match the decompressed size");
    }
    if (block_count > (available - NczBlockHeaderSize) / sizeof(u32)) {
        return Fail(error, "the NCZBLOCK block table is truncated");
    }

    const u64 block_table_size = block_count * sizeof(u32);
    std::vector<u8> block_table(static_cast<std::size_t>(block_table_size));
    if (!ReadExact(file, file_size, block_table.data(), block_table.size(),
                   entry.offset + payload_start + NczBlockHeaderSize)) {
        return Fail(error, "cannot read the NCZBLOCK block table");
    }
    const u64 payload_size = available - NczBlockHeaderSize - block_table_size;
    info->block_sizes.reserve(static_cast<std::size_t>(block_count));
    u64 compressed_total = 0;
    for (u64 i = 0; i < block_count; ++i) {
        const u32 compressed_size = ReadLE32(block_table.data() + i * sizeof(u32));
        compressed_total += compressed_size;
        if (compressed_total > payload_size) {
            return Fail(error, "the NCZBLOCK blocks extend past the end of the entry");
        }
        info->block_sizes.push_back(compressed_size);
    }
    info->payload_offset = entry.offset + payload_start + NczBlockHeaderSize + block_table_size;
    info->payload_size = payload_size;
    info->block_size = block_size;
    info->decompressed_size = decompressed_size;
    return true;
}

struct ZstdContextDeleter {
    void operator()(ZSTD_DCtx* context) const {
        static_cast<void>(ZSTD_freeDCtx(context));
    }
};

// Streams the zstd frames stored in a byte range of the NSZ, `Read` returning the next bytes of
// the decompressed data. Frames that follow each other are decoded as one stream.
class ZstdReader {
public:
    ZstdReader(const VirtualFile& file_, u64 file_size_)
        : file(file_), file_size(file_size_), context(ZSTD_createDCtx()), buffer(IoChunkSize) {}

    bool IsValid() const {
        return context != nullptr;
    }

    // Starts decoding at [offset, offset + size) of the file, dropping any previous state.
    bool Open(u64 offset, u64 size, std::string* error) {
        if (ZSTD_isError(ZSTD_DCtx_reset(context.get(), ZSTD_reset_session_only)) != 0) {
            return Fail(error, "cannot reset the zstd decoder");
        }
        input = ZSTD_inBuffer{buffer.data(), 0, 0};
        next_offset = offset;
        remaining = size;
        return true;
    }

    // Produces exactly `size` bytes, or fails.
    bool Read(u8* dst, std::size_t size, std::string* error) {
        ZSTD_outBuffer output{dst, size, 0};
        while (output.pos < size) {
            if (input.pos == input.size && remaining != 0 && !Refill(error)) {
                return false;
            }
            const std::size_t input_before = input.pos;
            const std::size_t output_before = output.pos;
            const std::size_t hint = ZSTD_decompressStream(context.get(), &output, &input);
            if (ZSTD_isError(hint) != 0) {
                return Fail(error, fmt::format("zstd error: {}", ZSTD_getErrorName(hint)));
            }
            if (input.pos == input_before && output.pos == output_before) {
                // Nothing was consumed and nothing was produced although there is room, and no
                // more compressed data is left to feed.
                return Fail(error, "the compressed data ends too early");
            }
        }
        return true;
    }

private:
    bool Refill(std::string* error) {
        const std::size_t count = static_cast<std::size_t>(std::min<u64>(remaining, buffer.size()));
        if (!ReadExact(file, file_size, buffer.data(), count, next_offset)) {
            return Fail(error, fmt::format("cannot read the compressed data at offset {:#x}",
                                           next_offset));
        }
        input = ZSTD_inBuffer{buffer.data(), count, 0};
        next_offset += count;
        remaining -= count;
        return true;
    }

    VirtualFile file;
    u64 file_size;
    std::unique_ptr<ZSTD_DCtx, ZstdContextDeleter> context;
    std::vector<u8> buffer;
    ZSTD_inBuffer input{nullptr, 0, 0};
    u64 next_offset = 0;
    u64 remaining = 0;
};

// The decompressed data of one NCZ, whether it is a single zstd stream or independent blocks.
class NczPayloadReader {
public:
    NczPayloadReader(const VirtualFile& file_, u64 file_size_)
        : file(file_), file_size(file_size_), zstd(file_, file_size_) {}

    bool IsValid() const {
        return zstd.IsValid();
    }

    bool Open(const NczInfo& ncz_info, std::string* error) {
        info = &ncz_info;
        block_index = 0;
        block_remaining = 0;
        block_raw = false;
        block_offset = ncz_info.payload_offset;
        if (!ncz_info.block_mode) {
            return zstd.Open(ncz_info.payload_offset, ncz_info.payload_size, error);
        }
        return true;
    }

    // Produces exactly `size` more bytes of the decompressed data, or fails.
    bool Read(u8* dst, std::size_t size, std::string* error) {
        if (!info->block_mode) {
            return zstd.Read(dst, size, error);
        }
        while (size != 0) {
            if (block_remaining == 0 && !OpenNextBlock(error)) {
                return false;
            }
            const std::size_t count =
                static_cast<std::size_t>(std::min<u64>(size, block_remaining));
            if (block_raw) {
                if (!ReadExact(file, file_size, dst, count, raw_offset)) {
                    return Fail(error, fmt::format("cannot read the stored block at offset {:#x}",
                                                   raw_offset));
                }
                raw_offset += count;
            } else if (!zstd.Read(dst, count, error)) {
                return false;
            }
            block_remaining -= count;
            dst += count;
            size -= count;
        }
        return true;
    }

private:
    bool OpenNextBlock(std::string* error) {
        if (block_index >= info->block_sizes.size()) {
            return Fail(error, "the decompressed data ends too early");
        }
        // Every block decompresses to `block_size` bytes except for the last, which holds the rest.
        const bool is_last = block_index + 1 == info->block_sizes.size();
        const u64 tail = info->decompressed_size % info->block_size;
        const u64 decompressed_size = (is_last && tail != 0) ? tail : info->block_size;
        const u64 compressed_size = info->block_sizes[block_index];

        // A block that did not get smaller is stored as it is.
        block_raw = compressed_size >= decompressed_size;
        if (block_raw) {
            raw_offset = block_offset;
        } else if (!zstd.Open(block_offset, compressed_size, error)) {
            return false;
        }
        block_offset += compressed_size;
        block_remaining = decompressed_size;
        ++block_index;
        return true;
    }

    VirtualFile file;
    u64 file_size;
    ZstdReader zstd;
    const NczInfo* info = nullptr;
    std::size_t block_index = 0;
    u64 block_remaining = 0; // Decompressed bytes left in the current block.
    u64 block_offset = 0;    // Absolute offset in the NSZ of the next block.
    bool block_raw = false;
    u64 raw_offset = 0;
};

// One entry of the NSP that is written.
struct OutEntry {
    std::string name;
    std::string source_name;
    u64 size = 0;
    u64 offset = 0; // Absolute offset in the NSP.
    u64 source_offset = 0;
    bool is_ncz = false;
    NczInfo ncz;
};

class Converter {
public:
    Converter(const VirtualFile& nsz_, const VirtualFile& out_,
              std::function<void(u64, u64)> progress_, const std::atomic<bool>* cancel_)
        : nsz(nsz_), out(out_), nsz_size(nsz_->GetSize()), progress(std::move(progress_)),
          cancel(cancel_), payload(nsz, nsz_size), chunk(IoChunkSize + AesHeadroom) {}

    bool Run() {
        if (!payload.IsValid()) {
            return Fail("cannot create the zstd decoder");
        }
        Pfs0Info pfs;
        std::string pfs_error;
        if (!ParsePfs0(nsz, nsz_size, true, &pfs, &pfs_error)) {
            return Fail(std::move(pfs_error));
        }
        if (!Plan(pfs) || !PrepareOutput()) {
            return false;
        }

        LOG_INFO(Loader, "NSZ: converting {} entries into an NSP of {} bytes", entries.size(),
                 total_size);
        ReportProgress(true);
        if (!WriteHeader()) {
            return false;
        }
        for (const OutEntry& entry : entries) {
            if (entry.is_ncz) {
                if (!ConvertNcz(entry)) {
                    return false;
                }
            } else if (!CopyEntry(entry)) {
                return false;
            }
        }
        if (done != total_size) {
            return Fail(fmt::format("wrote {} bytes, expected {}", done, total_size));
        }
        ReportProgress(true);
        LOG_INFO(Loader, "NSZ: conversion finished");
        return true;
    }

    const std::string& GetError() const {
        return error;
    }

private:
    bool Fail(std::string message) {
        error = std::move(message);
        return false;
    }

    bool Cancelled() {
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
            Fail("cancelled");
            return true;
        }
        return false;
    }

    // Names, sizes and offsets of the NSP. Every `.ncz` entry becomes an `.nca` entry whose size
    // is the size of the NCA, and the other entries keep theirs. The data is packed in the order
    // of the entry table, after the same amount of padding as in the input.
    bool Plan(const Pfs0Info& pfs) {
        if (pfs.data_padding > MaxPfs0DataPadding) {
            return Fail("the PFS0 header has too much padding");
        }
        entries.clear();
        entries.reserve(pfs.entries.size());
        u64 names_size = 0;
        bool has_ncz = false;
        for (const Pfs0Entry& source : pfs.entries) {
            OutEntry entry;
            entry.name = source.name;
            entry.source_name = source.name;
            entry.size = source.size;
            entry.source_offset = source.offset;
            if (IsNczName(source.name)) {
                std::string ncz_error;
                if (!ParseNcz(nsz, nsz_size, source, &entry.ncz, &ncz_error)) {
                    return Fail(fmt::format("{}: {}", source.name, ncz_error));
                }
                entry.is_ncz = true;
                entry.name = source.name.substr(0, source.name.size() - 4) + ".nca";
                entry.size = entry.ncz.nca_size;
                has_ncz = true;
            }
            names_size += entry.name.size() + 1;
            entries.push_back(std::move(entry));
        }
        if (!has_ncz) {
            return Fail("the file has no .ncz entries, it is already a plain NSP");
        }

        // The string table keeps its size (an `.ncz` and an `.nca` name are equally long), unless
        // it was too small for the names to begin with.
        string_table_size = std::max(pfs.string_table_size, names_size);
        metadata_size = Pfs0HeaderSize + entries.size() * Pfs0EntrySize + string_table_size;
        if (metadata_size > MaxPfs0MetadataSize) {
            return Fail("the PFS0 header would be too large");
        }
        data_start = metadata_size + pfs.data_padding;

        u64 offset = data_start;
        for (OutEntry& entry : entries) {
            if (entry.size > (std::numeric_limits<u64>::max)() - offset) {
                return Fail("the NSP would be too large");
            }
            entry.offset = offset;
            offset += entry.size;
        }
        total_size = offset;
        return true;
    }

    bool PrepareOutput() {
        // Writing starts from scratch. Do not grow the file ahead of the data either: on Windows
        // Resize() is _chsize_s (see Common::FS::IOFile::SetSize), which pads with zero bytes.
        if (out->GetSize() != 0 && !out->Resize(0)) {
            return Fail("cannot truncate the output file");
        }
        return true;
    }

    bool WriteHeader() {
        // Everything up to the first entry's data is written at once; the padding stays zero.
        std::vector<u8> header(static_cast<std::size_t>(data_start), 0);
        std::memcpy(header.data(), "PFS0", 4);
        WriteLE32(header.data() + 4, static_cast<u32>(entries.size()));
        WriteLE32(header.data() + 8, static_cast<u32>(string_table_size));
        u8* const entry_table = header.data() + Pfs0HeaderSize;
        u8* const string_table = entry_table + entries.size() * Pfs0EntrySize;
        u32 name_offset = 0;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const OutEntry& entry = entries[i];
            u8* const raw = entry_table + i * Pfs0EntrySize;
            WriteLE64(raw, entry.offset - metadata_size);
            WriteLE64(raw + 8, entry.size);
            WriteLE32(raw + 16, name_offset);
            std::memcpy(string_table + name_offset, entry.name.data(), entry.name.size());
            name_offset += static_cast<u32>(entry.name.size() + 1);
        }
        return WriteOut(header.data(), header.size(), 0);
    }

    bool CopyEntry(const OutEntry& entry) {
        u8* const data = chunk.data() + AesHeadroom;
        u64 copied = 0;
        while (copied < entry.size) {
            if (Cancelled()) {
                return false;
            }
            const std::size_t count =
                static_cast<std::size_t>(std::min<u64>(entry.size - copied, IoChunkSize));
            if (!ReadSource(data, count, entry.source_offset + copied) ||
                !WriteOut(data, count, entry.offset + copied)) {
                return false;
            }
            copied += count;
        }
        return true;
    }

    // Rebuilds the NCA of an NCZ entry: the header copy as it is, then the decompressed data,
    // encrypted again section by section.
    bool ConvertNcz(const OutEntry& entry) {
        const NczInfo& info = entry.ncz;
        LOG_INFO(Loader, "NSZ: {} -> {} ({} bytes, {} sections, {})", entry.source_name,
                 entry.name, entry.size, info.sections.size(),
                 info.block_mode ? "blocks" : "one stream");
        u8* const data = chunk.data() + AesHeadroom;
        Sha256 hasher;

        if (!ReadSource(data, static_cast<std::size_t>(NczHeaderCopySize), entry.source_offset)) {
            return false;
        }
        hasher.Update(data, static_cast<std::size_t>(NczHeaderCopySize));
        if (!WriteOut(data, static_cast<std::size_t>(NczHeaderCopySize), entry.offset)) {
            return false;
        }

        std::string payload_error;
        if (!payload.Open(info, &payload_error)) {
            return Fail(fmt::format("{}: {}", entry.name, payload_error));
        }

        // `position` is the offset in the NCA of the next byte of the decompressed data.
        u64 position = NczHeaderCopySize;
        const u64 first_offset = info.sections.front().offset;
        if (first_offset > position) {
            // Bytes between the header copy and the first section are not encrypted.
            if (!ProcessSegment(entry, nullptr, position, first_offset - position, hasher)) {
                return false;
            }
            position = first_offset;
        }
        for (const NczSection& section : info.sections) {
            u64 start = section.offset;
            u64 length = section.size;
            if (start < position) {
                // The first section starts inside the header copy, which is already written.
                const u64 skipped = position - start;
                start += skipped;
                length -= skipped;
            }
            if (start != position) {
                return Fail(fmt::format("{}: internal error, the sections are not contiguous",
                                        entry.name));
            }
            const bool encrypted = section.crypto_type == NczCryptoTypeCtr ||
                                   section.crypto_type == NczCryptoTypeBktr;
            if (!ProcessSegment(entry, encrypted ? &section : nullptr, start, length, hasher)) {
                return false;
            }
            position = start + length;
        }
        if (position != info.nca_size) {
            return Fail(fmt::format("{}: internal error, wrote {} bytes of the NCA instead of {}",
                                    entry.name, position, info.nca_size));
        }

        if constexpr (VerifyNcaIds) {
            std::array<u8, 0x20> digest{};
            const auto nca_id = NcaIdFromName(entry.name);
            if (nca_id && hasher.Finish(&digest)) {
                if (std::equal(nca_id->begin(), nca_id->end(), digest.begin())) {
                    LOG_INFO(Loader, "NSZ: {} verified, its SHA-256 matches the NCA ID",
                             entry.name);
                } else {
                    LOG_WARNING(Loader,
                                "NSZ: {} does not match its NCA ID, the source may be corrupt",
                                entry.name);
                }
            }
        }
        return true;
    }

    // Moves `length` bytes of decompressed data that start at offset `start` of the NCA to the
    // output, encrypting them with the crypto of `section` unless it is null.
    bool ProcessSegment(const OutEntry& entry, const NczSection* section, u64 start, u64 length,
                        Sha256& hasher) {
        u8* const data = chunk.data() + AesHeadroom;
        u64 position = start;
        u64 remaining = length;
        while (remaining != 0) {
            if (Cancelled()) {
                return false;
            }
            const std::size_t count =
                static_cast<std::size_t>(std::min<u64>(remaining, IoChunkSize));
            std::string read_error;
            if (!payload.Read(data, count, &read_error)) {
                return Fail(fmt::format("{}: {} (at offset {:#x} of the NCA)", entry.name,
                                        read_error, position));
            }
            if (section != nullptr) {
                Encrypt(*section, position, data, count);
            }
            hasher.Update(data, count);
            if (!WriteOut(data, count, entry.offset + position)) {
                return false;
            }
            position += count;
            remaining -= count;
        }
        return true;
    }

    // AES-128-CTR, in place. The counter is the section's counter with its last 8 bytes replaced by
    // the big-endian value of (offset in the NCA >> 4), which is nsz's AESCTR.seek(offset).
    // `data` must have AesHeadroom bytes of valid memory in front of it.
    void Encrypt(const NczSection& section, u64 position, u8* data, std::size_t size) {
        if (!cipher || cipher_key != section.key) {
            cipher.emplace(section.key, Core::Crypto::Mode::CTR);
            cipher_key = section.key;
        }

        std::array<u8, 0x10> counter = section.counter;
        u64 block = position >> 4;
        for (std::size_t i = 0; i < 8; ++i) {
            counter[counter.size() - 1 - i] = static_cast<u8>(block & 0xFF);
            block >>= 8;
        }
        cipher->SetIV(counter);

        // A start that is not 16 byte aligned (real NCAs never have one) continues in the middle
        // of a keystream block: encrypt zeros in front of the data to skip to the right byte.
        const std::size_t skip = static_cast<std::size_t>(position & 0xF);
        u8* const start = data - skip;
        if (skip != 0) {
            std::memset(start, 0, skip);
        }
        cipher->Transcode(start, size + skip, start, Core::Crypto::Op::Encrypt);
    }

    bool ReadSource(u8* dst, std::size_t size, u64 offset) {
        if (!ReadExact(nsz, nsz_size, dst, size, offset)) {
            return Fail(fmt::format("cannot read {} bytes at offset {:#x}, is the file truncated?",
                                    size, offset));
        }
        return true;
    }

    bool WriteOut(const u8* data, std::size_t size, u64 offset) {
        if (out->Write(data, size, static_cast<std::size_t>(offset)) != size) {
            return Fail(fmt::format("cannot write {} bytes at offset {:#x}, is the disk full?",
                                    size, offset));
        }
        done += size;
        ReportProgress(false);
        return true;
    }

    void ReportProgress(bool force) {
        if (!progress || (!force && done - last_reported < ProgressStep)) {
            return;
        }
        last_reported = done;
        progress(done, total_size);
    }

    VirtualFile nsz;
    VirtualFile out;
    u64 nsz_size;
    std::function<void(u64, u64)> progress;
    const std::atomic<bool>* cancel;
    NczPayloadReader payload;
    std::vector<u8> chunk;

    std::optional<Core::Crypto::AESCipher<Core::Crypto::Key128>> cipher;
    Core::Crypto::Key128 cipher_key{};

    std::vector<OutEntry> entries;
    u64 string_table_size = 0;
    u64 metadata_size = 0;
    u64 data_start = 0;
    u64 total_size = 0;
    u64 done = 0;
    u64 last_reported = 0;
    std::string error;
};

} // Anonymous namespace

bool IsNsz(const VirtualFile& file) {
    if (file == nullptr || !file->IsReadable()) {
        return false;
    }
    try {
        Pfs0Info info;
        std::string ignored;
        // A truncated file is still an NSZ; converting it reports that it is truncated.
        if (!ParsePfs0(file, file->GetSize(), false, &info, &ignored)) {
            return false;
        }
        return std::any_of(info.entries.begin(), info.entries.end(),
                           [](const Pfs0Entry& entry) { return IsNczName(entry.name); });
    } catch (const std::exception&) {
        return false;
    }
}

bool ConvertNszToNsp(const VirtualFile& nsz, const VirtualFile& out_nsp,
                     std::function<void(u64 done, u64 total)> progress, std::string* error,
                     const std::atomic<bool>* cancel) {
    std::string message;
    try {
        if (nsz == nullptr || out_nsp == nullptr) {
            message = "there is no input or output file";
        } else if (nsz == out_nsp) {
            message = "the input and the output are the same file";
        } else if (!nsz->IsReadable()) {
            message = "the input file is not readable";
        } else if (!out_nsp->IsWritable()) {
            message = "the output file is not writable";
        } else {
            Converter converter(nsz, out_nsp, std::move(progress), cancel);
            if (converter.Run()) {
                if (error != nullptr) {
                    error->clear();
                }
                return true;
            }
            message = converter.GetError();
        }
    } catch (const std::exception& e) {
        message = fmt::format("unexpected error: {}", e.what());
    } catch (...) {
        message = "unexpected error";
    }

    message = "NSZ: " + message;
    LOG_ERROR(Loader, "{}", message);
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

} // namespace FileSys
