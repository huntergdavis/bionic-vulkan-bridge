#!/usr/bin/env python3

import json
from pathlib import Path
import sys


def main() -> int:
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: test_map_memory2_architecture.py EVIDENCE CLIENT TEST CONFIG"
        )
    evidence_path, client_path, test_path, config_path = map(Path, sys.argv[1:])
    evidence = json.loads(evidence_path.read_text())
    assert evidence["gate"] == "E099"
    assert evidence["trigger"]["actual_invocation_proven"] is True
    assert evidence["trigger"]["entry"] == "vkMapMemory2KHR"
    assert evidence["implementation"]["new_opcode"] is False
    assert evidence["implementation"]["additional_round_trips"] == 0
    assert evidence["validation"]["policy_counts"] == {
        "executable": 92,
        "required_unimplemented": 348,
        "probed_null": 302,
    }

    client = client_path.read_text()
    assert "bvb_bridge_vkMapMemory2(" in client
    assert "map_info->sType != VK_STRUCTURE_TYPE_MEMORY_MAP_INFO" in client
    assert "map_info->pNext != NULL" in client
    assert 'BVB_DEVICE_MATCH("vkMapMemory2", bvb_bridge_vkMapMemory2)' in client
    assert 'BVB_DEVICE_MATCH("vkMapMemory2KHR", bvb_bridge_vkMapMemory2)' in client

    test = test_path.read_text()
    assert 'vkGetDeviceProcAddr(device, "vkMapMemory2")' in test
    assert 'vkGetDeviceProcAddr(device, "vkMapMemory2KHR") == erased' in test
    assert "map_memory_2(device, &map_info" in test
    assert "VK_ERROR_MEMORY_MAP_FAILED" in test

    dispatch = {
        line.strip() for line in config_path.read_text().splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    }
    assert "vkMapMemory2" in dispatch
    assert "vkMapMemory2KHR" in dispatch
    print("PASS: E099 MapMemory2 architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
