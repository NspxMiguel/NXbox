// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace EdenXbox {

/// Downloads `url` into `destination`, resuming a partial file with an HTTP Range request. Returns
/// true once the file on disk matches the size the server reports. Progress goes to the diagnostic
/// log. Must run on a thread that may block (an MTA worker), never on the UI thread.
// Updates destination when internal space is insufficient. The callback persists the selected
// path before any bytes are written, so a subsequent launch resumes on the same drive.
//
// `progress` (optional) receives the bytes on disk and the total after every chunk, from the
// calling thread. `cancel` (optional) is polled between chunks and between attempts; when it turns
// true the partial file is kept, so a later call resumes it, and the function returns false.
bool DownloadFile(const std::string& url, std::filesystem::path& destination,
                  const std::function<void(const std::filesystem::path&)>& remember_target,
                  const std::function<void(std::uint64_t have, std::uint64_t total)>& progress = {},
                  const std::atomic<bool>* cancel = nullptr);

} // namespace EdenXbox
