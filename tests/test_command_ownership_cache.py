#!/usr/bin/env python3

import json
import pathlib
import sys


def function(source: str, name: str, next_name: str) -> str:
    start = source.index(name)
    end = source.index(next_name, start + len(name))
    return source[start:end]


def main() -> int:
    if len(sys.argv) != 6:
        raise SystemExit(
            "usage: test_command_ownership_cache.py EVIDENCE HEADER CLIENT "
            "GLOBAL_TEST GLOBAL_RUNNER"
        )
    evidence_path, header_path, client_path, test_path, runner_path = map(
        pathlib.Path, sys.argv[1:]
    )
    evidence = json.loads(evidence_path.read_text())
    assert evidence["gate"] == "E080"
    assert evidence["base"] == "b89e152d792f25c406b10babc4e042bdf3369f94"
    assert evidence["default_strict_unchanged"] is True
    assert evidence["wire_changes"] == []
    assert evidence["cache"] == {
        "scope": "per-command-buffer recording",
        "kind": "positive direct-mapped typed ownership",
        "capacity": 16,
        "cleared_on_begin": True,
        "cleared_on_successful_command_pool_reset": True,
        "miss_validation": "existing registry rwlock and typed same-device lookup",
        "collision_behavior": "exact mismatch revalidates and replaces",
    }
    assert evidence["focused_concurrency"] == {
        "threads": 2,
        "distinct_command_buffers": 2,
        "commands_per_thread": 256,
        "client_registry_reads_per_command_buffer": 1,
        "client_registry_reads_total": 2,
        "recording_socket_exchanges": 0,
    }
    assert evidence["claims"] == {
        "client_lookup_reduction": True,
        "native_validation_count": None,
        "tablet_deployed": False,
        "tomb_raider_fps": None,
    }
    fail_closed = evidence["fail_closed"]
    assert fail_closed["command_pool_reset_next_reference_registry_reads"] == 1
    assert fail_closed["destroy_after_end_before_submit_rejects"] is True
    assert fail_closed["destroy_after_end_native_memory_sentinel_unchanged"] is True

    header = header_path.read_text()
    assert "bvb_command_buffer_ownership_registry_reads(" in header

    client = client_path.read_text()
    assert "BVB_COMMAND_OWNERSHIP_CACHE_CAPACITY = 16U" in client
    assert "struct bvb_command_ownership_cache_entry" in client
    assert "ownership_cache[BVB_COMMAND_OWNERSHIP_CACHE_CAPACITY]" in client
    assert "uint64_t ownership_registry_reads;" in client
    helper = function(
        client,
        "shared_object_owned_by_device_cached_locked(",
        "BVB_GLOBAL_EXPORT uint64_t bvb_buffer_proxy_id(",
    )
    cache_hit = helper.index("cached->wire_id == wire_id")
    stream_release = helper.index("pthread_mutex_unlock", cache_hit)
    registry_read = helper.index("pthread_rwlock_rdlock", stream_release)
    registry_release = helper.index("pthread_rwlock_unlock", registry_read)
    stream_reacquire = helper.index("pthread_mutex_lock", registry_release)
    registry_count = helper.index(
        "++command_state->ownership_registry_reads", stream_reacquire
    )
    positive_insert = helper.index("if (owned)", registry_count)
    assert (
        cache_hit < stream_release < registry_read < registry_release
        < stream_reacquire < registry_count < positive_insert
    )
    assert "bvb_handle_expect(wire_id, type)" in helper
    assert "proxy->parent_id == command_state->device_id" in helper
    assert "image_owned_by_device_locked(" in helper

    reset = function(
        client, "reset_command_stream_state(",
        "reset_command_streams_for_pool_locked("
    )
    assert "memset(proxy->ownership_cache, 0" in reset
    assert "proxy->ownership_registry_reads = 0U" in reset

    for name, next_name in (
        ("bvb_bridge_vkCmdPipelineBarrier2(",
         "bvb_bridge_vkCmdClearColorImage("),
        ("bvb_bridge_vkCmdClearColorImage(",
         "bvb_bridge_vkCmdFillBuffer("),
        ("bvb_bridge_vkCmdFillBuffer(", "fence_result_operation("),
    ):
        command = function(client, name, next_name)
        stream_lock = command.index("pthread_mutex_lock(&command_state->stream_mutex)")
        cached_lookup = command.index(
            "shared_object_owned_by_device_cached_locked(", stream_lock
        )
        assert stream_lock < cached_lookup
    assert "BVB_OPCODE_VULKAN_COMMAND_BUFFER_IMAGE_BARRIER" in client
    assert "BVB_OPCODE_VULKAN_COMMAND_BUFFER_CLEAR_COLOR_IMAGE" in client
    assert "BVB_OPCODE_VULKAN_COMMAND_BUFFER_FILL" in client

    global_test = test_path.read_text()
    assert "for (uint32_t index = 0U; index < 256U; ++index)" in global_test
    assert "CHECK(reads == 1U);" in global_test
    assert "collision_buffers[17]" in global_test
    assert "CHECK(collision_ownership_registry_reads == 4U);" in global_test
    assert "CHECK(rerecord_ownership_registry_reads == 1U);" in global_test
    assert "reset_command_pool(device, cache_pool, 0U)" in global_test
    assert "CHECK(pool_reset_ownership_registry_reads == 1U);" in global_test
    assert "stale_resource_rejected" in global_test
    assert "stale_native_replay_blocked" in global_test
    assert "UINT32_C(0xdeadcafe)" in global_test
    assert "UINT32_C(0xa5c3f00d)" in global_test
    assert "ownership_buffer" in global_test
    assert "ownership_image" in global_test

    runner = runner_path.read_text()
    assert '"shared-command-stream-concurrency"' in runner
    assert (
        '"concurrent_registry_reads=2 collision_registry_reads=4 "'
        in runner
    )
    assert '"rerecord_registry_reads=1 pool_reset_registry_reads=1 "' in runner
    assert (
        '"stale_resource_rejected=1 stale_native_replay_blocked=1"'
        in runner
    )
    print("PASS: E080 per-command-buffer positive ownership cache")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
