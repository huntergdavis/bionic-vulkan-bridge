#!/usr/bin/env python3

import json
from pathlib import Path
import sys


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing {label}: {needle}")


def main() -> int:
    if len(sys.argv) != 7:
        raise SystemExit(
            "usage: test_e126_descriptor_journal_architecture.py "
            "EVIDENCE PROTOCOL CLIENT SERVICE GLOBAL_TEST GLOBAL_RUNNER"
        )
    paths = list(map(Path, sys.argv[1:]))
    evidence = json.loads(paths[0].read_text(encoding="utf-8"))
    protocol, client, service, global_test, global_runner = (
        path.read_text(encoding="utf-8") for path in paths[1:]
    )

    assert evidence["gate"] == "E126"
    assert evidence["trigger"]["benchmark_scene_op112_calls_per_present"] > 500
    assert evidence["trigger"]["benchmark_scene_op112_blocked_ms_per_present"] > 70
    assert evidence["implementation"]["setup_opcode"] == 121
    assert evidence["implementation"]["flush_opcode"] == 122
    assert evidence["implementation"]["region_bytes"] == 16 * 1024 * 1024
    assert evidence["validation"]["shared_local_updates"] == 4097
    assert evidence["validation"]["shared_update_exchange_count"] == 0
    assert evidence["claims"]["tablet_deployed"] is False
    assert evidence["claims"]["benchmark_fps"] is None

    require(protocol, "BVB_OPCODE_VULKAN_DESCRIPTOR_JOURNAL_SETUP = 121", "setup opcode")
    require(protocol, "BVB_OPCODE_VULKAN_DESCRIPTOR_JOURNAL_FLUSH = 122", "flush opcode")
    require(protocol, "BVB_DESCRIPTOR_JOURNAL_REGION_BYTES = 16 * 1024 * 1024", "bounded region")
    require(protocol, "BVB_DESCRIPTOR_JOURNAL_MAX_RECORDS = 64 * 1024", "bounded records")
    require(client, 'getenv("BVB_DESCRIPTOR_JOURNAL")', "opt-in selector")
    require(client, "flush_descriptor_journal_before_exchange_locked", "ordered auto-drain")
    require(client, "append_descriptor_journal_locked(payload, payload_length)", "local append")
    require(client, "poison_descriptor_journal_connection_locked", "uncertain-state poison")
    require(service, "snapshot = malloc(flush.length)", "private snapshot")
    require(service, "memcpy(snapshot, region->address, flush.length)", "single snapshot copy")
    require(service, "validate_descriptor_journal_snapshot(", "whole-batch validation")
    require(service, "replay_descriptor_journal_snapshot(", "ordered native replay")
    require(service, "flush.sequence != region->last_sequence + 1U", "monotonic sequence")
    require(service, "F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL", "immutable capacity seals")
    require(global_test, "shared_descriptor_journal ? 4096U : 1U", "large journal proof")
    require(global_test, "exchanges_before_descriptor_updates", "exchange counter")
    require(global_test, "exchanges_before_descriptor_transaction", "ordered transaction")
    require(global_test, "BVB_OPCODE_VULKAN_DESCRIPTOR_TRANSACTION_ALLOCATE", "transaction opcode")
    require(global_runner, 'environment["BVB_DESCRIPTOR_JOURNAL"] = "shared"', "shared runner")
    print("PASS: E126 shared descriptor journal architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
