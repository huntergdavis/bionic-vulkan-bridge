#!/usr/bin/env python3

from pathlib import Path
import sys


def require(source: str, needle: str, label: str) -> None:
    if needle not in source:
        raise SystemExit(f"missing {label}: {needle}")


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: test_e136_sync_service_profile_architecture.py SERVICE EVIDENCE"
        )
    service = Path(sys.argv[1]).read_text(encoding="utf-8")
    evidence = Path(sys.argv[2]).read_text(encoding="utf-8")
    require(service, 'getenv("BVB_FRAME_PROFILE")', "existing opt-in selector")
    require(service, "BVB_E136_SYNC_SERVICE_PROFILE", "bounded profile record")
    require(service, "wait_native_ns", "native semaphore-wait stage")
    require(service, "submit_replay_ns", "shared-stream replay stage")
    require(service, "submit_queue_ns", "native queue-submit stage")
    require(service, "profile->wait_calls < 32U", "bounded emission cadence")
    require(
        service,
        "sync_service_profile_emit_and_reset(&sync_profile, true)",
        "final partial window",
    )
    require(evidence, '"behavior_changed": false', "no-behavior-change boundary")
    require(
        evidence,
        '"client_transport_residual_measurable": true',
        "transport residual",
    )
    global_contract = Path(__file__).with_name("test_global_dispatch.py").read_text(
        encoding="utf-8"
    )
    require(
        global_contract,
        'line.startswith("BVB_E136_SYNC_SERVICE_PROFILE ")',
        "dynamic profile parse",
    )
    print("E136 sync service profile architecture: PASS")


if __name__ == "__main__":
    main()
