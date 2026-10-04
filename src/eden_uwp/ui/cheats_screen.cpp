// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/ui/cheats_screen.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include "common/scope_exit.h"
#include "eden_uwp/diagnostic.h"
#include "eden_uwp/ui/anim.h"
#include "eden_uwp/ui/cheats.h"
#include "eden_uwp/ui/strings.h"
#include "eden_uwp/ui/theme.h"
#include "eden_uwp/ui/widgets.h"

namespace EdenXbox::Ui {
namespace {

using D2D1::Point2F;
using D2D1::RectF;
using winrt::Windows::UI::Core::CoreProcessEventsOption;
using winrt::Windows::UI::Core::CoreWindow;

// Layout, in canvas units; the header follows the mod store so the two screens read as one.
constexpr float kMargin = kScreenMargin;
constexpr float kRight = kCanvasWidth - kScreenMargin;
constexpr float kIconSize = 44.0f;
constexpr float kGameRowTop = 150.0f;
constexpr float kTitleTop = 204.0f;
constexpr float kTitleHeight = 76.0f;
constexpr float kSourceTop = 282.0f;
constexpr float kSourceHeight = 30.0f;
constexpr float kChipTop = 330.0f;
constexpr float kChipHeight = 50.0f;
constexpr float kChipPadding = 24.0f;
constexpr float kChipGap = 14.0f;
constexpr float kBumperWidth = 41.0f;
constexpr float kBumperHeight = 34.0f;

constexpr float kListTop = 400.0f;
constexpr float kRowHeight = 76.0f;
constexpr float kRowPitch = 82.0f;
constexpr float kRowRadius = 22.0f;
constexpr float kRowPadding = 28.0f;
constexpr int kVisibleRows = 5;
constexpr float kPillWidth = 150.0f;
constexpr float kPillHeight = 46.0f;

// The game versions the database names, then the credit line, between the list and the hint bar.
constexpr float kNotesTop = 836.0f;
constexpr float kFooterTop = 912.0f;
constexpr float kFooterHeight = 34.0f;

constexpr auto kToastDuration = std::chrono::milliseconds(2200);

class CheatsScreen {
public:
    CheatsScreen(Renderer& renderer, const CoreWindow& window, Input& input,
                 const ModsGame& game, const std::filesystem::path& local_state)
        : renderer_(renderer), window_(window), input_(input), game_(game) {
        store_ = std::make_unique<CheatStore>(local_state, game_.title_id);
    }

    // Returns true when the window was closed.
    bool Run() {
        const auto closed_token =
            window_.Closed([this](const auto&, const auto&) { closed_ = true; });
        SCOPE_EXIT {
            window_.Closed(closed_token);
        };
        Diagnostic("UI cheats screen open " + game_.title_id);
        while (!closed_ && !leaving_) {
            const Clock::time_point frame_start = Clock::now();
            window_.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            input_.Update();
            const Clock::time_point now = Clock::now();
            Update(now);
            renderer_.BeginFrame();
            Draw(now);
            renderer_.EndFrame();
            const Clock::duration spent = Clock::now() - frame_start;
            if (spent < std::chrono::milliseconds(8)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16) - spent);
            }
        }
        Diagnostic("UI cheats screen closed");
        return closed_;
    }

private:
    // ---- State and input ----

    bool Ready() const {
        return phase_ == CheatsPhase::Ready && !builds_.empty();
    }

    void Update(Clock::time_point now) {
        phase_ = store_->Phase();
        builds_ = store_->BuildIds();
        build_ = std::min(build_, builds_.empty() ? std::size_t{0} : builds_.size() - 1);
        items_ = Ready() ? store_->Cheats(build_) : std::vector<CheatItem>{};
        selected_ = std::clamp(selected_, 0, std::max(static_cast<int>(items_.size()) - 1, 0));
        HandleInput(now);
        selected_ = std::clamp(selected_, 0, std::max(static_cast<int>(items_.size()) - 1, 0));
        if (selected_ < top_) {
            top_ = selected_;
        } else if (selected_ > top_ + kVisibleRows - 1) {
            top_ = selected_ - (kVisibleRows - 1);
        }
        top_ = std::max(top_, 0);
        if (scroll_.Target() != static_cast<float>(top_)) {
            scroll_.To(static_cast<float>(top_), now, kDurationPanel);
        }
    }

    void HandleInput(Clock::time_point now) {
        if (input_.Pressed(Button::B)) {
            leaving_ = true;
            return;
        }
        if (!Ready()) {
            if (input_.Pressed(Button::A) &&
                (phase_ == CheatsPhase::Offline || phase_ == CheatsPhase::Failed)) {
                store_->Retry();
            }
            return;
        }
        if (input_.Pressed(Button::LeftShoulder) || input_.Pressed(Button::Left)) {
            SetBuild(build_ == 0 ? 0 : build_ - 1);
        }
        if (input_.Pressed(Button::RightShoulder) || input_.Pressed(Button::Right)) {
            SetBuild(std::min(build_ + 1, builds_.size() - 1));
        }
        if (input_.Pressed(Button::Up)) {
            selected_ = std::max(selected_ - 1, 0);
        }
        if (input_.Pressed(Button::Down)) {
            selected_ = std::min(selected_ + 1, std::max(static_cast<int>(items_.size()) - 1, 0));
        }
        if (input_.Pressed(Button::A) && !items_.empty()) {
            const bool now_on = !items_[static_cast<std::size_t>(selected_)].enabled;
            store_->Toggle(build_, static_cast<std::size_t>(selected_));
            items_[static_cast<std::size_t>(selected_)].enabled = now_on;
            Toast(now_on ? Tr(Text::ToastCheatOn) : Tr(Text::ToastCheatOff), now);
        }
        if (input_.Pressed(Button::X) && !items_.empty()) {
            store_->SetAll(build_, true);
            Toast(Tr(Text::HintAllOn), now);
        }
        if (input_.Pressed(Button::Y) && !items_.empty()) {
            store_->SetAll(build_, false);
            Toast(Tr(Text::HintAllOff), now);
        }
    }

    void SetBuild(std::size_t build) {
        if (build == build_) {
            return;
        }
        build_ = build;
        selected_ = 0;
        top_ = 0;
    }

    void Toast(const std::wstring& text, Clock::time_point now) {
        toast_ = text;
        toast_until_ = now + kToastDuration;
    }

    // ---- Drawing ----

    void Draw(Clock::time_point now) {
        DrawBackdrop();
        DrawHeader();
        if (Ready()) {
            DrawChips();
            DrawList(now);
        } else {
            DrawStatus();
        }
        DrawFooter();
        DrawTabs(renderer_, 0, false);
        DrawClock(renderer_);
        DrawToast(now);
        DrawHints(renderer_, CurrentHints());
    }

    void DrawBackdrop() {
        const D2D1_GRADIENT_STOP glow[2] = {{0.0f, WithOpacity(game_.glow, 0.5f)},
                                            {1.0f, WithOpacity(game_.glow, 0.0f)}};
        renderer_.FillGradient(RectF(0.0f, 0.0f, kCanvasWidth, 620.0f), 0.0f, glow, 2,
                               Point2F(0.0f, 0.0f), Point2F(0.0f, 620.0f));
    }

    void DrawHeader() {
        float x = kMargin;
        if (game_.icon != nullptr && game_.icon->brush) {
            renderer_.DrawImage(*game_.icon, RectF(x, kGameRowTop, x + kIconSize,
                                                   kGameRowTop + kIconSize),
                                11.0f, 1.0f);
            x += kIconSize + 14.0f;
        }
        renderer_.DrawString(game_.name, Font::Nav,
                             RectF(x, kGameRowTop, x + 900.0f, kGameRowTop + kIconSize),
                             Theme::kTextSecondary, HAlign::Left, VAlign::Middle);
        renderer_.DrawString(Tr(Text::CheatsTitle), Font::ModsTitle,
                             RectF(kMargin, kTitleTop, kMargin + 900.0f, kTitleTop + kTitleHeight),
                             Theme::kText, HAlign::Left, VAlign::Middle);
        const D2D1_RECT_F box =
            RectF(kMargin, kSourceTop, kRight, kSourceTop + kSourceHeight);
        const wchar_t* line = !Ready()               ? StatusText()
                              : builds_.size() > 1 ? Tr(Text::CheatsBuildHint)
                                                   : Tr(Text::CheatsSourceLine);
        renderer_.DrawString(line, Font::Meta, box,
                             Theme::kTextSecondary, HAlign::Left, VAlign::Middle);
    }

    const wchar_t* StatusText() const {
        switch (phase_) {
        case CheatsPhase::Loading:
            return Tr(Text::CheatsLoading);
        case CheatsPhase::NoCheats:
            return Tr(Text::CheatsNone);
        case CheatsPhase::Offline:
            return Tr(Text::CheatsOffline);
        case CheatsPhase::Failed:
            return Tr(Text::CheatsFailed);
        case CheatsPhase::Ready:
        default:
            return Tr(Text::CheatsNone);
        }
    }

    void DrawStatus() {
        renderer_.DrawString(StatusText(), Font::Body,
                             RectF(kMargin, kListTop + 20.0f, kMargin + 1200.0f, kListTop + 140.0f),
                             Theme::kTextSecondary);
    }

    void DrawBumper(float x, const wchar_t* label) {
        const D2D1_RECT_F rect = RectF(x, kChipTop, x + kBumperWidth, kChipTop + kBumperHeight);
        renderer_.StrokeRounded(Inflate(rect, -0.5f), 8.0f, 1.0f, Theme::Rgb(0xFFFFFF, 0.24f));
        renderer_.DrawString(label, Font::ChipCount, rect, Theme::kTextTertiary, HAlign::Center,
                             VAlign::Middle);
    }

    // One chip per build ID, with how many cheats are on in it. The row scrolls so the selected
    // chip is always visible.
    void DrawChips() {
        std::vector<std::wstring> labels;
        std::vector<std::wstring> counts;
        std::vector<float> widths;
        for (std::size_t i = 0; i < builds_.size(); ++i) {
            labels.push_back(Widen(builds_[i]));
            const int on = store_->EnabledCount(i);
            counts.push_back(on > 0 ? std::to_wstring(on) : std::wstring());
            float width = renderer_.MeasureString(labels.back(), Font::MetaMono).width +
                          2.0f * kChipPadding;
            if (!counts.back().empty()) {
                width += 12.0f + renderer_.MeasureString(counts.back(), Font::ChipCount).width;
            }
            widths.push_back(width);
        }
        const float available = kRight - kMargin - 2.0f * (kBumperWidth + 14.0f);
        std::size_t first = std::min(first_chip_, builds_.size() - 1);
        const auto span = [&](std::size_t from, std::size_t to) {
            float total = 0.0f;
            for (std::size_t i = from; i <= to; ++i) {
                total += widths[i] + (i > from ? kChipGap : 0.0f);
            }
            return total;
        };
        if (build_ < first) {
            first = build_;
        }
        while (first < build_ && span(first, build_) > available) {
            ++first;
        }
        first_chip_ = first;

        DrawBumper(kMargin, L"LB");
        float x = kMargin + kBumperWidth + 14.0f;
        for (std::size_t i = first; i < builds_.size(); ++i) {
            if (span(first, i) > available) {
                break;
            }
            const bool selected = i == build_;
            const D2D1_RECT_F chip = RectF(x, kChipTop, x + widths[i], kChipTop + kChipHeight);
            renderer_.FillRounded(chip, kChipHeight / 2.0f,
                                  selected ? Theme::kPrimaryFill : Theme::kSurface);
            const D2D1_COLOR_F ink = selected ? Theme::kTextOnLight : Theme::kTextSecondary;
            const float label_width = renderer_.MeasureString(labels[i], Font::MetaMono).width;
            renderer_.DrawString(labels[i], Font::MetaMono,
                                 RectF(x + kChipPadding, chip.top,
                                       x + kChipPadding + label_width + 8.0f, chip.bottom),
                                 ink, HAlign::Left, VAlign::Middle);
            if (!counts[i].empty()) {
                const float count_left = x + kChipPadding + label_width + 12.0f;
                renderer_.DrawString(counts[i], Font::ChipCount,
                                     RectF(count_left, chip.top, chip.right, chip.bottom),
                                     WithOpacity(ink, 0.7f), HAlign::Left, VAlign::Middle);
            }
            x += widths[i] + kChipGap;
        }
        DrawBumper(x, L"RB");
    }

    void DrawList(Clock::time_point now) {
        const float scroll = scroll_.Value(now);
        renderer_.PushClip(RectF(kMargin - 20.0f, kListTop - 10.0f, kRight + 20.0f,
                                 kListTop + static_cast<float>(kVisibleRows) * kRowPitch - 1.0f));
        for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
            const float y = kListTop + (static_cast<float>(i) - scroll) * kRowPitch;
            if (y + kRowHeight < kListTop - 12.0f ||
                y > kListTop + static_cast<float>(kVisibleRows) * kRowPitch) {
                continue;
            }
            DrawRow(items_[static_cast<std::size_t>(i)], i, y, i == selected_);
        }
        renderer_.PopClip();
        // Where the focus is in a long list.
        if (!items_.empty()) {
            const std::wstring position =
                std::to_wstring(selected_ + 1) + L" / " + std::to_wstring(items_.size());
            renderer_.DrawString(position, Font::MetaMono,
                                 RectF(kRight - 300.0f, kSourceTop, kRight,
                                       kSourceTop + kSourceHeight),
                                 Theme::kTextTertiary, HAlign::Right, VAlign::Middle);
        }
    }

    void DrawRow(const CheatItem& item, int index, float y, bool focused) {
        const D2D1_RECT_F row = RectF(kMargin, y, kRight, y + kRowHeight);
        if (focused) {
            renderer_.FillRounded(row, kRowRadius, Theme::kSurfaceStrong);
        } else if (index > 0 && index - 1 != selected_) {
            renderer_.FillRounded(RectF(kMargin, y - 3.0f, kRight, y - 2.0f), 0.0f,
                                  Theme::kHairline);
        }
        renderer_.DrawString(Widen(item.name), Font::RowTitle,
                             RectF(kMargin + kRowPadding, y, kRight - kRowPadding - kPillWidth - 24.0f,
                                   y + kRowHeight),
                             Theme::kText, HAlign::Left, VAlign::Middle);
        const D2D1_RECT_F pill = RectF(kRight - kRowPadding - kPillWidth,
                                       y + (kRowHeight - kPillHeight) / 2.0f,
                                       kRight - kRowPadding,
                                       y + (kRowHeight + kPillHeight) / 2.0f);
        if (item.enabled) {
            renderer_.FillRounded(pill, kPillHeight / 2.0f, Theme::kSuccessFill);
            renderer_.DrawString(Tr(Text::CheatOn), Font::Nav, pill, Theme::kSuccessText,
                                 HAlign::Center, VAlign::Middle);
        } else {
            renderer_.FillRounded(pill, kPillHeight / 2.0f, Theme::kGlass);
            renderer_.DrawString(Tr(Text::CheatOff), Font::Nav, pill, Theme::kTextSecondary,
                                 HAlign::Center, VAlign::Middle);
        }
        if (focused) {
            DrawRing(renderer_, row, kRowRadius + kRingGap, 1.0f);
        }
    }

    // The credit for the database, always on screen, and the versions it names for the game.
    void DrawFooter() {
        if (Ready()) {
            std::wstring notes;
            for (const std::string& note : store_->Notes()) {
                notes += (notes.empty() ? L"" : L"  ·  ") + Widen(note);
            }
            if (!notes.empty()) {
                renderer_.DrawString(notes, Font::Meta,
                                     RectF(kMargin, kNotesTop, kRight, kNotesTop + kFooterHeight),
                                     Theme::kTextTertiary, HAlign::Left, VAlign::Middle);
            }
        }
        renderer_.DrawString(Tr(Text::CheatsCredit), Font::Meta,
                             RectF(kMargin, kFooterTop, kRight, kFooterTop + kFooterHeight),
                             Theme::kTextSecondary, HAlign::Left, VAlign::Middle);
    }

    void DrawToast(Clock::time_point now) {
        if (toast_.empty() || now >= toast_until_) {
            return;
        }
        const float remaining = std::chrono::duration<float>(toast_until_ - now).count();
        const float elapsed = std::chrono::duration<float>(kToastDuration).count() - remaining;
        DrawToastCapsule(renderer_, toast_,
                         std::clamp(std::min(elapsed / 0.15f, remaining / 0.22f), 0.0f, 1.0f));
    }

    std::vector<Hint> CurrentHints() const {
        std::vector<Hint> hints;
        if (Ready()) {
            hints.push_back({Theme::kButtonA, L"A", Tr(Text::HintToggle)});
            hints.push_back({Theme::kButtonX, L"X", Tr(Text::HintAllOn)});
            hints.push_back({Theme::kButtonY, L"Y", Tr(Text::HintAllOff)});
        } else if (phase_ == CheatsPhase::Offline || phase_ == CheatsPhase::Failed) {
            hints.push_back({Theme::kButtonA, L"A", Tr(Text::HintSelect)});
        }
        hints.push_back({Theme::kButtonB, L"B", Tr(Text::HintBack)});
        return hints;
    }

    // ---- Members ----

    Renderer& renderer_;
    CoreWindow window_;
    Input& input_;
    ModsGame game_;
    std::unique_ptr<CheatStore> store_;

    CheatsPhase phase_ = CheatsPhase::Loading;
    std::vector<std::string> builds_;
    std::vector<CheatItem> items_;
    std::size_t build_ = 0;
    std::size_t first_chip_ = 0;
    int selected_ = 0;
    int top_ = 0;
    Tween scroll_;

    std::wstring toast_;
    Clock::time_point toast_until_{};

    bool closed_ = false;
    bool leaving_ = false;
};

// Settings > Credits.
class CreditsScreen {
public:
    CreditsScreen(Renderer& renderer, const CoreWindow& window, Input& input)
        : renderer_(renderer), window_(window), input_(input) {}

    bool Run() {
        const auto closed_token =
            window_.Closed([this](const auto&, const auto&) { closed_ = true; });
        SCOPE_EXIT {
            window_.Closed(closed_token);
        };
        Diagnostic("UI credits screen open");
        while (!closed_ && !leaving_) {
            const Clock::time_point frame_start = Clock::now();
            window_.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            input_.Update();
            if (input_.Pressed(Button::B) || input_.Pressed(Button::A)) {
                leaving_ = true;
            }
            renderer_.BeginFrame();
            renderer_.DrawString(Tr(Text::CreditsTitle), Font::ModsTitle,
                                 RectF(kMargin, kTitleTop, kMargin + 900.0f, kTitleTop + kTitleHeight),
                                 Theme::kText, HAlign::Left, VAlign::Middle);
            renderer_.DrawString(Tr(Text::CreditsBody), Font::Body,
                                 RectF(kMargin, kListTop - 40.0f, kMargin + 1300.0f, kFooterTop),
                                 Theme::kTextSecondary);
            DrawTabs(renderer_, 1, false);
            DrawClock(renderer_);
            DrawHints(renderer_, {{Theme::kButtonB, L"B", Tr(Text::HintBack)}});
            renderer_.EndFrame();
            const Clock::duration spent = Clock::now() - frame_start;
            if (spent < std::chrono::milliseconds(8)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16) - spent);
            }
        }
        Diagnostic("UI credits screen closed");
        return closed_;
    }

private:
    Renderer& renderer_;
    CoreWindow window_;
    Input& input_;
    bool closed_ = false;
    bool leaving_ = false;
};

} // namespace

bool RunCheatsScreen(Renderer& renderer, const CoreWindow& window, Input& input,
                     const ModsGame& game, const std::filesystem::path& local_state) {
    try {
        CheatsScreen screen(renderer, window, input, game, local_state);
        return screen.Run();
    } catch (const winrt::hresult_error& error) {
        Diagnostic("UI cheats screen failed " + winrt::to_string(error.message()));
    } catch (const std::exception& error) {
        Diagnostic(std::string("UI cheats screen failed ") + error.what());
    }
    return false;
}

bool RunCreditsScreen(Renderer& renderer, const CoreWindow& window, Input& input) {
    try {
        CreditsScreen screen(renderer, window, input);
        return screen.Run();
    } catch (const winrt::hresult_error& error) {
        Diagnostic("UI credits screen failed " + winrt::to_string(error.message()));
    } catch (const std::exception& error) {
        Diagnostic(std::string("UI credits screen failed ") + error.what());
    }
    return false;
}

} // namespace EdenXbox::Ui
