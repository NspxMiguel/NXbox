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
                resource = (driver / "d3d12_resource.cpp").read_text()
                self.assertIn("desc.Width = ALIGN(desc.Width, util_format_get_blockwidth", resource)
                self.assertIn("nxbox_bc_copy_end", (driver / "nxbox_sync_batch.h").read_text())
                context = (driver / "d3d12_context.cpp").read_text()
                self.assertNotIn("D3D12_RESOURCE_BARRIER_TYPE_ALIASING", context)
                self.assertIn("TEXTURE_BARRIER submit-and-wait", context)
                draw = (driver / "d3d12_draw.cpp").read_text()
                self.assertIn("RTV_FORMAT_MISMATCH", draw)
                self.assertIn("RTV_BIND slot=%u view=%u res=%s", draw)
                query = (driver / "d3d12_query.cpp").read_text()
                self.assertIn("return nxbox_query_ready(", query)
                self.assertNotIn("SetEventOnCompletion(query->fence_value, NULL)", query)
                self.assertTrue((driver / "nxbox_query_wait.h").is_file())
                batch = (driver / "d3d12_batch.cpp").read_text()
                submission = batch.split(
                    "screen->cmdqueue->ExecuteCommandLists(count_to_execute, to_execute);", 1
                )[1]
                sequence = [
                    "screen->cmdqueue->Signal(screen->fence, target)",
                    "nxbox_sync_wait(screen->dev, screen->fence, target)",
                    "const HRESULT after_wait = screen->dev->GetDeviceRemovedReason()",
                    "nxbox_sync_completed(screen->dev)",
                    'nxbox_observe(screen->dev, "execute-return")',
                    "batch->fence = d3d12_create_fence(screen)",
                ]
                positions = [submission.index(item) for item in sequence]
                self.assertEqual(positions, sorted(positions))
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
