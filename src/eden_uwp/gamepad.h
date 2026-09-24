// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>

#include "common/settings.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "input_common/input_engine.h"
#include "input_common/input_poller.h"

namespace EdenXbox {
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
        if (devices.Size() != 0) {
            reading = devices.GetAt(0).GetCurrentReading();
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
            SetButton(pad, i, pressed);
        }
        SetAxis(pad, 0, static_cast<float>(reading.LeftThumbstickX));
        SetAxis(pad, 1, static_cast<float>(reading.LeftThumbstickY));
        SetAxis(pad, 2, static_cast<float>(reading.RightThumbstickX));
        SetAxis(pad, 3, static_cast<float>(reading.RightThumbstickY));
    }

private:
    const PadIdentifier pad{};
};
} // namespace EdenXbox
