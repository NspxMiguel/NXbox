// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "eden_uwp/ui/art.h"
#include "eden_uwp/ui/renderer.h"
#include "eden_uwp/ui/strings.h"

namespace EdenXbox::Ui {

enum class LaunchPhase { Lookup, Keys, Shaders, Starting, Failed };

// The shader callback publishes counters without sharing graphics objects with the UI thread.
struct LaunchStatus {
    std::atomic<LaunchPhase> phase{LaunchPhase::Lookup};
    std::atomic<std::size_t> built{0};
    std::atomic<std::size_t> total{0};
};

class LaunchScreen {
public:
    LaunchScreen(const winrt::Windows::UI::Core::CoreWindow& window,
                 const std::filesystem::path& local_state, std::string title_id);
    void SetName(const std::wstring& name);
    void Draw(const LaunchStatus& status, Text failure = Text::LaunchFailed,
              Pixels* capture = nullptr);
    void ReadFrame(Pixels& pixels);
    // Release on the UI thread before Mesa takes the CoreWindow. Recreate only on the worker.
    void Release();
    void InitializeOffscreen();

private:
    void LoadArt();
    bool LoadImage(const std::filesystem::path& path, Image& image);
    std::filesystem::path local_state_;
    std::string title_id_;
    std::wstring name_;
    BannerSource banners_;
    std::unique_ptr<Renderer> renderer_;
    Image art_;
    bool banner_ = false;
    std::chrono::steady_clock::time_point next_art_check_{};
};

} // namespace EdenXbox::Ui
