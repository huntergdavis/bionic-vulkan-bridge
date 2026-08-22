#!/usr/bin/env python3

import json
from pathlib import Path
import sys


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: test_e102_low_map_architecture.py EVIDENCE CLIENT TEST"
        )
    evidence_path, client_path, test_path = map(Path, sys.argv[1:])
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    assert evidence["gate"] == "E102"
    assert evidence["trigger"]["map_failures"] == 12
    assert evidence["trigger"]["high_pointer_diagnostics"] == 14
    assert evidence["trigger"]["physical_oom"] is False
    assert evidence["implementation"]["overwrite_existing_mapping"] is False
    assert evidence["implementation"]["strict_shadow"] == "low-first anonymous mmap"
    assert evidence["implementation"]["shared_mirror"] == "low-first memfd mmap"

    client = client_path.read_text(encoding="utf-8")
    assert "map_client_visible_memory(" in client
    assert "MAP_FIXED_NOREPLACE" in client
    assert "BVB_CLIENT_LOW_MAP_START" in client
    assert "UINT32_MAX - normal_address" in client
    assert "memory_fd, (size_t)length" in client
    assert "MAP_PRIVATE | MAP_ANONYMOUS" in client
    assert "BVB_ICD_MEMORY_MAP" in client
    assert "free(proxy->mapped_bytes)" not in client

    test = test_path.read_text(encoding="utf-8")
    assert "CHECK((uintptr_t)mapped <= UINT32_MAX)" in test
    assert "CHECK((uintptr_t)ineligible_mapping <= UINT32_MAX)" in test
    print("PASS: E102 low 32-bit Vulkan mapped-address architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
