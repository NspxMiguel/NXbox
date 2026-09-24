// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.System.h>

#include "common/settings.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "input_common/input_engine.h"
#include "input_common/input_poller.h"

namespace EdenXbox {
// Engines the desktop frontends provide (keyboard, TAS, cameras...). The Xbox frontend has none of
// them; registering explicit null devices keeps behavior the same without logging an error for
// every device created.
template <typename Device>
class NullFactory final : public Common::Input::Factory<Device> {
public:
    std::unique_ptr<Device> Create(const Common::ParamPackage&) override {
        return std::make_unique<Device>();
    }
};

inline constexpr std::array UnsupportedEngines{"keyboard", "mouse",           "touch",
                                               "tas",      "virtual_gamepad", "virtual_amiibo",
                                               "camera",   "joycon",          "cemuhookudp"};

inline void RegisterUnsupportedEngines() {
    for (const auto* name : UnsupportedEngines) {
        Common::Input::RegisterInputFactory(
            name, std::make_shared<NullFactory<Common::Input::InputDevice>>());
        Common::Input::RegisterOutputFactory(
            name, std::make_shared<NullFactory<Common::Input::OutputDevice>>());
    }
}

inline void UnregisterUnsupportedEngines() {
    for (const auto* name : UnsupportedEngines) {
        Common::Input::UnregisterInputFactory(name);
        Common::Input::UnregisterOutputFactory(name);
    }
}

class XboxGamepad final : public InputCommon::InputEngine {
public:
    XboxGamepad() : InputEngine("nxbox") {
        PreSetController(pad);
        for (int i = 0; i < Settings::NativeButton::NumButtons; ++i)
            PreSetButton(pad, i);
        for (int i = 0; i < 4; ++i)
            PreSetAxis(pad, i);
    }

    static void Configure(const std::shared_ptr<XboxGamepad>& engine) {
        Common::Input::RegisterInputFactory("nxbox",
                                            std::make_shared<InputCommon::InputFactory>(engine));
        Common::Input::RegisterOutputFactory("nxbox",
                                             std::make_shared<InputCommon::OutputFactory>(engine));
        auto& player = Settings::values.players.GetValue()[0];
        player.connected = true;
        player.controller_type = Settings::ControllerType::ProController;
        for (int i = 0; i < Settings::NativeButton::NumButtons; ++i) {
            Common::ParamPackage parameters{{"engine", "nxbox"}, {"port", "0"}, {"pad", "0"}};
            parameters.Set("button", i);
            player.buttons[i] = parameters.Serialize();
        }
        for (int i = 0; i < 2; ++i) {
            Common::ParamPackage parameters{{"engine", "nxbox"}, {"port", "0"}, {"pad", "0"}};
            parameters.Set("axis_x", i * 2);
            parameters.Set("axis_y", i * 2 + 1);
            parameters.Set("deadzone", 0.15f);
            player.analogs[i] = parameters.Serialize();
        }
    }

    void Poll(Core::HID::EmulatedController& controller) {
        using namespace winrt::Windows::Gaming::Input;
        const auto devices = Gamepad::Gamepads();
        GamepadReading reading{};
        const auto keys = key_state.load(std::memory_order_acquire);
        const bool keys_seen = key_seen.load(std::memory_order_acquire);
        if (devices.Size() != 0) {
            reading = devices.GetAt(0).GetCurrentReading();
        }
        if (devices.Size() != 0 || keys_seen) {
            if (!controller.IsConnected())
                controller.Connect();
        } else if (controller.IsConnected()) {
            controller.Disconnect();
        }
        // Preserve button labels initially; a positional mapping can be selected by the launcher.
        constexpr std::array mappings{GamepadButtons::A,
                                      GamepadButtons::B,
                                      GamepadButtons::X,
                                      GamepadButtons::Y,
                                      GamepadButtons::LeftThumbstick,
                                      GamepadButtons::RightThumbstick,
                                      GamepadButtons::LeftShoulder,
                                      GamepadButtons::RightShoulder,
                                      GamepadButtons::None,
                                      GamepadButtons::None,
                                      GamepadButtons::Menu,
                                      GamepadButtons::View,
                                      GamepadButtons::DPadLeft,
                                      GamepadButtons::DPadUp,
                                      GamepadButtons::DPadRight,
                                      GamepadButtons::DPadDown};
        for (int i = 0; i < static_cast<int>(mappings.size()); ++i) {
            bool pressed = (reading.Buttons & mappings[i]) != GamepadButtons::None;
            if (i == Settings::NativeButton::ZL)
                pressed = reading.LeftTrigger > 0.5;
            if (i == Settings::NativeButton::ZR)
                pressed = reading.RightTrigger > 0.5;
            pressed = pressed || (keys & (1u << i)) != 0;
            SetButton(pad, i, pressed);
        }
        const auto key_axis = [keys](int positive, int negative, double value) {
            if (keys & (1u << positive))
                return 1.0f;
            if (keys & (1u << negative))
                return -1.0f;
            return static_cast<float>(value);
        };
        SetAxis(pad, 0, key_axis(KeyLeftStickRight, KeyLeftStickLeft, reading.LeftThumbstickX));
        SetAxis(pad, 1, key_axis(KeyLeftStickUp, KeyLeftStickDown, reading.LeftThumbstickY));
        SetAxis(pad, 2, static_cast<float>(reading.RightThumbstickX));
        SetAxis(pad, 3, static_cast<float>(reading.RightThumbstickY));
    }

    /// Gamepad virtual keys from the CoreWindow. Device Portal remote input only arrives this way,
    /// and returns true when the key belongs to the gamepad so the caller can mark it handled.
    bool OnKey(winrt::Windows::System::VirtualKey key, bool down) {
        const int bit = KeyBit(static_cast<int>(key));
        if (bit < 0)
            return false;
        key_seen.store(true, std::memory_order_release);
        if (down)
            key_state.fetch_or(1u << bit, std::memory_order_acq_rel);
        else
            key_state.fetch_and(~(1u << bit), std::memory_order_acq_rel);
        return true;
    }

private:
    // Bits 0-15 follow Settings::NativeButton; the rest are left-stick directions.
    static constexpr int KeyLeftStickUp = 16;
    static constexpr int KeyLeftStickDown = 17;
    static constexpr int KeyLeftStickRight = 18;
    static constexpr int KeyLeftStickLeft = 19;

    static int KeyBit(int key) {
        switch (key) {
        case 0xC3:
            return Settings::NativeButton::A;
        case 0xC4:
            return Settings::NativeButton::B;
        case 0xC5:
            return Settings::NativeButton::X;
        case 0xC6:
            return Settings::NativeButton::Y;
        case 0xC7:
            return Settings::NativeButton::R;
        case 0xC8:
            return Settings::NativeButton::L;
        case 0xC9:
            return Settings::NativeButton::ZL;
        case 0xCA:
            return Settings::NativeButton::ZR;
        case 0xCB:
            return Settings::NativeButton::DUp;
        case 0xCC:
            return Settings::NativeButton::DDown;
        case 0xCD:
            return Settings::NativeButton::DLeft;
        case 0xCE:
            return Settings::NativeButton::DRight;
        case 0xCF:
            return Settings::NativeButton::Plus;
        case 0xD0:
            return Settings::NativeButton::Minus;
        case 0xD1:
            return Settings::NativeButton::LStick;
        case 0xD2:
            return Settings::NativeButton::RStick;
        case 0xD3:
            return KeyLeftStickUp;
        case 0xD4:
            return KeyLeftStickDown;
        case 0xD5:
            return KeyLeftStickRight;
        case 0xD6:
            return KeyLeftStickLeft;
        default:
            return -1;
        }
    }

    const PadIdentifier pad{};
    std::atomic<std::uint32_t> key_state{0};
    std::atomic<bool> key_seen{false};
};
} // namespace EdenXbox
