#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reassemble list-ring fragments and audit captured static D3D12 arguments."""

import argparse
import json
import re
from collections import Counter, defaultdict
from pathlib import Path

RECORD = re.compile(
    r"D3D12_LIST_RING capture=(\d+).*?seq=(\d+) generation=(\d+) "
    r"tid=(\d+) call=(\w+) site=(.*?) fragment=(\d+) (.*)"
)
RESOURCE = re.compile(
    r"res=([0-9A-F]+) \{dim=(\d+) format=(\d+) size=(\d+)x(\d+)x(\d+) "
    r"mips=(\d+) samples=(\d+):(\d+) flags=0x([0-9a-f]+)[^}]*\}"
)
FAMILIES = [
    (1, 4),
    (5, 8),
    (9, 14),
    (15, 18),
    (19, 22),
    (23, 25),
    (27, 32),
    (33, 38),
    (39, 43),
    (44, 47),
    (48, 52),
    (53, 59),
    (60, 64),
    (70, 72),
    (73, 75),
    (76, 78),
    (79, 81),
    (82, 84),
    (94, 96),
    (97, 99),
]


def family(f):
    if f in (87, 90, 91):
        return 90
    if f in (88, 92, 93):
        return 92
    return next((lo for lo, hi in FAMILIES if lo <= f <= hi), f)


def bc(f):
    return 70 <= f <= 84 or 94 <= f <= 99


def texel_bytes(f):
    return {
        1: 16,
        5: 12,
        9: 8,
        15: 8,
        19: 8,
        23: 4,
        26: 4,
        27: 4,
        33: 4,
        39: 4,
        44: 4,
        48: 2,
        53: 2,
        60: 1,
        70: 8,
        73: 16,
        76: 16,
        79: 8,
        82: 16,
        94: 16,
        97: 16,
        90: 4,
        92: 4,
    }.get(family(f))


def compatible(a, b):
    if family(a) == family(b):
        return True
    if (a == 26 and family(b) == 39) or (b == 26 and family(a) == 39):
        return True
    plain = family(b if bc(a) else a)
    return (
        bc(a) != bc(b)
        and texel_bytes(a) == texel_bytes(b)
        and ((texel_bytes(a) == 8 and plain in (9, 15)) or (texel_bytes(a) == 16 and plain == 1))
    )


def resources(args):
    keys = ("ptr", "dim", "format", "w", "h", "d", "mips", "samples", "quality", "flags")
    return [
        dict(zip(keys, [v if i == 0 else int(v, 16 if i == 9 else 10) for i, v in enumerate(m)]))
        for m in RESOURCE.findall(args)
    ]


def parse(text):
    fragments = defaultdict(dict)
    for line in text.splitlines():
        m = RECORD.search(line)
        if not m:
            continue
        cap, seq, gen, tid, call, site, frag, args = m.groups()
        key = (int(cap), int(gen), int(seq))
        value = (tid, call, site, args)
        index = int(frag)
        if index in fragments[key] and fragments[key][index] != value:
            raise ValueError(f"Conflicting repeated fragment {key}/{index}")
        fragments[key][index] = value
    records = []
    for (cap, gen, seq), parts in sorted(fragments.items()):
        if sorted(parts) != list(range(len(parts))):
            raise ValueError(f"Missing fragment for sequence {seq}")
        tid, call, site, _ = parts[0]
        records.append(
            {
                "capture": cap,
                "generation": gen,
                "seq": seq,
                "tid": tid,
                "call": call,
                "site": site,
                "args": "".join(parts[i][3] for i in sorted(parts)),
            }
        )
    return records, sum(map(len, fragments.values()))


def endpoint(text):
    desc = resources(text)[0]
    fp = re.search(
        r"footprint=\{offset=(\d+) format=(\d+) size=(\d+)x(\d+)x(\d+) pitch=(\d+)\}", text
    )
    if fp:
        off, fmt, w, h, d, pitch = map(int, fp.groups())
        return desc, (w, h, d), fmt, (off, pitch)
    sub = int(re.search(r"sub=(\d+)", text)[1])
    mip = sub % desc["mips"]
    w, h = max(1, desc["w"] >> mip), max(1, desc["h"] >> mip)
    if bc(desc["format"]):
        w, h = (w + 3) // 4 * 4, (h + 3) // 4 * 4
    return desc, (w, h, max(1, desc["d"] >> mip) if desc["dim"] == 4 else 1), desc["format"], sub


def audit(record):
    a = record["args"]
    errors = []
    if record["call"] == "ResourceBarrier":
        for index, b in re.findall(r"barrier\[(\d+)\]=\{(.*?) \}", a):
            typ, flags = re.search(r"type=(\d+) flags=0x([0-9a-f]+)", b).groups()
            typ, flags = int(typ), int(flags, 16)
            if typ not in (0, 1, 2):
                errors.append(
                    f"barrier[{index}]: invalid type {typ} (0x{typ:x}), flags=0x{flags:x}"
                )
                continue
            if flags & ~3 or flags == 3 or (typ != 0 and flags):
                errors.append(f"barrier[{index}]: invalid flags 0x{flags:x}")
            rs = resources(b)
            if not rs:
                continue  # Null UAV and aliasing resources are legal.
            d = rs[0]
            states = re.search(r"before=0x([0-9a-f]+) after=0x([0-9a-f]+)", b)
            if typ == 2 and not d["flags"] & 4:
                errors.append(f"barrier[{index}]: UAV without ALLOW_UNORDERED_ACCESS")
            if not states:
                continue
            before, after = (int(v, 16) for v in states.groups())
            if before == after:
                errors.append(f"barrier[{index}]: equal states")
            for s in (before, after):
                for state, flag in ((8, 4), (4, 1), (48, 2)):
                    if s & state and not d["flags"] & flag:
                        errors.append(f"barrier[{index}]: state 0x{s:x} lacks resource flag {flag}")
                if s & 192 and d["flags"] & 8:
                    errors.append(f"barrier[{index}]: DENY_SHADER_RESOURCE with SRV state")
                writes = s & (4 | 8 | 16 | 256 | 1024 | 4096)
                if writes and (writes & (writes - 1) or s != writes):
                    errors.append(f"barrier[{index}]: invalid write/read union 0x{s:x}")
                if s & 4096 and d["samples"] <= 1 or s & 8192 and d["samples"] != 1:
                    errors.append(f"barrier[{index}]: resolve state/sample mismatch")
    if record["call"] == "CopyBufferRegion":
        d, s = resources(a)
        od, os, n = map(
            int, re.search(r"dst_offset=(\d+).*?src_offset=(\d+) bytes=(\d+)", a).groups()
        )
        if d["dim"] != 1 or s["dim"] != 1 or od + n > d["w"] or os + n > s["w"]:
            errors.append("buffer copy out of bounds")
        if d["ptr"] == s["ptr"] and n and od < os + n and os < od + n:
            errors.append("same buffer overlapping ranges")
    if record["call"] == "CopyTextureRegion":
        dt, st = a.split(" src=", 1)
        s, se, sf, sl = endpoint(st)
        d, de, df, dl = endpoint(dt)
        if not compatible(sf, df):
            errors.append(f"incompatible formats {sf}/{df}")
        xyz = tuple(map(int, re.search(r"x=(\d+) y=(\d+) z=(\d+)", dt).groups()))
        box = re.search(r"box=(\d+),(\d+),(\d+):(\d+),(\d+),(\d+)", st)
        coords = tuple(map(int, box.groups())) if box else (0, 0, 0, *se)
        lo, hi = coords[:3], coords[3:]
        if any(l > r or r > extent for l, r, extent in zip(lo, hi, se)):
            errors.append("source box out of bounds")
        scale = (4 if bc(df) else 1) / (4 if bc(sf) else 1)
        size = ((hi[0] - lo[0]) * scale, (hi[1] - lo[1]) * scale, hi[2] - lo[2])
        if any(x + n > e for x, n, e in zip(xyz, size, de)):
            errors.append("destination box out of bounds")
        if ((s["flags"] | d["flags"]) & 2 or max(s["samples"], d["samples"]) > 1) and (
            box or any(xyz) or se != de or s["samples"] != d["samples"]
        ):
            errors.append("partial depth/stencil or MSAA copy")
        if s["ptr"] == d["ptr"] and isinstance(sl, int) and sl == dl:
            errors.append("same texture subresource")
        for desc, extent, fmt, loc in ((s, se, sf, sl), (d, de, df, dl)):
            if not isinstance(loc, tuple):
                continue
            off, pitch = loc
            if off % 512 or pitch % 256:
                errors.append("footprint alignment")
            block = 4 if bc(fmt) else 1
            row_bytes = texel_bytes(fmt)
            if row_bytes is None:
                errors.append(f"unchecked footprint format {fmt}")
                continue
            row_bytes *= (extent[0] + block - 1) // block
            rows = (extent[1] + block - 1) // block
            required = off + pitch * (rows * extent[2] - 1) + row_bytes
            if desc["dim"] != 1 or pitch < row_bytes or required > desc["w"]:
                errors.append("footprint exceeds buffer or row pitch")
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    records, fragments = parse(args.capture.read_text())
    print(f"{len(records)} commands, {fragments} unique fragments")
    print(json.dumps(Counter(r["call"] for r in records), sort_keys=True))
    for r in records:
        r["errors"] = audit(r)
        if r["errors"]:
            print(f"seq={r['seq']} tid={r['tid']} site={r['site']}: {'; '.join(r['errors'])}")
    if args.json:
        args.json.write_text(json.dumps(records, indent=2) + "\n")


if __name__ == "__main__":
    main()
