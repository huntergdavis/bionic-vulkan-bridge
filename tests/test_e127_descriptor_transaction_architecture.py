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
            "usage: test_e127_descriptor_transaction_architecture.py "
            "EVIDENCE PROTOCOL DESCRIPTOR_WIRE CLIENT SERVICE GLOBAL_TEST FAKE"
        )
    paths = list(map(Path, sys.argv[1:]))
    evidence = json.loads(paths[0].read_text(encoding="utf-8"))
    protocol, descriptor_wire, client, service, global_test, fake = (
        path.read_text(encoding="utf-8") for path in paths[1:]
    )

    assert evidence["gate"] == "E127"
    assert evidence["trigger"]["e126_flush_calls_per_present"] > 350
    assert evidence["trigger"]["e126_updates_per_flush"] < 1.6
    assert evidence["implementation"]["transaction_opcode"] == 123
    assert evidence["validation"]["shared_update_exchange_count"] == 0
    assert evidence["validation"]["transaction_exchange_count"] == 1
    assert evidence["validation"]["following_wait_exchange_count"] == 1
    assert evidence["claims"]["tablet_deployed"] is False
    assert evidence["claims"]["benchmark_fps"] is None

    require(protocol, "BVB_OPCODE_VULKAN_DESCRIPTOR_TRANSACTION_ALLOCATE = 123", "opcode")
    require(descriptor_wire, "journal_record_count", "journal identity")
    require(descriptor_wire, "allocation_length", "canonical allocation payload")
    require(client, "BVB_OPCODE_VULKAN_DESCRIPTOR_TRANSACTION_ALLOCATE", "client transaction")
    require(client, ".allocation = decoded", "existing allocation request reuse")
    require(client, "poison_descriptor_journal_connection_locked", "uncertain-state poison")
    require(service, "answer_vulkan_descriptor_transaction_allocate", "service transaction")
    require(service, "memcpy(snapshot, region->address, transaction.journal_length)", "immutable snapshot")
    require(service, "replay_descriptor_journal_snapshot(", "ordered update replay")
    require(service, "bvb_vulkan_global_context_allocate_descriptor_sets(", "real native allocation")
    require(global_test, "exchanges_before_descriptor_transaction", "single exchange proof")
    require(client, "BVB_OPCODE_VULKAN_DESCRIPTOR_TRANSACTION_ALLOCATE", "legacy exact opcode retained")
    require(fake, "fake_descriptor_transaction_allocate_seen", "native order proof")
    print("PASS: E127 descriptor transaction architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
