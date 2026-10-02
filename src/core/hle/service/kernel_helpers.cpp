// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
#include <atomic>
#include <cstdio>
#include <cstring>
#include "common/logging.h"
#endif
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_memory_manager.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/kernel/k_resource_limit.h"
#include "core/hle/kernel/k_scoped_resource_reservation.h"
#include "core/hle/service/kernel_helpers.h"

namespace Service::KernelHelpers {

ServiceContext::ServiceContext(Core::System& system_, std::string name_)
    : kernel(system_.Kernel())
{
    if (process = Kernel::GetCurrentProcessPointer(kernel); process != nullptr) {
        return;
    }

    // Create the process.
    process = Kernel::KProcess::Create(kernel);
    ASSERT(R_SUCCEEDED(process->Initialize(kernel, Kernel::Svc::CreateProcessParameter{}, kernel.GetSystemResourceLimit(), false)));

    // Register the process.
    Kernel::KProcess::Register(kernel, process);
    process_created = true;
}

ServiceContext::~ServiceContext() {
    if (process_created) {
        process->Close(kernel);
        process = nullptr;
    }
}

Kernel::KEvent* ServiceContext::CreateEvent(std::string&& name) {
#ifdef _WIN32
    // NXbox diagnostic: something on this port creates far more KEvent objects than any well
    // behaved title should need. Track a small fixed set of distinct names (POD-only static
    // storage, no runtime-constructed statics, so this is safe to run this early in boot) and
    // report counts periodically.
    {
        struct Slot {
            std::atomic<bool> used;
            char name[48];
            std::atomic<unsigned> count;
        };
        static Slot slots[24];
        static std::atomic<unsigned> total{0};
        const unsigned my_total = total.fetch_add(1, std::memory_order_relaxed) + 1;
        for (auto& slot : slots) {
            if (!slot.used.load(std::memory_order_acquire)) {
                bool expected = false;
                if (slot.used.compare_exchange_strong(expected, true)) {
                    std::snprintf(slot.name, sizeof(slot.name), "%s", name.c_str());
                    slot.count.store(1, std::memory_order_relaxed);
                    break;
                }
                continue;
            }
            if (std::strncmp(slot.name, name.c_str(), sizeof(slot.name) - 1) == 0) {
                slot.count.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
        if (my_total % 100 == 0) {
            for (auto& slot : slots) {
                if (slot.used.load(std::memory_order_acquire)) {
                    LOG_CRITICAL(Service, "NXBOX CreateEvent[{}] name={} count={}", my_total,
                                 slot.name, slot.count.load(std::memory_order_relaxed));
                }
            }
        }
    }
#endif
    // Reserve a new event from the process resource limit
    Kernel::KScopedResourceReservation event_reservation(kernel, process,
                                                         Kernel::LimitableResource::EventCountMax);
    if (!event_reservation.Succeeded()) {
        LOG_CRITICAL(Service, "Resource limit reached!");
        return {};
    }

    // Create a new event.
    auto* event = Kernel::KEvent::Create(kernel);
    if (!event) {
        LOG_CRITICAL(Service, "Unable to create event!");
        return {};
    }

    // Initialize the event.
    event->Initialize(kernel, process);

    // Commit the thread reservation.
    event_reservation.Commit();

    // Register the event.
    Kernel::KEvent::Register(kernel, event);

    return event;
}

void ServiceContext::CloseEvent(Kernel::KEvent* event) {
    if (event) {
        event->GetReadableEvent().Close(kernel);
        event->Close(kernel);
    }
}

} // namespace Service::KernelHelpers
