// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/file_sys/fssystem/fssystem_aes_ctr_counter_extended_storage.h"
#include "core/file_sys/fssystem/fssystem_aes_ctr_storage.h"
#include "core/file_sys/fssystem/fssystem_nca_header.h"
#include "core/file_sys/vfs/vfs_offset.h"

namespace FileSys {

namespace {

class SoftwareDecryptor final : public AesCtrCounterExtendedStorage::IDecryptor {
public:
    virtual void Decrypt(
        u8* buf, size_t buf_size, const std::array<u8, AesCtrCounterExtendedStorage::KeySize>& key,
        const std::array<u8, AesCtrCounterExtendedStorage::IvSize>& iv) override final;
};

} // namespace

Result AesCtrCounterExtendedStorage::CreateSoftwareDecryptor(std::unique_ptr<IDecryptor>* out) {
    std::unique_ptr<IDecryptor> decryptor = std::make_unique<SoftwareDecryptor>();
    R_UNLESS(decryptor != nullptr, ResultAllocationMemoryFailedInAesCtrCounterExtendedStorageA);
    *out = std::move(decryptor);
    R_SUCCEED();
}

Result AesCtrCounterExtendedStorage::Initialize(const void* key, size_t key_size, u32 secure_value,
                                                VirtualFile data_storage,
                                                VirtualFile table_storage) {
    // Read and verify the bucket tree header.
    BucketTree::Header header;
    table_storage->ReadObject(std::addressof(header), 0);
    R_TRY(header.Verify());

    // Determine extents.
    const auto node_storage_size = QueryNodeStorageSize(header.entry_count);
    const auto entry_storage_size = QueryEntryStorageSize(header.entry_count);
    const auto node_storage_offset = QueryHeaderStorageSize();
    const auto entry_storage_offset = node_storage_offset + node_storage_size;

    // Create a software decryptor.
    std::unique_ptr<IDecryptor> sw_decryptor;
    R_TRY(CreateSoftwareDecryptor(std::addressof(sw_decryptor)));

    // Initialize.
    R_RETURN(this->Initialize(
        key, key_size, secure_value, 0, data_storage,
        std::make_shared<OffsetVfsFile>(table_storage, node_storage_size, node_storage_offset),
        std::make_shared<OffsetVfsFile>(table_storage, entry_storage_size, entry_storage_offset),
        header.entry_count, std::move(sw_decryptor)));
}

Result AesCtrCounterExtendedStorage::Initialize(const void* key, size_t key_size, u32 secure_value,
                                                s64 counter_offset, VirtualFile data_storage,
                                                VirtualFile node_storage, VirtualFile entry_storage,
                                                s32 entry_count,
                                                std::unique_ptr<IDecryptor>&& decryptor) {
    // Validate preconditions.
    ASSERT(key != nullptr);
    ASSERT(key_size == KeySize);
    ASSERT(counter_offset >= 0);
    ASSERT(decryptor != nullptr);

    // Initialize the bucket tree table.
    if (entry_count > 0) {
        R_TRY(
            m_table.Initialize(node_storage, entry_storage, NodeSize, sizeof(Entry), entry_count));
    } else {
        m_table.Initialize(NodeSize, 0);
    }

    // Set members.
    m_data_storage = data_storage;
    std::memcpy(m_key.data(), key, key_size);
    m_secure_value = secure_value;
    m_counter_offset = counter_offset;
    m_decryptor = std::move(decryptor);
    m_diagnostics.Initialize(entry_count);

    R_SUCCEED();
}

void AesCtrCounterExtendedStorage::Finalize() {
    if (this->IsInitialized()) {
        m_table.Finalize();
        m_data_storage = VirtualFile();
    }
}

Result AesCtrCounterExtendedStorage::GetEntryList(Entry* out_entries, s32* out_entry_count,
                                                  s32 entry_count, s64 offset, s64 size) {
    // Validate pre-conditions.
    ASSERT(offset >= 0);
    ASSERT(size >= 0);
    ASSERT(this->IsInitialized());

    // Clear the out count.
    R_UNLESS(out_entry_count != nullptr, ResultNullptrArgument);
    *out_entry_count = 0;

    // Succeed if there's no range.
    R_SUCCEED_IF(size == 0);

    // If we have an output array, we need it to be non-null.
    R_UNLESS(out_entries != nullptr || entry_count == 0, ResultNullptrArgument);

    // Check that our range is valid.
    BucketTree::Offsets table_offsets{};
    R_TRY(m_table.GetOffsets(std::addressof(table_offsets)));

    R_UNLESS(table_offsets.IsInclude(offset, size), ResultOutOfRange);

    // Find the offset in our tree.
    BucketTree::Visitor visitor;
    R_TRY(m_table.Find(std::addressof(visitor), offset));
    {
        const auto entry_offset = visitor.Get<Entry>()->GetOffset();
        R_UNLESS(0 <= entry_offset && table_offsets.IsInclude(entry_offset),
                 ResultInvalidAesCtrCounterExtendedEntryOffset);
    }

    // Prepare to loop over entries.
    const auto end_offset = offset + static_cast<s64>(size);
    s32 count = 0;

    auto cur_entry = *visitor.Get<Entry>();
    while (cur_entry.GetOffset() < end_offset) {
        // Try to write the entry to the out list.
        if (entry_count != 0) {
            if (count >= entry_count) {
                break;
            }
            std::memcpy(out_entries + count, std::addressof(cur_entry), sizeof(Entry));
        }

        count++;

        // Advance.
        if (visitor.CanMoveNext()) {
            R_TRY(visitor.MoveNext());
            cur_entry = *visitor.Get<Entry>();
        } else {
            break;
        }
    }

    // Write the output count.
    *out_entry_count = count;
    R_SUCCEED();
}

size_t AesCtrCounterExtendedStorage::Read(u8* buffer, size_t size, size_t offset) const {
    ASSERT(this->IsInitialized());
    if (size == 0) {
        return 0;
    }
    u64 visited = 0;
    const auto fail = [&](Result result) -> size_t {
        m_diagnostics.Record(visited, true);
        m_diagnostics.Failure(offset, size, result.raw);
        return 0;
    };
    if (buffer == nullptr || !Common::IsAligned(offset, BlockSize) ||
        !Common::IsAligned(size, BlockSize)) {
        return fail(ResultInvalidArgument);
    }
    BucketTree::Offsets table_offsets{};
    if (const Result result = m_table.GetOffsets(&table_offsets); R_FAILED(result)) {
        return fail(result);
    }
    if (offset > static_cast<u64>(table_offsets.end_offset) ||
        size > static_cast<u64>(table_offsets.end_offset) - offset ||
        offset < static_cast<u64>(table_offsets.start_offset)) {
        return fail(ResultOutOfRange);
    }
    if (m_data_storage->Read(buffer, size, offset) != size) {
        return fail(ResultInvalidSize);
    }
    BucketTree::Visitor visitor;
    if (const Result result = m_table.Find(&visitor, offset); R_FAILED(result)) {
        return fail(result);
    }
    size_t current = offset;
    const size_t end = offset + size;
    while (current < end) {
        ++visited;
        const auto entry = *visitor.Get<Entry>();
        const s64 entry_offset = entry.GetOffset();
        if (entry_offset < 0 || static_cast<u64>(entry_offset) > current ||
            !Common::IsAligned(entry_offset, BlockSize)) {
            return fail(ResultInvalidAesCtrCounterExtendedEntryOffset);
        }
        s64 next_offset = table_offsets.end_offset;
        if (visitor.CanMoveNext()) {
            if (const Result result = visitor.MoveNext(); R_FAILED(result)) {
                return fail(result);
            }
            next_offset = visitor.Get<Entry>()->GetOffset();
        }
        if (next_offset <= static_cast<s64>(current) || next_offset > table_offsets.end_offset ||
            !Common::IsAligned(next_offset, BlockSize)) {
            return fail(ResultInvalidAesCtrCounterExtendedEntryOffset);
        }
        const size_t chunk = std::min(end - current, static_cast<size_t>(next_offset) - current);
        if (entry.encryption_value == Entry::Encryption::Encrypted) {
            const NcaAesCtrUpperIv upper_iv = {
                .part = {.generation = static_cast<u32>(entry.generation),
                         .secure_value = m_secure_value}};
            std::array<u8, IvSize> iv{};
            // The low counter uses the physical NCA position, not the virtual
            // relocation offset or the start of the subsection.
            AesCtrStorage::MakeIv(iv.data(), iv.size(), upper_iv.value, m_counter_offset + current);
            m_decryptor->Decrypt(buffer + (current - offset), chunk, m_key, iv);
        } else if (entry.encryption_value != Entry::Encryption::NotEncrypted) {
            return fail(ResultInvalidArgument);
        }
        current += chunk;
    }
    m_diagnostics.Record(visited, false);
    return size;
}

size_t AesCtrCounterExtendedStorage::ReadReference(u8* buffer, size_t size, u64 offset,
                                                   u32 generation, bool encrypted) const {
    if (!m_data_storage || offset > m_data_storage->GetSize() ||
        size > m_data_storage->GetSize() - offset ||
        m_data_storage->Read(buffer, size, offset) != size) {
        return 0;
    }
    if (!encrypted) {
        return size;
    }
    Core::Crypto::AESCipher<Core::Crypto::Key128> cipher(m_key, Core::Crypto::Mode::ECB);
    size_t done = 0;
    while (done < size) {
        const u64 absolute = static_cast<u64>(m_counter_offset) + offset + done;
        const u64 counter = absolute / BlockSize;
        std::array<u8, BlockSize> input{}, stream{};
        for (size_t i = 0; i < 4; ++i) {
            input[i] = static_cast<u8>(m_secure_value >> (24 - 8 * i));
            input[4 + i] = static_cast<u8>(generation >> (24 - 8 * i));
        }
        for (size_t i = 0; i < 8; ++i) {
            input[8 + i] = static_cast<u8>(counter >> (56 - 8 * i));
        }
        cipher.Transcode(input.data(), input.size(), stream.data(), Core::Crypto::Op::Encrypt);
        const size_t skip = absolute % BlockSize;
        const size_t chunk = std::min(size - done, BlockSize - skip);
        for (size_t i = 0; i < chunk; ++i) {
            buffer[done + i] ^= stream[skip + i];
        }
        done += chunk;
    }
    return size;
}

void SoftwareDecryptor::Decrypt(u8* buf, size_t buf_size, const std::array<u8, AesCtrCounterExtendedStorage::KeySize>& key, const std::array<u8, AesCtrCounterExtendedStorage::IvSize>& iv) {
    Core::Crypto::AESCipher<Core::Crypto::Key128> cipher(key, Core::Crypto::Mode::CTR);
    cipher.SetIV(iv);
    cipher.Transcode(buf, buf_size, buf, Core::Crypto::Op::Decrypt);
}

} // namespace FileSys
