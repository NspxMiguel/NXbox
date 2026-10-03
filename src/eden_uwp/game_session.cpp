// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/game_session.h"
#include "eden_uwp/shader_share.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <vector>
#include <functional>
#include <filesystem>
#include <fstream>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
#include <mutex>
#include <optional>
#include <thread>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.h>

#include <fmt/format.h>
#include "common/fs/fs.h"
#include "common/nxbox_stall.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/cpu_manager.h"
#include "core/file_sys/nsz.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/svc/svc_debug_string.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/perf_stats.h"
#include "eden_uwp/diagnostic.h"
#include "common/fs/file.h"
#include "eden_uwp/await_bounded.h"
#include "eden_uwp/game_download.h"
#include "eden_uwp/http_vfs_file.h"
#include "eden_uwp/gamepad.h"
#include "eden_uwp/mesa_window.h"
#include "eden_uwp/save_sync.h"
#include "eden_uwp/setup_ui.h"
#include "eden_uwp/ui/library.h"
#include "eden_uwp/ui/library_screen.h"
#include "eden_uwp/ui/mods.h"
#include "eden_uwp/ui/savesync_ui.h"
#include "eden_uwp/usb_library.h"
#include "video_core/gpu.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"

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

// LocalState\game.txt names the file to boot, relative to LocalState (for example
// "games\\p5r.nsp"), or an absolute removable-drive path. If LocalState\game.url also exists, that URL is downloaded to that file first
// (resuming a partial download), unless the game was just chosen from the library: game.url then
// belongs to whatever game.txt named before, and downloading it over the chosen file would replace
// the player's game. Without game.txt the bundled homebrew boots.
// Converts a remote .nsz straight into an .nsp, reading the source over HTTP: the console's
// storage cannot hold a large game's .nsz and its .nsp at once (Breath of the Wild: 9.6 + 13.5
// GB). The .nsp goes where it fits, LocalState first, then a removable drive's NXbox\games.
// On success `target` names the .nsp; an .nsp already converted in either place is reused.
bool StreamNszToNsp(const std::string& url, std::filesystem::path& target,
                    const std::function<void(const std::filesystem::path&)>& remember_target) {
    namespace fs = std::filesystem;
    fs::path name = target.filename();
    name.replace_extension(L".nsp");
    std::vector<fs::path> places{target.parent_path()};
    for (const fs::path& folder : ExternalGameFolders()) {
        places.push_back(folder);
    }
    for (const fs::path& place : places) {
        if (Common::FS::IsFile(place / name) && Common::FS::GetSize(place / name) > 0) {
            target = place / name;
            remember_target(target);
            Diagnostic("NSZ_STREAM_COMPLETE already converted " + target.string());
            return true;
        }
    }
    const FileSys::VirtualFile source = OpenHttpFile(url);
    if (!source) {
        return false;
    }
    // An .nsp runs about 1.4 times its .nsz for the games measured; 1.5 leaves a margin. A
    // conversion that still runs out of space fails cleanly and removes its partial file.
    const std::uint64_t needed = static_cast<std::uint64_t>(source->GetSize()) * 3 / 2;
    // A partial .nsz from an earlier plain download is given up for the .nsp, so its space
    // counts as free (it held 3.9 GB of the 16.6 GB the removable drive had for BotW).
    std::optional<fs::path> folder;
    for (std::size_t i = 0; i < places.size() && !folder; ++i) {
        const auto free = FreeSpace(places[i]);
        const fs::path stale = places[i] / target.filename();
        const std::uint64_t stale_size =
            Common::FS::IsFile(stale) ? Common::FS::GetSize(stale) : 0;
        if (free && *free + stale_size >= needed) {
            folder = places[i];
            if (stale_size != 0 && Common::FS::RemoveFile(stale)) {
                Diagnostic(fmt::format("NSZ_STREAM removed the partial .nsz ({} MiB)",
                                       stale_size >> 20));
            }
        }
    }
    if (!folder) {
        Diagnostic(fmt::format("NSZ_STREAM_NO_SPACE need={} MiB", needed >> 20));
        return false;
    }
    const fs::path out = *folder / name;
    fs::path partial = out;
    partial += L".partial";
    remember_target(out);
    Diagnostic("NSZ_STREAM_TARGET " + out.string());
    FileSys::RealVfsFilesystem vfs;
    FileSys::VirtualFile output =
        vfs.CreateFile(Common::FS::PathToUTF8String(partial), FileSys::OpenMode::ReadWrite);
    if (!output) {
        Diagnostic("NSZ_STREAM_FAILED cannot create " + partial.string());
        return false;
    }
    auto sample_at = std::chrono::steady_clock::now();
    u64 sample_bytes = 0;
    const auto progress = [&](u64 done, u64 total) {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - sample_at).count();
        if (elapsed >= 5.0) {
            Diagnostic(fmt::format("NSZ_STREAM {}/{} MiB {:.1f} MiB/s", done >> 20, total >> 20,
                                   static_cast<double>(done - sample_bytes) / (1 << 20) / elapsed));
            sample_at = now;
            sample_bytes = done;
        }
    };
    std::string error;
    const bool converted = FileSys::ConvertNszToNsp(source, output, progress, &error);
    output.reset();
    if (!converted || !Common::FS::RenameFile(partial, out)) {
        void(Common::FS::RemoveFile(partial));
        Diagnostic("NSZ_STREAM_FAILED " + (error.empty() ? std::string("rename") : error));
        return false;
    }
    target = out;
    Diagnostic(fmt::format("NSZ_STREAM_COMPLETE {} MiB", Common::FS::GetSize(out) >> 20));
    return true;
}

std::string ResolveGamePath(const std::string& bundled, bool chosen_in_library) {
    namespace fs = std::filesystem;
    const fs::path local(
        winrt::to_string(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()));
    const auto read_line = [](const fs::path& file) {
        std::string line;
        std::ifstream in(file);
        std::getline(in, line);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        return line;
    };
    if (!fs::exists(local / "game.txt")) {
        return bundled;
    }
    const std::string selected = read_line(local / "game.txt");
    if (selected.empty() || selected == "none") {
        return bundled;
    }
    const fs::path selected_path{Common::FS::ToU8String(selected)};
    fs::path target = selected_path.is_absolute() ? selected_path : local / selected_path;
    const auto remember_target = [&local](const fs::path& path) {
        std::ofstream out(local / "game.txt", std::ios::trunc);
        out << Common::FS::PathToUTF8String(path) << '\n';
        out.flush();
        if (!out) {
            throw std::runtime_error("cannot persist the download target");
        }
    };
    if (chosen_in_library && fs::exists(local / "game.url")) {
        Diagnostic("GAME_URL ignored, the game was chosen in the library");
    }
    const std::string url = !chosen_in_library && fs::exists(local / "game.url")
                                ? read_line(local / "game.url")
                                : std::string{};
    const auto is_nsz = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text.size() > 4 && text.ends_with(".nsz");
    };
    if (!url.empty() && is_nsz(url) && is_nsz(target.string())) {
        if (StreamNszToNsp(url, target, remember_target)) {
            Diagnostic("GAME_TARGET " + target.string());
            return Common::FS::PathToUTF8String(target);
        }
        Diagnostic("NSZ_STREAM_UNAVAILABLE falling back to downloading the .nsz");
    }
    if (!url.empty() && !DownloadFile(url, target, remember_target)) {
        // The download source can go away (the LAN host was reinstalled); a copy that is
        // already on the console is still usable.
        if (!Common::FS::IsFile(target) || Common::FS::GetSize(target) == 0) {
            return bundled;
        }
        Diagnostic("GAME_DOWNLOAD_UNAVAILABLE using the local copy");
    }
    if (!Common::FS::IsFile(target)) {
        Diagnostic("GAME_MISSING " + target.string());
        return bundled;
    }
    Diagnostic("GAME_TARGET " + target.string());
    return Common::FS::PathToUTF8String(target);
}

// Writes the game the player chose in the library into LocalState\game.txt, in the format
// ResolveGamePath reads: a path relative to LocalState. A path outside LocalState is written as it
// is, which ResolveGamePath resolves just as well.
void RememberChosenGame(const std::filesystem::path& local, const std::string& chosen) {
    namespace fs = std::filesystem;
    const fs::path game(std::wstring_view(winrt::to_hstring(chosen)));
    fs::path selected = game.lexically_relative(local);
    if (selected.empty() || selected.begin()->native() == L"..") {
        selected = game;
    }
    std::ofstream(local / "game.txt", std::ios::trunc) << Common::FS::PathToUTF8String(selected) << '\n';
    Diagnostic("GAME_CHOSEN " + selected.string());
}

// LocalState\eden_settings.txt: "label=value" lines applied over the defaults, by the same labels
// as Eden's qt-config (e.g. accelerate_astc=0), so settings can be tried on the console without a
// rebuild. The save sync needs the active profile (current_user) before the game boots, so this
// runs once ahead of the boot when a sync is due and again in RunGame; applying it twice is
// harmless.
void ApplyEdenSettingsFile() {
    const std::filesystem::path settings_file =
        std::filesystem::path(winrt::to_string(
            winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path())) /
        "eden_settings.txt";
    std::ifstream settings_in(settings_file);
    std::string line;
    while (std::getline(settings_in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        const auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) {
            continue;
        }
        const std::string label = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        bool applied = false;
        for (auto& [category, settings] : Settings::values.linkage.by_category) {
            for (Settings::BasicSetting* setting : settings) {
                if (setting->GetLabel() == label) {
                    setting->LoadString(value);
                    applied = true;
                }
            }
        }
        Diagnostic(fmt::format("SETTING {}={} {}", label, value, applied ? "applied" : "unknown"));
    }
}

// NXBOX_READ_CHECK=1: reads 64 random 1 MiB chunks of the game file through IOFile (the
// CreateFileFromAppW path used on removable drives) and through a WinRT StorageFile stream, and
// compares them. A mismatch would mean the emulator was fed wrong bytes from the drive, which
// would explain guest code writing to unmapped memory.
void CheckGameReads(const std::string& path) {
    using namespace winrt::Windows::Storage;
    using namespace winrt::Windows::Storage::Streams;
    Common::FS::IOFile file(path, Common::FS::FileAccessMode::Read, Common::FS::FileType::BinaryFile);
    if (!file.IsOpen()) {
        Diagnostic("READ_CHECK cannot open " + path);
        return;
    }
    const u64 size = file.GetSize();
    const auto storage = AwaitBounded(
        StorageFile::GetFileFromPathAsync(winrt::to_hstring(path)), std::chrono::seconds(10));
    if (!storage) {
        Diagnostic("READ_CHECK StorageFile unavailable");
        return;
    }
    const auto stream = AwaitBounded(storage->OpenReadAsync(), std::chrono::seconds(10));
    if (!stream) {
        Diagnostic("READ_CHECK stream unavailable");
        return;
    }
    constexpr u32 Chunk = 1u << 20;
    std::vector<u8> a(Chunk);
    u64 seed = 0x9E3779B97F4A7C15ull;
    int mismatches = 0;
    int compared = 0;
    for (int i = 0; i < 64 && size > Chunk; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        // Half the samples beyond 4 GiB when the file is that large.
        const u64 base = (i % 2 == 1 && size > (5ull << 30)) ? (4ull << 30) : 0;
        const u64 offset = (base + seed % (size - base - Chunk)) & ~u64{0xFFF};
        if (!file.Seek(static_cast<s64>(offset)) || file.ReadSpan(std::span<u8>(a)) != Chunk) {
            Diagnostic(fmt::format("READ_CHECK ioread failed at {:#x}", offset));
            ++mismatches;
            continue;
        }
        stream->Seek(offset);
        Buffer buffer{Chunk};
        const auto read =
            AwaitBounded(stream->ReadAsync(buffer, Chunk, InputStreamOptions::None),
                         std::chrono::seconds(30));
        if (!read || read->Length() != Chunk) {
            Diagnostic(fmt::format("READ_CHECK winrt read failed at {:#x}", offset));
            continue;
        }
        ++compared;
        if (std::memcmp(a.data(), read->data(), Chunk) != 0) {
            ++mismatches;
            Diagnostic(fmt::format("READ_CHECK mismatch at {:#x}", offset));
        }
    }
    Diagnostic(fmt::format("READ_CHECK {} size={} compared={} mismatches={}", path, size, compared,
                           mismatches));
}

void RunGame(MesaWindow& window, const std::string& bundled_path, const std::atomic<bool>& closed,
             const std::shared_ptr<XboxGamepad>& gamepad, Lifecycle& lifecycle,
             bool chosen_in_library) {
    Diagnostic("GAME_BEGIN");
    const auto memory_stage = [](const char* stage) {
        Diagnostic(fmt::format("MEM {} commit={} MiB limit={} MiB", stage,
                               winrt::Windows::System::MemoryManager::AppMemoryUsage() >> 20,
                               winrt::Windows::System::MemoryManager::AppMemoryUsageLimit() >> 20));
    };
    memory_stage("begin");
    const std::string path = ResolveGamePath(bundled_path, chosen_in_library);
    if (const char* check = std::getenv("NXBOX_READ_CHECK"); check != nullptr && check[0] == '1') {
        CheckGameReads(path);
    }
    {
        // LocalState\\log_filter.txt (for example "*:Debug") raises the Eden log verbosity for a
        // run.
        std::ifstream in(
            std::filesystem::path(winrt::to_string(
                winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path())) /
            "log_filter.txt");
        std::string filter;
        std::string flush;
        if (std::getline(in, filter)) {
            while (!filter.empty() && (filter.back() == '\r' || filter.back() == ' ')) {
                filter.pop_back();
            }
            if (!filter.empty()) {
                Settings::values.log_filter.SetValue(filter);
            }
            // A second line "flush" writes every log line through immediately, so a hung guest
            // still leaves its last activity in the file.
            if (std::getline(in, flush) && flush.rfind("flush", 0) == 0) {
                Settings::values.log_flush_line.SetValue(true);
            }
            // A third line "gldebug" turns on the OpenGL debug callback.
            std::string gl_debug;
            if (std::getline(in, gl_debug) && gl_debug.rfind("gldebug", 0) == 0) {
                Settings::values.renderer_debug.SetValue(true);
            }
        }
    }
    Common::Log::Initialize();
    Settings::values.renderer_backend = Settings::RendererBackend::OpenGL_GLSL;
    // XAudio2 is the audio path available to UWP; NXBOX_AUDIO=null (LocalState\nxbox_env.txt)
    // falls back to silence.
    {
        const char* audio = std::getenv("NXBOX_AUDIO");
        Settings::values.sink_id = audio != nullptr && std::string_view{audio} == "null"
                                       ? Settings::AudioEngine::Null
                                       : Settings::AudioEngine::XAudio2;
    }
    Settings::values.cpuopt_fastmem = false;
    Settings::values.cpuopt_fastmem_exclusives = false;
    Settings::values.use_asynchronous_shaders = false;
    // Mesa d3d12: the GPU ASTC decoder leaves garbage in the lower mip levels (sparkling dots and
    // moire on every distant texture in P5R), and GPU video decoding (d3d11va) freezes the intro
    // movie on a frame. Decode both on the CPU, which renders cleanly on the Xbox.
    Settings::values.accelerate_astc.SetValue(Settings::AstcDecodeMode::Cpu);
    Settings::values.nvdec_emulation.SetValue(Settings::NvdecEmulation::Cpu);
    ApplyEdenSettingsFile();
    // Mods the player turned off in the mod store go into Eden's disabled add-ons list.
    Ui::ApplyDisabledMods(std::filesystem::path(winrt::to_string(
        winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path())));
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
    memory_stage("initialized");
    system.ApplySettings();
    system.RegisterExitCallback([&] { guest_exited.store(true, std::memory_order_release); });
    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
    // Updates and DLC are read in place from the game folders (Eden's external content
    // directories) instead of being installed into the NAND: the console's internal storage has
    // no room for second copies, and without this Breath of the Wild ran as 1.0 with its 1.6
    // update and DLC sitting next to it.
    {
        auto& dirs = Settings::values.external_content_dirs;
        dirs.clear();
        const auto local_state = std::filesystem::path(winrt::to_string(
            winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()));
        dirs.push_back(Common::FS::PathToUTF8String(local_state / "games"));
        for (const auto& folder : ExternalGameFolders()) {
            dirs.push_back(Common::FS::PathToUTF8String(folder));
        }
        for (const auto& dir : dirs) {
            Diagnostic("CONTENT_DIR " + dir);
        }
        // NXBOX_DISABLE_DLC=<title id in hex> (LocalState\nxbox_env.txt) runs a game without its
        // DLC, to tell a DLC problem from an update problem.
        if (const char* no_dlc = std::getenv("NXBOX_DISABLE_DLC"); no_dlc != nullptr) {
            const u64 title = std::strtoull(no_dlc, nullptr, 16);
            Settings::values.disabled_addons[title].push_back("DLC");
            Diagnostic(fmt::format("CONTENT_DLC_DISABLED {:016X}", title));
        }
    }
    system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
    system.GetUserChannel().clear();
    Diagnostic("GAME_LOADING");
    // Same parameters the desktop frontend uses to boot an application. With a zeroed applet id the
    // applet manager treats the game as a library applet, sends it ChangeIntoForeground instead of
    // FocusStateChanged, and the guest waits for that message forever.
    Service::AM::FrontendAppletParameters parameters{
        .applet_id = Service::AM::AppletId::Application,
        .applet_type = Service::AM::AppletType::Application,
    };
    memory_stage("before_load");
    const auto result = system.Load(window, path, parameters);
    memory_stage("after_load");
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
    // Load (and precompile) the per-title shader cache on the renderer's own context, before the
    // GPU thread takes it: a second GL context cannot be made current on the Xbox's Mesa.
    // Without this every session compiled each shader the first time it appeared, stalling for
    // seconds at scene changes. NXBOX_SHADER_CACHE=0 in LocalState\nxbox_env.txt disables it.
    const char* shader_cache = std::getenv("NXBOX_SHADER_CACHE");
    if (Settings::values.use_disk_shader_cache.GetValue() &&
        !(shader_cache != nullptr && std::string_view{shader_cache} == "0")) {
        const auto title_id = system.GetApplicationProcessProgramID();
        // The shared copy may be what crashed, so a session after a crash skips it too.
        if (BeginShaderCacheLoad(title_id)) {
            Diagnostic("SHADER_CACHE previous load crashed; cache set aside");
        } else {
            DownloadSharedShaderCache(title_id);
        }
        Diagnostic("SHADER_CACHE loading");
        {
            auto& render_context = system.Renderer().Context();
            render_context.MakeCurrent();
            system.Renderer().ReadRasterizer()->LoadDiskResources(
                system.GetApplicationProcessProgramID(), std::stop_token{},
                [](VideoCore::LoadCallbackStage stage, size_t value, size_t total) {
                    // Progress goes to the flushed diagnostic file, so a load that hangs or
                    // dies shows how far it got.
                    // Inline builds report a running count with no total; the final call
                    // reports the total with a count of zero.
                    if (stage == VideoCore::LoadCallbackStage::Build && value % 32 == 0) {
                        // The commit shows whether a hang while loading is memory pressure.
                        const auto commit =
                            winrt::Windows::System::MemoryManager::AppMemoryUsage() >> 20;
                        Diagnostic(total == 0
                                       ? fmt::format("SHADER_CACHE built {} commit={} MiB", value,
                                                     commit)
                                       : fmt::format("SHADER_CACHE total {}", total));
                    }
                });
            render_context.DoneCurrent();
        }
        EndShaderCacheLoad(title_id);
        Diagnostic("SHADER_CACHE ready");
        memory_stage("shader_cache");
    }
    system.GPU().Start();
    system.GetCpuManager().OnGpuReady();
    void(system.Run());
    Diagnostic("GAME_RUNNING");
    memory_stage("loaded");
    // The guest's own HID resource manager applies Settings::values.players lazily, on its first
    // HID service call. Poll() runs immediately on the host thread regardless of guest timing, so
    // without this the controller stays at its construction default (NpadStyleIndex::None,
    // "Controller type 0 is not supported") until the guest happens to touch HID first.
    system.HIDCore().ReloadInputDevices();
    auto* controller = system.HIDCore().GetEmulatedControllerByIndex(0);
    auto measured_at = std::chrono::steady_clock::now();
    auto measured_frames = window.FrameCount();
    auto last_frame_count = measured_frames;
    auto last_frame_at = std::chrono::steady_clock::now();
    double worst_gap_ms = 0.0;
    int hitches = 0;
    bool paused = false;
    int stall_dumps = 0;
    const auto started_at = std::chrono::steady_clock::now();
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
                // Leaving through Home suspends the app: share the shaders met this session.
                UploadSharedShaderCache(system.GetApplicationProcessProgramID());
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
        // Stutter: the longest gap between two presented frames in each window.
        if (const auto count = window.FrameCount(); count != last_frame_count) {
            const double gap =
                std::chrono::duration<double, std::milli>(now - last_frame_at).count();
            if (last_frame_count != 0) {
                worst_gap_ms = std::max(worst_gap_ms, gap);
                hitches += gap > 100.0 ? 1 : 0;
            }
            last_frame_count = count;
            last_frame_at = now;
        }
        const auto elapsed = std::chrono::duration<double>(now - measured_at).count();
        if (elapsed >= 5.0) {
            const auto frames = window.FrameCount();
            const auto stats = system.GetAndResetPerfStats();
            Diagnostic(fmt::format("GAME_PRESENT frames={} seconds={:.3f} fps={:.2f} "
                                   "game_fps={:.2f} system_fps={:.2f} "
                                   "frametime_ms={:.2f} speed={:.1f}% memory={} "
                                   "worst_gap_ms={:.0f} hitches={}",
                                   frames - measured_frames, elapsed,
                                   (frames - measured_frames) / elapsed, stats.average_game_fps,
                                   stats.system_fps, stats.frametime * 1000.0,
                                   stats.emulation_speed * 100.0,
                                   winrt::Windows::System::MemoryManager::AppMemoryUsage(),
                                   worst_gap_ms, hitches));
            // The patched Mesa publishes its reports in environment variables (pipeline states,
            // root signatures, batch failures, device removal); log each one when it changes.
            {
                static std::array<std::string, 7> last;
                static constexpr std::array<const char*, 7> names{
                    "NXBOX_D3D12_PSO",           "NXBOX_D3D12_PSO_FAIL",
                    "NXBOX_D3D12_ROOTSIG",       "NXBOX_D3D12_REMOVED",
                    "NXBOX_D3D12_BATCH",         "NXBOX_D3D12_RESET",
                    "NXBOX_D3D12_FIRST_FAILURE"};
                for (std::size_t i = 0; i < names.size(); ++i) {
                    char value[512] = "";
                    if (GetEnvironmentVariableA(names[i], value, sizeof(value)) != 0 &&
                        last[i] != value) {
                        last[i] = value;
                        Diagnostic(std::string(names[i] + 6) + " " + value); // drop "NXBOX_"
                    }
                }
            }
#if NXBOX_STALL_PROFILE
            // Host work in the window, as total/longest milliseconds and call count, so each
            // hitch can be traced to shader builds, texture uploads, decoding or cache eviction.
            std::string stall_line = "GAME_STALL";
            constexpr std::array<std::pair<NxboxStall::Kind, const char*>, 21> stall_kinds{{
                {NxboxStall::Kind::Shader, "shader"},
                {NxboxStall::Kind::Upload, "upload"},
                {NxboxStall::Kind::Convert, "convert"},
                {NxboxStall::Kind::GarbageCollect, "gc"},
                {NxboxStall::Kind::Video, "video"},
                {NxboxStall::Kind::Jit, "jit"},
                {NxboxStall::Kind::JitFlush, "jitflush"},
                {NxboxStall::Kind::JitProtect, "jitprotect"},
                {NxboxStall::Kind::JitTranslate, "jittranslate"},
                {NxboxStall::Kind::JitOptimize, "jitoptimize"},
                {NxboxStall::Kind::JitEmit, "jitemit"},
                {NxboxStall::Kind::JitInvalidate, "jitinvalidate"},
                {NxboxStall::Kind::Io, "io"},
                {NxboxStall::Kind::Aes, "aes"},
                {NxboxStall::Kind::GpuBusy, "gpu"},
                {NxboxStall::Kind::GlSync, "glsync"},
                {NxboxStall::Kind::GlFinish, "glfinish"},
                {NxboxStall::Kind::Readback, "readback"},
                {NxboxStall::Kind::Query, "query"},
                {NxboxStall::Kind::Decommit, "decommit"},
                {NxboxStall::Kind::Present, "present"},
            }};
            for (const auto& [kind, name] : stall_kinds) {
                const auto taken = NxboxStall::Take(kind);
                stall_line += fmt::format(" {}={:.0f}/{:.0f}/{}", name, taken.total_us / 1000.0,
                                          taken.max_us / 1000.0, taken.calls);
            }
            // Brackets distinguish raw counts/bytes from duration triples and let existing
            // duration-only stall-report.py readers ignore these fields.
            constexpr std::array<std::pair<NxboxStall::JitEvent, const char*>, 17> jit_events{{
                {NxboxStall::JitEvent::RangeCalls, "jit_inv_calls"},
                {NxboxStall::JitEvent::RangeBytes, "jit_inv_bytes"},
                {NxboxStall::JitEvent::ClearRequests, "jit_clear_req"},
                {NxboxStall::JitEvent::CacheClears, "jit_clears"},
                {NxboxStall::JitEvent::ClearBlocks, "jit_clear_blocks"},
                {NxboxStall::JitEvent::RangeBlocks, "jit_range_blocks"},
                {NxboxStall::JitEvent::EmptyInvalidations, "jit_inv_empty"},
                {NxboxStall::JitEvent::NewPc, "jit_pc_new"},
                {NxboxStall::JitEvent::RepeatPc, "jit_pc_repeat"},
                {NxboxStall::JitEvent::UnknownPc, "jit_pc_unknown"},
                {NxboxStall::JitEvent::PcCompileMax, "jit_pc_max"},
                {NxboxStall::JitEvent::NewKey, "jit_key_new"},
                {NxboxStall::JitEvent::RepeatKey, "jit_key_repeat"},
                {NxboxStall::JitEvent::UnknownKey, "jit_key_unknown"},
                {NxboxStall::JitEvent::InstructionInvalidations, "jit_ic"},
                {NxboxStall::JitEvent::PageTableInvalidations, "jit_pt"},
                {NxboxStall::JitEvent::ProtectBytes, "jit_protect_bytes"},
            }};
            for (const auto& [event, name] : jit_events) {
                stall_line += fmt::format(" {}=[{}]", name, NxboxStall::TakeJit(event));
            }
            for (std::size_t core = 0; core < NxboxStall::JitCaches().size(); ++core) {
                const auto& cache = NxboxStall::JitCaches()[core];
                stall_line += fmt::format(" jit_cache{}=[{}/{}]", core,
                                          cache.used.load(std::memory_order_relaxed),
                                          cache.capacity.load(std::memory_order_relaxed));
            }
            Diagnostic(stall_line);
#endif
            worst_gap_ms = 0.0;
            hitches = 0;
            measured_at = now;
            measured_frames = frames;
            // While no frame has appeared, record what every guest thread is doing (state, wait
            // reason and last saved PC/LR) so a stalled boot can be diagnosed.
            if (frames == 0 && stall_dumps < 3 &&
                std::chrono::duration<double>(now - started_at).count() > 20.0) {
                ++stall_dumps;
                if (auto* process = system.ApplicationProcess()) {
                    for (auto& thread : process->GetThreadList()) {
                        const auto& ctx = thread.GetContext();
                        Diagnostic(fmt::format(
                            "GUEST_THREAD id={} core={} prio={} state={} wait={} pc={:#x} lr={:#x} "
                            "sp={:#x}",
                            thread.GetThreadId(), thread.GetActiveCore(), thread.GetPriority(),
                            static_cast<unsigned>(thread.GetState()),
                            static_cast<unsigned>(thread.GetWaitReasonForDebugging()), ctx.pc,
                            ctx.lr, ctx.sp));
                    }
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
}

// Title ID from an nxbox://play?title=<ID> activation; empty when the app was launched normally.
std::mutex g_protocol_mutex;
std::string g_protocol_title;
bool g_protocol_set = false;

std::string TakeProtocolPlayTitle() {
    const std::lock_guard lock(g_protocol_mutex);
    return g_protocol_title;
}

// Looks `title_id` up in the library (scans LocalState\games on a worker thread while this thread
// keeps pumping the window, the same way the library screen does). Returns true and fills
// `choice` with what the library screen would have returned for that game.
bool FindLibraryGame(const winrt::Windows::UI::Core::CoreWindow& window,
                     const std::filesystem::path& local_state, const std::string& title_id,
                     Ui::ChosenGame& choice) {
    using namespace winrt::Windows::UI::Core;
    Ui::LibraryScan scan(local_state);
    while (!scan.Finished()) {
        window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        Sleep(1);
    }
    for (const Ui::GameEntry& game : scan.Take()) {
        if (_stricmp(game.title_id.c_str(), title_id.c_str()) != 0) {
            continue;
        }
        choice.path = winrt::to_string(game.path.wstring());
        choice.title_id = game.title_id;
        choice.sync_name = SaveSync::GameNameFromNames(game.nacp_names);
        if (choice.sync_name.empty()) {
            choice.sync_name = winrt::to_string(winrt::hstring(game.name));
        }
        choice.display_name = game.name;
        return true;
    }
    return false;
}

} // namespace

// LocalState\move.txt: one "source|destination" pair per line, moved in order before the library
// opens. The console's internal storage and the removable drive are both small, so making room
// for one game means moving another. Each file is copied in 4 MiB chunks through IOFile (the
// CreateFileFromAppW path reaches removable drives), checked by size, and only then deleted at
// its source. Lines that fail stay in move.txt for the next launch.
void ProcessMoves(const std::filesystem::path& local_state) {
    namespace fs = std::filesystem;
    const fs::path list = local_state / "move.txt";
    std::ifstream in(list);
    if (!in) {
        return;
    }
    std::vector<std::string> remaining;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto bar = line.find('|');
        if (bar == std::string::npos) {
            continue;
        }
        const std::string from = line.substr(0, bar);
        const std::string to = line.substr(bar + 1);
        const u64 size = Common::FS::GetSize(from);
        Common::FS::IOFile src(from, Common::FS::FileAccessMode::Read,
                               Common::FS::FileType::BinaryFile);
        const std::string partial = to + ".partial";
        bool ok = src.IsOpen() && size != 0 &&
                  Common::FS::CreateDirs(fs::path{Common::FS::ToU8String(to)}.parent_path());
        if (ok) {
            Common::FS::IOFile dst(partial, Common::FS::FileAccessMode::Write,
                                   Common::FS::FileType::BinaryFile);
            std::vector<u8> chunk(4u << 20);
            u64 done = 0;
            auto sample = std::chrono::steady_clock::now();
            ok = dst.IsOpen();
            while (ok && done < size) {
                const std::size_t want =
                    static_cast<std::size_t>(std::min<u64>(chunk.size(), size - done));
                ok = src.ReadSpan(std::span<u8>(chunk.data(), want)) == want &&
                     dst.WriteSpan(std::span<const u8>(chunk.data(), want)) == want;
                done += want;
                if (std::chrono::steady_clock::now() - sample > std::chrono::seconds(10)) {
                    sample = std::chrono::steady_clock::now();
                    Diagnostic(fmt::format("MOVE {} {}/{} MiB", to, done >> 20, size >> 20));
                }
            }
            ok = ok && dst.Commit();
        }
        src.Close();
        ok = ok && Common::FS::GetSize(partial) == size && Common::FS::RenameFile(partial, to) &&
             Common::FS::RemoveFile(from);
        if (!ok) {
            void(Common::FS::RemoveFile(partial));
            remaining.push_back(line);
        }
        Diagnostic(fmt::format("MOVE {} {} -> {} ({} MiB)", ok ? "ok" : "failed", from, to,
                               size >> 20));
    }
    in.close();
    if (remaining.empty()) {
        void(Common::FS::RemoveFile(list));
    } else {
        std::ofstream out(list, std::ios::trunc);
        for (const auto& pending : remaining) {
            out << pending << '\n';
        }
    }
}

void SetProtocolPlayTitle(std::string title_id) {
    const std::lock_guard lock(g_protocol_mutex);
    if (!g_protocol_set) {
        g_protocol_set = true;
        g_protocol_title = std::move(title_id);
    }
}

void RunGameView(const winrt::Windows::UI::Core::CoreWindow& window, const std::string& path) {
    using namespace winrt::Windows::UI::Core;
    // LocalState\usb_scan_test.txt: a one-off trigger to test the setup screen's render pipeline
    // (Direct2D/DirectWrite on this same CoreWindow, before Mesa/OpenGL takes it over) together
    // with the USB scanner, without wiring either into the real boot flow yet.
    if (std::filesystem::exists(
            std::filesystem::path(winrt::to_string(
                winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path())) /
            "usb_scan_test.txt")) {
        auto scan_worker = std::async(std::launch::async, [] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            SCOPE_EXIT { winrt::uninit_apartment(); };
            return ScanUsbForGamesAndKeys();
        });
        while (scan_worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            Sleep(1);
        }
        const auto scan = scan_worker.get();
        ShowSetupScreen(window, "USB drive_found=" + std::string(scan.drive_found ? "yes" : "no") +
                                    " games=" + std::to_string(scan.games.size()));
    }
    // The library: the player picks a game on this same window, drawn with Direct2D/DirectWrite
    // before Mesa/OpenGL takes the window over. The screen releases every graphics object before it
    // returns. A chosen game is written to game.txt and booted by the normal path below; leaving
    // with B boots whatever game.txt names, or the bundled homebrew, exactly as before.
    // LocalState\skip_library.txt skips the screen, for unattended runs where nobody can press A.
    //
    // SwitchSaveSync (docs/nxbox-ui.md, Increment 4) hangs off the same flow, and only for a game
    // chosen in the library, because that is where its title ID and NACP names are known:
    //  - an upload still owed from the last session (the app was closed or suspended before the
    //    game's save went to the cloud) is done first, behind a progress screen;
    //  - before the chosen game boots, with sync enabled, its save is reconciled with the cloud;
    //  - after the game closed (the worker below, once Eden released the save files), the Xbox
    //    save is uploaded. The window has no renderer then, so that step has no screen.
    // A marker in LocalState (savesync_pending.txt) is written before the boot and removed once the
    // upload succeeded, so a session that never reached the upload (Home suspends the app and the
    // system may end it; the window can be closed) is finished at the next launch.
    bool chosen_in_library = false;
    bool sync_after_exit = false;
    Ui::SyncGame sync_game;
    const std::filesystem::path local_state(
        winrt::to_string(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()));
    if (std::filesystem::exists(local_state / "move.txt")) {
        // A worker does the copying; the window keeps pumping so the system sees a live app.
        auto mover = std::async(std::launch::async, [&local_state] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            SCOPE_EXIT { winrt::uninit_apartment(); };
            ProcessMoves(local_state);
        });
        while (mover.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            window.Dispatcher().ProcessEvents(
                winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);
            Sleep(16);
        }
    }
    if (!std::filesystem::exists(local_state / "skip_library.txt")) {
        if (Ui::HasPendingSync(local_state)) {
            ApplyEdenSettingsFile(); // the active profile decides which save folder is synced
            if (Ui::RunPendingSync(window, local_state)) {
                return; // the window was closed
            }
        }
        Ui::ChosenGame choice;
        std::string chosen;
        // A per-game tile ("Install as game") starts NXbox with nxbox://play?title=<ID>: boot that
        // game through the same path as a library choice, without showing the library. When no
        // game matches, fall through to the library.
        const std::string protocol_title = TakeProtocolPlayTitle();
        if (!protocol_title.empty()) {
            if (FindLibraryGame(window, local_state, protocol_title, choice)) {
                Diagnostic("PROTOCOL_LAUNCH " + protocol_title);
                chosen = choice.path;
            } else {
                Diagnostic("PROTOCOL_GAME_NOT_FOUND " + protocol_title);
            }
        }
        if (chosen.empty()) {
            chosen = Ui::RunLibrary(window, &choice);
        }
        if (!chosen.empty()) {
            RememberChosenGame(local_state, chosen);
            chosen_in_library = true;
            if (Ui::SyncEnabled()) {
                sync_game.title_id = choice.title_id;
                sync_game.name = choice.sync_name;
                sync_game.display_name = choice.display_name;
                ApplyEdenSettingsFile();
                if (Ui::RunBootSync(window, local_state, sync_game)) {
                    return; // the window was closed
                }
                sync_after_exit = true;
            }
        }
    }
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
            RunGame(*graphics, path, closed, gamepad, lifecycle, chosen_in_library);
            // The game is over and Eden released the save files: this is the safe point to upload.
            if (sync_after_exit) {
                Ui::SyncAfterExitHeadless(local_state, sync_game);
            }
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
