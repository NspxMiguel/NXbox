# SPDX-License-Identifier: GPL-3.0-or-later
"""Map stable vMAJOR.MINOR.BUILD[.REVISION] tags to increasing Appx versions."""

import json
import re
import sys


def parse_version(tag):
    if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+(?:\.[0-9]+)?", tag):
        raise ValueError("Expected vMAJOR.MINOR.BUILD[.REVISION], without prerelease suffix")
    parts = tuple(map(int, tag[1:].split(".")))
    parts += (0,) * (4 - len(parts))
    if parts[0] < 1 or any(part > 65535 for part in parts):
        raise ValueError("Use major >= 1 and components <= 65535 (above 0.3 diagnostics)")
    return parts


def validate_version(tag, pages):
    version = parse_version(tag)
    for page in pages:
        for release in page:
            if release["draft"] or release["prerelease"]:
                continue
            try:
                previous = parse_version(release["tag_name"])
            except ValueError:
                continue
            if version <= previous:
                raise ValueError("Package version must exceed every published stable release")
    return ".".join(map(str, version))


if __name__ == "__main__":
    with open(sys.argv[2], encoding="utf-8") as source:
        version = validate_version(sys.argv[1], json.load(source))
    print(f"package_version={version}")
