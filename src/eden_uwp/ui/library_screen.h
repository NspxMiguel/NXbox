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
// The first time (LocalState\setup_done.txt missing) it asks whether to sync saves with
// SwitchSaveSync before the library shows.
//
// Returns the absolute path (UTF-8) of the game the player launched, or an empty string when the
// player left with B or the screen could not be shown. When a game was launched and `chosen` is
// given, it is filled with what the save sync needs. Must be called on the thread that owns the
// window, before the window is handed to Mesa.
struct ChosenGame {
    std::string path;
    std::string title_id;  // 16 upper-case hex digits
    std::string sync_name; // the game's name as SwitchSaveSync names it (from the NACP names)
    std::wstring display_name;
};

std::string RunLibrary(const winrt::Windows::UI::Core::CoreWindow& window,
                       ChosenGame* chosen = nullptr);

} // namespace EdenXbox::Ui
