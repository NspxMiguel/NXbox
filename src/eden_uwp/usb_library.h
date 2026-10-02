// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace EdenXbox {

// All StorageFolder helpers below must run on an MTA worker, never the UI thread.
std::vector<std::filesystem::path> ListExternalGames();
std::vector<std::filesystem::path> ExternalGameFolders();
std::optional<std::uint64_t> StorageFreeSpace(const std::filesystem::path& folder);
// Free bytes for writing into `folder`: the smaller of std::filesystem::space and the
// StorageFolder System.FreeSpace property. On the console the first overstated the internal
// storage (a 7.3 GB conversion passed the check and failed after 4.2 GB). Logs both values.
std::optional<std::uint64_t> FreeSpace(const std::filesystem::path& folder);

struct UsbLibraryScan {
    bool drive_found = false;
    std::wstring drive_path; // e.g. L"E:\"
    bool prod_keys_found = false;
    bool title_keys_found = false;
    std::vector<std::wstring> games; // relative paths on the drive, e.g. L"switch\\games\\p5r.nsp"
};

// Looks at every removable drive the app has been granted (the "removableStorage" capability)
// for a Lockpick-style dump layout: "switch\prod.keys", "switch\title.keys",
// "switch\games\*.nsp" (also tried at the drive root, without the "switch\" prefix, since not
// every dump tool nests it). Read-only: nothing is copied yet.
UsbLibraryScan ScanUsbForGamesAndKeys();

// Copies whatever ScanUsbForGamesAndKeys found into LocalState\eden\keys and LocalState\games,
// where the emulator's existing key loader and ResolveGamePath() already look. Returns the number
// of files copied; games already present (same name, same size) are skipped, not re-copied.
int ImportFromUsb(const UsbLibraryScan& scan);

} // namespace EdenXbox
