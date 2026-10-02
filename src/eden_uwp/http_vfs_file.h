// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/file_sys/vfs/vfs_types.h"

namespace EdenXbox {

// A read-only VirtualFile backed by HTTP range requests, so a converter can read a remote file
// without a local copy (the console's storage cannot hold an .nsz and its .nsp at once). Reads
// are served from 16 MiB blocks fetched on demand and kept in a small cache; sequential reads
// cost one request per block. Returns nullptr when the server does not report a size or does
// not honour ranges. Blocking: call from a worker thread (MTA), never the UI thread.
FileSys::VirtualFile OpenHttpFile(const std::string& url);

} // namespace EdenXbox
