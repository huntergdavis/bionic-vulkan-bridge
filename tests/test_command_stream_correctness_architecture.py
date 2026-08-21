#!/usr/bin/env python3

import json
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 8:
        raise SystemExit(
            "usage: test_command_stream_correctness_architecture.py "
            "EVIDENCE SERVICE BATCH CLIENT BATCH_TEST GLOBAL_TEST FAKE"
        )
    evidence_path, service_path, batch_path, client_path, batch_test_path, global_test_path, fake_path = map(
        pathlib.Path, sys.argv[1:]
    )
    evidence = json.loads(evidence_path.read_text())
    assert evidence["gate"] == "E075a"
    assert evidence["service_snapshot"]["shared_mapping_revisited_after_snapshot"] is False
    assert evidence["generation_transaction"]["partial_commit_on_failure"] is False
    assert evidence["submit_retry"] == {
        "opcode_105_transport_ack_separate_from_vk_result": True,
        "slot_retired_after_clean_ack_and_native_non_success": True,
        "retry_opcode": 85,
        "stream_replay_opcode": 105,
        "forced_native_result": "VK_ERROR_OUT_OF_DEVICE_MEMORY",
    }
    assert evidence["ab_contract"] == {
        "strict_recording_socket_exchanges": 5,
        "shared_recording_socket_exchanges": 0,
        "changed": False,
    }

    service = service_path.read_text()
    snapshot_call = service.index("bvb_command_batch_snapshot(")
    snapshot_validation = service.index(
        "bvb_command_batch_validate(\n                snapshots[index]"
    )
    native_validation = service.index(
        "context, snapshots[index], command->stream_length, device_id"
    )
    generation_apply = service.index("bvb_command_stream_generations_apply(")
    native_replay = service.index("bvb_vulkan_global_context_replay_command_stream(")
    assert snapshot_call < snapshot_validation < native_validation
    assert native_validation < generation_apply < native_replay
    assert "context, snapshots[index], command->stream_length, device_id" in service
    assert "command_buffer_generation_is_live" in service

    batch = batch_path.read_text()
    transaction = batch[batch.index("int bvb_command_stream_generations_apply("):]
    assert "struct bvb_command_stream_generation *shadow = malloc(bytes);" in transaction
    assert "memcpy(shadow, generations, bytes);" in transaction
    assert transaction.index("if (result == 0) {") < transaction.index(
        "memcpy(generations, shadow, bytes);"
    )
    assert "!is_live(shadow[index].command_buffer_id, user_data)" in transaction

    client = client_path.read_text()
    submit = client[client.index("static VkResult VKAPI_CALL bvb_bridge_vkQueueSubmit2("):]
    assert "bool submit_acknowledged = false;" in submit
    assert "if (submit_acknowledged) {" in submit
    assert "if (vulkan_result == VK_SUCCESS) {" not in submit[:submit.index("bvb_bridge_vkQueueWaitIdle")]

    batch_test = batch_test_path.read_text()
    assert "CHECK(snapshot[0] != bytes[0]);" in batch_test
    assert "memcmp(generations, committed, sizeof(committed)) == 0" in batch_test
    assert "CHECK(reclaimed == 1U);" in batch_test

    global_test = global_test_path.read_text()
    assert ".flags = 0U," in global_test
    assert "BVB_OPCODE_VULKAN_QUEUE_SUBMIT_2_STREAM" in global_test
    assert "BVB_OPCODE_VULKAN_QUEUE_SUBMIT_2" in global_test
    fake = fake_path.read_text()
    assert "return VK_ERROR_OUT_OF_DEVICE_MEMORY;" in fake
    print("PASS: E075a command-stream correctness architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
