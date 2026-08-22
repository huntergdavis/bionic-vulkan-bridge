#!/usr/bin/env python3

import json
from pathlib import Path
import sys


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing {label}: {needle}")


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: test_e129_descriptor_worker_profile_architecture.py "
            "EVIDENCE SERVICE GLOBAL_RUNNER"
        )
    evidence_path, service_path, runner_path = map(Path, sys.argv[1:])
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    service = service_path.read_text(encoding="utf-8")
    runner = runner_path.read_text(encoding="utf-8")

    assert evidence["gate"] == "E129"
    assert evidence["implementation"]["default_enabled"] is False
    assert evidence["implementation"]["sample_calls"] == 4096
    assert evidence["claims"]["tablet_deployed"] is False
    require(service, 'getenv("BVB_FRAME_PROFILE")', "existing selector")
    require(service, "BVB_E129_DESCRIPTOR_WORKER_PROFILE", "bounded record")
    require(service, "worker->profile_calls == 4096U", "bounded cadence")
    for phase in evidence["implementation"]["phases"]:
        require(service, f"{phase}=", f"phase {phase}")
    require(runner, 'server_environment["BVB_FRAME_PROFILE"] = "1"', "opt-in test")
    require(runner, 'profile["allocate_ns"] > 0', "native allocation proof")
    print("PASS: E129 descriptor worker phase profile architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
