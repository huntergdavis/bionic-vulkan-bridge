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
            "usage: test_e130_descriptor_ring_active_handoff_architecture.py "
            "EVIDENCE RING_HEADER RING_SOURCE RING_TEST"
        )
    evidence_path, header_path, source_path, test_path = map(Path, sys.argv[1:])
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    header = header_path.read_text(encoding="utf-8")
    source = source_path.read_text(encoding="utf-8")
    test = test_path.read_text(encoding="utf-8")

    assert evidence["gate"] == "E130"
    assert evidence["implementation"]["ring_version"] == 2
    assert evidence["implementation"]["spin_budget_nanoseconds_per_waiter"] == 250000
    assert evidence["implementation"]["native_allocation_result_deferred"] is False
    assert evidence["claims"]["tablet_deployed"] is False
    require(header, "BVB_DESCRIPTOR_TRANSACTION_RING_VERSION = 3", "versioned ABI successor")
    require(header, "BVB_DESCRIPTOR_TRANSACTION_WAIT_SPINNING", "spinning state")
    require(header, "BVB_DESCRIPTOR_TRANSACTION_WAIT_SLEEPING", "sleeping state")
    require(header, "request_wait_state", "request waiter state")
    require(header, "completion_wait_state", "completion waiter state")
    require(source, "BVB_DESCRIPTOR_TRANSACTION_SPIN_NS = 250000", "bounded spin")
    require(source, "wait_for_sequence(", "shared wait helper")
    require(source, "wake_if_sleeping(", "conditional futex wake")
    require(source, "BVB_DESCRIPTOR_TRANSACTION_WAIT_SLEEPING\n        ? futex_wake", "sleep-only syscall")
    require(test, ".delay_first_completion = true", "forced completion sleep")
    require(test, "delay_five_ms();", "forced request sleep")
    require(test, "BVB_DESCRIPTOR_TRANSACTION_WAIT_IDLE", "idle teardown proof")
    print("PASS: E130 descriptor ring active handoff architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
