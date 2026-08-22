#!/usr/bin/env python3

import json
from pathlib import Path
import sys


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing {label}: {needle}")


def main() -> int:
    if len(sys.argv) != 7:
        raise SystemExit(
            "usage: test_e103_graphics_format_architecture.py "
            "EVIDENCE PROTOCOL PIPELINE_WIRE CLIENT NATIVE TEST"
        )
    evidence_path, protocol_path, wire_path, client_path, native_path, test_path = (
        map(Path, sys.argv[1:])
    )
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    assert evidence["gate"] == "E103"
    assert evidence["trigger"]["render_target_view_rejections"] == 29
    assert evidence["trigger"]["shader_resource_view_rejections"] == 30
    assert evidence["implementation"]["blob_max_bytes"] == 262144
    assert evidence["implementation"]["round_trips_per_pipeline"] == 1

    protocol = protocol_path.read_text(encoding="utf-8")
    require(protocol, "BVB_OPCODE_VULKAN_FORMAT_PROPERTIES_3 = 115", "format3 opcode")
    require(protocol, "BVB_VULKAN_FORMAT_PROPERTIES_3_SIZE = 24", "64-bit response")

    wire = wire_path.read_text(encoding="utf-8")
    require(wire, "BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_E103_BLOB_VERSION = 2", "E103 schema")
    require(wire, "BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_BLOB_MAX_SIZE = 256U * 1024U", "cap")

    client = client_path.read_text(encoding="utf-8")
    require(client, "get_format_properties_3(", "format3 client")
    require(client, "create_general_graphics_pipeline(", "general pipeline client")
    require(client, "F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_WRITE | F_SEAL_SEAL", "immutable blob")
    require(client, "BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_DYNAMIC_STATES", "bounded dynamics")

    native = native_path.read_text(encoding="utf-8")
    require(native, "bvb_vulkan_global_context_get_format_properties_3(", "native format3")
    require(native, "bvb_vulkan_global_context_create_general_graphics_pipeline(", "native pipeline")
    require(native, "general_graphics_blob_resolve(", "relative-offset validation")
    require(native, "MAP_PRIVATE", "private service snapshot")

    test = test_path.read_text(encoding="utf-8")
    require(test, "format_properties3.linearTilingFeatures", "64-bit test")
    require(test, "general_pipeline_info", "general pipeline test")
    require(test, "general_pipeline_id != builtin_pipeline_id", "typed distinct pipeline")
    print("PASS: E103 format-properties3 and general graphics architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
