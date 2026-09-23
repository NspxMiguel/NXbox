# SPDX-License-Identifier: GPL-3.0-or-later
"""Stage and sign a minimal UWP guest-CPU diagnostic package."""

import argparse
import base64
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import xml.etree.ElementTree as ET
import zlib

ROOT = Path(__file__).resolve().parents[2]
FOUNDATION = "http://schemas.microsoft.com/appx/manifest/foundation/windows10"


def write_icon(path: Path, size: int):
    """Rasterize the code-defined NX monogram for required package tile sizes."""
    palette = json.loads((ROOT / "dist/nxbox/tokens.json").read_text())
    glyphs = [
        (0.20, ["10001", "11001", "10101", "10011", "10001"]),
        (0.54, ["10001", "01010", "00100", "01010", "10001"]),
    ]
    rows = bytearray()
    for y in range(size):
        rows.append(0)
        for x in range(size):
            color = palette["background"]
            for index, (left, glyph) in enumerate(glyphs):
                gx = int((x / size - left) / 0.052)
                gy = int((y / size - 0.34) / 0.064)
                if left <= x / size < left + 0.26 and 0.34 <= y / size < 0.66:
                    if 0 <= gx < 5 and 0 <= gy < 5 and glyph[gy][gx] == "1":
                        color = palette["foreground" if index == 0 else "accent"]
            rows.extend(color)

    def chunk(kind, payload):
        return (
            struct.pack(">I", len(payload))
            + kind
            + payload
            + struct.pack(">I", zlib.crc32(kind + payload))
        )

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(rows))
        + chunk(b"IEND", b"")
    )


def stage(executable: Path, destination: Path, version: str, kind: str = "cpu"):
    if kind not in {"cpu", "graphics", "game"}:
        raise ValueError("Unknown diagnostic kind")
    if not re.fullmatch(r"\d+\.\d+\.\d+\.\d+", version) or any(
        int(p) > 65535 for p in version.split(".")
    ):
        raise ValueError("Package version must contain four integers between 0 and 65535")
    with executable.open("rb") as source:
        header = source.read(64)
        if len(header) != 64 or header[:2] != b"MZ":
            raise ValueError("Expected a Windows x64 PE executable")
        source.seek(struct.unpack_from("<I", header, 0x3C)[0])
        pe_header = source.read(6)
        if pe_header != b"PE\0\0\x64\x86":
            raise ValueError("Expected a Windows x64 PE executable")
    if destination.exists():
        raise FileExistsError("Use a new staging directory to avoid packaging stale files")
    payload = ROOT / "homebrew/jit-smoke/fixtures/boot.nro"
    data = payload.read_bytes()
    if data[16:20] != b"NRO0" or b"EDEN_XBOX_JIT_ALIVE" not in data:
        raise ValueError("Guest smoke-test fixture is invalid")
    destination.mkdir(parents=True)
    shutil.copy2(executable, destination / "nxbox.exe")
    # Copy runtime libraries only from the actual executable's output directory.
    for library in executable.parent.glob("*.dll"):
        shutil.copy2(library, destination / library.name)
    if kind == "cpu":
        shutil.copy2(payload, destination / "boot.nro")
    elif kind == "game":
        game = ROOT / "homebrew/paddle-test/fixtures/game.nro"
        if game.read_bytes()[16:20] != b"NRO0":
            raise ValueError("Guest game fixture is invalid")
        for name in ("opengl32.dll", "libgallium_wgl.dll", "libglapi.dll", "dxil.dll"):
            if not (destination / name).is_file():
                raise FileNotFoundError(f"Missing game runtime: {name}")
        shutil.copy2(game, destination / "game.nro")
    for name, size in [
        ("StoreLogo", 50),
        ("Square44x44Logo", 44),
        ("Square150x150Logo", 150),
    ]:
        write_icon(destination / "Assets" / f"{name}.png", size)
    manifest = (ROOT / "dist/nxbox/AppxManifest.xml").read_text()
    manifest = manifest.replace('Version="0.1.0.0"', f'Version="{version}"')
    if kind == "graphics":
        manifest = manifest.replace("NSPX.NXbox", "NSPX.NXbox.GraphicsProbe")
        manifest = manifest.replace("NXbox.App", "NXbox.GraphicsProbe")
        manifest = manifest.replace(">NXbox<", ">NXbox Graphics Probe<")
        manifest = manifest.replace('DisplayName="NXbox"', 'DisplayName="NXbox Graphics Probe"')
    ET.fromstring(manifest)
    (destination / "AppxManifest.xml").write_text(manifest, encoding="utf-8")
    notices = destination / "Notices"
    notices.mkdir()
    shutil.copy2(ROOT / "LICENSE.txt", notices / "Eden-LICENSE.txt")
    if kind in {"cpu", "game"}:
        shutil.copy2(ROOT / "homebrew/jit-smoke/LICENSE.libnx.md", notices / "libnx-LICENSE.md")
    if kind in {"graphics", "game"}:
        shutil.copy2(executable.parent / "Mesa-LICENSE.rst", notices / "Mesa-LICENSE.rst")
        for name in ["DXC-LICENSE-MS.txt", "DXC-LICENSE-LLVM.txt"]:
            shutil.copy2(executable.parent / name, notices / name)


def sdk_tool(name: str) -> Path:
    base = (
        Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Windows Kits/10/bin"
    )
    candidates = sorted(
        base.glob(f"10.*/x64/{name}.exe"),
        key=lambda p: tuple(map(int, p.parents[1].name.split("."))),
    )
    if not candidates:
        raise FileNotFoundError(f"Windows SDK tool not installed: {name}")
    return candidates[-1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--version", default="0.1.0.0")
    parser.add_argument("--output", type=Path, default=Path("out"))
    parser.add_argument("--stage-only", action="store_true")
    parser.add_argument("--kind", choices=["cpu", "graphics", "game"], default="cpu")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    staging = args.output / "package"
    stage(args.exe, staging, args.version, args.kind)
    if args.stage_only:
        print(f"Staged NXbox {args.version}: {staging}")
        return
    package = (args.output / f"NXbox_{args.kind}_{args.version}_x64.appx").resolve()
    subprocess.run(
        [
            str(sdk_tool("makeappx")),
            "pack",
            "/d",
            str(staging.resolve()),
            "/p",
            str(package),
            "/o",
        ],
        check=True,
    )
    pfx_data = os.environ.get("NXBOX_PFX_BASE64")
    password = os.environ.get("NXBOX_PFX_PASSWORD")
    if not pfx_data or not password:
        raise RuntimeError("Stable NXbox signing secrets must be configured before distribution")
    with tempfile.TemporaryDirectory() as private:
        certificate = Path(private) / "signing.pfx"
        certificate.write_bytes(base64.b64decode(pfx_data, validate=True))
        subprocess.run(
            [
                str(sdk_tool("signtool")),
                "sign",
                "/fd",
                "SHA256",
                "/f",
                str(certificate),
                "/p",
                password,
                str(package),
            ],
            check=True,
        )
    print(f"Signed package: {package}")


if __name__ == "__main__":
    main()
