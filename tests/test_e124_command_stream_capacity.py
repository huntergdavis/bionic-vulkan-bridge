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
            "usage: test_e124_command_stream_capacity.py "
            "EVIDENCE PROTOCOL CLIENT CODEC CODEC_TEST"
        )
    evidence_path, protocol_path, client_path, codec_path, test_path = map(
        Path, sys.argv[1:]
    )
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    assert evidence["gate"] == "E124"
    assert evidence["trigger"]["entry"] == "vkCmdCopyImage2"
    assert evidence["trigger"]["result"] == -28
    assert evidence["trigger"]["activity_present_sequence"] == 682
    assert evidence["implementation"]["slot_count"] == 64
    assert evidence["implementation"]["slot_bytes"] == 2 * 1024 * 1024
    assert evidence["implementation"]["region_bytes"] == 128 * 1024 * 1024
    assert evidence["implementation"]["simultaneous_recordings"] == 64
    assert evidence["validation"]["ten_thousand_copy_image_records"] is True

    protocol = protocol_path.read_text(encoding="utf-8")
    client = client_path.read_text(encoding="utf-8")
    codec = codec_path.read_text(encoding="utf-8")
    test = test_path.read_text(encoding="utf-8")
    require(protocol, "BVB_COMMAND_STREAM_SLOT_COUNT = 64", "slot count")
    require(
        protocol,
        "BVB_COMMAND_STREAM_SLOT_BYTES = 2 * 1024 * 1024",
        "two-megabyte slot",
    )
    require(protocol, "BVB_COMMAND_STREAM_REGION_BYTES =", "region formula")
    require(
        protocol,
        "bvb_protocol_encode_vulkan_command_stream_setup",
        "command-stream-specific setup codec",
    )
    require(client, "command_stream_slot_exhausted", "exact capacity diagnostic")
    require(client, "BVB_COMMAND_STREAM_SLOT_BYTES", "client slot arithmetic")
    require(codec, "transfer_payload_size", "compact transfer codec")
    require(test, "test_e124_large_stream_slot_capacity", "capacity regression")
    require(test, "index < 10000U", "ten-thousand-record proof")
    require(test, "BVB_COMMAND_VULKAN_COPY_IMAGE_2", "observed command shape")
    print("PASS: E124 post-frame command-stream capacity architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
