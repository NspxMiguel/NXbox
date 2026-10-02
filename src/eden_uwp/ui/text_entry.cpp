// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/ui/text_entry.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <cwctype>
#include <utility>

#include "eden_uwp/ui/strings.h"
#include "eden_uwp/ui/theme.h"
#include "eden_uwp/ui/widgets.h"

namespace EdenXbox::Ui {
namespace {

using D2D1::RectF;

constexpr float kSheetLeft = 260.0f;
constexpr float kSheetTop = 110.0f;
constexpr float kSheetWidth = 1400.0f;
constexpr float kSheetHeight = 800.0f;
constexpr float kPadding = 56.0f;
constexpr float kFieldTop = kSheetTop + 90.0f;
constexpr float kFieldHeight = 84.0f;
constexpr float kKeysTop = kSheetTop + 206.0f;
constexpr float kKeyHeight = 72.0f;
constexpr float kKeyGap = 12.0f;
constexpr float kKeyUnit = 112.0f; // width of a plain key
constexpr std::size_t kMaxLength = 512;
constexpr auto kBlink = std::chrono::milliseconds(530);

} // namespace

TextEntry::TextEntry(std::wstring title, std::wstring initial)
    : title_(std::move(title)), text_(std::move(initial)) {
    if (text_.size() > kMaxLength) {
        text_.resize(kMaxLength);
    }
    caret_ = text_.size();
    const auto chars = [](const wchar_t* list) {
        std::vector<Key> keys;
        for (const wchar_t* c = list; *c != L'\0'; ++c) {
            keys.push_back({std::wstring(1, *c), Action::Char, 1.0f, std::wstring(1, *c)});
        }
        return keys;
    };
    rows_.push_back(chars(L"1234567890"));
    rows_.push_back(chars(L"qwertyuiop"));
    rows_.push_back(chars(L"asdfghjkl-"));
    rows_.push_back(chars(L"zxcvbnm._/"));
    rows_.push_back(chars(L":?=&%~+#@,"));
    rows_.push_back({{Tr(Text::KeyShift), Action::Shift, 1.5f, {}},
                     {L"http://", Action::Insert, 1.5f, L"http://"},
                     {L"https://", Action::Insert, 1.5f, L"https://"},
                     {L".com", Action::Insert, 1.5f, L".com"},
                     {Tr(Text::KeyBackspace), Action::Backspace, 1.5f, {}},
                     {Tr(Text::HintCancel), Action::Cancel, 1.5f, {}},
                     {Tr(Text::KeyOk), Action::Ok, 1.0f, {}}});
    // Start on the "q" row so a first A types something useful.
    row_ = 1;
    column_ = 0;
}

float TextEntry::CenterOf(int row, int column) const {
    float x = 0.0f;
    const auto& keys = rows_[static_cast<std::size_t>(row)];
    for (int i = 0; i < column; ++i) {
        x += keys[static_cast<std::size_t>(i)].width;
    }
    return x + keys[static_cast<std::size_t>(column)].width * 0.5f;
}

void TextEntry::Move(int dx, int dy) {
    const int rows = static_cast<int>(rows_.size());
    if (dy != 0) {
        const float center = CenterOf(row_, column_);
        row_ = std::clamp(row_ + dy, 0, rows - 1);
        const auto& keys = rows_[static_cast<std::size_t>(row_)];
        int best = 0;
        float best_distance = 1e9f;
        for (int i = 0; i < static_cast<int>(keys.size()); ++i) {
            const float distance = std::fabs(CenterOf(row_, i) - center);
            if (distance < best_distance) {
                best_distance = distance;
                best = i;
            }
        }
        column_ = best;
    }
    if (dx != 0) {
        const int count = static_cast<int>(rows_[static_cast<std::size_t>(row_)].size());
        column_ = (column_ + dx + count) % count;
    }
}

void TextEntry::Type(const std::wstring& text) {
    if (text_.size() + text.size() > kMaxLength) {
        return;
    }
    text_.insert(caret_, text);
    caret_ += text.size();
}

void TextEntry::Backspace() {
    if (caret_ > 0) {
        text_.erase(caret_ - 1, 1);
        --caret_;
    }
}

void TextEntry::Press(const Key& key) {
    switch (key.action) {
    case Action::Char: {
        std::wstring typed = key.insert;
        if (shift_ && typed.size() == 1) {
            typed[0] = static_cast<wchar_t>(std::towupper(typed[0]));
        }
        Type(typed);
        break;
    }
    case Action::Insert:
        Type(key.insert);
        break;
    case Action::Shift:
        shift_ = !shift_;
        break;
    case Action::Backspace:
        Backspace();
        break;
    case Action::Ok:
    case Action::Cancel:
        break; // handled by Update
    }
}

TextEntry::Result TextEntry::Update(const Input& input) {
    if (input.Pressed(Button::Menu) || input.Pressed(Button::X)) {
        return Result::Confirmed;
    }
    if (input.Pressed(Button::B)) {
        if (text_.empty()) {
            return Result::Cancelled;
        }
        Backspace();
    }
    if (input.Pressed(Button::Y)) {
        shift_ = !shift_;
    }
    if (input.Pressed(Button::LeftShoulder) && caret_ > 0) {
        --caret_;
    }
    if (input.Pressed(Button::RightShoulder) && caret_ < text_.size()) {
        ++caret_;
    }
    if (input.Pressed(Button::Left)) {
        Move(-1, 0);
    }
    if (input.Pressed(Button::Right)) {
        Move(1, 0);
    }
    if (input.Pressed(Button::Up)) {
        Move(0, -1);
    }
    if (input.Pressed(Button::Down)) {
        Move(0, 1);
    }
    if (input.Pressed(Button::A)) {
        const Key& key = rows_[static_cast<std::size_t>(row_)][static_cast<std::size_t>(column_)];
        if (key.action == Action::Ok) {
            return Result::Confirmed;
        }
        if (key.action == Action::Cancel) {
            return Result::Cancelled;
        }
        Press(key);
    }
    return Result::Editing;
}

void TextEntry::Draw(Renderer& renderer, Clock::time_point now) const {
    DrawSheet(renderer, RectF(kSheetLeft, kSheetTop, kSheetLeft + kSheetWidth,
                              kSheetTop + kSheetHeight));
    const float left = kSheetLeft + kPadding;
    const float right = kSheetLeft + kSheetWidth - kPadding;
    renderer.DrawString(title_, Font::Heading,
                        RectF(left, kSheetTop + 40.0f, right, kSheetTop + 100.0f), Theme::kText,
                        HAlign::Left, VAlign::Middle);

    // The field: the text with a caret. A long text scrolls so the caret stays visible.
    const D2D1_RECT_F field = RectF(left, kFieldTop, right, kFieldTop + kFieldHeight);
    renderer.FillRounded(field, 20.0f, Theme::kSurfaceStrong);
    renderer.StrokeRounded(Inflate(field, -0.5f), 19.5f, 1.0f, Theme::kHairline);
    const float inner_left = field.left + 28.0f;
    const float inner_right = field.right - 28.0f;
    const float before = caret_ == 0
                             ? 0.0f
                             : renderer.MeasureString(text_.substr(0, caret_), Font::MetaMono).width;
    const float overflow = std::max(0.0f, before - (inner_right - inner_left) + 6.0f);
    renderer.PushClip(RectF(inner_left, field.top, inner_right, field.bottom));
    renderer.DrawString(text_, Font::MetaMono,
                        RectF(inner_left - overflow, field.top, inner_left - overflow + 4000.0f,
                              field.bottom),
                        Theme::kText, HAlign::Left, VAlign::Middle);
    const bool caret_on = (std::chrono::duration_cast<std::chrono::milliseconds>(
                               now.time_since_epoch()) /
                           kBlink) %
                              2 ==
                          0;
    if (caret_on) {
        const float x = inner_left - overflow + before;
        renderer.FillRounded(RectF(x, field.top + 20.0f, x + 3.0f, field.bottom - 20.0f), 1.5f,
                             Theme::kText);
    }
    renderer.PopClip();

    // The keys. Each row is centered; a row's width is the sum of its key widths.
    for (std::size_t r = 0; r < rows_.size(); ++r) {
        const auto& keys = rows_[r];
        float row_width = 0.0f;
        for (const Key& key : keys) {
            row_width += key.width * kKeyUnit;
        }
        row_width += kKeyGap * static_cast<float>(keys.size() - 1);
        float x = kSheetLeft + (kSheetWidth - row_width) * 0.5f;
        const float y = kKeysTop + static_cast<float>(r) * (kKeyHeight + kKeyGap);
        for (std::size_t c = 0; c < keys.size(); ++c) {
            const Key& key = keys[c];
            const float width = key.width * kKeyUnit;
            const D2D1_RECT_F rect = RectF(x, y, x + width, y + kKeyHeight);
            const bool focused = static_cast<int>(r) == row_ && static_cast<int>(c) == column_;
            const bool lit = key.action == Action::Shift && shift_;
            if (focused) {
                renderer.FillRounded(rect, 18.0f, Theme::kPrimaryFill);
            } else {
                renderer.FillRounded(rect, 18.0f, lit ? Theme::kHighlight : Theme::kGlass);
            }
            std::wstring label = key.label;
            if (shift_ && key.action == Action::Char && label.size() == 1) {
                label[0] = static_cast<wchar_t>(std::towupper(label[0]));
            }
            renderer.DrawString(label, Font::Button, rect,
                                focused ? Theme::kTextOnLight : Theme::kText, HAlign::Center,
                                VAlign::Middle);
            x += width + kKeyGap;
        }
    }

    DrawHints(renderer, {{Theme::kButtonA, L"A", Tr(Text::HintType)},
                         {Theme::kButtonB, L"B", Tr(Text::KeyBackspace)},
                         {Theme::kButtonY, L"Y", Tr(Text::KeyShift)},
                         {Theme::kButtonX, L"X", Tr(Text::KeyOk)}});
}

} // namespace EdenXbox::Ui
