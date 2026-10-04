// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/file_sys/fssystem/fssystem_nca_file_system_driver.h"

namespace FileSys {

void ResetRomfsVerification();

// Retains the raw section and exact RomFS returned by this mount. IVFC hashes
// authenticate VFS reads; a separate flat-table/ECB reference diagnoses discrepancies.
class RomfsVerification {
public:
    RomfsVerification(VirtualFile romfs, NcaFsHeader header,
                      NcaFileSystemDriver::StorageContext context, Hash header_hash, u64 title_id,
                      std::string identity, bool is_update);
    void Verify(VirtualFile received, bool full) const;

private:
    void Trace(std::string_view role, u64 offset, u64 size) const;
    void LogCounters(std::string_view phase) const;

    VirtualFile m_romfs;
    NcaFsHeader m_header;
    NcaFileSystemDriver::StorageContext m_context;
    Hash m_header_hash;
    u64 m_title_id;
    std::string m_identity;
    bool m_is_update;
};

} // namespace FileSys
