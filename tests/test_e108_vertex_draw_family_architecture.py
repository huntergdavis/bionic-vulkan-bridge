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
            "usage: test_e108_vertex_draw_family_architecture.py "
            "EVIDENCE HEADER CODEC CLIENT NATIVE FAKE POLICY"
        )
    evidence_path, header_path, codec_path, client_path, native_path, fake_path, policy_path = map(
        Path, sys.argv[1:]
    )
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    assert evidence["gate"] == "E108"
    assert evidence["implementation"]["maximum_vertex_bindings"] == 16
    assert evidence["implementation"]["command_record_ids"] == list(range(24, 33))
    assert evidence["validation"]["strict_recording_rpcs"] == 22
    assert evidence["validation"]["shared_recording_rpcs"] == 0
    assert evidence["validation"]["visible_game_frame"] is False

    header = header_path.read_text(encoding="utf-8")
    codec = codec_path.read_text(encoding="utf-8")
    client = client_path.read_text(encoding="utf-8")
    native = native_path.read_text(encoding="utf-8")
    fake = fake_path.read_text(encoding="utf-8")
    policy = policy_path.read_text(encoding="utf-8")
    require(header, "BVB_COMMAND_VULKAN_MAX_VERTEX_BINDINGS = 16", "vertex bound")
    require(header, "BVB_COMMAND_VULKAN_DRAW_INDEXED_INDIRECT_COUNT = 32", "family tail")
    require(codec, "BVB_VULKAN_BIND_VERTEX_BUFFERS_SIZE = 16 +", "vertex wire")
    require(codec, "bvb_command_batch_append_vulkan_draw_indirect_count", "count codec")
    for name in evidence["implementation"]["entry_points"]:
        require(client, f'BVB_DEVICE_MATCH("{name}"', f"client {name}")
        require(policy, name, f"policy {name}")
    require(client, "shared_buffers_owned_by_command_device", "ownership cache router")
    require(client, "finish_single_render_record(", "strict immediate router")
    require(native, "PFN_vkCmdBindVertexBuffers2", "native vertex reconstruction")
    require(native, "PFN_vkCmdBindIndexBuffer2", "native index reconstruction")
    require(native, "PFN_vkCmdDrawIndexedIndirectCount", "native count draw")
    require(fake, "fake_cmd_bind_vertex_buffers_2", "exact vertex proof")
    require(fake, "fake_cmd_draw_indexed_indirect_count", "exact draw proof")
    print("PASS: E108 vertex/index/draw family architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
