#!/usr/bin/env python3

from pathlib import Path
import sys


def require(source: str, needle: str, label: str) -> None:
    if needle not in source:
        raise SystemExit(f"missing {label}: {needle}")


def main() -> None:
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: test_e137_native_sync_shape_profile_architecture.py "
            "SERVICE GLOBAL HEADER EVIDENCE"
        )
    service = Path(sys.argv[1]).read_text(encoding="utf-8")
    global_source = Path(sys.argv[2]).read_text(encoding="utf-8")
    header = Path(sys.argv[3]).read_text(encoding="utf-8")
    evidence = Path(sys.argv[4]).read_text(encoding="utf-8")
    for needle, label in (
        ("wait_zero_timeout_calls", "zero-timeout wait bucket"),
        ("wait_finite_timeout_calls", "finite-timeout wait bucket"),
        ("wait_infinite_timeout_calls", "infinite-timeout wait bucket"),
        ("wait_timeout_result_calls", "timeout-result bucket"),
        ("wait_semaphore_total", "semaphore-count shape"),
        ("submit_mirror_ns", "mirror-sync stage"),
        ("submit_native_ns", "native-submit stage"),
        ("submit_command_total", "submit shape"),
    ):
        require(service, needle, label)
    require(
        header,
        "struct bvb_vulkan_queue_submit_2_profile",
        "optional internal profile ABI",
    )
    require(
        global_source,
        "profile->mirror_sync_ns",
        "native mirror timing",
    )
    require(
        global_source,
        "profile->native_submit_ns",
        "Turnip submit timing",
    )
    require(evidence, '"behavior_changed": false', "behavior boundary")
    require(evidence, '"e136_transport_residual_percent": 3.333', "measured input")
    print("E137 native sync-shape profile architecture: PASS")


if __name__ == "__main__":
    main()
