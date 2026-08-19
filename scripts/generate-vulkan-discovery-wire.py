#!/usr/bin/env python3
"""Generate fixed-width Vulkan discovery codecs from the pinned vk.xml."""

from __future__ import annotations

import argparse
import re
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT_TYPES = (
    "VkPhysicalDeviceProperties",
    "VkQueueFamilyProperties",
    "VkPhysicalDeviceMemoryProperties",
    "VkExtensionProperties",
)

U32_TYPES = {
    "uint32_t",
    "int32_t",
    "float",
    "VkBool32",
    "VkMemoryHeapFlags",
    "VkMemoryPropertyFlags",
    "VkPhysicalDeviceType",
    "VkQueueFlags",
    "VkSampleCountFlags",
}
U64_TYPES = {"size_t", "VkDeviceSize"}
BYTE_TYPES = {"char", "uint8_t"}


def member_dimension(member: ET.Element) -> str | None:
    name = member.find("name")
    if name is None or name.tail is None:
        return None
    match = re.search(r"\[([^]]+)\]", name.tail)
    if match:
        return match.group(1)
    bound = member.find("enum")
    if name.tail.startswith("[") and bound is not None and bound.text:
        return bound.text
    return None


def generate(registry: Path) -> str:
    root = ET.parse(registry).getroot()
    structures = {
        node.get("name"): node
        for node in root.findall("./types/type[@category='struct']")
        if node.get("name")
    }
    ordered: list[str] = []
    visiting: set[str] = set()

    def visit(name: str) -> None:
        if name in ordered:
            return
        if name in visiting or name not in structures:
            raise ValueError(f"unsupported or recursive Vulkan type: {name}")
        visiting.add(name)
        for member in structures[name].findall("member"):
            type_name = member.findtext("type")
            if type_name in structures:
                visit(type_name)
            elif type_name not in U32_TYPES | U64_TYPES | BYTE_TYPES:
                raise ValueError(f"unsupported member type {type_name} in {name}")
        visiting.remove(name)
        ordered.append(name)

    for root_type in ROOT_TYPES:
        visit(root_type)

    lines = [
        "/* Generated from the pinned Khronos vk.xml; do not edit. */",
        "",
    ]
    for structure_name in ordered:
        structure = structures[structure_name]
        lines.append(
            f"static void bvb_wire_encode_{structure_name}("
            f"struct bvb_wire_writer *writer, const {structure_name} *value) {{"
        )
        for member in structure.findall("member"):
            type_name = member.findtext("type")
            member_name = member.findtext("name")
            dimension = member_dimension(member)
            if dimension is not None:
                if type_name in BYTE_TYPES:
                    lines.append(
                        f"    bvb_writer_put_bytes(writer, value->{member_name}, "
                        f"sizeof(value->{member_name}));"
                    )
                else:
                    lines.append(
                        f"    for (uint32_t index = 0U; index < {dimension}; ++index) {{"
                    )
                    if type_name in structures:
                        lines.append(
                            f"        bvb_wire_encode_{type_name}(writer, "
                            f"&value->{member_name}[index]);"
                        )
                    elif type_name == "float":
                        lines.append(
                            f"        bvb_writer_put_float(writer, "
                            f"value->{member_name}[index]);"
                        )
                    elif type_name in U64_TYPES:
                        lines.append(
                            f"        bvb_writer_put_u64(writer, "
                            f"(uint64_t)value->{member_name}[index]);"
                        )
                    else:
                        lines.append(
                            f"        bvb_writer_put_u32(writer, "
                            f"(uint32_t)value->{member_name}[index]);"
                        )
                    lines.append("    }")
            elif type_name in structures:
                lines.append(
                    f"    bvb_wire_encode_{type_name}(writer, &value->{member_name});"
                )
            elif type_name == "float":
                lines.append(
                    f"    bvb_writer_put_float(writer, value->{member_name});"
                )
            elif type_name in U64_TYPES:
                lines.append(
                    f"    bvb_writer_put_u64(writer, (uint64_t)value->{member_name});"
                )
            else:
                lines.append(
                    f"    bvb_writer_put_u32(writer, (uint32_t)value->{member_name});"
                )
        lines.extend(["}", ""])

        lines.append(
            f"static void bvb_wire_decode_{structure_name}("
            f"struct bvb_wire_reader *reader, {structure_name} *value) {{"
        )
        for member in structure.findall("member"):
            type_name = member.findtext("type")
            member_name = member.findtext("name")
            dimension = member_dimension(member)
            if dimension is not None:
                if type_name in BYTE_TYPES:
                    lines.append(
                        f"    bvb_reader_get_bytes(reader, value->{member_name}, "
                        f"sizeof(value->{member_name}));"
                    )
                    if member.get("len") == "null-terminated":
                        lines.append(
                            f"    if (memchr(value->{member_name}, '\\0', "
                            f"sizeof(value->{member_name})) == NULL) {{"
                        )
                        lines.append("        reader->status = -EPROTO;")
                        lines.append("    }")
                else:
                    lines.append(
                        f"    for (uint32_t index = 0U; index < {dimension}; ++index) {{"
                    )
                    if type_name in structures:
                        lines.append(
                            f"        bvb_wire_decode_{type_name}(reader, "
                            f"&value->{member_name}[index]);"
                        )
                    elif type_name == "float":
                        lines.append(
                            f"        value->{member_name}[index] = "
                            f"bvb_reader_get_float(reader);"
                        )
                    elif type_name in U64_TYPES:
                        lines.append(
                            f"        value->{member_name}[index] = "
                            f"({type_name})bvb_reader_get_u64(reader);"
                        )
                    else:
                        lines.append(
                            f"        value->{member_name}[index] = "
                            f"({type_name})bvb_reader_get_u32(reader);"
                        )
                    lines.append("    }")
            elif type_name in structures:
                lines.append(
                    f"    bvb_wire_decode_{type_name}(reader, &value->{member_name});"
                )
            elif type_name == "float":
                lines.append(
                    f"    value->{member_name} = bvb_reader_get_float(reader);"
                )
            elif type_name in U64_TYPES:
                lines.append(
                    f"    value->{member_name} = "
                    f"({type_name})bvb_reader_get_u64(reader);"
                )
            else:
                lines.append(
                    f"    value->{member_name} = "
                    f"({type_name})bvb_reader_get_u32(reader);"
                )
        lines.extend(["}", ""])
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("registry", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    rendered = generate(args.registry)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(rendered, encoding="utf-8")


if __name__ == "__main__":
    main()
