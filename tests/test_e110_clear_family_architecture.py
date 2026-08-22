#!/usr/bin/env python3

import json
from pathlib import Path
import sys


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing {label}: {needle}")


def main() -> int:
    if len(sys.argv) != 8:
        raise SystemExit(
            "usage: test_e110_clear_family_architecture.py "
            "EVIDENCE HEADER CODEC CLIENT NATIVE FAKE POLICY"
        )
    evidence_path, header_path, codec_path, client_path, native_path, fake_path, policy_path = map(
        Path, sys.argv[1:]
    )
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    assert evidence["gate"] == "E110"
    assert evidence["implementation"]["command_record_ids"] == [34, 35]
    assert evidence["implementation"]["maximum_image_ranges"] == 4
    assert evidence["implementation"]["maximum_attachments"] == 8
    assert evidence["implementation"]["maximum_rectangles"] == 8
    assert evidence["validation"]["strict_recording_rpcs"] == 43
    assert evidence["validation"]["shared_recording_rpcs"] == 0
    assert evidence["validation"]["tablet_runtime"]["visible_game_frame"] is False

    header = header_path.read_text(encoding="utf-8")
    codec = codec_path.read_text(encoding="utf-8")
    client = client_path.read_text(encoding="utf-8")
    native = native_path.read_text(encoding="utf-8")
    fake = fake_path.read_text(encoding="utf-8")
    policy = policy_path.read_text(encoding="utf-8")
    require(header, "BVB_COMMAND_VULKAN_CLEAR_DEPTH_STENCIL_IMAGE = 34", "depth record")
    require(header, "BVB_COMMAND_VULKAN_CLEAR_ATTACHMENTS = 35", "attachment record")
    require(header, "BVB_COMMAND_VULKAN_MAX_CLEAR_ATTACHMENTS = 8", "attachment bound")
    require(codec, "bvb_command_batch_append_vulkan_clear_depth_stencil_image", "depth codec")
    require(codec, "bvb_command_batch_append_vulkan_clear_attachments", "attachment codec")
    for name in evidence["implementation"]["entry_points"]:
        require(client, f'BVB_DEVICE_MATCH("{name}"', f"client {name}")
        require(policy, name, f"policy {name}")
    require(client, "finish_single_render_record(", "strict/shared router")
    require(native, "replay_command_stream_clear_depth_stencil_image", "native depth replay")
    require(native, "replay_command_stream_clear_attachments", "native attachment replay")
    require(fake, "fake_cmd_clear_depth_stencil_image", "fake depth command")
    require(fake, "fake_cmd_clear_attachments", "fake attachment command")
    print("PASS: E110 clear command family architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
