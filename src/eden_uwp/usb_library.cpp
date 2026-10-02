// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/usb_library.h"

#include <algorithm>
#include <cwctype>
#include <array>
#include <chrono>
#include <future>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.FileProperties.h>

#include <fmt/format.h>

#include "eden_uwp/diagnostic.h"
#include "eden_uwp/await_bounded.h"

namespace EdenXbox {
namespace {

using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Foundation::Collections;

// Tries "switch\<relative>" first (the common Lockpick-style SD backup layout), then
// "<relative>" at the drive root, so a dump that was not nested under "switch" still works.
std::optional<IStorageItem> FindItem(const StorageFolder& drive, const wchar_t* relative) {
    for (const auto& prefix : {L"switch\\", L""}) {
        try {
            std::wstring path = prefix;
            path += relative;
            auto item = AwaitBounded(drive.TryGetItemAsync(path), std::chrono::seconds(3));
            if (item && *item) {
                return *item;
            }
        } catch (const winrt::hresult_error&) {
            // Not there under this prefix; try the next one.
        }
    }
    return std::nullopt;
}

void ListGames(const StorageFolder& drive, const wchar_t* directory,
               std::vector<std::wstring>& out) {
    try {
        auto item = FindItem(drive, directory);
        if (!item) {
            return;
        }
        auto folder = item->try_as<StorageFolder>();
        if (!folder) {
            return;
        }
        auto files = AwaitBounded(folder.GetFilesAsync(), std::chrono::seconds(5));
        if (!files) {
            return;
        }
        for (const auto& file : *files) {
            const auto name = std::wstring(file.Name());
            if (name.size() > 4 &&
                (name.ends_with(L".nsp") || name.ends_with(L".nsz") || name.ends_with(L".xci"))) {
                out.push_back(std::wstring(directory) + L"\\" + name);
            }
        }
    } catch (const winrt::hresult_error& error) {
        Diagnostic("USB_LIST_FAILED " + winrt::to_string(error.message()));
    }
}

} // namespace

std::vector<std::filesystem::path> ListExternalGames() {
    std::vector<std::filesystem::path> result;
    try {
        const auto drives = AwaitBounded(KnownFolders::RemovableDevices().GetFoldersAsync(),
                                         std::chrono::seconds(5));
        if (!drives) {
            return result;
        }
        for (const auto& drive : *drives) {
            try {
                // TryGetItemAsync: a drive without the folder is the normal case, not an error.
                const auto item = AwaitBounded(drive.TryGetItemAsync(L"NXbox\\games"),
                                               std::chrono::seconds(5));
                const auto folder = item && *item ? item->try_as<StorageFolder>() : nullptr;
                if (!folder) {
                    continue;
                }
                const auto files = AwaitBounded(folder.GetFilesAsync(), std::chrono::seconds(5));
                if (!files) {
                    continue;
                }
                for (const auto& file : *files) {
                    const std::filesystem::path path{std::wstring(file.Path())};
                    auto extension = path.extension().wstring();
                    std::transform(extension.begin(), extension.end(), extension.begin(),
                                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
                    if (extension == L".nsp" || extension == L".nsz" || extension == L".xci") {
                        result.push_back(path);
                        Diagnostic("USB_GAME " + winrt::to_string(file.Path()));
                    }
                }
            } catch (const winrt::hresult_error& error) {
                Diagnostic("USB_LIBRARY_SKIP " + winrt::to_string(error.message()));
            }
        }
    } catch (const winrt::hresult_error& error) {
        Diagnostic("USB_LIBRARY_FAILED " + winrt::to_string(error.message()));
    }
    return result;
}

std::vector<std::filesystem::path> ExternalGameFolders() {
    std::vector<std::filesystem::path> result;
    try {
        const auto drives = AwaitBounded(KnownFolders::RemovableDevices().GetFoldersAsync(),
                                         std::chrono::seconds(5));
        if (!drives) {
            return result;
        }
        for (const auto& drive : *drives) {
            try {
                const auto root = AwaitBounded(
                    drive.CreateFolderAsync(L"NXbox", CreationCollisionOption::OpenIfExists),
                    std::chrono::seconds(5));
                if (!root) {
                    continue;
                }
                const auto games = AwaitBounded(
                    root->CreateFolderAsync(L"games", CreationCollisionOption::OpenIfExists),
                    std::chrono::seconds(5));
                if (games) {
                    result.emplace_back(std::wstring(games->Path()));
                }
            } catch (const winrt::hresult_error& error) {
                Diagnostic("USB_TARGET_SKIP " + winrt::to_string(error.message()));
            }
        }
    } catch (const winrt::hresult_error& error) {
        Diagnostic("USB_TARGET_FAILED " + winrt::to_string(error.message()));
    }
    return result;
}

std::optional<std::uint64_t> StorageFreeSpace(const std::filesystem::path& path) {
    try {
        const auto folder = AwaitBounded(StorageFolder::GetFolderFromPathAsync(path.native()),
                                         std::chrono::seconds(5));
        if (folder) {
            const auto properties = AwaitBounded(
                folder->Properties().RetrievePropertiesAsync({L"System.FreeSpace"}),
                std::chrono::seconds(5));
            if (properties && properties->HasKey(L"System.FreeSpace")) {
                const auto value = properties->Lookup(L"System.FreeSpace");
                if (value) {
                    return value.as<winrt::Windows::Foundation::IPropertyValue>().GetUInt64();
                }
            }
        }
    } catch (const winrt::hresult_error& error) {
        Diagnostic("USB_SPACE_FAILED " + winrt::to_string(error.message()));
    }
    return std::nullopt;
}

std::optional<std::uint64_t> FreeSpace(const std::filesystem::path& folder) {
    std::error_code ec;
    const auto space = std::filesystem::space(folder, ec);
    const auto storage = StorageFreeSpace(folder);
    Diagnostic(fmt::format("SPACE {} fs={} storage={} MiB", winrt::to_string(folder.native()),
                           ec ? -1 : static_cast<long long>(space.available >> 20),
                           storage ? static_cast<long long>(*storage >> 20) : -1));
    if (!ec && storage) {
        return std::min<std::uint64_t>(space.available, *storage);
    }
    if (storage) {
        return storage;
    }
    if (!ec) {
        return space.available;
    }
    return std::nullopt;
}

UsbLibraryScan ScanUsbForGamesAndKeys() {
    UsbLibraryScan result;
    try {
        auto drives = AwaitBounded(KnownFolders::RemovableDevices().GetFoldersAsync(),
                                   std::chrono::seconds(5));
        if (!drives || (*drives).Size() == 0) {
            Diagnostic("USB_SCAN no removable drive found (or capability not granted)");
            return result;
        }
        // The first drive with a recognizable layout wins; a console has one game USB at a time
        // in practice, and picking the first keeps this simple.
        for (const auto& drive : *drives) {
            std::vector<std::wstring> games;
            ListGames(drive, L"games", games);
            if (games.empty()) {
                ListGames(drive, L"nxbox\\games", games);
            }
            const bool prod = FindItem(drive, L"prod.keys").has_value();
            const bool title = FindItem(drive, L"title.keys").has_value();
            if (games.empty() && !prod && !title) {
                continue; // Not a games drive; keep looking.
            }
            result.drive_found = true;
            result.drive_path = std::wstring(drive.Path());
            result.prod_keys_found = prod;
            result.title_keys_found = title;
            result.games = std::move(games);
            Diagnostic("USB_SCAN drive=" + winrt::to_string(drive.Path()) +
                      " prod_keys=" + (prod ? "yes" : "no") + " title_keys=" + (title ? "yes" : "no") +
                      " games=" + std::to_string(result.games.size()));
            return result;
        }
        Diagnostic("USB_SCAN drive(s) present but none has switch/, nxbox/games/, prod.keys or "
                  "title.keys");
    } catch (const winrt::hresult_error& error) {
        Diagnostic("USB_SCAN_FAILED " + winrt::to_string(error.message()));
    }
    return result;
}

namespace {
bool CopyItemInto(const StorageFolder& drive, const wchar_t* relative_path,
                  const StorageFolder& destination, const wchar_t* destination_name) {
    auto item = FindItem(drive, relative_path);
    if (!item) {
        return false;
    }
    auto file = item->try_as<StorageFile>();
    if (!file) {
        return false;
    }
    try {
        // Skip a re-copy when a same-size file is already there; a 14 GiB NSP copy is not
        // something to repeat every time the setup screen opens with the drive still in.
        auto existing = AwaitBounded(destination.TryGetItemAsync(destination_name),
                                     std::chrono::seconds(3));
        if (existing && *existing) {
            auto existing_file = (*existing).try_as<StorageFile>();
            if (existing_file) {
                auto existing_props =
                    AwaitBounded(existing_file.GetBasicPropertiesAsync(), std::chrono::seconds(3));
                auto source_props = AwaitBounded(file.GetBasicPropertiesAsync(), std::chrono::seconds(3));
                if (existing_props && source_props &&
                    (*existing_props).Size() == (*source_props).Size()) {
                    return true; // Already imported.
                }
            }
        }
        const auto copied = AwaitBounded(
            file.CopyAsync(destination, destination_name,
                           NameCollisionOption::ReplaceExisting),
            std::chrono::minutes(20)); // A full-size game copy from USB genuinely takes a while.
        return copied.has_value();
    } catch (const winrt::hresult_error& error) {
        Diagnostic("USB_COPY_FAILED " + winrt::to_string(item ? file.Name() : L"?") + " " +
                  winrt::to_string(error.message()));
        return false;
    }
}
} // namespace

int ImportFromUsb(const UsbLibraryScan& scan) {
    if (!scan.drive_found) {
        return 0;
    }
    int copied = 0;
    try {
        auto drive_item = AwaitBounded(
            StorageFolder::GetFolderFromPathAsync(scan.drive_path), std::chrono::seconds(5));
        if (!drive_item) {
            Diagnostic("USB_IMPORT_FAILED could not reopen " + winrt::to_string(scan.drive_path));
            return 0;
        }
        const auto& drive = *drive_item;
        const auto local = ApplicationData::Current().LocalFolder();
        auto keys_dir =
            AwaitBounded(local.CreateFolderAsync(L"eden\\keys", CreationCollisionOption::OpenIfExists),
                        std::chrono::seconds(5));
        auto games_dir =
            AwaitBounded(local.CreateFolderAsync(L"games", CreationCollisionOption::OpenIfExists),
                        std::chrono::seconds(5));
        if (!keys_dir || !games_dir) {
            Diagnostic("USB_IMPORT_FAILED could not create LocalState folders");
            return 0;
        }
        if (scan.prod_keys_found && CopyItemInto(drive, L"prod.keys", *keys_dir, L"prod.keys")) {
            ++copied;
        }
        if (scan.title_keys_found && CopyItemInto(drive, L"title.keys", *keys_dir, L"title.keys")) {
            ++copied;
        }
        for (const auto& game : scan.games) {
            const auto slash = game.find_last_of(L'\\');
            const auto file_name = slash == std::wstring::npos ? game : game.substr(slash + 1);
            if (CopyItemInto(drive, game.c_str(), *games_dir, file_name.c_str())) {
                ++copied;
            }
        }
        Diagnostic("USB_IMPORT copied=" + std::to_string(copied));
    } catch (const winrt::hresult_error& error) {
        Diagnostic("USB_IMPORT_FAILED " + winrt::to_string(error.message()));
    }
    return copied;
}

} // namespace EdenXbox
