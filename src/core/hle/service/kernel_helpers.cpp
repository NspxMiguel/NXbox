// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>
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
    : kernel(system_.Kernel()) {
    if (process = Kernel::GetCurrentProcessPointer(kernel); process != nullptr) {
        return;
    }

    // Create the process.
    process = Kernel::KProcess::Create(kernel);
    ASSERT(R_SUCCEEDED(process->Initialize(Kernel::Svc::CreateProcessParameter{},
                                           kernel.GetSystemResourceLimit(), false)));

    // Register the process.
    Kernel::KProcess::Register(kernel, process);
    process_created = true;
}

ServiceContext::~ServiceContext() {
    if (process_created) {
        process->Close();
        process = nullptr;
    }
}

Kernel::KEvent* ServiceContext::CreateEvent(std::string&& name) {
#ifdef _WIN32
    // NXbox diagnostic: something on this port creates far more KEvent objects than any well
    // behaved title should need. Track which names churn and report the running total and the
    // top offenders periodically.
    {
        static std::mutex counts_mutex;
        static std::unordered_map<std::string, unsigned> counts;
        static unsigned total = 0;
        std::scoped_lock lock{counts_mutex};
        ++total;
        // Names carry a numeric suffix in some services (event ids, slot indices); strip a
        // trailing "_<digits>" or "-<digits>" so those don't each get their own bucket.
        std::string bucket = name;
        auto pos = bucket.find_last_of("_-");
        if (pos != std::string::npos &&
            bucket.find_first_not_of("0123456789", pos + 1) == std::string::npos &&
            pos + 1 < bucket.size()) {
            bucket.resize(pos);
        }
        ++counts[bucket];
        if (total % 100 == 0) {
            std::vector<std::pair<std::string, unsigned>> sorted(counts.begin(), counts.end());
            std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second > b.second; });
            std::string top;
            for (size_t i = 0; i < std::min<size_t>(5, sorted.size()); ++i) {
                top += fmt::format("{}={} ", sorted[i].first, sorted[i].second);
            }
            LOG_CRITICAL(Service, "NXBOX CreateEvent total={} top: {}", total, top);
        }
    }
#endif
    // Reserve a new event from the process resource limit
    Kernel::KScopedResourceReservation event_reservation(process,
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
    event->Initialize(process);

    // Commit the thread reservation.
    event_reservation.Commit();

    // Register the event.
    Kernel::KEvent::Register(kernel, event);

    return event;
}

void ServiceContext::CloseEvent(Kernel::KEvent* event) {
    if (!event) {
        return;
    }
    event->GetReadableEvent().Close();
    event->Close();
}

} // namespace Service::KernelHelpers
