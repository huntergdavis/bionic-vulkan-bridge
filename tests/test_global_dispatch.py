#!/usr/bin/env python3

import os
import array
import pathlib
import re
import socket
import struct
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
    with tempfile.TemporaryDirectory(prefix="bvb-e034-") as temporary:
        socket_path = pathlib.Path(temporary) / "runtime" / "bridge.sock"
        token = bytes.fromhex(
            "00112233445566778899aabbccddeeff"
            "fedcba98765432100123456789abcdef"
        )
        activity_frame_name = f"bvb-e060-global-{os.getpid()}"
        activity_frame_listener = socket.socket(socket.AF_UNIX,
                                                socket.SOCK_STREAM)
        activity_frame_listener.bind("\0" + activity_frame_name)
        activity_frame_listener.listen(1)
        activity_frame_listener.settimeout(2.0)
        server_environment = os.environ.copy()
        server_environment["BVB_FAKE_HIDE_SWAPCHAIN"] = "1"
        server_environment["BVB_FAKE_REQUIRE_INIT_IMAGE_COMMANDS"] = "1"
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
                token.hex(),
                "--activity-frame-socket",
                activity_frame_name,
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
            ready = server.stdout.readline()
            assert "ready socket=" in ready
            activity_port = int(ready.split("activity_port=", 1)[1].strip())
            assert 0 < activity_port <= 65535

            def send_event(event: int, sequence: int,
                           width: int = 0, height: int = 0) -> None:
                record = struct.pack(
                    "<IHHIIIIQ32s",
                    0x314C5642,
                    1,
                    event,
                    sequence,
                    width,
                    height,
                    12345,
                    9876543210 + sequence,
                    token,
                )
                with socket.create_connection(
                    ("127.0.0.1", activity_port), timeout=1.0
                ) as connection:
                    connection.sendall(record)
                    ack = connection.recv(16)
                magic, version, reserved, accepted_sequence, status = (
                    struct.unpack("<IHHIi", ack)
                )
                assert magic == 0x314C5642
                assert version == 1
                assert reserved == 0
                assert accepted_sequence == sequence
                assert status == 0

            send_event(1, 1)
            send_event(2, 2)
            send_event(3, 3)
            send_event(7, 4, 2800, 1752)
            send_event(11, 5, 2800, 1752)
            send_event(9, 6)
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
            if completed.returncode != 0:
                _, service_stderr = server.communicate(timeout=5.0)
                raise AssertionError(
                    f"{completed.stderr}service stderr: {service_stderr}"
                )
            assert completed.stderr == ""
            assert completed.stdout.startswith("PASS: global Vulkan discovery")
            assert f"api={0x00400000 | (4 << 12) | 354}" in completed.stdout
            assert "exposed_extensions=7 exposed_layers=0" in completed.stdout
            assert 'device=BVB Fake Adreno 730 "quoted"' in completed.stdout
            assert f"device_api={0x00400000 | (3 << 12) | 275}" in completed.stdout
            assert "driver=16909060 vendor=20803 device_id=1840" in completed.stdout
            assert "queues=2 memory_types=1 memory_heaps=2 device_extensions=7" in completed.stdout
            assert "sampler_anisotropy=1" in completed.stdout
            assert f"logical_device={0x0300000000000001}" in completed.stdout
            assert f"queue={0x0400000000000001}" in completed.stdout
            assert "empty_submit=0 queue_wait=0 device_wait=0" in completed.stdout
            assert f"command_pool={0x0A00000000000001}" in completed.stdout
            assert f"command_buffer={0x0B00000000000001}" in completed.stdout
            assert "command_submit=0 pool_reset=0" in completed.stdout
            assert f"buffer={0x1300000000000001}" in completed.stdout
            assert f"memory={0x0900000000000001}" in completed.stdout
            assert "buffer_requirements2=4096,256,1" in completed.stdout
            assert f"buffer_address={0x123456780000}" in completed.stdout
            image_match = re.search(r"\bimage=(\d+)\b", completed.stdout)
            assert image_match is not None
            assert int(image_match.group(1)) >> 56 == 7
            assert f"image_view={0x0800000000000001}" in completed.stdout
            assert "image_bytes=16384" in completed.stdout
            assert "image_allocation_bytes=19623936" in completed.stdout
            assert "image_dedicated=1,1" in completed.stdout
            assert "mapped_bytes=4096 mapped_mismatches=0" in completed.stdout
            assert "fill_words=1024 mismatches=0" in completed.stdout
            assert f"fence={0x1200000000000001}" in completed.stdout
            assert "fence_before=1 fenced_submit=0 fence_after=0" in completed.stdout
            assert "fence_wait=0 fence_reset=0 fence_after_reset=1" in completed.stdout
            frame_connection, _ = activity_frame_listener.accept()
            with frame_connection:
                setup, ancillary, _, _ = frame_connection.recvmsg(
                    128, socket.CMSG_SPACE(4 * struct.calcsize("i"))
                )
            assert len(setup) == 128
            received_fds = array.array("i")
            for level, kind, value in ancillary:
                if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                    received_fds.frombytes(
                        value[: len(value) - len(value) % received_fds.itemsize]
                    )
            assert len(received_fds) == 4
            for descriptor in received_fds:
                os.close(descriptor)
            server_stdout, server_stderr = server.communicate(timeout=5.0)
            assert server.returncode == 0, server_stderr
            assert server_stdout.splitlines() == [
                "bvb-bridge-service: activity_event=1 sequence=1 "
                "pid=12345 width=0 height=0",
                "bvb-bridge-service: activity_event=2 sequence=2 "
                "pid=12345 width=0 height=0",
                "bvb-bridge-service: activity_event=3 sequence=3 "
                "pid=12345 width=0 height=0",
                "bvb-bridge-service: activity_event=7 sequence=4 "
                "pid=12345 width=2800 height=1752",
                "bvb-bridge-service: activity_event=11 sequence=5 "
                "pid=12345 width=2800 height=1752",
                "bvb-bridge-service: activity_event=9 sequence=6 "
                "pid=12345 width=0 height=0",
            ]
            assert server_stderr == ""
            assert not socket_path.exists()
        finally:
            activity_frame_listener.close()
            if server.poll() is None:
                server.terminate()
                server.wait(timeout=5.0)
    print("PASS: E034 mapped-memory integration")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
