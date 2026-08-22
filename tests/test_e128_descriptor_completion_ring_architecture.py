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
            "usage: test_e128_descriptor_completion_ring_architecture.py "
            "EVIDENCE PROTOCOL RING_HEADER RING_SOURCE CLIENT SERVICE "
            "GLOBAL_TEST TERMUX_BUILDER"
        )
    paths = list(map(Path, sys.argv[1:]))
    evidence = json.loads(paths[0].read_text(encoding="utf-8"))
    protocol, ring_header, ring_source, client, service, global_test, builder = (
        path.read_text(encoding="utf-8") for path in paths[1:]
    )

    assert evidence["gate"] == "E128"
    assert evidence["trigger"]["e127_descriptor_transaction_calls_per_present"] > 500
    assert evidence["implementation"]["setup_opcode"] == 124
    assert evidence["implementation"]["steady_state_socket_exchanges_per_allocation"] == 0
    assert evidence["validation"]["ring_ordered_calls"] == 4096
    assert evidence["validation"]["steady_state_transaction_exchange_count"] == 0
    assert evidence["claims"]["tablet_deployed"] is False

    require(protocol, "BVB_OPCODE_VULKAN_DESCRIPTOR_TRANSACTION_RING_SETUP = 124", "setup opcode")
    require(ring_header, "BVB_DESCRIPTOR_TRANSACTION_RING_LEASE_OFFSET = 8192", "preserved ring boundary")
    require(ring_header, "BVB_DESCRIPTOR_TRANSACTION_RING_SLOT_COUNT = 16", "bounded slots")
    require(ring_source, "FUTEX_WAIT", "shared wait")
    require(ring_source, "FUTEX_WAKE", "shared wake")
    require(client, "setup_descriptor_transaction_ring_locked", "one-time setup")
    require(client, "bvb_descriptor_transaction_ring_call(", "zero-socket call")
    require(client, "BVB_E128_DESCRIPTOR_RING_PROFILE", "logical wait profile")
    require(service, "descriptor_transaction_worker_main", "Bionic worker")
    require(service, "memcpy(snapshot, worker->journal_region->address", "immutable snapshot")
    require(service, "bvb_vulkan_global_context_allocate_descriptor_sets(", "real native allocation")
    require(global_test, "exchanges_before_descriptor_ring_setup", "one-time setup proof")
    require(global_test, "exchanges_before_descriptor_transaction == 0U", "steady-state zero exchange proof")
    require(builder, '"$project_dir/src/descriptor_transaction_ring.c"', "glibc source")
    print("PASS: E128 descriptor completion ring architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
