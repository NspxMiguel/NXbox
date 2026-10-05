# SPDX-License-Identifier: GPL-3.0-or-later
"""Build, sign and stage a per-game launcher tile ("Install as game").

The package is the tiny nxbox-launcher.exe (src/nxbox_launcher) plus title.txt, a display name and
art generated from the eShop art index (dist/art/eshop-art.json). Launching the tile opens
nxbox://play?title=<ID>, which NXbox handles by booting that library game.
"""

import argparse
import base64
import io
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import urllib.request
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape, quoteattr

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import package_uwp  # noqa: E402  (shared SDK tool lookup)

ROOT = Path(__file__).resolve().parents[2]
ART_INDEX = ROOT / "dist/art/eshop-art.json"
MAX_IMAGE_BYTES = 16 << 20

# Shipped at 4x their nominal size, like dist/nxbox/Assets, so the dashboard shows them sharp.
ASSET_SIZES = {
    "StoreLogo": (200, 200),
    "Square44x44Logo": (176, 176),
    "Square150x150Logo": (600, 600),
    "Wide310x150Logo": (1240, 600),
    "SplashScreen": (2480, 1200),
}
BACKGROUND = (0, 0, 0, 255)  # Match the loading screen.


def validate_title_id(title_id: str) -> str:
    if not re.fullmatch(r"[0-9A-Fa-f]{16}", title_id):
        raise ValueError("Title ID must be 16 hexadecimal digits")
    return title_id.upper()


def validate_name(name: str) -> str:
    name = " ".join(name.split())
    if not name or len(name) > 256 or any(ord(c) < 32 for c in name):
        raise ValueError("Display name must be 1 to 256 printable characters")
    return name


def art_urls(index: dict, title_id: str):
    """(banner_url, icon_url) of a title from the eShop art index; None for a missing one."""
    entry = index.get("titles", {}).get(title_id)
    if not entry:
        return None, None
    base, ext = index["base"], index["ext"]
    banner = base + entry[0] + ext if len(entry) > 0 and entry[0] else None
    icon = base + entry[1] + ext if len(entry) > 1 and entry[1] else None
    return banner, icon


def download(url: str) -> Image.Image:
    if not url.startswith("https://"):
        raise ValueError("Art must be downloaded over https")
    request = urllib.request.Request(url, headers={"User-Agent": "nxbox-package-launcher"})
    with urllib.request.urlopen(request, timeout=30) as response:
        data = response.read(MAX_IMAGE_BYTES + 1)
    if len(data) > MAX_IMAGE_BYTES:
        raise ValueError("Art image is too large")
    return Image.open(io.BytesIO(data))


def fit_cover(image: Image.Image, size) -> Image.Image:
    """Scales `image` to cover `size` and crops the overflow around the center."""
    width, height = size
    image = image.convert("RGBA")
    scale = max(width / image.width, height / image.height)
    resized = image.resize(
        (max(width, round(image.width * scale)), max(height, round(image.height * scale))),
        Image.LANCZOS,
    )
    left = (resized.width - width) // 2
    top = (resized.height - height) // 2
    return resized.crop((left, top, left + width, top + height))


def fit_contain(image: Image.Image, size) -> Image.Image:
    """Centers `image` inside `size` on the NXbox background, keeping its aspect ratio."""
    width, height = size
    image = image.convert("RGBA")
    scale = min(width / image.width, height / image.height)
    resized = image.resize((round(image.width * scale), round(image.height * scale)), Image.LANCZOS)
    canvas = Image.new("RGBA", size, BACKGROUND)
    canvas.alpha_composite(resized, ((width - resized.width) // 2, (height - resized.height) // 2))
    return canvas


def render_assets(icon, banner, destination: Path):
    """Writes the five tile PNGs. Either image may be None; NXbox's own logo is the fallback."""
    if icon is None:
        icon = Image.open(ROOT / "dist/nxbox/Assets/Square150x150Logo.png")
    wide_source = banner if banner is not None else None
    destination.mkdir(parents=True, exist_ok=True)
    for name, size in ASSET_SIZES.items():
        if size[0] == size[1]:
            image = fit_cover(icon, size)
        elif wide_source is not None:
            image = fit_cover(wide_source, size)
        else:
            image = fit_contain(icon, size)
        image.save(destination / f"{name}.png", "PNG")


def build_manifest(title_id: str, name: str, version: str) -> str:
    """The launcher's AppxManifest.xml, derived from NXbox's own (same publisher, target family
    and VCLibs dependency, so it installs and runs next to NXbox as a UWP app)."""
    title_id = validate_title_id(title_id)
    name = validate_name(name)
    if not re.fullmatch(r"\d+\.\d+\.\d+\.\d+", version) or any(
        int(p) > 65535 for p in version.split(".")
    ):
        raise ValueError("Package version must contain four integers between 0 and 65535")
    template = (ROOT / "dist/nxbox/AppxManifest.xml").read_text()
    publisher = re.search(r'Publisher="([^"]+)"', template).group(1)
    dependencies = re.search(r"  <Dependencies>.*?</Dependencies>\n", template, re.S).group(0)
    resources = re.search(r"  <Resources>.*?</Resources>\n", template, re.S).group(0)
    display = quoteattr(name)
    return f"""<?xml version="1.0" encoding="utf-8"?>
<Package xmlns="{package_uwp.FOUNDATION}"
         xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"
         IgnorableNamespaces="uap">
  <Identity Name="NSPX.NXbox.Game.{title_id}" Publisher="{publisher}" Version="{version}" ProcessorArchitecture="x64" />
  <Properties>
    <DisplayName>{escape(name)}</DisplayName>
    <PublisherDisplayName>NSPX</PublisherDisplayName>
    <Logo>Assets\\StoreLogo.png</Logo>
  </Properties>
{dependencies}{resources}  <Applications>
    <Application Id="App" Executable="nxbox-launcher.exe" EntryPoint="NXbox.GameLauncher">
      <uap:VisualElements DisplayName={display} Description={display}
        Square150x150Logo="Assets\\Square150x150Logo.png"
        Square44x44Logo="Assets\\Square44x44Logo.png" BackgroundColor="#000000">
        <uap:DefaultTile Wide310x150Logo="Assets\\Wide310x150Logo.png" />
        <uap:SplashScreen Image="Assets\\SplashScreen.png" BackgroundColor="#000000" />
      </uap:VisualElements>
    </Application>
  </Applications>
</Package>
"""


def stage(executable: Path, destination: Path, title_id: str, name: str, version: str, icon, banner):
    title_id = validate_title_id(title_id)
    with executable.open("rb") as source:
        header = source.read(64)
        if len(header) != 64 or header[:2] != b"MZ":
            raise ValueError("Expected a Windows x64 PE executable")
        source.seek(struct.unpack_from("<I", header, 0x3C)[0])
        if source.read(6) != b"PE\0\0\x64\x86":
            raise ValueError("Expected a Windows x64 PE executable")
    manifest = build_manifest(title_id, name, version)
    ET.fromstring(manifest)
    if destination.exists():
        raise FileExistsError("Use a new staging directory to avoid packaging stale files")
    destination.mkdir(parents=True)
    shutil.copy2(executable, destination / "nxbox-launcher.exe")
    (destination / "title.txt").write_text(title_id + "\n", encoding="ascii")
    render_assets(icon, banner, destination / "Assets")
    (destination / "AppxManifest.xml").write_text(manifest, encoding="utf-8")
    notices = destination / "Notices"
    notices.mkdir()
    shutil.copy2(ROOT / "LICENSE.txt", notices / "Eden-LICENSE.txt")


def load_art(title_id, icon_url, banner_url, no_art):
    """Downloads the icon and banner; a failed download keeps the NXbox fallback art."""
    if no_art:
        return None, None
    index_banner, index_icon = (None, None)
    if ART_INDEX.is_file():
        index_banner, index_icon = art_urls(json.loads(ART_INDEX.read_text()), title_id)
    images = []
    for label, url in (("icon", icon_url or index_icon), ("banner", banner_url or index_banner)):
        image = None
        if url:
            try:
                image = download(url)
            except Exception as error:  # a missing image must not block the tile
                print(f"warning: {label} {url} unavailable ({error}); using the fallback")
        else:
            print(f"warning: no {label} art for {title_id}; using the fallback")
        images.append(image)
    return images[0], images[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True, help="built nxbox-launcher.exe")
    parser.add_argument("--title-id", required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--icon-url")
    parser.add_argument("--banner-url")
    parser.add_argument("--no-art", action="store_true", help="skip downloads, use NXbox art")
    parser.add_argument("--version", default="1.0.0.0")
    parser.add_argument("--output", type=Path, default=Path("out"))
    parser.add_argument("--stage-only", action="store_true")
    args = parser.parse_args()
    title_id = validate_title_id(args.title_id)
    icon, banner = load_art(title_id, args.icon_url, args.banner_url, args.no_art)
    args.output.mkdir(parents=True, exist_ok=True)
    staging = args.output / f"game-{title_id}"
    stage(args.exe, staging, title_id, args.name, args.version, icon, banner)
    if args.stage_only:
        print(f"Staged game tile {title_id}: {staging}")
        return
    package = (args.output / f"NXbox_Game_{title_id}_{args.version}_x64.appx").resolve()
    subprocess.run(
        [str(package_uwp.sdk_tool("makeappx")), "pack", "/d", str(staging.resolve()), "/p", str(package), "/o"],
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
                str(package_uwp.sdk_tool("signtool")),
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
