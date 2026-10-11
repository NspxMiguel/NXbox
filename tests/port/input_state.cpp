// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <chrono>
#include <stdexcept>

#include "eden_uwp/ui/input_state.h"

using namespace EdenXbox::Ui;
using namespace std::chrono_literals;

int main() {
    InputState input;
    InputState::Buttons held{};
    auto now = InputState::Clock::time_point{};
    const auto set = [&](Button button, bool down) {
        held[static_cast<std::size_t>(button)] = down;
    };
    const auto poll = [&](auto elapsed) {
        now += elapsed;
        input.Update(held, now);
    };

    // Every button must be released and pressed again across a screen transition.
    for (std::size_t index = 0; index < kButtonCount; ++index) {
        const auto button = static_cast<Button>(index);
        held.fill(false);
        poll(1ms);
        set(button, true);
        poll(1ms);
        assert(input.Pressed(button));
        input.ConsumeUntilRelease();
        assert(!input.Pressed(button)); // also clears the current frame
        poll(2s);
        assert(!input.Pressed(button)); // no held-direction repeats across screens
        set(button, false);
        poll(1ms);
        assert(!input.Pressed(button)); // release never confirms or cancels
        set(button, true);
        poll(1ms);
        assert(input.Pressed(button));
    }

    held.fill(false);
    poll(1ms);
    set(Button::X, true);
    poll(1ms);
    assert(input.Pressed(Button::X));
    {
        const InputTransition transition(input);
        assert(!input.Pressed(Button::X));
        poll(1ms);
        assert(!input.Pressed(Button::X));
        set(Button::X, false);
        poll(1ms);
        set(Button::B, true);
        poll(1ms);
        assert(input.Pressed(Button::B)); // B belongs to Mods only
    }
    assert(!input.Pressed(Button::B)); // resumed library handler must not quit
    assert(!input.Pressed(Button::A)); // or launch
    poll(1ms);
    assert(!input.Pressed(Button::B));

    // Even a button first held after the transition is blocked until sampled released.
    input.ConsumeUntilRelease();
    set(Button::A, true);
    poll(1ms);
    assert(!input.Pressed(Button::A));
    held.fill(false);
    poll(1ms);
    set(Button::A, true);
    poll(1ms);
    assert(input.Pressed(Button::A)); // deliberate library launch still works

    // Credits can close with A; an exceptional/early return must fence it as well.
    try {
        const InputTransition transition(input);
        held.fill(false);
        poll(1ms);
        set(Button::A, true);
        set(Button::B, true);
        poll(1ms);
        assert(input.Pressed(Button::A) && input.Pressed(Button::B));
        throw std::runtime_error("screen exit");
    } catch (const std::runtime_error&) {
    }
    assert(!input.Pressed(Button::A) && !input.Pressed(Button::B));
    poll(2s);
    assert(!input.Pressed(Button::A) && !input.Pressed(Button::B));

    // Ordinary direction repeats and action-button edges keep their original timing.
    held.fill(false);
    poll(1ms);
    set(Button::Left, true);
    set(Button::A, true);
    poll(1ms);
    assert(input.Pressed(Button::Left) && input.Pressed(Button::A));
    poll(399ms);
    assert(!input.Pressed(Button::Left) && !input.Pressed(Button::A));
    poll(1ms);
    assert(input.Pressed(Button::Left) && !input.Pressed(Button::A));
    poll(119ms);
    assert(!input.Pressed(Button::Left));
    poll(1ms);
    assert(input.Pressed(Button::Left));
}
