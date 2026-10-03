// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "core/crypto/aes_util.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/fssystem/fssystem_aes_ctr_counter_extended_storage.h"
#include "core/file_sys/fssystem/fssystem_aes_ctr_storage.h"
#include "core/file_sys/fssystem/fssystem_alignment_matching_storage.h"
#include "core/file_sys/fssystem/fssystem_indirect_storage.h"
#include "core/file_sys/vfs/vfs_offset.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace {
using namespace FileSys;
constexpr size_t FourGiB = size_t{1} << 32;

class PatternStorage : public IReadOnlyStorage {
public:
    size_t GetSize() const override {
        return FourGiB + 64;
    }
    size_t Read(u8* out, size_t size, size_t offset) const override {
        last_offset = offset;
        if (offset >= GetSize()) {
            return 0;
        }
        size = std::min(size, GetSize() - offset);
        if (short_read && size != 0) {
            --size;
        }
        for (size_t i = 0; i < size; ++i) {
            out[i] = Byte(offset + i);
        }
        return size;
    }
    static u8 Byte(size_t offset) {
        return static_cast<u8>((offset >> 32) * 73 + offset % 251);
    }
    bool short_read{};
    mutable size_t last_offset{};
};

template <typename Entry, size_t Count>
auto MakeTable(const std::array<Entry, Count>& entries, s64 end) {
    constexpr size_t NodeSize = 0x4000;
    std::vector<u8> node(NodeSize), table(NodeSize);
    const BucketTree::NodeHeader root{0, 1, end};
    const BucketTree::NodeHeader entry_set{0, static_cast<s32>(Count), end};
    std::memcpy(node.data(), &root, sizeof(root));
    std::memcpy(table.data(), &entry_set, sizeof(entry_set));
    // Both entry formats start with the first virtual/physical subsection offset.
    std::memcpy(node.data() + sizeof(root), entries.data(), sizeof(s64));
    std::memcpy(table.data() + sizeof(entry_set), entries.data(), sizeof(entries));
    return std::pair{std::make_shared<VectorVfsFile>(std::move(node)),
                     std::make_shared<VectorVfsFile>(std::move(table))};
}
} // namespace

TEST_CASE("BKTR relocation crosses 4 GiB and overlays a short update fragment",
          "[file_sys][bktr]") {
    auto base = std::make_shared<PatternStorage>();
    auto patch = std::make_shared<VectorVfsFile>(std::vector<u8>(64, 0xAB));
    std::array<IndirectStorage::Entry, 3> entries{};
    entries[0].SetVirtualOffset(0);
    entries[0].SetPhysicalOffset(0);
    entries[1].SetVirtualOffset(FourGiB - 8);
    entries[1].SetPhysicalOffset(32);
    entries[1].storage_index = 1;
    entries[2].SetVirtualOffset(FourGiB + 8);
    entries[2].SetPhysicalOffset(FourGiB + 8);
    auto [node, table] = MakeTable(entries, FourGiB + 64);
    IndirectStorage storage;
    REQUIRE(R_SUCCEEDED(storage.Initialize(node, table, static_cast<s32>(entries.size()))));
    storage.SetStorage(0, base);
    storage.SetStorage(1, patch);
    std::array<u8, 48> merged{}, unmerged{};
    REQUIRE(storage.Read(merged.data(), merged.size(), FourGiB - 16) == merged.size());
    REQUIRE(storage.ReadUnmerged(unmerged.data(), unmerged.size(), FourGiB - 16) ==
            unmerged.size());
    REQUIRE(merged == unmerged);
    for (size_t i = 0; i < merged.size(); ++i) {
        REQUIRE(merged[i] == (i >= 8 && i < 24 ? 0xAB : PatternStorage::Byte(FourGiB - 16 + i)));
    }
    base->short_read = true;
    REQUIRE(storage.Read(merged.data(), merged.size(), FourGiB - 16) == 0);
    REQUIRE(storage.Read(merged.data(), 1, storage.GetSize()) == 0);
}

TEST_CASE("BKTR subsection counters retain high offsets and generation changes",
          "[file_sys][bktr]") {
    using Core::Crypto::AESCipher;
    using Core::Crypto::Key128;
    using Core::Crypto::Mode;
    using Core::Crypto::Op;
    const Key128 key{1, 2, 3, 4};
    constexpr u32 SecureValue = 0x10203040;
    constexpr u64 CounterOffset = FourGiB - 16;
    std::vector<u8> plain(64), encrypted(64);
    for (size_t i = 0; i < plain.size(); ++i) {
        plain[i] = static_cast<u8>(i * 13);
    }
    AESCipher<Key128> ecb(key, Mode::ECB);
    for (size_t offset = 0; offset < plain.size(); offset += 16) {
        // Construct CTR keystream via ECB, independently of MakeIv/AddCounter.
        const u64 upper = (u64{SecureValue} << 32) | (offset < 16 ? 3 : 7);
        const u64 lower = (CounterOffset + offset) / 16;
        Key128 counter{}, stream{};
        for (size_t byte = 0; byte < 8; ++byte) {
            counter[7 - byte] = static_cast<u8>(upper >> (8 * byte));
            counter[15 - byte] = static_cast<u8>(lower >> (8 * byte));
        }
        ecb.Transcode(counter.data(), counter.size(), stream.data(), Op::Encrypt);
        for (size_t byte = 0; byte < 16; ++byte) {
            encrypted[offset + byte] = plain[offset + byte] ^ stream[byte];
        }
    }
    std::array<AesCtrCounterExtendedStorage::Entry, 2> entries{};
    entries[0].SetOffset(0);
    entries[0].generation = 3;
    entries[1].SetOffset(16);
    entries[1].generation = 7;
    auto [node, table] = MakeTable(entries, encrypted.size());
    auto storage = std::make_shared<AesCtrCounterExtendedStorage>();
    std::unique_ptr<AesCtrCounterExtendedStorage::IDecryptor> decryptor;
    REQUIRE(R_SUCCEEDED(AesCtrCounterExtendedStorage::CreateSoftwareDecryptor(&decryptor)));
    REQUIRE(R_SUCCEEDED(storage->Initialize(key.data(), key.size(), SecureValue, CounterOffset,
                                            std::make_shared<VectorVfsFile>(std::move(encrypted)),
                                            node, table, static_cast<s32>(entries.size()),
                                            std::move(decryptor))));
    AlignmentMatchingStorage<16, 1> aligned(storage);
    std::array<u8, 42> out{};
    REQUIRE(aligned.Read(out.data(), out.size(), 7) == out.size());
    REQUIRE(std::equal(out.begin(), out.end(), plain.begin() + 7));
    REQUIRE(storage->Read(out.data(), 16, 64) == 0);
}

TEST_CASE("Offset and aligned AES storages do not report unread bytes", "[file_sys][bktr]") {
    auto base = std::make_shared<PatternStorage>();
    OffsetVfsFile view(base, 32, FourGiB);
    std::array<u8, 32> out{};
    REQUIRE(view.Read(out.data(), out.size(), 8) == 24);
    REQUIRE(base->last_offset == FourGiB + 8);
    REQUIRE(view.Read(out.data(), 1, 33) == 0);
    REQUIRE(view.ReadBytes(1, 33).empty());
    const Core::Crypto::Key128 key{}, iv{};
    auto ctr = std::make_shared<AesCtrStorage>(base, key.data(), key.size(), iv.data(), iv.size());
    AlignmentMatchingStorage<16, 1> aligned(ctr);
    base->short_read = true;
    REQUIRE(aligned.Read(out.data(), 17, 1) == 0);
}

TEST_CASE("AES CTR preserves its stream across bounded EVP updates", "[crypto][bktr]") {
    using namespace Core::Crypto;
    const Key128 key{1, 2, 3, 4}, iv{4, 3, 2, 1};
    constexpr size_t Batch = 1 << 20;
    std::vector<u8> input(Batch + 37, 0x5A), whole(input.size()), split(input.size());
    AESCipher<Key128> cipher(key, Mode::CTR);
    cipher.SetIV(iv);
    cipher.Transcode(input.data(), input.size(), whole.data(), Op::Encrypt);
    cipher.SetIV(iv);
    cipher.Transcode(input.data(), Batch, split.data(), Op::Encrypt);
    auto next_iv = iv;
    const u64 counter = Batch / 16;
    for (size_t i = 0; i < 8; ++i) {
        next_iv[15 - i] = static_cast<u8>(counter >> (8 * i));
    }
    cipher.SetIV(next_iv);
    cipher.Transcode(input.data() + Batch, input.size() - Batch, split.data() + Batch, Op::Encrypt);
    REQUIRE(whole == split);
    cipher.SetIV(iv);
    cipher.Transcode(whole.data(), whole.size(), whole.data(), Op::Decrypt);
    REQUIRE(whole == input);
}

TEST_CASE("BKTR rejects a truncated bucket node", "[file_sys][bktr]") {
    IndirectStorage storage;
    auto truncated = std::make_shared<VectorVfsFile>(std::vector<u8>(8));
    REQUIRE(R_FAILED(storage.Initialize(truncated, truncated, 1)));
}
