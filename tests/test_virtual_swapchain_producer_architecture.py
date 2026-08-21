#!/usr/bin/env python3

import json
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 7:
        raise SystemExit(
            "usage: test_virtual_swapchain_producer_architecture.py "
            "EVIDENCE DECISION PROTOCOL CLIENT NATIVE SERVICE"
        )
    evidence_path, decision_path, protocol_path, client_path, native_path, service_path = map(
        pathlib.Path, sys.argv[1:]
    )
    evidence = json.loads(evidence_path.read_text())
    assert evidence["gate"] == "E060-virtual-swapchain-producer-host"
    assert evidence["result"] == "pass"
    assert evidence["public_swapchain_host_contract"] is True
    assert evidence["tablet_runtime_proof"] is False
    assert evidence["visible_output"] is False
    assert evidence["transport"]["backing_images"] == 3
    assert evidence["transport"]["dummy_image_handles"] is False
    assert evidence["transport"]["dummy_swapchain_handles"] is False
    assert evidence["hot_path"]["java_calls"] == 0
    assert evidence["hot_path"]["binder_calls"] == 0
    assert evidence["hot_path"]["fd_transfers"] == 0

    decision = " ".join(decision_path.read_text().split())
    assert "tablet runtime proof pending" in decision
    assert "Launch wiring requirement" in decision
    assert "FrameTransportClient TOKEN RESULT_JSON SETUP_SOCKET" in decision
    assert "--activity-frame-socket SETUP_SOCKET" in decision
    assert "host contract must not be described as visible Tomb Raider output" in decision
    for reuse in ("E035", "E038", "E041", "E042", "E057"):
        assert reuse in decision

    protocol = protocol_path.read_text()
    assert "BVB_OPCODE_VULKAN_SWAPCHAIN_ACQUIRE = 100" in protocol
    assert "BVB_OPCODE_VULKAN_SWAPCHAIN_PRESENT = 101" in protocol
    assert "BVB_VULKAN_MAX_PRESENT_WAIT_SEMAPHORES = 16" in protocol

    client = client_path.read_text()
    create_start = client.index("bvb_bridge_vkCreateSwapchainKHR")
    create_end = client.index("bvb_bridge_vkDestroySwapchainKHR", create_start)
    create = client[create_start:create_end]
    assert "BVB_OPCODE_VULKAN_SWAPCHAIN_PREPARE" in create
    assert "exchange_fds_locked" in create
    assert "bvb_wsi_frame_ring_validate" in create
    assert "prepared.images[index].image_id" in create
    assert "frame_transport=e060" in create
    assert "return VK_ERROR_FEATURE_NOT_PRESENT" not in create
    assert "BVB_OPCODE_VULKAN_SWAPCHAIN_ACQUIRE" in client
    assert "BVB_OPCODE_VULKAN_SWAPCHAIN_PRESENT" in client
    assert "VK_SEMAPHORE_TYPE_BINARY" in client

    native = native_path.read_text()
    for primitive in (
        "create_swapchain_producer_resources",
        "record_swapchain_barrier",
        "VK_QUEUE_FAMILY_EXTERNAL",
        "VK_IMAGE_LAYOUT_PRESENT_SRC_KHR",
        "VK_IMAGE_LAYOUT_GENERAL",
        "bvb_wsi_frame_ring_acquire",
        "vkQueueSubmit",
        "vkWaitForFences",
        "bvb_wsi_frame_ring_present",
        "bvb_wsi_frame_ring_fail_producer",
    ):
        assert primitive in native
    present_start = native.index("bvb_vulkan_global_context_present_swapchain_image")
    present_end = native.index("bvb_vulkan_global_context_destroy_swapchain", present_start)
    present = native[present_start:present_end]
    assert present.index("result = wait(") < present.index("bvb_wsi_frame_ring_present(")

    service = service_path.read_text()
    prepare_start = service.index("answer_vulkan_swapchain_prepare")
    prepare_end = service.index("answer_vulkan_swapchain_destroy", prepare_start)
    prepare = service[prepare_start:prepare_end]
    assert "activity_frame_socket == NULL" in prepare
    assert "-ENOTCONN" in prepare
    assert "BVB_ACTIVITY_RESUMED" in prepare
    assert "bvb_activity_frame_setup_send" in prepare
    print("PASS: E060 honest virtual swapchain producer architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
