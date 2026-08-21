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
    assert "BVB_ACTIVITY_FRAME_FLAG_DMA_BUF" in setup
    assert "BVB_ACTIVITY_FRAME_FLAG_AHARDWAREBUFFER" in setup
    assert "AHardwareBuffer transport carries only the frame-ring FD" in setup

    client = client_path.read_text()
    assert "getPeerCredentials" in client and "Process.myUid()" in client
    assert "TRANSACTION_REQUEST_GAME_FRAME_RING" in client
    assert "same_uid_scm_rights_then_binder_reply" in client
    assert "setupFlags" in client and "SETUP_FLAG_DMA_BUF" in client
    assert "SETUP_FLAG_AHARDWAREBUFFER" in client
    assert "nativeReceiveHardwareBuffers" in client

    receiver = receiver_path.read_text()
    assert "nativeInstallFrameTransport" in receiver
    assert "TRANSACTION_GAME_FRAME_RING_RESULT" in receiver
    provider = provider_path.read_text()
    assert "nativeInstallFrameTransport" in provider
    assert "BVB_VISIBLE_HOST_NATIVE_LIBRARY" in provider
    assert "System.load(explicitNativeLibrary)" in provider

    native = native_path.read_text()
    for primitive in (
        "VkImportMemoryFdInfoKHR",
        "VkImportAndroidHardwareBufferInfoANDROID",
        "vkGetMemoryFdPropertiesKHR",
        "state.get_memory_fd_properties",
        'vkGetDeviceProcAddr(\n            state.device, "vkGetMemoryFdPropertiesKHR")',
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
        'E088_IMPORT_FAIL stage=renderer_ready',
        'E088_IMPORT_FAIL stage=format_blit',
        'E088_IMPORT_FAIL stage=allocation_size',
        'E090_IMPORT_FAIL stage=memory_type',
        'E088_IMPORT_FAIL stage=allocate',
        'E088_IMPORT_FAIL stage=bind',
    ):
        assert primitive in native
    frame_import = native.split("static int import_game_frame_transport(", 1)[1]
    frame_import = frame_import.split("static ", 1)[0]
    assert "VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT" in frame_import
    assert "VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID" in frame_import
    assert "state.get_memory_fd_properties(" in frame_import
    assert "VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME" in native
    assert "VkImportMemoryFdInfoKHR import_info" in frame_import
    assert ".memoryTypeIndex = consumer_memory_type" in frame_import
    assert "E057_FRAME_TRANSPORT_IMPORTED" in native
    assert "E057_FRAME_CONSUMER_STOP" in native

    global_source = global_path.read_text()
    assert "bvb_wsi_frame_ring_fail_producer" in vulkan_global_path.read_text()
    assert "BVB_OPCODE_VULKAN_SWAPCHAIN_ACQUIRE" in global_source
    assert "BVB_OPCODE_VULKAN_SWAPCHAIN_PRESENT" in global_source
    print("PASS: E057 Activity frame import/consumer architecture preserved")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
