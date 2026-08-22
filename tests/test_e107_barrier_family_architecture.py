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
            "usage: test_e107_barrier_family_architecture.py "
            "EVIDENCE HEADER CODEC CLIENT NATIVE FAKE"
        )
    evidence_path, header_path, codec_path, client_path, native_path, fake_path = map(
        Path, sys.argv[1:]
    )
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    assert evidence["gate"] == "E107"
    assert evidence["implementation"]["maximum_memory_barriers"] == 16
    assert evidence["implementation"]["maximum_buffer_barriers"] == 16
    assert evidence["implementation"]["maximum_image_barriers"] == 16
    assert evidence["implementation"]["encoded_payload_bytes"] == 2832
    assert evidence["validation"]["visible_game_frame"] is False

    header = header_path.read_text(encoding="utf-8")
    codec = codec_path.read_text(encoding="utf-8")
    client = client_path.read_text(encoding="utf-8")
    native = native_path.read_text(encoding="utf-8")
    fake = fake_path.read_text(encoding="utf-8")
    require(header, "BVB_COMMAND_VULKAN_MAX_MEMORY_BARRIERS = 16", "memory bound")
    require(header, "BVB_COMMAND_VULKAN_MAX_BUFFER_BARRIERS = 16", "buffer bound")
    require(header, "BVB_COMMAND_VULKAN_MAX_IMAGE_BARRIERS = 16", "image bound")
    require(header, "struct bvb_vulkan_buffer_barrier_2", "typed buffer record")
    require(codec, "BVB_VULKAN_MEMORY_BARRIER_2_RECORD_SIZE = 32", "memory wire")
    require(codec, "BVB_VULKAN_BUFFER_BARRIER_2_RECORD_SIZE = 64", "buffer wire")
    require(codec, "BVB_VULKAN_IMAGE_BARRIER_2_RECORD_SIZE = 80", "image wire")
    require(codec, "~UINT32_C(0x6f)", "dependency flag validation")
    require(client, "dependency_info->memoryBarrierCount", "memory input")
    require(client, "dependency_info->bufferMemoryBarrierCount", "buffer input")
    require(client, "finish_single_render_record(", "strict immediate route")
    require(client, "shared_object_owned_by_device_cached_locked(", "shared ownership cache")
    require(native, "VkMemoryBarrier2", "native memory reconstruction")
    require(native, "VkBufferMemoryBarrier2", "native buffer reconstruction")
    require(native, "VkImageMemoryBarrier2", "native image reconstruction")
    require(native, "validate_pipeline_barrier_record", "service validation")
    require(fake, "memory->srcStageMask", "mixed memory proof")
    require(fake, "buffer->srcQueueFamilyIndex == 2U", "queue transfer proof")
    print("PASS: E107 synchronization2 barrier family architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
