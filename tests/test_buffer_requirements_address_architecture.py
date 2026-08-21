#!/usr/bin/env python3

import json
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 7:
        raise SystemExit(
            "usage: test_buffer_requirements_address_architecture.py "
            "EVIDENCE PROTOCOL CLIENT NATIVE SERVICE DISPATCH"
        )
    evidence_path, protocol_path, client_path, native_path, service_path, dispatch_path = (
        pathlib.Path(value) for value in sys.argv[1:]
    )
    evidence = json.loads(evidence_path.read_text())
    assert evidence["gate"] == "E068"
    assert evidence["result"] == "pass_host_contract"
    assert evidence["source"]["dxvk_commit"] == (
        "a6764047e587178283fcde4073ae6e1410af594f"
    )
    assert evidence["wire"]["opcodes"] == {
        "vkGetBufferMemoryRequirements2": 78,
        "vkGetBufferDeviceAddress": 79,
    }
    assert evidence["dispatch_policy"]["counts"] == {
        "executable": 86,
        "required_unimplemented": 354,
        "probed_null": 302,
    }
    assert evidence["verification"]["hardware_or_game_claim"] is False

    protocol = protocol_path.read_text()
    assert "BVB_OPCODE_VULKAN_BUFFER_REQUIREMENTS_2 = 78" in protocol
    assert "BVB_OPCODE_VULKAN_BUFFER_DEVICE_ADDRESS = 79" in protocol
    assert "BVB_VULKAN_BUFFER_REQUIREMENTS_2_REQUEST_SIZE = 16" in protocol
    assert "BVB_VULKAN_BUFFER_REQUIREMENTS_2_RESPONSE_SIZE = 32" in protocol
    assert "BVB_VULKAN_BUFFER_DEVICE_ADDRESS_REQUEST_SIZE = 8" in protocol
    assert "BVB_VULKAN_BUFFER_DEVICE_ADDRESS_RESPONSE_SIZE = 8" in protocol
    assert "BVB_VULKAN_MAX_MEMORY_ALLOCATION_SIZE = 256U * 1024U * 1024U" in protocol

    client = client_path.read_text()
    assert 'BVB_DEVICE_MATCH("vkGetBufferMemoryRequirements2"' in client
    assert 'BVB_DEVICE_MATCH("vkGetBufferDeviceAddress"' in client
    assert 'BVB_DEVICE_MATCH("vkGetBufferMemoryRequirements2KHR"' not in client
    assert 'BVB_DEVICE_MATCH("vkGetBufferDeviceAddressKHR"' not in client
    assert 'BVB_DEVICE_MATCH("vkGetBufferDeviceAddressEXT"' not in client
    assert "BVB_OPCODE_VULKAN_BUFFER_REQUIREMENTS_2" in client
    assert "BVB_OPCODE_VULKAN_BUFFER_DEVICE_ADDRESS" in client
    assert "create_info->size > BVB_VULKAN_MAX_MEMORY_ALLOCATION_SIZE" in client
    assert "!buffer_state->memory_bound" in client
    assert "resource_proxy_locked(\n        buffer_id, BVB_OBJECT_BUFFER)" in client

    native = native_path.read_text()
    assert 'device, "vkGetBufferMemoryRequirements2"' in native
    assert 'device, "vkGetBufferDeviceAddress"' in native
    assert 'device, "vkGetBufferMemoryRequirements2KHR"' not in native
    assert 'device, "vkGetBufferDeviceAddressKHR"' not in native
    assert "resolve_device_child(\n        context, request->buffer_id, BVB_OBJECT_BUFFER" in native
    assert "VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS" in native
    assert "request->size > BVB_VULKAN_MAX_MEMORY_ALLOCATION_SIZE" in native
    assert "!metadata->memory_bound" in native

    service = service_path.read_text()
    assert "answer_vulkan_buffer_requirements_2" in service
    assert "answer_vulkan_buffer_device_address" in service

    dispatch = set(dispatch_path.read_text().splitlines())
    assert "vkGetBufferMemoryRequirements2" in dispatch
    assert "vkGetBufferDeviceAddress" in dispatch
    assert "vkGetBufferMemoryRequirements2KHR" not in dispatch
    assert "vkGetBufferDeviceAddressKHR" not in dispatch
    assert "vkGetBufferDeviceAddressEXT" not in dispatch

    print("PASS: E068 buffer requirements2 and device address architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
