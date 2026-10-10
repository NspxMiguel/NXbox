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
        for name in ("RegisterBlock", "Patch", "Unpatch", "ClearCache", "InvalidateBasicBlocks"):
            match = re.search(
                rf"^(?:void|EmitX64::BlockDescriptor) EmitX64::{name}\([^\n]*\) \{{.*?^\}}",
                source,
                re.MULTILINE | re.DOTALL,
            )
            self.assertIsNotNone(match, name)
            methods.append(match.group())
        header = EMITTER.with_suffix(".h").read_text()
        patch_struct = re.search(
            r"    struct PatchInformation \{.*?^    \};",
            header,
            re.MULTILINE | re.DOTALL,
        ).group()
        program = r"""
#include <cassert>
#include <cstddef>
#include <map>
#include <string>
#include <vector>
#include <set>
namespace boost::container {
template<typename T, unsigned N> using small_vector = std::vector<T>;
}
namespace Common {
template<typename T> using unordered_set = std::set<T>;
}
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
void PerfMapClear() {}
struct EmitX64 {
    struct Code {
        CodePtr cursor = nullptr;
        unsigned moves = 0;
        void EnableWriting() {}
        void DisableWriting() {}
        CodePtr getCurr() const { return cursor; }
        void SetCodePtr(CodePtr p) { cursor = p; ++moves; }
    } code;
PATCH_STRUCT
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
    void ClearCache();
    void InvalidateBasicBlocks(const Common::unordered_set<IR::LocationDescriptor>&);
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
    auto& info = emitter.patch_information[{7}];
    using Kind = EmitX64::PatchInformation::Kind;
    info.Add(Kind::Jg, storage + 1);
    info.Add(Kind::Jmp, storage + 4);
    info.Add(Kind::Jz, storage + 3);
    info.Add(Kind::MovRcx, storage + 5);
    info.Add(Kind::Jg, storage + 2);
#ifdef YUZU_UWP_APPCONTAINER
    static_assert(sizeof(info) == sizeof(std::vector<CodePtr>));
    // Exercise growth and movement of the actual compact production records.
    for (unsigned i = 0; i < 100; ++i) info.Add(Kind::Jmp, storage + 4);
    auto moved = std::move(info);
    assert(moved.sites.size() == 105);
    assert(moved.sites[0].kind == Kind::Jg && moved.sites[1].kind == Kind::Jmp);
    info = std::move(moved);
#endif
    emitter.RegisterBlock({7}, storage + 20, 8);
    assert(emitter.targets.size() == 5 && emitter.code.cursor == storage + 63);
    for (const auto& [site, target] : emitter.targets) { (void)site; assert(target == storage + 20); }
    emitter.Unpatch({7});
    for (const auto& [site, target] : emitter.targets) { (void)site; assert(target == nullptr); }
    emitter.RegisterBlock({7}, storage + 30, 8);
    for (const auto& [site, target] : emitter.targets) { (void)site; assert(target == storage + 30); }
    emitter.InvalidateBasicBlocks({{7}});
    assert(emitter.block_descriptors.empty());
    assert(emitter.patch_information.size() == 1);
    for (const auto& [site, target] : emitter.targets) { (void)site; assert(target == nullptr); }
    emitter.RegisterBlock({7}, storage + 40, 8);
    for (const auto& [site, target] : emitter.targets) { (void)site; assert(target == storage + 40); }
    emitter.Unpatch({8});
    assert(emitter.patch_information.size() == 1);
    assert(emitter.code.cursor == storage + 63);
#ifdef NXBOX_UWP
    assert(formatted_names == 0);
#else
    assert(formatted_names == 3);
#endif
    emitter.ClearCache();
    assert(emitter.patch_information.empty() && emitter.block_descriptors.empty());
    emitter.Patch({7}, storage);
#ifdef NXBOX_UWP
    assert(emitter.patch_information.empty());
#endif
}
""".replace("METHODS", "\n".join(methods)).replace("PATCH_STRUCT", patch_struct)
        for defines in ((), ("NXBOX_UWP=1",), ("NXBOX_UWP=1", "YUZU_UWP_APPCONTAINER=1")):
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
