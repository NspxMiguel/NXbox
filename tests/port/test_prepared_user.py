# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise offline account policy and real Mii serialization without the emulator runtime."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MII = ROOT / "src/core/hle/service/mii"


class PreparedUserTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="nxbox-prepared-build-")
        cls.addClassCleanup(cls.build.cleanup)
        cls.binary = Path(cls.build.name) / "prepared-user"
        sources = [
            ROOT / "tests/port/prepared_user.cpp",
            *[
                MII / name
                for name in (
                    "mii_database.cpp",
                    "mii_database_manager.cpp",
                    "mii_manager.cpp",
                    "types/core_data.cpp",
                    "types/store_data.cpp",
                    "types/char_info.cpp",
                    "types/raw_data.cpp",
                    "types/ver3_store_data.cpp",
                )
            ],
        ]
        subprocess.run(
            [
                "clang++",
                "-std=c++20",
                "-DNXBOX_UWP",
                "-I",
                str(ROOT / "tests/port/fixtures/mii_host"),
                "-I",
                str(ROOT / "src"),
                *map(str, sources),
                "-o",
                str(cls.binary),
            ],
            check=True,
            capture_output=True,
            text=True,
        )

    def test_other_frontends_default_to_unlinked(self):
        source = Path(self.build.name) / "desktop-policy.cpp"
        source.write_text(
            "#include <cassert>\n"
            '#include "core/hle/service/acc/offline_account.h"\n'
            "int main() { using namespace Service::Account::Offline; "
            "assert(IsFakeLinkEnabled() == "
            'FakeLinkEnabled(std::getenv("NXBOX_FAKE_NA_LINK"), false)); }\n'
        )
        binary = Path(self.build.name) / "desktop-policy"
        subprocess.run(
            [
                "clang++",
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "src"),
                str(source),
                "-o",
                str(binary),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        for switch in (None, "0", "1"):
            environment = dict(os.environ)
            environment.pop("NXBOX_FAKE_NA_LINK", None)
            if switch is not None:
                environment["NXBOX_FAKE_NA_LINK"] = switch
            subprocess.run([str(binary)], check=True, env=environment, timeout=10)

    def test_prepared_user_and_mii_persistence(self):
        for switch in (None, "0", "1"):
            with (
                self.subTest(fake_link=switch),
                tempfile.TemporaryDirectory(prefix="nxbox-prepared-nand-") as nand,
            ):
                environment = dict(os.environ)
                environment.pop("NXBOX_FAKE_NA_LINK", None)
                if switch is not None:
                    environment["NXBOX_FAKE_NA_LINK"] = switch
                subprocess.run([str(self.binary), nand], check=True, env=environment, timeout=10)


if __name__ == "__main__":
    unittest.main()
