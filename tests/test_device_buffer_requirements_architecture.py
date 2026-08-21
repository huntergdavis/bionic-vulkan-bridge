#!/usr/bin/env python3

import json
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 7:
        raise SystemExit(
            "usage: test_device_buffer_requirements_architecture.py "
            "EVIDENCE PROTOCOL CLIENT NATIVE SERVICE DISPATCH_LIST"
        )
    evidence_path, protocol_path, client_path, native_path, service_path, dispatch_path = map(
        pathlib.Path, sys.argv[1:]
    )
    evidence = json.loads(evidence_path.read_text())
    assert evidence["gate"] == "E066"
    assert evidence["wire"] == {
        "opcode": 76,
        "request_bytes": 64,
        "response_bytes": 32,
        "maximum_queue_families": 8,
        "representation": "fixed-width little-endian scalars and one typed device ID",
        "pointers_crossed": False,
        "native_reconstruction": "the Bionic service reconstructs local VkBufferCreateInfo, VkDeviceBufferMemoryRequirements, VkMemoryRequirements2, and VkMemoryDedicatedRequirements records",
    }
    assert evidence["dispatch_policy"]["counts"] == {
        "executable": 84,
        "required_unimplemented": 356,
        "probed_null": 302,
    }
    assert evidence["source"]["khr_alias_in_pinned_loader"] is False
    assert evidence["next_runtime_boundary"]["exact_eager_unimplemented_call"] is None

    protocol = protocol_path.read_text()
    assert "BVB_OPCODE_VULKAN_DEVICE_BUFFER_REQUIREMENTS = 76" in protocol
    assert "BVB_VULKAN_DEVICE_BUFFER_REQUIREMENTS_REQUEST_SIZE = 64" in protocol
    assert "BVB_VULKAN_DEVICE_BUFFER_REQUIREMENTS_RESPONSE_SIZE = 32" in protocol
    assert "BVB_VULKAN_DEVICE_BUFFER_MAX_QUEUE_FAMILIES = 8" in protocol

    client = client_path.read_text()
    assert 'BVB_DEVICE_MATCH("vkGetDeviceBufferMemoryRequirements"' in client
    assert 'BVB_DEVICE_MATCH("vkGetDeviceBufferMemoryRequirementsKHR"' not in client
    assert "device_buffer_create_info_supported" in client
    assert "device_buffer_dedicated_output" in client

    native = native_path.read_text()
    assert 'device, "vkGetDeviceBufferMemoryRequirements"' in native
    assert "VkDeviceBufferMemoryRequirements info" in native
    assert "VkMemoryDedicatedRequirements dedicated" in native

    service = service_path.read_text()
    assert "answer_vulkan_device_buffer_requirements" in service
    assert "BVB_VULKAN_DEVICE_BUFFER_REQUIREMENTS_REQUEST_SIZE" in service
    assert "BVB_VULKAN_DEVICE_BUFFER_REQUIREMENTS_RESPONSE_SIZE" in service

    dispatch = {
        line.strip()
        for line in dispatch_path.read_text().splitlines()
        if line.strip() and not line.startswith("#")
    }
    assert "vkGetDeviceBufferMemoryRequirements" in dispatch
    assert "vkGetDeviceBufferMemoryRequirementsKHR" not in dispatch
    assert "vkGetImageMemoryRequirements2" in dispatch
    print("PASS: E066 device buffer requirements architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
