#!/usr/bin/env python3

import json
import pathlib
import sys


def function(source: str, name: str, next_name: str) -> str:
    start = source.index(name)
    end = source.index(next_name, start + len(name))
    return source[start:end]


def shared_precedes_control(function_source: str) -> None:
    shared = function_source.index("command_stream_is_enabled()")
    recording = function_source.index("stream_mutex", shared)
    control = function_source.index(
        "pthread_mutex_lock(&bvb_global_client.mutex)", recording
    )
    assert shared < recording < control


def main() -> int:
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: test_command_stream_client_locking.py EVIDENCE CLIENT "
            "GLOBAL_TEST GLOBAL_RUNNER"
        )
    evidence_path, client_path, test_path, runner_path = map(
        pathlib.Path, sys.argv[1:]
    )
    evidence = json.loads(evidence_path.read_text())
    assert evidence["gate"] == "E078"
    assert evidence["base"] == "e193e241844f993fb97fed5e4802e7fb9ed281f0"
    assert evidence["default_strict_unchanged"] is True
    assert evidence["wire_changes"] == []
    assert evidence["focused_concurrency"] == {
        "threads": 2,
        "distinct_command_buffers": 2,
        "commands_per_thread": 256,
        "recording_socket_exchanges": 0,
        "submit_streams": 2,
        "whole_stream_validation_and_replay": True,
    }
    assert evidence["claims"] == {
        "host_concurrency_and_replay": True,
        "tablet_deployed": False,
        "tomb_raider_fps": None,
    }

    client = client_path.read_text()
    assert "pthread_mutex_t stream_mutex;" in client
    assert "pthread_mutex_t command_stream_slots_mutex;" in client
    assert "atomic_bool command_stream_enabled;" in client
    assert "pthread_rwlock_t bvb_object_registry_lock" in client
    assert "memory_order_release" in client
    assert "memory_order_acquire" in client

    begin = function(
        client, "bvb_bridge_vkBeginCommandBuffer(",
        "bvb_bridge_vkEndCommandBuffer("
    )
    end = function(
        client, "bvb_bridge_vkEndCommandBuffer(", "destroy_resource("
    )
    barrier = function(
        client, "bvb_bridge_vkCmdPipelineBarrier2(",
        "bvb_bridge_vkCmdClearColorImage("
    )
    clear = function(
        client, "bvb_bridge_vkCmdClearColorImage(",
        "bvb_bridge_vkCmdFillBuffer("
    )
    fill = function(
        client, "bvb_bridge_vkCmdFillBuffer(", "fence_result_operation("
    )
    for hot_function in (begin, end, clear, fill):
        shared_precedes_control(hot_function)
    assert barrier.index("command_stream_is_enabled()") < barrier.index(
        "pthread_mutex_lock(&command_state->stream_mutex)"
    )
    assert "finish_single_render_record(" in barrier

    poison = function(
        client, "poison_shared_command_stream(",
        "init_image_subresource_range_supported("
    )
    assert "stream_mutex" in poison
    assert "bvb_global_client.mutex" not in poison
    assert "shared_object_owned_by_device_cached_locked(" in barrier
    assert "shared_object_owned_by_device_cached_locked(" in clear
    assert "shared_object_owned_by_device_cached_locked(" in fill
    assert "bvb_command_batch_append_vulkan_image_barrier_2" in barrier
    assert "finish_single_render_record(" in barrier
    assert "BVB_OPCODE_VULKAN_COMMAND_BUFFER_CLEAR_COLOR_IMAGE" in clear
    assert "BVB_OPCODE_VULKAN_COMMAND_BUFFER_FILL" in fill

    submit = function(
        client, "bvb_bridge_vkQueueSubmit2(",
        "bvb_bridge_vkQueueWaitIdle("
    )
    assert submit.index("pthread_mutex_lock(&bvb_global_client.mutex)") < \
        submit.index("pthread_mutex_lock(&state->stream_mutex)")
    assert "release_command_stream_slot(state);" in submit

    global_test = test_path.read_text()
    assert "pthread_barrier_init(&start, NULL, 3U)" in global_test
    assert "for (uint32_t index = 0U; index < 256U; ++index)" in global_test
    assert "commandBufferInfoCount = 2U" in global_test
    assert "bvb_global_dispatch_exchange_count() ==" in global_test
    assert "ownership_buffer" in global_test
    assert "ownership_image" in global_test

    runner = runner_path.read_text()
    assert '"shared-command-stream-concurrency"' in runner
    assert 'environment["BVB_TEST_CONCURRENT_COMMAND_STREAMS"] = "1"' in runner
    assert '"concurrent_streams=2 concurrent_commands=512"' in runner
    print("PASS: E078 per-command-buffer shared-stream client locking")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
