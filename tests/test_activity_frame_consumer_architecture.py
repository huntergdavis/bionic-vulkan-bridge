#!/usr/bin/env python3

import json
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 10:
        raise SystemExit(
            "usage: test_activity_frame_consumer_architecture.py EVIDENCE "
            "DECISION SETUP_HEADER CLIENT RECEIVER PROVIDER NATIVE GLOBAL "
            "VULKAN_GLOBAL"
        )
    evidence_path, decision_path, setup_path, client_path, receiver_path, provider_path, native_path, global_path, vulkan_global_path = map(
        pathlib.Path, sys.argv[1:]
    )
    evidence = json.loads(evidence_path.read_text())
    assert evidence["result"] == "pass"
    assert evidence["visible_output"] is False
    assert evidence["public_swapchain_success"] is False
    assert evidence["setup_transport"]["native_envelope_bytes"] == 128
    assert evidence["frame_loop"]["java_calls"] == 0
    assert evidence["frame_loop"]["binder_calls"] == 0
    assert evidence["frame_loop"]["fd_transfers"] == 0

    decision = " ".join(decision_path.read_text().split())
    assert "tablet runtime proof pending" in decision
    assert "Java, Binder, socket calls, and FD transfer remain setup-only" in decision
    assert "VK_QUEUE_FAMILY_EXTERNAL" in decision

    setup = setup_path.read_text()
    assert "BVB_ACTIVITY_FRAME_SETUP_BYTES = 128" in setup
    assert "image_count image-memory FDs followed by" in setup

    client = client_path.read_text()
    assert "getPeerCredentials" in client and "Process.myUid()" in client
    assert "TRANSACTION_REQUEST_GAME_FRAME_RING" in client
    assert "same_uid_scm_rights_then_binder_reply" in client

    receiver = receiver_path.read_text()
    assert "nativeInstallFrameTransport" in receiver
    assert "TRANSACTION_GAME_FRAME_RING_RESULT" in receiver
    provider = provider_path.read_text()
    assert "nativeInstallFrameTransport" in provider

    native = native_path.read_text()
    for primitive in (
        "VkImportMemoryFdInfoKHR",
        "vkGetMemoryFdPropertiesKHR",
        "bvb_wsi_frame_ring_wait_present",
        "vkAcquireNextImageKHR",
        "vkCmdCopyImage",
        "vkCmdBlitImage",
        "vkQueueSubmit",
        "vkQueuePresentKHR",
        "vkWaitForFences",
        "bvb_wsi_frame_ring_release",
        "VK_QUEUE_FAMILY_EXTERNAL",
        "activity_resumed",
        "activity_window_present",
    ):
        assert primitive in native
    assert "E057_FRAME_TRANSPORT_IMPORTED" in native

    global_source = global_path.read_text()
    assert "bvb_wsi_frame_ring_fail_producer" in vulkan_global_path.read_text()
    create_start = global_source.index("bvb_bridge_vkCreateSwapchainKHR")
    create_end = global_source.index("bvb_bridge_vkDestroySwapchainKHR", create_start)
    create = global_source[create_start:create_end]
    assert "return VK_ERROR_FEATURE_NOT_PRESENT" in create
    print("PASS: Activity frame import/consumer architecture remains fail-closed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
