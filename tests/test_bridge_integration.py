#!/usr/bin/env python3

import json
import pathlib
import subprocess
import sys
import tempfile
import time


def run_exchange(
    service: str,
    client: str,
    socket_path: pathlib.Path,
    loader: str | None = None,
) -> dict[str, object]:
    service_command = [service, "--socket", str(socket_path), "--once"]
    client_command = [client, "--socket", str(socket_path)]
    if loader is not None:
        service_command.extend(["--loader", loader])
        client_command.append("--vulkan-caps")
    server = subprocess.Popen(
            service_command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    try:
        deadline = time.monotonic() + 5.0
        while not socket_path.exists() and time.monotonic() < deadline:
            if server.poll() is not None:
                break
            time.sleep(0.01)
        assert socket_path.exists(), server.communicate(timeout=1.0)

        completed = subprocess.run(
            client_command,
            check=False,
            capture_output=True,
            text=True,
            timeout=5.0,
        )
        assert completed.returncode == 0, completed.stderr
        document = json.loads(completed.stdout)
        assert document["schema_version"] == 1
        assert document["protocol_version"] == 1
        assert document["request_id"] == 0x42564201
        assert document["pointer_bits"] in (32, 64)
        assert document["page_size"] > 0

        server_stdout, server_stderr = server.communicate(timeout=5.0)
        assert server.returncode == 0, server_stderr
        assert "ready socket=" in server_stdout
        assert not socket_path.exists()
        return document
    finally:
        if server.poll() is None:
            server.terminate()
            server.wait(timeout=5.0)


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: test_bridge_integration.py SERVICE CLIENT FAKE_LOADER"
        )

    service = str(pathlib.Path(sys.argv[1]).resolve())
    client = str(pathlib.Path(sys.argv[2]).resolve())
    fake_loader = str(pathlib.Path(sys.argv[3]).resolve())
    with tempfile.TemporaryDirectory(prefix="bvb-test-") as temp_directory:
        temp_path = pathlib.Path(temp_directory)
        run_exchange(service, client, temp_path / "hello" / "bridge.sock")
        document = run_exchange(
            service,
            client,
            temp_path / "caps" / "bridge.sock",
            fake_loader,
        )
        caps = document["vulkan_caps"]
        assert isinstance(caps, dict)
        assert caps["instance_extension_count"] == 3
        assert caps["physical_device_count"] == 1
        device = caps["physical_devices"][0]
        assert device["name"] == 'BVB Fake Adreno 730 "quoted"'
        assert device["vendor_id"] == 0x5143
        assert device["device_local_bytes"] == 512 * 1024 * 1024

    print("PASS: bridge handshake integration")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
