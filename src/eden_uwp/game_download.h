// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>

namespace EdenXbox {

/// Downloads `url` into `destination`, resuming a partial file with an HTTP Range request. Returns
/// true once the file on disk matches the size the server reports. Progress goes to the diagnostic
/// log. Must run on a thread that may block (an MTA worker), never on the UI thread.
bool DownloadFile(const std::string& url, const std::filesystem::path& destination);

} // namespace EdenXbox
