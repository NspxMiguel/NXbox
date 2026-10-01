// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace EdenXbox {

/// Downloads `url` into `destination`, resuming a partial file with an HTTP Range request. Returns
/// true once the file on disk matches the size the server reports. Progress goes to the diagnostic
/// log. Must run on a thread that may block (an MTA worker), never on the UI thread.
// Updates destination when internal space is insufficient. The callback persists the selected
// path before any bytes are written, so a subsequent launch resumes on the same drive.
bool DownloadFile(const std::string& url, std::filesystem::path& destination,
                  const std::function<void(const std::filesystem::path&)>& remember_target);

} // namespace EdenXbox
