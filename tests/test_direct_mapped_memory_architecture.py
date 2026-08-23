#!/usr/bin/env python3

import json
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 8:
        raise SystemExit(
            "usage: test_direct_mapped_memory_architecture.py EVIDENCE "
            "HEADER CLIENT SERVICE NATIVE DRIVER GLOBAL_RUNNER"
        )
    evidence_path, header_path, client_path, service_path, native_path, \
        driver_path, runner_path = map(pathlib.Path, sys.argv[1:])

    evidence = json.loads(evidence_path.read_text())
    assert evidence["gate"] == "E139"
    assert evidence["source_base_commit"] == \
        "462d84ab33157fec0b3078c5e6a353622501c624"
    assert evidence["prerequisite"] == {
        "gate": "E138",
        "private_turnip_raw_mmap": True,
        "bidirectional_mismatched_bytes": 0,
    }
    assert evidence["wire"] == {
        "direct_setup": {"opcode": 125, "bytes": 40, "fd_transfers": 1},
        "flush": {"socket_exchanges": 0},
        "invalidate": {"socket_exchanges": 0},
        "unmap": {"opcode": 109, "bytes": 24, "fd_transfers": 0},
        "pointer_values_crossed": False,
    }
    assert evidence["host_ab"] == {
        "order": ["map", "flush", "invalidate", "unmap", "submit"],
        "mirror_rtts": [1, 1, 1, 1, 1],
        "direct_rtts": [1, 0, 0, 1, 1],
        "direct_opcodes": [125, 0, 0, 109, 47],
        "direct_mismatched_bytes": 0,
    }
    assert evidence["validation"] == {
        "focused_contracts": "pass",
        "complete_host_suite": "101/101 pass",
    }
    assert evidence["claims"] == {
        "host_transport_contract": True,
        "tablet_bridge_deployment": False,
        "tomb_raider_run": False,
        "fps": None,
    }

    header = header_path.read_text()
    for marker in (
        "BVB_OPCODE_VULKAN_MEMORY_DIRECT_MAP_SETUP = 125",
        "BVB_VULKAN_MEMORY_ALLOCATE_PNEXT_EXPORT_OPAQUE_FD = 1U << 2",
        "BVB_VULKAN_BUFFER_CREATE_EXPORT_OPAQUE_FD = 1U << 31",
        "BVB_VULKAN_MEMORY_MIRROR_SETUP_SIZE = 40",
    ):
        assert marker in header

    client = client_path.read_text()
    for marker in (
        'strcmp(mode, "direct") == 0',
        "VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME",
        "BVB_VULKAN_MEMORY_ALLOCATE_PNEXT_EXPORT_OPAQUE_FD",
        "BVB_VULKAN_BUFFER_CREATE_EXPORT_OPAQUE_FD",
        "BVB_OPCODE_VULKAN_MEMORY_DIRECT_MAP_SETUP",
        "exchange_fds_locked(",
        "map_client_visible_memory_at_offset(",
        "state->mapped_direct = used_direct_mapping",
        "if (state->mapped_direct)",
    ):
        assert marker in client

    service = service_path.read_text()
    assert "answer_vulkan_memory_direct_map_setup(" in service
    assert "bvb_vulkan_global_context_setup_direct_memory(" in service
    assert "bvb_transport_send_fd(client_fd, &response, export_fd)" in service

    native = native_path.read_text()
    for marker in (
        "bvb_vulkan_global_context_setup_direct_memory(",
        "native_metadata->opaque_fd_exportable",
        "VK_MEMORY_PROPERTY_HOST_COHERENT_BIT",
        'device, "vkGetMemoryFdKHR"',
        "VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT",
        ".direct = true",
        "!mirror->direct",
    ):
        assert marker in native

    driver = driver_path.read_text()
    assert 'getenv("BVB_FAKE_REQUIRE_DIRECT_MAPPED_MEMORY")' in driver
    assert "VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO" in driver
    assert "fake_get_memory_fd" in driver

    runner = runner_path.read_text()
    assert '"direct-mapped-memory"' in runner
    assert '"memory_rtts=1,0,0,1,1"' in runner
    assert '"memory_opcodes=125,0,0,109,47"' in runner
    print("PASS: E139 direct mapped-memory host architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
