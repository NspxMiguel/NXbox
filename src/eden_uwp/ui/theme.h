// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d2d1.h>

namespace EdenXbox::Ui::Theme {

// Every color of the UI lives here, so nothing is hard-coded in a component. They are straight
// (non-premultiplied) RGBA, which is what Direct2D brushes take.
constexpr D2D1_COLOR_F Rgb(unsigned hex, float alpha = 1.0f) {
    return {static_cast<float>((hex >> 16) & 0xFFu) / 255.0f,
            static_cast<float>((hex >> 8) & 0xFFu) / 255.0f,
            static_cast<float>(hex & 0xFFu) / 255.0f, alpha};
}

inline constexpr D2D1_COLOR_F kBackground = Rgb(0x000000); // true black, never blue-grey
inline constexpr D2D1_COLOR_F kText = Rgb(0xF5F5F7);
inline constexpr D2D1_COLOR_F kTextSecondary = Rgb(0xF5F5F7, 0.64f);
inline constexpr D2D1_COLOR_F kTextTertiary = Rgb(0xF5F5F7, 0.40f);
inline constexpr D2D1_COLOR_F kTextOnLight = Rgb(0x000000);

// Primary actions and the selected tab are solid white with black text; everything else that
// floats is translucent. There are no drop shadows: depth comes from translucency, a 1 px hairline
// at 8% white and a light line along the top edge of glass.
inline constexpr D2D1_COLOR_F kPrimaryFill = Rgb(0xF5F5F7);
inline constexpr D2D1_COLOR_F kGlass = Rgb(0xFFFFFF, 0.12f);     // secondary pills
inline constexpr D2D1_COLOR_F kGlassTabs = Rgb(0x141418, 0.50f); // the tab capsule
inline constexpr D2D1_COLOR_F kGlassBar = Rgb(0x1C1C20, 0.62f);  // the hint bar and the toast
inline constexpr D2D1_COLOR_F kSurface = Rgb(0xFFFFFF, 0.055f);  // tiles without art
inline constexpr D2D1_COLOR_F kSurfaceStrong = Rgb(0xFFFFFF, 0.09f); // the track of a progress bar
inline constexpr D2D1_COLOR_F kHairline = Rgb(0xFFFFFF, 0.08f);
inline constexpr D2D1_COLOR_F kHighlight = Rgb(0xFFFFFF, 0.12f);
inline constexpr D2D1_COLOR_F kVeil = Rgb(0x000000, 0.62f);  // dims the screen behind a sheet
inline constexpr D2D1_COLOR_F kSheet = Rgb(0x141416, 0.94f); // the surface of a sheet

// The brand ribbon, the one accent, used only as the focus ring: green to red through two warm
// stops, so the middle does not turn to mud.
inline constexpr D2D1_COLOR_F kRibbon0 = Rgb(0x2FD07A); // at 0%
inline constexpr D2D1_COLOR_F kRibbon1 = Rgb(0x8FBF6A); // at 38%
inline constexpr D2D1_COLOR_F kRibbon2 = Rgb(0xE05A45); // at 64%
inline constexpr D2D1_COLOR_F kRibbon3 = Rgb(0xE8343E); // at 100%

// Controller button glyphs of the hint bar.
inline constexpr D2D1_COLOR_F kButtonA = Rgb(0x5EC26A);
inline constexpr D2D1_COLOR_F kButtonB = Rgb(0xE5534B);
inline constexpr D2D1_COLOR_F kButtonX = Rgb(0x4F8FF7);
inline constexpr D2D1_COLOR_F kButtonY = Rgb(0xF2C94C);

} // namespace EdenXbox::Ui::Theme
