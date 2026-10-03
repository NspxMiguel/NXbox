// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <memory>

#include <d2d1.h>
#include <windows.h>

#include <winrt/Windows.UI.Core.h>

#include "eden_uwp/ui/input.h"
#include "eden_uwp/ui/renderer.h"

namespace EdenXbox::Ui {

enum class UsbMode { Unset, Ask, Copy, External, Off };
UsbMode LoadUsbMode(const std::filesystem::path& local_state);
bool SaveUsbMode(const std::filesystem::path& local_state, UsbMode mode);
const wchar_t* UsbModeLabel(UsbMode mode);

// Polls removable drives on an MTA worker. Destruction cancels without delaying game launch.
class UsbDetection {
public:
    explicit UsbDetection(const std::filesystem::path& local_state);
    ~UsbDetection();
    bool Ready();
    bool Run(Renderer& renderer, const winrt::Windows::UI::Core::CoreWindow& window, Input& input);

private:
    struct State;
    std::shared_ptr<State> state_;
};

// "Import from USB drive": scans the removable drives for games (.nsp .nsz .xci .xcz) and keys
// (prod.keys, title.keys), then lets the player copy them into the console's storage or move them
// into <drive>\NXbox\games so the library plays them from the drive. It runs its own loop on the
// library's renderer and input until the player leaves with B. Returns true when the window was
// closed meanwhile, so the caller can stop too. The caller rescans the library afterwards.
bool RunUsbImportScreen(Renderer& renderer, const winrt::Windows::UI::Core::CoreWindow& window,
                        Input& input, const std::filesystem::path& local_state);

} // namespace EdenXbox::Ui
