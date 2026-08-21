#!/usr/bin/env python3
import hashlib
import json
import math
import pathlib
import re
import sys


EXPECTED_TABLET_EVIDENCE_SHA256 = (
    "6e782f8c88ab4983d9b9aee8df5c0a91134405fb9f298743f1bd1ca2bb44d545"
)
SHA256 = re.compile(r"^[0-9a-f]{64}$")


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    if len(sys.argv) != 7:
        raise SystemExit(
            "usage: test_e077_tablet_evidence.py TABLET_JSON ROLLBACK_JSON "
            "WRAPPER LOG STRICT_RAW SHARED_RAW"
        )
    (
        tablet_path,
        rollback_path,
        wrapper_path,
        log_path,
        strict_raw_path,
        shared_raw_path,
    ) = map(pathlib.Path, sys.argv[1:])
    tablet = json.loads(tablet_path.read_text())
    rollback = json.loads(rollback_path.read_text())
    wrapper = wrapper_path.read_text()
    log = log_path.read_text()

    assert sha256(tablet_path) == EXPECTED_TABLET_EVIDENCE_SHA256
    assert tablet["result"] == "pass"
    assert tablet["source"]["commit"] == "7dd1ffe3c86bbbd510e76e00f40dd5b0d6e460f8"
    assert tablet["claims"]["real_turnip_cross_libc_transport"] is True
    for unproven in ("activity_imported_images", "visible_frame", "tomb_raider", "benchmark", "fps"):
        assert tablet["claims"][unproven] is False

    strict = tablet["strict_kept_mapped"]
    shared = tablet["shared_upload_only"]
    strict_total = sum(strict["round_trips"].values())
    shared_total = sum(shared["round_trips"].values())
    comparison = tablet["comparison"]
    assert strict_total == comparison["eligible_control_round_trips_strict"] == 11
    assert shared_total == comparison["eligible_control_round_trips_shared"] == 5
    assert math.isclose(
        comparison["eligible_control_round_trip_reduction_percent"],
        100.0 * (strict_total - shared_total) / strict_total,
    )
    assert strict["opcodes"] == {
        "map": 49, "flush": 48, "invalidate": 49, "unmap": 48, "submit": 47,
    }
    assert shared["opcodes"] == {
        "map": 106, "flush": 107, "invalidate": 108, "unmap": 109, "submit": 47,
    }
    for run in (strict, shared):
        assert run["mapped_mismatches"] == 0
        assert run["gpu_fill_mismatches"] == 0
        for field in ("raw_evidence_sha256", "client_stdout_sha256", "service_stdout_sha256", "harness_sha256"):
            assert SHA256.fullmatch(run[field])
    assert sha256(strict_raw_path) == strict["raw_evidence_sha256"]
    assert sha256(shared_raw_path) == shared["raw_evidence_sha256"]
    strict_raw = json.loads(strict_raw_path.read_text())
    shared_raw = json.loads(shared_raw_path.read_text())
    for raw in (strict_raw, shared_raw):
        assert raw["result"] == "pass"
        assert raw["source_commit"] == tablet["source"]["commit"]

    artifacts = tablet["candidate_artifacts"]
    for field in ("bionic_service_sha256", "glibc_icd_sha256", "glibc_client_sha256"):
        assert artifacts[field] in wrapper
    assert wrapper.index('"$@"') < wrapper.index("--expected-service-sha256")

    assert rollback["result"] == "pass"
    assert rollback["snapshot"]["no_clobber"] is True
    assert rollback["snapshot"]["mode"] == "0700"
    assert len(rollback["files"]) == 7
    assert len({entry["source"] for entry in rollback["files"]}) == 7
    assert len({entry["backup"] for entry in rollback["files"]}) == 7
    for entry in rollback["files"]:
        assert entry["backup"].startswith(rollback["snapshot"]["path"] + "/")
        assert SHA256.fullmatch(entry["source_sha256"])
        assert entry["source_sha256"] == entry["backup_sha256"]
        assert entry["mode"] in ("0600", "0700")
    installed = tablet["installed_state"]
    by_label = {entry["label"]: entry for entry in rollback["files"]}
    assert by_label["installed E073 Bionic service"]["source_sha256"] == installed["installed_service_sha256"]
    assert by_label["installed E073 glibc ICD"]["source_sha256"] == installed["installed_glibc_icd_sha256"]
    assert by_label["installed E073 ICD manifest"]["source_sha256"] == installed["installed_manifest_sha256"]
    assert rollback["private_turnip"]["sha256"] == artifacts["private_turnip_sha256"]
    assert rollback["private_turnip"]["copied"] is False
    assert rollback["changes"] == {
        "installed_files_replaced": False,
        "bridge_deployed": False,
        "apk_installed": False,
        "ui_launched": False,
    }
    processes = {entry["name"]: entry for entry in rollback["protected_processes"]}
    assert processes["Steam"] == {"name": "Steam", "pid": 5973, "start_ticks": 97713020, "survived": True}
    assert processes["Termux:X11"] == {"name": "Termux:X11", "pid": 27923, "start_ticks": 97300251, "survived": True}

    assert EXPECTED_TABLET_EVIDENCE_SHA256 in log
    assert rollback["snapshot"]["path"].replace("/data/data/com.termux/files/home", "~") in log
    print("PASS: E077 tablet transport and rollback evidence remain internally exact")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
