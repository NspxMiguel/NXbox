// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Minimal, SDL-free, headless EmuWindow for the Xbox/UWP headless boot (Phase 2, toward GATE 2).
//
// The abstract Core::Frontend::EmuWindow has only two pure-virtuals (CreateSharedContext, IsShown)
// and Core::Frontend::GraphicsContext is already concrete (no-op defaults), so under the Null
// renderer the whole window collapses to the few lines below: no GPU, no input, no windowing.
// Authored from AGENT CORE's boot-skeleton, validated against the real EmuWindow interface.

#pragma once

#include <memory>

#include "common/common_types.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"

namespace EdenXbox {

/// Headless EmuWindow for null-renderer boot. No surface, never minimized, hands back a no-op
/// context.
class HeadlessEmuWindow final : public Core::Frontend::EmuWindow {
public:
    explicit HeadlessEmuWindow(u32 width = 1280, u32 height = 720) {
        // Headless: no native surface. The video backend treats render_surface == nullptr as
        // headless.
        window_info.type = Core::Frontend::WindowSystemType::Headless;
        window_info.render_surface = nullptr;
        window_info.render_surface_scale = 1.0f;
        // Seed a framebuffer layout so anything querying it (even the null renderer's present path)
        // gets a self-consistent size.
        UpdateCurrentFramebufferLayout(width, height);
    }

    ~HeadlessEmuWindow() override = default;

    // The Null renderer needs a context object but never touches the surface. GraphicsContext's
    // base implementation is already a complete no-op, so hand one back directly.
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override {
        return std::make_unique<Core::Frontend::GraphicsContext>();
    }

    // Headless app is always "shown" — there is no minimize/occlusion concept on the boot path.
    bool IsShown() const override {
        return true;
    }
};

} // namespace EdenXbox
