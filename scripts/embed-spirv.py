#!/usr/bin/env python3

"""Embed little-endian SPIR-V binaries as aligned uint32_t C arrays."""

from __future__ import annotations

import argparse
from pathlib import Path
import struct


def array(name: str, path: Path) -> str:
    raw = path.read_bytes()
    if not raw or len(raw) % 4:
        raise ValueError(f"{path}: SPIR-V size is not a nonzero word count")
    words = struct.unpack(f"<{len(raw) // 4}I", raw)
    if words[0] != 0x07230203:
        raise ValueError(f"{path}: invalid SPIR-V magic")
    rendered = [f"static const uint32_t {name}[] = {{"]
    for offset in range(0, len(words), 8):
        rendered.append(
            "    " + ", ".join(f"UINT32_C(0x{word:08x})" for word in words[offset : offset + 8]) + ","
        )
    rendered.append("};")
    return "\n".join(rendered)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("vertex", type=Path)
    parser.add_argument("fragment", type=Path)
    parser.add_argument("output", type=Path)
    arguments = parser.parse_args()
    output = "\n\n".join(
        (
            "/* Generated from checked-in GLSL; do not edit. */",
            array("BVB_TRIANGLE_VERTEX_SPIRV", arguments.vertex),
            array("BVB_TRIANGLE_FRAGMENT_SPIRV", arguments.fragment),
        )
    ) + "\n"
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(output)


if __name__ == "__main__":
    main()
