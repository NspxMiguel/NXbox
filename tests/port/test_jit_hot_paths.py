# SPDX-License-Identifier: GPL-3.0-or-later
"""Run production JIT bookkeeping/assertions with host stubs, without a Windows SDK.

This checks lookup/link lifetimes and assertion semantics, not emitted x86 code.
Run with CXX set to a GCC/Clang-compatible compiler (defaults to c++).
"""

import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EMITTER = ROOT / "src/dynarmic/src/dynarmic/backend/x64/emit_x64.cpp"


class JitHotPathTests(unittest.TestCase):
    def compile_and_run(self, source, defines=(), stub_logging=False):
        with tempfile.TemporaryDirectory(prefix="nxbox-jit-") as directory:
            directory = Path(directory)
            if stub_logging:
                (directory / "common").mkdir()
                (directory / "common/logging.h").write_text(
                    "#define LOG_CRITICAL(category, ...) Log(__VA_ARGS__)\n"
                )
            cpp = directory / "check.cpp"
            cpp.write_text(source)
            binary = directory / "check"
            subprocess.run(
                [
                    *shlex.split(os.environ.get("CXX", "c++")),
                    "-std=c++20",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    *[f"-D{define}" for define in defines],
                    f"-I{directory}",
                    f"-I{ROOT / 'src'}",
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_patch_lifetime_and_unused_profiler(self):
        source = EMITTER.read_text()
        methods = []
        for name in ("RegisterBlock", "Patch", "Unpatch"):
            match = re.search(
                rf"^(?:void|EmitX64::BlockDescriptor) EmitX64::{name}\([^\n]*\) \{{.*?^\}}",
                source,
                re.MULTILINE | re.DOTALL,
            )
            self.assertIsNotNone(match, name)
            methods.append(match.group())
        program = r"""
#include <cassert>
#include <cstddef>
#include <map>
#include <string>
#include <vector>
namespace IR {
struct LocationDescriptor {
    unsigned value;
    unsigned Value() const { return value; }
    auto operator<=>(const LocationDescriptor&) const = default;
};
}
using CodePtr = const unsigned char*;
static unsigned formatted_names;
std::string LocationDescriptorToFriendlyName(IR::LocationDescriptor) {
    ++formatted_names;
    return "block";
}
void PerfMapRegister(CodePtr, CodePtr, const std::string&) {}
struct EmitX64 {
    struct Code {
        CodePtr cursor = nullptr;
        unsigned moves = 0;
        CodePtr getCurr() const { return cursor; }
        void SetCodePtr(CodePtr p) { cursor = p; ++moves; }
    } code;
    struct PatchInformation { std::vector<CodePtr> jg, jz, jmp, mov_rcx; };
    struct BlockDescriptor { CodePtr entrypoint; size_t size; };
    std::map<IR::LocationDescriptor, PatchInformation> patch_information;
    std::map<IR::LocationDescriptor, BlockDescriptor> block_descriptors;
    std::map<CodePtr, CodePtr> targets;
    void EmitPatchJg(IR::LocationDescriptor, CodePtr p) { targets[code.cursor] = p; }
    void EmitPatchJz(IR::LocationDescriptor, CodePtr p) { targets[code.cursor] = p; }
    void EmitPatchJmp(IR::LocationDescriptor, CodePtr p) { targets[code.cursor] = p; }
    void EmitPatchMovRcx(CodePtr p) { targets[code.cursor] = p; }
    BlockDescriptor RegisterBlock(const IR::LocationDescriptor&, CodePtr, size_t);
    void Patch(const IR::LocationDescriptor&, CodePtr);
    void Unpatch(const IR::LocationDescriptor&);
};
METHODS
int main() {
    unsigned char storage[64]{};
    EmitX64 emitter;
    emitter.code.cursor = storage + 63;
    for (unsigned i = 0; i < 100000; ++i) emitter.Patch({i}, storage);
#ifdef NXBOX_UWP
    assert(emitter.patch_information.empty() && emitter.code.moves == 0);
#else
    assert(emitter.patch_information.size() == 100000);
#endif
    emitter.patch_information.clear();
    emitter.patch_information[{7}] = {{storage + 1, storage + 2},
        {storage + 3}, {storage + 4}, {storage + 5}};
    emitter.RegisterBlock({7}, storage + 20, 8);
    assert(emitter.targets.size() == 5 && emitter.code.cursor == storage + 63);
    for (const auto& [site, target] : emitter.targets) { (void)site; assert(target == storage + 20); }
    emitter.Unpatch({7});
    for (const auto& [site, target] : emitter.targets) { (void)site; assert(target == nullptr); }
    emitter.RegisterBlock({7}, storage + 30, 8);
    for (const auto& [site, target] : emitter.targets) { (void)site; assert(target == storage + 30); }
    emitter.Unpatch({8});
    assert(emitter.patch_information.size() == 1);
    assert(emitter.code.cursor == storage + 63);
#ifdef NXBOX_UWP
    assert(formatted_names == 0);
#else
    assert(formatted_names == 2);
#endif
}
""".replace("METHODS", "\n".join(methods))
        for defines in ((), ("NXBOX_UWP=1",)):
            with self.subTest(defines=defines):
                self.compile_and_run(program, defines)

    def test_assertions_preserve_evaluation_and_failure(self):
        program = r"""
#include <cassert>
#include <cstdlib>
static int logs, failures;
template <typename... Args> void Log(Args&&...) { ++logs; }
#include "common/assert.h"
void AssertFailSoftImpl() { ++failures; }
[[noreturn]] void AssertFatalImpl() { std::abort(); }
int main() {
    int conditions = 0, messages = 0;
    ASSERT_MSG(++conditions == 1, "{}", ++messages);
    assert(conditions == 1 && messages == 0 && logs == 0 && failures == 0);
    ASSERT_MSG(++conditions == 1, "{}", ++messages);
    assert(conditions == 2 && messages == 1 && logs == 1 && failures == 1);
    ASSERT(true);
    ASSERT(false);
    assert(logs == 2 && failures == 2);
    int executed = 0;
    ASSERT_OR_EXECUTE(false, ++executed;);
    assert(executed == 1 && logs == 3 && failures == 3);
}
"""
        for defines in ((), ("NXBOX_UWP=1",), ("NXBOX_UWP=1", "NXBOX_INLINE_JIT_ASSERTS=1")):
            with self.subTest(defines=defines):
                self.compile_and_run(program, defines, stub_logging=True)


if __name__ == "__main__":
    unittest.main()
