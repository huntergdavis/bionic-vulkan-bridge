#!/usr/bin/env python3

import json
from pathlib import Path
import sys


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing {label}: {needle}")


def main() -> int:
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: test_e116_wsi_frame_profile_architecture.py "
            "EVIDENCE CLIENT SERVICE ACTIVITY"
        )
    evidence_path, client_path, service_path, activity_path = map(
        Path, sys.argv[1:]
    )
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    client = client_path.read_text(encoding="utf-8")
    service = service_path.read_text(encoding="utf-8")
    activity = activity_path.read_text(encoding="utf-8")
    assert evidence["gate"] == "E116"
    assert evidence["implementation"]["summary_window_frames"] == 32
    require(client, 'getenv("BVB_FRAME_PROFILE")', "strict client selector")
    require(client, "BVB_E116_WSI_CLIENT_PROFILE", "client RPC summary")
    require(service, "BVB_E116_WSI_SERVICE_PROFILE", "service GPU summary")
    for field in (
        "present_fence_wait_ns",
        "present_submit_ns",
        "acquire_ring_wait_ns",
    ):
        require(service, field, f"service {field}")
    require(activity, '"bvb_frame_profile"', "Activity Intent selector")
    require(activity, "BVB_E116_WSI_ACTIVITY_PROFILE", "Activity copy summary")
    for field in (
        "profile_ring_wait_ns",
        "profile_acquire_ns",
        "profile_record_ns",
        "profile_queue_present_ns",
        "profile_completion_wait_ns",
    ):
        require(activity, field, f"Activity {field}")
    assert evidence["implementation"]["wire_change"] == "none"
    print("PASS: E116 bounded WSI frame profiler architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
