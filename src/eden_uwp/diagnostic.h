// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <fstream>
#include <mutex>
#include <string>
#include <windows.h>
#include <winrt/Windows.Storage.h>

namespace EdenXbox {
inline void Diagnostic(const std::string& message) noexcept {
    try {
        static std::mutex mutex;
        const std::lock_guard lock(mutex);
        OutputDebugStringA((message + "\n").c_str());
        const auto path = winrt::to_string(
            winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path());
        std::ofstream(path + "\\eden_uwp_diag.txt", std::ios::app) << message << '\n';
    } catch (...) {
        OutputDebugStringA("NXbox diagnostic file unavailable\n");
    }
}
} // namespace EdenXbox
