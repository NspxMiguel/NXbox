# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the shared UI transition fence without WinRT or a controller."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class InputStateTests(unittest.TestCase):
    def test_screen_transitions_and_button_edges(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-input-") as directory:
            binary = Path(directory) / "input-state-tests"
            subprocess.run(
                [
                    "clang++",
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pedantic",
                    "-I",
                    str(ROOT / "src"),
                    str(ROOT / "tests/port/input_state.cpp"),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_library_transition_fences_are_wired(self):
        source = (ROOT / "src/eden_uwp/ui/library_screen.cpp").read_text()
        for name in ("OpenMods", "OpenUsbImport", "OpenSources", "OpenCredits"):
            with self.subTest(screen=name):
                body = source.split(f"    void {name}() {{", 1)[1].split("\n    }", 1)[0]
                self.assertIn("const InputTransition transition(input_);", body)
        handler = source.split("    void HandleInput(", 1)[1].split("\n    }", 1)[0]
        for overlay in ("details", "resolution", "update"):
            with self.subTest(overlay=overlay):
                self.assertRegex(
                    handler,
                    rf"{overlay}_open_ = false;\s+input_\.ConsumeUntilRelease\(\);",
                )
        self.assertIn("OpenMods();\n            return;", handler)
        self.assertIn("Activate(now);\n            return;", handler)
        for call in ("usb_detection_->Run", "RunSignIn"):
            before = source.split(call, 1)[0]
            self.assertIn("const InputTransition transition(input_);", before[-200:])

    def test_empty_library_selection_cannot_boot_remembered_game(self):
        source = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
        after = source.split("chosen = Ui::RunLibrary(window, &choice);", 1)[1]
        self.assertRegex(after, r"^\s+if \(chosen\.empty\(\)\) \{\s+return;")


if __name__ == "__main__":
    unittest.main()
