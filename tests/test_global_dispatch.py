#!/usr/bin/env python3

import os
import pathlib
import subprocess
import sys
import tempfile
import time


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: test_global_dispatch.py SERVICE CLIENT FAKE_LOADER"
        )
    service, client, loader = map(
        lambda value: str(pathlib.Path(value).resolve()), sys.argv[1:]
    )
    with tempfile.TemporaryDirectory(prefix="bvb-e025-") as temporary:
        socket_path = pathlib.Path(temporary) / "runtime" / "bridge.sock"
        server = subprocess.Popen(
            [
                service,
                "--socket",
                str(socket_path),
                "--loader",
                loader,
                "--once",
            ],
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
            ready = server.stdout.readline()
            assert "ready socket=" in ready
            environment = os.environ.copy()
            environment["BVB_BRIDGE_SOCKET"] = str(socket_path)
            completed = subprocess.run(
                [client],
                check=False,
                capture_output=True,
                text=True,
                timeout=5.0,
                env=environment,
            )
            assert completed.returncode == 0, completed.stderr
            assert completed.stderr == ""
            assert completed.stdout.startswith("PASS: global Vulkan bootstrap")
            server_stdout, server_stderr = server.communicate(timeout=5.0)
            assert server.returncode == 0, server_stderr
            assert server_stdout == ""
            assert server_stderr == ""
            assert not socket_path.exists()
        finally:
            if server.poll() is None:
                server.terminate()
                server.wait(timeout=5.0)
    print("PASS: E025 global dispatch integration")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
