# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise native-clock selection and the production notified timed wait without an SDK."""

import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class CpuJitPolicyTests(unittest.TestCase):
    def compile_and_run(self, program, override=None):
        with tempfile.TemporaryDirectory(prefix="nxbox-cpu-policy-") as directory:
            source = Path(directory) / "check.cpp"
            binary = Path(directory) / "check"
            source.write_text(program)
            subprocess.run(
                [
                    *shlex.split(os.environ.get("CXX", "c++")),
                    "-std=c++20",
                    "-pthread",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{ROOT / 'src'}",
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            environment = os.environ.copy()
            environment.pop("NXBOX_EVENT_WAIT", None)
            if override is not None:
                environment["NXBOX_EVENT_WAIT"] = override
            subprocess.run([str(binary)], check=True, env=environment, timeout=30)

    def test_missing_cpuid_frequency_and_calibration_rejection(self):
        self.compile_and_run(
            r"""
#include <cassert>
#include <limits>
#include "common/native_clock_policy.h"
using namespace Common::NativeClockPolicy;
int main() {
    assert(Enabled(nullptr) && Enabled("1") && Enabled(""));
    assert(!Enabled("0") && Enabled("00"));
    unsigned calls = 0;
    auto estimate = [&] { ++calls; return 3'500'000'000ULL; };
    // Missing leaf 0x15 and nonzero denominator with zero crystal both report zero.
    assert(ResolveFrequency(true, 0, true, estimate) == 3'500'000'000ULL);
    assert(ResolveFrequency(true, 0, false, estimate) == 0);
    assert(ResolveFrequency(false, 0, true, estimate) == 0);
    assert(ResolveFrequency(true, 2'000'000'000, true, estimate) == 2'000'000'000);
    assert(calls == 1);
    assert(ResolveFrequency(true, 0, true, [] { return 0; }) == 0);
    assert(SampleFrequency(350'000'000, 100'000'000) == 3'500'000'000);
    // Overslept samples use actual elapsed time.
    assert(SampleFrequency(700'000'000, 200'000'000) == 3'500'000'000);
    for (auto ns : {-1LL, 0LL, 49'999'999LL, 5'000'000'001LL})
        assert(SampleFrequency(350'000'000, ns) == 0);
    assert(SampleFrequency(0, 100'000'000) == 0);
    assert(SampleFrequency(std::numeric_limits<unsigned long long>::max(), 100'000'000) == 0);
    assert(StableFrequency(3'500'000'000, 3'501'000'000) == 3'500'500'000);
    assert(StableFrequency(3'500'000'000, 3'600'000'000) == 0);
    assert(StableFrequency(0, 3'500'000'000) == 0);
    assert(StableFrequency(1'000'000'000, 1'000'000'000) == 0);
}
"""
        )

    def test_notified_wait_timeout_signal_and_consumption(self):
        source = (ROOT / "src/common/thread.cpp").read_text()
        method = re.search(
            r"^bool Event::WaitFor\(const std::chrono::nanoseconds time\) \{.*?^\}",
            source,
            re.MULTILINE | re.DOTALL,
        ).group()
        program = r"""
#include <cassert>
#include <cstdlib>
#include "common/thread.h"
#include "common/native_clock_policy.h"
#define YUZU_UWP_APPCONTAINER 1
namespace Common {
METHOD
}
using namespace std::chrono_literals;
int main() {
    Common::Event event;
    const auto start = std::chrono::steady_clock::now();
    assert(!event.WaitFor(40ms));
    assert(std::chrono::steady_clock::now() - start >= 20ms);
    event.Set();
    assert(event.WaitFor(0ns));
    assert(!event.IsSet());
    assert(!event.WaitFor(0ns));
    std::jthread notifier([&] { std::this_thread::sleep_for(5ms); event.Set(); });
    assert(event.WaitFor(5s));
    assert(!event.IsSet());
}
""".replace("METHOD", method)
        for override in (None, "0"):
            with self.subTest(override=override):
                self.compile_and_run(program, override)


if __name__ == "__main__":
    unittest.main()
