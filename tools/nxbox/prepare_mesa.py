# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch checksum-pinned Mesa UWP runtime libraries for the graphics probe."""

import argparse
import hashlib
from pathlib import Path
import urllib.request
import zipfile

URL = "https://github.com/aerisarn/mesa-uwp/releases/download/alpha-2-resfix/alpha-2-resfix.zip"
SHA256 = "1502dfd1af9fb831b0ab82ea7e16b71a5b415d8f07a9090578e3a89c8acc1bd6"
LICENSE_URL = "https://raw.githubusercontent.com/aerisarn/mesa-uwp/15acdd7ea2b9dcdd62f26fe86b88280d79efc46b/docs/license.rst"
LICENSE_SHA256 = "a00275a53178e2645fb65be99a785c110513446a5071ff2c698ed260ad917d75"
LIBRARIES = ("opengl32.dll", "libgallium_wgl.dll", "libglapi.dll", "z-1.dll")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    archive = args.output / "mesa.zip"
    urllib.request.urlretrieve(URL, archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise RuntimeError("Mesa archive checksum mismatch")
    with zipfile.ZipFile(archive) as bundle:
        for name in LIBRARIES:
            # Read only explicit top-level runtime files, never extract arbitrary archive paths.
            (args.output / name).write_bytes(bundle.read(name))
    archive.unlink()
    with urllib.request.urlopen(LICENSE_URL, timeout=60) as response:
        notice = response.read()
    if hashlib.sha256(notice).hexdigest() != LICENSE_SHA256:
        raise RuntimeError("Mesa license checksum mismatch")
    (args.output / "Mesa-LICENSE.rst").write_bytes(notice)
    print("Verified Mesa UWP alpha-2-resfix runtime")


if __name__ == "__main__":
    main()
