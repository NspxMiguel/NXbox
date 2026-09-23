// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <windows.h>
#include <winrt/Windows.UI.Core.h>

namespace EdenXbox {
void RunGameView(const winrt::Windows::UI::Core::CoreWindow& window, const std::string& path);
}
