// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <windows.h>
#include <winrt/Windows.UI.Core.h>

namespace EdenXbox {
// Records the title ID of an "nxbox://play?title=<ID>" protocol activation (a per-game tile
// launched NXbox). RunGameView boots that library game without showing the library. Safe to call
// from any thread, before or while RunGameView runs; only the first call per process counts.
void SetProtocolPlayTitle(std::string title_id);
void RunGameView(const winrt::Windows::UI::Core::CoreWindow& window, const std::string& path);
}
