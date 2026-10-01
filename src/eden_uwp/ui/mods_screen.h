// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>

#include <d2d1.h>
#include <windows.h>

#include <winrt/Windows.UI.Core.h>

#include "eden_uwp/ui/input.h"
#include "eden_uwp/ui/renderer.h"

namespace EdenXbox::Ui {

// The game a mod store belongs to, as the library knows it.
struct ModsGame {
    std::string title_id; // 16 upper-case hex digits
    std::wstring name;    // shown in the header and used to find the game on GameBanana
    const Image* icon = nullptr; // the game's icon on the GPU, or null
    D2D1_COLOR_F glow{};         // the color the top of the screen glows with
};

// The mod store of one game (docs/nxbox-ui.md, Increment 2): category chips, a list of mods with
// thumbnail, author, category, count and an action pill, and a details sheet. It runs its own loop
// on the library's renderer and input until the player leaves with B. Returns true when the
// window was closed meanwhile, so the caller can stop too.
bool RunModsScreen(Renderer& renderer, const winrt::Windows::UI::Core::CoreWindow& window,
                   Input& input, const ModsGame& game, const std::filesystem::path& local_state);

} // namespace EdenXbox::Ui
