// SPDX-FileCopyrightText: Copyright 2026 NXbox contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <zstd.h>

#include "common/common_types.h"
#include "common/zstd_compression.h"
#include "core/crypto/aes_util.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/nsz.h"
#include "core/file_sys/partition_filesystem.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/loader.h"

// These tests build small NSZ files in memory, so no game data is needed. The expected NCAs are
// encrypted here with an AES-ECB keystream instead of AES-CTR, so they do not share the converter's
// counter handling.

namespace {

using Core::Crypto::Key128;
using Counter = std::array<u8, 0x10>;

constexpr u64 HeaderCopySize = 0x4000;
constexpr std::size_t NoDifference = ~std::size_t{0};

struct Section {
    u64 offset;
    u64 size;
    u64 crypto_type;
    Key128 key;
    Counter counter;
};

struct Nca {
    std::vector<u8> encrypted; // The NCA the way an NSP stores it.
    std::vector<u8> decrypted; // The same NCA with its AES-CTR sections decrypted.
};

struct Entry {
    std::string name;
    std::vector<u8> data;
};

const Key128 KeyA{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const Key128 KeyB{0xF0, 0xE1, 0xD2, 0xC3, 0xB4, 0xA5, 0x96, 0x87,
                  0x78, 0x69, 0x5A, 0x4B, 0x3C, 0x2D, 0x1E, 0x0F};
// Only the first 8 bytes of a counter matter, the rest is replaced by the offset.
const Counter CounterA{0x00, 0x00, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44, 0xFF, 0xFF, 0xFF, 0xFF,
                       0xFF, 0xFF, 0xFF, 0xFF};
const Counter CounterB{0x00, 0x00, 0x00, 0x07, 0x55, 0x66, 0x77, 0x88, 0x00, 0x00, 0x00, 0x00,
                       0x00, 0x00, 0x00, 0x00};

std::vector<u8> PatternBytes(std::size_t size, u64 seed, bool compressible) {
    std::vector<u8> data(size);
    u64 state = seed * 0x9E3779B97F4A7C15ULL + 0x1234567ULL;
    for (std::size_t i = 0; i < size; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        if (compressible) {
            // A short repeating sequence with a little noise.
            data[i] = static_cast<u8>((i % 61) + ((state >> 60) == 0 ? u64{1} : u64{0}));
        } else {
            data[i] = static_cast<u8>(state >> 32);
        }
    }
    return data;
}

void AppendLE(std::vector<u8>& out, u64 value, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(static_cast<u8>((value >> (8 * i)) & 0xFF));
    }
}

void Append(std::vector<u8>& out, const std::vector<u8>& data) {
    out.insert(out.end(), data.begin(), data.end());
}

void Append(std::vector<u8>& out, const std::string& text) {
    out.insert(out.end(), text.begin(), text.end());
}

std::size_t FirstDifference(const std::vector<u8>& a, const std::vector<u8>& b) {
    const std::size_t common = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < common; ++i) {
        if (a[i] != b[i]) {
            return i;
        }
    }
    return a.size() == b.size() ? NoDifference : common;
}

// Encrypts [offset, offset + size) of `data` with AES-128-CTR, one keystream block at a time.
// The keystream block of offset `x` is AES(key, counter[0..8) + big-endian(x / 16)).
void ApplyCtr(std::vector<u8>& data, const Section& section) {
    Core::Crypto::AESCipher<Key128> ecb(section.key, Core::Crypto::Mode::ECB);
    std::array<u8, 0x10> keystream{};
    u64 loaded_block = ~u64{0};
    for (u64 position = section.offset; position < section.offset + section.size; ++position) {
        const u64 block = position / 16;
        if (block != loaded_block) {
            Counter counter = section.counter;
            for (std::size_t i = 0; i < 8; ++i) {
                counter[counter.size() - 1 - i] = static_cast<u8>((block >> (8 * i)) & 0xFF);
            }
            ecb.Transcode(counter.data(), counter.size(), keystream.data(),
                          Core::Crypto::Op::Encrypt);
            loaded_block = block;
        }
        data[position] = static_cast<u8>(data[position] ^ keystream[position % 16]);
    }
}

// The sections must be contiguous. Even sections are random bytes, odd ones compress well.
Nca MakeNca(const std::vector<Section>& sections, u64 seed) {
    const u64 end = sections.back().offset + sections.back().size;
    Nca nca;
    nca.decrypted = PatternBytes(static_cast<std::size_t>(end), seed, false);
    for (std::size_t i = 0; i < sections.size(); ++i) {
        const std::vector<u8> bytes =
            PatternBytes(static_cast<std::size_t>(sections[i].size), seed + 1 + i, i % 2 == 1);
        std::copy_n(bytes.data(), bytes.size(), nca.decrypted.data() + sections[i].offset);
    }
    nca.encrypted = nca.decrypted;
    for (const Section& section : sections) {
        if (section.crypto_type == 3 || section.crypto_type == 4) {
            ApplyCtr(nca.encrypted, section);
        }
    }
    return nca;
}

// A zstd frame like the one nsz writes in solid mode: it is streamed, so the frame header does not
// say how large the content is.
std::vector<u8> CompressStream(const std::vector<u8>& data) {
    const std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> context(ZSTD_createCCtx(),
                                                                       &ZSTD_freeCCtx);
    std::vector<u8> out(ZSTD_compressBound(data.size()) + 1024);
    ZSTD_outBuffer output{out.data(), out.size(), 0};
    ZSTD_inBuffer input{data.data(), data.size(), 0};
    while (input.pos < input.size) {
        const std::size_t result =
            ZSTD_compressStream2(context.get(), &output, &input, ZSTD_e_continue);
        REQUIRE(ZSTD_isError(result) == 0U);
    }
    ZSTD_inBuffer nothing{nullptr, 0, 0};
    std::size_t remaining = 1;
    while (remaining != 0) {
        remaining = ZSTD_compressStream2(context.get(), &output, &nothing, ZSTD_e_end);
        REQUIRE(ZSTD_isError(remaining) == 0U);
    }
    out.resize(output.pos);
    return out;
}

// Builds an NCZ: the first 0x4000 bytes of the NCA, the section table, then the decrypted rest of
// the NCA as one zstd stream (`block_exponent` 0) or as independent blocks of 1 << block_exponent.
std::vector<u8> MakeNcz(const Nca& nca, const std::vector<Section>& sections, u32 block_exponent) {
    std::vector<u8> ncz(nca.encrypted.data(), nca.encrypted.data() + HeaderCopySize);
    Append(ncz, std::string("NCZSECTN"));
    AppendLE(ncz, sections.size(), 8);
    for (const Section& section : sections) {
        AppendLE(ncz, section.offset, 8);
        AppendLE(ncz, section.size, 8);
        AppendLE(ncz, section.crypto_type, 8);
        AppendLE(ncz, 0, 8);
        ncz.insert(ncz.end(), section.key.begin(), section.key.end());
        ncz.insert(ncz.end(), section.counter.begin(), section.counter.end());
    }

    const std::vector<u8> stream(nca.decrypted.data() + HeaderCopySize,
                                 nca.decrypted.data() + nca.decrypted.size());
    if (block_exponent == 0) {
        Append(ncz, CompressStream(stream));
        return ncz;
    }

    const u64 block_size = u64{1} << block_exponent;
    const u64 block_count = (stream.size() + block_size - 1) / block_size;
    std::vector<std::vector<u8>> blocks;
    for (u64 i = 0; i < block_count; ++i) {
        const u8* const begin = stream.data() + i * block_size;
        const std::size_t length = static_cast<std::size_t>(
            std::min<u64>(block_size, stream.size() - i * block_size));
        std::vector<u8> compressed = Common::Compression::CompressDataZSTD(begin, length, 3);
        // A block that does not get smaller is stored as it is.
        blocks.push_back(compressed.size() < length ? std::move(compressed)
                                                    : std::vector<u8>(begin, begin + length));
    }
    Append(ncz, std::string("NCZBLOCK"));
    AppendLE(ncz, 2, 1); // Version
    AppendLE(ncz, 1, 1); // Type
    AppendLE(ncz, 0, 1);
    AppendLE(ncz, block_exponent, 1);
    AppendLE(ncz, block_count, 4);
    AppendLE(ncz, stream.size(), 8);
    for (const std::vector<u8>& block : blocks) {
        AppendLE(ncz, block.size(), 4);
    }
    for (const std::vector<u8>& block : blocks) {
        Append(ncz, block);
    }
    return ncz;
}

// A PFS0 with the data of the entries one after the other. `string_table_padding` extra bytes
// follow the names, and the first file starts `data_padding` bytes after the string table.
std::vector<u8> MakePfs0(const std::vector<Entry>& entries, u64 string_table_padding,
                         u64 data_padding) {
    std::vector<u8> string_table;
    std::vector<u64> name_offsets;
    for (const Entry& entry : entries) {
        name_offsets.push_back(string_table.size());
        Append(string_table, entry.name);
        string_table.push_back(0);
    }
    string_table.resize(string_table.size() + static_cast<std::size_t>(string_table_padding), 0);

    std::vector<u8> pfs0;
    Append(pfs0, std::string("PFS0"));
    AppendLE(pfs0, entries.size(), 4);
    AppendLE(pfs0, string_table.size(), 4);
    AppendLE(pfs0, 0, 4);
    u64 offset = data_padding;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        AppendLE(pfs0, offset, 8);
        AppendLE(pfs0, entries[i].data.size(), 8);
        AppendLE(pfs0, name_offsets[i], 4);
        AppendLE(pfs0, 0, 4);
        offset += entries[i].data.size();
    }
    Append(pfs0, string_table);
    pfs0.resize(pfs0.size() + static_cast<std::size_t>(data_padding), 0);
    for (const Entry& entry : entries) {
        Append(pfs0, entry.data);
    }
    return pfs0;
}

struct Converted {
    bool ok = false;
    std::string error;
    std::vector<u8> bytes;
    std::vector<std::pair<u64, u64>> progress;
};

Converted Convert(const std::vector<u8>& nsz, const std::atomic<bool>* cancel = nullptr) {
    const auto input = std::make_shared<FileSys::VectorVfsFile>(nsz, "game.nsz");
    // The output starts with stale content that has to be discarded.
    const auto output = std::make_shared<FileSys::VectorVfsFile>(
        std::vector<u8>(nsz.size() * 2, 0xAA), "game.nsp");
    Converted result;
    result.ok = FileSys::ConvertNszToNsp(
        input, output,
        [&result](u64 done, u64 total) { result.progress.emplace_back(done, total); },
        &result.error, cancel);
    result.bytes = output->ReadAllBytes();
    return result;
}

// Eden's own PFS0 parser has to agree with the entries that were written.
void CheckPfs0Contents(const std::vector<u8>& pfs0, const std::vector<Entry>& expected) {
    FileSys::PartitionFilesystem pfs(std::make_shared<FileSys::VectorVfsFile>(pfs0, "check.nsp"));
    REQUIRE(pfs.GetStatus() == Loader::ResultStatus::Success);
    const std::vector<FileSys::VirtualFile> files = pfs.GetFiles();
    REQUIRE(files.size() == expected.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        REQUIRE(files[i]->GetName() == expected[i].name);
        REQUIRE(files[i]->GetSize() == expected[i].data.size());
        REQUIRE(FirstDifference(files[i]->ReadAllBytes(), expected[i].data) == NoDifference);
    }
}

struct Scenario {
    std::vector<Entry> nsz_entries;
    std::vector<Entry> nsp_entries;
    std::vector<u8> nsz;
    std::vector<u8> nsp;
};

// An NSZ with two NCZ entries around two ordinary ones. The first NCZ uses `sections` and
// `block_exponent`, the second one is a small solid NCZ. The returned NSP is what the conversion
// has to produce.
Scenario MakeScenario(const std::vector<Section>& sections, u32 block_exponent) {
    const std::vector<Section> small_sections{{0x4000, 0x1200, 3, KeyB, CounterB}};
    const Nca nca_a = MakeNca(sections, 1);
    const Nca nca_b = MakeNca(small_sections, 50);
    const std::vector<u8> ticket = PatternBytes(0x2C0, 70, false);
    const std::vector<u8> meta = PatternBytes(0x3F1, 71, true);

    Scenario scenario;
    scenario.nsz_entries = {
        {"0123456789abcdef0123456789abcdef.ncz", MakeNcz(nca_a, sections, block_exponent)},
        {"0123456789abcdef0123456789abcdef.tik", ticket},
        {"fedcba9876543210fedcba9876543210.cnmt.nca", meta},
        {"00112233445566778899aabbccddeeff.ncz", MakeNcz(nca_b, small_sections, 0)},
    };
    scenario.nsp_entries = {
        {"0123456789abcdef0123456789abcdef.nca", nca_a.encrypted},
        {"0123456789abcdef0123456789abcdef.tik", ticket},
        {"fedcba9876543210fedcba9876543210.cnmt.nca", meta},
        {"00112233445566778899aabbccddeeff.nca", nca_b.encrypted},
    };
    // Padding after the string table and before the first file is kept, like nsz does.
    scenario.nsz = MakePfs0(scenario.nsz_entries, 0x13, 0x2A0);
    scenario.nsp = MakePfs0(scenario.nsp_entries, 0x13, 0x2A0);
    return scenario;
}

void CheckScenario(const std::vector<Section>& sections, u32 block_exponent) {
    const Scenario scenario = MakeScenario(sections, block_exponent);
    REQUIRE(FileSys::IsNsz(std::make_shared<FileSys::VectorVfsFile>(scenario.nsz)));

    const Converted result = Convert(scenario.nsz);
    REQUIRE(result.ok);
    REQUIRE(result.error.empty());
    REQUIRE(result.bytes.size() == scenario.nsp.size());
    REQUIRE(FirstDifference(result.bytes, scenario.nsp) == NoDifference);
    CheckPfs0Contents(result.bytes, scenario.nsp_entries);

    // The progress ends at the size of the NSP and never goes back.
    REQUIRE(!result.progress.empty());
    REQUIRE(result.progress.front().first == 0U);
    REQUIRE(result.progress.back().first == scenario.nsp.size());
    for (std::size_t i = 0; i < result.progress.size(); ++i) {
        REQUIRE(result.progress[i].second == scenario.nsp.size());
        REQUIRE((i == 0U || result.progress[i].first >= result.progress[i - 1].first));
    }

    // The result is a plain NSP.
    REQUIRE_FALSE(FileSys::IsNsz(std::make_shared<FileSys::VectorVfsFile>(result.bytes)));
}

} // Anonymous namespace

TEST_CASE("NSZ: solid streams convert to the original NSP", "[core][file_sys]") {
    CheckScenario({{0x4000, 0x3000, 3, KeyA, CounterA},
                   {0x7000, 0x1000, 1, {}, {}},
                   {0x8000, 0x2400, 4, KeyB, CounterB},
                   {0xA400, 0x600, 3, KeyA, CounterB}},
                  0);
}

TEST_CASE("NSZ: block streams convert to the original NSP", "[core][file_sys]") {
    // The random sections do not compress, so their blocks are stored as they are, the small one
    // between them is compressed, and the last block is shorter than the others: with 16 KiB
    // blocks there are six full ones and a short stored one, with 64 KiB blocks there are two.
    const std::vector<Section> sections{{0x4000, 0x11000, 3, KeyA, CounterA},
                                        {0x15000, 0x2200, 4, KeyB, CounterB},
                                        {0x17200, 0x6000, 1, {}, {}}};
    CheckScenario(sections, 14);
    CheckScenario(sections, 16);
}

TEST_CASE("NSZ: unusual section layouts convert to the original NSP", "[core][file_sys]") {
    // Plain bytes between the header copy and the first section.
    CheckScenario({{0x4400, 0x2000, 3, KeyA, CounterA}, {0x6400, 0x800, 4, KeyB, CounterB}}, 0);
    CheckScenario({{0x4400, 0x2000, 3, KeyA, CounterA}, {0x6400, 0x800, 4, KeyB, CounterB}}, 14);
    // The first section starts inside the header copy, so its first bytes are not in the data.
    CheckScenario({{0xC00, 0x4400, 3, KeyA, CounterA}, {0x5000, 0x1000, 1, {}, {}}}, 0);
    CheckScenario({{0xC00, 0x4400, 3, KeyA, CounterA}, {0x5000, 0x1000, 1, {}, {}}}, 14);
    // Sections that do not start on an AES block, as the counter then has to skip into a block.
    CheckScenario({{0x4000, 0x1003, 3, KeyA, CounterA},
                   {0x5003, 0x2005, 4, KeyB, CounterB},
                   {0x7008, 0x91, 3, KeyA, CounterB}},
                  0);
    CheckScenario({{0x4000, 0x1003, 3, KeyA, CounterA},
                   {0x5003, 0x2005, 4, KeyB, CounterB},
                   {0x7008, 0x91, 3, KeyA, CounterB}},
                  14);
}

TEST_CASE("NSZ: IsNsz only accepts PFS0 containers with .ncz entries", "[core][file_sys]") {
    const auto wrap = [](const std::vector<u8>& bytes) {
        return std::make_shared<FileSys::VectorVfsFile>(bytes);
    };
    const Scenario scenario = MakeScenario({{0x4000, 0x1000, 3, KeyA, CounterA}}, 0);

    REQUIRE(FileSys::IsNsz(wrap(scenario.nsz)));
    REQUIRE_FALSE(FileSys::IsNsz(wrap(scenario.nsp)));
    REQUIRE_FALSE(FileSys::IsNsz(nullptr));
    REQUIRE_FALSE(FileSys::IsNsz(wrap({})));
    REQUIRE_FALSE(FileSys::IsNsz(wrap(PatternBytes(0x1000, 5, false))));

    // A download that is not finished is still recognized, converting it fails.
    std::vector<u8> truncated = scenario.nsz;
    truncated.resize(truncated.size() - 0x100);
    REQUIRE(FileSys::IsNsz(wrap(truncated)));

    // Other containers are not NSZ.
    std::vector<u8> hfs0 = scenario.nsz;
    hfs0[0] = 'H';
    REQUIRE_FALSE(FileSys::IsNsz(wrap(hfs0)));
}

TEST_CASE("NSZ: damaged files are rejected", "[core][file_sys]") {
    const Scenario scenario =
        MakeScenario({{0x4000, 0x2000, 3, KeyA, CounterA}, {0x6000, 0x1000, 4, KeyB, CounterB}}, 0);
    REQUIRE(Convert(scenario.nsz).ok);

    // The end of the last entry is missing.
    std::vector<u8> truncated = scenario.nsz;
    truncated.resize(truncated.size() - 0x40);
    Converted result = Convert(truncated);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("NSZ") == 0U);

    // The first NCZ lost its section table.
    std::vector<u8> no_magic = scenario.nsz;
    const std::string magic = "NCZSECTN";
    const auto magic_at = std::search(no_magic.begin(), no_magic.end(), magic.begin(), magic.end());
    REQUIRE(magic_at != no_magic.end());
    *magic_at = 'X';
    result = Convert(no_magic);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("NCZSECTN") != std::string::npos);

    // The second section does not start where the first one ends.
    std::vector<u8> gap = scenario.nsz;
    const auto table_at = std::search(gap.begin(), gap.end(), magic.begin(), magic.end());
    REQUIRE(table_at != gap.end());
    const std::size_t second_offset = static_cast<std::size_t>(table_at - gap.begin()) + 16 + 0x40;
    gap[second_offset + 1] = static_cast<u8>(gap[second_offset + 1] + 1);
    result = Convert(gap);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("contiguous") != std::string::npos);

    // Not an NSZ at all.
    result = Convert(scenario.nsp);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find(".ncz") != std::string::npos);
    result = Convert(PatternBytes(0x1000, 6, false));
    REQUIRE_FALSE(result.ok);
}

TEST_CASE("NSZ: a conversion can be cancelled", "[core][file_sys]") {
    const Scenario scenario = MakeScenario({{0x4000, 0x2000, 3, KeyA, CounterA}}, 0);
    const std::atomic<bool> cancel{true};
    const Converted result = Convert(scenario.nsz, &cancel);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("cancelled") != std::string::npos);
}
