# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the per-game launcher tile packaging without a Windows SDK or signing secrets."""

import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
try:
    from PIL import Image
except ImportError:  # the CI step installs Pillow; a bare host skips these tests
    Image = None

launcher = None
if Image is not None:
    spec = importlib.util.spec_from_file_location(
        "package_launcher", ROOT / "tools/nxbox/package_launcher.py"
    )
    launcher = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(launcher)

UAP = "http://schemas.microsoft.com/appx/manifest/uap/windows10"
TITLE = "01007ef00011e000"


@unittest.skipIf(Image is None, "Pillow is not installed")
class LauncherTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.exe = self.root / "nxbox-launcher.exe"
        header = bytearray(0x100)
        header[:2] = b"MZ"
        struct.pack_into("<I", header, 0x3C, 0x80)
        header[0x80:0x86] = b"PE\0\0\x64\x86"
        self.exe.write_bytes(header)
        self.destination = self.root / "package"

    def tearDown(self):
        self.directory.cleanup()

    def test_validates_title_id_and_name(self):
        self.assertEqual(launcher.validate_title_id(TITLE), TITLE.upper())
        for bad in ["", "0100", TITLE + "0", "01007EF00011E00G", "../../etc/passwd0000"]:
            with self.assertRaises(ValueError):
                launcher.validate_title_id(bad)
        self.assertEqual(launcher.validate_name("  Zelda   BotW "), "Zelda BotW")
        for bad in ["", "   ", "x" * 257, "bell\x07"]:
            with self.assertRaises(ValueError):
                launcher.validate_name(bad)

    def test_art_urls_come_from_the_index(self):
        index = {"base": "https://img.example/i/", "ext": ".jpg", "titles": {"AAAA": ["b1", "i1"]}}
        self.assertEqual(
            launcher.art_urls(index, "AAAA"),
            ("https://img.example/i/b1.jpg", "https://img.example/i/i1.jpg"),
        )
        self.assertEqual(launcher.art_urls(index, "BBBB"), (None, None))

    def test_repository_index_resolves_a_known_title(self):
        import json

        index = json.loads(launcher.ART_INDEX.read_text())
        title = next(iter(index["titles"]))
        banner, icon = launcher.art_urls(index, title)
        self.assertTrue(banner.startswith(index["base"]) and banner.endswith(index["ext"]))
        self.assertTrue(icon.startswith(index["base"]))

    def test_manifest_identity_name_and_escaping(self):
        manifest = launcher.build_manifest(TITLE, 'Link & "Zelda" <3', "1.0.0.0")
        tree = ET.fromstring(manifest)
        foundation = launcher.package_uwp.FOUNDATION
        identity = tree.find(f"{{{foundation}}}Identity")
        self.assertEqual(identity.attrib["Name"], "NSPX.NXbox.Game.01007EF00011E000")
        nxbox = ET.parse(ROOT / "dist/nxbox/AppxManifest.xml").getroot()
        self.assertEqual(
            identity.attrib["Publisher"], nxbox.find(f"{{{foundation}}}Identity").attrib["Publisher"]
        )
        self.assertEqual(
            tree.find(f"{{{foundation}}}Properties/{{{foundation}}}DisplayName").text,
            'Link & "Zelda" <3',
        )
        visual = tree.find(f".//{{{UAP}}}VisualElements")
        self.assertEqual(visual.attrib["DisplayName"], 'Link & "Zelda" <3')
        self.assertIsNotNone(visual.find(f"{{{UAP}}}DefaultTile"))
        self.assertIsNotNone(visual.find(f"{{{UAP}}}SplashScreen"))
        self.assertIsNotNone(tree.find(f"{{{foundation}}}Dependencies/{{{foundation}}}PackageDependency"))
        # The launcher never owns the protocol; only NXbox does.
        self.assertNotIn("windows.protocol", manifest)

    def test_nxbox_manifest_registers_the_protocol(self):
        tree = ET.parse(ROOT / "dist/nxbox/AppxManifest.xml")
        protocols = [p.attrib["Name"] for p in tree.iter(f"{{{UAP}}}Protocol")]
        self.assertEqual(protocols, ["nxbox"])

    def test_stage_writes_title_art_and_manifest_with_fallback_art(self):
        launcher.stage(self.exe, self.destination, TITLE, "Zelda", "1.0.0.0", None, None)
        self.assertEqual((self.destination / "title.txt").read_text(), "01007EF00011E000\n")
        self.assertTrue((self.destination / "nxbox-launcher.exe").is_file())
        for name, size in launcher.ASSET_SIZES.items():
            with Image.open(self.destination / "Assets" / f"{name}.png") as image:
                self.assertEqual(image.size, size)
        ET.parse(self.destination / "AppxManifest.xml")

    def test_stage_uses_given_art_and_refuses_a_reused_directory(self):
        icon = Image.new("RGBA", (300, 300), (255, 0, 0, 255))
        banner = Image.new("RGBA", (1920, 1080), (0, 0, 255, 255))
        launcher.stage(self.exe, self.destination, TITLE, "Zelda", "1.0.0.0", icon, banner)
        with Image.open(self.destination / "Assets/Square150x150Logo.png") as image:
            self.assertEqual(image.convert("RGB").getpixel((300, 300)), (255, 0, 0))
        with Image.open(self.destination / "Assets/Wide310x150Logo.png") as image:
            self.assertEqual(image.convert("RGB").getpixel((620, 300)), (0, 0, 255))
        with self.assertRaises(FileExistsError):
            launcher.stage(self.exe, self.destination, TITLE, "Zelda", "1.0.0.0", icon, banner)

    def test_rejects_a_non_pe_launcher_and_bad_version(self):
        bad = self.root / "bad.exe"
        bad.write_bytes(b"not an exe" * 20)
        with self.assertRaises(ValueError):
            launcher.stage(bad, self.destination, TITLE, "Zelda", "1.0.0.0", None, None)
        with self.assertRaises(ValueError):
            launcher.build_manifest(TITLE, "Zelda", "1.0.0")
        with self.assertRaises(ValueError):
            launcher.build_manifest(TITLE, "Zelda", "1.0.0.70000")

    def test_fit_cover_crops_to_the_exact_size(self):
        wide = Image.new("RGBA", (1920, 1080), (10, 20, 30, 255))
        self.assertEqual(launcher.fit_cover(wide, (600, 600)).size, (600, 600))
        self.assertEqual(launcher.fit_contain(wide, (600, 600)).size, (600, 600))


if __name__ == "__main__":
    unittest.main()
