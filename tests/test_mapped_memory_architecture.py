#!/usr/bin/env python3

import json
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 9:
        raise SystemExit(
            "usage: test_mapped_memory_architecture.py EVIDENCE HEADER "
            "CLIENT SERVICE NATIVE GLOBAL_TEST DRIVER TEST_RUNNER"
        )
    (
        evidence_path,
        header_path,
        client_path,
        service_path,
        native_path,
        global_test_path,
        driver_path,
        test_runner_path,
    ) = map(pathlib.Path, sys.argv[1:])

    evidence = json.loads(evidence_path.read_text())
    assert evidence["gate"] == "E077"
    assert evidence["wire"] == {
        "setup": {"opcode": 106, "bytes": 40, "fd_transfers": 1},
        "flush": {"opcode": 107, "bytes": 40, "fd_transfers": 0},
        "invalidate": {"opcode": 108, "bytes": 40, "fd_transfers": 0},
        "unmap": {"opcode": 109, "bytes": 24, "fd_transfers": 0},
        "pointer_values_crossed": False,
    }
    assert evidence["ab_contract"] == {
        "strict_memory_rtts": [2, 2, 2, 2, 3],
        "eligible_shared_memory_rtts": [1, 1, 1, 1, 1],
        "order": ["map", "flush", "invalidate", "unmap", "submit"],
        "eligible_shared_opcodes": [106, 107, 108, 109, 47],
        "ineligible_hybrid_rtts": [2, 2],
        "ineligible_hybrid_opcodes": [49, 48],
        "eligible_submit_memory_write_rtts": 0,
    }
    assert evidence["limits"] == {
        "live_mirrors": 64,
        "total_bytes": 268435456,
        "exact_size_fd": True,
        "immutable_capacity_seals": True,
    }
    assert evidence["noncoherent_upload"]["map_implicitly_invalidates_or_reads_native"] is False
    assert evidence["noncoherent_upload"]["unmap_implicitly_flushes"] is False
    assert evidence["mirror"]["device_to_host_completion_refresh"] is False
    assert evidence["uncertain_transport"]["reconnect_with_stale_proxies"] is False
    assert evidence["claims"] == {
        "eligible_upload_transport_contract": True,
        "general_vulkan_map_semantics": False,
        "tablet_deployment": False,
        "visible_frame": False,
        "tomb_raider_run": False,
        "fps": None,
    }

    header = header_path.read_text()
    for marker in (
        "BVB_OPCODE_VULKAN_MEMORY_MIRROR_SETUP = 106",
        "BVB_OPCODE_VULKAN_MEMORY_MIRROR_FLUSH = 107",
        "BVB_OPCODE_VULKAN_MEMORY_MIRROR_INVALIDATE = 108",
        "BVB_OPCODE_VULKAN_MEMORY_MIRROR_UNMAP = 109",
        "BVB_VULKAN_MEMORY_MIRROR_SETUP_SIZE = 40",
        "BVB_VULKAN_MEMORY_MIRROR_RANGE_SIZE = 40",
        "BVB_VULKAN_MEMORY_MIRROR_UNMAP_SIZE = 24",
        "BVB_VULKAN_MEMORY_MIRROR_CAPACITY = 64",
    ):
        assert marker in header

    client = client_path.read_text()
    assert 'getenv("BVB_MAPPED_MEMORY")' in client
    assert "exchange_pass_fd_locked(&request, &response, memory_fd)" in client
    assert "F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL" in client
    assert "!state->mapped_shared" in client
    assert "if (shared_mapping) atomic_thread_fence(memory_order_release);" in client
    client_classifier = client[
        client.index("static bool buffer_usage_is_upload_only("):
        client.index("static bool memory_is_upload_only_locked(")
    ]
    assert "VK_BUFFER_USAGE_TRANSFER_SRC_BIT" in client_classifier
    assert "VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT" not in client_classifier
    assert "memory_is_upload_only_locked(state->wire_id)" in client
    assert "!memory_state->mapped_shared ||\n         buffer_usage_is_upload_only" in client
    assert "!memory_state->mapped_shared" in client
    assert "connection_poisoned" in client
    assert "if (bvb_global_client.connection_poisoned) return -EPIPE;" in client
    assert 'getenv("BVB_TEST_DROP_MEMORY_UNMAP_ACK")' in client

    service = service_path.read_text()
    assert "BVB_OPCODE_VULKAN_MEMORY_MIRROR_SETUP" in service
    assert "answer_vulkan_memory_mirror_setup(" in service
    assert "answer_vulkan_memory_mirror_range(" in service
    assert "answer_vulkan_memory_mirror_unmap(" in service

    native = native_path.read_text()
    assert "(uint64_t)metadata.st_size != request->length" in native
    assert "(seals & required_seals) != required_seals" in native
    assert "(seals & F_SEAL_WRITE) != 0" in native
    assert "memory mirror references unknown or cross-device memory" in native
    native_classifier_start = native.index(
        "static bool buffer_usage_is_upload_only(uint32_t usage) {"
    )
    native_classifier = native[
        native_classifier_start:
        native.index("static bool memory_is_upload_only(", native_classifier_start)
    ]
    assert "VK_BUFFER_USAGE_TRANSFER_SRC_BIT" in native_classifier
    assert "VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT" not in native_classifier
    assert "memory mirror is not bound only to GPU-read-only buffers" in native
    assert "upload_host_diverged_range(" in native
    assert "refresh_coherent_memory_mirrors" not in native
    assert "sync_coherent_memory_mirrors(context, device_id)" in native
    unmap = native[native.index("bvb_vulkan_global_context_unmap_memory_mirror("):]
    before_sync = unmap[:unmap.index("sync_coherent_memory_mirrors(")]
    assert "VK_MEMORY_PROPERTY_HOST_COHERENT_BIT" in before_sync
    assert "maintain_native_memory_range(context, mirror, false)" not in before_sync

    global_test = global_test_path.read_text()
    assert "memory_rtts=%" in global_test
    assert "mapped[0] = UINT8_C(0x44)" in global_test
    assert "ineligible_memory_rtts=" in global_test
    assert "bind_buffer_memory(device, unsafe_buffer, upload_memory" in global_test
    assert "bind_image_memory(device, unsafe_image, upload_memory" in global_test
    assert ".offset = 257U" in global_test
    assert ".size = 7U" in global_test
    assert "bvb_verify_memory_fill(" in global_test
    driver = driver_path.read_text()
    assert 'getenv("BVB_FAKE_REQUIRE_MEMORY_MIRROR")' in driver
    assert 'getenv("BVB_FAKE_NONCOHERENT_MEMORY")' in driver
    runner = test_runner_path.read_text()
    assert '"memory_rtts=1,1,1,1,1"' in runner
    assert '"memory_rtts=2,2,2,2,3"' in runner
    assert '"memory_opcodes=106,107,108,109,47"' in runner
    assert '"ineligible_memory_opcodes=49,48"' in runner
    assert "poison_retry_rtts=0" in runner
    print("PASS: E077 upload-only mapped-memory mirror architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
