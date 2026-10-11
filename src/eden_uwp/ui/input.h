// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>

#include <windows.h>

#include <winrt/Windows.UI.Core.h>

#include "eden_uwp/ui/anim.h"
#include "eden_uwp/ui/input_state.h"

namespace EdenXbox::Ui {

// Controller and keyboard input for the screens, polled once per frame on the UI thread.
//
// The D-pad and the left stick both drive Left/Right/Up/Down. A held direction repeats after
// 400 ms and then every 120 ms. The CoreWindow key events cover the keyboard (for debugging) and
// the gamepad virtual keys that Device Portal remote input arrives as; the keys it uses are marked
// handled so the system does not treat B or Menu as navigation.
class Input {
public:
    explicit Input(const winrt::Windows::UI::Core::CoreWindow& window);
    ~Input();
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;

    // Samples the pads and the keys. Call once per frame, after the dispatcher has run.
    void Update();

    // True on the frame the button went down and, for directions, on each auto-repeat.
    bool Pressed(Button button) const;

    // Clears this frame and suppresses each button until it has been sampled released.
    void ConsumeUntilRelease();

private:
    void PollGamepads(std::array<bool, kButtonCount>& held) const;

    winrt::Windows::UI::Core::CoreWindow window_;
    winrt::event_token key_down_token_;
    winrt::event_token key_up_token_;
    winrt::event_token activated_token_;
    std::array<bool, kButtonCount> keys_{};  // held, from the key events
    InputState state_;
};

} // namespace EdenXbox::Ui
