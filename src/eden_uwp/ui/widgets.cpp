// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/ui/widgets.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cwchar>
#include <fstream>
#include <iterator>

#include <windows.h>

#include "eden_uwp/ui/strings.h"

namespace EdenXbox::Ui {
namespace {

using D2D1::Point2F;
using D2D1::RectF;

constexpr float kTabsPadding = 6.0f;
constexpr float kTabsGap = 8.0f;
constexpr float kTabHeight = 48.0f;
constexpr float kTabLabelPadding = 24.0f;

constexpr float kHintHeight = 62.0f;
constexpr float kHintTop = kCanvasHeight - 44.0f - kHintHeight;
constexpr float kGlyphSize = 30.0f;
constexpr float kHintPadding = 30.0f;
constexpr float kHintLabelGap = 10.0f;
constexpr float kHintItemGap = 34.0f;

} // namespace

D2D1_RECT_F Inflate(const D2D1_RECT_F& rect, float amount) {
    return RectF(rect.left - amount, rect.top - amount, rect.right + amount, rect.bottom + amount);
}

D2D1_COLOR_F Mix(const D2D1_COLOR_F& from, const D2D1_COLOR_F& to, float t) {
    return {from.r + (to.r - from.r) * t, from.g + (to.g - from.g) * t,
            from.b + (to.b - from.b) * t, from.a + (to.a - from.a) * t};
}

D2D1_COLOR_F WithOpacity(D2D1_COLOR_F color, float opacity) {
    color.a *= opacity;
    return color;
}

std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) {
        return {};
    }
    const std::streamsize size = in.tellg();
    if (size <= 0) {
        return {};
    }
    in.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}

std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                          nullptr, 0);
    if (count <= 0) {
        return std::wstring(utf8.begin(), utf8.end());
    }
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                        count);
    return wide;
}

// 58872 as "58.872" in Portuguese and "58,872" in English.
std::wstring FormatCount(std::int64_t value) {
    std::wstring digits = std::to_wstring(value < 0 ? 0 : value);
    const wchar_t separator = CurrentLanguage() == Language::Portuguese ? L'.' : L',';
    std::wstring out;
    int count = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (count > 0 && count % 3 == 0) {
            out.insert(out.begin(), separator);
        }
        out.insert(out.begin(), *it);
        ++count;
    }
    return out;
}

std::wstring FormatBytes(std::uint64_t bytes) {
    constexpr double kMiB = 1024.0 * 1024.0;
    wchar_t buffer[32];
    if (static_cast<double>(bytes) >= 1024.0 * kMiB) {
        swprintf_s(buffer, std::size(buffer), L"%.1f GB",
                   static_cast<double>(bytes) / (1024.0 * kMiB));
    } else if (static_cast<double>(bytes) >= kMiB) {
        swprintf_s(buffer, std::size(buffer), L"%.1f MB", static_cast<double>(bytes) / kMiB);
    } else {
        swprintf_s(buffer, std::size(buffer), L"%.0f KB", static_cast<double>(bytes) / 1024.0);
    }
    std::wstring text = buffer;
    if (CurrentLanguage() == Language::Portuguese) {
        std::replace(text.begin(), text.end(), L'.', L',');
    }
    return text;
}

void DrawGlassEdge(Renderer& renderer, const D2D1_RECT_F& rect, float radius) {
    const D2D1_GRADIENT_STOP light[2] = {{0.0f, Theme::kHighlight},
                                         {0.5f, Theme::Rgb(0xFFFFFF, 0.0f)}};
    renderer.StrokeGradient(Inflate(rect, -0.5f), radius - 0.5f, 1.0f, light, 2,
                            Point2F(0.0f, rect.top), Point2F(0.0f, rect.bottom));
}

// A 3 px band whose outer edge lies 6 px outside the element and keeps the element's corner
// radius, green to red at 100 degrees.
void DrawRing(Renderer& renderer, const D2D1_RECT_F& element, float outer_radius, float opacity) {
    const D2D1_RECT_F outer = Inflate(element, kRingGap);
    const D2D1_RECT_F center_line = Inflate(element, kRingGap - kRingWidth / 2.0f);
    // A CSS linear-gradient(100deg, ...) runs along the angle through the middle of the box and
    // just spans the box.
    constexpr float kSin = 0.98480775f;  // sin(100 degrees)
    constexpr float kCos = -0.17364818f; // cos(100 degrees)
    const float half = (std::fabs((outer.right - outer.left) * kSin) +
                        std::fabs((outer.bottom - outer.top) * kCos)) /
                       2.0f;
    const float x = (outer.left + outer.right) / 2.0f;
    const float y = (outer.top + outer.bottom) / 2.0f;
    renderer.StrokeGradient(center_line, std::max(outer_radius - kRingWidth / 2.0f, 0.0f),
                            kRingWidth, kRibbonStops, 4, Point2F(x - kSin * half, y + kCos * half),
                            Point2F(x + kSin * half, y - kCos * half), opacity);
}

float PillWidth(const Renderer& renderer, const std::wstring& label, bool primary) {
    return renderer.MeasureString(label, Font::Button).width + 2.0f * kPillPadding +
           (primary ? kPlayGlyph + kPlayGap : 0.0f);
}

void DrawPill(Renderer& renderer, const D2D1_RECT_F& rect, const std::wstring& label,
              bool primary, bool focused) {
    const float radius = (rect.bottom - rect.top) / 2.0f;
    const float middle = (rect.top + rect.bottom) / 2.0f;
    if (primary) {
        renderer.FillRounded(rect, radius, Theme::kPrimaryFill);
        const float left = rect.left + kPillPadding;
        // The triangle of the play icon, from a 24 unit drawing shown at 22 px.
        const float unit = kPlayGlyph / 24.0f;
        const float top = middle - kPlayGlyph / 2.0f;
        const D2D1_POINT_2F triangle[3] = {Point2F(left + 8.0f * unit, top + 5.5f * unit),
                                           Point2F(left + 8.0f * unit, top + 18.5f * unit),
                                           Point2F(left + 19.0f * unit, top + 12.0f * unit)};
        renderer.FillPolygon(triangle, 3, Theme::kTextOnLight);
        // The box runs to the edge of the pill, not to the padding: a box exactly as wide as the
        // text can make DirectWrite trim it with an ellipsis over a rounding error.
        renderer.DrawString(label, Font::Button,
                            RectF(left + kPlayGlyph + kPlayGap, rect.top, rect.right, rect.bottom),
                            Theme::kTextOnLight, HAlign::Left, VAlign::Middle);
    } else {
        renderer.FillRounded(rect, radius, Theme::kGlass);
        DrawGlassEdge(renderer, rect, radius);
        renderer.DrawString(label, Font::Button, rect, Theme::kText, HAlign::Center,
                            VAlign::Middle);
    }
    if (focused) {
        DrawRing(renderer, rect, radius + kRingGap, 1.0f);
    }
}

void DrawChoicePill(Renderer& renderer, const D2D1_RECT_F& rect, const std::wstring& label,
                    bool primary, bool focused) {
    const float radius = (rect.bottom - rect.top) / 2.0f;
    if (primary) {
        renderer.FillRounded(rect, radius, Theme::kPrimaryFill);
        renderer.DrawString(label, Font::Button, rect, Theme::kTextOnLight, HAlign::Center,
                            VAlign::Middle);
    } else {
        renderer.FillRounded(rect, radius, Theme::kGlass);
        DrawGlassEdge(renderer, rect, radius);
        renderer.DrawString(label, Font::Button, rect, Theme::kText, HAlign::Center,
                            VAlign::Middle);
    }
    if (focused) {
        DrawRing(renderer, rect, radius + kRingGap, 1.0f);
    }
}

void DrawHints(Renderer& renderer, const std::vector<Hint>& hints) {
    if (hints.empty()) {
        return;
    }
    std::vector<float> widths;
    float total = 2.0f * kHintPadding + static_cast<float>(hints.size() - 1) * kHintItemGap;
    for (const Hint& hint : hints) {
        widths.push_back(renderer.MeasureString(hint.label, Font::Hint).width);
        total += kGlyphSize + kHintLabelGap + widths.back();
    }
    const float left = (kCanvasWidth - total) / 2.0f;
    const D2D1_RECT_F bar = RectF(left, kHintTop, left + total, kHintTop + kHintHeight);
    renderer.FillRounded(bar, kHintHeight / 2.0f, Theme::kGlassBar);
    DrawGlassEdge(renderer, bar, kHintHeight / 2.0f);
    const float glyph_top = kHintTop + (kHintHeight - kGlyphSize) / 2.0f;
    float x = left + kHintPadding;
    for (std::size_t i = 0; i < hints.size(); ++i) {
        renderer.FillDisc(Point2F(x + kGlyphSize / 2.0f, kHintTop + kHintHeight / 2.0f),
                          kGlyphSize / 2.0f, hints[i].color);
        renderer.DrawString(hints[i].glyph, Font::Glyph,
                            RectF(x, glyph_top, x + kGlyphSize, glyph_top + kGlyphSize),
                            Theme::kTextOnLight, HAlign::Center, VAlign::Middle);
        x += kGlyphSize + kHintLabelGap;
        renderer.DrawString(hints[i].label, Font::Hint,
                            RectF(x, kHintTop, x + widths[i] + 8.0f, kHintTop + kHintHeight),
                            Theme::kTextSecondary, HAlign::Left, VAlign::Middle);
        x += widths[i] + kHintItemGap;
    }
}

void DrawTabs(Renderer& renderer, int active, bool focused) {
    constexpr std::size_t kTabCount = 2;
    const std::array<const wchar_t*, kTabCount> labels = {Tr(Text::NavLibrary),
                                                          Tr(Text::NavSettings)};
    std::array<float, kTabCount> widths{};
    float total = 2.0f * kTabsPadding + kTabsGap * static_cast<float>(kTabCount - 1);
    for (std::size_t i = 0; i < kTabCount; ++i) {
        widths[i] = renderer.MeasureString(labels[i], Font::Nav).width + 2.0f * kTabLabelPadding;
        total += widths[i];
    }
    const float capsule_height = kTabHeight + 2.0f * kTabsPadding;
    const D2D1_RECT_F capsule = RectF(kScreenMargin, kTopCenter - capsule_height / 2.0f,
                                      kScreenMargin + total, kTopCenter + capsule_height / 2.0f);
    renderer.FillRounded(capsule, capsule_height / 2.0f, Theme::kGlassTabs);
    DrawGlassEdge(renderer, capsule, capsule_height / 2.0f);
    float x = capsule.left + kTabsPadding;
    for (std::size_t i = 0; i < kTabCount; ++i) {
        const bool is_active = static_cast<int>(i) == active;
        const D2D1_RECT_F pill = RectF(x, capsule.top + kTabsPadding, x + widths[i],
                                       capsule.top + kTabsPadding + kTabHeight);
        if (is_active) {
            renderer.FillRounded(pill, kTabHeight / 2.0f, Theme::kPrimaryFill);
        }
        renderer.DrawString(labels[i], Font::Nav, pill,
                            is_active ? Theme::kTextOnLight : Theme::kTextSecondary,
                            HAlign::Center, VAlign::Middle);
        if (is_active && focused) {
            DrawRing(renderer, pill, kTabHeight / 2.0f + kRingGap, 1.0f);
        }
        x += widths[i] + kTabsGap;
    }
}

void DrawClock(Renderer& renderer) {
    SYSTEMTIME local_time{};
    GetLocalTime(&local_time);
    wchar_t text[8];
    swprintf_s(text, std::size(text), L"%02u:%02u", static_cast<unsigned>(local_time.wHour),
               static_cast<unsigned>(local_time.wMinute));
    renderer.DrawString(text, Font::Clock,
                        RectF(kCanvasWidth - kScreenMargin - 240.0f, kTopCenter - 20.0f,
                              kCanvasWidth - kScreenMargin, kTopCenter + 20.0f),
                        Theme::kText, HAlign::Right, VAlign::Middle);
}

void DrawToastCapsule(Renderer& renderer, const std::wstring& text, float fade) {
    const float height = kTabHeight + 2.0f * kTabsPadding;
    const float width = renderer.MeasureString(text, Font::Nav).width + 2.0f * kTabLabelPadding +
                        2.0f * kTabsPadding;
    const float left = (kCanvasWidth - width) / 2.0f;
    const D2D1_RECT_F rect =
        RectF(left, kTopCenter - height / 2.0f, left + width, kTopCenter + height / 2.0f);
    renderer.FillRounded(rect, height / 2.0f, WithOpacity(Theme::kGlassBar, fade));
    renderer.DrawString(text, Font::Nav, rect, WithOpacity(Theme::kText, fade), HAlign::Center,
                        VAlign::Middle);
}

void DrawProgressBar(Renderer& renderer, const D2D1_RECT_F& track, double fraction) {
    const float height = track.bottom - track.top;
    const float width = track.right - track.left;
    fraction = std::clamp(fraction, 0.0, 1.0);
    renderer.FillRounded(track, height / 2.0f, Theme::kSurfaceStrong);
    if (fraction > 0.0) {
        // The ribbon runs across the filled part, so it always ends in red.
        const D2D1_RECT_F fill = RectF(
            track.left, track.top,
            track.left + std::max(static_cast<float>(fraction) * width, height), track.bottom);
        renderer.FillGradient(fill, height / 2.0f, kRibbonStops, 4, Point2F(fill.left, track.top),
                              Point2F(fill.right, track.top), 0.9f);
    }
}

void DrawSheet(Renderer& renderer, const D2D1_RECT_F& sheet) {
    renderer.FillRounded(RectF(0.0f, 0.0f, kCanvasWidth, kCanvasHeight), 0.0f, Theme::kVeil);
    renderer.FillRounded(sheet, 32.0f, Theme::kSheet);
    renderer.StrokeRounded(Inflate(sheet, -0.5f), 31.5f, 1.0f, Theme::kHairline);
}

} // namespace EdenXbox::Ui
