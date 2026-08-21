#!/usr/bin/env python3

import os
import pathlib
import subprocess
import sys
import tempfile
import time


def run_case(service: str, client: str, loader: str, mode: str) -> None:
    with tempfile.TemporaryDirectory(prefix=f"bvb-e072-{mode}-") as temporary:
        socket_path = pathlib.Path(temporary) / "bridge.sock"
        server_environment = os.environ.copy()
        server_environment["BVB_FAKE_HIDE_SWAPCHAIN"] = "1"
        server_environment["BVB_FAKE_REQUIRE_NO_NATIVE_SWAPCHAIN"] = "1"
        if mode in ("inject", "explicit"):
            server_environment["BVB_FAKE_REQUIRE_EXTERNAL_MEMORY_FD"] = "1"
            server_environment[
                "BVB_FAKE_REQUIRE_EXTERNAL_MEMORY_DMA_BUF"
            ] = "1"
            server_environment["BVB_FAKE_EXPECT_EXTERNAL_MEMORY_FD_COUNT"] = "1"
            server_environment[
                "BVB_FAKE_EXPECT_EXTERNAL_MEMORY_DMA_BUF_COUNT"
            ] = "1"
            server_environment["BVB_FAKE_EXPECT_AHARDWAREBUFFER_COUNT"] = "1"
            server_environment["BVB_FAKE_EXPECT_DEVICE_EXTENSION_COUNT"] = "3"
        elif mode == "no-swapchain":
            server_environment["BVB_FAKE_EXPECT_EXTERNAL_MEMORY_FD_COUNT"] = "0"
            server_environment[
                "BVB_FAKE_EXPECT_EXTERNAL_MEMORY_DMA_BUF_COUNT"
            ] = "0"
            server_environment["BVB_FAKE_EXPECT_AHARDWAREBUFFER_COUNT"] = "0"
            server_environment["BVB_FAKE_EXPECT_DEVICE_EXTENSION_COUNT"] = "0"
        else:
            server_environment[
                "BVB_FAKE_HIDE_EXTERNAL_MEMORY_DMA_BUF"
            ] = "1"
            server_environment["BVB_FAKE_FORBID_CREATE_DEVICE"] = "1"
        server = subprocess.Popen(
            [
                service,
                "--socket",
                str(socket_path),
                "--loader",
                loader,
                "--activity-port",
                "0",
                "--activity-token",
                "00112233445566778899aabbccddeeff"
                "fedcba98765432100123456789abcdef",
                "--once",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=server_environment,
        )
        try:
            deadline = time.monotonic() + 5.0
            while not socket_path.exists() and time.monotonic() < deadline:
                if server.poll() is not None:
                    break
                time.sleep(0.01)
            assert socket_path.exists(), server.communicate(timeout=1.0)
            assert "ready socket=" in server.stdout.readline()
            client_environment = os.environ.copy()
            client_environment["BVB_BRIDGE_SOCKET"] = str(socket_path)
            completed = subprocess.run(
                [client, mode],
                check=False,
                capture_output=True,
                text=True,
                timeout=5.0,
                env=client_environment,
            )
            server_stdout, server_stderr = server.communicate(timeout=5.0)
            assert completed.returncode == 0, completed.stderr
            assert completed.stderr == ""
            assert completed.stdout == (
                f"PASS: E072 device dependency mode={mode}\n"
            )
            assert server.returncode == 0, (server_stdout, server_stderr)
            assert server_stderr == ""
            assert not socket_path.exists()
        finally:
            if server.poll() is None:
                server.terminate()
                server.wait(timeout=5.0)


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: test_device_extension_dependency.py SERVICE CLIENT LOADER"
        )
    service, client, loader = map(
        lambda path: str(pathlib.Path(path).resolve()), sys.argv[1:]
    )
    for mode in ("inject", "explicit", "no-swapchain", "unavailable"):
        run_case(service, client, loader, mode)
    print("PASS: E072 virtual swapchain native dependency matrix")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
