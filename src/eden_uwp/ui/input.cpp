// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/ui/input.h"

#include <cstdint>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.System.h>

namespace EdenXbox::Ui {
namespace {

using winrt::Windows::System::VirtualKey;
using winrt::Windows::UI::Core::KeyEventArgs;

// A stick counts as pushed past 0.55 and stays pushed until it falls below 0.35, so a stick
// resting near the threshold does not flicker.
constexpr double kStickPress = 0.55;
constexpr double kStickRelease = 0.35;

constexpr int Code(VirtualKey key) {
    return static_cast<int>(key);
}

// Gamepad virtual keys, as the CoreWindow delivers them.
constexpr int kGamepadA = 0xC3;
constexpr int kGamepadB = 0xC4;
constexpr int kGamepadX = 0xC5;
constexpr int kGamepadY = 0xC6;
constexpr int kGamepadRightShoulder = 0xC7;
constexpr int kGamepadLeftShoulder = 0xC8;
constexpr int kGamepadDPadUp = 0xCB;
constexpr int kGamepadDPadDown = 0xCC;
constexpr int kGamepadDPadLeft = 0xCD;
constexpr int kGamepadDPadRight = 0xCE;
constexpr int kGamepadMenu = 0xCF;
constexpr int kGamepadView = 0xD0;
constexpr int kGamepadStickUp = 0xD3;
constexpr int kGamepadStickDown = 0xD4;
constexpr int kGamepadStickRight = 0xD5;
constexpr int kGamepadStickLeft = 0xD6;

constexpr std::size_t Index(Button button) {
    return static_cast<std::size_t>(button);
}

// The button a key stands for, or Button::Count when the UI does not use the key.
Button ButtonForKey(int key) {
    switch (key) {
    case Code(VirtualKey::Left):
    case kGamepadDPadLeft:
    case kGamepadStickLeft:
        return Button::Left;
    case Code(VirtualKey::Right):
    case kGamepadDPadRight:
    case kGamepadStickRight:
        return Button::Right;
    case Code(VirtualKey::Up):
    case kGamepadDPadUp:
    case kGamepadStickUp:
        return Button::Up;
    case Code(VirtualKey::Down):
    case kGamepadDPadDown:
    case kGamepadStickDown:
        return Button::Down;
    case Code(VirtualKey::Enter):
    case Code(VirtualKey::Space):
    case kGamepadA:
        return Button::A;
    case Code(VirtualKey::Escape):
    case kGamepadB:
        return Button::B;
    case Code(VirtualKey::X):
    case kGamepadX:
        return Button::X;
    case Code(VirtualKey::Y):
    case kGamepadY:
        return Button::Y;
    case Code(VirtualKey::Q):
    case Code(VirtualKey::PageUp):
    case kGamepadLeftShoulder:
        return Button::LeftShoulder;
    case Code(VirtualKey::E):
    case Code(VirtualKey::PageDown):
    case kGamepadRightShoulder:
        return Button::RightShoulder;
    case kGamepadView:
        return Button::View;
    case kGamepadMenu:
        return Button::Menu;
    default:
        return Button::Count;
    }
}

} // namespace

Input::Input(const winrt::Windows::UI::Core::CoreWindow& window) : window_(window) {
    key_down_token_ = window_.KeyDown([this](const auto&, const KeyEventArgs& args) {
        const Button button = ButtonForKey(static_cast<int>(args.VirtualKey()));
        if (button != Button::Count) {
            keys_[Index(button)] = true;
            args.Handled(true);
        }
    });
    key_up_token_ = window_.KeyUp([this](const auto&, const KeyEventArgs& args) {
        const Button button = ButtonForKey(static_cast<int>(args.VirtualKey()));
        if (button != Button::Count) {
            keys_[Index(button)] = false;
            args.Handled(true);
        }
    });
    // A key released while another app had the focus never reports its KeyUp.
    activated_token_ = window_.Activated([this](const auto&, const auto&) { keys_.fill(false); });
}

Input::~Input() {
    window_.KeyDown(key_down_token_);
    window_.KeyUp(key_up_token_);
    window_.Activated(activated_token_);
}

void Input::PollGamepads(std::array<bool, kButtonCount>& held) const {
    using namespace winrt::Windows::Gaming::Input;
    const auto pads = Gamepad::Gamepads();
    for (std::uint32_t pad_index = 0; pad_index < pads.Size(); ++pad_index) {
        const GamepadReading reading = pads.GetAt(pad_index).GetCurrentReading();
        const auto pressed = [&reading](GamepadButtons flag) {
            return (reading.Buttons & flag) != GamepadButtons::None;
        };
        const auto pushed = [this](Button button, double value) {
            return value > (state_.Down(button) ? kStickRelease : kStickPress);
        };
        const auto merge = [&held](Button button, bool value) {
            held[Index(button)] = held[Index(button)] || value;
        };
        merge(Button::Left, pressed(GamepadButtons::DPadLeft) ||
                                pushed(Button::Left, -reading.LeftThumbstickX));
        merge(Button::Right, pressed(GamepadButtons::DPadRight) ||
                                 pushed(Button::Right, reading.LeftThumbstickX));
        merge(Button::Up, pressed(GamepadButtons::DPadUp) ||
                              pushed(Button::Up, reading.LeftThumbstickY));
        merge(Button::Down, pressed(GamepadButtons::DPadDown) ||
                                pushed(Button::Down, -reading.LeftThumbstickY));
        merge(Button::A, pressed(GamepadButtons::A));
        merge(Button::B, pressed(GamepadButtons::B));
        merge(Button::X, pressed(GamepadButtons::X));
        merge(Button::Y, pressed(GamepadButtons::Y));
        merge(Button::LeftShoulder, pressed(GamepadButtons::LeftShoulder));
        merge(Button::RightShoulder, pressed(GamepadButtons::RightShoulder));
        merge(Button::View, pressed(GamepadButtons::View));
        merge(Button::Menu, pressed(GamepadButtons::Menu));
    }
}

void Input::Update() {
    std::array<bool, kButtonCount> held = keys_;
    PollGamepads(held);
    state_.Update(held, InputState::Clock::now());
}

bool Input::Pressed(Button button) const {
    return state_.Pressed(button);
}

void Input::ConsumeUntilRelease() {
    state_.ConsumeUntilRelease();
}

} // namespace EdenXbox::Ui
