// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifdef YUZU_UWP_APPCONTAINER
#include <windows.h>

#include <fileapifromapp.h>

#include <filesystem>
#include <string_view>

namespace Common::FS::Uwp {

// Use the enumeration's metadata directly: directory_entry would query the path through Win32.
template <typename Callback>
bool ForEachEntry(const std::filesystem::path& directory, Callback&& callback) {
    WIN32_FIND_DATAW data{};
    const auto pattern = directory / L"*";
    const HANDLE search = FindFirstFileExFromAppW(pattern.c_str(), FindExInfoBasic, &data,
                                                 FindExSearchNameMatch, nullptr, 0);
    if (search == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    struct SearchHandle {
        HANDLE value;
        ~SearchHandle() { FindClose(value); }
    } guard{search};
    do {
        const std::wstring_view name{data.cFileName};
        if (name != L"." && name != L".." && !callback(directory / data.cFileName, data)) {
            return false;
        }
    } while (FindNextFileW(search, &data));
    return GetLastError() == ERROR_NO_MORE_FILES;
}

} // namespace Common::FS::Uwp
#endif
