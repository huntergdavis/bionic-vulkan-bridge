#!/usr/bin/env python3

import json
import pathlib
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_selftest.py SELFTEST FAKE_LOADER")
    selftest = str(pathlib.Path(sys.argv[1]).resolve())
    fake_loader = str(pathlib.Path(sys.argv[2]).resolve())
    completed = subprocess.run(
        [selftest, "--loader", fake_loader],
        check=False,
        capture_output=True,
        text=True,
        timeout=5.0,
    )
    assert completed.returncode == 0, completed.stderr
    document = json.loads(completed.stdout)
    assert document["schema_version"] == 1
    assert document["loader_path"] == fake_loader
    assert document["buffer_bytes"] == 4096
    assert document["fill_word"] == 0xA5C3F00D
    assert document["mismatched_words"] == 0
    assert document["submit_wait_elapsed_ns"] >= 0
    assert "VK_KHR_surface" in document["known_instance_extensions"]
    assert "VK_KHR_swapchain" in document["known_device_extensions"]
    assert "VK_KHR_dynamic_rendering" in document["known_device_extensions"]
    print("PASS: Vulkan command submission self-test")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
