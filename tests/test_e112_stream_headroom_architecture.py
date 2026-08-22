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
            "usage: test_e112_stream_headroom_architecture.py "
            "EVIDENCE PROTOCOL CODEC TEST"
        )
    evidence_path, protocol_path, codec_path, test_path = map(Path, sys.argv[1:])
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    assert evidence["gate"] == "E112"
    assert evidence["implementation"]["shared_region_total_bytes"] == 16 * 1024 * 1024
    assert evidence["implementation"]["slot_count_after"] == 64
    assert evidence["implementation"]["slot_bytes_after"] == 256 * 1024
    assert evidence["implementation"]["one_each_barrier_after_bytes"] == 192
    assert evidence["validation"]["strict_recording_rpcs"] == 43
    assert evidence["validation"]["shared_recording_rpcs"] == 0

    protocol = protocol_path.read_text(encoding="utf-8")
    codec = codec_path.read_text(encoding="utf-8")
    test = test_path.read_text(encoding="utf-8")
    require(protocol, "BVB_COMMAND_STREAM_SLOT_COUNT = 64", "slot count")
    require(protocol, "BVB_COMMAND_STREAM_SLOT_BYTES = 256 * 1024", "slot bytes")
    require(codec, "barrier_payload_size", "compact barrier formula")
    require(codec, "base += command->memory_count", "packed memory array")
    require(codec, "base += command->buffer_count", "packed buffer array")
    require(test, "record.payload_length == 192U", "one-each barrier proof")
    require(test, "test_expanded_stream_slot_capacity", "256k capacity proof")
    require(test, "index < 1500U", "1500-record proof")
    print("PASS: E112 command stream headroom architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
