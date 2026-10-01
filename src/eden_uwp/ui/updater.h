// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace EdenXbox::Ui {

enum class UpdateState {
    Checking,
    None,
    Available,
    Downloading,
    Installing,
    Restarting,
    Failed,
    MissingPortal
};

// Owned by the library. Only StartInstall (A on the update pill) can start a deployment.
// The worker never touches rendering, and destruction cancels outstanding network operations.
class Updater {
public:
    Updater();
    ~Updater();
    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;

    UpdateState State() const {
        return state_.load();
    }
    double Progress() const;
    void StartInstall();

private:
    void Check();
    void Install();
    std::atomic<UpdateState> state_{UpdateState::Checking};
    std::atomic<bool> cancelled_{false};
    std::atomic<std::uint64_t> downloaded_{0};
    std::uint64_t size_ = 0;
    std::wstring url_;
    std::thread worker_;
};

} // namespace EdenXbox::Ui
