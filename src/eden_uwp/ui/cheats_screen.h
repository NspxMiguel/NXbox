// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>

#include <d2d1.h>
#include <windows.h>

#include <winrt/Windows.UI.Core.h>

#include "eden_uwp/ui/input.h"
#include "eden_uwp/ui/mods_screen.h"
#include "eden_uwp/ui/renderer.h"

namespace EdenXbox::Ui {

// The cheats of one game, opened from its mod store with the Menu button: one chip per build ID
// the community database (nx-cheats-db by sthetix, the one CNX Updater uses) has cheats for, and
// a switch per cheat. What is on is written to the game's NXboxCheats mod at once. The credit
// line for the database is drawn on this screen. It runs its own loop on the library's renderer
// and input until the player leaves with B. Returns true when the window was closed meanwhile.
bool RunCheatsScreen(Renderer& renderer, const winrt::Windows::UI::Core::CoreWindow& window,
                     Input& input, const ModsGame& game,
                     const std::filesystem::path& local_state);

// Settings > Credits: who made what NXbox builds on. Returns true when the window was closed.
bool RunCreditsScreen(Renderer& renderer, const winrt::Windows::UI::Core::CoreWindow& window,
                      Input& input);

} // namespace EdenXbox::Ui
