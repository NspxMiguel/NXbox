# SPDX-License-Identifier: GPL-3.0-or-later
"""Release version and rollback checks, without Windows or GitHub access."""

import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "release_version", ROOT / "tools/nxbox/release_version.py"
)
release_version = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release_version)


class ReleaseVersionTests(unittest.TestCase):
    def test_three_and_four_components(self):
        self.assertEqual(release_version.validate_version("v1.2.3", []), "1.2.3.0")
        self.assertEqual(release_version.validate_version("v1.2.3.4", []), "1.2.3.4")

    def test_invalid_or_diagnostic_versions(self):
        for tag in [
            "v0.3.999",
            "1.2.3",
            "v1.2",
            "v1.2.3-beta",
            "v1.2.3.",
            "v1.2.3.4.5",
            "v65536.0.0",
            "v1.-2.3",
            "v1.2.3\n",
        ]:
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                release_version.parse_version(tag)

    def test_component_bounds(self):
        self.assertEqual(release_version.parse_version("v65535.65535.65535.65535"), (65535,) * 4)

    def test_compares_numerically_across_all_pages(self):
        pages = [[self.release("v1.9.0")], [self.release("v1.10.0")]]
        self.assertEqual(release_version.validate_version("v1.11.0", pages), "1.11.0.0")
        with self.assertRaises(ValueError):
            release_version.validate_version("v1.9.1", pages)

    def test_equal_or_older_release_is_rejected(self):
        pages = [[self.release("v2.0.0.1")]]
        for tag in ["v2.0.0.1", "v2.0.0", "v1.99.99"]:
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                release_version.validate_version(tag, pages)

    def test_drafts_prereleases_and_legacy_tags_do_not_block(self):
        pages = [
            [
                self.release("v9.0.0", draft=True),
                self.release("v8.0.0", prerelease=True),
                self.release("diagnostic-10"),
            ]
        ]
        self.assertEqual(release_version.validate_version("v1.0.0", pages), "1.0.0.0")

    def test_normalized_tag_cannot_republish_same_version(self):
        with self.assertRaises(ValueError):
            release_version.validate_version("v01.2.3.0", [[self.release("v1.2.3")]])

    @staticmethod
    def release(tag, draft=False, prerelease=False):
        return {"tag_name": tag, "draft": draft, "prerelease": prerelease}


if __name__ == "__main__":
    unittest.main()
