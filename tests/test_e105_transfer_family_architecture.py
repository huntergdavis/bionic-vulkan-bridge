#!/usr/bin/env python3

import json
from pathlib import Path
import sys


TRANSFER_ENTRIES = (
    "vkCmdCopyBuffer2",
    "vkCmdCopyBufferToImage2",
    "vkCmdCopyImageToBuffer2",
    "vkCmdCopyImage2",
    "vkCmdBlitImage2",
    "vkCmdResolveImage2",
)


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing {label}: {needle}")


def main() -> int:
    if len(sys.argv) != 7:
        raise SystemExit(
            "usage: test_e105_transfer_family_architecture.py "
            "EVIDENCE HEADER CODEC CLIENT NATIVE POLICY"
        )
    evidence_path, header_path, codec_path, client_path, native_path, policy_path = map(
        Path, sys.argv[1:]
    )
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    assert evidence["gate"] == "E105"
    assert evidence["implementation"]["family_entry_count"] == 6
    assert evidence["implementation"]["maximum_regions_per_command"] == 16
    assert evidence["validation"]["visible_game_frame"] is False

    header = header_path.read_text(encoding="utf-8")
    codec = codec_path.read_text(encoding="utf-8")
    client = client_path.read_text(encoding="utf-8")
    native = native_path.read_text(encoding="utf-8")
    policy = set(policy_path.read_text(encoding="utf-8").splitlines())

    require(header, "BVB_COMMAND_VULKAN_COPY_BUFFER_2 = 14", "first record ID")
    require(header, "BVB_COMMAND_VULKAN_RESOLVE_IMAGE_2 = 19", "last record ID")
    require(header, "BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS = 16", "region bound")
    require(codec, "bvb_command_batch_append_vulkan_transfer", "canonical encoder")
    require(codec, "bvb_command_decode_vulkan_transfer", "canonical decoder")
    require(client, "submit_transfer_command", "strict/shared client path")
    require(native, "validate_transfer_command_record", "typed native validation")
    require(native, "command_stream_transfer_opcode", "shared replay family")
    for entry in TRANSFER_ENTRIES:
        require(client, f'BVB_DEVICE_MATCH("{entry}"', f"{entry} client dispatch")
        require(native, f'"{entry}"', f"{entry} native dispatch")
        if entry not in policy:
            raise AssertionError(f"missing executable policy entry: {entry}")
    print("PASS: E105 bounded Vulkan transfer-command family architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
