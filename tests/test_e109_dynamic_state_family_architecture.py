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
            "usage: test_e109_dynamic_state_family_architecture.py "
            "EVIDENCE HEADER CODEC CLIENT NATIVE FAKE POLICY"
        )
    evidence_path, header_path, codec_path, client_path, native_path, fake_path, policy_path = map(
        Path, sys.argv[1:]
    )
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    assert evidence["gate"] == "E109"
    assert evidence["implementation"]["command_record_id"] == 33
    assert evidence["implementation"]["canonical_state_count"] == 19
    assert evidence["implementation"]["public_entry_count"] == 31
    assert evidence["validation"]["strict_recording_rpcs"] == 41
    assert evidence["validation"]["shared_recording_rpcs"] == 0
    assert evidence["validation"]["visible_game_frame"] is False

    header = header_path.read_text(encoding="utf-8")
    codec = codec_path.read_text(encoding="utf-8")
    client = client_path.read_text(encoding="utf-8")
    native = native_path.read_text(encoding="utf-8")
    fake = fake_path.read_text(encoding="utf-8")
    policy = policy_path.read_text(encoding="utf-8")
    require(header, "BVB_COMMAND_VULKAN_DYNAMIC_STATE = 33", "record id")
    require(header, "BVB_VULKAN_DYNAMIC_STATE_BLEND_CONSTANTS = 19", "family tail")
    require(codec, "float_word_is_finite", "finite float rejection")
    require(codec, "bvb_command_batch_append_vulkan_dynamic_state", "codec")
    for name in evidence["implementation"]["entry_points"]:
        require(client, f'BVB_DEVICE_MATCH("{name}"', f"client {name}")
        require(policy, name, f"policy {name}")
    require(client, "submit_dynamic_state_command", "single router")
    require(client, "finish_single_render_record(", "strict/shared router")
    require(native, "BVB_RESOLVE_DYNAMIC_CORE", "core/EXT fallback")
    require(native, "PFN_vkCmdSetBlendConstants", "native scalar replay")
    require(fake, "fake_cmd_set_cull_mode", "first exact native state")
    require(fake, "fake_cmd_set_blend_constants", "last exact native state")
    print("PASS: E109 scalar dynamic-state family architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
