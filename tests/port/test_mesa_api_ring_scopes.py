# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify every wrapper's lexical scope against its pre-instrumentation source.

The host has no Windows SDK or Mesa-generated configuration. Instead of fake
COM declarations accepting arbitrary members, prove that wrapping preserves the
entire original expression, declarations, scopes and include order. Only the
free function nxbox_api and a string literal may be introduced at a call site.
This is a scope-conservation check, not a replacement for the MSVC build.
"""

import importlib.util
import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(os.environ.get("NXBOX_MESA_SRC", "/tmp/mesa-pin"))
SPEC = importlib.util.spec_from_file_location(
    "mesa_scope_patch", ROOT / "tools/nxbox/patch_mesa_uwp.py"
)
PATCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PATCH)


def mask(source):
    return re.sub(
        r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        lambda m: "".join("\n" if c == "\n" else " " for c in m[0]),
        source,
    )


def extract(source):
    """Read actual output, independently of the patcher's receiver scanner."""
    result = []
    masked = mask(source)
    for match in re.finditer(r"\bnxbox_api\s*\(", masked):
        prefix = masked[: match.start()].rstrip()
        if prefix.endswith(("->", ".", "::")):
            raise AssertionError("nxbox_api must be a free function, not a receiver member")
        stack = []
        for end in range(match.end(), len(masked)):
            char = masked[end]
            if char in "([{":
                stack.append(char)
            elif char in ")]}":
                if not stack:
                    raise AssertionError("Missing site argument")
                assert stack.pop() == {")": "(", "]": "[", "}": "{"}[char]
            elif char == "," and not stack:
                break
        suffix = re.match(r',\s*"([^"\n]+):(\d+)"\)\.(\w+)\(', source[end:])
        assert suffix, source[match.start() : end + 100]
        result.append(
            (match.start(), end + suffix.end(), source[match.end() : end], *suffix.groups())
        )
    return result


def check_conservation(before, after):
    calls = extract(after)
    restored = after
    for start, end, receiver, _, _, method in reversed(calls):
        restored = restored[:start] + receiver + "->" + method + "(" + restored[end:]
    # Normalize tokens only: the one permitted argument edit is NULL -> nullptr.
    normalize = lambda s: re.sub(r"\bNULL\b", "nullptr", mask(s))

    # String literals must also be preserved; mask only for NULL normalization.
    def normalized_source(s):
        m = mask(s)
        for hit in reversed(list(re.finditer(r"\bNULL\b", m))):
            s = s[: hit.start()] + "nullptr" + s[hit.end() :]
        return s

    assert normalize(restored) == normalize(before), "Call changed identifiers or lexical scopes"
    assert normalized_source(restored) == normalized_source(before), "Call changed source text"
    return calls


def visible_scope(source, position):
    """Retain parameters and active lexical blocks, discard closed siblings."""
    frames = [["", ""]]
    for char in mask(source[:position]):
        if char == "{":
            parent = frames[-1][1]
            boundary = max(parent.rfind(";"), parent.rfind("}")) + 1
            frames[-1][1] = parent[:boundary]
            frames.append([parent[boundary:], ""])
        elif char == "}":
            frames.pop()
            frames[-1][1] += "}"
        else:
            frames[-1][1] += char
    return "\n".join(header + body for header, body in frames[1:])


def check_receiver_scope(source, position, receiver, driver):
    scope = visible_scope(source, position)
    tokens = list(re.finditer(r"[A-Za-z_]\w*", receiver))
    for token in tokens:
        name = token[0]
        prefix = receiver[: token.start()].rstrip()
        suffix = receiver[token.end() :].lstrip()
        if prefix.endswith(("->", ".", "::")) or suffix.startswith("("):
            continue  # Member paths and free functions are conserved verbatim.
        if name.isupper():
            continue  # Compile-time video depth macros, not local variables.
        declaration = (
            r"\b(?:[A-Za-z_]\w*(?:::\w+)?(?:<[^;{}]+>)?)\s+[*&\s]*" + re.escape(name) + r"\b"
        )
        if re.search(declaration, scope):
            continue
        # These two C++ DPB manager implementations use a class member.
        if name == "m_pDevice":
            headers = list(driver.glob("d3d12_video*dpb_manager.h"))
            assert any(
                re.search(r"ID3D12Device\s*\*\s*m_pDevice\b", h.read_text()) for h in headers
            )
            continue
        raise AssertionError(f"Receiver identifier {name!r} has no visible declaration: {receiver}")


class MesaApiRingScopeTests(unittest.TestCase):
    def test_receiver_forms_and_argument_preservation(self):
        for receiver in (
            "dev",
            "screen->dev",
            "d3d12_screen(ctx->base.screen)->dev",
            "d3d12_screen(pipe->screen)->dev",
            "pool[index % DEPTH].allocator",
            "get_device(nested(ctx, index))->dev",
            "device.Get()",
            "(*device)",
        ):
            with self.subTest(receiver=receiver):
                original = f'  {receiver}->CreateFence(NULL, "NULL"); // dev->CreateFence(NULL)\n'
                result, inventory = PATCH.instrument_api_calls(
                    original, ["CreateFence"], "test.cpp"
                )
                calls = check_conservation(original, result)
                self.assertEqual(len(calls), 1)
                self.assertEqual(calls[0][2], receiver)
                self.assertEqual(inventory[0]["receiver"], receiver)
                self.assertIn('nullptr, "NULL"', result)

    def test_checker_rejects_member_wrapper_and_changed_local(self):
        original = "d3d12_screen(ctx->base.screen)->dev->CreateCommandSignature();"
        with self.assertRaisesRegex(AssertionError, "free function"):
            check_conservation(
                original,
                'd3d12_screen(ctx->base.screen)->nxbox_api(dev, "test.cpp:1").CreateCommandSignature();',
            )
        with self.assertRaisesRegex(AssertionError, "identifiers"):
            check_conservation(
                original, 'nxbox_api(screen->dev, "test.cpp:1").CreateCommandSignature();'
            )

    def test_scope_checker_rejects_closed_sibling_and_later_declarations(self):
        for body in (
            "{ ID3D12Device *dev; } dev->Reset();",
            "dev->Reset(); ID3D12Device *dev;",
        ):
            source = "void run() { " + body + " }"
            with (
                self.subTest(body=body),
                self.assertRaisesRegex(AssertionError, "no visible declaration"),
            ):
                check_receiver_scope(source, source.index("dev->"), "dev", ROOT)
        source = "void run(ID3D12Device *dev) { { int ignored; } dev->Reset(); }"
        check_receiver_scope(source, source.index("dev->"), "dev", ROOT)

    def test_unknown_receiver_fails_closed(self):
        for receiver in ("ns::dev", "get<Device>()"):
            with self.subTest(receiver=receiver), self.assertRaises(RuntimeError):
                PATCH.instrument_api_calls(receiver + "->Reset();", ["Reset"], "test.cpp")

    @unittest.skipUnless(SOURCE.is_dir(), "Set NXBOX_MESA_SRC or provide /tmp/mesa-pin")
    def test_all_full_chain_call_sites(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-api-scopes-") as temporary:
            root = Path(temporary) / "mesa"
            shutil.copytree(SOURCE, root, ignore=shutil.ignore_patterns(".git"))
            captured = {}
            instrument = PATCH.instrument_api_calls

            def capture(source, methods, name):
                after, inventory = instrument(source, methods, name)
                captured[name] = (source, after, inventory)
                return after, inventory

            with (
                patch.object(PATCH, "SKIP", set()),
                patch.object(PATCH, "instrument_api_calls", capture),
            ):
                PATCH.patch(root)
            driver = root / "src/gallium/drivers/d3d12"
            inventory = json.loads((driver / "nxbox_api_inventory.json").read_text())
            extracted = []
            for name, (before, after, expected) in captured.items():
                with self.subTest(file=name):
                    calls = check_conservation(before, after)
                    final = (driver / name).read_text()
                    final_calls = extract(final)
                    self.assertEqual(
                        [call[2:] for call in final_calls], [call[2:] for call in calls]
                    )
                    for start, _, receiver, file, line, method in final_calls:
                        check_receiver_scope(final, start, receiver, driver)
                        extracted.append(
                            {"file": file, "line": int(line), "api": method, "receiver": receiver}
                        )
                    self.assertEqual(len(calls), len(expected))
                    if calls and name.endswith(".cpp"):
                        self.assertIn(
                            '#include "d3d12_common.h"\n#include "nxbox_api_ring.h"', final
                        )
                    if calls and name.endswith(".h"):
                        self.assertIn('#pragma once\n#include "nxbox_api_ring.h"', final)
            order = lambda item: (item["file"], item["line"])
            self.assertEqual(sorted(extracted, key=order), inventory)
            self.assertEqual(len(extracted), 169)
            self.assertEqual(len({item["file"] for item in extracted}), 28)
            # The ring never dereferences a Mesa screen; its definition requires
            # COM types, not a complete d3d12_screen. No member is added to it.
            helper = (driver / "nxbox_api_ring.h").read_text()
            self.assertNotIn("d3d12_screen", mask(helper))
            self.assertNotIn("nxbox_api", (driver / "d3d12_screen.h").read_text())
            self.assertIn("template <typename T> auto nxbox_api(", helper)
            self.compile_command_signature(driver, Path(temporary))

    def compile_command_signature(self, driver, temporary):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            return  # The exhaustive static check above still runs.
        # Compile the real failing function with narrow SDK/Mesa stubs. Keep
        # the actual API ring template, including its forwarding implementation.
        from test_mesa_api_ring import MOCK

        mock = MOCK.replace(
            "HRESULT removed=0;",
            "template<class... A> HRESULT CreateCommandSignature(A&&...) { return 0; }\n HRESULT removed=0;",
        )
        code = (
            mock
            + '\n#include "nxbox_api_ring.h"\n'
            + r"""
struct ID3D12RootSignature {};
struct ID3D12CommandSignature {};
struct pipe_screen {};
struct d3d12_screen : pipe_screen { ID3D12Device *dev; };
struct d3d12_screen *d3d12_screen(pipe_screen *p) { return static_cast<struct d3d12_screen *>(p); }
struct d3d12_context { struct { pipe_screen *screen; } base; };
struct D3D12_INDIRECT_ARGUMENT_DESC {
 unsigned Type;
 struct { unsigned RootParameterIndex, DestOffsetIn32BitValues, Num32BitValuesToSet; } Constant;
};
struct D3D12_COMMAND_SIGNATURE_DESC {
 unsigned ByteStride, NumArgumentDescs; D3D12_INDIRECT_ARGUMENT_DESC *pArgumentDescs;
};
enum { D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT, D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH,
       D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED, D3D12_INDIRECT_ARGUMENT_TYPE_DRAW };
"""
        )
        header = (driver / "d3d12_cmd_signature.h").read_text()
        code += (
            "struct d3d12_cmd_signature_key {"
            + header.split("struct d3d12_cmd_signature_key {", 1)[1].split("};", 1)[0]
            + "};\n"
        )
        source = (driver / "d3d12_cmd_signature.cpp").read_text()
        code += (
            "static ID3D12CommandSignature *\n"
            + source.split("static ID3D12CommandSignature *\n", 1)[1].split(
                "\nID3D12CommandSignature *", 1
            )[0]
        )
        path = temporary / "command_signature.cpp"
        path.write_text(code)
        result = subprocess.run(
            [compiler, "-std=c++17", "-fsyntax-only", "-I", str(driver), str(path)],
            check=False,
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
