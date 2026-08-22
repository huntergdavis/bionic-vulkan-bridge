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
            "usage: test_e120_free_unmaps_mirror.py "
            "EVIDENCE CLIENT GLOBAL_TEST GLOBAL_CONTRACT"
        )
    evidence_path, client_path, global_test_path, contract_path = map(
        Path, sys.argv[1:]
    )
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    client = client_path.read_text(encoding="utf-8")
    global_test = global_test_path.read_text(encoding="utf-8")
    contract = contract_path.read_text(encoding="utf-8")

    assert evidence["gate"] == "E120"
    assert evidence["implementation"]["wire_change"] == "none"
    assert evidence["implementation"]["free_implicitly_unmaps"] is True
    require(
        client,
        "bvb_bridge_vkUnmapMemory(device, memory);",
        "implicit unmap before free",
    )
    require(
        client,
        "BVB_OPCODE_VULKAN_MEMORY_FREE, allocator",
        "free after unmap",
    )
    require(
        global_test,
        "mapped_free_rtts == 2U",
        "unmap plus free exchange proof",
    )
    require(
        global_test,
        "mapped_free_released",
        "client proxy release proof",
    )
    require(
        contract,
        '"shared-mapped-free-memory"',
        "dedicated integration mode",
    )
    require(
        contract,
        "mapped_free_rtts=2 mapped_free_opcode=38 ",
        "exact unmap/free exchange acceptance",
    )
    require(
        contract,
        "mapped_free_released=1",
        "exact proxy-release acceptance",
    )
    assert evidence["claims"]["present_46_boundary_fixed"] is None
    print("PASS: E120 free-memory implicit-unmap architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
