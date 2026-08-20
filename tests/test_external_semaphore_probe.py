#!/usr/bin/env python3
import json
import os
import subprocess
import sys


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_external_semaphore_probe.py PROBE LOADER")
    probe, loader = map(os.path.abspath, sys.argv[1:])
    completed = subprocess.run(
        [probe, "--loader", loader],
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert completed.returncode == 0, completed.stderr
    document = json.loads(completed.stdout)
    assert document["schema_version"] == 1
    assert document["loader_path"] == loader
    assert document["device_name"] == 'BVB Fake Adreno 730 "quoted"'
    assert document["opaque_fd"]["features"] == 3
    assert document["opaque_fd"]["compatible"] & 1
    assert document["sync_fd"]["features"] == 3
    assert document["sync_fd"]["compatible"] & 16
    print("PASS: external semaphore FD capability probe")


if __name__ == "__main__":
    main()
