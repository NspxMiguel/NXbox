# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch the pinned host shader compiler required by the UWP build."""

import hashlib
import os
from pathlib import Path
import shutil
import urllib.request
import zipfile

URL = "https://github.com/KhronosGroup/glslang/releases/download/16.6.0/glslang-16.6.0-windows-x86_64-release.zip"
SHA256 = "82bf434e69b9bb4829de7e2b4bc2c5e7a7861e53d66cf75e5cc70f5f694a8d9b"


def main():
    destination = Path("build-host-tools/glslang").resolve()
    destination.mkdir(parents=True, exist_ok=True)
    archive = destination / "glslang.zip"
    urllib.request.urlretrieve(URL, archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise RuntimeError("Shader compiler checksum mismatch")
    with zipfile.ZipFile(archive) as bundle:
        for member in bundle.infolist():
            if (
                not (destination / member.filename)
                .resolve()
                .is_relative_to(destination)
            ):
                raise RuntimeError("Invalid archive member path")
        bundle.extractall(destination)
    candidates = list(destination.rglob("glslangValidator.exe"))
    if not candidates:
        compiler = next(destination.rglob("glslang.exe"))
        alias = compiler.with_name("glslangValidator.exe")
        shutil.copy2(compiler, alias)
        candidates = [alias]
    compiler = candidates[0]
    if "GITHUB_PATH" in os.environ:
        with open(os.environ["GITHUB_PATH"], "a", encoding="utf-8") as output:
            output.write(str(compiler.parent) + "\n")
    print(f"Verified glslang 16.6.0: {compiler}")


if __name__ == "__main__":
    main()
