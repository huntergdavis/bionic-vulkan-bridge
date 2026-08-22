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
            "usage: test_e111_compact_transfer_architecture.py "
            "EVIDENCE HEADER CODEC TEST"
        )
    evidence_path, header_path, codec_path, test_path = map(Path, sys.argv[1:])
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    assert evidence["gate"] == "E111"
    assert evidence["trigger"]["first_rejection_result"] == -28
    assert evidence["implementation"]["family_entry_count"] == 6
    assert evidence["implementation"]["one_region_payload_before_bytes"] == 2080
    assert evidence["implementation"]["one_region_payload_after_bytes"] == 160
    assert evidence["implementation"]["maximum_payload_bytes"] == 2080
    assert evidence["validation"]["strict_recording_rpcs"] == 43
    assert evidence["validation"]["shared_recording_rpcs"] == 0

    header = header_path.read_text(encoding="utf-8")
    codec = codec_path.read_text(encoding="utf-8")
    test = test_path.read_text(encoding="utf-8")
    require(header, "BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS = 16", "region bound")
    require(codec, "transfer_payload_size(command->region_count)", "compact append")
    require(codec, "payload_length_is_valid", "count/length validation")
    require(codec, "BVB_VULKAN_TRANSFER_MAX_SIZE", "bounded maximum")
    require(test, "test_compact_transfer_capacity", "64k capacity proof")
    require(test, "record.payload_length == 160U", "one-region proof")
    require(test, "index < 300U", "300-record proof")
    print("PASS: E111 compact transfer stream architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
