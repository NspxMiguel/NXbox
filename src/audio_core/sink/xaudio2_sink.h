// SPDX-FileCopyrightText: Copyright 2026 NXbox contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "audio_core/sink/sink.h"

namespace Core {
class System;
}

namespace AudioCore::Sink {
class SinkStream;

/**
 * XAudio2 backend sink. XAudio2 is available to UWP apps on Xbox, where neither cubeb's WASAPI
 * device enumeration nor SDL is built; used for Audio Render and Audio Out (Audio In is silent).
 */
class XAudio2Sink final : public Sink {
public:
    explicit XAudio2Sink(std::string_view device_id);
    ~XAudio2Sink() override;

    SinkStream* AcquireSinkStream(Core::System& system, u32 system_channels,
                                  const std::string& name, StreamType type) override;
    void CloseStream(SinkStream* stream) override;
    void CloseStreams() override;
    f32 GetDeviceVolume() const override;
    void SetDeviceVolume(f32 volume) override;
    void SetSystemVolume(f32 volume) override;

    /// The XAudio2 engine and mastering voice, shared with the streams that play through it.
    struct Engine;

private:
    std::shared_ptr<Engine> engine;
    std::vector<SinkStreamPtr> sink_streams;
};

std::vector<std::string> ListXAudio2SinkDevices(bool capture);
u32 GetXAudio2Latency();

} // namespace AudioCore::Sink
