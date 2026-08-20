#!/usr/bin/env python3

import json
import pathlib
import subprocess
import sys


def run(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, check=False, capture_output=True, text=True)


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_probe.py PROBE FAKE_LOADER")

    probe = str(pathlib.Path(sys.argv[1]).resolve())
    fake_loader = str(pathlib.Path(sys.argv[2]).resolve())

    success = run(probe, "--loader", fake_loader)
    assert success.returncode == 0, success.stderr
    document = json.loads(success.stdout)
    assert document["schema_version"] == 1
    assert document["loader_path"] == fake_loader
    assert document["loader_api_version"]["major"] == 1
    assert document["loader_api_version"]["minor"] == 4
    assert document["loader_api_version"]["patch"] == 354
    assert document["instance_extension_count"] == 5
    assert document["physical_device_count"] == 1
    device = document["physical_devices"][0]
    assert device["name"] == 'BVB Fake Adreno 730 "quoted"'
    assert device["vendor_id"] == 0x5143
    assert device["device_id"] == 0x0730
    assert device["queue_family_count"] == 2
    assert device["memory_heap_count"] == 2
    assert device["device_local_bytes"] == 512 * 1024 * 1024

    relative = run(probe, "--loader", "relative.so")
    assert relative.returncode == 2
    assert "ABSOLUTE_PATH" in relative.stderr

    missing = run(probe, "--loader", "/definitely/not/a/vulkan/loader.so")
    assert missing.returncode == 3
    assert "could not load" in missing.stderr

    print("PASS: Vulkan probe contract and failure modes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
