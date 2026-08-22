#!/usr/bin/env python3

import json
from pathlib import Path
import sys


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing {label}: {needle}")


def main() -> int:
    if len(sys.argv) != 6:
        raise SystemExit(
            "usage: test_e104_depth_clip_pipeline_architecture.py "
            "EVIDENCE WIRE CLIENT NATIVE GLOBAL_TEST"
        )
    evidence_path, wire_path, client_path, native_path, test_path = map(
        Path, sys.argv[1:]
    )
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    assert evidence["gate"] == "E104"
    assert evidence["trigger"]["format_render_target_rejections_after_e103"] == 0
    assert evidence["trigger"]["format_shader_resource_rejections_after_e103"] == 0
    assert evidence["implementation"]["general_graphics_schema"] == 3

    wire = wire_path.read_text(encoding="utf-8")
    require(wire, "BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_BLOB_HEADER_SIZE = 88", "v3 header")
    require(wire, "BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_BLOB_VERSION = 3", "v3 schema")

    client = client_path.read_text(encoding="utf-8")
    require(client, "VkPipelineRasterizationDepthClipStateCreateInfoEXT *depth_clip", "client state")
    require(client, "depth_clip_offset = BVB_APPEND_OBJECT", "client serialization")
    require(client, "BVB_ICD_GRAPHICS_RASTER_PNEXT", "runtime diagnostic")

    native = native_path.read_text(encoding="utf-8")
    require(native, "VkPipelineRasterizationDepthClipStateCreateInfoEXT *depth_clip", "native state")
    require(native, "rasterization->pNext = depth_clip", "native reconstruction")

    test = test_path.read_text(encoding="utf-8")
    require(test, "general_depth_clip", "cross-process fixture")
    require(test, ".depthClipEnable = VK_TRUE", "truthful depth clip value")
    print("PASS: E104 depth-clip graphics pipeline architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
