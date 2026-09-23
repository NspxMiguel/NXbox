// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/sparse_memory.h"

#include <array>
#include <atomic>
#include <mutex>
#include <thread>
#include <windows.h>

#include "common/demand_commit.h"

namespace Common::SparseMemory {
namespace {
using AllocateFunction = void*(WINAPI*)(void*, SIZE_T, ULONG, ULONG);
using QueryFunction = SIZE_T(WINAPI*)(const void*, MEMORY_BASIC_INFORMATION*, SIZE_T);
using AddHandlerFunction = void*(WINAPI*)(ULONG, PVECTORED_EXCEPTION_HANDLER);

struct Range {
    std::atomic<void*> base{nullptr};
    std::atomic<std::size_t> size{0};
    std::atomic<unsigned> readers{0};
};

// Bounded registry: fault handling never allocates or takes an allocator lock.
// This is for VirtualBuffer data only; DRAM has its own handler in HostMemory.
std::array<Range, 256> ranges;
std::mutex allocation_mutex;
std::once_flag initialize_flag;
AllocateFunction allocate_pages = nullptr;
QueryFunction query_pages = nullptr;
void* handler = nullptr;

LONG NTAPI HandleFault(EXCEPTION_POINTERS* exception) {
    if (!exception || !exception->ExceptionRecord ||
        exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        exception->ExceptionRecord->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto& record = *exception->ExceptionRecord;
    for (auto& range : ranges) {
        auto* base = range.base.load(std::memory_order_acquire);
        if (!base) {
            continue;
        }
        const auto size = range.size.load(std::memory_order_relaxed);
        const auto chunk =
            DemandCommit::GetChunk(reinterpret_cast<std::uintptr_t>(base), size,
                                   record.ExceptionInformation[1], record.ExceptionInformation[0]);
        if (!chunk) {
            continue;
        }
        range.readers.fetch_add(1, std::memory_order_acq_rel);
        void* committed = nullptr;
        if (range.base.load(std::memory_order_acquire) == base) {
            MEMORY_BASIC_INFORMATION info{};
            const auto* address = reinterpret_cast<const void*>(record.ExceptionInformation[1]);
            if (query_pages(address, &info, sizeof(info)) == sizeof(info)) {
                if (info.State == MEM_RESERVE) {
                    committed = allocate_pages(static_cast<unsigned char*>(base) + chunk->offset,
                                               chunk->size, MEM_COMMIT, PAGE_READWRITE);
                } else if (info.State == MEM_COMMIT && info.Protect == PAGE_READWRITE) {
                    // Another thread may have committed the chunk after this fault.
                    committed = base;
                }
                // Protection violations must propagate, not retry forever.
            }
        }
        range.readers.fetch_sub(1, std::memory_order_release);
        return committed ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void Initialize() {
    // Avoid a hard import of the VEH API set absent from the Xbox loader.
    const auto library = LoadLibraryExW(L"kernelbase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library) {
        return;
    }
    allocate_pages =
        reinterpret_cast<AllocateFunction>(GetProcAddress(library, "VirtualAllocFromApp"));
    const auto add_handler = reinterpret_cast<AddHandlerFunction>(
        GetProcAddress(library, "AddVectoredExceptionHandler"));
    query_pages = reinterpret_cast<QueryFunction>(GetProcAddress(library, "VirtualQuery"));
    if (allocate_pages && query_pages && add_handler) {
        handler = add_handler(1, HandleFault);
    }
    // The handler and its module reference intentionally have process lifetime.
}
} // namespace

void* Allocate(std::size_t size) noexcept {
    if (size == 0) {
        return nullptr;
    }
    std::call_once(initialize_flag, Initialize);
    if (!handler) {
        return nullptr;
    }
    std::lock_guard lock(allocation_mutex);
    for (auto& range : ranges) {
        if (range.base.load(std::memory_order_acquire)) {
            continue;
        }
        void* base = allocate_pages(nullptr, size, MEM_RESERVE, PAGE_READWRITE);
        if (!base) {
            return nullptr;
        }
        range.size.store(size, std::memory_order_relaxed);
        range.base.store(base, std::memory_order_release);
        return base;
    }
    return nullptr;
}

bool Free(void* base) noexcept {
    if (!base) {
        return true;
    }
    std::lock_guard lock(allocation_mutex);
    for (auto& range : ranges) {
        if (range.base.load(std::memory_order_acquire) != base) {
            continue;
        }
        range.base.store(nullptr, std::memory_order_release);
        while (range.readers.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }
        return VirtualFree(base, 0, MEM_RELEASE) != FALSE;
    }
    return false;
}
} // namespace Common::SparseMemory
