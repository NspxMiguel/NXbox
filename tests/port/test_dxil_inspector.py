# SPDX-License-Identifier: GPL-3.0-or-later
"""Container parsing against captured shaders and malformed binary boundaries."""

import importlib.util
import json
import struct
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("inspect_dxil", ROOT / "tools/nxbox/inspect_dxil.py")
DXIL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DXIL)
CAPTURE = json.loads((ROOT / "tests/port/fixtures/botw-pipe6-dxil.json").read_text())


def container(parts):
    offset = 32 + len(parts) * 4
    offsets, data = [], b""
    for name, payload in parts:
        offsets.append(offset)
        part = name.encode() + struct.pack("<I", len(payload)) + payload
        part += b"\0" * (-len(part) % 4)
        data += part
        offset += len(part)
    return (
        b"DXBC"
        + bytes(16)
        + struct.pack("<III", 1, offset, len(parts))
        + struct.pack(f"<{len(parts)}I", *offsets)
        + data
    )


class DxilInspectorTests(unittest.TestCase):
    def test_capture_resources_versions_and_signatures(self):
        vs, ps = (DXIL.inspect(bytes.fromhex(CAPTURE[stage]))["parts"] for stage in ("VS", "PS"))
        for parts in (vs, ps):
            self.assertEqual(set(parts), {"SFI0", "ISG1", "OSG1", "PSV0", "DXIL"})
            self.assertEqual(parts["SFI0"]["flags"], 0)
            program = parts["DXIL"]
            self.assertEqual(program["shader_model"], "6.4")
            self.assertEqual(program["header_dxil_version"], "1.0")
            self.assertEqual(program["versions"], {"dx.version": [1, 4], "dx.valver": [1, 8]})
            self.assertFalse(program["half_type_declared"])
            self.assertFalse(program["int16_type_declared"])
            self.assertFalse(program["derivative_declarations"])
            self.assertFalse(program["sample_declarations"])
            self.assertTrue(all(e["min_precision"] == 0 for e in parts["ISG1"]["elements"]))
        self.assertEqual(
            vs["PSV0"]["resources"],
            [dict(type="cbv", space=0, lower=1, upper=1, unbounded=False, kind="cbuffer", flags=0)],
        )
        self.assertEqual(
            ps["PSV0"]["resources"],
            [
                dict(
                    type="srv_typed",
                    space=0,
                    lower=0,
                    upper=0,
                    unbounded=False,
                    kind="texture2darray",
                    flags=0,
                )
            ],
        )
        self.assertIn("dx.op.textureLoad.f32", ps["DXIL"]["symbols"])
        self.assertEqual([e["index"] for e in vs["ISG1"]["elements"]], [0, 1])
        self.assertEqual(vs["OSG1"]["elements"][-1]["system_value"], "SV_Position")
        self.assertEqual(ps["OSG1"]["elements"][0]["system_value"], "SV_Target")

    def test_hash_rdat_and_unknown_parts(self):
        resource = struct.pack("<8I", 0, 7, 0, 0, 0, 0, 0, 0)
        table = struct.pack("<II", 1, 32) + resource
        rdat = struct.pack("<IIIII", 0x10, 1, 12, 3, len(table)) + table
        parsed = DXIL.inspect(
            container(
                [
                    ("HASH", struct.pack("<I", 1) + bytes(range(16))),
                    ("RDAT", rdat),
                    ("ZZZZ", b"test"),
                ]
            )
        )["parts"]
        self.assertEqual(parsed["HASH"]["flags"], 1)
        self.assertEqual(parsed["HASH"]["digest"], bytes(range(16)).hex())
        self.assertEqual(parsed["RDAT"]["tables"][0]["resources"][0]["kind"], 7)
        self.assertEqual(parsed["ZZZZ"]["hex"], "74657374")

    def test_every_truncated_capture_and_duplicate_part_rejected(self):
        data = bytes.fromhex(CAPTURE["VS"])
        for length in range(len(data)):
            with self.assertRaises(ValueError):
                DXIL.inspect(data[:length])
        with self.assertRaisesRegex(ValueError, "duplicate"):
            DXIL.inspect(container([("HASH", bytes(20)), ("HASH", bytes(20))]))
        with self.assertRaises(ValueError):
            DXIL.inspect(container([("HASH", bytes(19))]))

    def test_nested_offsets_and_resource_counts_rejected(self):
        data = bytearray(bytes.fromhex(CAPTURE["VS"]))
        for index in range(5):
            pos = DXIL.word(data, 32 + index * 4)
            damaged = data.copy()
            struct.pack_into("<I", damaged, pos + 4, len(data))
            with self.assertRaises(ValueError):
                DXIL.inspect(damaged)
        for runtime, count, stride in ((24, 1, 0), (24, 1000, 16), (20, 0, 16)):
            payload = (
                struct.pack("<I", runtime) + bytes(runtime) + struct.pack("<II", count, stride)
            )
            with self.assertRaises(ValueError):
                DXIL.inspect(container([("PSV0", payload)]))
        with self.assertRaises(ValueError):
            DXIL.inspect(container([("ISG1", struct.pack("<II", 10, 8))]))


if __name__ == "__main__":
    unittest.main()
