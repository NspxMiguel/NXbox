# SPDX-License-Identifier: GPL-3.0-or-later
"""The statistics query policy must patch the pinned Mesa and keep the other query types real."""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(os.environ.get("NXBOX_MESA_SRC", "/tmp/mesa-pin"))


@unittest.skipUnless(SOURCE.exists(), "needs a pristine pinned Mesa checkout")
class MesaQueryPolicyTests(unittest.TestCase):
    def test_statistics_queries_become_software_queries(self):
        with tempfile.TemporaryDirectory() as temporary:
            tree = Path(temporary) / "mesa"
            shutil.copytree(SOURCE, tree)
            subprocess.run(
                [sys.executable, str(ROOT / "tools/nxbox/patch_mesa_uwp.py"), str(tree)],
                check=True,
                capture_output=True,
            )
            source = (tree / "src/gallium/drivers/d3d12/d3d12_query.cpp").read_text()
        self.assertEqual(source.count("nxbox_soft_subquery("), 3)
        self.assertIn('"NXBOX_D3D12_QUERIES"', source)
        self.assertIn("D3D12_QUERY_TYPE_PIPELINE_STATISTICS ||", source)
        # Occlusion and timestamp queries are not matched by the policy.
        helper = source.split("nxbox_soft_subquery(const", 1)[1].split("}\n", 1)[0]
        self.assertNotIn("OCCLUSION", helper)
        self.assertNotIn("TIMESTAMP", helper)
        # The gate sits at the top of subquery_should_be_active and of the CPU result path.
        gate = source.split("subquery_should_be_active(struct d3d12_context", 1)[1][:200]
        self.assertIn("nxbox_soft_subquery(q, sub_query)", gate)


if __name__ == "__main__":
    unittest.main()
