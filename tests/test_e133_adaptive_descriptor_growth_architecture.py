#!/usr/bin/env python3

import json
from pathlib import Path
import sys


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing {label}: {needle}")


def main() -> int:
    if len(sys.argv) != 6:
        raise SystemExit(
            "usage: test_e133_adaptive_descriptor_growth_architecture.py "
            "EVIDENCE SERVICE GLOBAL_TEST FAKE_VULKAN GLOBAL_PY"
        )
    evidence_path, service_path, global_test_path, fake_path, global_py_path = map(
        Path, sys.argv[1:]
    )
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    service = service_path.read_text(encoding="utf-8")
    global_test = global_test_path.read_text(encoding="utf-8")
    fake = fake_path.read_text(encoding="utf-8")
    global_py = global_py_path.read_text(encoding="utf-8")

    assert evidence["gate"] == "E133"
    assert evidence["implementation"]["cold_repetitions"] == 2
    assert evidence["implementation"]["growth_sequence"] == [2, 4, 8, 16]
    assert evidence["implementation"]["reset_prefill_sets"] == 0
    assert evidence["implementation"]["strict_mode_changed"] is False
    assert evidence["claims"]["tablet_deployed"] is False
    assert evidence["claims"]["fps_claim"] is False

    require(service, ".batch_repetitions = 2U", "cold signature start")
    require(service, "plan->batch_repetitions * 2U", "geometric growth")
    require(service, "++worker->profile_batch_growths", "growth profile")
    require(service, "cursor_result == 0 && cursor < count", "demand proof")
    require(service, "candidate->batch_repetitions = 2U", "reset cold state")
    reset_definition = service.rfind("static int descriptor_lease_after_pool_reset(")
    reset_end = service.find("static void *descriptor_transaction_worker_main", reset_definition)
    if reset_definition < 0 or reset_end < 0:
        raise AssertionError("missing reset implementation boundary")
    reset_body = service[reset_definition:reset_end]
    if "bvb_vulkan_global_context_allocate_descriptor_sets(" in reset_body:
        raise AssertionError("reset must not speculatively allocate descriptor sets")

    require(global_test, "ring_calls_after_live_batch + 2U", "2-4-8 miss count")
    require(global_test, "lease_hits_after_live_batch + 11U", "1+3+7 lease hits")
    require(global_test, "ring_calls_before_descriptor_lease + 1U", "cold post-reset miss")
    require(fake, "allocate_info->descriptorSetCount == 2U", "cold fake allocation")
    require(fake, "allocate_info->descriptorSetCount == 4U", "warm fake allocation")
    require(fake, "allocate_info->descriptorSetCount == 8U", "hot fake allocation")
    require(global_py, 'profile["batch_growths"] >= 2', "profile growth proof")
    print("PASS: E133 adaptive descriptor signature growth")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
