#!/usr/bin/env python3

import json
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 7:
        raise SystemExit(
            "usage: test_init_image_commands_architecture.py "
            "EVIDENCE PROTOCOL CLIENT NATIVE SERVICE DISPATCH_LIST"
        )
    evidence_path, protocol_path, client_path, native_path, service_path, dispatch_path = map(
        pathlib.Path, sys.argv[1:]
    )
    evidence = json.loads(evidence_path.read_text())
    assert evidence["gate"] == "E069"
    assert evidence["wire"] == {
        "opcodes": {
            "vkCmdPipelineBarrier2": 102,
            "vkCmdClearColorImage": 103,
        },
        "barrier_request_bytes": 48,
        "clear_request_bytes": 16,
        "representation": "typed command-buffer ID, one to four typed image IDs, count, and zeroed reserved slots; all Vulkan constants are validated and reconstructed rather than transmitted",
        "pointers_crossed": False,
        "native_reconstruction": "the Bionic service resolves same-device native handles and reconstructs local VkDependencyInfo, VkImageMemoryBarrier2 array, VkClearColorValue, and VkImageSubresourceRange records",
    }
    assert evidence["dispatch_policy"]["counts"] == {
        "executable": 88,
        "required_unimplemented": 352,
        "probed_null": 302,
    }
    assert evidence["source"]["khr_alias_in_pinned_loader"] is False
    assert evidence["next_runtime_boundary"]["exact_eager_unimplemented_call"] is None
    assert evidence["excluded"]["opcode_104"] == "left unassigned"

    protocol = protocol_path.read_text()
    assert "BVB_OPCODE_VULKAN_COMMAND_BUFFER_IMAGE_BARRIER = 102" in protocol
    assert "BVB_OPCODE_VULKAN_COMMAND_BUFFER_CLEAR_COLOR_IMAGE = 103" in protocol
    assert "BVB_VULKAN_INIT_IMAGE_MAX_BARRIERS = 4" in protocol
    assert "BVB_VULKAN_COMMAND_BUFFER_IMAGE_BARRIER_REQUEST_SIZE = 48" in protocol
    assert "BVB_VULKAN_COMMAND_BUFFER_CLEAR_COLOR_IMAGE_REQUEST_SIZE = 16" in protocol

    client = client_path.read_text()
    assert 'BVB_DEVICE_MATCH("vkCmdPipelineBarrier2"' in client
    assert 'BVB_DEVICE_MATCH("vkCmdPipelineBarrier2KHR"' not in client
    assert 'BVB_DEVICE_MATCH("vkCmdClearColorImage"' in client
    assert "bvb_command_batch_append_vulkan_image_barrier_2" in client
    assert "BVB_OPCODE_VULKAN_COMMAND_BUFFER_IMMEDIATE_RECORD" in client
    assert "init_image_subresource_range_supported" in client

    native = native_path.read_text()
    assert 'command_device, "vkCmdPipelineBarrier2"' in native
    assert 'command_device, "vkCmdClearColorImage"' in native
    assert "VkImageMemoryBarrier2 image_barriers[" in native
    assert "const VkDependencyInfo dependency" in native
    assert "const VkClearColorValue color = {0}" in native

    service = service_path.read_text()
    assert "answer_vulkan_command_buffer_image_barrier" in service
    assert "answer_vulkan_command_buffer_clear_color_image" in service

    dispatch = {
        line.strip()
        for line in dispatch_path.read_text().splitlines()
        if line.strip() and not line.startswith("#")
    }
    assert "vkCmdPipelineBarrier2" in dispatch
    assert "vkCmdPipelineBarrier2KHR" not in dispatch
    assert "vkCmdClearColorImage" in dispatch
    assert "vkCmdClearDepthStencilImage" not in dispatch
    print("PASS: E069 DXVK init-image command architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
