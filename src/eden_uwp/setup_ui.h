// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <winrt/Windows.UI.Core.h>

namespace EdenXbox {

// The setup screen: drawn with Direct2D/DirectWrite directly on the CoreWindow, independent of
// the Mesa/OpenGL context the emulator itself uses (see docs/nxbox-ui.md — NXbox has no XAML
// toolchain, so this is the UI layer instead of a Frame of XAML pages). Blocks until the window
// closes or the player confirms; the caller then proceeds to the normal game boot.
void ShowSetupScreen(const winrt::Windows::UI::Core::CoreWindow& window, const std::string& status_line);

} // namespace EdenXbox
