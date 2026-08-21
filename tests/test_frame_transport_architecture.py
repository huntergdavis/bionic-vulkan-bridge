#!/usr/bin/env python3

import json
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 6:
        raise SystemExit(
            "usage: test_frame_transport_architecture.py EVIDENCE DECISION "
            "RING GLOBAL SERVICE"
        )
    evidence_path, decision_path, ring_path, global_path, service_path = map(
        pathlib.Path, sys.argv[1:]
    )
    evidence = json.loads(evidence_path.read_text())
    assert evidence["result"] == "pass"
    assert evidence["visible_output"] is False
    assert evidence["allocation"]["dummy_swapchain_handles"] is False
    assert evidence["frame_loop"]["java_calls"] == 0
    assert evidence["frame_loop"]["binder_calls"] == 0
    assert evidence["frame_loop"]["fd_transfers"] == 0
    assert evidence["setup_transport"]["descriptor_messages"] == 1

    decision = decision_path.read_text()
    normalized_decision = " ".join(decision.split())
    assert "Activity import/copy/present" in normalized_decision
    assert "game-facing producer implementation complete" in normalized_decision
    assert "Treat host success as transport evidence only" in normalized_decision
    assert "producer-local GPU completion" in normalized_decision
    assert "Activity-local GPU completion" in normalized_decision

    ring = ring_path.read_text()
    assert "SYS_futex" in ring
    assert "BVB_WSI_SLOT_ACQUIRED" in ring
    assert "BVB_WSI_SLOT_PRESENTED" in ring
    assert "Binder" not in ring and "Java" not in ring

    global_source = global_path.read_text()
    assert "VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT" in global_source
    assert "VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO" in global_source
    assert "vkGetMemoryFdKHR" in global_source
    assert "bvb_wsi_frame_ring_initialize" in global_source

    service = service_path.read_text()
    assert "BVB_ACTIVITY_RENDERER_READY" in service
    assert "BVB_ACTIVITY_WINDOW_PRESENT" in service
    assert "bvb_transport_send_fds" in service
    print("PASS: persistent frame transport architecture invariants")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
