#!/usr/bin/env python3

import json
import pathlib
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: test_external_memory.py SELFTEST FAKE_LOADER"
        )
    selftest = str(pathlib.Path(sys.argv[1]).resolve())
    fake_loader = str(pathlib.Path(sys.argv[2]).resolve())
    for extra, gate in (([], "E035"), (["--raw-fd-mmap"], "E138")):
        completed = subprocess.run(
            [selftest, "--loader", fake_loader, *extra],
            check=False,
            capture_output=True,
            text=True,
            timeout=5.0,
        )
        assert completed.returncode == 0, completed.stderr
        assert completed.stderr == ""
        document = json.loads(completed.stdout)
        assert document["schema_version"] == 1
        assert document["gate"] == gate
        assert document["loader_path"] == fake_loader
        assert document["handle_type"] == "opaque_fd"
        assert document["logical_device_count"] == 2
        assert document["external_memory_features"] & 0x2
        assert document["external_memory_features"] & 0x4
        assert document["compatible_handle_types"] & 0x1
        assert document["export_from_imported_handle_types"] & 0x1
        assert document["memory_type_index"] == 0
        assert document["memory_property_flags"] & 0x2
        assert document["buffer_bytes"] == 4096
        assert document["mismatched_bytes"] == 0
        if gate == "E138":
            assert document["raw_fd_mmap_bytes"] == 4096
            assert document["raw_fd_source_mismatched_bytes"] == 0
            assert document["raw_fd_destination_mismatched_bytes"] == 0
        else:
            assert document["raw_fd_mmap_bytes"] == 0
    print("PASS: E035 import and E138 raw-FD mmap round trips")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
