# SPDX-License-Identifier: GPL-3.0-or-later
"""Check staging boundaries without a Windows SDK or signing secrets."""

import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("package_uwp", ROOT / "tools/nxbox/package_uwp.py")
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.exe = self.root / "candidate.exe"
        header = bytearray(0x100)
        header[:2] = b"MZ"
        struct.pack_into("<I", header, 0x3C, 0x80)
        header[0x80:0x86] = b"PE\0\0\x64\x86"
        self.exe.write_bytes(header)
        self.destination = self.root / "package"

    def tearDown(self):
        self.directory.cleanup()

    def test_stages_manifest_payload_and_only_runtime_libraries(self):
        (self.root / "prod.keys").write_text("do not package")
        (self.root / "game.nsp").write_text("do not package")
        (self.root / "runtime.dll").write_bytes(b"test library")
        package.stage(self.exe, self.destination, "0.1.3.0")
        self.assertFalse((self.destination / "prod.keys").exists())
        self.assertFalse((self.destination / "game.nsp").exists())
        self.assertTrue((self.destination / "runtime.dll").is_file())
        tree = ET.parse(self.destination / "AppxManifest.xml")
        identity = tree.find(f"{{{package.FOUNDATION}}}Identity")
        self.assertEqual(identity.attrib["Version"], "0.1.3.0")
        self.assertEqual(identity.attrib["Name"], "NSPX.NXbox")
        self.assertEqual((self.destination / "boot.nro").read_bytes()[16:20], b"NRO0")
        for name, size in [("StoreLogo", 50), ("Square44x44Logo", 44), ("Square150x150Logo", 150)]:
            data = (self.destination / "Assets" / f"{name}.png").read_bytes()
            self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
            self.assertEqual(struct.unpack(">II", data[16:24]), (size, size))

    def test_graphics_probe_has_separate_identity_and_license(self):
        (self.root / "Mesa-LICENSE.rst").write_text("Mesa license fixture")
        for name in ["DXC-LICENSE-MS.txt", "DXC-LICENSE-LLVM.txt"]:
            (self.root / name).write_text("DXC license fixture")
        package.stage(self.exe, self.destination, "0.1.4.0", "graphics")
        tree = ET.parse(self.destination / "AppxManifest.xml")
        identity = tree.find(f"{{{package.FOUNDATION}}}Identity")
        self.assertEqual(identity.attrib["Name"], "NSPX.NXbox.GraphicsProbe")
        self.assertFalse((self.destination / "boot.nro").exists())
        self.assertTrue((self.destination / "Notices/Mesa-LICENSE.rst").is_file())

    def test_game_payload_requires_graphics_runtime(self):
        with self.assertRaises(FileNotFoundError):
            package.stage(self.exe, self.destination, "0.2.0.0", "game")

    def test_game_payload_retains_emulator_identity_and_licenses(self):
        for name in ["opengl32.dll", "libgallium_wgl.dll", "libglapi.dll", "dxil.dll"]:
            (self.root / name).write_bytes(b"runtime fixture")
        for name in ["Mesa-LICENSE.rst", "DXC-LICENSE-MS.txt", "DXC-LICENSE-LLVM.txt"]:
            (self.root / name).write_text("license fixture")
        package.stage(self.exe, self.destination, "0.2.0.0", "game")
        tree = ET.parse(self.destination / "AppxManifest.xml")
        self.assertEqual(
            tree.find(f"{{{package.FOUNDATION}}}Identity").attrib["Name"], "NSPX.NXbox"
        )
        self.assertEqual((self.destination / "game.nro").read_bytes()[16:20], b"NRO0")
        self.assertFalse((self.destination / "boot.nro").exists())
        self.assertTrue((self.destination / "Notices/libnx-LICENSE.md").is_file())
        self.assertTrue((self.destination / "Notices/Mesa-LICENSE.rst").is_file())

    def test_rejects_stale_staging_directory(self):
        self.destination.mkdir()
        with self.assertRaises(FileExistsError):
            package.stage(self.exe, self.destination, "0.1.0.0")

    def test_rejects_invalid_or_injected_version(self):
        for version in ['0.1.0.0" bad="yes', "0.1", "0.1.65536.0", "-1.0.0.0"]:
            with self.subTest(version=version), self.assertRaises(ValueError):
                package.stage(self.exe, self.destination, version)
        self.assertFalse(self.destination.exists())

    def test_rejects_non_windows_binary(self):
        self.exe.write_bytes(b"\x7fELF" + bytes(256))
        with self.assertRaises(ValueError):
            package.stage(self.exe, self.destination, "0.1.0.0")
        self.assertFalse(self.destination.exists())

    def test_rejects_arm64_binary(self):
        data = bytearray(self.exe.read_bytes())
        data[0x84:0x86] = b"\x64\xaa"
        self.exe.write_bytes(data)
        with self.assertRaises(ValueError):
            package.stage(self.exe, self.destination, "0.1.0.0")


if __name__ == "__main__":
    unittest.main()
