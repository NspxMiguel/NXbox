// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Headless GATE-2 boot frontend for the Xbox/UWP AppContainer target.
//
// Brings up Core::System with the Null renderer + null audio sink, loads a homebrew NRO staged in
// the app's sandboxed local storage, runs it through the dynarmic JIT, and emits a deterministic
// JIT-liveness marker. No GPU device, no input, no audio device.
//
// The Core::System bring-up is modeled on the proven desktop boot in src/yuzu_cmd/yuzu.cpp; the
// WinRT IFrameworkView wrapper is the UWP entry point that drives it.
//
// JIT-LIVENESS CONTRACT (agreed with AGENT QA for the GATE-2 checklist): GATE 2 means "Eden
// executed guest code via the JIT", not "the process didn't crash". The homebrew NRO (NO
// keys/firmware/ROM — house rule) issues svcOutputDebugString with the exact sentinel below; Eden's
// SVC handler logs OutputDebugString, so observing this line is positive proof the JIT decoded +
// executed guest code.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/cpu_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/kernel/svc/svc_debug_string.h" // Kernel::Svc::SetDebugStringObserver
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "video_core/gpu.h"

#include "eden_uwp/headless_emu_window.h"
#include "eden_uwp/game_session.h"
#include "eden_uwp/protocol_uri.h"

namespace EdenXbox {

constexpr const char* JIT_LIVENESS_SENTINEL = "EDEN_XBOX_JIT_ALIVE";

// Force the renderer-independent, device-light configuration the headless boot needs.
static void ApplyHeadlessBootSettings() {
    Settings::values.renderer_backend = Settings::RendererBackend::Null; // no Vk/GL device created
    Settings::values.sink_id = Settings::AudioEngine::Null; // audio_core/sink/null_sink
    Settings::values.cpuopt_fastmem = false; // Phase-2: bounds-checked page-table path
    Settings::values.cpuopt_fastmem_exclusives = false;
    // memory_layout_mode stays at its default until the Series-S budget is measured on-console; the
    // DRAM clamp is a separate reservation follow-up, not here.
}

// Returns a process exit-style status. 0 == boot reached the run phase cleanly.
int RunHeadlessBoot(const std::string& nro_path) {
    Common::Log::Initialize();
    ApplyHeadlessBootSettings();

    Core::System system{};
    system.Initialize();
    system.ApplySettings();

    HeadlessEmuWindow emu_window{};

    // Filesystem + content plumbing, exactly as yuzu_cmd does it.
    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
    system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
    system.GetUserChannel().clear();

    Service::AM::FrontendAppletParameters load_parameters{};
    const Core::SystemResultStatus load_result = system.Load(emu_window, nro_path, load_parameters);
    if (load_result != Core::SystemResultStatus::Success) {
        LOG_CRITICAL(Frontend, "Headless boot: failed to load NRO {} (status {})", nro_path,
                     static_cast<int>(load_result));
        return 2;
    }

    // Install the JIT-liveness observer BEFORE running any guest code: it watches every guest
    // svcOutputDebugString chunk for the sentinel and signals the wait below. Cheap no-op for any
    // write that isn't the sentinel; zero cost in builds that never install an observer.
    std::mutex live_mutex;
    std::condition_variable live_cv;
    std::atomic<bool> jit_alive{false};
    Kernel::Svc::SetDebugStringObserver([&](std::string_view chunk) {
        if (chunk.find(JIT_LIVENESS_SENTINEL) != std::string_view::npos) {
            std::lock_guard lock(live_mutex);
            jit_alive.store(true, std::memory_order_release);
            live_cv.notify_all();
        }
    });

    SCOPE_EXIT {
        Kernel::Svc::SetDebugStringObserver(nullptr);
    };

    // Start the GPU host thread (null renderer — no device) and release the CPU manager.
    system.GPU().Start();
    system.GetCpuManager().OnGpuReady();

    // Run the guest. CpuManager spins up guest threads; dynarmic compiles + executes their code.
    void(system.Run());

    // Headless: no window event loop. Wait for the guest to execute through the JIT and emit the
    // sentinel; the timeout is only a backstop (a hung/failed boot), not the success path.
    constexpr auto kLivenessTimeout = std::chrono::seconds(30);
    {
        std::unique_lock lock(live_mutex);
        live_cv.wait_for(lock, kLivenessTimeout,
                         [&] { return jit_alive.load(std::memory_order_acquire); });
    }
    const bool alive = jit_alive.load(std::memory_order_acquire);

    Kernel::Svc::SetDebugStringObserver(nullptr); // detach before teardown
    void(system.Pause());
    system.ShutdownMainProcess();

    if (alive) {
        LOG_INFO(Frontend, "Headless boot: JIT liveness CONFIRMED ('{}' observed).",
                 JIT_LIVENESS_SENTINEL);
        return 0;
    }
    LOG_CRITICAL(Frontend, "Headless boot: JIT-liveness sentinel '{}' not observed within timeout.",
                 JIT_LIVENESS_SENTINEL);
    return 3;
}

} // namespace EdenXbox

// ============================================================================================
// UWP entry point: a CoreApplication IFrameworkView whose Run() drives RunHeadlessBoot() against
// the homebrew NRO bundled in the package install location (Package.InstalledLocation\boot.nro).
// Reading the fixed GATE-2 payload from the read-only install dir keeps the MSIX self-contained —
// no Device-Portal file-push or LocalState chicken-and-egg. (Eden's log still writes to the
// writable LocalFolder; see common/fs/path_util.cpp under YUZU_UWP_APPCONTAINER.)
// ============================================================================================
#include <atomic>
#include <fstream>
#include <thread>

#include <windows.h> // OutputDebugStringA/W + ::Sleep (sets the target-arch macros winnt.h needs)

#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Core.h>

using namespace winrt;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::UI::Core;

namespace {

// Best-effort startup diagnostics that survive an early crash (before Eden's own logging is up):
// append to a pullable file in the app's LocalFolder AND emit on the debugger channel. This is how
// we see *where* the headless boot fails on-console when no eden_log.txt and no crash dump are
// produced.
void WriteDiag(const std::string& msg) {
    const std::string line = "[eden-uwp] " + msg + "\n";
    OutputDebugStringA(line.c_str());
    try {
        const std::string local =
            winrt::to_string(Windows::Storage::ApplicationData::Current().LocalFolder().Path());
        std::ofstream f(local + "\\eden_uwp_diag.txt", std::ios::app);
        f << line;
    } catch (...) {
        OutputDebugStringW(L"[eden-uwp] WriteDiag: could not write diag file\n");
    }
}

// Set by the first activation of the process (normal launch or protocol), so Run() knows the
// activation arguments were delivered before it boots anything.
std::atomic<bool> g_activation_seen{false};

// A protocol activation "nxbox://play?title=<ID>" comes from a per-game launcher tile (see
// docs/nxbox-ui.md). The title ID goes to the game session, which boots that game directly.
void OnActivated(CoreApplicationView const&,
                 Windows::ApplicationModel::Activation::IActivatedEventArgs const& args) {
    try {
        if (args.Kind() == Windows::ApplicationModel::Activation::ActivationKind::Protocol) {
            const auto protocol =
                args.as<Windows::ApplicationModel::Activation::ProtocolActivatedEventArgs>();
            const std::string uri = winrt::to_string(protocol.Uri().AbsoluteUri());
            const std::string title = EdenXbox::ParsePlayTitleUri(uri);
            WriteDiag("protocol activation: " + uri);
            if (!title.empty()) {
                EdenXbox::SetProtocolPlayTitle(title);
            }
        }
    } catch (winrt::hresult_error const& e) {
        WriteDiag("protocol activation failed: " + winrt::to_string(e.message()));
    }
    g_activation_seen.store(true);
}

struct BootView : implements<BootView, IFrameworkViewSource, IFrameworkView> {
    IFrameworkView CreateView() {
        return *this;
    }
    void Initialize(CoreApplicationView const& view) {
        view.Activated([](CoreApplicationView const& sender,
                          Windows::ApplicationModel::Activation::IActivatedEventArgs const& args) {
            OnActivated(sender, args);
        });
    }
    void SetWindow(CoreWindow const&) {}
    void Load(hstring const&) {}
    void Uninitialize() {}

    void Run() {
        WriteDiag("BootView::Run entered");

        // A UWP app MUST activate its CoreWindow and pump the dispatcher, or the OS terminates it a
        // couple seconds after launch (no crash, no dump - exactly the "flashes then closes"
        // symptom). The hello-world/triangle apps survive because they render (activate + pump);
        // this headless boot did neither. Activate the (blank, Null-renderer) window, run the
        // blocking boot on a worker thread, and keep the UI thread pumping so the OS sees an
        // activated, responsive app.
        CoreWindow window = CoreWindow::GetForCurrentThread();
        window.Activate();

        // The activation event is queued on this dispatcher; deliver it now (bounded) so a
        // protocol launch is known before the library would be shown.
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!g_activation_seen.load() && std::chrono::steady_clock::now() < deadline) {
                window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                ::Sleep(10);
            }
            if (!g_activation_seen.load()) {
                WriteDiag("no activation event within 2 s, booting as a normal launch");
            }
        }

        const auto install_path =
            Windows::ApplicationModel::Package::Current().InstalledLocation().Path();
        const auto game_path = winrt::to_string(install_path) + "\\game.nro";
        if (std::filesystem::exists(game_path)) {
            try {
                EdenXbox::RunGameView(window, game_path);
            } catch (const std::exception& error) {
                WriteDiag(std::string("Graphics startup failed: ") + error.what());
            }
            return;
        }

        std::atomic<bool> done{false};
        std::thread worker([&done]() {
            SCOPE_EXIT {
                done.store(true);
            };
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
            } catch (...) {
                OutputDebugStringW(L"[eden-uwp] Worker apartment initialization failed\n");
                return;
            }
            SCOPE_EXIT {
                winrt::uninit_apartment();
            };
            std::string nro_path;
            try {
                // Bundled NRO from the read-only package install location.
                const auto install_path =
                    Windows::ApplicationModel::Package::Current().InstalledLocation().Path();
                nro_path = winrt::to_string(install_path) + "\\boot.nro";
                WriteDiag("resolved NRO path: " + nro_path);
            } catch (...) {
                WriteDiag("FAILED resolving Package.InstalledLocation");
            }
            // Capture any early throw to the diag file instead of a silent exit.
            try {
                WriteDiag("calling RunHeadlessBoot");
                const int rc = EdenXbox::RunHeadlessBoot(nro_path);
                WriteDiag("RunHeadlessBoot returned " + std::to_string(rc));
            } catch (winrt::hresult_error const& e) {
                WriteDiag("winrt::hresult_error: " + winrt::to_string(e.message()));
            } catch (std::exception const& e) {
                WriteDiag(std::string("std::exception: ") + e.what());
            } catch (...) {
                WriteDiag("unknown exception in RunHeadlessBoot");
            }
        });

        CoreDispatcher dispatcher = window.Dispatcher();
        while (!done.load()) {
            dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            ::Sleep(50);
        }
        worker.join();
        WriteDiag("boot worker joined; exiting");
    }
};

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    winrt::init_apartment();
    CoreApplication::Run(winrt::make<BootView>());
    return 0;
}
