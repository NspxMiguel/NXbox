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
#include "eden_uwp/save_sync.h"
#include "eden_uwp/ui/anim.h"
#include "eden_uwp/ui/art.h"
#include "eden_uwp/ui/input.h"
#include "eden_uwp/ui/library.h"
#include "eden_uwp/ui/cheats_screen.h"
#include "eden_uwp/ui/mods_screen.h"
#include "eden_uwp/ui/renderer.h"
#include "eden_uwp/ui/savesync_ui.h"
#include "eden_uwp/ui/sources_screen.h"
#include "eden_uwp/ui/strings.h"
#include "eden_uwp/ui/theme.h"
#include "eden_uwp/ui/updater.h"
#include "eden_uwp/ui/usb_import_screen.h"
#include "eden_uwp/ui/widgets.h"

namespace EdenXbox::Ui {
namespace {

using D2D1::Point2F;
using D2D1::RectF;
using winrt::Windows::ApplicationModel::Core::CoreApplication;
using winrt::Windows::UI::Core::CoreProcessEventsOption;
using winrt::Windows::UI::Core::CoreWindow;

// Layout, in canvas units (the canvas is 1920x1080), from the approved preview.
constexpr float kMargin = 96.0f; // left and right edge of the content

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
constexpr float kNameOffset = 22.0f; // from the bottom of the tile to the name under it
constexpr float kNameWidth = 520.0f;
constexpr float kNameHeight = 28.0f;
constexpr int kSkeletonTiles = 6;

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

// The rows of the settings list.
constexpr int kSettingSaveSync = 0;
constexpr int kSettingSources = 1;
constexpr int kSettingUsb = 2;
constexpr int kSettingCredits = 3;
constexpr int kSettingsRowCount = 4;

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

// What the hero shows: a gradient and the icon of one game (an index into the games, or -1).
struct Look {
    Palette palette = kBlackPalette;
    int art = -1; // the game whose icon is drawn, when its icon decoded
    int key = -1; // the game this look belongs to, -1 for the others
};

// A game's icon on the GPU and the colors taken from it, loaded the first time it is needed.
struct Visual {
    bool loaded = false;
    Image image;
    Palette palette = kNeutralPalette;
    // The eShop banner of the hero. It is decoded when the download (or the cache) has it, and it
    // fades in over 320 ms from then on. `banner_final` is set once there is nothing more to wait
    // for: it is on the GPU, or the title has none.
    Image banner;
    Tween banner_fade{0.0f};
    bool banner_final = false;
};

Palette MixPalette(const Palette& from, const Palette& to, float t) {
    return {Mix(from.top, to.top, t), Mix(from.middle, to.middle, t),
            Mix(from.bottom, to.bottom, t)};
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

class LibraryScreen {
public:
    LibraryScreen(Renderer& renderer, const CoreWindow& window,
                  const std::filesystem::path& local_state)
        : renderer_(renderer), window_(window), input_(window), local_state_(local_state),
          banners_(local_state) {
        sync_account_ = GetSyncAccount();
        sources_count_ = CountSources(local_state_);
        usb_mode_ = LoadUsbMode(local_state_);
        usb_detection_ = std::make_unique<UsbDetection>(local_state_);
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

    // The game that was launched, or null when none was.
    const GameEntry* Chosen() const {
        return launching_ && launched_ >= 0 && launched_ < GameCount() ? &Game(launched_) : nullptr;
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
        banner_order_.clear();
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
            return Look{kNeutralPalette, -1, -1}; // the "add games" tile
        }
        const Visual& visual = EnsureVisual(key);
        return Look{visual.palette, visual.image.bitmap ? key : -1, key};
    }

    // ---- The eShop banner of the hero ----

    bool HasBanner(int index) const {
        return index >= 0 && index < GameCount() && visuals_[Idx(index)].banner.bitmap;
    }

    // How much of the banner shows: 0 until it is on the GPU, then it fades in over 320 ms.
    float BannerOpacity(int index, Clock::time_point now) const {
        return HasBanner(index) ? visuals_[Idx(index)].banner_fade.Value(now) : 0.0f;
    }

    // Picks the banner up once the worker has downloaded it (or found it in the cache). Decoding
    // and uploading happen here, on the render thread, once per game.
    void PollBanner(int index, Clock::time_point now) {
        if (index < 0 || index >= GameCount()) {
            return;
        }
        Visual& visual = visuals_[Idx(index)];
        if (visual.banner_final) {
            return;
        }
        const GameEntry& game = Game(index);
        std::filesystem::path file;
        const BannerState state = banners_.Get(game.title_id, file);
        if (state == BannerState::Pending) {
            return;
        }
        visual.banner_final = true;
        if (state == BannerState::None) {
            return; // offline or no banner: the icon gradient stays
        }
        Pixels pixels;
        if (!renderer_.DecodeImage(ReadFileBytes(file), pixels) ||
            !renderer_.UploadImage(pixels, visual.banner)) {
            Diagnostic("UI banner unusable for " + game.title_id);
            banners_.Discard(game.title_id);
            return;
        }
        visual.banner_fade = Tween(0.0f);
        visual.banner_fade.To(1.0f, now, kDurationPanel);
        banner_order_.push_back(index);
        Diagnostic("UI banner shown " + game.title_id);
        EvictBanners();
    }

    // A banner is 8 MB on the GPU: keep the last few, never the one on screen.
    void EvictBanners() {
        constexpr std::size_t kKeep = 3;
        for (std::size_t i = 0; i < banner_order_.size() && banner_order_.size() > kKeep;) {
            const int index = banner_order_[i];
            if (index == selected_ || index == hero_from_.key || index == hero_to_.key) {
                ++i;
                continue;
            }
            Visual& visual = visuals_[Idx(index)];
            visual.banner = Image{};
            visual.banner_final = false; // asked for again if the game is selected later
            banner_order_.erase(banner_order_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    // ---- Input ----

    void Update(Clock::time_point now) {
        AdoptScan();
        if (!launching_) {
            HandleInput(now);
        }
        // Input gets first refusal: an A press that starts a game must beat detection.
        if (!launching_ && !leaving_ && !closed_ && !scan_ && !details_open_ && !update_open_ &&
            tab_ == Tab::Library && usb_detection_->Ready()) {
            closed_ = usb_detection_->Run(renderer_, window_, input_);
            usb_mode_ = LoadUsbMode(local_state_);
            if (!closed_)
                StartScan();
        }
        Refresh(now);
        PollBanner(hero_key_, now);
    }

    void HandleInput(Clock::time_point now) {
        if (update_open_) {
            const auto state = updater_.State();
            if (state == UpdateState::Failed || state == UpdateState::MissingPortal) {
                if (input_.Pressed(Button::B))
                    update_open_ = false;
                else if (input_.Pressed(Button::A))
                    updater_.StartInstall();
            }
            // Never start a game or another download while replacing the running package.
            return;
        }
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
        if (input_.Pressed(Button::X) && HasGame() && !update_focused_) {
            OpenMods();
        }
        if (input_.Pressed(Button::Y) && HasGame() && !update_focused_) {
            details_open_ = true;
        }
        if (input_.Pressed(Button::A)) {
            Activate(now);
            if (update_open_) {
                return;
            }
        }
        if (input_.Pressed(Button::B)) {
            Diagnostic("UI library quit");
            leaving_ = true;
        }
    }

    void SetTab(Tab tab) {
        update_focused_ = false;
        tab_ = tab;
        if (tab_ == Tab::Settings) {
            layer_ = Layer::Nav;
        }
    }

    void MoveHorizontal(int direction) {
        switch (layer_) {
        case Layer::Rail:
            if (tab_ == Tab::Settings) {
                break; // the settings are a list: left and right do nothing
            }
            if (!scan_) {
                selected_ = std::clamp(selected_ + direction, 0, GameCount());
            }
            break;
        case Layer::Actions:
            action_ = std::clamp(action_ + direction, 0, kActionCount - 1);
            break;
        case Layer::Nav:
            if (update_focused_) {
                if (direction < 0)
                    update_focused_ = false;
                break;
            }
            if (direction > 0 && tab_ == Tab::Settings && HasUpdate()) {
                update_focused_ = true;
                break;
            }
            // Focus on a tab selects it.
            SetTab(static_cast<Tab>(std::clamp(static_cast<int>(tab_) + direction, 0,
                                               kTabCount - 1)));
            break;
        }
    }

    void MoveVertical(int direction) {
        update_focused_ = false;
        if (tab_ == Tab::Settings && layer_ == Layer::Rail) {
            // The settings list: up and down walk the rows, and up from the first leaves it.
            if (direction < 0) {
                if (settings_row_ > 0) {
                    --settings_row_;
                } else {
                    layer_ = Layer::Nav;
                }
            } else {
                settings_row_ = std::min(settings_row_ + 1, kSettingsRowCount - 1);
            }
            return;
        }
        if (direction < 0) {
            if (layer_ == Layer::Rail) {
                layer_ = HasGame() ? Layer::Actions : Layer::Nav;
            } else if (layer_ == Layer::Actions) {
                layer_ = Layer::Nav;
            }
        } else if (layer_ == Layer::Actions) {
            layer_ = Layer::Rail;
        } else if (layer_ == Layer::Nav) {
            if (tab_ == Tab::Settings) {
                layer_ = Layer::Rail;
            } else {
                layer_ = HasGame() ? Layer::Actions : Layer::Rail;
            }
        }
    }

    void Activate(Clock::time_point now) {
        if (layer_ == Layer::Nav && update_focused_ && HasUpdate()) {
            update_open_ = true;
            updater_.StartInstall();
            return;
        }
        switch (layer_) {
        case Layer::Rail:
            if (tab_ == Tab::Settings) {
                ActivateSetting(now);
            } else if (HasGame()) {
                Launch(now);
            } else if (OnAddTile()) {
                OpenUsbImport();
            }
            break;
        case Layer::Actions:
            if (action_ == kActionPlay) {
                Launch(now);
            } else if (action_ == kActionMods) {
                OpenMods();
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
        launched_ = selected_;
        launching_ = true;
        launch_started_ = now;
    }

    // The mod store of the focused game, on this window and this renderer. It runs its own loop
    // and returns when the player leaves with B.
    void OpenMods() {
        if (!HasGame()) {
            return;
        }
        const Visual& visual = EnsureVisual(selected_);
        ModsGame mods;
        mods.title_id = Game(selected_).title_id;
        mods.name = Game(selected_).name;
        mods.icon = visual.image.bitmap ? &visual.image : nullptr;
        mods.glow = visual.palette.top;
        Diagnostic("UI library open mods " + mods.title_id);
        if (RunModsScreen(renderer_, window_, input_, mods, local_state_)) {
            closed_ = true;
        }
    }

    // The USB import screen, on this window and this renderer. It runs its own loop and returns
    // when the player leaves with B; whatever it copied or moved is picked up by a new scan.
    void OpenUsbImport() {
        Diagnostic("UI library open usb import");
        if (RunUsbImportScreen(renderer_, window_, input_, local_state_)) {
            closed_ = true;
            return;
        }
        StartScan();
    }

    // The sources screen, on this window and this renderer. It runs its own loop and returns when
    // the player leaves with B; a download it finished is picked up by a new scan.
    void OpenSources() {
        Diagnostic("UI library open sources");
        if (RunSourcesScreen(renderer_, window_, input_, local_state_)) {
            closed_ = true;
            return;
        }
        sources_count_ = CountSources(local_state_);
        StartScan();
    }

    // Settings > Credits: who made what NXbox builds on (cheats_screen.cpp).
    void OpenCredits() {
        Diagnostic("UI library open credits");
        if (RunCreditsScreen(renderer_, window_, input_)) {
            closed_ = true;
        }
    }

    // A on a settings row.
    void ActivateSetting(Clock::time_point now) {
        if (settings_row_ == kSettingCredits) {
            OpenCredits();
            return;
        }
        if (settings_row_ == kSettingUsb) {
            const auto next = usb_mode_ == UsbMode::Ask        ? UsbMode::Copy
                              : usb_mode_ == UsbMode::Copy     ? UsbMode::External
                              : usb_mode_ == UsbMode::External ? UsbMode::Off
                                                               : UsbMode::Ask;
            if (SaveUsbMode(local_state_, next)) {
                usb_mode_ = next;
            } else {
                Toast(Tr(Text::UsbSaveFailed), now);
            }
            return;
        }
        if (settings_row_ == kSettingSources) {
            OpenSources();
            return;
        }
        switch (sync_account_) {
        case SyncAccount::SignedIn:
            SaveSync::SignOut();
            Diagnostic("UI settings savesync signed out");
            sync_account_ = GetSyncAccount();
            Toast(Tr(Text::SyncStateOff), now);
            break;
        case SyncAccount::SignedOut: {
            bool window_closed = false;
            const SignInOutcome outcome = RunSignIn(renderer_, window_, input_, window_closed);
            Diagnostic("UI settings savesync sign-in outcome " +
                       std::to_string(static_cast<int>(outcome)));
            if (window_closed) {
                closed_ = true;
            }
            sync_account_ = GetSyncAccount();
            break;
        }
        case SyncAccount::NotConfigured:
            Toast(Tr(Text::SyncRowMissingHint), now);
            break;
        }
    }

    void Toast(const std::wstring& text, Clock::time_point now) {
        toast_ = text;
        toast_until_ = now + kToastDuration;
    }

    // ---- Animation targets ----

    // Keeps focus on a layer that exists, and points every animation at where it should be.
    void Refresh(Clock::time_point now) {
        if (tab_ == Tab::Settings) {
            if (layer_ == Layer::Actions) {
                layer_ = Layer::Rail; // the settings have no hero pills
            }
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
        from.palette = MixPalette(hero_from_.palette, hero_to_.palette, shown);
        from.art = shown < 0.5f ? hero_from_.art : hero_to_.art;
        from.key = shown < 0.5f ? hero_from_.key : hero_to_.key;
        hero_from_ = from;
        hero_to_ = LookFor(key);
        if (key >= 0 && key < GameCount()) {
            banners_.Request(Game(key).title_id);
        }
        hero_blend_ = Tween(0.0f);
        hero_blend_.To(1.0f, now, kDurationScreen);
    }

    // ---- Drawing ----

    void Draw(Clock::time_point now) {
        DrawBackdrop(now);
        if (tab_ == Tab::Library) {
            DrawHeroBanner(now);
        }
        DrawScrim();
        if (tab_ == Tab::Library) {
            DrawHeroArt(now);
            DrawHeroText(now);
            DrawRail(now);
        } else {
            DrawSettings();
        }
        DrawTabs(renderer_, static_cast<int>(tab_), layer_ == Layer::Nav && !update_focused_);
        DrawClock(renderer_);
        if (HasUpdate()) {
            DrawUpdatePill();
            // Keep feedback from Settings visible without covering the update pill.
            renderer_.SetLocalTransform(D2D1::Matrix3x2F::Translation(0.0f, 100.0f));
            DrawToast(now);
            renderer_.ClearLocalTransform();
        } else {
            DrawToast(now);
        }
        if (details_open_) {
            DrawDetails();
        }
        if (update_open_)
            DrawUpdateSheet();
        DrawHints(renderer_, CurrentHints()); // above the sheet's veil: the sheet's own hint says how to close it
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

    // The gradient that stands in for the banner while there is none (offline, no banner for the
    // title, or still on its way).
    void DrawBackdrop(Clock::time_point now) {
        const Palette palette =
            MixPalette(hero_from_.palette, hero_to_.palette, hero_blend_.Value(now));
        // Three stops with the middle one pulled forward (0 / 48 / 72%): a two-stop ramp is the
        // default gradient of every tool.
        const D2D1_GRADIENT_STOP ramp[3] = {
            {0.0f, palette.top}, {0.48f, palette.middle}, {0.72f, palette.bottom}};
        renderer_.FillGradient(RectF(0.0f, 0.0f, kCanvasWidth, kHeroHeight), 0.0f, ramp, 3,
                               Point2F(0.0f, 0.0f), Point2F(0.0f, kHeroHeight));
    }

    // The scrim is its own layer over the gradient and the banner: a veil under the tabs, clear
    // through the middle, closing to the page black at the bottom of the hero (the approved
    // preview: 180deg, 0.5 at 0%, 0 at 16% and 52%, 0.72 at 76%, black at 98%).
    void DrawScrim() {
        const D2D1_GRADIENT_STOP scrim[5] = {{0.0f, Theme::Rgb(0x000000, 0.5f)},
                                             {0.16f, Theme::Rgb(0x000000, 0.0f)},
                                             {0.52f, Theme::Rgb(0x000000, 0.0f)},
                                             {0.76f, Theme::Rgb(0x000000, 0.72f)},
                                             {0.98f, Theme::Rgb(0x000000, 1.0f)}};
        renderer_.FillGradient(RectF(0.0f, 0.0f, kCanvasWidth, kHeroHeight), 0.0f, scrim, 5,
                               Point2F(0.0f, 0.0f), Point2F(0.0f, kHeroHeight));
    }

    // One banner covering the hero box, like CSS object-fit: cover with object-position 50% 18%.
    void DrawBannerImage(const Image& banner, float opacity) {
        const float scale = std::max(kCanvasWidth / banner.size.width,
                                     kHeroHeight / banner.size.height);
        const float width = banner.size.width * scale;
        const float height = banner.size.height * scale;
        const float left = (kCanvasWidth - width) * 0.5f;
        const float top = (kHeroHeight - height) * 0.18f;
        renderer_.PushClip(RectF(0.0f, 0.0f, kCanvasWidth, kHeroHeight));
        renderer_.DrawImage(banner, RectF(left, top, left + width, top + height), 0.0f, opacity);
        renderer_.PopClip();
    }

    // The full-bleed eShop banner. It fades in when it arrives, and crossfades with the previous
    // game's banner when the selection changes.
    void DrawHeroBanner(Clock::time_point now) {
        const float blend = hero_blend_.Value(now);
        const auto draw = [&](int key, float opacity) {
            const float shown = opacity * BannerOpacity(key, now);
            if (shown > 0.001f) {
                DrawBannerImage(visuals_[Idx(key)].banner, shown);
            }
        };
        if (hero_from_.key == hero_to_.key) {
            draw(hero_to_.key, 1.0f);
        } else {
            // The old banner stays underneath while the new one fades in over it.
            draw(hero_from_.key, HasBanner(hero_to_.key) ? 1.0f : 1.0f - blend);
            draw(hero_to_.key, blend);
        }
    }

    // How much of the selected game's banner is showing: the big title and the icon on the right
    // give way to it, because the banner carries the game's logo.
    float BannerCover(Clock::time_point now) const {
        return HasGame() ? BannerOpacity(selected_, now) : 0.0f;
    }

    void DrawHeroArt(Clock::time_point now) {
        const float blend = hero_blend_.Value(now);
        const float uncovered = 1.0f - BannerCover(now);
        if (uncovered <= 0.001f) {
            return;
        }
        const D2D1_RECT_F rect = RectF(kCanvasWidth - kMargin - kArtSize, kArtTop,
                                       kCanvasWidth - kMargin, kArtTop + kArtSize);
        const auto draw = [&](int art, float opacity) {
            if (art >= 0 && opacity > 0.001f) {
                renderer_.DrawImage(EnsureVisual(art).image, rect, kArtRadius, opacity * uncovered);
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
                                    WithOpacity(Theme::kHairline, uncovered));
        }
    }

    void DrawTitle(const std::wstring& title, float opacity = 1.0f) {
        if (opacity <= 0.001f) {
            return;
        }
        const Font font = renderer_.MeasureString(title, Font::Title, kHeroTextWidth).lines > 2
                              ? Font::TitleSmall
                              : Font::Title;
        renderer_.DrawString(title, font,
                             RectF(kMargin, kTitleBottom - kTitleHeight, kMargin + kHeroTextWidth,
                                   kTitleBottom),
                             WithOpacity(Theme::kText, opacity), HAlign::Left, VAlign::Bottom);
    }

    // A title with a paragraph under it, for the states without a game.
    void DrawMessage(const std::wstring& title, const std::wstring& body) {
        DrawTitle(title);
        renderer_.DrawString(body, Font::Body,
                             RectF(kMargin, kMetaTop, kMargin + 900.0f, kSectionTop - 16.0f),
                             Theme::kTextSecondary);
    }

    void DrawHeroText(Clock::time_point now) {
        if (scan_) {
            const LibraryScan::Conversion conversion = scan_->CurrentConversion();
            if (conversion.active) {
                DrawConversion(conversion);
            } else {
                DrawMessage(Tr(Text::ScanningTitle), std::to_wstring(scan_->Done()) + L" / " +
                                                         std::to_wstring(scan_->Total()));
            }
        } else if (HasGame()) {
            DrawGameHero(Game(selected_), now);
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

    void DrawGameHero(const GameEntry& game, Clock::time_point now) {
        // With a banner there is no big title: the banner carries the logo.
        DrawTitle(game.name, 1.0f - BannerCover(now));

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

    // Play, Mods and Details.
    void DrawActions() {
        const std::array<const wchar_t*, kActionCount> labels = {
            Tr(Text::ActionPlay), Tr(Text::ActionMods), Tr(Text::ActionDetails)};
        float x = kMargin;
        for (int i = 0; i < kActionCount; ++i) {
            const bool primary = i == kActionPlay;
            const std::wstring label = labels[Idx(i)];
            const float width = PillWidth(renderer_, label, primary);
            DrawPill(renderer_, RectF(x, kPillTop, x + width, kPillTop + kPillHeight), label,
                     primary, layer_ == Layer::Actions && action_ == i);
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
            DrawRing(renderer_, rect, kTileRadius, lift);
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

    // A short message in a glass capsule in the top row, fading in and out.
    void DrawToast(Clock::time_point now) {
        if (toast_.empty() || now >= toast_until_) {
            return;
        }
        const float remaining = std::chrono::duration<float>(toast_until_ - now).count();
        const float elapsed = std::chrono::duration<float>(kToastDuration).count() - remaining;
        DrawToastCapsule(renderer_, toast_,
                         std::clamp(std::min(elapsed / 0.15f, remaining / 0.22f), 0.0f, 1.0f));
    }

    bool HasUpdate() const {
        const auto state = updater_.State();
        return state != UpdateState::Checking && state != UpdateState::None;
    }

    void DrawUpdatePill() {
        const std::wstring label = Tr(Text::UpdateAvailable);
        const float width = PillWidth(renderer_, label, false);
        const float right = kCanvasWidth - kMargin - 190.0f;
        DrawChoicePill(renderer_,
                       RectF(right - width, kTopCenter - kPillHeight / 2.0f, right,
                             kTopCenter + kPillHeight / 2.0f),
                       label, false, layer_ == Layer::Nav && update_focused_);
    }

    void DrawUpdateSheet() {
        const auto state = updater_.State();
        Text message = Text::UpdateDownloading;
        if (state == UpdateState::Installing)
            message = Text::UpdateInstalling;
        else if (state == UpdateState::Restarting)
            message = Text::UpdateRestarting;
        else if (state == UpdateState::Failed)
            message = Text::UpdateFailed;
        else if (state == UpdateState::MissingPortal)
            message = Text::UpdateMissingPortal;
        DrawSheet(renderer_,
                  RectF(kSheetLeft, kSheetTop, kSheetLeft + kSheetWidth, kSheetTop + kSheetHeight));
        renderer_.DrawString(Tr(Text::UpdateAvailable), Font::Heading,
                             RectF(476.0f, 290.0f, 1444.0f, 380.0f), Theme::kText);
        renderer_.DrawString(Tr(message), Font::Body, RectF(476.0f, 400.0f, 1444.0f, 610.0f),
                             Theme::kTextSecondary);
        if (state == UpdateState::Downloading) {
            DrawProgressBar(renderer_, RectF(476.0f, 650.0f, 1250.0f, 664.0f), updater_.Progress());
            renderer_.DrawString(
                std::to_wstring(static_cast<int>(updater_.Progress() * 100)) + L"%", Font::MetaMono,
                RectF(1280.0f, 630.0f, 1444.0f, 686.0f), Theme::kText);
        }
    }

    std::vector<Hint> CurrentHints() const {
        if (update_open_) {
            const auto state = updater_.State();
            if (state == UpdateState::Failed || state == UpdateState::MissingPortal) {
                return {{Theme::kButtonA, L"A", Tr(Text::UpdateAction)},
                        {Theme::kButtonB, L"B", Tr(Text::HintBack)}};
            }
            return {};
        }
        if (layer_ == Layer::Nav && update_focused_) {
            return {{Theme::kButtonA, L"A", Tr(Text::UpdateAction)},
                    {Theme::kButtonB, L"B", Tr(Text::HintQuit)}};
        }
        if (details_open_) {
            return {{Theme::kButtonB, L"B", Tr(Text::HintBack)}};
        }
        std::vector<Hint> hints;
        if (tab_ == Tab::Settings) {
            if (layer_ == Layer::Rail && settings_row_ == kSettingSaveSync) {
                if (sync_account_ == SyncAccount::SignedIn) {
                    hints.push_back({Theme::kButtonA, L"A", Tr(Text::HintSignOut)});
                } else if (sync_account_ == SyncAccount::SignedOut) {
                    hints.push_back({Theme::kButtonA, L"A", Tr(Text::HintSignIn)});
                }
            }
            if (layer_ == Layer::Rail &&
                (settings_row_ == kSettingSources || settings_row_ == kSettingUsb ||
                 settings_row_ == kSettingCredits)) {
                hints.push_back({Theme::kButtonA, L"A", Tr(Text::HintSelect)});
            }
        } else if (HasGame()) {
            if (layer_ == Layer::Rail) {
                hints.push_back({Theme::kButtonA, L"A", Tr(Text::ActionPlay)});
            } else if (layer_ == Layer::Actions) {
                hints.push_back({Theme::kButtonA, L"A", Tr(Text::HintSelect)});
            }
            hints.push_back({Theme::kButtonX, L"X", Tr(Text::ActionMods)});
            hints.push_back({Theme::kButtonY, L"Y", Tr(Text::ActionDetails)});
        } else if (OnAddTile() && layer_ == Layer::Rail) {
            hints.push_back({Theme::kButtonA, L"A", Tr(Text::HintImport)});
        }
        hints.push_back({Theme::kButtonB, L"B", Tr(Text::HintQuit)});
        return hints;
    }

    // The settings tab: a list of rows, each with a name, its state and one line of explanation.
    void DrawSettings() {
        DrawTitle(Tr(Text::SettingsTitle));
        struct Row {
            const wchar_t* name;
            const wchar_t* state;
            D2D1_COLOR_F state_color;
            const wchar_t* hint;
        };
        Row sync_row = {Tr(Text::SyncRowTitle), Tr(Text::SyncStateOff), Theme::kTextSecondary,
                        Tr(Text::SyncRowOffHint)};
        switch (sync_account_) {
        case SyncAccount::SignedIn:
            sync_row.state = Tr(Text::SyncStateOn);
            sync_row.state_color = Theme::kSuccessText;
            sync_row.hint = Tr(Text::SyncRowOnHint);
            break;
        case SyncAccount::NotConfigured:
            sync_row.state = Tr(Text::SyncStateNotConfigured);
            sync_row.state_color = Theme::kTextTertiary;
            sync_row.hint = Tr(Text::SyncRowMissingHint);
            break;
        case SyncAccount::SignedOut:
            break;
        }
        Row sources_row = {Tr(Text::SourcesRowTitle), Tr(Text::SourcesRowState),
                           Theme::kTextTertiary, Tr(Text::SourcesRowHint)};
        std::wstring sources_state;
        if (sources_count_ > 0) {
            sources_state = std::to_wstring(sources_count_) + L" " +
                            Tr(sources_count_ == 1 ? Text::SourcesUnitOne : Text::SourcesUnitMany);
            sources_row.state = sources_state.c_str();
            sources_row.state_color = Theme::kSuccessText;
        }
        const Row usb_row = {Tr(Text::UsbSetting), UsbModeLabel(usb_mode_), Theme::kTextSecondary,
                             Tr(Text::UsbSettingHint)};
        const Row credits_row = {Tr(Text::CreditsRowTitle), Tr(Text::CreditsRowState),
                                 Theme::kTextSecondary, Tr(Text::CreditsRowHint)};
        const std::array<Row, kSettingsRowCount> rows = {
            {sync_row, sources_row, usb_row, credits_row}};
        constexpr float kRowTop = 560.0f;
        constexpr float kRowHeight = 88.0f;
        constexpr float kRowPitch = 96.0f;
        constexpr float kRowRadius = 22.0f;
        for (int i = 0; i < kSettingsRowCount; ++i) {
            const float y = kRowTop + static_cast<float>(i) * kRowPitch;
            const D2D1_RECT_F row = RectF(kMargin, y, kCanvasWidth - kMargin, y + kRowHeight);
            const bool focused = layer_ == Layer::Rail && settings_row_ == i;
            if (focused) {
                renderer_.FillRounded(row, kRowRadius, Theme::kSurfaceStrong);
            }
            renderer_.DrawString(rows[Idx(i)].name, Font::RowTitle,
                                 RectF(kMargin + 28.0f, y + 12.0f, kMargin + 700.0f, y + 48.0f),
                                 Theme::kText, HAlign::Left, VAlign::Middle);
            renderer_.DrawString(
                rows[Idx(i)].hint, Font::RowSub,
                RectF(kMargin + 28.0f, y + 50.0f,
                      kCanvasWidth - kMargin - (i == kSettingUsb ? 790.0f : 360.0f), y + 78.0f),
                Theme::kTextSecondary, HAlign::Left, VAlign::Middle);
            renderer_.DrawString(
                rows[Idx(i)].state, Font::Button,
                RectF(kCanvasWidth - kMargin - (i == kSettingUsb ? 770.0f : 340.0f), y,
                      kCanvasWidth - kMargin - 28.0f, y + kRowHeight),
                rows[Idx(i)].state_color, HAlign::Right, VAlign::Middle);
            if (focused) {
                DrawRing(renderer_, row, kRowRadius + kRingGap, 1.0f);
            }
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
    int sources_count_ = 0; // sources saved by the Sources screen, shown on its Settings row
    Updater updater_;
    bool update_focused_ = false;
    bool update_open_ = false;
    BannerSource banners_; // the eShop banners of the hero, fetched off the render thread
    std::vector<int> banner_order_; // games whose banner is on the GPU, the oldest first

    UsbMode usb_mode_ = UsbMode::Unset;
    std::unique_ptr<UsbDetection> usb_detection_;
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
    int settings_row_ = 0;
    SyncAccount sync_account_ = SyncAccount::SignedOut;
    int launched_ = -1;
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

std::string RunLibrary(const winrt::Windows::UI::Core::CoreWindow& window, ChosenGame* chosen) {
    Diagnostic("UI library begin");
    std::string path;
    try {
        const std::filesystem::path local_state(std::wstring_view(
            winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()));
        Renderer renderer;
        renderer.Initialize(window);
        bool window_closed = false;
        if (!IsSetupDone(local_state)) {
            // The first run asks once whether to sync saves; the answer is remembered in
            // setup_done.txt, and Settings has the same switch.
            Input setup_input(window);
            window_closed = RunFirstRunSetup(renderer, window, setup_input, local_state);
        }
        if (!window_closed) {
            // The screen owns GPU images, so it has to be gone before the renderer is.
            LibraryScreen screen(renderer, window, local_state);
            path = screen.Run();
            if (!path.empty() && chosen != nullptr) {
                if (const GameEntry* game = screen.Chosen()) {
                    chosen->path = path;
                    chosen->title_id = game->title_id;
                    // SwitchSaveSync names the cloud folder after the first NACP name, not after
                    // the name in the UI language.
                    chosen->sync_name = SaveSync::GameNameFromNames(game->nacp_names);
                    if (chosen->sync_name.empty()) {
                        chosen->sync_name = winrt::to_string(winrt::hstring(game->name));
                    }
                    chosen->display_name = game->name;
                }
            }
        }
    } catch (const winrt::hresult_error& error) {
        Diagnostic("UI library failed " + winrt::to_string(error.message()));
        path.clear();
    } catch (const std::exception& error) {
        Diagnostic(std::string("UI library failed ") + error.what());
        path.clear();
    }
    Diagnostic(path.empty() ? "UI library end, no game chosen" : "UI library end");
    return path;
}

} // namespace EdenXbox::Ui
