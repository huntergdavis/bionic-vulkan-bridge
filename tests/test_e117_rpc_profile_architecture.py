#!/usr/bin/env python3

import json
from pathlib import Path
import sys


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing {label}: {needle}")


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: test_e117_rpc_profile_architecture.py "
            "EVIDENCE CLIENT PROTOCOL"
        )
    evidence_path, client_path, protocol_path = map(Path, sys.argv[1:])
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    client = client_path.read_text(encoding="utf-8")
    protocol = protocol_path.read_text(encoding="utf-8")
    assert evidence["gate"] == "E117"
    assert evidence["implementation"]["summary_window_presents"] == 32
    assert evidence["implementation"]["top_opcode_count"] == 8
    assert evidence["implementation"]["per_rpc_log_lines"] == 0
    assert evidence["implementation"]["wire_change"] == "none"
    require(client, "BVB_E117_RPC_PROFILE", "bounded RPC summary")
    require(client, "frame_profile_record_rpc_locked", "central recorder")
    require(client, "frame_profile_rpc_counts[BVB_OPCODE_LAST + 1U]", "count array")
    require(client, "frame_profile_rpc_total_ns[BVB_OPCODE_LAST + 1U]", "time array")
    require(client, "BVB_FRAME_PROFILE_RPC_TOP_COUNT = 8U", "top-eight bound")
    require(client, "opcode != BVB_OPCODE_VULKAN_SWAPCHAIN_PRESENT", "present window")
    require(client, "frame_profile_rpc_window_started", "post-first-present boundary")
    require(client, "write(STDERR_FILENO, line, used)", "single bounded summary write")
    assert client.count("BVB_E117_RPC_PROFILE") == 1
    assert client.count("frame_profile_record_rpc_locked(") >= 5
    require(protocol, "BVB_OPCODE_LAST", "bounded opcode inventory")
    print("PASS: E117 bounded per-opcode RPC profiler architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
