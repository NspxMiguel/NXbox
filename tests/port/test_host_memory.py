# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile production memory bookkeeping with minimal host-only hardware stubs."""

import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def without_includes(path):
    return re.sub(r"^#(?:include[^\n]*|pragma once)\n", "", path.read_text(), flags=re.MULTILINE)


class HostMemoryTests(unittest.TestCase):
    def compile_and_run(self, source, defines=("NXBOX_UWP=1",)):
        with tempfile.TemporaryDirectory(prefix="nxbox-memory-") as directory:
            cpp = Path(directory) / "check.cpp"
            binary = Path(directory) / "check"
            cpp.write_text(source)
            subprocess.run(
                [
                    *shlex.split(os.environ.get("CXX", "c++")),
                    "-std=c++20",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    *[f"-D{define}" for define in defines],
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_descriptor_limits_growth_and_invalidation(self):
        table = without_includes(ROOT / "src/video_core/texture_cache/descriptor_table.h")
        source = r"""
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using GPUVAddr = u64;
#define DEBUG_ASSERT(x) assert(x)
namespace Common {
template<typename T, typename U> auto DivCeil(T x, U y) { return (x + y - 1) / y; }
}
namespace Tegra {
struct MemoryManager {
    mutable GPUVAddr last_address{};
    u64 value = 42;
    void ReadBlockUnsafe(GPUVAddr address, void* out, size_t size) const {
        assert(size == sizeof(value));
        last_address = address;
        std::memcpy(out, &value, size);
    }
};
}
TABLE
int main() {
    Tegra::MemoryManager memory;
    // All four tables share this implementation: graphics/compute TIC and TSC.
    std::array<VideoCommon::DescriptorTable<u64>, 4> tables;
    for (auto& table : tables) {
        assert(table.descriptors.empty());
        assert(table.Synchronize(0, 0));
        assert(table.descriptors.size() == 1 && table.read_descriptors.size() == 1);
        assert(table.Read(memory, 0).second);
        assert(!table.Read(memory, 0).second);
        assert(!table.Synchronize(0, 0));
        assert(table.Synchronize(128, 63));
        assert(table.descriptors.size() == 64 && table.read_descriptors.size() == 1);
        assert(table.Read(memory, 63).second);
        assert(memory.last_address == 128 + 63 * sizeof(u64));
        assert(!table.Read(memory, 63).second);
        ++memory.value;
        assert(table.Read(memory, 63).second);
        assert(table.Synchronize(128, 64));
        assert(table.descriptors.size() == 65 && table.read_descriptors.size() == 2);
        assert(table.Read(memory, 63).second);
        assert(table.Read(memory, 64).second);
        assert(!table.Read(memory, 64).second);
        table.Invalidate();
        assert(table.Read(memory, 64).second);
        assert(table.Synchronize(256, 3));
        assert(table.descriptors.size() == 65);
        assert(table.Read(memory, 3).second);
        assert(memory.last_address == 256 + 3 * sizeof(u64));
        // A smaller limit and subsequent regrowth must not retain stale read bits.
        assert(table.Synchronize(128, 64));
        assert(table.Read(memory, 64).second);
        assert(table.Synchronize(512, 4095));
        assert(table.descriptors.size() == 4096);
        assert(table.descriptors.capacity() <= 8192);
        assert(table.Read(memory, 4095).second);
        assert(!table.Read(memory, 4095).second);
        // Large guest tables remain supported; there is no artificial UWP cap.
        assert(table.Synchronize(512, 0x80000));
        assert(table.descriptors.size() == 0x80001);
        assert(table.Read(memory, 0x80000).second);
    }
}
""".replace("TABLE", table)
        self.compile_and_run(source)

    def test_gpu_command_list_storage_and_moves(self):
        header = (ROOT / "src/video_core/dma_pusher.h").read_text()
        command_list = re.search(
            r"struct CommandList final \{.*?^\};", header, re.MULTILINE | re.DOTALL
        ).group()
        source = r"""
#include <array>
#include <cassert>
#include <cstdint>
#include <utility>
#include <variant>
#include <vector>
// The UWP constructor still accepts the core's existing Boost prefetch list.
// Only that input type is stubbed; the stored production vectors are compiled below.
namespace boost::container {
template<typename T, unsigned N> using small_vector = std::vector<T>;
}
struct CommandHeader { std::uint32_t argument; };
struct CommandListHeader { std::uint64_t raw; };
COMMAND_LIST
int main() {
    static_assert(sizeof(CommandList) == 2 * sizeof(std::vector<CommandHeader>));
    using QueueSlot = std::variant<std::monostate, CommandList>;
    static_assert(sizeof(std::array<QueueSlot, 4096>) < 512 * 1024);
    CommandList empty;
    assert(empty.command_lists.capacity() == 0 && empty.prefetch_command_list.capacity() == 0);
    CommandList submitted(8192);
    for (size_t i = 0; i < submitted.command_lists.size(); ++i) submitted.command_lists[i].raw = i;
    const auto* allocation = submitted.command_lists.data();
    QueueSlot slot{std::move(submitted)};
    auto received = std::move(std::get<CommandList>(slot));
    assert(received.command_lists.data() == allocation);
    assert(received.command_lists.size() == 8192);
    for (size_t i = 0; i < received.command_lists.size(); ++i) assert(received.command_lists[i].raw == i);
    boost::container::small_vector<CommandHeader, 512> commands(2048);
    for (size_t i = 0; i < commands.size(); ++i) commands[i].argument = unsigned(i);
    CommandList prefetch{std::move(commands)};
    assert(prefetch.command_lists.empty() && prefetch.prefetch_command_list.size() == 2048);
    for (size_t i = 0; i < prefetch.prefetch_command_list.size(); ++i) assert(prefetch.prefetch_command_list[i].argument == i);
}
""".replace("COMMAND_LIST", command_list)
        self.compile_and_run(source)

    def test_host1x_lazy_slots_and_device_lifetime(self):
        header = (ROOT / "src/video_core/host1x/host1x.h").read_text()
        declaration = re.search(
            r"    using Device = .*?^    std::array<std::unique_ptr<Device>, 1024> devices;",
            header,
            re.MULTILINE | re.DOTALL,
        ).group()
        push = re.search(
            r"    void PushEntries\(s32 fd, ChCommandHeaderList&& entries\) \{.*?^    \}",
            header,
            re.MULTILINE | re.DOTALL,
        ).group()
        implementation = (ROOT / "src/video_core/host1x/host1x.cpp").read_text()
        methods = [
            re.search(
                rf"void Host1x::{name}\([^\n]*\) \{{.*?^\}}",
                implementation,
                re.MULTILINE | re.DOTALL,
            ).group()
            for name in ("StartDevice", "StopDevice")
        ]
        source = (
            r"""
#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <utility>
#include <variant>
using s32 = std::int32_t;
using u32 = std::uint32_t;
#define LOG_ERROR(...) ((void)0)
namespace Tegra::Host1x {
struct Host1x;
enum class ChannelType { NvDec, VIC, Invalid };
struct ChCommandHeaderList { unsigned value; };
struct Nvdec {
    static inline unsigned live = 0;
    unsigned total = 0;
    Nvdec(Host1x&, s32, u32) { ++live; }
    Nvdec(const Nvdec&) = delete;
    Nvdec(Nvdec&&) = delete;
    ~Nvdec() { --live; }
    void PushEntries(ChCommandHeaderList&& entries) { total += entries.value; }
};
struct Vic : Nvdec { using Nvdec::Nvdec; };
struct Host1x {
DECLARATION
PUSH
    void StartDevice(s32, ChannelType, u32);
    void StopDevice(s32, [[maybe_unused]] ChannelType);
};
METHODS
}
int main() {
    using namespace Tegra::Host1x;
    static_assert(sizeof(Host1x) == 1024 * sizeof(void*));
    Host1x host;
    for (const auto& slot : host.devices) assert(!slot);
    host.PushEntries(1023, {5});
    assert(!host.devices[1023]);
    host.StopDevice(1023, ChannelType::NvDec);
    assert(Nvdec::live == 0);
    host.StartDevice(1023, ChannelType::NvDec, 7);
    const auto* address = host.devices[1023].get();
    for (s32 i = 0; i < 1023; ++i) host.StartDevice(i, ChannelType::VIC, 9);
    assert(host.devices[1023].get() == address && Nvdec::live == 1024);
    host.PushEntries(1023, {5});
    assert(std::get<Nvdec>(*host.devices[1023]).total == 5);
    host.StartDevice(1023, ChannelType::VIC, 3);
    assert(Nvdec::live == 1024 && host.devices[1023].get() == address);
    host.PushEntries(1023, {9});
    assert(std::get<Vic>(*host.devices[1023]).total == 9);
    for (s32 i = 0; i < 1024; ++i) host.StopDevice(i, ChannelType::VIC);
    assert(Nvdec::live == 0);
    for (const auto& slot : host.devices) assert(!slot);
}
""".replace("DECLARATION", declaration)
            .replace("PUSH", push)
            .replace("METHODS", "\n".join(methods))
        )
        # The production parameter is unused; suppress only that existing warning.
        self.compile_and_run(
            source.replace("ChannelType type) {\n#if", "[[maybe_unused]] ChannelType type) {\n#if")
        )

    def test_fast_dispatch_mask_covers_exactly_the_table(self):
        header = (ROOT / "src/dynarmic/src/dynarmic/backend/x64/a64_emit_x64.h").read_text()
        constants = re.search(
            r"#if defined\(YUZU_UWP_APPCONTAINER\).*?fast_dispatch_table_mask[^;]*;",
            header,
            re.DOTALL,
        ).group()
        source = r"""
#include <cassert>
#include <cstddef>
#include <cstdint>
using u64 = std::uint64_t;
struct FastDispatchEntry { u64 descriptor; const void* code; };
CONSTANTS
static_assert(sizeof(FastDispatchEntry) == 16);
static_assert((fast_dispatch_table_size & (fast_dispatch_table_size - 1)) == 0);
int main() {
#ifdef YUZU_UWP_APPCONTAINER
    static_assert(fast_dispatch_table_size * sizeof(FastDispatchEntry) == 1024 * 1024);
#else
    static_assert(fast_dispatch_table_size * sizeof(FastDispatchEntry) == 16 * 1024 * 1024);
#endif
    // Covers every possible low-24-bit hash, including the CRC32 lookup path.
    for (u64 hash = 0; hash <= 0xFFFFFF; ++hash) {
        const auto offset = hash & fast_dispatch_table_mask;
        assert(offset % sizeof(FastDispatchEntry) == 0);
        assert(offset / sizeof(FastDispatchEntry) < fast_dispatch_table_size);
    }
}
""".replace("CONSTANTS", constants)
        for defines in ((), ("YUZU_UWP_APPCONTAINER=1",)):
            with self.subTest(defines=defines):
                self.compile_and_run(source, defines)


if __name__ == "__main__":
    unittest.main()
