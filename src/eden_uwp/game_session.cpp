// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/game_session.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.System.h>

#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/cpu_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/kernel/svc/svc_debug_string.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "eden_uwp/diagnostic.h"
#include "eden_uwp/gamepad.h"
#include "eden_uwp/mesa_window.h"
#include "video_core/gpu.h"

namespace EdenXbox {
namespace {
// The system suspends the app when another app takes the foreground; pause emulation before the
// process is frozen and resume it afterwards.
struct Lifecycle {
    enum Request : int { None, Suspend, Resume };
    std::atomic<int> request{None};
    std::mutex mutex;
    std::optional<winrt::Windows::ApplicationModel::SuspendingDeferral> deferral;

    void CompleteDeferral() {
        std::scoped_lock lock{mutex};
        if (deferral) {
            deferral->Complete();
            deferral.reset();
        }
    }
};

void RunGame(MesaWindow& window, const std::string& path, const std::atomic<bool>& closed,
             const std::shared_ptr<XboxGamepad>& gamepad, Lifecycle& lifecycle) {
    Diagnostic("GAME_BEGIN");
    Common::Log::Initialize();
    Settings::values.renderer_backend = Settings::RendererBackend::OpenGL_GLSL;
    Settings::values.sink_id = Settings::AudioEngine::Null;
    Settings::values.cpuopt_fastmem = false;
    Settings::values.cpuopt_fastmem_exclusives = false;
    Settings::values.use_asynchronous_shaders = false;
    XboxGamepad::Configure(gamepad);
    RegisterUnsupportedEngines();
    SCOPE_EXIT {
        Common::Input::UnregisterInputFactory("nxbox");
        Common::Input::UnregisterOutputFactory("nxbox");
        UnregisterUnsupportedEngines();
    };
    std::atomic<bool> guest_exited{false};
    Core::System system{};
    system.Initialize();
    system.ApplySettings();
    system.RegisterExitCallback([&] { guest_exited.store(true, std::memory_order_release); });
    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
    system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
    system.GetUserChannel().clear();
    Diagnostic("GAME_LOADING");
    Service::AM::FrontendAppletParameters parameters{};
    const auto result = system.Load(window, path, parameters);
    if (result != Core::SystemResultStatus::Success) {
        Diagnostic("GAME_LOAD_FAILED status=" + std::to_string(static_cast<int>(result)));
        return;
    }
    SCOPE_EXIT {
        void(system.Pause());
        system.ShutdownMainProcess();
        Diagnostic("GAME_STOPPED");
    };
    Kernel::Svc::SetDebugStringObserver([](std::string_view message) {
        if (message.starts_with("NXBOX_PADDLE_"))
            Diagnostic(std::string(message));
    });
    SCOPE_EXIT {
        Kernel::Svc::SetDebugStringObserver(nullptr);
    };
    system.GPU().Start();
    system.GetCpuManager().OnGpuReady();
    void(system.Run());
    Diagnostic("GAME_RUNNING");
    auto* controller = system.HIDCore().GetEmulatedControllerByIndex(0);
    auto measured_at = std::chrono::steady_clock::now();
    auto measured_frames = window.FrameCount();
    bool paused = false;
    SCOPE_EXIT {
        lifecycle.CompleteDeferral();
    };
    while (!closed.load(std::memory_order_acquire) &&
           !guest_exited.load(std::memory_order_acquire)) {
        const int request = lifecycle.request.exchange(Lifecycle::None, std::memory_order_acq_rel);
        if (request == Lifecycle::Suspend) {
            if (!paused) {
                void(system.Pause());
                paused = true;
                Diagnostic("GAME_SUSPENDED");
            }
            lifecycle.CompleteDeferral();
        } else if (request == Lifecycle::Resume && paused) {
            void(system.Run());
            paused = false;
            measured_at = std::chrono::steady_clock::now();
            measured_frames = window.FrameCount();
            Diagnostic("GAME_RESUMED");
        }
        if (paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            continue;
        }
        gamepad->Poll(*controller);
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration<double>(now - measured_at).count();
        if (elapsed >= 5.0) {
            const auto frames = window.FrameCount();
            Diagnostic("GAME_PRESENT frames=" + std::to_string(frames - measured_frames) +
                       " seconds=" + std::to_string(elapsed) +
                       " fps=" + std::to_string((frames - measured_frames) / elapsed) + " memory=" +
                       std::to_string(winrt::Windows::System::MemoryManager::AppMemoryUsage()));
            measured_at = now;
            measured_frames = frames;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
}
} // namespace

void RunGameView(const winrt::Windows::UI::Core::CoreWindow& window, const std::string& path) {
    using namespace winrt::Windows::UI::Core;
    std::atomic<bool> closed{false};
    std::atomic<bool> done{false};
    const auto close_token = window.Closed([&](const auto&, const auto&) { closed.store(true); });
    auto gamepad = std::make_shared<XboxGamepad>();
    // Handled so the system does not treat B or Menu as navigation while a game runs.
    const auto key_down_token = window.KeyDown([gamepad](const auto&, const KeyEventArgs& args) {
        if (gamepad->OnKey(args.VirtualKey(), true))
            args.Handled(true);
    });
    const auto key_up_token = window.KeyUp([gamepad](const auto&, const KeyEventArgs& args) {
        if (gamepad->OnKey(args.VirtualKey(), false))
            args.Handled(true);
    });
    Lifecycle lifecycle;
    using winrt::Windows::ApplicationModel::Core::CoreApplication;
    const auto suspending_token =
        CoreApplication::Suspending([&lifecycle](const auto&, const auto& args) {
            {
                std::scoped_lock lock{lifecycle.mutex};
                lifecycle.deferral = args.SuspendingOperation().GetDeferral();
            }
            lifecycle.request.store(Lifecycle::Suspend, std::memory_order_release);
        });
    const auto resuming_token = CoreApplication::Resuming([&lifecycle](const auto&, const auto&) {
        lifecycle.request.store(Lifecycle::Resume, std::memory_order_release);
    });
    SCOPE_EXIT {
        window.Closed(close_token);
        window.KeyDown(key_down_token);
        window.KeyUp(key_up_token);
        CoreApplication::Suspending(suspending_token);
        CoreApplication::Resuming(resuming_token);
    };
    // The driver targets the current HDMI surface. Layout remains 16:9 until resize handling lands.
    auto graphics = std::make_shared<MesaWindow>(window, 1920, 1080);
    std::thread worker([&, graphics = std::move(graphics)]() mutable {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        SCOPE_EXIT {
            // Keep the UI dispatcher alive while the graphics driver releases its resources.
            graphics.reset();
            winrt::uninit_apartment();
            done.store(true, std::memory_order_release);
        };
        try {
            RunGame(*graphics, path, closed, gamepad, lifecycle);
        } catch (const winrt::hresult_error& error) {
            Diagnostic("GAME_FAIL " + winrt::to_string(error.message()));
        } catch (const std::exception& error) {
            Diagnostic(std::string("GAME_FAIL ") + error.what());
        }
    });
    while (!done.load(std::memory_order_acquire)) {
        window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        Sleep(1);
    }
    worker.join();
}
} // namespace EdenXbox
