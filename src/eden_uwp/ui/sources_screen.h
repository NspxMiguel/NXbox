// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>

#include <d2d1.h>
#include <windows.h>

#include <winrt/Windows.UI.Core.h>

#include "eden_uwp/ui/input.h"
#include "eden_uwp/ui/renderer.h"

namespace EdenXbox::Ui {

// Settings -> Sources. The player adds their own sources, each a URL that serves a list in the
// Tinfoil shop format (a JSON index of files and directories), either by typing the address on
// the on-screen keyboard or by importing a sources.txt from a USB drive. A source can be opened to
// browse what it lists and an item can be downloaded into LocalState\games. Nothing is
// preconfigured: the list is empty until the player fills it. The addresses live in
// LocalState\sources.json.
//
// Runs its own loop on the library's renderer and input until the player leaves with B. Returns
// true when the window was closed meanwhile, so the caller can stop too. The caller rescans the
// library afterwards, since a download may have added a game.
bool RunSourcesScreen(Renderer& renderer, const winrt::Windows::UI::Core::CoreWindow& window,
                      Input& input, const std::filesystem::path& local_state);

// The number of sources saved in LocalState\sources.json, for the Settings row.
int CountSources(const std::filesystem::path& local_state);

} // namespace EdenXbox::Ui
