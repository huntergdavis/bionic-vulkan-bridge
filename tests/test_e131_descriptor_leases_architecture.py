#!/usr/bin/env python3

import json
from pathlib import Path
import sys


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing {label}: {needle}")


def main() -> int:
    if len(sys.argv) != 8:
        raise SystemExit(
            "usage: test_e131_descriptor_leases_architecture.py "
            "EVIDENCE RING_HEADER RING_SOURCE CLIENT SERVICE RING_TEST GLOBAL_TEST"
        )
    evidence_path, header_path, source_path, client_path, service_path, ring_test_path, global_test_path = map(
        Path, sys.argv[1:]
    )
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    header = header_path.read_text(encoding="utf-8")
    source = source_path.read_text(encoding="utf-8")
    client = client_path.read_text(encoding="utf-8")
    service = service_path.read_text(encoding="utf-8")
    ring_test = ring_test_path.read_text(encoding="utf-8")
    global_test = global_test_path.read_text(encoding="utf-8")

    assert evidence["gate"] == "E131"
    assert evidence["implementation"]["lease_bank_count"] == 4
    assert evidence["implementation"]["sets_per_bank"] == 512
    assert evidence["implementation"]["steady_state_socket_exchanges_per_lease_hit"] == 0
    assert evidence["implementation"]["steady_state_ring_calls_per_lease_hit"] == 0
    assert evidence["implementation"]["native_allocation_result_fabricated"] is False
    assert evidence["claims"]["tablet_deployed"] is False

    require(header, "BVB_DESCRIPTOR_TRANSACTION_RING_VERSION = 4", "lease ABI successor")
    require(header, "BVB_DESCRIPTOR_TRANSACTION_RING_REGION_BYTES = 131072", "bounded successor region")
    require(header, "BVB_DESCRIPTOR_LEASE_BANK_COUNT = 64", "bounded successor banks")
    require(header, "BVB_DESCRIPTOR_LEASE_BANK_CAPACITY = 64", "bounded successor capacity")
    require(source, "bvb_descriptor_lease_bank_publish(", "release publication")
    require(source, "bvb_descriptor_lease_claim(", "typed local claim")
    require(client, "descriptor_lease_claimed", "local dispatch branch")
    require(client, "descriptor_ring_call_count", "zero-ring counter")
    require(client, "BVB_E131_DESCRIPTOR_LEASE_PROFILE", "runtime hit profile")
    require(service, "descriptor_lease_allocate_live_batch(", "successful-call successor")
    require(service, "candidate->batch_repetitions = 2U", "reset cold state")
    require(service, "descriptor_lease_forget_pool(", "destroy invalidation")
    require(service, "bvb_vulkan_global_context_allocate_descriptor_sets(", "real native allocation")
    require(ring_test, "wrong_layout", "mismatch cursor proof")
    require(global_test, "ring_calls_before_descriptor_lease", "zero ring call proof")
    require(global_test, "ring_calls_before_descriptor_lease + 3U", "cold reset proof")
    print("PASS: E131 bounded descriptor reset-epoch leases")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
