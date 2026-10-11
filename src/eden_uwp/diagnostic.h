// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <windows.h>

#include <fileapifromapp.h>
#include <winrt/Windows.Storage.h>

namespace EdenXbox {
inline void Diagnostic(const std::string& message) noexcept {
    try {
        static std::mutex mutex;
        const std::lock_guard lock(mutex);
        OutputDebugStringA((message + "\n").c_str());
        const std::filesystem::path folder{
            winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str()};
        const auto file = folder / "eden_uwp_diag.txt";
        // The log only ever grew; on the first line of each process a log past 4 MiB is moved to
        // eden_uwp_diag.old.txt, so a session's lines stay together and the disk is not eaten.
        static bool rotated = false;
        if (!rotated) {
            rotated = true;
            std::error_code ec;
            if (std::filesystem::file_size(file, ec) > (4u << 20) && !ec) {
                std::filesystem::rename(file, folder / "eden_uwp_diag.old.txt", ec);
            }
        }
        static const HANDLE handle = CreateFile2FromAppW(
            file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            DWORD written;
            OVERLAPPED append{};
            append.Offset = append.OffsetHigh = MAXDWORD;
            WriteFile(handle, message.data(), static_cast<DWORD>(message.size()), &written,
                      &append);
            WriteFile(handle, "\n", 1, &written, &append);
            // Closing an ofstream only drains the CRT buffer; persist the OS buffer too.
            FlushFileBuffers(handle);
        } else {
            OutputDebugStringA("NXbox diagnostic file unavailable\n");
        }
    } catch (...) {
        OutputDebugStringA("NXbox diagnostic file unavailable\n");
    }
}
} // namespace EdenXbox
