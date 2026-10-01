// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <d2d1.h>

#include "eden_uwp/ui/renderer.h"
#include "eden_uwp/ui/theme.h"

namespace EdenXbox::Ui {

// The pieces every screen shares: the focus ring, pills, the hint bar, the tab capsule, the clock,
// toasts and sheets. They were part of the library screen and moved here unchanged so the mod
// store and the save sync screens look the same.

inline constexpr float kRingGap = 6.0f; // from the element to the outer edge of the ring
inline constexpr float kRingWidth = 3.0f;

// The pills of the hero and of the dialogs.
inline constexpr float kPillHeight = 64.0f;
inline constexpr float kPillPadding = 34.0f;
inline constexpr float kPillGap = 18.0f;
inline constexpr float kPlayGlyph = 22.0f;
inline constexpr float kPlayGap = 12.0f;

// The left edge of the content and the center line of the top row (the tab capsule on the left,
// the clock on the right).
inline constexpr float kScreenMargin = 96.0f;
inline constexpr float kTopCenter = 74.0f;

// The brand ribbon, green to red through two warm stops: the focus ring and the progress bars.
inline constexpr D2D1_GRADIENT_STOP kRibbonStops[4] = {{0.0f, Theme::kRibbon0},
                                                       {0.38f, Theme::kRibbon1},
                                                       {0.64f, Theme::kRibbon2},
                                                       {1.0f, Theme::kRibbon3}};

struct Hint {
    D2D1_COLOR_F color;
    const wchar_t* glyph;
    const wchar_t* label;
};

D2D1_RECT_F Inflate(const D2D1_RECT_F& rect, float amount);
D2D1_COLOR_F Mix(const D2D1_COLOR_F& from, const D2D1_COLOR_F& to, float t);
D2D1_COLOR_F WithOpacity(D2D1_COLOR_F color, float opacity);
std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& file);
std::wstring Widen(const std::string& utf8);
// 58872 as "58.872" in Portuguese and "58,872" in English.
std::wstring FormatCount(std::int64_t value);
// A byte count as "12,5 MB" (decimal comma in Portuguese).
std::wstring FormatBytes(std::uint64_t bytes);

// The light line along the top edge of glass, fading out down the sides.
void DrawGlassEdge(Renderer& renderer, const D2D1_RECT_F& rect, float radius);
// The ribbon focus ring around `element`; `outer_radius` is the radius of the ring's outer edge.
void DrawRing(Renderer& renderer, const D2D1_RECT_F& element, float outer_radius, float opacity);

// Width of a pill with this label; the primary one carries the play glyph.
float PillWidth(const Renderer& renderer, const std::wstring& label, bool primary);
// A pill of the hero: the primary one is white with a play glyph, the others are glass.
void DrawPill(Renderer& renderer, const D2D1_RECT_F& rect, const std::wstring& label,
              bool primary, bool focused);
// The same shapes without the glyph, for dialogs that choose between answers.
void DrawChoicePill(Renderer& renderer, const D2D1_RECT_F& rect, const std::wstring& label,
                    bool primary, bool focused);

// The dark glass bar at the bottom: a round colored glyph for each button, with its label.
void DrawHints(Renderer& renderer, const std::vector<Hint>& hints);
// The two tabs in their glass capsule. `active` is the selected one; `focused` draws the ring.
void DrawTabs(Renderer& renderer, int active, bool focused);
void DrawClock(Renderer& renderer);
// A short message in a glass capsule in the top row. `fade` is 0..1.
void DrawToastCapsule(Renderer& renderer, const std::wstring& text, float fade);
// A progress bar with the ribbon across the filled part.
void DrawProgressBar(Renderer& renderer, const D2D1_RECT_F& track, double fraction);
// A dark veil over the whole canvas and a sheet on top of it.
void DrawSheet(Renderer& renderer, const D2D1_RECT_F& sheet);

} // namespace EdenXbox::Ui
