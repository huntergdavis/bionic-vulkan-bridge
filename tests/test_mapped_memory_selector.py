#!/usr/bin/env python3

import os
import pathlib
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_mapped_memory_selector.py CLIENT")
    client = str(pathlib.Path(sys.argv[1]).resolve())
    environment = os.environ.copy()
    environment["BVB_BRIDGE_SOCKET"] = "/nonexistent/bvb-e077.sock"
    environment["BVB_MAPPED_MEMORY"] = "corrupt-selector"
    completed = subprocess.run(
        [client], check=False, capture_output=True, text=True,
        timeout=2.0, env=environment,
    )
    assert completed.returncode != 0
    assert "CHECK failed" in completed.stderr
    print("PASS: invalid mapped-memory selector fails closed before connect")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
