// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/ui/mods_screen.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <winrt/Windows.Foundation.h>

#include "common/scope_exit.h"
#include "eden_uwp/diagnostic.h"
#include "eden_uwp/ui/anim.h"
#include "eden_uwp/ui/mods.h"
#include "eden_uwp/ui/strings.h"
#include "eden_uwp/ui/theme.h"
#include "eden_uwp/ui/widgets.h"

namespace EdenXbox::Ui {
namespace {

using D2D1::Point2F;
using D2D1::RectF;
using winrt::Windows::UI::Core::CoreProcessEventsOption;
using winrt::Windows::UI::Core::CoreWindow;

// Layout, in canvas units, from the approved preview (nxbox-mods.png).
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
constexpr float kRowHeight = 104.0f;
constexpr float kRowPitch = 110.0f;
constexpr float kRowRadius = 22.0f;
constexpr float kRowPadding = 22.0f;
constexpr float kThumbWidth = 186.0f;
constexpr float kThumbHeight = 76.0f;
constexpr float kThumbRadius = 12.0f;
constexpr float kInfoLeft = kMargin + kRowPadding + kThumbWidth + 28.0f; // 332
constexpr float kStatRight = 1542.0f;
constexpr float kStatWidth = 170.0f;
constexpr float kActionRight = kRight - kRowPadding; // 1802
constexpr float kActionHeight = 52.0f;
constexpr int kVisibleRows = 5;
constexpr int kLoadAhead = 5;
constexpr int kMaxSparseLoads = 8;

constexpr float kSheetLeft = 420.0f;
constexpr float kSheetTop = 150.0f;
constexpr float kSheetWidth = 1080.0f;
constexpr float kSheetHeight = 740.0f;
constexpr float kSheetPadding = 56.0f;
constexpr float kSheetRowTop = 140.0f;
constexpr float kSheetRowHeight = 64.0f;
constexpr float kSheetLabelWidth = 240.0f;

constexpr auto kToastDuration = std::chrono::milliseconds(2600);

constexpr int kChipCount = 5;

const wchar_t* ChipLabel(int chip) {
    switch (chip) {
    case 0:
        return Tr(Text::ChipTop);
    case 1:
        return Tr(Text::ChipGraphics);
    case 2:
        return Tr(Text::ChipInterface);
    case 3:
        return Tr(Text::ChipGameplay);
    default:
        return Tr(Text::ChipInstalled);
    }
}

ModFilter FilterOf(int chip) {
    switch (chip) {
    case 0:
        return ModFilter::Top;
    case 1:
        return ModFilter::Graphics;
    case 2:
        return ModFilter::Interface;
    case 3:
        return ModFilter::Gameplay;
    default:
        return ModFilter::Installed;
    }
}

class ModsScreen {
public:
    ModsScreen(Renderer& renderer, const CoreWindow& window, Input& input, const ModsGame& game,
               const std::filesystem::path& local_state)
        : renderer_(renderer), window_(window), input_(input), game_(game),
          local_state_(local_state) {
        store_ = std::make_unique<ModStore>(local_state_, game_.title_id,
                                            winrt::to_string(winrt::hstring(game_.name)));
    }

    // Returns true when the window was closed.
    bool Run() {
        const auto closed_token =
            window_.Closed([this](const auto&, const auto&) { closed_ = true; });
        SCOPE_EXIT {
            window_.Closed(closed_token);
        };
        Diagnostic("UI mods screen open " + game_.title_id);
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
        Diagnostic("UI mods screen closed");
        return closed_;
    }

private:
    // ---- State and input ----

    int RowCount() const {
        return static_cast<int>(rows_.size());
    }

    void Update(Clock::time_point now) {
        rows_ = store_->List(FilterOf(chip_));
        selected_ = std::clamp(selected_, 0, std::max(RowCount() - 1, 0));
        if (!details_open_) {
            HandleInput(now);
        } else {
            HandleDetailsInput();
        }
        selected_ = std::clamp(selected_, 0, std::max(RowCount() - 1, 0));
        // Keep the focused row inside the five that fit.
        if (selected_ < top_) {
            top_ = selected_;
        } else if (selected_ > top_ + kVisibleRows - 1) {
            top_ = selected_ - (kVisibleRows - 1);
        }
        top_ = std::max(top_, 0);
        if (scroll_.Target() != static_cast<float>(top_)) {
            scroll_.To(static_cast<float>(top_), now, kDurationPanel);
        }
        LoadAhead();
        LoadThumbnails();
    }

    void HandleInput(Clock::time_point now) {
        if (input_.Pressed(Button::LeftShoulder)) {
            SetChip(chip_ - 1);
        }
        if (input_.Pressed(Button::RightShoulder)) {
            SetChip(chip_ + 1);
        }
        if (input_.Pressed(Button::Up)) {
            selected_ = std::max(selected_ - 1, 0);
        }
        if (input_.Pressed(Button::Down)) {
            selected_ = std::min(selected_ + 1, std::max(RowCount() - 1, 0));
        }
        if (input_.Pressed(Button::A)) {
            Activate(now);
        }
        if (input_.Pressed(Button::X) && RowCount() > 0) {
            Toggle(now);
        }
        if (input_.Pressed(Button::Y) && RowCount() > 0) {
            details_open_ = true;
            store_->RequestDetails(rows_[static_cast<std::size_t>(selected_)]);
        }
        if (input_.Pressed(Button::B)) {
            leaving_ = true;
        }
    }

    void HandleDetailsInput() {
        if (input_.Pressed(Button::A) || input_.Pressed(Button::B) || input_.Pressed(Button::Y)) {
            details_open_ = false;
        }
    }

    void SetChip(int chip) {
        chip = std::clamp(chip, 0, kChipCount - 1);
        if (chip == chip_) {
            return;
        }
        chip_ = chip;
        selected_ = 0;
        top_ = 0;
        sparse_loads_ = 0;
    }

    void Activate(Clock::time_point now) {
        if (store_->Phase() == StorePhase::Offline && RowCount() == 0) {
            store_->Retry();
            return;
        }
        if (RowCount() == 0) {
            return;
        }
        const std::shared_ptr<ModEntry>& entry = rows_[static_cast<std::size_t>(selected_)];
        switch (entry->State()) {
        case ModState::NotInstalled:
        case ModState::Failed:
            Diagnostic("UI mods install " + std::to_string(entry->id) + " " + entry->name);
            store_->Install(entry);
            break;
        case ModState::Installed:
            Toast(Tr(Text::ToastModAlready), now);
            break;
        default:
            break; // busy or unsupported: nothing to do
        }
    }

    void Toggle(Clock::time_point now) {
        const std::shared_ptr<ModEntry>& entry = rows_[static_cast<std::size_t>(selected_)];
        if (entry->State() != ModState::Installed) {
            return;
        }
        const bool enabled = store_->ToggleEnabled(entry);
        Toast(enabled ? Tr(Text::ToastModOn) : Tr(Text::ToastModOff), now);
    }

    void Toast(const std::wstring& text, Clock::time_point now) {
        toast_ = text;
        toast_until_ = now + kToastDuration;
    }

    // Fetches the next page when the focus nears the end of what is loaded, and when a category
    // shows too little to scroll (the categories filter what has been loaded so far).
    void LoadAhead() {
        if (chip_ == kChipCount - 1 || !store_->MoreAvailable() || store_->PageLoading()) {
            return;
        }
        if (selected_ < RowCount() - kLoadAhead) {
            return;
        }
        if (chip_ != 0 && RowCount() < kVisibleRows) {
            if (sparse_loads_ >= kMaxSparseLoads) {
                return;
            }
            ++sparse_loads_;
        }
        store_->LoadMore();
    }

    // Decodes the thumbnails that arrived, a couple per frame so the screen never stalls.
    void LoadThumbnails() {
        int decoded = 0;
        const int first = std::max(top_ - 1, 0);
        const int last = std::min(top_ + kVisibleRows + 1, RowCount() - 1);
        for (int i = first; i <= last; ++i) {
            const std::shared_ptr<ModEntry>& entry = rows_[static_cast<std::size_t>(i)];
            if (images_.count(entry->id) != 0 || failed_.count(entry->id) != 0) {
                continue;
            }
            store_->RequestThumb(entry);
            std::vector<std::uint8_t> bytes;
            if (decoded < 2 && store_->TakeThumb(entry, bytes)) {
                ++decoded;
                Pixels pixels;
                Image image;
                if (renderer_.DecodeImage(bytes, pixels) && renderer_.UploadImage(pixels, image)) {
                    images_[entry->id] = std::move(image);
                } else {
                    failed_.insert(entry->id);
                }
            }
        }
    }

    // ---- Drawing ----

    void Draw(Clock::time_point now) {
        DrawBackdrop();
        DrawHeader();
        DrawChips();
        DrawList(now);
        DrawTabs(renderer_, 0, false);
        DrawClock(renderer_);
        DrawToast(now);
        if (details_open_ && RowCount() > 0) {
            DrawDetails();
        }
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
        renderer_.DrawString(Tr(Text::ModsTitle), Font::ModsTitle,
                             RectF(kMargin, kTitleTop, kMargin + 900.0f, kTitleTop + kTitleHeight),
                             Theme::kText, HAlign::Left, VAlign::Middle);

        const D2D1_RECT_F box = RectF(kMargin, kSourceTop, kMargin + 1500.0f, kSourceTop + kSourceHeight);
        if (store_->Phase() == StorePhase::Ready && store_->TotalCount() >= 0) {
            const std::wstring number = std::to_wstring(store_->TotalCount());
            const float width = renderer_.MeasureString(number, Font::Nav).width;
            renderer_.DrawString(number, Font::Nav, box, Theme::kText, HAlign::Left,
                                 VAlign::Middle);
            renderer_.DrawString(Tr(Text::ModsSourceLine), Font::Meta,
                                 RectF(kMargin + width + 8.0f, box.top, box.right, box.bottom),
                                 Theme::kTextSecondary, HAlign::Left, VAlign::Middle);
        } else {
            renderer_.DrawString(StatusText(), Font::Meta, box, Theme::kTextSecondary,
                                 HAlign::Left, VAlign::Middle);
        }
    }

    // What the store is doing, while it has no list to show.
    const wchar_t* StatusText() const {
        switch (store_->Phase()) {
        case StorePhase::Searching:
            return Tr(Text::ModsSearching);
        case StorePhase::Loading:
            return Tr(Text::ModsLoading);
        case StorePhase::NoGame:
            return Tr(Text::ModsNoGame);
        case StorePhase::Offline:
            return Tr(Text::ModsOffline);
        case StorePhase::Ready:
        default:
            return L"";
        }
    }

    void DrawBumper(float x, const wchar_t* label) {
        const D2D1_RECT_F rect = RectF(x, kChipTop, x + kBumperWidth, kChipTop + kBumperHeight);
        renderer_.StrokeRounded(Inflate(rect, -0.5f), 8.0f, 1.0f, Theme::Rgb(0xFFFFFF, 0.24f));
        renderer_.DrawString(label, Font::ChipCount, rect, Theme::kTextTertiary, HAlign::Center,
                             VAlign::Middle);
    }

    void DrawChips() {
        DrawBumper(kMargin, L"LB");
        float x = kMargin + kBumperWidth + 14.0f;
        for (int i = 0; i < kChipCount; ++i) {
            const std::wstring label = ChipLabel(i);
            const bool selected = i == chip_;
            const bool counted = i == kChipCount - 1;
            const std::wstring count =
                counted ? std::to_wstring(store_->InstalledCount()) : std::wstring();
            float width = renderer_.MeasureString(label, Font::Nav).width + 2.0f * kChipPadding;
            const float count_width =
                counted ? renderer_.MeasureString(count, Font::ChipCount).width : 0.0f;
            if (counted) {
                width += 10.0f + count_width;
            }
            const D2D1_RECT_F chip = RectF(x, kChipTop, x + width, kChipTop + kChipHeight);
            renderer_.FillRounded(chip, kChipHeight / 2.0f,
                                  selected ? Theme::kPrimaryFill : Theme::kSurface);
            const D2D1_COLOR_F ink = selected ? Theme::kTextOnLight : Theme::kTextSecondary;
            const float label_width = renderer_.MeasureString(label, Font::Nav).width;
            renderer_.DrawString(label, Font::Nav,
                                 RectF(x + kChipPadding, chip.top, x + kChipPadding + label_width + 8.0f,
                                       chip.bottom),
                                 ink, HAlign::Left, VAlign::Middle);
            if (counted) {
                const float count_left = x + kChipPadding + label_width + 10.0f;
                renderer_.DrawString(count, Font::ChipCount,
                                     RectF(count_left, chip.top, count_left + count_width + 8.0f,
                                           chip.bottom),
                                     WithOpacity(ink, 0.7f), HAlign::Left, VAlign::Middle);
            }
            x += width + kChipGap;
        }
        DrawBumper(x, L"RB");
    }

    void DrawList(Clock::time_point now) {
        if (RowCount() == 0) {
            DrawEmpty();
            return;
        }
        const float scroll = scroll_.Value(now);
        renderer_.PushClip(RectF(kMargin - 20.0f, kListTop - 10.0f, kRight + 20.0f,
                                 kListTop + 5.0f * kRowPitch - 1.0f));
        for (int i = 0; i < RowCount(); ++i) {
            const float y = kListTop + (static_cast<float>(i) - scroll) * kRowPitch;
            if (y + kRowHeight < kListTop - 12.0f || y > kListTop + 5.0f * kRowPitch) {
                continue;
            }
            DrawRow(rows_[static_cast<std::size_t>(i)], i, y, i == selected_);
        }
        renderer_.PopClip();
    }

    void DrawEmpty() {
        const wchar_t* text = L"";
        if (store_->Phase() != StorePhase::Ready && chip_ != kChipCount - 1) {
            text = StatusText();
        } else if (chip_ == kChipCount - 1) {
            text = Tr(Text::ModsNoneInstalled);
        } else if (store_->PageLoading() || store_->MoreAvailable()) {
            text = Tr(Text::ModsLoading);
        } else {
            text = Tr(Text::ModsNothing);
        }
        renderer_.DrawString(text, Font::Body,
                             RectF(kMargin, kListTop + 20.0f, kMargin + 1200.0f, kListTop + 120.0f),
                             Theme::kTextSecondary);
    }

    // A check mark from two rounded bars, centered on (cx, cy).
    void DrawCheck(float cx, float cy, const D2D1_COLOR_F& color) {
        constexpr float kThick = 2.6f;
        const D2D1_POINT_2F a = Point2F(cx - 7.0f, cy + 0.5f);
        const D2D1_POINT_2F b = Point2F(cx - 2.6f, cy + 5.0f);
        const D2D1_POINT_2F c = Point2F(cx + 7.4f, cy - 5.4f);
        const auto bar = [&](const D2D1_POINT_2F& from, const D2D1_POINT_2F& to) {
            const float dx = to.x - from.x;
            const float dy = to.y - from.y;
            const float length = std::sqrt(dx * dx + dy * dy);
            const float angle = std::atan2(dy, dx) * 180.0f / 3.14159265f;
            const D2D1_POINT_2F mid = Point2F((from.x + to.x) / 2.0f, (from.y + to.y) / 2.0f);
            renderer_.SetLocalTransform(D2D1::Matrix3x2F::Rotation(angle, mid));
            renderer_.FillRounded(RectF(mid.x - length / 2.0f - kThick / 2.0f, mid.y - kThick / 2.0f,
                                        mid.x + length / 2.0f + kThick / 2.0f,
                                        mid.y + kThick / 2.0f),
                                  kThick / 2.0f, color);
            renderer_.ClearLocalTransform();
        };
        bar(a, b);
        bar(b, c);
    }

    // The pill at the right of a row. Its look follows the mod's state.
    void DrawAction(const std::shared_ptr<ModEntry>& entry, float row_top) {
        const float top = row_top + (kRowHeight - kActionHeight) / 2.0f;
        const float radius = kActionHeight / 2.0f;
        const auto width_of = [&](const std::wstring& label, Font font, float padding,
                                  float minimum, float extra) {
            return std::max(renderer_.MeasureString(label, font).width + 2.0f * padding + extra,
                            minimum);
        };
        switch (entry->State()) {
        case ModState::NotInstalled: {
            const std::wstring label = Tr(Text::ModInstall);
            const float width = width_of(label, Font::Nav, 40.0f, 176.0f, 0.0f);
            const D2D1_RECT_F pill = RectF(kActionRight - width, top, kActionRight, top + kActionHeight);
            renderer_.FillRounded(pill, radius, Theme::kPrimaryFill);
            renderer_.DrawString(label, Font::Nav, pill, Theme::kTextOnLight, HAlign::Center,
                                 VAlign::Middle);
            break;
        }
        case ModState::Downloading:
        case ModState::Installing: {
            const bool installing = entry->State() == ModState::Installing;
            const int percent = std::clamp(entry->percent.load(), 0, 100);
            const std::wstring label =
                installing ? std::wstring(Tr(Text::ModInstalling))
                           : std::wstring(Tr(Text::ModDownloading)) + L" " +
                                 std::to_wstring(percent) + L"%";
            const float width = width_of(std::wstring(Tr(Text::ModDownloading)) + L" 100%",
                                         Font::MetaStrong, 24.0f, 196.0f, 0.0f);
            const D2D1_RECT_F pill = RectF(kActionRight - width, top, kActionRight, top + kActionHeight);
            renderer_.FillRounded(pill, radius, Theme::kSurfaceStrong);
            const float fraction = installing ? 1.0f : static_cast<float>(percent) / 100.0f;
            if (fraction > 0.0f) {
                // The ribbon runs across the filled part and the text stays over it.
                renderer_.PushClip(RectF(pill.left, pill.top,
                                         pill.left + std::max(fraction * width, radius),
                                         pill.bottom));
                renderer_.FillGradient(pill, radius, kRibbonStops, 4, Point2F(pill.left, top),
                                       Point2F(pill.right, top), 0.85f);
                renderer_.PopClip();
            }
            renderer_.DrawString(label, Font::MetaStrong, pill, Theme::kText, HAlign::Center,
                                 VAlign::Middle);
            break;
        }
        case ModState::Installed: {
            const bool enabled = store_->IsEnabled(*entry);
            const std::wstring label = enabled ? Tr(Text::ModInstalled) : Tr(Text::ModDisabled);
            const float width = width_of(label, Font::Nav, 28.0f, 176.0f, enabled ? 32.0f : 0.0f);
            const D2D1_RECT_F pill = RectF(kActionRight - width, top, kActionRight, top + kActionHeight);
            if (enabled) {
                renderer_.FillRounded(pill, radius, Theme::kSuccessFill);
                const float text_width = renderer_.MeasureString(label, Font::Nav).width;
                const float start = (pill.left + pill.right) / 2.0f - (text_width + 32.0f) / 2.0f;
                DrawCheck(start + 10.0f, (pill.top + pill.bottom) / 2.0f, Theme::kSuccessText);
                renderer_.DrawString(label, Font::Nav,
                                     RectF(start + 32.0f, pill.top, pill.right, pill.bottom),
                                     Theme::kSuccessText, HAlign::Left, VAlign::Middle);
            } else {
                renderer_.FillRounded(pill, radius, Theme::kGlass);
                renderer_.DrawString(label, Font::Nav, pill, Theme::kTextSecondary, HAlign::Center,
                                     VAlign::Middle);
            }
            break;
        }
        case ModState::Unsupported: {
            const std::wstring label = Tr(Text::ModUnsupported);
            const float width = width_of(label, Font::Nav, 28.0f, 176.0f, 0.0f);
            const D2D1_RECT_F pill = RectF(kActionRight - width, top, kActionRight, top + kActionHeight);
            renderer_.FillRounded(pill, radius, Theme::kGlass);
            renderer_.DrawString(label, Font::Nav, pill, Theme::kTextSecondary, HAlign::Center,
                                 VAlign::Middle);
            break;
        }
        case ModState::Failed:
        default: {
            const std::wstring label = Tr(Text::ModFailed);
            const float width = width_of(label, Font::Nav, 28.0f, 176.0f, 0.0f);
            const D2D1_RECT_F pill = RectF(kActionRight - width, top, kActionRight, top + kActionHeight);
            renderer_.FillRounded(pill, radius, Theme::kDangerFill);
            renderer_.DrawString(label, Font::Nav, pill, Theme::kDangerText, HAlign::Center,
                                 VAlign::Middle);
            break;
        }
        }
    }

    void DrawRow(const std::shared_ptr<ModEntry>& entry, int index, float y, bool focused) {
        const D2D1_RECT_F row = RectF(kMargin, y, kRight, y + kRowHeight);
        if (focused) {
            renderer_.FillRounded(row, kRowRadius, Theme::kSurfaceStrong);
        } else if (index > 0 && index - 1 != selected_) {
            // A hairline between two resting rows, in the gap above this one.
            renderer_.FillRounded(RectF(kMargin, y - 3.0f, kRight, y - 2.0f), 0.0f,
                                  Theme::kHairline);
        }
        const D2D1_RECT_F thumb = RectF(kMargin + kRowPadding, y + (kRowHeight - kThumbHeight) / 2.0f,
                                        kMargin + kRowPadding + kThumbWidth,
                                        y + (kRowHeight + kThumbHeight) / 2.0f);
        const auto image = images_.find(entry->id);
        if (image != images_.end() && image->second.brush) {
            renderer_.DrawImage(image->second, thumb, kThumbRadius, 1.0f);
        } else {
            renderer_.FillRounded(thumb, kThumbRadius, Theme::kSurface);
        }

        const std::wstring name = Widen(entry->name);
        std::wstring sub = Widen(entry->author);
        if (!entry->category.empty()) {
            sub += (sub.empty() ? L"" : L" · ") + Widen(entry->category);
        }
        const float info_right = kStatRight - kStatWidth - 24.0f;
        renderer_.DrawString(name, Font::RowTitle, RectF(kInfoLeft, y + 20.0f, info_right, y + 54.0f),
                             Theme::kText, HAlign::Left, VAlign::Middle);
        renderer_.DrawString(sub, Font::RowSub, RectF(kInfoLeft, y + 58.0f, info_right, y + 84.0f),
                             Theme::kTextSecondary, HAlign::Left, VAlign::Middle);

        const std::int64_t downloads = entry->downloads.load();
        const bool has_downloads = downloads >= 0;
        renderer_.DrawString(FormatCount(has_downloads ? downloads : entry->likes), Font::Stat,
                             RectF(kStatRight - kStatWidth, y + 22.0f, kStatRight, y + 54.0f),
                             Theme::kText, HAlign::Right, VAlign::Middle);
        renderer_.DrawString(has_downloads ? Tr(Text::ModDownloadsCaption)
                                           : Tr(Text::ModLikesCaption),
                             Font::StatCaption,
                             RectF(kStatRight - kStatWidth, y + 58.0f, kStatRight, y + 80.0f),
                             Theme::kTextTertiary, HAlign::Right, VAlign::Middle);
        DrawAction(entry, y);
        if (focused) {
            DrawRing(renderer_, row, kRowRadius + kRingGap, 1.0f);
        }
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
        if (details_open_) {
            return {{Theme::kButtonB, L"B", Tr(Text::HintBack)}};
        }
        if (RowCount() == 0) {
            std::vector<Hint> hints;
            if (store_->Phase() == StorePhase::Offline) {
                hints.push_back({Theme::kButtonA, L"A", Tr(Text::HintSelect)});
            }
            hints.push_back({Theme::kButtonB, L"B", Tr(Text::HintBack)});
            return hints;
        }
        return {{Theme::kButtonA, L"A", Tr(Text::HintInstall)},
                {Theme::kButtonY, L"Y", Tr(Text::HintViewMod)},
                {Theme::kButtonX, L"X", Tr(Text::HintToggle)},
                {Theme::kButtonB, L"B", Tr(Text::HintBack)}};
    }

    // The mod's author, category, count, files and description, in a sheet over the dimmed screen.
    void DrawDetails() {
        const std::shared_ptr<ModEntry>& entry = rows_[static_cast<std::size_t>(selected_)];
        const ModDetails details = store_->Details(entry);
        const D2D1_RECT_F sheet =
            RectF(kSheetLeft, kSheetTop, kSheetLeft + kSheetWidth, kSheetTop + kSheetHeight);
        DrawSheet(renderer_, sheet);
        const float left = sheet.left + kSheetPadding;
        const float right = sheet.right - kSheetPadding;
        renderer_.DrawString(Widen(entry->name), Font::Heading,
                             RectF(left, sheet.top + 36.0f, right, sheet.top + kSheetRowTop - 16.0f),
                             Theme::kText, HAlign::Left, VAlign::Middle);

        const std::int64_t downloads = entry->downloads.load();
        std::wstring files = Tr(Text::ModLoadingDetails);
        if (details.loaded) {
            files.clear();
            if (!details.files.empty()) {
                files = Widen(details.files.front().first) + L" (" +
                        FormatBytes(details.files.front().second) + L")";
                if (details.files.size() > 1) {
                    files += L"  +" + std::to_wstring(details.files.size() - 1);
                }
            }
        } else if (details.failed) {
            files.clear();
        }
        struct Row {
            const wchar_t* label;
            std::wstring value;
            Font font;
        };
        const std::array<Row, 4> rows = {{
            {Tr(Text::ModAuthor), Widen(entry->author), Font::Meta},
            {Tr(Text::ModCategory), Widen(entry->category), Font::Meta},
            {Tr(Text::ModDownloadsLabel),
             downloads >= 0 ? FormatCount(downloads) : FormatCount(entry->likes) + L" " +
                                                          Tr(Text::ModLikesCaption),
             Font::MetaMono},
            {Tr(Text::ModFiles), files, Font::Meta},
        }};
        float y = sheet.top + kSheetRowTop;
        for (const Row& row : rows) {
            renderer_.FillRounded(RectF(left, y, right, y + 1.0f), 0.0f, Theme::kHairline);
            renderer_.DrawString(row.label, Font::Meta,
                                 RectF(left, y, left + kSheetLabelWidth, y + kSheetRowHeight),
                                 Theme::kTextSecondary, HAlign::Left, VAlign::Middle);
            renderer_.DrawString(row.value, row.font,
                                 RectF(left + kSheetLabelWidth, y, right, y + kSheetRowHeight),
                                 Theme::kText, HAlign::Left, VAlign::Middle);
            y += kSheetRowHeight;
        }
        renderer_.FillRounded(RectF(left, y, right, y + 1.0f), 0.0f, Theme::kHairline);
        if (!details.description.empty()) {
            renderer_.PushClip(RectF(left, y + 16.0f, right, sheet.bottom - 24.0f));
            renderer_.DrawString(Widen(details.description), Font::Body,
                                 RectF(left, y + 16.0f, right, sheet.bottom - 24.0f),
                                 Theme::kTextSecondary);
            renderer_.PopClip();
        }
    }

    // ---- Members ----

    Renderer& renderer_;
    CoreWindow window_;
    Input& input_;
    ModsGame game_;
    std::filesystem::path local_state_;
    std::unique_ptr<ModStore> store_;

    std::vector<std::shared_ptr<ModEntry>> rows_;
    std::map<std::int64_t, Image> images_;
    std::set<std::int64_t> failed_;
    int chip_ = 0;
    int selected_ = 0;
    int top_ = 0;
    int sparse_loads_ = 0;
    Tween scroll_;
    bool details_open_ = false;

    std::wstring toast_;
    Clock::time_point toast_until_{};

    bool closed_ = false;
    bool leaving_ = false;
};

} // namespace

bool RunModsScreen(Renderer& renderer, const CoreWindow& window, Input& input,
                   const ModsGame& game, const std::filesystem::path& local_state) {
    try {
        ModsScreen screen(renderer, window, input, game, local_state);
        return screen.Run();
    } catch (const winrt::hresult_error& error) {
        Diagnostic("UI mods screen failed " + winrt::to_string(error.message()));
    } catch (const std::exception& error) {
        Diagnostic(std::string("UI mods screen failed ") + error.what());
    }
    return false;
}

} // namespace EdenXbox::Ui
