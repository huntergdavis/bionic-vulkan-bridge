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
            "usage: test_e134_zero_speculation_descriptor_learning_architecture.py "
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

    assert evidence["gate"] == "E134"
    assert evidence["implementation"]["exact_observations_before_batch"] == 2
    assert evidence["implementation"]["cold_speculative_extras"] == 0
    assert evidence["implementation"]["first_batched_request_ordinal"] == 3
    assert evidence["implementation"]["reset_prefill_sets"] == 0
    assert evidence["implementation"]["strict_mode_changed"] is False
    assert evidence["claims"]["tablet_deployed"] is False
    assert evidence["claims"]["fps_claim"] is False

    require(service, "plan->observed_requests < 2U", "two exact observations")
    require(service, "++worker->profile_batch_cold_exact", "cold exact profile")
    require(service, "candidate->observed_requests = 0U", "reset observation state")
    require(service, "plan->batch_repetitions * 2U", "drained hot growth")
    require(global_test, "ring_calls_after_live_batch + 5U", "primary ring sequence")
    require(global_test, "lease_hits_after_live_batch + 14U", "primary hit sequence")
    require(global_test, "ring_calls_before_second_signature + 3U", "independent cold learning")
    require(global_test, "ring_calls_before_descriptor_lease + 3U", "reset cold learning")
    require(fake, "fake_core_descriptor_batch8_failed", "capacity fallback injection")
    require(global_py, 'profile["batch_cold_exact"] >= 6', "cold exact metric")
    require(global_py, 'profile["batch_fallbacks"] >= 1', "fallback metric")
    print("PASS: E134 zero-speculation descriptor learning")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
