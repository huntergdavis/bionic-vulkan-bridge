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
            "usage: test_e118_buffer_mirror_architecture.py "
            "EVIDENCE CLIENT NATIVE GLOBAL_TEST RUNNER"
        )
    evidence_path, client_path, native_path, global_test_path, runner_path = map(
        Path, sys.argv[1:]
    )
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    client = client_path.read_text(encoding="utf-8")
    native = native_path.read_text(encoding="utf-8")
    global_test = global_test_path.read_text(encoding="utf-8")
    runner = runner_path.read_text(encoding="utf-8")
    assert evidence["gate"] == "E118"
    assert evidence["implementation"]["wire_change"] == "none"
    assert evidence["target"]["opcode"] == 48
    assert evidence["target"]["percent_of_rpc_ns"] == 96.484
    require(client, "static bool memory_is_buffer_only_locked(", "client classifier")
    require(client, "memory_is_buffer_only_locked(state->wire_id)", "map selection")
    require(native, "static bool memory_is_buffer_only(", "service classifier")
    require(native, "memory mirror is not bound exclusively to buffers", "service guard")
    require(native, "upload_host_diverged_range(", "baseline reconciliation")
    require(client, "!memory_state->mapped_shared", "image bind rejection")
    require(native, "memory_mirror_slot(context, request->memory_id)", "image bind guard")
    require(global_test, "unsafe_buffer, upload_memory, 0U) ==", "GPU-writable bind")
    require(global_test, "unsafe_image, upload_memory, 0U) !=", "image rejection")
    require(runner, '"ineligible_memory_rtts=1,1 "', "buffer mirror RTT")
    require(runner, '"ineligible_memory_opcodes=106,109"', "buffer mirror opcodes")
    assert evidence["claims"]["general_coherent_device_to_host_visibility"] is False
    print("PASS: E118 buffer-only delta mirror architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
