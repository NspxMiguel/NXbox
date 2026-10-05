# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise every Mesa patch on an optional pristine checkout, including CRLF."""

import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(os.environ.get("NXBOX_MESA_SRC", "/tmp/mesa-pin"))


def snapshot(root):
    return {
        path.relative_to(root): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in root.rglob("*")
        if path.is_file()
    }


class MesaFullChainTests(unittest.TestCase):
    @unittest.skipUnless(SOURCE.is_dir(), "Set NXBOX_MESA_SRC or provide /tmp/mesa-pin")
    def test_pristine_chain_and_repeat_rejection(self):
        environment = os.environ.copy()
        environment.pop("NXBOX_MESA_SKIP", None)
        outputs = []
        for newline in (b"\n", b"\r\n"):
            with (
                self.subTest(newline=newline),
                tempfile.TemporaryDirectory(prefix="nxbox-mesa-chain-") as temporary,
            ):
                root = Path(temporary) / "mesa"
                shutil.copytree(SOURCE, root, ignore=shutil.ignore_patterns(".git"))
                # Simulate checkout line endings for all driver/winsys patch inputs.
                for directory in ("src/gallium/drivers/d3d12", "src/gallium/winsys"):
                    for path in (root / directory).rglob("*"):
                        if path.suffix in (".cpp", ".h"):
                            data = path.read_bytes().replace(b"\r\n", b"\n")
                            path.write_bytes(data.replace(b"\n", newline))
                command = [sys.executable, str(ROOT / "tools/nxbox/patch_mesa_uwp.py"), str(root)]
                result = subprocess.run(
                    command,
                    env=environment,
                    capture_output=True,
                    text=True,
                    timeout=60,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                driver = root / "src/gallium/drivers/d3d12"
                query = (driver / "d3d12_query.cpp").read_text()
                self.assertIn("return nxbox_query_ready(", query)
                self.assertNotIn("SetEventOnCompletion(query->fence_value, NULL)", query)
                self.assertTrue((driver / "nxbox_query_wait.h").is_file())
                outputs.append(
                    {path.name: path.read_text() for path in driver.iterdir() if path.is_file()}
                )
                before = snapshot(root)
                result = subprocess.run(
                    command,
                    env=environment,
                    capture_output=True,
                    text=True,
                    timeout=60,
                    check=False,
                )
                # Like the existing patches, the full chain rejects repeat application.
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(
                    "Pinned Mesa source does not match patch: volatile bool finished = false;",
                    result.stderr,
                )
                self.assertEqual(before, snapshot(root))
        self.assertEqual(outputs[0], outputs[1])


if __name__ == "__main__":
    unittest.main()
