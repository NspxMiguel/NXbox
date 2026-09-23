# SPDX-License-Identifier: GPL-3.0-or-later
"""Stage the Microsoft shader validator and its distribution notices."""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import urllib.request
import zipfile

URL = "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2607/dxc_2026_07_29.zip"
SHA256 = "a1dfb116ba3eeae6a1582291b53a8e7bf65ad760676bd3194685c8f7367cd241"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    archive = args.output / "dxc.zip"
    urllib.request.urlretrieve(URL, archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise RuntimeError("DXC release checksum mismatch")
    with zipfile.ZipFile(archive) as bundle:
        (args.output / "dxil.dll").write_bytes(bundle.read("bin\\x64\\dxil.dll"))
        for name in ["LICENSE-MS.txt", "LICENSE-LLVM.txt"]:
            (args.output / f"DXC-{name}").write_bytes(bundle.read(name))
    archive.unlink()
    # The SDK redistributable is built for the Windows platform API surface.
    # Prefer it over the desktop CRT build when the Windows SDK provides one.
    sdk = (
        Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
        / "Windows Kits/10/Redist"
    )
    candidates = sorted(p for p in sdk.rglob("dxil.dll") if "x64" in p.parts)
    if candidates:
        shutil.copy2(candidates[-1], args.output / "dxil.dll")
        print(f"Using Windows SDK shader validator: {candidates[-1]}")
    else:
        print("Using checksum-pinned DXC 1.9.2607 shader validator")
    print("DXIL SHA256:", hashlib.sha256((args.output / "dxil.dll").read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
