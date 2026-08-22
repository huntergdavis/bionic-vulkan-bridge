#!/usr/bin/env python3

import json
from pathlib import Path
import sys


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing {label}: {needle}")


def main() -> int:
    if len(sys.argv) != 10:
        raise SystemExit(
            "usage: test_e122_query_family.py EVIDENCE PROTOCOL COMMAND "
            "CLIENT SERVICE NATIVE FAKE GLOBAL_TEST POLICY"
        )
    paths = list(map(Path, sys.argv[1:]))
    evidence = json.loads(paths[0].read_text(encoding="utf-8"))
    protocol, command, client, service, native, fake, global_test, policy = (
        path.read_text(encoding="utf-8") for path in paths[1:]
    )
    assert evidence["gate"] == "E122"
    assert evidence["implementation"]["opcodes"] == {
        "create": 117,
        "destroy": 118,
        "results": 119,
        "host_reset": 120,
    }
    require(protocol, "BVB_OPCODE_VULKAN_QUERY_POOL_CREATE = 117", "create opcode")
    require(protocol, "BVB_OPCODE_VULKAN_QUERY_POOL_RESET = 120", "reset opcode")
    require(command, "BVB_COMMAND_VULKAN_QUERY = 37", "shared record")
    for name in evidence["implementation"]["host_calls"] + evidence["implementation"]["commands"]:
        require(client, f'BVB_DEVICE_MATCH("{name}"', f"client {name}")
        assert name in policy.splitlines()
    require(service, "answer_vulkan_query_pool_results(", "service results")
    require(native, 'device, "vkCreateQueryPool"', "native create")
    require(native, "query_pool_range_matches_device(", "native range ownership")
    require(fake, "fake_create_query_pool(", "fake native create")
    require(fake, "create_info->queryCount != 16384U", "exact DXVK pool size")
    require(global_test, "query_values[0] == 42U", "result round trip")
    require(global_test, "recording_rtts == (shared_command_stream ? 0U : 47U)", "A/B recording")
    assert evidence["claims"]["benchmark_completed"] is None
    print("PASS: E122 complete DXVK query family architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
