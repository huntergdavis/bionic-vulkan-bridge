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
            "usage: test_e113_large_barrier_architecture.py "
            "EVIDENCE HEADER PROTOCOL CODEC TEST"
        )
    evidence_path, header_path, protocol_path, codec_path, test_path = map(
        Path, sys.argv[1:]
    )
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    assert evidence["gate"] == "E113"
    assert evidence["implementation"]["maximum_memory_barriers"] == 64
    assert evidence["implementation"]["maximum_buffer_barriers"] == 256
    assert evidence["implementation"]["maximum_image_barriers"] == 2048
    assert evidence["implementation"]["maximum_payload_bytes"] < 256 * 1024
    assert evidence["validation"]["large_batch_image_count"] == 1024

    header = header_path.read_text(encoding="utf-8")
    protocol = protocol_path.read_text(encoding="utf-8")
    codec = codec_path.read_text(encoding="utf-8")
    test = test_path.read_text(encoding="utf-8")
    require(header, "BVB_COMMAND_VULKAN_MAX_MEMORY_BARRIERS = 64", "memory bound")
    require(header, "BVB_COMMAND_VULKAN_MAX_BUFFER_BARRIERS = 256", "buffer bound")
    require(header, "BVB_COMMAND_VULKAN_MAX_IMAGE_BARRIERS = 2048", "image bound")
    require(protocol, "BVB_COMMAND_STREAM_SLOT_BYTES = 256 * 1024", "slot size")
    require(codec, "maximum synchronization2 record must fit one stream slot", "fit assertion")
    require(codec, "barrier_payload_size", "count-sized barrier codec")
    require(test, "test_large_image_barrier_batch", "large batch test")
    require(test, "const uint32_t image_count = 1024U", "1024-image proof")
    print("PASS: E113 large synchronization2 batch architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
