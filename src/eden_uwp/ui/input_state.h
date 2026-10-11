// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <cstddef>

namespace EdenXbox::Ui {

enum class Button {
    Left,
    Right,
    Up,
    Down,
    A,
    B,
    X,
    Y,
    LeftShoulder,
    RightShoulder,
    View,
    Menu,
    Count,
};

inline constexpr std::size_t kButtonCount = static_cast<std::size_t>(Button::Count);

// Portable edge detection shared by every UI screen.
class InputState {
public:
    using Clock = std::chrono::steady_clock;
    using Buttons = std::array<bool, kButtonCount>;

    void ConsumeUntilRelease() {
        fired_.fill(false);
        blocked_.fill(true);
    }

    void Update(const Buttons& held, Clock::time_point now) {
        for (std::size_t index = 0; index < kButtonCount; ++index) {
            fired_[index] = false;
            if (!held[index]) {
                down_[index] = false;
                blocked_[index] = false;
                continue;
            }
            if (blocked_[index]) {
                down_[index] = true;
                repeat_at_[index] = now + std::chrono::milliseconds(400);
                continue;
            }
            if (!down_[index]) {
                fired_[index] = true;
                repeat_at_[index] = now + std::chrono::milliseconds(400);
            } else if (index <= static_cast<std::size_t>(Button::Down) &&
                       now >= repeat_at_[index]) {
                fired_[index] = true;
                repeat_at_[index] = now + std::chrono::milliseconds(120);
            }
            down_[index] = true;
        }
    }

    bool Down(Button button) const {
        return down_[static_cast<std::size_t>(button)];
    }

    bool Pressed(Button button) const {
        return fired_[static_cast<std::size_t>(button)];
    }

private:
    Buttons down_{};
    Buttons fired_{};
    Buttons blocked_{};
    std::array<Clock::time_point, kButtonCount> repeat_at_{};
};

// Nested loops share Input. Fence both sides, including early returns and exceptions.
template <typename InputType>
class InputTransition {
public:
    explicit InputTransition(InputType& input) : input_(input) {
        input_.ConsumeUntilRelease();
    }
    ~InputTransition() {
        input_.ConsumeUntilRelease();
    }
    InputTransition(const InputTransition&) = delete;
    InputTransition& operator=(const InputTransition&) = delete;

private:
    InputType& input_;
};

} // namespace EdenXbox::Ui
