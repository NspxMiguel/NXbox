# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise crash-file formatting and exception selection without Windows APIs."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class CrashFormatTests(unittest.TestCase):
    def test_fixed_storage_addresses_and_exception_codes(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        self.assertIsNotNone(compiler, "A host C++ compiler is required")
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "crash_format.cpp"
            executable = Path(directory) / "crash_format"
            source.write_text(
                r"""
#include <cassert>
#include <cstring>
#include <limits>
#include "eden_uwp/crash_format.h"
using namespace EdenXbox::Crash;
int main() {
    Line windows;
    windows.Address("Q:\\Package\\eden-uwp.exe", 0x12345678, 0x12000000);
    windows.Finish();
    assert(std::strcmp(windows.data, "eden-uwp.exe+0x345678\n") == 0);
    Line dll;
    dll.Address("/package/opengl32.dll", 0x1000, 0x1000);
    dll.Finish();
    assert(std::strcmp(dll.data, "opengl32.dll+0x0\n") == 0);
    for (const auto base : {std::uintptr_t{0}, std::uintptr_t{0x2000}}) {
        Line unknown;
        unknown.Address("ntdll.dll", 0x1234, base);
        unknown.Finish();
        assert(std::strcmp(unknown.data, "unknown+0x1234\n") == 0);
    }
    Line numbers;
    numbers.Number(std::numeric_limits<std::uint64_t>::max());
    numbers.Add(" ");
    numbers.Number(std::numeric_limits<std::uint64_t>::max(), 16);
    numbers.Finish();
    assert(std::strcmp(numbers.data, "18446744073709551615 ffffffffffffffff\n") == 0);
    Line sanitized;
    sanitized.Add("name\nwith\rcontrol\t");
    sanitized.Finish();
    assert(std::strcmp(sanitized.data, "name?with?control?\n") == 0);
    Line full;
    for (unsigned i = 0; i < 2000; ++i) full.Add("x");
    full.Finish();
    assert(full.size == sizeof(full.data) - 1);
    assert(full.data[full.size - 1] == '\n' && full.data[full.size] == 0);
    for (const auto code : {0xc0000005u, 0xc00000fdu, 0xc000001du, 0xc0000094u,
                            0xc0000409u, 0xc0000374u}) {
        assert(FatalCode(code, false));
    }
    assert(!FatalCode(0xe06d7363u, false));
    assert(FatalCode(0xe06d7363u, true));
    assert(!FatalCode(0x80000003u, false)); // debugger breakpoint
    assert(!FatalCode(0x80000001u, false)); // guard-page exception
}
"""
            )
            subprocess.run(
                [
                    compiler,
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "src"),
                    str(source),
                    "-o",
                    str(executable),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(executable)], check=True)

    def test_mesa_failure_bridge_preserves_hresult_and_removal(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "bridge.cpp"
            executable = Path(directory) / "bridge"
            source.write_text(
                r"""
#include <cassert>
#include <cstdint>
#include <cstring>
using HRESULT = std::int32_t;
#define SUCCEEDED(hr) ((hr) >= 0)
#define WINAPI
unsigned resolutions = 0, calls = 0;
bool available = true;
void Callback(const char* stage, HRESULT result, HRESULT removed) {
    ++calls;
    assert(std::strcmp(stage, "Signal") == 0);
    assert(result == -9 && removed == -2);
}
void* GetModuleHandleW(const wchar_t* name) { assert(!name); return nullptr; }
using Proc = void (*)();
Proc GetProcAddress(void*, const char* name) {
    ++resolutions;
    assert(std::strcmp(name, "NXboxCrashGpuError") == 0);
    return available ? reinterpret_cast<Proc>(Callback) : nullptr;
}
#include "mesa_crash.h"
int main() {
    nxbox_crash_gpu_error("success", 0, 0);
    assert(resolutions == 0 && calls == 0); // no loader work on the success path
    nxbox_crash_gpu_error("Signal", -9, -2);
    assert(resolutions == 1 && calls == 1);
    available = false;
    nxbox_crash_gpu_error("Signal", -9, -2);
    assert(resolutions == 2 && calls == 1); // standalone/old executable is harmless
}
"""
            )
            subprocess.run(
                [
                    compiler,
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "tools/nxbox"),
                    str(source),
                    "-o",
                    str(executable),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
