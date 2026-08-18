#!/usr/bin/env python3

import json
import pathlib
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: test_surface_probe.py PROBE FAKE_VULKAN FAKE_MEDIA_NDK"
        )
    probe = str(pathlib.Path(sys.argv[1]).resolve())
    fake_vulkan = str(pathlib.Path(sys.argv[2]).resolve())
    fake_media = str(pathlib.Path(sys.argv[3]).resolve())
    completed = subprocess.run(
        [
            probe,
            "--loader",
            fake_vulkan,
            "--media-loader",
            fake_media,
        ],
        check=False,
        capture_output=True,
        text=True,
        timeout=5.0,
    )
    assert completed.returncode == 0, completed.stderr
    document = json.loads(completed.stdout)
    assert document["schema_version"] == 1
    assert document["loader_path"] == fake_vulkan
    assert document["media_loader_path"] == fake_media
    assert document["width"] == 64
    assert document["height"] == 64
    assert document["image_reader_usage"] == 1 << 8
    assert document["physical_device_count"] == 1
    assert document["queue_family_count"] == 2
    assert document["present_queue_family_index"] == 0
    assert document["present_queue_count"] == 1
    assert document["min_image_count"] == 2
    assert document["max_image_count"] == 4
    assert document["current_extent"] == {"width": 64, "height": 64}
    assert document["min_extent"] == {"width": 16, "height": 16}
    assert document["max_extent"] == {"width": 4096, "height": 4096}
    assert document["formats"] == [
        {"format": 37, "color_space": 0},
        {"format": 44, "color_space": 0},
    ]
    assert document["present_modes"] == [2, 0]
    assert document["elapsed_ns"] >= 0

    invalid_cli = subprocess.run(
        [
            probe,
            "--loader",
            "relative-vulkan.so",
            "--media-loader",
            fake_media,
        ],
        check=False,
        capture_output=True,
        text=True,
        timeout=5.0,
    )
    assert invalid_cli.returncode == 2
    assert "usage:" in invalid_cli.stderr

    missing_media_api = subprocess.run(
        [
            probe,
            "--loader",
            fake_vulkan,
            "--media-loader",
            fake_vulkan,
        ],
        check=False,
        capture_output=True,
        text=True,
        timeout=5.0,
    )
    assert missing_media_api.returncode == 3
    assert "missing AImageReader APIs" in missing_media_api.stderr
    print("PASS: controlled ANativeWindow Vulkan surface probe")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
