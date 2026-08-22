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
            "usage: test_e121_descriptor_pool_reset.py EVIDENCE PROTOCOL "
            "CLIENT SERVICE NATIVE FAKE GLOBAL_TEST POLICY"
        )
    paths = list(map(Path, sys.argv[1:]))
    evidence = json.loads(paths[0].read_text(encoding="utf-8"))
    protocol, client, service, native, fake, global_test, policy = (
        path.read_text(encoding="utf-8") for path in paths[1:]
    )

    assert evidence["gate"] == "E121"
    assert evidence["implementation"]["opcode"] == 116
    require(protocol, "BVB_OPCODE_VULKAN_DESCRIPTOR_POOL_RESET = 116", "opcode")
    require(client, "bvb_bridge_vkResetDescriptorPool(", "client entry")
    require(client, "remove_descriptor_sets_for_pool_locked(pool_id);", "client retirement")
    require(client, 'BVB_DEVICE_MATCH("vkResetDescriptorPool"', "device GPA")
    require(service, "answer_vulkan_descriptor_pool_reset(", "service dispatch")
    require(native, 'device, "vkResetDescriptorPool"', "native resolution")
    require(native, "entry->parent_id == request->descriptor_pool_id", "service retirement")
    require(fake, "fake_reset_descriptor_pool(", "fake native reset")
    require(global_test, "core_set_before_reset", "first allocation")
    require(global_test, "bvb_descriptor_set_proxy_id(core_set_before_reset) == 0U", "old proxy retirement")
    require(global_test, "core_set_after_id != core_set_before_id", "reallocation")
    assert "vkResetDescriptorPool" in policy.splitlines()
    assert evidence["claims"]["present_66_boundary_fixed"] is None
    print("PASS: E121 descriptor-pool reset architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
