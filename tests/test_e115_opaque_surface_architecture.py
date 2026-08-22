#!/usr/bin/env python3

import json
from pathlib import Path
import sys


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing {label}: {needle}")


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: test_e115_opaque_surface_architecture.py EVIDENCE NATIVE"
        )
    evidence_path, native_path = map(Path, sys.argv[1:])
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    native = native_path.read_text(encoding="utf-8")
    assert evidence["gate"] == "E115"
    assert evidence["implementation"]["storage_bytes_per_pixel_unchanged"] == 4
    require(native, "WINDOW_FORMAT_RGBX_8888", "opaque NativeWindow format")
    if "ANativeActivity_setWindowFormat(activity, WINDOW_FORMAT_RGBA_8888)" in native:
        raise AssertionError("NativeActivity still advertises a blended RGBA layer")
    require(native, "VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR", "opaque alpha preference")
    require(native, "E115_SURFACE_OPACITY", "runtime surface diagnostic")
    require(
        native,
        "BVB_ACTIVITY_TRIANGLE_BATCH_BYTES = 4096",
        "bounded current-record triangle capacity",
    )
    assert native.count(
        "uint8_t triangle_batch[BVB_ACTIVITY_TRIANGLE_BATCH_BYTES]"
    ) == 2
    if "uint8_t triangle_batch[512]" in native:
        raise AssertionError("Activity still uses the obsolete 512-byte batch")
    print("PASS: E115 opaque Android game-surface architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
