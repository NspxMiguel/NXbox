// SPDX-FileCopyrightText: Copyright 2026 NXbox contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include <windows.h>
#include <xaudio2.h>

#include "audio_core/common/common.h"
#include "audio_core/sink/sink_stream.h"
#include "audio_core/sink/xaudio2_sink.h"
#include "common/logging.h"

namespace AudioCore::Sink {

struct XAudio2Sink::Engine {
    IXAudio2* xaudio{};
    IXAudio2MasteringVoice* master{};

    ~Engine() {
        if (master) {
            master->DestroyVoice();
        }
        if (xaudio) {
            xaudio->Release();
        }
    }
};

namespace {
/// Frames per submitted buffer (10 ms at 48 kHz) and how many are kept queued.
constexpr u32 FramesPerBuffer = TargetSampleCount * 2;
constexpr size_t QueuedBuffers = 4;

class XAudio2SinkStream final : public SinkStream, private IXAudio2VoiceCallback {
public:
    XAudio2SinkStream(std::shared_ptr<XAudio2Sink::Engine> engine_, u32 device_channels_,
                      u32 system_channels_, StreamType type_, Core::System& system_)
        : SinkStream{system_, type_}, engine{std::move(engine_)} {
        device_channels = device_channels_;
        system_channels = system_channels_;
        if (type == StreamType::In || engine == nullptr || engine->xaudio == nullptr) {
            // No capture device on the console; the stream stays silent.
            return;
        }
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = static_cast<WORD>(device_channels);
        format.nSamplesPerSec = TargetSampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(format.nChannels * sizeof(s16));
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        const HRESULT hr = engine->xaudio->CreateSourceVoice(&voice, &format, 0,
                                                             XAUDIO2_DEFAULT_FREQ_RATIO, this);
        if (FAILED(hr)) {
            LOG_CRITICAL(Audio_Sink, "XAudio2 CreateSourceVoice failed: {:#010x}",
                         static_cast<u32>(hr));
            voice = nullptr;
            return;
        }
        for (auto& buffer : buffers) {
            buffer.resize(static_cast<size_t>(FramesPerBuffer) * device_channels);
        }
        feeder = std::jthread([this](std::stop_token token) { Feed(token); });
    }

    ~XAudio2SinkStream() override {
        Finalize();
    }

    void Finalize() override {
        if (feeder.joinable()) {
            feeder.request_stop();
            Wake();
            feeder.join();
        }
        if (voice) {
            voice->Stop();
            voice->FlushSourceBuffers();
            voice->DestroyVoice();
            voice = nullptr;
        }
    }

    void Start(bool resume = false) override {
        if (voice == nullptr || !paused) {
            return;
        }
        paused = false;
        voice->Start();
        Wake();
    }

    void Stop() override {
        if (voice == nullptr || paused) {
            return;
        }
        SignalPause();
        voice->Stop();
    }

private:
    /// Taking the lock before notifying keeps the feeder from missing a wake-up between its
    /// predicate check and its wait.
    void Wake() {
        { std::scoped_lock lock{mutex}; }
        ready.notify_all();
    }

    void Feed(std::stop_token token) {
        size_t next = 0;
        while (!token.stop_requested()) {
            {
                std::unique_lock lock{mutex};
                ready.wait(lock, [&] {
                    return token.stop_requested() ||
                           (!paused && queued.load() < QueuedBuffers);
                });
            }
            if (token.stop_requested()) {
                break;
            }
            auto& buffer = buffers[next];
            ProcessAudioOutAndRender(std::span<s16>{buffer}, FramesPerBuffer);
            XAUDIO2_BUFFER submit{};
            submit.AudioBytes = static_cast<UINT32>(buffer.size() * sizeof(s16));
            submit.pAudioData = reinterpret_cast<const BYTE*>(buffer.data());
            queued++;
            if (FAILED(voice->SubmitSourceBuffer(&submit))) {
                queued--;
            }
            next = (next + 1) % QueuedBuffers;
        }
    }

    // IXAudio2VoiceCallback
    void STDMETHODCALLTYPE OnBufferEnd(void*) override {
        queued--;
        Wake();
    }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT error) override {
        LOG_ERROR(Audio_Sink, "XAudio2 voice error {:#010x}", static_cast<u32>(error));
    }

    std::shared_ptr<XAudio2Sink::Engine> engine;
    IXAudio2SourceVoice* voice{};
    std::array<std::vector<s16>, QueuedBuffers> buffers;
    std::atomic<size_t> queued{0};
    std::mutex mutex;
    std::condition_variable ready;
    std::jthread feeder;
};
} // namespace

XAudio2Sink::XAudio2Sink(std::string_view) : engine{std::make_shared<Engine>()} {
    device_channels = 2;
    HRESULT hr = XAudio2Create(&engine->xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (SUCCEEDED(hr)) {
        hr = engine->xaudio->CreateMasteringVoice(&engine->master, 2, TargetSampleRate);
    }
    if (FAILED(hr)) {
        LOG_CRITICAL(Audio_Sink, "XAudio2 initialisation failed: {:#010x}",
                     static_cast<u32>(hr));
        engine.reset();
        return;
    }
    LOG_INFO(Audio_Sink, "XAudio2 sink ready: 2 channels at {} Hz", TargetSampleRate);
}

XAudio2Sink::~XAudio2Sink() {
    // Streams hold voices that must be destroyed before the engine.
    sink_streams.clear();
}

SinkStream* XAudio2Sink::AcquireSinkStream(Core::System& system, u32 system_channels_,
                                           const std::string&, StreamType type) {
    system_channels = system_channels_;
    SinkStreamPtr& stream = sink_streams.emplace_back(std::make_unique<XAudio2SinkStream>(
        engine, device_channels, system_channels, type, system));
    return stream.get();
}

void XAudio2Sink::CloseStream(SinkStream* stream) {
    for (size_t i = 0; i < sink_streams.size(); i++) {
        if (sink_streams[i].get() == stream) {
            sink_streams.erase(sink_streams.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
}

void XAudio2Sink::CloseStreams() {
    sink_streams.clear();
}

f32 XAudio2Sink::GetDeviceVolume() const {
    return sink_streams.empty() ? 1.0f : sink_streams[0]->GetDeviceVolume();
}

void XAudio2Sink::SetDeviceVolume(f32 volume) {
    for (auto& stream : sink_streams) {
        stream->SetDeviceVolume(volume);
    }
}

void XAudio2Sink::SetSystemVolume(f32 volume) {
    for (auto& stream : sink_streams) {
        stream->SetSystemVolume(volume);
    }
}

std::vector<std::string> ListXAudio2SinkDevices(bool capture) {
    return capture ? std::vector<std::string>{} : std::vector<std::string>{"Default"};
}

u32 GetXAudio2Latency() {
    return FramesPerBuffer * static_cast<u32>(QueuedBuffers);
}

} // namespace AudioCore::Sink
