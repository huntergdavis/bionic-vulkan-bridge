#!/usr/bin/env python3

import json
from pathlib import Path
import sys


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing {label}: {needle}")


def main() -> int:
    if len(sys.argv) != 9:
        raise SystemExit(
            "usage: test_e114_upload_copy_architecture.py "
            "EVIDENCE HEADER CODEC CLIENT SERVICE TEST GLOBAL_TEST POLICY"
        )
    paths = map(Path, sys.argv[1:])
    evidence_path, header_path, codec_path, client_path, service_path, test_path, global_test_path, policy_path = paths
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    assert evidence["gate"] == "E114"
    assert evidence["implementation"]["new_shared_record_id"] == 36
    assert evidence["implementation"]["maximum_update_bytes"] == 65536
    assert len(evidence["implementation"]["legacy_transfer_entries"]) == 6
    assert evidence["implementation"]["shared_recording_rpc_count"] == 0
    assert evidence["implementation"]["policy"] == {
        "executable": 158,
        "required_unimplemented": 282,
        "probed_null": 302,
    }

    header = header_path.read_text(encoding="utf-8")
    codec = codec_path.read_text(encoding="utf-8")
    client = client_path.read_text(encoding="utf-8")
    service = service_path.read_text(encoding="utf-8")
    test = test_path.read_text(encoding="utf-8")
    global_test = global_test_path.read_text(encoding="utf-8")
    policy = set(policy_path.read_text(encoding="utf-8").splitlines())
    require(header, "BVB_COMMAND_VULKAN_UPDATE_BUFFER = 36", "record id")
    require(header, "64 * 1024", "bounded update data")
    require(codec, "bvb_command_batch_append_vulkan_update_buffer", "encoder")
    require(codec, "bvb_command_decode_vulkan_update_buffer", "decoder")
    require(client, "bvb_bridge_vkCmdUpdateBuffer", "client update")
    require(service, 'device, "vkCmdUpdateBuffer"', "native update replay")
    require(test, "test_vulkan_update_buffer", "maximum update test")
    require(global_test, "recording_rtts == (shared_command_stream ? 0U : 47U)",
            "zero shared recording RPC proof")

    wrappers = {
        "vkCmdCopyBuffer": "bvb_bridge_vkCmdCopyBuffer2",
        "vkCmdCopyBufferToImage": "bvb_bridge_vkCmdCopyBufferToImage2",
        "vkCmdCopyImageToBuffer": "bvb_bridge_vkCmdCopyImageToBuffer2",
        "vkCmdCopyImage": "bvb_bridge_vkCmdCopyImage2",
        "vkCmdBlitImage": "bvb_bridge_vkCmdBlitImage2",
        "vkCmdResolveImage": "bvb_bridge_vkCmdResolveImage2",
    }
    for name, target in wrappers.items():
        require(client, f"bvb_bridge_{name}(", f"{name} wrapper")
        require(client, target, f"{name} sync2 conversion")
        assert name in policy
    assert "vkCmdUpdateBuffer" in policy
    print("PASS: E114 upload and copy command-family architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
