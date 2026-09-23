// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <memory>
#include <windows.h>
#include <winrt/Windows.UI.Core.h>

#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"

namespace EdenXbox {
struct MesaRuntime;
class MesaWindow final : public Core::Frontend::EmuWindow {
public:
    MesaWindow(const winrt::Windows::UI::Core::CoreWindow& window, u32 width, u32 height);
    ~MesaWindow() override;
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override;
    bool IsShown() const override;
    void OnFrameDisplayed() override;
    u64 FrameCount() const;

private:
    winrt::Windows::UI::Core::CoreWindow window;
    std::shared_ptr<MesaRuntime> runtime;
    std::atomic<u64> frames{0};
};
} // namespace EdenXbox
