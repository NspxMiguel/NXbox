# SPDX-License-Identifier: GPL-3.0-or-later
"""Check capture reassembly and the BotW corrupt barrier regression."""

import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "capture_analysis", ROOT / "tools/nxbox/analyze_mesa_capture.py"
)
analysis = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analysis)


class MesaCaptureAnalysisTests(unittest.TestCase):
    def test_duplicate_blocks_and_fragmented_corrupt_barrier(self):
        prefix = (
            "D3D12_LIST_RING capture=0 part=887 offset=0 seq=759 "
            "generation=1690 tid=1900 call=ResourceBarrier "
            "site=d3d12_resource_state.cpp:598 fragment="
        )
        lines = [
            prefix + "0 count=1 barriers=barrier[2]={type=1826445160 ",
            prefix + "1 flags=0x7ff6 } ",
        ]
        records, fragments = analysis.parse("\n".join(lines * 3))
        self.assertEqual((len(records), fragments), (1, 2))
        errors = analysis.audit(records[0])
        self.assertEqual(len(errors), 1)
        self.assertIn("barrier[2]: invalid type 1826445160", errors[0])

    def test_conflicting_duplicate_and_missing_fragment_are_rejected(self):
        line = (
            "D3D12_LIST_RING capture=0 seq=1 generation=1 tid=2 "
            "call=ResourceBarrier site=test fragment=0 count=0 "
        )
        with self.assertRaises(ValueError):
            analysis.parse(line + "\n" + line.replace("count=0", "count=1"))
        with self.assertRaises(ValueError):
            analysis.parse(line.replace("fragment=0", "fragment=1"))

    def test_null_barriers_and_read_unions_are_legal(self):
        record = {
            "call": "ResourceBarrier",
            "args": (
                "count=3 barriers=barrier[0]={type=1 flags=0x0 before=res=NULL after=res=NULL } "
                "barrier[1]={type=2 flags=0x0 res=NULL } "
                "barrier[2]={type=0 flags=0x0 res=ABC {dim=3 format=28 size=32x32x1 "
                "mips=1 samples=1:0 flags=0x1} sub=4294967295 before=0x80 after=0x880 } "
            ),
        }
        self.assertEqual(analysis.audit(record), [])


if __name__ == "__main__":
    unittest.main()
