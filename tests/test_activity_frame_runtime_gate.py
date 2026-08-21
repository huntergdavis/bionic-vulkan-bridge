#!/usr/bin/env python3

import importlib.util
import json
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import zipfile


def load_gate(path: pathlib.Path):
    spec = importlib.util.spec_from_file_location("bvb_v40_runtime_gate", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def write_tool(path: pathlib.Path, body: str) -> pathlib.Path:
    path.write_text("#!/usr/bin/env python3\n" + body)
    path.chmod(0o700)
    return path


def write_apk(path: pathlib.Path, native: bytes, dex: bytes = b"dex\n") -> None:
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("classes.dex", dex)
        archive.writestr("lib/arm64-v8a/libbvb-visible-host.so", native)


def main() -> int:
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: test_activity_frame_runtime_gate.py SCRIPT GLOBAL_TEST "
            "EVIDENCE DOC"
        )
    script_path, global_test_path, evidence_path, doc_path = map(
        pathlib.Path, sys.argv[1:]
    )
    gate = load_gate(script_path)

    package, version, name = gate.parse_badging(
        "package: name='io.github.huntergdavis.bvb.visiblehost' "
        "versionCode='40' versionName='0.1.39'\n"
    )
    assert package == gate.PACKAGE and version == 40 and name == "0.1.39"
    assert gate.parse_certificate(
        "Signer #1 certificate SHA-256 digest: " + "ab" * 32
    ) == "ab" * 32

    with tempfile.TemporaryDirectory() as temporary:
        root = pathlib.Path(temporary)
        manifest = root / "AndroidManifest.xml"
        manifest.write_text(
            '<?xml version="1.0"?><manifest '
            'xmlns:android="http://schemas.android.com/apk/res/android" '
            f'package="{gate.PACKAGE}" android:versionCode="40" '
            'android:versionName="0.1.39" />\n'
        )
        native = (
            b"ELF\0" + gate.E057_IMPORT_MARKER.encode() + b"\0"
            + gate.E057_PRESENT_MARKER.encode() + b"\0"
        )
        staged = root / "staged.apk"
        installed = root / "installed.apk"
        write_apk(staged, native)
        installed.write_bytes(staged.read_bytes())
        aapt = write_tool(
            root / "aapt",
            "print(\"package: name='io.github.huntergdavis.bvb.visiblehost' "
            "versionCode='40' versionName='0.1.39'\")\n",
        )
        apksigner = write_tool(
            root / "apksigner",
            "print('Signer #1 certificate SHA-256 digest: " + "cd" * 32 + "')\n",
        )
        staged_identity, installed_identity = gate.validate_apk_pair(
            manifest, staged, installed, str(aapt), str(apksigner), 2.0
        )
        assert staged_identity.version_code == 40
        assert staged_identity.sha256 == installed_identity.sha256

        valid_frame = {
            "result": "pass",
            "generation": 7,
            "image_count": 3,
            "per_frame_java_calls": 0,
            "per_frame_binder_calls": 0,
        }
        gate.validate_frame_document(valid_frame, True)
        for field, value in (
            ("generation", 0),
            ("image_count", 4),
            ("per_frame_java_calls", 1),
            ("per_frame_binder_calls", True),
        ):
            invalid_frame = dict(valid_frame)
            invalid_frame[field] = value
            try:
                gate.validate_frame_document(invalid_frame, True)
            except gate.GateFailure:
                pass
            else:
                raise AssertionError(f"invalid helper field was accepted: {field}")

        producer = root / "bvb-global-dispatch-test-glibc"
        bridge_icd = root / "libvulkan-bvb-glibc.so"
        producer.write_bytes(b"producer")
        producer.chmod(0o700)
        bridge_icd.write_bytes(b"icd")
        readelf = write_tool(
            root / "readelf",
            "print(' 0x1 (NEEDED) Shared library: [libvulkan-bvb-glibc.so]')\n"
            f"print(' 0x1d (RUNPATH) Library runpath: [{root}]')\n",
        )
        gate.validate_client_bridge_icd(producer, bridge_icd, str(readelf), 2.0)

        pm = write_tool(
            root / "pm",
            f"print('package:{installed}')\n",
        )
        preflight_output = root / "preflight-output"
        preflight = subprocess.run(
            [
                sys.executable,
                str(script_path),
                "--preflight-only",
                "--staged-apk",
                str(staged),
                "--pm",
                str(pm),
                "--aapt",
                str(aapt),
                "--apksigner",
                str(apksigner),
                "--output-root",
                str(preflight_output),
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5.0,
            check=False,
        )
        assert preflight.returncode == 0, preflight.stderr
        assert "activity_frame_import_v40=preflight_pass" in preflight.stdout
        preflight_evidence = list(preflight_output.glob("*/evidence.json"))
        assert len(preflight_evidence) == 1
        assert json.loads(preflight_evidence[0].read_text())["result"] == "preflight_pass"

        installed.unlink()
        write_apk(installed, native, b"different-dex\n")
        try:
            gate.validate_apk_pair(
                manifest, staged, installed, str(aapt), str(apksigner), 2.0
            )
        except gate.GateFailure as error:
            assert "not byte-identical" in str(error)
        else:
            raise AssertionError("mismatched installed APK was accepted")

        manifest.write_text(manifest.read_text().replace('versionCode="40"', 'versionCode="39"'))
        try:
            gate.validate_apk_pair(
                manifest, staged, staged, str(aapt), str(apksigner), 2.0
            )
        except gate.GateFailure as error:
            assert "versionCode 39 is explicitly refused" in str(error)
        else:
            raise AssertionError("v39 source manifest was accepted")

        service_log = root / "service.log"
        service_log.write_text(
            "bvb-bridge-service: activity_event=1 sequence=1 pid=321 width=0 height=0\n"
            "bvb-bridge-service: activity_event=2 sequence=2 pid=321 width=0 height=0\n"
            "bvb-bridge-service: activity_event=3 sequence=3 pid=321 width=0 height=0\n"
            "bvb-bridge-service: activity_event=7 sequence=4 pid=321 width=2800 height=1752\n"
            "bvb-bridge-service: activity_event=11 sequence=5 pid=321 width=2800 height=1752\n"
        )
        assert {event["event"] for event in gate.parse_activity_events(service_log)} == {
            1, 2, 3, 7, 11
        }

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    port = listener.getsockname()[1]

    def reject_once() -> None:
        connection, _ = listener.accept()
        wire = gate.receive_exact(connection, gate.LIFECYCLE_RECORD.size)
        magic, version, _event, sequence, *_rest = gate.LIFECYCLE_RECORD.unpack(wire)
        assert magic == gate.LIFECYCLE_MAGIC and version == gate.LIFECYCLE_VERSION
        connection.sendall(
            gate.LIFECYCLE_ACK.pack(
                gate.LIFECYCLE_MAGIC, gate.LIFECYCLE_VERSION, 0, sequence, -13
            )
        )
        connection.close()
        listener.close()

    server = threading.Thread(target=reject_once)
    server.start()
    assert gate.prove_wrong_token_rejection(port, 2.0) == -13
    server.join(timeout=2.0)
    assert not server.is_alive()

    help_result = subprocess.run(
        [sys.executable, str(script_path), "--help"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=5.0,
        check=False,
    )
    assert help_result.returncode == 0, help_result.stderr
    assert "--preflight-only" in help_result.stdout
    assert "--present-hold-ms" in help_result.stdout
    assert "--expected-icd-sha256" in help_result.stdout
    assert "--bridge-icd" in help_result.stdout

    source = script_path.read_text()
    for required in (
        "bvb_retain_external_renderer",
        "versionCode 39 is explicitly refused",
        "E057_FRAME_TRANSPORT_IMPORTED",
        "E057_FRAME_PRESENTED",
        "E057_FRAME_CONSUMER_FAIL",
        "prove_wrong_token_rejection",
        "FrameTransportClient",
        "force-stop",
        "visible_game_claim",
        "fps_claim",
        "EXPECTED_BRIDGE_CLIENT_SHA256",
        "EXPECTED_BRIDGE_SERVICE_SHA256",
        "EXPECTED_PRIVATE_TURNIP_SHA256",
        '"libvulkan-bvb-glibc.so"',
        "installed E073 baseline glibc ICD",
        "validate_client_bridge_icd",
        "validate_frame_document",
        'logcat, "-T", "1"',
        'for variable in ("LD_LIBRARY_PATH", "LD_PRELOAD", "LD_AUDIT")',
    ):
        assert required in source
    assert "pkill" not in source
    assert "start-steam" not in source
    assert "Termux:X11" in source

    global_source = global_test_path.read_text()
    for variable in (
        "BVB_GLOBAL_DISPATCH_WSI_WIDTH",
        "BVB_GLOBAL_DISPATCH_WSI_HEIGHT",
        "BVB_GLOBAL_DISPATCH_PRESENT_HOLD_MS",
    ):
        assert variable in global_source
    assert "30000U" in global_source

    evidence = json.loads(evidence_path.read_text())
    assert evidence["result"] == "pass"
    assert evidence["gate"].startswith("E074-")
    assert evidence["visible_output_claim"] is False
    assert evidence["game_output_claim"] is False
    assert evidence["fps_claim"] is False
    assert evidence["apk_identity"]["required_version_code"] == 40
    assert evidence["apk_identity"]["refuses_version_code"] == 39
    assert evidence["runtime_contract"]["retain_external_renderer"] is True
    assert evidence["runtime_contract"]["wrong_token_status"] == -13
    assert evidence["installed_payload_before_gate"]["one_shot_icd_test"] == "pass"
    installed_payload = evidence["installed_payload_before_gate"]
    assert installed_payload["bridge_client_sha256"] == gate.EXPECTED_BRIDGE_CLIENT_SHA256
    assert installed_payload["bridge_service_sha256"] == gate.EXPECTED_BRIDGE_SERVICE_SHA256
    assert installed_payload["private_turnip_sha256"] == gate.EXPECTED_PRIVATE_TURNIP_SHA256

    document = " ".join(doc_path.read_text().split())
    assert "test-activity-frame-import-v40-termux.py" in document
    assert "may remain black" in document
    assert "does not install" in document
    assert "E057_FRAME_PRESENTED" in document
    print("PASS: v40 Activity frame-import runtime gate remains fail-closed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
