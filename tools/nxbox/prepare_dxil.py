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
        validator = next(
            member
            for member in bundle.infolist()
            if member.filename.replace("\\", "/") == "bin/x64/dxil.dll"
        )
        (args.output / "dxil.dll").write_bytes(bundle.read(validator))
        # dxcompiler.dll only gives Mesa readable validation errors; it is optional at runtime.
        compiler = next(
            member
            for member in bundle.infolist()
            if member.filename.replace("\\", "/") == "bin/x64/dxcompiler.dll"
        )
        (args.output / "dxcompiler.dll").write_bytes(bundle.read(compiler))
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
    # The DXC release validator imports the desktop CRT and fails to load in the package
    # (error 126), so the SDK validator stays the default; NXBOX_DXIL_SDK=0 forces DXC.
    if candidates and os.environ.get("NXBOX_DXIL_SDK") != "0":
        shutil.copy2(candidates[-1], args.output / "dxil.dll")
        print(f"Using Windows SDK shader validator: {candidates[-1]}")
    else:
        print("Using checksum-pinned DXC 1.9.2607 shader validator")
    # dxcompiler.dll (diagnostics only) needs the desktop C++ runtime; ship it app-local.
    vs = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Microsoft Visual Studio"
    crt_dirs = sorted(
        p for p in vs.glob("*/*/VC/Redist/MSVC/*/x64/Microsoft.VC14*.CRT") if p.is_dir()
    )
    if crt_dirs:
        for name in ["msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"]:
            if (crt_dirs[-1] / name).exists():
                shutil.copy2(crt_dirs[-1] / name, args.output / name)
        print(f"Staged the C++ runtime from {crt_dirs[-1]}")
    else:
        print("No Visual C++ runtime found; dxcompiler.dll will not load")
    print("DXIL SHA256:", hashlib.sha256((args.output / "dxil.dll").read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
