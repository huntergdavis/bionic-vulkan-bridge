#!/usr/bin/env python3

import json
from pathlib import Path
import sys


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing {label}: {needle}")


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: test_e119_mirror_fast_scan.py EVIDENCE NATIVE GLOBAL_TEST"
        )
    evidence_path, native_path, global_test_path = map(Path, sys.argv[1:])
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    native = native_path.read_text(encoding="utf-8")
    global_test = global_test_path.read_text(encoding="utf-8")

    assert evidence["gate"] == "E119"
    assert evidence["implementation"]["wire_change"] == "none"
    assert evidence["implementation"]["outer_equal_region_bytes"] == 65536
    assert evidence["implementation"]["inner_equal_region_bytes"] == 4096
    require(
        native,
        "BVB_MEMORY_MIRROR_COMPARE_SUPERBLOCK_BYTES = 65536",
        "bounded outer comparison",
    )
    require(
        native,
        "BVB_MEMORY_MIRROR_COMPARE_BLOCK_BYTES = 4096",
        "bounded inner comparison",
    )
    if native.count("memcmp(mirror->mirror + cursor, mirror->baseline + cursor,") < 2:
        raise AssertionError("both hierarchical equal-region fast paths are required")
    require(
        native,
        "mirror->mirror[cursor] != mirror->baseline[cursor]",
        "byte-exact dirty-run boundary",
    )
    require(
        native,
        "mirror->native + mirror->offset + dirty_first",
        "native dirty-run destination",
    )
    require(
        native,
        "mirror->baseline + dirty_first",
        "private baseline update",
    )
    require(global_test, "UINT32_C(0xa5c3f00d)", "GPU native preservation sentinel")
    assert evidence["claims"]["general_coherent_device_to_host_visibility"] is False
    print("PASS: E119 hierarchical mirror scan architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
