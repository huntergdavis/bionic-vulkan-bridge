#!/usr/bin/env python3

import json
import os
import pathlib
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 7:
        raise SystemExit(
            "usage: test_global_activity_harness.py "
            "HARNESS SERVICE CLIENT FAKE_LOADER EVIDENCE TERMUX_GATE"
        )
    harness, service, client, loader, evidence_path, termux_gate_path = map(
        lambda value: str(pathlib.Path(value).resolve()), sys.argv[1:]
    )
    evidence = json.loads(pathlib.Path(evidence_path).read_text())
    assert evidence == {
        "schema_version": 1,
        "gate": "synthetic-global-activity-harness-host",
        "result": "pass",
        "service": "real bridge service with fake native Vulkan loader",
        "client": "real global-dispatch Vulkan client",
        "activity": "synthetic authenticated lifecycle and frame FD sink",
        "activity_event_sequence": [1, 2, 3, 7, 11, 9],
        "one_time_frame_setup_received": True,
        "received_frame_fd_count": 4,
        "activity_imported_images": False,
        "visible_frame_claim": False,
        "fps_claim": False,
    }
    termux_gate = pathlib.Path(termux_gate_path).read_text()
    assert 'python3 "$harness"' in termux_gate
    assert "BVB_VULKAN_SERVICE_LOADER" in termux_gate
    assert '--service-loader "$service_loader"' in termux_gate
    assert '--activity-frame-socket' not in termux_gate
    assert '--result-json "$harness_result"' in termux_gate
    assert "--hardware-validation" in termux_gate
    assert '-- grun "$client"' in termux_gate
    assert 'harness_result["authenticated_event_count"] == 6' in termux_gate
    assert 'harness_result["visible_frame_claim"] is False' in termux_gate
    assert 'harness_result["fps_claim"] is False' in termux_gate
    with tempfile.TemporaryDirectory(prefix="bvb-global-harness-test-") as temporary:
        root = pathlib.Path(temporary)
        runtime = root / "runtime"
        runtime.mkdir()
        outputs = root / "outputs"
        outputs.mkdir()
        result_path = outputs / "result.json"
        environment = os.environ.copy()
        environment["BVB_FAKE_HIDE_SWAPCHAIN"] = "1"
        environment["BVB_FAKE_REAL_HARDWARE_VALUES"] = "1"
        completed = subprocess.run(
            [
                sys.executable,
                harness,
                "--service",
                service,
                "--service-loader",
                loader,
                "--runtime-parent",
                str(runtime),
                "--width",
                "2800",
                "--height",
                "1752",
                "--timeout",
                "10",
                "--hardware-validation",
                "--service-stdout",
                str(outputs / "service.stdout"),
                "--service-stderr",
                str(outputs / "service.stderr"),
                "--client-stdout",
                str(outputs / "client.stdout"),
                "--client-stderr",
                str(outputs / "client.stderr"),
                "--result-json",
                str(result_path),
                "--",
                client,
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=20,
            env=environment,
        )
        assert completed.returncode == 0, completed.stderr
        assert completed.stderr == ""
        assert completed.stdout == (
            "global_activity_harness=PASS events=6 frame_fds=4 "
            "visible_frame_claim=false fps_claim=false\n"
        )
        result = json.loads(result_path.read_text())
        assert result["result"] == "pass"
        assert result["synthetic_activity"] is True
        assert result["authenticated_activity_events"] == [1, 2, 3, 7, 11, 9]
        assert result["authenticated_event_count"] == 6
        assert result["requested_width"] == 2800
        assert result["requested_height"] == 1752
        assert result["client_exit"] == 0
        assert result["client_validation_mode"] == "hardware"
        assert result["service_exit"] == 0
        assert result["visible_frame_claim"] is False
        assert result["fps_claim"] is False
        setup = result["activity_frame_setup"]
        assert setup["received"] is True
        assert setup["image_count"] == 3
        assert setup["width"] == 2800
        assert setup["height"] == 1752
        assert setup["descriptor_count"] == 4
        assert all(size > 0 for size in setup["allocation_sizes"])
        assert len(setup["memory_type_indices"]) == 3
        assert (outputs / "client.stderr").read_bytes() == b""
        assert (outputs / "service.stderr").read_bytes() == b""
        assert (outputs / "client.stdout").read_text().startswith(
            "PASS: global Vulkan discovery validation_mode=hardware"
        )
        service_lines = (outputs / "service.stdout").read_text().splitlines()
        assert "activity_port=" in service_lines[0]
        assert [
            int(line.split("activity_event=", 1)[1].split(" ", 1)[0])
            for line in service_lines[1:]
        ] == [1, 2, 3, 7, 11, 9]
        assert list(runtime.iterdir()) == []
        assert "token" not in json.dumps(result).lower()

        failure_outputs = root / "failure-outputs"
        failure_outputs.mkdir()
        failure_result_path = failure_outputs / "result.json"
        failed = subprocess.run(
            [
                sys.executable,
                harness,
                "--service",
                service,
                "--service-loader",
                loader,
                "--runtime-parent",
                str(runtime),
                "--timeout",
                "2",
                "--service-stdout",
                str(failure_outputs / "service.stdout"),
                "--service-stderr",
                str(failure_outputs / "service.stderr"),
                "--client-stdout",
                str(failure_outputs / "client.stdout"),
                "--client-stderr",
                str(failure_outputs / "client.stderr"),
                "--result-json",
                str(failure_result_path),
                "--",
                "/bin/false",
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
            env=environment,
        )
        assert failed.returncode == 1
        assert "global-dispatch client exited 1" in failed.stderr
        failure_result = json.loads(failure_result_path.read_text())
        assert failure_result["result"] == "fail"
        assert failure_result["client_exit"] == 1
        assert failure_result["client_validation_mode"] == "strict-fake"
        assert failure_result["activity_frame_setup"] == {"received": False}
        assert failure_result["visible_frame_claim"] is False
        assert failure_result["fps_claim"] is False
        assert list(runtime.iterdir()) == []
        assert "token" not in json.dumps(failure_result).lower()
    print("PASS: standalone synthetic global Activity harness")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
