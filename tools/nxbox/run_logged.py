#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stream build output to the CI console and retain a complete local log."""

from pathlib import Path
import subprocess
import sys


def run(log: Path, command: list[str]) -> int:
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("wb") as output:
        with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as process:
            for line in process.stdout:
                output.write(line)
                output.flush()
                sys.stdout.buffer.write(line)
                sys.stdout.buffer.flush()
            return process.wait()


if __name__ == "__main__":
    sys.exit(run(Path(sys.argv[1]), sys.argv[2:]))
