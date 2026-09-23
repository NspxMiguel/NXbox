// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The Vulkan Memory Allocator implementation must be compiled in exactly one TU per binary that
// links video_core (GPUOpen::VulkanMemoryAllocator is a header-only INTERFACE target).
// yuzu_cmd/yuzu.cpp do this in their main TU; the headless UWP frontend keeps it in its own TU so
// uwp_boot.cpp stays clean of the WinRT/VMA mix. Null renderer never allocates through VMA, but
// video_core references the symbols, so the implementation must still be present at link time.
#define VMA_IMPLEMENTATION
#include "video_core/vulkan_common/vma.h"
