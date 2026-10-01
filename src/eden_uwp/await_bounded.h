// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <future>
#include <optional>
#include <thread>
#include <utility>

#include <winrt/base.h>

namespace EdenXbox {

// Worker threads only. Keep the operation alive after timeout, without a std::async future whose
// destructor would wait forever. The waiter has its own MTA apartment; it never blocks the UI.
template <typename Awaitable>
auto AwaitBounded(Awaitable awaitable, std::chrono::seconds timeout)
    -> std::optional<decltype(awaitable.get())> {
    using Result = decltype(awaitable.get());
    std::packaged_task<Result()> task([awaitable] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        struct Apartment {
            ~Apartment() { winrt::uninit_apartment(); }
        } apartment;
        return awaitable.get();
    });
    auto future = task.get_future();
    std::thread(std::move(task)).detach();
    if (future.wait_for(timeout) != std::future_status::ready) {
        awaitable.Cancel();
        return std::nullopt;
    }
    return future.get();
}

} // namespace EdenXbox
