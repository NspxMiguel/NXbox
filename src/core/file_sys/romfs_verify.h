// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/file_sys/fssystem/fssystem_nca_file_system_driver.h"

namespace FileSys {

void ResetRomfsVerification();

// Retains the same raw section and final RomFS used by this mount. No second mount
// or independent decryption path can accidentally hide a read-path discrepancy.
class RomfsVerification {
public:
    RomfsVerification(VirtualFile romfs, NcaFsHeader header,
                      NcaFileSystemDriver::StorageContext context, Hash header_hash, u64 title_id,
                      std::string identity);
    void Verify() const;

private:
    void Trace(std::string_view role, u64 offset, u64 size) const;
    void LogCounters(std::string_view phase) const;

    VirtualFile m_romfs;
    NcaFsHeader m_header;
    NcaFileSystemDriver::StorageContext m_context;
    Hash m_header_hash;
    u64 m_title_id;
    std::string m_identity;
};

} // namespace FileSys
