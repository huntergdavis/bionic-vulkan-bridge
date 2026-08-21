#!/usr/bin/env python3

import json
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 12:
        raise SystemExit(
            "usage: test_rich_command_stream_architecture.py EVIDENCE "
            "HEADER BATCH CLIENT SERVICE GLOBAL GLOBAL_TEST SINK FAKE "
            "ACTIVITY_RUNNER WRAPPER"
        )
    (
        evidence_path,
        header_path,
        batch_path,
        client_path,
        service_path,
        global_path,
        global_test_path,
        sink_path,
        fake_path,
        activity_runner_path,
        wrapper_path,
    ) = map(pathlib.Path, sys.argv[1:])

    evidence = json.loads(evidence_path.read_text())
    assert evidence["gate"] == "E076"
    assert evidence["records"] == {
        "10": "general Barrier2",
        "11": "general ClearColor",
        "maximum_image_barriers": 4,
        "maximum_clear_ranges": 4,
        "pointer_free_fixed_width": True,
    }
    assert evidence["synthetic_producer"]["colors"] == [
        "red", "green", "blue", "white"
    ]
    assert evidence["synthetic_producer"]["ring_slots"] == [0, 1, 2, 0]
    assert evidence["synthetic_producer"]["command_recording_rtts"] == 0
    assert evidence["claims"] == {
        "native_replay": True,
        "activity_import": False,
        "tablet_visible_pixels": False,
        "tomb_raider_first_frame": False,
        "fps": None,
    }

    header = header_path.read_text()
    assert "BVB_COMMAND_VULKAN_IMAGE_BARRIER_2 = 10" in header
    assert "BVB_COMMAND_VULKAN_CLEAR_COLOR_IMAGE_GENERAL = 11" in header
    assert "BVB_COMMAND_VULKAN_MAX_IMAGE_BARRIERS = 4" in header
    assert "BVB_COMMAND_VULKAN_MAX_CLEAR_RANGES = 4" in header

    batch = batch_path.read_text()
    assert "BVB_VULKAN_IMAGE_BARRIER_2_SIZE" in batch
    assert "BVB_VULKAN_CLEAR_COLOR_IMAGE_GENERAL_SIZE" in batch
    assert "image_range_wire_is_zero" in batch

    client = client_path.read_text()
    assert "bvb_command_batch_append_vulkan_image_barrier_2" in client
    assert "bvb_command_batch_append_vulkan_clear_color_image_general" in client
    assert "BVB_OPCODE_VULKAN_COMMAND_BUFFER_IMAGE_BARRIER" in client
    assert "BVB_OPCODE_VULKAN_COMMAND_BUFFER_CLEAR_COLOR_IMAGE" in client
    assert "image_owned_by_device_locked" in client

    service = service_path.read_text()
    snapshot = service.index("bvb_command_batch_snapshot(")
    validation = service.index(
        "bvb_vulkan_global_context_validate_command_stream("
    )
    generation_apply = service.index("bvb_command_stream_generations_apply(")
    replay = service.index("bvb_vulkan_global_context_replay_command_stream(")
    assert snapshot < validation < generation_apply < replay

    global_source = global_path.read_text()
    validate_rich = global_source.index(
        "BVB_COMMAND_VULKAN_IMAGE_BARRIER_2", global_source.index(
            "bvb_vulkan_global_context_validate_command_stream("
        )
    )
    replay_rich = global_source.index(
        "BVB_COMMAND_VULKAN_IMAGE_BARRIER_2", global_source.index(
            "bvb_vulkan_global_context_replay_command_stream("
        )
    )
    assert validate_rich < replay_rich
    assert "command_stream_child_matches_device(" in global_source

    global_test = global_test_path.read_text()
    assert "for (uint32_t frame = 0U; frame < 4U; ++frame)" in global_test
    assert "VK_IMAGE_LAYOUT_PRESENT_SRC_KHR" in global_test
    assert "animated_recording_rtts == 0U" in global_test
    assert "ownership_image" in global_test

    sink = sink_path.read_text()
    assert "[0, 1, 2, 0]" in sink
    assert "Mirror bvb_wsi_frame_ring_release publication order" in sink

    fake = fake_path.read_text()
    assert 'getenv("BVB_FAKE_REQUIRE_ANIMATED_WSI")' in fake
    assert "fake_animation_frame_count != 4U" in fake
    assert "fake_animation_submit_count != 4U" in fake
    activity_runner = activity_runner_path.read_text()
    assert 'parser.add_argument("--animated-rgbw", action="store_true")' in activity_runner
    assert '"--expected-service-sha256"' in activity_runner
    assert '"--expected-client-sha256"' in activity_runner
    assert '"--expected-icd-sha256"' in activity_runner
    assert '"--bridge-icd"' in activity_runner
    assert '"E076-or-newer global-dispatch producer client"' in activity_runner
    assert "validate_client_bridge_icd" in activity_runner
    assert "validate_frame_document" in activity_runner
    assert "expected_slots = [0, 1, 2, 0]" in activity_runner
    assert "len(import_matches) != 1" in activity_runner
    assert "len(present_matches) != required_present_count" in activity_runner
    assert "E057_CONSUMER_FAIL_MARKER in app_text" in activity_runner
    assert '[logcat, "-T", "1"' in activity_runner
    assert '"BVB_TEST_ANIMATED_WSI"' in activity_runner
    assert '"expected_frame_correlations": correlations' in activity_runner
    assert "E076_VISUAL_CONFIRMATION_REQUIRED" in activity_runner
    assert "metadata correlation " in activity_runner
    assert "alone is not pixel proof" in activity_runner
    wrapper = wrapper_path.read_text()
    assert "test-activity-frame-import-v40-termux.py" in wrapper
    assert "--animated-rgbw" in wrapper
    assert "--skip-build" in wrapper
    assert "BVB_V40_STAGED_APK" in wrapper
    assert "BVB_E077_SERVICE" in wrapper
    assert "BVB_E077_CLIENT" in wrapper
    assert "BVB_E077_ICD" in wrapper
    assert "214e8b112ade7a727af6748c8e4cd029f4f273ca38fef018613ca6481773a9a8" in wrapper
    assert "50a2589e2b166e8e3b796eda7839fb49c78a23a8506f357d553710fd44819024" in wrapper
    assert "e6479b4abb15cca258a44d72a674c93900ea716e7030a1643409e8bcc7049d2f" in wrapper
    assert wrapper.index('"$@"') < wrapper.index("--expected-service-sha256")
    assert wrapper.index('"$@"') < wrapper.index("--expected-client-sha256")
    assert wrapper.index('"$@"') < wrapper.index("--expected-icd-sha256")
    print("PASS: E076 rich command stream animation architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
