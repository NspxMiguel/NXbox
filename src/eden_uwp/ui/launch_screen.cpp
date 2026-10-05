// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/ui/launch_screen.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include "eden_uwp/diagnostic.h"
#include "eden_uwp/ui/theme.h"

namespace EdenXbox::Ui {

LaunchScreen::LaunchScreen(const winrt::Windows::UI::Core::CoreWindow& window,
                           const std::filesystem::path& local_state, std::string title_id)
    : local_state_(local_state), title_id_(std::move(title_id)),
      name_(winrt::to_hstring(title_id_).c_str()), banners_(local_state, true),
      renderer_(std::make_unique<Renderer>()) {
    // Cached metadata is only for display; LibraryScan still validates the actual game path.
    try {
        const auto file = local_state_ / "library" / (title_id_ + ".json");
        std::error_code ec;
        if (std::filesystem::file_size(file, ec) <= (1u << 20) && !ec) {
            std::ifstream in(file, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)), {});
            winrt::Windows::Data::Json::JsonObject record;
            if (winrt::Windows::Data::Json::JsonObject::TryParse(winrt::to_hstring(text), record)) {
                SetName(record.GetNamedString(L"name").c_str());
            }
        }
    } catch (const winrt::hresult_error&) {
        // Missing or old metadata must not block a launch.
    }
    renderer_->Initialize(window);
    banners_.Request(title_id_);
    Diagnostic("PROTOCOL_SPLASH " + title_id_);
}

void LaunchScreen::SetName(const std::wstring& name) {
    if (!name.empty()) {
        name_ = name;
    }
}

bool LaunchScreen::LoadImage(const std::filesystem::path& path, Image& image) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > (16u << 20)) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
    Pixels pixels;
    return renderer_->DecodeImage(bytes, pixels) && renderer_->UploadImage(pixels, image);
}

void LaunchScreen::LoadArt() {
    if (banner_ || std::chrono::steady_clock::now() < next_art_check_) {
        return;
    }
    next_art_check_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    // Read a cached banner immediately, even when the index refresh is still offline/pending.
    const auto banner = local_state_ / "library" / (title_id_ + ".banner.jpg");
    Image image;
    if (LoadImage(banner, image)) {
        art_ = std::move(image);
        banner_ = true;
    } else if (!art_.bitmap) {
        LoadImage(local_state_ / "library" / (title_id_ + ".jpg"), art_);
    }
}

void LaunchScreen::Draw(const LaunchStatus& status, Text failure, Pixels* capture) {
    LoadArt();
    renderer_->BeginFrame();
    if (art_.bitmap) {
        renderer_->DrawImage(
            art_, banner_ ? D2D1::RectF(0, 0, 1920, 1080) : D2D1::RectF(720, 120, 1200, 600), 0, 1);
    }
    auto clear = Theme::kBackground;
    clear.a = 0;
    const D2D1_GRADIENT_STOP scrim[] = {
        {0, clear}, {0.48f, clear}, {0.72f, Theme::kBackground}, {1, Theme::kBackground}};
    renderer_->FillGradient(D2D1::RectF(0, 0, 1920, 1080), 0, scrim, 4, D2D1::Point2F(0, 0),
                            D2D1::Point2F(0, 1080));
    renderer_->DrawString(name_, Font::TitleSmall, D2D1::RectF(120, 790, 1800, 930), Theme::kText);
    const auto phase = status.phase.load();
    Text text = Text::LaunchLookup;
    float progress = 0.08f;
    switch (phase) {
    case LaunchPhase::Lookup:
        break;
    case LaunchPhase::Keys:
        text = Text::LaunchKeys;
        progress = 0.2f;
        break;
    case LaunchPhase::Shaders:
        text = Text::LaunchShaders;
        progress = 0.4f;
        break;
    case LaunchPhase::Starting:
        text = Text::LaunchStarting;
        progress = 0.95f;
        break;
    case LaunchPhase::Failed:
        text = failure;
        progress = 0;
        break;
    }
    std::wstring label = Tr(text);
    if (phase == LaunchPhase::Shaders) {
        const auto built = status.built.load();
        const auto total = status.total.load();
        label += L"  " + std::to_wstring(built);
        if (total > 0) {
            label += L" / " + std::to_wstring(total);
            progress += 0.5f * static_cast<float>(std::min(built, total)) / total;
        }
    }
    renderer_->DrawString(label, Font::Body, D2D1::RectF(120, 942, 1800, 1000),
                          Theme::kTextSecondary);
    renderer_->FillRounded(D2D1::RectF(120, 1016, 1800, 1019), 1.5f, Theme::kSurfaceStrong);
    if (progress > 0) {
        renderer_->FillRounded(D2D1::RectF(120, 1016, 120 + 1680 * progress, 1019), 1.5f,
                               Theme::kText);
    }
    renderer_->EndFrame(capture);
}

void LaunchScreen::ReadFrame(Pixels& pixels) {
    renderer_->ReadFrame(pixels);
}

void LaunchScreen::Release() {
    art_ = {};
    renderer_.reset();
    banner_ = false;
    next_art_check_ = {};
}

void LaunchScreen::InitializeOffscreen() {
    renderer_ = std::make_unique<Renderer>();
    renderer_->Initialize(nullptr);
}

} // namespace EdenXbox::Ui
