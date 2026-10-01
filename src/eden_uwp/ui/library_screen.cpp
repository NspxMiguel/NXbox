// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/ui/library_screen.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>

#include "common/scope_exit.h"
#include "eden_uwp/diagnostic.h"
#include "eden_uwp/ui/anim.h"
#include "eden_uwp/ui/input.h"
#include "eden_uwp/ui/library.h"
#include "eden_uwp/ui/renderer.h"
#include "eden_uwp/ui/strings.h"
#include "eden_uwp/ui/theme.h"

namespace EdenXbox::Ui {
namespace {

using D2D1::Point2F;
using D2D1::RectF;
using winrt::Windows::ApplicationModel::Core::CoreApplication;
using winrt::Windows::UI::Core::CoreProcessEventsOption;
using winrt::Windows::UI::Core::CoreWindow;

// Layout, in canvas units (the canvas is 1920x1080), from the approved preview.
constexpr float kMargin = 96.0f; // left and right edge of the content

// The top row: the tab capsule on the left, the clock on the right, both centered on y = 74.
constexpr float kTopCenter = 74.0f;
constexpr float kTabsPadding = 6.0f;
constexpr float kTabsGap = 8.0f;
constexpr float kTabHeight = 48.0f;
constexpr float kTabLabelPadding = 24.0f;

// The hero lives on the top 760 px: the gradient and the scrim are laid out on that box, and the
// rail sits on plain black below it. Text on the left, the game's icon on the right.
constexpr float kHeroHeight = 760.0f;
constexpr float kHeroTextWidth = 1100.0f;
constexpr float kTitleBottom = 540.0f;
constexpr float kTitleHeight = 200.0f;
constexpr float kMetaTop = 560.0f;
constexpr float kMetaHeight = 24.0f;
constexpr float kMetaGap = 22.0f; // between a run of the meta line and the dot after it
constexpr float kMetaDot = 5.0f;
constexpr float kPillTop = 622.0f;
constexpr float kPillHeight = 64.0f;
constexpr float kPillPadding = 34.0f;
constexpr float kPillGap = 18.0f;
constexpr float kPlayGlyph = 22.0f;
constexpr float kPlayGap = 12.0f;
constexpr float kArtSize = 520.0f;
constexpr float kArtTop = kPillTop + kPillHeight - kArtSize; // its bottom lines up with the pills
constexpr float kArtRadius = 32.0f;

// The progress bar of a conversion, on the row of the hero pills.
constexpr float kBarWidth = 560.0f;
constexpr float kBarHeight = 14.0f;

// The rail of tiles, the last one being "add games".
constexpr float kSectionTop = 708.0f;
constexpr float kSectionHeight = 30.0f;
constexpr float kRailTop = 778.0f;
constexpr float kTileSize = 196.0f;
constexpr float kTileRadius = 22.0f;
constexpr float kTileGap = 30.0f;
constexpr float kTilePitch = kTileSize + kTileGap;
constexpr float kLiftScale = 0.08f;
constexpr float kLiftRise = 16.0f;
constexpr float kLiftTilt = -1.2f; // degrees
constexpr float kRingGap = 6.0f;   // from the element to the outer edge of the ring
constexpr float kRingWidth = 3.0f;
constexpr float kNameOffset = 22.0f; // from the bottom of the tile to the name under it
constexpr float kNameWidth = 520.0f;
constexpr float kNameHeight = 28.0f;
constexpr int kSkeletonTiles = 6;

// The hint bar floats 44 px above the bottom edge.
constexpr float kHintHeight = 62.0f;
constexpr float kHintTop = kCanvasHeight - 44.0f - kHintHeight;
constexpr float kGlyphSize = 30.0f;
constexpr float kHintPadding = 30.0f;
constexpr float kHintLabelGap = 10.0f;
constexpr float kHintItemGap = 34.0f;

// The details sheet.
constexpr float kSheetLeft = 420.0f;
constexpr float kSheetTop = 250.0f;
constexpr float kSheetWidth = 1080.0f;
constexpr float kSheetHeight = 500.0f;
constexpr float kSheetPadding = 56.0f;
constexpr float kSheetRowTop = 170.0f; // from the top of the sheet
constexpr float kSheetRowHeight = 72.0f;
constexpr float kSheetLabelWidth = 280.0f;

constexpr auto kToastDuration = std::chrono::milliseconds(2600);
constexpr auto kLaunchFade = std::chrono::milliseconds(180);

// Focus moves vertically through three layers, like the Xbox dashboard.
enum class Layer {
    Rail,    // the game rail: the default focus; A launches the focused game
    Actions, // the hero pills of the focused game
    Nav,     // the tabs at the top
};

enum class Tab { Library, Settings };

constexpr int kTabCount = 2;
constexpr int kActionPlay = 0;
constexpr int kActionMods = 1;
constexpr int kActionDetails = 2;
constexpr int kActionCount = 3;

constexpr std::size_t Idx(int index) {
    return static_cast<std::size_t>(index);
}

// The three colors of the hero gradient, top to bottom.
struct Palette {
    D2D1_COLOR_F top;
    D2D1_COLOR_F middle;
    D2D1_COLOR_F bottom;
};

constexpr Palette kNeutralPalette = {Theme::Rgb(0x26262B), Theme::Rgb(0x151518),
                                     Theme::Rgb(0x0A0A0C)};
constexpr Palette kBlackPalette = {Theme::Rgb(0x000000), Theme::Rgb(0x000000),
                                   Theme::Rgb(0x000000)};

// The brand ribbon, green to red through two warm stops: the focus ring and the progress bar.
constexpr D2D1_GRADIENT_STOP kRibbonStops[4] = {{0.0f, Theme::kRibbon0},
                                                {0.38f, Theme::kRibbon1},
                                                {0.64f, Theme::kRibbon2},
                                                {1.0f, Theme::kRibbon3}};

// What the hero shows: a gradient and the icon of one game (an index into the games, or -1).
struct Look {
    Palette palette = kBlackPalette;
    int art = -1;
};

// A game's icon on the GPU and the colors taken from it, loaded the first time it is needed.
struct Visual {
    bool loaded = false;
    Image image;
    Palette palette = kNeutralPalette;
};

struct Hint {
    D2D1_COLOR_F color;
    const wchar_t* glyph;
    const wchar_t* label;
};

D2D1_COLOR_F Mix(const D2D1_COLOR_F& from, const D2D1_COLOR_F& to, float t) {
    return {from.r + (to.r - from.r) * t, from.g + (to.g - from.g) * t,
            from.b + (to.b - from.b) * t, from.a + (to.a - from.a) * t};
}

Palette Mix(const Palette& from, const Palette& to, float t) {
    return {Mix(from.top, to.top, t), Mix(from.middle, to.middle, t),
            Mix(from.bottom, to.bottom, t)};
}

D2D1_COLOR_F WithOpacity(D2D1_COLOR_F color, float opacity) {
    color.a *= opacity;
    return color;
}

// Scales a color down until its luma is at most `cap`, so the white text on it stays readable.
D2D1_COLOR_F Dim(const D2D1_COLOR_F& color, float cap) {
    const float luma = 0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b;
    const float scale = luma > cap ? cap / luma : 1.0f;
    return {color.r * scale, color.g * scale, color.b * scale, 1.0f};
}

// The hero gradient comes from the icon, not from a palette per category: its three dominant
// colors, most dominant at the top, each darkened to keep the text legible.
Palette ExtractPalette(const Pixels& pixels) {
    struct Bin {
        float weight = 0.0f;
        float red = 0.0f;
        float green = 0.0f;
        float blue = 0.0f;
    };
    // 4 bits per channel. Vivid and bright pixels count for more than murky ones.
    std::vector<Bin> bins(16 * 16 * 16);
    for (std::size_t i = 0; i + 3 < pixels.bgra.size(); i += 4) {
        if (pixels.bgra[i + 3] < 128) {
            continue;
        }
        const float blue = static_cast<float>(pixels.bgra[i]) / 255.0f;
        const float green = static_cast<float>(pixels.bgra[i + 1]) / 255.0f;
        const float red = static_cast<float>(pixels.bgra[i + 2]) / 255.0f;
        const float high = std::max({red, green, blue});
        const float low = std::min({red, green, blue});
        const float saturation = high > 0.0f ? (high - low) / high : 0.0f;
        const float weight = 0.4f + 1.6f * saturation * high;
        const std::size_t cell = (static_cast<std::size_t>(red * 15.0f + 0.5f) << 8) |
                                 (static_cast<std::size_t>(green * 15.0f + 0.5f) << 4) |
                                 static_cast<std::size_t>(blue * 15.0f + 0.5f);
        Bin& bin = bins[cell];
        bin.weight += weight;
        bin.red += red * weight;
        bin.green += green * weight;
        bin.blue += blue * weight;
    }

    // The three heaviest bins that are not close to one already picked.
    std::array<D2D1_COLOR_F, 3> picked{};
    std::size_t found = 0;
    for (; found < picked.size(); ++found) {
        const Bin* best = nullptr;
        for (const Bin& bin : bins) {
            if (bin.weight <= 0.0f || (best != nullptr && bin.weight <= best->weight)) {
                continue;
            }
            bool distinct = true;
            for (std::size_t other = 0; other < found; ++other) {
                const float dr = bin.red / bin.weight - picked[other].r;
                const float dg = bin.green / bin.weight - picked[other].g;
                const float db = bin.blue / bin.weight - picked[other].b;
                if (dr * dr + dg * dg + db * db < 0.06f) {
                    distinct = false;
                    break;
                }
            }
            if (distinct) {
                best = &bin;
            }
        }
        if (best == nullptr) {
            break;
        }
        picked[found] = {best->red / best->weight, best->green / best->weight,
                         best->blue / best->weight, 1.0f};
    }
    if (found == 0) {
        return kNeutralPalette;
    }
    for (std::size_t i = found; i < picked.size(); ++i) {
        picked[i] = picked[i - 1];
    }
    return {Dim(picked[0], 0.44f), Dim(picked[1], 0.30f), Dim(picked[2], 0.16f)};
}

D2D1_RECT_F Inflate(const D2D1_RECT_F& rect, float amount) {
    return RectF(rect.left - amount, rect.top - amount, rect.right + amount, rect.bottom + amount);
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

// A size as a number and its unit ("14,2" "GB"), so the number can be emphasized.
struct SizeText {
    std::wstring amount;
    const wchar_t* unit;
};

SizeText FormatSize(std::uint64_t bytes) {
    constexpr double kMiB = 1024.0 * 1024.0;
    constexpr double kGiB = kMiB * 1024.0;
    const bool gigabytes = static_cast<double>(bytes) >= kGiB;
    wchar_t buffer[32];
    swprintf_s(buffer, std::size(buffer), gigabytes ? L"%.1f" : L"%.0f",
               static_cast<double>(bytes) / (gigabytes ? kGiB : kMiB));
    std::wstring amount = buffer;
    if (CurrentLanguage() == Language::Portuguese) {
        std::replace(amount.begin(), amount.end(), L'.', L',');
    }
    return {amount, gigabytes ? L"GB" : L"MB"};
}

// "NSP" or "XCI".
std::wstring FormatName(const GameEntry& game) {
    std::wstring name = game.path.extension().wstring();
    if (!name.empty() && name.front() == L'.') {
        name.erase(0, 1);
    }
    std::transform(name.begin(), name.end(), name.begin(),
                   [](wchar_t letter) { return static_cast<wchar_t>(std::towupper(letter)); });
    return name;
}

std::wstring Widen(const std::string& ascii) {
    return std::wstring(ascii.begin(), ascii.end());
}

class LibraryScreen {
public:
    LibraryScreen(Renderer& renderer, const CoreWindow& window,
                  const std::filesystem::path& local_state)
        : renderer_(renderer), window_(window), input_(window), local_state_(local_state) {
        StartScan();
    }

    // Draws and handles input until the player launches a game or leaves. Returns the chosen
    // game's path (UTF-8), or an empty string.
    std::string Run() {
        const auto closed_token =
            window_.Closed([this](const auto&, const auto&) { closed_ = true; });
        // The system wants the GPU memory trimmed when the app is suspended.
        const auto suspending_token =
            CoreApplication::Suspending([this](const auto&, const auto&) { renderer_.Trim(); });
        SCOPE_EXIT {
            window_.Closed(closed_token);
            CoreApplication::Suspending(suspending_token);
        };
        Diagnostic("UI library screen open");
        while (!closed_ && !leaving_) {
            const Clock::time_point frame_start = Clock::now();
            window_.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            input_.Update();
            const Clock::time_point now = Clock::now();
            Update(now);
            renderer_.BeginFrame();
            Draw(now);
            renderer_.EndFrame();
            if (launching_ && now - launch_started_ >= kLaunchFade) {
                break;
            }
            // Present1(1, 0) paces the loop on the display. Only an early return, as for an
            // occluded window, needs a sleep so that the loop never spins.
            const Clock::duration spent = Clock::now() - frame_start;
            if (spent < std::chrono::milliseconds(8)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16) - spent);
            }
        }
        return launching_ ? result_ : std::string();
    }

private:
    // ---- State ----

    int GameCount() const {
        return static_cast<int>(games_.size());
    }
    const GameEntry& Game(int index) const {
        return games_[Idx(index)];
    }
    int RailCount() const {
        return scan_ ? 0 : GameCount() + 1; // the games and the "add games" tile
    }
    bool HasGame() const {
        return tab_ == Tab::Library && !scan_ && selected_ >= 0 && selected_ < GameCount();
    }
    bool OnAddTile() const {
        return tab_ == Tab::Library && !scan_ && selected_ == GameCount();
    }
    std::string SelectedTitleId() const {
        return HasGame() ? Game(selected_).title_id : std::string();
    }

    // Lists LocalState\games again, on a worker thread. The focus stays on the same game when it
    // is still there.
    void StartScan() {
        keep_title_id_ = SelectedTitleId();
        games_.clear();
        visuals_.clear();
        lift_.clear();
        name_.clear();
        hero_from_ = Look{};
        hero_to_ = Look{};
        hero_key_ = -2;
        selected_ = 0;
        keys_missing_ = false;
        scan_ = std::make_unique<LibraryScan>(local_state_);
        Diagnostic("UI library scan started");
    }

    void AdoptScan() {
        if (!scan_ || !scan_->Finished()) {
            return;
        }
        games_ = scan_->Take();
        keys_missing_ = scan_->KeysMissing();
        scan_.reset();
        visuals_.assign(games_.size(), Visual{});
        lift_.assign(games_.size() + 1, Tween(0.0f));
        name_.assign(games_.size() + 1, Tween(0.0f));
        selected_ = 0;
        for (int i = 0; i < GameCount(); ++i) {
            if (!keep_title_id_.empty() && Game(i).title_id == keep_title_id_) {
                selected_ = i;
            }
        }
        scroll_ = Tween(ScrollGoal(selected_));
        name_[Idx(selected_)] = Tween(1.0f);
        Diagnostic("UI library ready games=" + std::to_string(games_.size()));
    }

    Visual& EnsureVisual(int index) {
        Visual& visual = visuals_[Idx(index)];
        if (visual.loaded) {
            return visual;
        }
        visual.loaded = true;
        const GameEntry& game = Game(index);
        if (game.icon.empty()) {
            return visual;
        }
        Pixels pixels;
        if (!renderer_.DecodeImage(ReadFileBytes(game.icon), pixels) ||
            !renderer_.UploadImage(pixels, visual.image)) {
            Diagnostic("UI library icon unusable for " + game.title_id);
            return visual;
        }
        visual.palette = ExtractPalette(pixels);
        return visual;
    }

    Look LookFor(int key) {
        if (key < 0) {
            return Look{};
        }
        if (key >= GameCount()) {
            return Look{kNeutralPalette, -1}; // the "add games" tile
        }
        const Visual& visual = EnsureVisual(key);
        return Look{visual.palette, visual.image.bitmap ? key : -1};
    }

    // ---- Input ----

    void Update(Clock::time_point now) {
        AdoptScan();
        if (!launching_) {
            HandleInput(now);
        }
        Refresh(now);
    }

    void HandleInput(Clock::time_point now) {
        if (details_open_) {
            if (input_.Pressed(Button::A) || input_.Pressed(Button::B)) {
                details_open_ = false;
            }
            return;
        }
        if (input_.Pressed(Button::LeftShoulder)) {
            SetTab(Tab::Library);
        }
        if (input_.Pressed(Button::RightShoulder)) {
            SetTab(Tab::Settings);
        }
        if (input_.Pressed(Button::Left)) {
            MoveHorizontal(-1);
        }
        if (input_.Pressed(Button::Right)) {
            MoveHorizontal(1);
        }
        if (input_.Pressed(Button::Up)) {
            MoveVertical(-1);
        }
        if (input_.Pressed(Button::Down)) {
            MoveVertical(1);
        }
        // X and Y are shortcuts for the focused game, from any layer.
        if (input_.Pressed(Button::X) && HasGame()) {
            ShowModsNotice(now);
        }
        if (input_.Pressed(Button::Y) && HasGame()) {
            details_open_ = true;
        }
        if (input_.Pressed(Button::A)) {
            Activate(now);
        }
        if (input_.Pressed(Button::B)) {
            Diagnostic("UI library quit");
            leaving_ = true;
        }
    }

    void SetTab(Tab tab) {
        tab_ = tab;
        if (tab_ == Tab::Settings) {
            layer_ = Layer::Nav;
        }
    }

    void MoveHorizontal(int direction) {
        switch (layer_) {
        case Layer::Rail:
            if (!scan_) {
                selected_ = std::clamp(selected_ + direction, 0, GameCount());
            }
            break;
        case Layer::Actions:
            action_ = std::clamp(action_ + direction, 0, kActionCount - 1);
            break;
        case Layer::Nav:
            // Focus on a tab selects it.
            SetTab(static_cast<Tab>(std::clamp(static_cast<int>(tab_) + direction, 0,
                                               kTabCount - 1)));
            break;
        }
    }

    void MoveVertical(int direction) {
        if (direction < 0) {
            if (layer_ == Layer::Rail) {
                layer_ = HasGame() ? Layer::Actions : Layer::Nav;
            } else if (layer_ == Layer::Actions) {
                layer_ = Layer::Nav;
            }
        } else if (layer_ == Layer::Actions) {
            layer_ = Layer::Rail;
        } else if (layer_ == Layer::Nav && tab_ == Tab::Library) {
            layer_ = HasGame() ? Layer::Actions : Layer::Rail;
        }
    }

    void Activate(Clock::time_point now) {
        switch (layer_) {
        case Layer::Rail:
            if (HasGame()) {
                Launch(now);
            } else if (OnAddTile()) {
                StartScan();
            }
            break;
        case Layer::Actions:
            if (action_ == kActionPlay) {
                Launch(now);
            } else if (action_ == kActionMods) {
                ShowModsNotice(now);
            } else if (action_ == kActionDetails) {
                details_open_ = true;
            }
            break;
        case Layer::Nav:
            break; // focus on a tab already selected it
        }
    }

    void Launch(Clock::time_point now) {
        const GameEntry& game = Game(selected_);
        result_ = winrt::to_string(game.path.wstring());
        Diagnostic("UI library launch " + game.title_id + " " + result_);
        launching_ = true;
        launch_started_ = now;
    }

    // The mod store is the next increment; until then the Mods pill and X only say so.
    void ShowModsNotice(Clock::time_point now) {
        toast_ = Tr(Text::ModsSoon);
        toast_until_ = now + kToastDuration;
    }

    // ---- Animation targets ----

    // Keeps focus on a layer that exists, and points every animation at where it should be.
    void Refresh(Clock::time_point now) {
        if (tab_ == Tab::Settings) {
            layer_ = Layer::Nav;
        } else if (layer_ == Layer::Actions && !HasGame()) {
            layer_ = Layer::Rail;
        }
        selected_ = std::clamp(selected_, 0, std::max(RailCount() - 1, 0));

        for (int i = 0; i < static_cast<int>(lift_.size()); ++i) {
            // Only the focused tile lifts; the selected game keeps its name under its tile.
            const bool selected = tab_ == Tab::Library && i == selected_;
            const float lift_target = selected && layer_ == Layer::Rail ? 1.0f : 0.0f;
            Tween& lift = lift_[Idx(i)];
            if (lift.Target() != lift_target) {
                lift.To(lift_target, now);
            }
            const float name_target = selected ? 1.0f : 0.0f;
            Tween& name = name_[Idx(i)];
            if (name.Target() != name_target) {
                name.To(name_target, now);
            }
        }
        const float goal = ScrollGoal(selected_);
        if (goal != scroll_.Target()) {
            scroll_.To(goal, now, kDurationPanel);
        }
        RefreshHero(now);
    }

    // The focused tile always stands at the left margin, where the approved layout puts it, and
    // the rail scrolls under it. That also keeps its name clear of the hint bar.
    float ScrollGoal(int index) const {
        return static_cast<float>(index) * kTilePitch;
    }

    // The hero fades to the look of whatever is selected whenever that changes.
    void RefreshHero(Clock::time_point now) {
        const int key = tab_ == Tab::Library && !scan_ ? selected_ : -1;
        if (key == hero_key_) {
            return;
        }
        hero_key_ = key;
        // What is on screen right now is where the new fade starts from.
        const float shown = hero_blend_.Value(now);
        Look from;
        from.palette = Mix(hero_from_.palette, hero_to_.palette, shown);
        from.art = shown < 0.5f ? hero_from_.art : hero_to_.art;
        hero_from_ = from;
        hero_to_ = LookFor(key);
        hero_blend_ = Tween(0.0f);
        hero_blend_.To(1.0f, now, kDurationScreen);
    }

    // ---- Drawing ----

    void Draw(Clock::time_point now) {
        DrawBackdrop(now);
        if (tab_ == Tab::Library) {
            DrawHeroArt(now);
            DrawHeroText();
            DrawRail(now);
        } else {
            DrawMessage(Tr(Text::SettingsTitle), Tr(Text::SettingsBody));
        }
        DrawNav();
        DrawClock();
        DrawToast(now);
        if (details_open_) {
            DrawDetails();
        }
        DrawHints(); // above the sheet's veil: the sheet's own hint says how to close it
        if (launching_) {
            const float progress =
                std::chrono::duration<float>(now - launch_started_).count() /
                std::chrono::duration<float>(kLaunchFade).count();
            renderer_.FillRounded(RectF(0.0f, 0.0f, kCanvasWidth, kCanvasHeight), 0.0f,
                                  Theme::Rgb(0x000000, std::clamp(progress, 0.0f, 1.0f)));
        }
    }

    float TextWidth(const std::wstring& text, Font font) const {
        return renderer_.MeasureString(text, font).width;
    }

    // The light line along the top edge of glass, fading out down the sides.
    void DrawGlassEdge(const D2D1_RECT_F& rect, float radius) {
        const D2D1_GRADIENT_STOP light[2] = {{0.0f, Theme::kHighlight},
                                             {0.5f, Theme::Rgb(0xFFFFFF, 0.0f)}};
        renderer_.StrokeGradient(Inflate(rect, -0.5f), radius - 0.5f, 1.0f, light, 2,
                                 Point2F(0.0f, rect.top), Point2F(0.0f, rect.bottom));
    }

    void DrawBackdrop(Clock::time_point now) {
        const Palette palette =
            Mix(hero_from_.palette, hero_to_.palette, hero_blend_.Value(now));
        // Three stops with the middle one pulled forward (0 / 48 / 72%): a two-stop ramp is the
        // default gradient of every tool.
        const D2D1_GRADIENT_STOP ramp[3] = {
            {0.0f, palette.top}, {0.48f, palette.middle}, {0.72f, palette.bottom}};
        // The scrim is its own layer: a veil under the tabs, clear through the middle, closing to
        // the page black at the bottom of the hero.
        const D2D1_GRADIENT_STOP scrim[5] = {{0.0f, Theme::Rgb(0x000000, 0.5f)},
                                             {0.16f, Theme::Rgb(0x000000, 0.0f)},
                                             {0.52f, Theme::Rgb(0x000000, 0.0f)},
                                             {0.76f, Theme::Rgb(0x000000, 0.72f)},
                                             {0.98f, Theme::Rgb(0x000000, 1.0f)}};
        const D2D1_RECT_F hero = RectF(0.0f, 0.0f, kCanvasWidth, kHeroHeight);
        renderer_.FillGradient(hero, 0.0f, ramp, 3, Point2F(0.0f, 0.0f),
                               Point2F(0.0f, kHeroHeight));
        renderer_.FillGradient(hero, 0.0f, scrim, 5, Point2F(0.0f, 0.0f),
                               Point2F(0.0f, kHeroHeight));
    }

    void DrawHeroArt(Clock::time_point now) {
        const float blend = hero_blend_.Value(now);
        const D2D1_RECT_F rect = RectF(kCanvasWidth - kMargin - kArtSize, kArtTop,
                                       kCanvasWidth - kMargin, kArtTop + kArtSize);
        const auto draw = [&](int art, float opacity) {
            if (art >= 0 && opacity > 0.001f) {
                renderer_.DrawImage(EnsureVisual(art).image, rect, kArtRadius, opacity);
            }
        };
        // The old icon stays underneath while the new one fades in over it.
        if (hero_from_.art == hero_to_.art) {
            draw(hero_to_.art, 1.0f);
        } else {
            draw(hero_from_.art, hero_to_.art >= 0 ? 1.0f : 1.0f - blend);
            draw(hero_to_.art, blend);
        }
        if (hero_to_.art >= 0) {
            renderer_.StrokeRounded(Inflate(rect, -0.5f), kArtRadius - 0.5f, 1.0f,
                                    Theme::kHairline);
        }
    }

    void DrawTitle(const std::wstring& title) {
        const Font font = renderer_.MeasureString(title, Font::Title, kHeroTextWidth).lines > 2
                              ? Font::TitleSmall
                              : Font::Title;
        renderer_.DrawString(title, font,
                             RectF(kMargin, kTitleBottom - kTitleHeight, kMargin + kHeroTextWidth,
                                   kTitleBottom),
                             Theme::kText, HAlign::Left, VAlign::Bottom);
    }

    // A title with a paragraph under it, for the states without a game.
    void DrawMessage(const std::wstring& title, const std::wstring& body) {
        DrawTitle(title);
        renderer_.DrawString(body, Font::Body,
                             RectF(kMargin, kMetaTop, kMargin + 900.0f, kSectionTop - 16.0f),
                             Theme::kTextSecondary);
    }

    void DrawHeroText() {
        if (scan_) {
            const LibraryScan::Conversion conversion = scan_->CurrentConversion();
            if (conversion.active) {
                DrawConversion(conversion);
            } else {
                DrawMessage(Tr(Text::ScanningTitle), std::to_wstring(scan_->Done()) + L" / " +
                                                         std::to_wstring(scan_->Total()));
            }
        } else if (HasGame()) {
            DrawGameHero(Game(selected_));
        } else if (GameCount() == 0 && keys_missing_) {
            // The folder has games, but nothing could be opened: say why, not "no games".
            DrawMessage(Tr(Text::MissingKeysTitle), Tr(Text::MissingKeysBody));
        } else {
            DrawMessage(GameCount() == 0 ? Tr(Text::EmptyTitle) : Tr(Text::AddGamesTitle),
                        Tr(Text::AddGamesBody));
        }
    }

    // A package being converted from .nsz to .nsp: its name, what is going on, a progress bar and
    // the percentage. Leaving with B cancels the conversion.
    void DrawConversion(const LibraryScan::Conversion& conversion) {
        DrawTitle(conversion.name);
        float x = DrawMetaRun(kMargin, Tr(Text::ConvertingTitle), Font::MetaMono,
                              Theme::kTextSecondary);
        if (conversion.count > 1) {
            x = DrawMetaDot(x);
            DrawMetaRun(x,
                        std::to_wstring(conversion.index) + L" / " +
                            std::to_wstring(conversion.count),
                        Font::MetaMono, Theme::kTextSecondary);
        }

        const double fraction =
            conversion.total > 0 ? std::min(static_cast<double>(conversion.done) /
                                                static_cast<double>(conversion.total),
                                            1.0)
                                 : 0.0;
        const float top = kPillTop + (kPillHeight - kBarHeight) / 2.0f;
        const D2D1_RECT_F track = RectF(kMargin, top, kMargin + kBarWidth, top + kBarHeight);
        renderer_.FillRounded(track, kBarHeight / 2.0f, Theme::kSurfaceStrong);
        if (fraction > 0.0) {
            // The ribbon runs across the filled part, so it always ends in red.
            const D2D1_RECT_F fill =
                RectF(track.left, track.top,
                      track.left + std::max(static_cast<float>(fraction) * kBarWidth, kBarHeight),
                      track.bottom);
            renderer_.FillGradient(fill, kBarHeight / 2.0f, kRibbonStops, 4,
                                   Point2F(fill.left, top), Point2F(fill.right, top), 0.9f);
        }
        renderer_.DrawString(std::to_wstring(static_cast<int>(fraction * 100.0)) + L"%",
                             Font::MetaStrong,
                             RectF(track.right + 24.0f, top - 12.0f, track.right + 200.0f,
                                   top + kBarHeight + 12.0f),
                             Theme::kText, HAlign::Left, VAlign::Middle);
    }

    // One run of the meta line, in the monospaced face. Returns where the next one starts.
    float DrawMetaRun(float x, const std::wstring& text, Font font, const D2D1_COLOR_F& color) {
        const float width = TextWidth(text, font);
        renderer_.DrawString(text, font,
                             RectF(x, kMetaTop, x + width + 8.0f, kMetaTop + kMetaHeight), color,
                             HAlign::Left, VAlign::Middle);
        return x + width;
    }

    // The dot between two runs, with the gap on both sides. Returns where the next run starts.
    float DrawMetaDot(float x) {
        renderer_.FillDisc(Point2F(x + kMetaGap + kMetaDot / 2.0f, kMetaTop + kMetaHeight / 2.0f),
                           kMetaDot / 2.0f, Theme::kTextTertiary);
        return x + 2.0f * kMetaGap + kMetaDot;
    }

    void DrawGameHero(const GameEntry& game) {
        DrawTitle(game.name);

        // Format, size and title ID in one monospaced line; the number of the size is the one
        // thing set in the brighter, heavier face.
        const SizeText size = FormatSize(game.size);
        float x = DrawMetaRun(kMargin, FormatName(game), Font::MetaMono, Theme::kTextSecondary);
        x = DrawMetaDot(x);
        x = DrawMetaRun(x, size.amount, Font::MetaStrong, Theme::kText);
        x = DrawMetaRun(x, std::wstring(L" ") + size.unit, Font::MetaMono, Theme::kTextSecondary);
        x = DrawMetaDot(x);
        DrawMetaRun(x, Widen(game.title_id), Font::MetaMono, Theme::kTextSecondary);

        DrawActions();
    }

    // The ribbon focus ring: a 3 px band whose outer edge lies 6 px outside the element and keeps
    // the element's corner radius, green to red at 100 degrees.
    void DrawRing(const D2D1_RECT_F& element, float outer_radius, float opacity) {
        const D2D1_RECT_F outer = Inflate(element, kRingGap);
        const D2D1_RECT_F center_line = Inflate(element, kRingGap - kRingWidth / 2.0f);
        // A CSS linear-gradient(100deg, ...) runs along the angle through the middle of the box
        // and just spans the box.
        constexpr float kSin = 0.98480775f;  // sin(100 degrees)
        constexpr float kCos = -0.17364818f; // cos(100 degrees)
        const float half = (std::fabs((outer.right - outer.left) * kSin) +
                            std::fabs((outer.bottom - outer.top) * kCos)) /
                           2.0f;
        const float x = (outer.left + outer.right) / 2.0f;
        const float y = (outer.top + outer.bottom) / 2.0f;
        renderer_.StrokeGradient(center_line, std::max(outer_radius - kRingWidth / 2.0f, 0.0f),
                                 kRingWidth, kRibbonStops, 4,
                                 Point2F(x - kSin * half, y + kCos * half),
                                 Point2F(x + kSin * half, y - kCos * half), opacity);
    }

    // The pills of the hero: Play is the one white pill, with a play glyph; the others are glass.
    void DrawPill(const D2D1_RECT_F& rect, const std::wstring& label, bool primary, bool focused) {
        const float radius = (rect.bottom - rect.top) / 2.0f;
        const float middle = (rect.top + rect.bottom) / 2.0f;
        if (primary) {
            renderer_.FillRounded(rect, radius, Theme::kPrimaryFill);
            const float left = rect.left + kPillPadding;
            // The triangle of the play icon, from a 24 unit drawing shown at 22 px.
            const float unit = kPlayGlyph / 24.0f;
            const float top = middle - kPlayGlyph / 2.0f;
            const D2D1_POINT_2F triangle[3] = {Point2F(left + 8.0f * unit, top + 5.5f * unit),
                                               Point2F(left + 8.0f * unit, top + 18.5f * unit),
                                               Point2F(left + 19.0f * unit, top + 12.0f * unit)};
            renderer_.FillPolygon(triangle, 3, Theme::kTextOnLight);
            // The box runs to the edge of the pill, not to the padding: a box exactly as wide as
            // the text can make DirectWrite trim it with an ellipsis over a rounding error.
            renderer_.DrawString(
                label, Font::Button,
                RectF(left + kPlayGlyph + kPlayGap, rect.top, rect.right, rect.bottom),
                Theme::kTextOnLight, HAlign::Left, VAlign::Middle);
        } else {
            renderer_.FillRounded(rect, radius, Theme::kGlass);
            DrawGlassEdge(rect, radius);
            renderer_.DrawString(label, Font::Button, rect, Theme::kText, HAlign::Center,
                                 VAlign::Middle);
        }
        if (focused) {
            DrawRing(rect, radius + kRingGap, 1.0f);
        }
    }

    // Play, Mods and Details.
    void DrawActions() {
        const std::array<const wchar_t*, kActionCount> labels = {
            Tr(Text::ActionPlay), Tr(Text::ActionMods), Tr(Text::ActionDetails)};
        float x = kMargin;
        for (int i = 0; i < kActionCount; ++i) {
            const bool primary = i == kActionPlay;
            const std::wstring label = labels[Idx(i)];
            const float width = TextWidth(label, Font::Button) + 2.0f * kPillPadding +
                                (primary ? kPlayGlyph + kPlayGap : 0.0f);
            DrawPill(RectF(x, kPillTop, x + width, kPillTop + kPillHeight), label, primary,
                     layer_ == Layer::Actions && action_ == i);
            x += width + kPillGap;
        }
    }

    void DrawRail(Clock::time_point now) {
        renderer_.DrawString(Tr(Text::RailTitle), Font::Section,
                             RectF(kMargin, kSectionTop, kMargin + 600.0f,
                                   kSectionTop + kSectionHeight),
                             Theme::kTextSecondary);
        if (scan_) {
            DrawSkeletonRail(now);
            return;
        }
        const float scroll = scroll_.Value(now);
        // Tiles that scrolled past the left margin are hidden, so nothing sticks out under it.
        renderer_.PushClip(RectF(kMargin - 30.0f, 0.0f, kCanvasWidth, kCanvasHeight));
        // Resting tiles first, then the lifted ones, so a lifted tile is never covered.
        for (int pass = 0; pass < 2; ++pass) {
            for (int i = 0; i < RailCount(); ++i) {
                const float x = kMargin + static_cast<float>(i) * kTilePitch - scroll;
                if (x + kTileSize < 0.0f || x > kCanvasWidth) {
                    continue;
                }
                const float lift = lift_[Idx(i)].Value(now);
                if ((lift >= 0.001f) == (pass == 1)) {
                    DrawTile(i, x, lift, name_[Idx(i)].Value(now));
                }
            }
        }
        renderer_.PopClip();
    }

    // Placeholders for the tiles while the folder is being read, so the screen does not jump.
    void DrawSkeletonRail(Clock::time_point now) {
        const double seconds = std::chrono::duration<double>(now.time_since_epoch()).count();
        const float phase = static_cast<float>(std::fmod(seconds, 1.8) / 1.8);
        const D2D1_COLOR_F fill =
            WithOpacity(Theme::kSurface, 0.775f + 0.225f * std::sin(phase * 6.2831853f));
        for (int i = 0; i < kSkeletonTiles; ++i) {
            const float x = kMargin + static_cast<float>(i) * kTilePitch;
            renderer_.FillRounded(RectF(x, kRailTop, x + kTileSize, kRailTop + kTileSize),
                                  kTileRadius, fill);
        }
    }

    // One tile of the rail. A focused tile lifts, tilts and gets the ring, all at once, and the
    // game's name under it goes along.
    void DrawTile(int index, float x, float lift, float name_opacity) {
        const D2D1_RECT_F rect = RectF(x, kRailTop, x + kTileSize, kRailTop + kTileSize);
        const D2D1_POINT_2F center = Point2F(x + kTileSize / 2.0f, kRailTop + kTileSize / 2.0f);
        const float scale = 1.0f + kLiftScale * lift;
        renderer_.SetLocalTransform(D2D1::Matrix3x2F::Scale(scale, scale, center) *
                                    D2D1::Matrix3x2F::Rotation(kLiftTilt * lift, center) *
                                    D2D1::Matrix3x2F::Translation(0.0f, -kLiftRise * lift));
        if (index == GameCount()) {
            DrawAddTile(rect);
        } else {
            DrawGameTile(index, rect);
            renderer_.DrawString(
                Game(index).name, Font::Caption,
                RectF(x, rect.bottom + kNameOffset, x + kNameWidth,
                      rect.bottom + kNameOffset + kNameHeight),
                WithOpacity(Theme::kText, name_opacity), HAlign::Left, VAlign::Top);
        }
        if (layer_ == Layer::Rail && index == selected_) {
            DrawRing(rect, kTileRadius, lift);
        }
        renderer_.ClearLocalTransform();
    }

    void DrawGameTile(int index, const D2D1_RECT_F& rect) {
        const Visual& visual = EnsureVisual(index);
        if (visual.image.brush) {
            renderer_.DrawImage(visual.image, rect, kTileRadius, 1.0f);
            return;
        }
        // No icon in the package: the first letter of the name on a translucent surface.
        renderer_.FillRounded(rect, kTileRadius, Theme::kSurface);
        renderer_.StrokeRounded(Inflate(rect, -0.5f), kTileRadius - 0.5f, 1.0f, Theme::kHairline);
        renderer_.DrawString(Game(index).name.substr(0, 1), Font::TitleSmall, rect,
                             Theme::kTextTertiary, HAlign::Center, VAlign::Middle);
    }

    // A translucent surface with a plus sign and its two-line label.
    void DrawAddTile(const D2D1_RECT_F& rect) {
        renderer_.FillRounded(rect, kTileRadius, Theme::kSurface);
        renderer_.StrokeRounded(Inflate(rect, -0.5f), kTileRadius - 0.5f, 1.0f, Theme::kHairline);
        // The plus (46 px box, strokes of 3 px) over the label (two lines of 23.4 px), 14 px apart,
        // the pair centered in the tile.
        constexpr float kPlusBox = 46.0f;
        constexpr float kPlusArm = 13.4f; // half of the 26.8 px stroke length
        constexpr float kPlusStroke = 3.0f;
        constexpr float kLabelGap = 14.0f;
        constexpr float kLabelHeight = 46.8f;
        const float cx = (rect.left + rect.right) / 2.0f;
        const float top =
            (rect.top + rect.bottom) / 2.0f - (kPlusBox + kLabelGap + kLabelHeight) / 2.0f;
        const float cy = top + kPlusBox / 2.0f;
        const D2D1_COLOR_F plus = WithOpacity(Theme::kTextSecondary, 0.6f);
        renderer_.FillRounded(RectF(cx - kPlusArm, cy - kPlusStroke / 2.0f, cx + kPlusArm,
                                    cy + kPlusStroke / 2.0f),
                              kPlusStroke / 2.0f, plus);
        renderer_.FillRounded(RectF(cx - kPlusStroke / 2.0f, cy - kPlusArm,
                                    cx + kPlusStroke / 2.0f, cy + kPlusArm),
                              kPlusStroke / 2.0f, plus);
        const float label_top = top + kPlusBox + kLabelGap;
        renderer_.DrawString(Tr(Text::AddGamesTile), Font::TileLabel,
                             RectF(rect.left, label_top, rect.right, label_top + kLabelHeight),
                             Theme::kTextSecondary, HAlign::Center, VAlign::Top);
    }

    // The tabs: a dark glass capsule, the selected tab a white pill in it. Focus on a tab selects
    // it, so the focused pill is always the selected one.
    void DrawNav() {
        const std::array<const wchar_t*, kTabCount> labels = {Tr(Text::NavLibrary),
                                                              Tr(Text::NavSettings)};
        std::array<float, kTabCount> widths{};
        float total = 2.0f * kTabsPadding + kTabsGap * static_cast<float>(kTabCount - 1);
        for (int i = 0; i < kTabCount; ++i) {
            widths[Idx(i)] = TextWidth(labels[Idx(i)], Font::Nav) + 2.0f * kTabLabelPadding;
            total += widths[Idx(i)];
        }
        const float capsule_height = kTabHeight + 2.0f * kTabsPadding;
        const D2D1_RECT_F capsule = RectF(kMargin, kTopCenter - capsule_height / 2.0f,
                                          kMargin + total, kTopCenter + capsule_height / 2.0f);
        renderer_.FillRounded(capsule, capsule_height / 2.0f, Theme::kGlassTabs);
        DrawGlassEdge(capsule, capsule_height / 2.0f);
        float x = capsule.left + kTabsPadding;
        for (int i = 0; i < kTabCount; ++i) {
            const bool active = i == static_cast<int>(tab_);
            const D2D1_RECT_F pill = RectF(x, capsule.top + kTabsPadding, x + widths[Idx(i)],
                                           capsule.top + kTabsPadding + kTabHeight);
            if (active) {
                renderer_.FillRounded(pill, kTabHeight / 2.0f, Theme::kPrimaryFill);
            }
            renderer_.DrawString(labels[Idx(i)], Font::Nav, pill,
                                 active ? Theme::kTextOnLight : Theme::kTextSecondary,
                                 HAlign::Center, VAlign::Middle);
            if (active && layer_ == Layer::Nav) {
                DrawRing(pill, kTabHeight / 2.0f + kRingGap, 1.0f);
            }
            x += widths[Idx(i)] + kTabsGap;
        }
    }

    void DrawClock() {
        SYSTEMTIME local_time{};
        GetLocalTime(&local_time);
        wchar_t text[8];
        swprintf_s(text, std::size(text), L"%02u:%02u", static_cast<unsigned>(local_time.wHour),
                   static_cast<unsigned>(local_time.wMinute));
        renderer_.DrawString(text, Font::Clock,
                             RectF(kCanvasWidth - kMargin - 240.0f, kTopCenter - 20.0f,
                                   kCanvasWidth - kMargin, kTopCenter + 20.0f),
                             Theme::kText, HAlign::Right, VAlign::Middle);
    }

    // A short message in a glass capsule in the top row, fading in and out.
    void DrawToast(Clock::time_point now) {
        if (toast_.empty() || now >= toast_until_) {
            return;
        }
        const float remaining = std::chrono::duration<float>(toast_until_ - now).count();
        const float elapsed = std::chrono::duration<float>(kToastDuration).count() - remaining;
        const float fade = std::clamp(std::min(elapsed / 0.15f, remaining / 0.22f), 0.0f, 1.0f);
        const float height = kTabHeight + 2.0f * kTabsPadding;
        const float width =
            TextWidth(toast_, Font::Nav) + 2.0f * kTabLabelPadding + 2.0f * kTabsPadding;
        const float left = (kCanvasWidth - width) / 2.0f;
        const D2D1_RECT_F rect =
            RectF(left, kTopCenter - height / 2.0f, left + width, kTopCenter + height / 2.0f);
        renderer_.FillRounded(rect, height / 2.0f, WithOpacity(Theme::kGlassBar, fade));
        renderer_.DrawString(toast_, Font::Nav, rect, WithOpacity(Theme::kText, fade),
                             HAlign::Center, VAlign::Middle);
    }

    std::vector<Hint> CurrentHints() const {
        if (details_open_) {
            return {{Theme::kButtonB, L"B", Tr(Text::HintBack)}};
        }
        std::vector<Hint> hints;
        if (HasGame()) {
            if (layer_ == Layer::Rail) {
                hints.push_back({Theme::kButtonA, L"A", Tr(Text::ActionPlay)});
            } else if (layer_ == Layer::Actions) {
                hints.push_back({Theme::kButtonA, L"A", Tr(Text::HintSelect)});
            }
            hints.push_back({Theme::kButtonX, L"X", Tr(Text::ActionMods)});
            hints.push_back({Theme::kButtonY, L"Y", Tr(Text::ActionDetails)});
        } else if (OnAddTile() && layer_ == Layer::Rail) {
            hints.push_back({Theme::kButtonA, L"A", Tr(Text::HintScan)});
        }
        hints.push_back({Theme::kButtonB, L"B", Tr(Text::HintQuit)});
        return hints;
    }

    // The dark glass bar at the bottom: a round colored glyph for each button, with its label.
    void DrawHints() {
        const std::vector<Hint> hints = CurrentHints();
        std::vector<float> widths;
        float total = 2.0f * kHintPadding + static_cast<float>(hints.size() - 1) * kHintItemGap;
        for (const Hint& hint : hints) {
            widths.push_back(TextWidth(hint.label, Font::Hint));
            total += kGlyphSize + kHintLabelGap + widths.back();
        }
        const float left = (kCanvasWidth - total) / 2.0f;
        const D2D1_RECT_F bar = RectF(left, kHintTop, left + total, kHintTop + kHintHeight);
        renderer_.FillRounded(bar, kHintHeight / 2.0f, Theme::kGlassBar);
        DrawGlassEdge(bar, kHintHeight / 2.0f);
        const float glyph_top = kHintTop + (kHintHeight - kGlyphSize) / 2.0f;
        float x = left + kHintPadding;
        for (std::size_t i = 0; i < hints.size(); ++i) {
            renderer_.FillDisc(Point2F(x + kGlyphSize / 2.0f, kHintTop + kHintHeight / 2.0f),
                               kGlyphSize / 2.0f, hints[i].color);
            renderer_.DrawString(hints[i].glyph, Font::Glyph,
                                 RectF(x, glyph_top, x + kGlyphSize, glyph_top + kGlyphSize),
                                 Theme::kTextOnLight, HAlign::Center, VAlign::Middle);
            x += kGlyphSize + kHintLabelGap;
            renderer_.DrawString(hints[i].label, Font::Hint,
                                 RectF(x, kHintTop, x + widths[i] + 8.0f, kHintTop + kHintHeight),
                                 Theme::kTextSecondary, HAlign::Left, VAlign::Middle);
            x += widths[i] + kHintItemGap;
        }
    }

    // What the package says about the focused game, in a sheet over the dimmed screen.
    void DrawDetails() {
        const GameEntry& game = Game(selected_);
        renderer_.FillRounded(RectF(0.0f, 0.0f, kCanvasWidth, kCanvasHeight), 0.0f, Theme::kVeil);
        const D2D1_RECT_F sheet =
            RectF(kSheetLeft, kSheetTop, kSheetLeft + kSheetWidth, kSheetTop + kSheetHeight);
        renderer_.FillRounded(sheet, 32.0f, Theme::kSheet);
        renderer_.StrokeRounded(Inflate(sheet, -0.5f), 31.5f, 1.0f, Theme::kHairline);
        const float left = sheet.left + kSheetPadding;
        const float right = sheet.right - kSheetPadding;
        renderer_.DrawString(
            game.name, Font::Heading,
            RectF(left, sheet.top + 40.0f, right, sheet.top + kSheetRowTop - 16.0f), Theme::kText,
            HAlign::Left, VAlign::Middle);

        const std::filesystem::path relative = game.path.lexically_relative(local_state_);
        const std::wstring file = relative.empty() ? game.path.wstring() : relative.wstring();
        const SizeText size = FormatSize(game.size);
        struct Row {
            const wchar_t* label;
            std::wstring value;
            Font font;
        };
        const std::array<Row, 4> rows = {{
            {Tr(Text::DetailsTitleId), Widen(game.title_id), Font::MetaMono},
            {Tr(Text::DetailsFormat), FormatName(game), Font::MetaMono},
            {Tr(Text::DetailsSize), size.amount + L" " + size.unit, Font::MetaMono},
            {Tr(Text::DetailsFile), file, Font::Meta},
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
    }

    // ---- Members ----

    Renderer& renderer_;
    CoreWindow window_;
    Input input_;
    std::filesystem::path local_state_;

    std::unique_ptr<LibraryScan> scan_; // set while the folder is being read
    std::string keep_title_id_;         // the game to focus again when the scan ends
    bool keys_missing_ = false;         // the last scan found games it could not open for want of keys
    std::vector<GameEntry> games_;
    std::vector<Visual> visuals_;
    std::vector<Tween> lift_; // per rail item: how lifted it is (0..1)
    std::vector<Tween> name_; // per rail item: how visible the name under it is (0..1)
    Tween scroll_;

    Tab tab_ = Tab::Library;
    Layer layer_ = Layer::Rail;
    int selected_ = 0; // rail index; GameCount() is the "add games" tile
    int action_ = kActionPlay;
    bool details_open_ = false;

    Look hero_from_;
    Look hero_to_;
    Tween hero_blend_{1.0f};
    int hero_key_ = -2;

    std::wstring toast_;
    Clock::time_point toast_until_{};

    bool closed_ = false;
    bool leaving_ = false;
    bool launching_ = false;
    Clock::time_point launch_started_{};
    std::string result_;
};

} // namespace

std::string RunLibrary(const winrt::Windows::UI::Core::CoreWindow& window) {
    Diagnostic("UI library begin");
    std::string chosen;
    try {
        const std::filesystem::path local_state(std::wstring_view(
            winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()));
        Renderer renderer;
        renderer.Initialize(window);
        {
            // The screen owns GPU images, so it has to be gone before the renderer is.
            LibraryScreen screen(renderer, window, local_state);
            chosen = screen.Run();
        }
    } catch (const winrt::hresult_error& error) {
        Diagnostic("UI library failed " + winrt::to_string(error.message()));
        chosen.clear();
    } catch (const std::exception& error) {
        Diagnostic(std::string("UI library failed ") + error.what());
        chosen.clear();
    }
    Diagnostic(chosen.empty() ? "UI library end, no game chosen" : "UI library end");
    return chosen;
}

} // namespace EdenXbox::Ui
