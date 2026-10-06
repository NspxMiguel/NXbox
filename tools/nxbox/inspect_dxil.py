#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Inspect DXIL containers without a Windows SDK; this is not a DXIL validator.

Input: a binary container or a JSON mapping of stage names to hex containers.
Unknown fields/parts are retained, never interpreted as an absence of features.
"""

import argparse
import json
import struct
from pathlib import Path


RESOURCE_TYPES = (
    "invalid",
    "sampler",
    "cbv",
    "srv_typed",
    "srv_raw",
    "srv_structured",
    "uav_typed",
    "uav_raw",
    "uav_structured",
    "uav_structured_counter",
)
RESOURCE_KINDS = (
    "invalid",
    "texture1d",
    "texture2d",
    "texture2dms",
    "texture3d",
    "texturecube",
    "texture1darray",
    "texture2darray",
    "texture2dmsarray",
    "texturecubearray",
    "typed_buffer",
    "raw_buffer",
    "structured_buffer",
    "cbuffer",
    "sampler",
    "tbuffer",
)
SYSTEM_VALUES = {
    0: "none",
    1: "SV_Position",
    2: "SV_ClipDistance",
    3: "SV_CullDistance",
    4: "SV_RenderTargetArrayIndex",
    5: "SV_ViewportArrayIndex",
    6: "SV_VertexID",
    7: "SV_PrimitiveID",
    8: "SV_InstanceID",
    9: "SV_IsFrontFace",
    10: "SV_SampleIndex",
    64: "SV_Target",
    65: "SV_Depth",
    66: "SV_Coverage",
    67: "SV_DepthGreaterEqual",
    68: "SV_DepthLessEqual",
    69: "SV_StencilRef",
    70: "SV_InnerCoverage",
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def word(data, offset=0):
    require(0 <= offset <= len(data) - 4, "truncated word")
    return struct.unpack_from("<I", data, offset)[0]


def region(data, offset, size):
    require(0 <= offset <= len(data) and 0 <= size <= len(data) - offset, "truncated region")
    return data[offset : offset + size]


def signature(data):
    count, offset = word(data), word(data, 4)
    require(offset >= 8, "invalid signature offset")
    region(data, offset, count * 32)
    result = []
    for index in range(count):
        pos = offset + index * 32
        stream, name, semantic, system, component, register = struct.unpack_from("<6I", data, pos)
        require(offset + count * 32 <= name < len(data), "invalid semantic string offset")
        end = data.find(b"\0", name)
        require(end >= 0, "unterminated semantic string")
        result.append(
            dict(
                semantic=data[name:end].decode("ascii"),
                index=semantic,
                stream=stream,
                system_value=SYSTEM_VALUES.get(system, system),
                component={1: "uint32", 2: "sint32", 3: "float32"}.get(component, component),
                register=register,
                mask=data[pos + 24],
                usage_mask=data[pos + 25],
                min_precision=word(data, pos + 28),
            )
        )
    return result


def psv(data):
    size = word(data)
    require(size >= 24, "invalid PSV runtime info size")
    runtime = region(data, 4, size)
    count = word(data, 4 + size)
    result = dict(
        runtime_size=size,
        runtime_hex=runtime.hex(),
        wave_min=word(runtime, 16),
        wave_max=word(runtime, 20),
        resources=[],
    )
    if size >= 36:
        result.update(
            stage=runtime[24],
            uses_view_id=runtime[25],
            input_elements=runtime[28],
            output_elements=runtime[29],
            input_vectors=runtime[31],
            output_vectors=list(runtime[32:36]),
        )
        if runtime[24] == 1:
            result["output_position_present"] = bool(runtime[0])
        if runtime[24] == 0:
            result.update(depth_output=bool(runtime[0]), sample_frequency=bool(runtime[1]))
    if count:
        stride = word(data, 8 + size)
        require(stride >= 16, "invalid PSV resource stride")
        region(data, 12 + size, count * stride)
        for index in range(count):
            pos = 12 + size + index * stride
            kind, space, lower, upper = struct.unpack_from("<4I", data, pos)
            require(lower <= upper, "reversed PSV binding range")
            resource = dict(
                type=RESOURCE_TYPES[kind] if kind < len(RESOURCE_TYPES) else kind,
                space=space,
                lower=lower,
                upper=upper,
                unbounded=upper == 0xFFFFFFFF,
            )
            if stride >= 24:
                kind = word(data, pos + 16)
                resource.update(
                    kind=RESOURCE_KINDS[kind] if kind < len(RESOURCE_KINDS) else kind,
                    flags=word(data, pos + 20),
                )
            else:
                resource.update(kind=None, flags=None)
            result["resources"].append(resource)
    return result


class Bitstream:
    """Bounded LLVM bitstream record reader (including BLOCKINFO abbreviations).

    Exposes records, not executable LLVM semantics. Symbol presence is evidence
    of a declaration; it does not prove that a call is reachable.
    """

    def __init__(self, data):
        require(data[:4] == b"BC\xc0\xde", "invalid LLVM bitcode magic")
        self.data, self.pos, self.inherited, self.records = data, 32, {}, []

    def bits(self, count):
        require(0 <= count <= 64 and self.pos + count <= len(self.data) * 8, "truncated bitstream")
        start, shift = divmod(self.pos, 8)
        self.pos += count
        return (int.from_bytes(self.data[start : (self.pos + 7) // 8], "little") >> shift) & (
            (1 << count) - 1
        )

    def vbr(self, width):
        require(2 <= width <= 32, "invalid VBR width")
        value = shift = 0
        while shift < 64:
            part = self.bits(width)
            value |= (part & ((1 << (width - 1)) - 1)) << shift
            if not part >> (width - 1):
                return value
            shift += width - 1
        raise ValueError("oversized VBR")

    def align(self):
        self.pos = (self.pos + 31) & ~31

    def operand(self, op):
        kind, value = op
        if kind == 0:
            return value
        if kind == 1:
            return self.bits(value)
        if kind == 2:
            return self.vbr(value)
        if kind == 4:
            return ord(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._"[self.bits(6)]
            )
        raise ValueError("invalid scalar abbreviation")

    def block(self, block_id=-1, width=2, end=None, depth=0):
        require(depth < 32, "excessive bitstream nesting")
        end = len(self.data) * 8 if end is None else end
        abbreviations = list(self.inherited.get(block_id, []))
        target = None
        while self.pos < end:
            code = self.bits(width)
            if code == 0:
                self.align()
                require(self.pos == end, "invalid block length")
                return
            if code == 1:
                child, child_width = self.vbr(8), self.vbr(4)
                require(2 <= child_width <= 32, "invalid block code width")
                self.align()
                length = self.bits(32)
                child_end = self.pos + length * 32
                require(child_end <= end, "block exceeds parent")
                self.block(child, child_width, child_end, depth + 1)
                continue
            if code == 2:
                ops = []
                for _ in range(self.vbr(5)):
                    if self.bits(1):
                        ops.append((0, self.vbr(8)))
                    else:
                        kind = self.bits(3)
                        require(1 <= kind <= 5, "invalid abbreviation")
                        ops.append((kind, self.vbr(5) if kind in (1, 2) else None))
                if block_id == 0:
                    require(target is not None, "BLOCKINFO missing target")
                    self.inherited.setdefault(target, []).append(ops)
                else:
                    abbreviations.append(ops)
                continue
            if code == 3:
                record = self.vbr(6)
                values = [self.vbr(6) for _ in range(self.vbr(6))]
            else:
                require(code - 4 < len(abbreviations), "unknown abbreviation")
                ops, values, index = abbreviations[code - 4], [], 0
                while index < len(ops):
                    op = ops[index]
                    if op[0] == 3:
                        require(index + 2 == len(ops), "invalid array abbreviation")
                        count = self.vbr(6)
                        require(count <= len(self.data) * 8, "oversized record array")
                        values.extend(self.operand(ops[index + 1]) for _ in range(count))
                        index += 2
                    elif op[0] == 5:
                        count = self.vbr(6)
                        self.align()
                        values.extend(self.bits(8) for _ in range(count))
                        self.align()
                        index += 1
                    else:
                        values.append(self.operand(op))
                        index += 1
                require(values, "empty abbreviation")
                record, values = values[0], values[1:]
            require(self.pos <= end, "record exceeds block")
            if block_id == 0 and record == 1:
                require(len(values) == 1, "invalid BLOCKINFO target")
                target = values[0]
            self.records.append((block_id, record, values))
        require(self.pos == end, "invalid end of bitstream")


def bitcode_facts(data):
    reader = Bitstream(data)
    reader.block()
    symbols = [bytes(v[1:]).decode("ascii") for b, c, v in reader.records if b == 14 and c == 1]
    strings = [bytes(v).decode("ascii") for b, c, v in reader.records if b == 15 and c in (1, 4)]
    # Mesa emits module values before module constants and old-style metadata.
    # Resolve only version tuples; do not pretend this is an LLVM disassembler.
    values, nodes, versions, name = [], [], {}, None
    for block, code, operands in reader.records:
        if block == 8 and code in (7, 8, 9):
            values.append(None)
        elif block == 11 and code != 1:
            value = None
            if code == 2:
                value = 0
            elif code == 4 and len(operands) == 1:
                encoded = operands[0]
                value = -(encoded >> 1) if encoded & 1 else encoded >> 1
            values.append(value)
        elif block == 15:
            if code == 1:
                nodes.append(bytes(operands).decode("ascii"))
            elif code == 2:
                nodes.append(values[operands[1]] if operands[1] < len(values) else None)
            elif code == 3:
                nodes.append(tuple(operands))
            elif code == 4:
                name = bytes(operands).decode("ascii")
            elif code == 10 and name in ("dx.version", "dx.valver"):
                require(len(operands) == 1 and operands[0] < len(nodes), "invalid version metadata")
                refs = nodes[operands[0]]
                require(isinstance(refs, tuple) and len(refs) == 2, "invalid version tuple")
                require(all(0 < ref <= len(nodes) for ref in refs), "invalid version reference")
                versions[name] = [nodes[ref - 1] for ref in refs]
    return dict(
        symbols=symbols,
        metadata_strings=strings,
        versions=versions,
        half_type_declared=any(b == 17 and c == 10 for b, c, v in reader.records),
        int16_type_declared=any(b == 17 and c == 7 and v == [16] for b, c, v in reader.records),
        derivative_declarations=[s for s in symbols if "deriv" in s.lower()],
        sample_declarations=[s for s in symbols if "sample" in s.lower()],
    )


def inspect(data):
    require(len(data) >= 32 and data[:4] == b"DXBC", "invalid container header")
    require(word(data, 24) == len(data), "container size mismatch")
    count = word(data, 28)
    region(data, 32, count * 4)
    result = dict(size=len(data), digest=data[4:20].hex(), parts={})
    spans = []
    for index in range(count):
        offset = word(data, 32 + index * 4)
        require(offset >= 32 + count * 4 and offset % 4 == 0, "invalid part offset")
        tag = region(data, offset, 4).decode("ascii")
        size = word(data, offset + 4)
        payload = region(data, offset + 8, size)
        require(tag not in result["parts"], "duplicate part")
        require(
            all(offset + 8 + size <= start or offset >= end for start, end in spans),
            "overlapping parts",
        )
        spans.append((offset, offset + 8 + size))
        value = dict(size=size)
        if tag in ("ISG1", "OSG1", "PSG1"):
            value["elements"] = signature(payload)
        elif tag == "PSV0":
            value.update(psv(payload))
        elif tag == "SFI0":
            require(size == 8, "invalid feature flags size")
            value["flags"] = int.from_bytes(payload, "little")
        elif tag == "HASH":
            require(size == 20, "invalid HASH size")
            value.update(flags=word(payload), digest=payload[4:].hex())
        elif tag == "DXIL":
            version, words, magic, dxil, start, length = struct.unpack(
                "<6I", region(payload, 0, 24)
            )
            require(
                magic == 0x4C495844 and words * 4 == size and start >= 16,
                "invalid DXIL program header",
            )
            value.update(
                stage={0: "pixel", 1: "vertex"}.get(version >> 16, version >> 16),
                shader_model=f"{(version >> 4) & 15}.{version & 15}",
                header_dxil_version=f"{dxil >> 8}.{dxil & 255}",
                bitcode_size=length,
            )
            value.update(bitcode_facts(region(payload, 8 + start, length)))
        elif tag == "RDAT":
            version, parts = word(payload), word(payload, 4)
            region(payload, 8, parts * 4)
            value.update(version=version, tables=[])
            for i in range(parts):
                pos = word(payload, 8 + i * 4)
                require(pos >= 8 + parts * 4 and pos % 4 == 0, "invalid RDAT offset")
                kind, length = word(payload, pos), word(payload, pos + 4)
                table = region(payload, pos + 8, length)
                item = dict(type=kind, size=length, hex=table.hex())
                if kind == 3:  # RuntimeDataResourceTable, extensible record stride.
                    records, stride = word(table), word(table, 4)
                    require(stride >= 32, "invalid RDAT resource stride")
                    region(table, 8, records * stride)
                    item["resources"] = [
                        dict(
                            zip(
                                (
                                    "class",
                                    "kind",
                                    "id",
                                    "space",
                                    "lower",
                                    "upper",
                                    "name_offset",
                                    "flags",
                                ),
                                struct.unpack_from("<8I", table, 8 + j * stride),
                            )
                        )
                        for j in range(records)
                    ]
                value["tables"].append(item)
        else:
            value["hex"] = payload.hex()
        result["parts"][tag] = value
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    args = parser.parse_args()
    data = args.input.read_bytes()
    result = (
        inspect(data)
        if data[:4] == b"DXBC"
        else {stage: inspect(bytes.fromhex(value)) for stage, value in json.loads(data).items()}
    )
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
