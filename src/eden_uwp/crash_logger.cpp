// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/crash_logger.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <string>
#include <windows.h>

#include <fileapifromapp.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.Threading.h>
#include <winrt/Windows.System.h>

#include "common/nxbox_stall.h"
#include "eden_uwp/crash_format.h"

namespace EdenXbox {
namespace {
std::atomic<HANDLE> crash_file{INVALID_HANDLE_VALUE};
DWORD main_thread_id{};
thread_local bool in_crash_handler{};
static_assert(std::atomic<HANDLE>::is_always_lock_free);

const char* ThreadRole(DWORD id) noexcept {
    if (id == NxboxStall::gpu_thread_id.load(std::memory_order_relaxed)) {
        return "gpu";
    }
    constexpr const char* names[]{"cpu0", "cpu1", "cpu2", "cpu3"};
    for (unsigned core = 0; core < 4; ++core) {
        if (id == NxboxStall::cpu_thread_ids[core].load(std::memory_order_relaxed)) {
            return names[core];
        }
    }
    return id == main_thread_id ? "main" : "worker";
}

void Prefix(Crash::Line& line, std::string_view event) noexcept {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    line.Add("utc_100ns=");
    line.Number((std::uint64_t{now.dwHighDateTime} << 32) | now.dwLowDateTime);
    line.Add(" tick_ms=");
    line.Number(GetTickCount64());
    line.Add(" pid=");
    line.Number(GetCurrentProcessId());
    line.Add(" tid=");
    const auto id = GetCurrentThreadId();
    line.Number(id);
    line.Add(" role=");
    line.Add(ThreadRole(id));
    line.Add(" event=");
    line.Add(event);
}

void Write(Crash::Line& line) noexcept {
    line.Finish();
    const auto file = crash_file.load(std::memory_order_acquire);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written;
        // GENERIC_WRITE permits FlushFileBuffers; sentinel offsets append without seek/locks.
        OVERLAPPED append{};
        append.Offset = append.OffsetHigh = MAXDWORD;
        WriteFile(file, line.data, static_cast<DWORD>(line.size), &written, &append);
        FlushFileBuffers(file);
    } else {
        OutputDebugStringA(line.data);
    }
}

void ResolveAddress(Crash::Line& line, const void* address) noexcept {
    HMODULE module{};
    char name[256]{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module)) {
        const auto length = GetModuleFileNameA(module, name, sizeof(name));
        if (!length || length >= sizeof(name)) {
            name[0] = '\0';
        }
    }
    line.Address(name, reinterpret_cast<std::uintptr_t>(address),
                 reinterpret_cast<std::uintptr_t>(module));
}

void Address(Crash::Line& line, const void* address) noexcept {
    __try {
        ResolveAddress(line, address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        line.Add("unavailable+0x");
        line.Number(reinterpret_cast<std::uintptr_t>(address), 16);
    }
}

USHORT Capture(void** frames) noexcept {
    __try {
        return RtlCaptureStackBackTrace(0, 24, frames, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void RecordCrash(const char* event, DWORD code, EXCEPTION_POINTERS* info = nullptr) noexcept {
    if (in_crash_handler) {
        return;
    }
    in_crash_handler = true;
    // Persist the reason BEFORE any loader/stack work, which can itself fault or deadlock.
    {
        Crash::Line line;
        Prefix(line, event);
        line.Add(" code=0x");
        line.Number(code, 16);
        if (info) {
            line.Add(" pc=0x");
            line.Number(reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress),
                        16);
            if (code == EXCEPTION_ACCESS_VIOLATION &&
                info->ExceptionRecord->NumberParameters >= 2) {
                line.Add(" access=");
                line.Number(info->ExceptionRecord->ExceptionInformation[0]);
                line.Add(" target=0x");
                line.Number(info->ExceptionRecord->ExceptionInformation[1], 16);
            }
        }
        Write(line);
    }
    if (info) {
        Crash::Line line;
        Prefix(line, "fault");
        line.Add(" address=");
        Address(line, info->ExceptionRecord->ExceptionAddress);
        Write(line);
    }
    // This is the handler/caller stack, not a reconstruction of the interrupted stack. The
    // exception PC above is authoritative. Use the UWP stack capture without DbgHelp symbols.
    void* frames[24]{};
    const auto count = Capture(frames);
    if (!count) {
        CrashEvent("stack_unavailable");
    }
    for (unsigned index = 0; index < count; ++index) {
        Crash::Line line;
        Prefix(line, "handler_stack");
        line.Add(" frame=");
        line.Number(index);
        line.Add(" address=");
        Address(line, frames[index]);
        Write(line);
    }
    in_crash_handler = false;
}

LONG WINAPI Unhandled(EXCEPTION_POINTERS* info) noexcept {
    RecordCrash("unhandled", info->ExceptionRecord->ExceptionCode, info);
    return EXCEPTION_CONTINUE_SEARCH;
}

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
// These APIs are excluded by Microsoft's UWP SDK partition. Never import/resolve them in APP.
LONG WINAPI Vectored(EXCEPTION_POINTERS* info) noexcept {
    if (Crash::FatalCode(info->ExceptionRecord->ExceptionCode, false)) {
        RecordCrash("first_chance", info->ExceptionRecord->ExceptionCode, info);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

[[noreturn]] void FatalRuntime(const char* event, DWORD code) noexcept {
    RecordCrash(event, code);
    RaiseFailFastException(nullptr, nullptr, 0);
    std::_Exit(3);
}
void InvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, std::uintptr_t) {
    FatalRuntime("invalid_parameter", 0xc0000417);
}
void MemoryEvent(const char* event, std::uint64_t next_limit = 0) noexcept {
    try {
        using winrt::Windows::System::MemoryManager;
        CrashEvent(event, MemoryManager::AppMemoryUsage(), MemoryManager::AppMemoryUsageLimit(),
                   next_limit);
    } catch (...) {
        CrashEvent("memory_query_failed");
    }
}
} // namespace

void InstallCrashHandlers() noexcept {
    main_thread_id = GetCurrentThreadId();
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    AddVectoredExceptionHandler(1, Vectored);
    ULONG reserve = 64 * 1024;
    SetThreadStackGuarantee(&reserve);
#endif
    SetUnhandledExceptionFilter(Unhandled);
    std::set_terminate([] { FatalRuntime("terminate", 0); });
    _set_invalid_parameter_handler(InvalidParameter);
    _set_purecall_handler([] { FatalRuntime("purecall", 0xc0000025); });
    std::signal(SIGABRT, [](int) { FatalRuntime("SIGABRT", SIGABRT); });
}

void OpenCrashLog() {
    // Called immediately after apartment initialization, before CoreApplication::Run or workers.
    const auto folder = winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path();
    const std::wstring path = std::wstring(folder) + L"\\nxbox_crash.txt";
    const auto file = CreateFile2FromAppW(path.c_str(), GENERIC_WRITE,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS, nullptr);
    crash_file.store(file, std::memory_order_release);
    CrashEvent(file == INVALID_HANDLE_VALUE ? "crash_log_open_failed" : "process_start");
    CrashEvent("uwp_no_vectored_handler_or_stack_guarantee");
    // Kept open for the process lifetime, including static destruction/terminate.
}

void CrashEvent(std::string_view event, std::uint64_t usage, std::uint64_t limit,
                std::uint64_t next_limit) noexcept {
    Crash::Line line;
    Prefix(line, event);
    line.Add(" usage=");
    line.Number(usage);
    line.Add(" limit=");
    line.Number(limit);
    line.Add(" next_limit=");
    line.Number(next_limit);
    Write(line);
}

void CrashGpuError(const char* stage, std::int32_t result, std::int32_t removed) noexcept {
    Crash::Line line;
    Prefix(line, "gpu_error");
    line.Add(" stage=");
    line.Add(stage);
    line.Add(" hr=0x");
    line.Number(static_cast<std::uint32_t>(result), 16);
    line.Add(" removed=0x");
    line.Number(static_cast<std::uint32_t>(removed), 16);
    Write(line);
}

void ObserveCrashLifecycle() {
    using winrt::Windows::ApplicationModel::Core::CoreApplication;
    using winrt::Windows::System::MemoryManager;
    // Process-lifetime subscriptions, installed before activation/library/game selection.
    CoreApplication::Suspending([](const auto&, const auto&) { MemoryEvent("Suspending"); });
    CoreApplication::Resuming([](const auto&, const auto&) { MemoryEvent("Resuming"); });
    CoreApplication::EnteredBackground(
        [](const auto&, const auto&) { MemoryEvent("EnteredBackground"); });
    CoreApplication::LeavingBackground(
        [](const auto&, const auto&) { MemoryEvent("LeavingBackground"); });
    MemoryManager::AppMemoryUsageIncreased(
        [](const auto&, const auto&) { MemoryEvent("AppMemoryUsageIncreased"); });
    MemoryManager::AppMemoryUsageLimitChanging([](const auto&, const auto& args) {
        MemoryEvent("AppMemoryUsageLimitChanging", args.NewLimit());
    });
    MemoryEvent("memory_initial");
    // Continues sampling while the UI/emulation threads stall; suspension pauses the timer.
    static const auto heartbeat =
        winrt::Windows::System::Threading::ThreadPoolTimer::CreatePeriodicTimer(
            [](const auto&) { MemoryEvent("memory_heartbeat"); }, std::chrono::seconds(1));
}
} // namespace EdenXbox

// Mesa resolves this exported, allocation-free callback from the executable on a failed API.
extern "C" __declspec(dllexport) void WINAPI NXboxCrashGpuError(const char* stage, HRESULT result,
                                                                HRESULT removed) noexcept {
    EdenXbox::CrashGpuError(stage, result, removed);
}
