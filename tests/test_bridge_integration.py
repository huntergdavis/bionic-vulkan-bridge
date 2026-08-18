#!/usr/bin/env python3

import json
import pathlib
import subprocess
import sys
import tempfile
import time


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_bridge_integration.py SERVICE CLIENT")

    service = str(pathlib.Path(sys.argv[1]).resolve())
    client = str(pathlib.Path(sys.argv[2]).resolve())
    with tempfile.TemporaryDirectory(prefix="bvb-test-") as temp_directory:
        socket_path = pathlib.Path(temp_directory) / "runtime" / "bridge.sock"
        server = subprocess.Popen(
            [service, "--socket", str(socket_path), "--once"],
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
                [client, "--socket", str(socket_path)],
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
        finally:
            if server.poll() is None:
                server.terminate()
                server.wait(timeout=5.0)

    print("PASS: bridge handshake integration")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

