#!/usr/bin/env python3

import json
import pathlib
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: test_present_selftest.py SELFTEST FAKE_VULKAN FAKE_MEDIA"
        )
    selftest = str(pathlib.Path(sys.argv[1]).resolve())
    fake_vulkan = str(pathlib.Path(sys.argv[2]).resolve())
    fake_media = str(pathlib.Path(sys.argv[3]).resolve())
    completed = subprocess.run(
        [
            selftest,
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
    assert document["surface_format"] == 37
    assert document["color_space"] == 0
    assert document["present_mode"] == 2
    assert document["requested_image_count"] == 2
    assert document["actual_image_count"] == 3
    assert document["acquired_image_index"] == 0
    assert document["width"] == 64
    assert document["height"] == 64
    assert document["plane_count"] == 1
    assert document["pixel_stride"] == 4
    assert document["row_stride"] == 256
    assert document["data_length"] == 16384
    assert document["expected_rgba_word"] == 0xFFFF00FF
    assert document["mismatched_pixels"] == 0
    assert document["acquire_poll_count"] == 1
    assert document["submit_present_ns"] >= 0
    assert document["total_elapsed_ns"] >= document["submit_present_ns"]
    print("PASS: swapchain clear/present/readback self-test")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
