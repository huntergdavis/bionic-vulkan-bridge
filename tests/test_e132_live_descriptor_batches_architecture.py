#!/usr/bin/env python3

import json
from pathlib import Path
import sys


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing {label}: {needle}")


def main() -> int:
    if len(sys.argv) != 9:
        raise SystemExit(
            "usage: test_e132_live_descriptor_batches_architecture.py "
            "EVIDENCE RING_HEADER RING_SOURCE CLIENT SERVICE GLOBAL_TEST "
            "FAKE_VULKAN GLOBAL_PY"
        )
    (
        evidence_path,
        header_path,
        ring_source_path,
        client_path,
        service_path,
        global_test_path,
        fake_path,
        global_py_path,
    ) = map(Path, sys.argv[1:])
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    header = header_path.read_text(encoding="utf-8")
    ring_source = ring_source_path.read_text(encoding="utf-8")
    client = client_path.read_text(encoding="utf-8")
    service = service_path.read_text(encoding="utf-8")
    global_test = global_test_path.read_text(encoding="utf-8")
    fake = fake_path.read_text(encoding="utf-8")
    global_py = global_py_path.read_text(encoding="utf-8")

    assert evidence["gate"] == "E132"
    assert evidence["implementation"]["maximum_native_batch_sets"] == 16
    assert evidence["implementation"]["lease_bank_count"] == 64
    assert evidence["implementation"]["sets_per_bank"] == 64
    assert evidence["implementation"]["failed_batch_partial_sets_survive"] is False
    assert evidence["claims"]["tablet_deployed"] is False
    assert evidence["claims"]["fps_claim"] is False

    require(header, "BVB_DESCRIPTOR_TRANSACTION_RING_VERSION = 4", "successor ABI")
    require(header, "BVB_DESCRIPTOR_TRANSACTION_RING_REGION_BYTES = 131072", "bounded region")
    require(header, "BVB_DESCRIPTOR_LEASE_BANK_COUNT = 64", "signature banks")
    require(header, "BVB_DESCRIPTOR_LEASE_BANK_CAPACITY = 64", "bank capacity")
    require(ring_source, "matches = true", "multi-bank pool scan")
    require(service, "descriptor_lease_allocate_live_batch(", "live miss batch")
    require(service, "descriptor_lease_smaller_batch_set_count(", "bounded backoff")
    require(service, "VK_ERROR_OUT_OF_POOL_MEMORY", "pool-capacity fallback")
    require(service, "profile_batch_prefetched_sets", "batch profile")
    require(client, "descriptor_journal_length == 0U", "journal-order claim gate")
    require(global_test, "lease_hits_after_live_batch + 11U", "adaptive local hits")
    require(global_test, "ring_calls_before_second_signature + 1U", "second signature miss")
    require(global_test, "lease_hits_after_live_batch + 12U", "coexisting signature hit")
    require(fake, "VK_ERROR_OUT_OF_POOL_MEMORY", "native fallback fake")
    require(global_py, 'profile["batch_fallbacks"] >= 1', "fallback profile proof")
    print("PASS: E132 live descriptor signature batches")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
