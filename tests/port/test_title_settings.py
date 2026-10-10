# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile and exercise per-title settings parsing and persistence on the host."""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

DRIVER = r"""
#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include "eden_uwp/title_settings.h"
using namespace EdenXbox::TitleSettings;
int main(int argc, char** argv) {
    assert(argc >= 2);
    const std::string test = argv[1];
    if (test == "lines") {
        for (const auto line : {"", "  ", " # resolution_setup=7", "no equals", " =6"})
            assert(!ParseLine(line));
        const auto entry = ParseLine(" \tresolution_setup = 5 \r");
        assert(entry && entry->label == "resolution_setup" && entry->value == "5");
        const auto string = ParseLine("name=a=b");
        assert(string && string->value == "a=b");
        assert(ParseLine("name=")->value.empty());
    } else if (test == "values") {
        for (int value = 0; value <= 12; ++value)
            assert(ParseResolution(std::to_string(value)) == value);
        assert(ParseResolution(" 6\t\r") == 6);
        for (const auto value : {"", "-1", "+6", "13", "9999999999999999999999", "6junk",
                                 "1.5", "6=7", "6#comment"})
            assert(!ParseResolution(value));
    } else if (test == "precedence") {
        std::istringstream empty;
        assert(ReadResolution(empty) == 3);
        std::istringstream global("resolution_setup=6\n");
        int value = ReadResolution(global);
        std::istringstream title("# comment\r\nunknown=value\r\nresolution_setup=5\r\n");
        value = ReadResolution(title, value);
        assert(value == 5);
        std::istringstream duplicates("resolution_setup=7\nresolution_setup=bad\n");
        assert(ReadResolution(duplicates, value) == 7);
        std::istringstream missing;
        assert(ReadResolution(missing, value) == 5);
        std::istringstream next_global;
        std::istringstream next_title;
        assert(ReadResolution(next_title, ReadResolution(next_global)) == 3);
    } else if (test == "paths") {
        const std::filesystem::path local("local");
        assert(File(local, "01007ef00011e000") ==
               local / "settings" / "01007EF00011E000.txt");
        for (const auto title : {"", "../other", "01007EF00011E00", "01007EF00011E0000",
                                 "01007EF00011E00/", "01007EF00011E00G"})
            assert(File(local, title).empty());
    } else if (test == "preservation") {
        const std::string content = "# comment\r\nunknown=a=b\r\nresolution_setup=6\r\n"
                                    "\r\n resolution_setup = 5\r\nother=7";
        const auto updated = WithResolution(content, 7);
        assert(updated == "# comment\r\nunknown=a=b\r\n\r\nother=7\nresolution_setup=7\n");
        assert(WithResolution(updated, 3).find("resolution_setup=7") == std::string::npos);
    } else if (test == "persistence") {
        assert(argc == 3);
        const std::filesystem::path local(argv[2]);
        const auto botw = File(local, "01007EF00011E000");
        const auto mario = File(local, "010028600EBDA000");
        assert(SaveResolution(botw, 5));
        assert(SaveResolution(mario, 7));
        { std::ofstream out(botw, std::ios::app); out << "# keep\nother=value\n"; }
        for (const int value : kResolutionValues) {
            assert(SaveResolution(botw, value));
            std::ifstream in(botw);
            assert(ReadResolution(in) == value);
        }
        std::ifstream in(botw);
        const std::string body{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        assert(body.find("# keep\nother=value\n") != std::string::npos);
        std::ifstream other(mario);
        assert(ReadResolution(other) == 7);
        assert(!SaveResolution(botw, 999));
        assert(!SaveResolution({}, 3));
        const auto blocked = local / "blocked";
        { std::ofstream out(blocked); out << "file"; }
        assert(!SaveResolution(blocked / "settings.txt", 3));
        assert(!SaveResolution(local, 3));
    } else {
        assert(false);
    }
}
"""


class TitleSettingsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="nxbox-title-settings-")
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(cls.temp.name) / "driver.cpp"
        cls.binary = Path(cls.temp.name) / "driver"
        source.write_text(DRIVER)
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
                str(cls.binary),
            ],
            check=True,
            capture_output=True,
            text=True,
        )

    def test_enum_values_match_eden(self):
        enums = (ROOT / "src/common/settings_enums.h").read_text()
        values = re.search(r"ENUM\(ResolutionSetup,\s*([^;]+)\);", enums).group(1).split(",")
        names = [value.strip() for value in values]
        self.assertEqual(
            [names.index(name) for name in ("Res1X", "Res3_2X", "Res2X", "Res3X")], [3, 5, 6, 7]
        )
        self.assertEqual(len(names), 13)


def make_test(case):
    def test(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-title-files-") as local:
            subprocess.run([str(self.binary), case, local], check=True, timeout=10)

    return test


for case in ("lines", "values", "precedence", "paths", "preservation", "persistence"):
    setattr(TitleSettingsTests, f"test_{case}", make_test(case))

if __name__ == "__main__":
    unittest.main()
