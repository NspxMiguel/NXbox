#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stage runtime DLLs from a successful, trusted NXbox Mesa workflow."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def stage(run_id: str, output: Path) -> None:
    if not run_id.isdecimal():
        raise ValueError("Expected a numeric Mesa workflow run ID")
    repository = os.environ.get("GITHUB_REPOSITORY", "NspxMiguel/NXbox")
    response = subprocess.check_output(
        ["gh", "api", f"repos/{repository}/actions/runs/{run_id}"], text=True
    )
    run = json.loads(response)
    if (
        run["conclusion"] != "success"
        or run["head_branch"] != "main"
        or run["head_repository"]["full_name"] != repository
        or run["path"] != ".github/workflows/mesa-uwp.yml"
        or run["event"] != "workflow_dispatch"
    ):
        raise ValueError("Mesa runtime must come from a successful trusted main-branch build")
    with tempfile.TemporaryDirectory(prefix="nxbox-mesa-") as temporary:
        root = Path(temporary)
        subprocess.run(
            [
                "gh",
                "run",
                "download",
                run_id,
                "-R",
                repository,
                "-n",
                "nxbox-mesa-uwp",
                "-D",
                str(root),
            ],
            check=True,
        )
        libraries = list((root / "mesa-install").rglob("*.dll"))
        names = [library.name.lower() for library in libraries]
        required = {"opengl32.dll", "libgallium_wgl.dll", "libglapi.dll"}
        if not required.issubset(names) or len(names) != len(set(names)):
            raise ValueError("Mesa artifact has missing or ambiguous runtime DLLs")
        license_path = root / ".cache/mesa/docs/license.rst"
        if not license_path.is_file():
            raise ValueError("Mesa artifact is missing its license")
        output.mkdir(parents=True, exist_ok=True)
        for library in libraries:
            shutil.copy2(library, output / library.name)
        shutil.copy2(license_path, output / "Mesa-LICENSE.rst")
        (output / "mesa-build.json").write_text(
            json.dumps(
                {
                    "workflowRun": run_id,
                    "nxboxRevision": run["head_sha"],
                    "upstreamRevision": "15acdd7ea2b9dcdd62f26fe86b88280d79efc46b",
                },
                indent=2,
            )
            + "\n"
        )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", required=True)
    parser.add_argument("--output", required=True, type=Path)
    arguments = parser.parse_args()
    stage(arguments.run, arguments.output)
