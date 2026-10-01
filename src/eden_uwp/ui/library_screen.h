// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include <windows.h>

#include <winrt/Windows.UI.Core.h>

namespace EdenXbox::Ui {

// The library screen, drawn with Direct2D/DirectWrite on the CoreWindow before Mesa/OpenGL takes
// it (see docs/nxbox-ui.md). It blocks, pumping the window's dispatcher and presenting every
// frame, until the player decides, and releases every D3D11/D2D/DXGI object, the swap chain
// included, before it returns.
//
// Returns the absolute path (UTF-8) of the game the player launched, or an empty string when the
// player left with B or the screen could not be shown. Must be called on the thread that owns the
// window, before the window is handed to Mesa.
std::string RunLibrary(const winrt::Windows::UI::Core::CoreWindow& window);

} // namespace EdenXbox::Ui
