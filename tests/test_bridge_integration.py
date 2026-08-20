#!/usr/bin/env python3

import json
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import time


def run_exchange(
    service: str,
    client: str,
    socket_path: pathlib.Path,
    loader: str | None = None,
    lifecycle: bool = False,
    batched: bool = False,
    shared: bool = False,
    shared_iterations: int = 0,
) -> dict[str, object]:
    service_command = [service, "--socket", str(socket_path), "--once"]
    client_command = [client, "--socket", str(socket_path)]
    if loader is not None:
        service_command.extend(["--loader", loader])
        client_command.append("--vulkan-caps")
        if shared_iterations:
            client_command.extend(
                ["--vulkan-shared-batch-benchmark", str(shared_iterations)]
            )
        else:
            client_command.append(
                "--vulkan-shared-batch-selftest"
                if shared
                else "--vulkan-batch-selftest"
                if batched
                else "--vulkan-selftest"
            )
    token = bytes.fromhex(
        "00112233445566778899aabbccddeeff"
        "fedcba98765432100123456789abcdef"
    )
    if lifecycle:
        service_command.extend(
            ["--activity-port", "0", "--activity-token", token.hex()]
        )
        client_command.append("--activity-status")
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

        ready_line = server.stdout.readline()
        assert "ready socket=" in ready_line
        if lifecycle:
            port_text = ready_line.split("activity_port=", 1)[1].strip()
            activity_port = int(port_text)
            assert 0 < activity_port <= 65535

            def send_event(
                event: int,
                sequence: int,
                width: int = 0,
                height: int = 0,
                event_token: bytes = token,
            ) -> int:
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
                    event_token,
                )
                with socket.create_connection(
                    ("127.0.0.1", activity_port), timeout=1.0
                ) as connection:
                    connection.sendall(record)
                    ack = connection.recv(16)
                magic, version, reserved, accepted_sequence, status = struct.unpack(
                    "<IHHIi", ack
                )
                assert magic == 0x314C5642
                assert version == 1
                assert reserved == 0
                assert accepted_sequence == sequence
                return status

            assert send_event(1, 1, event_token=bytes(32)) == -13
            assert send_event(1, 1) == 0
            assert send_event(2, 2) == 0
            assert send_event(3, 3) == 0
            assert send_event(7, 4, 2800, 1752) == 0
            assert send_event(11, 5, 2800, 1752) == 0
            assert send_event(9, 6) == 0

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
        assert "ready socket=" not in server_stdout
        assert not socket_path.exists()
        return document
    finally:
        if server.poll() is None:
            server.terminate()
            server.wait(timeout=5.0)


def hello_packet(request_id: int) -> bytes:
    return struct.pack(
        "<IHHHHIIiHHI",
        0x31425642,
        1,
        1,
        1,
        0,
        request_id,
        8,
        0,
        1,
        1,
        0,
    )


def receive_exact(connection: socket.socket, length: int) -> bytes:
    output = b""
    while len(output) < length:
        part = connection.recv(length - len(output))
        assert part
        output += part
    return output


def run_concurrent_connection_contract(
    service: str, socket_path: pathlib.Path
) -> None:
    server = subprocess.Popen(
        [service, "--socket", str(socket_path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    first: socket.socket | None = None
    second: socket.socket | None = None
    try:
        deadline = time.monotonic() + 5.0
        while not socket_path.exists() and time.monotonic() < deadline:
            if server.poll() is not None:
                break
            time.sleep(0.01)
        assert socket_path.exists(), server.communicate(timeout=1.0)
        assert server.stdout is not None
        assert "ready socket=" in server.stdout.readline()

        first = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        first.settimeout(2.0)
        first.connect(str(socket_path))
        first.sendall(hello_packet(101))
        first_response = receive_exact(first, 40)
        assert struct.unpack_from("<IHHHHI", first_response) == (
            0x31425642,
            1,
            2,
            1,
            0,
            101,
        )

        second = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        second.settimeout(2.0)
        second.connect(str(socket_path))
        second.sendall(hello_packet(202))
        second_response = receive_exact(second, 40)
        assert struct.unpack_from("<IHHHHI", second_response) == (
            0x31425642,
            1,
            2,
            1,
            0,
            202,
        )
    finally:
        if second is not None:
            second.close()
        if first is not None:
            first.close()
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
        run_concurrent_connection_contract(
            service, temp_path / "concurrent" / "bridge.sock"
        )
        run_exchange(service, client, temp_path / "hello" / "bridge.sock")
        document = run_exchange(
            service,
            client,
            temp_path / "caps" / "bridge.sock",
            fake_loader,
        )
        caps = document["vulkan_caps"]
        assert isinstance(caps, dict)
        assert caps["instance_extension_count"] == 5
        assert caps["physical_device_count"] == 1
        device = caps["physical_devices"][0]
        assert device["name"] == 'BVB Fake Adreno 730 "quoted"'
        assert device["vendor_id"] == 0x5143
        assert device["device_local_bytes"] == 512 * 1024 * 1024
        selftest = document["vulkan_selftest"]
        assert isinstance(selftest, dict)
        assert selftest["buffer_bytes"] == 4096
        assert selftest["fill_word"] == 0xA5C3F00D
        assert selftest["mismatched_words"] == 0
        assert "VK_KHR_surface" in selftest["known_instance_extensions"]
        assert "VK_KHR_swapchain" in selftest["known_device_extensions"]

        batch_document = run_exchange(
            service,
            client,
            temp_path / "batch" / "bridge.sock",
            fake_loader,
            batched=True,
        )
        batch_selftest = batch_document["vulkan_batch_selftest"]
        assert isinstance(batch_selftest, dict)
        assert batch_selftest["buffer_bytes"] == 4096
        assert batch_selftest["fill_word"] == 0xA5C3F00D
        assert batch_selftest["mismatched_words"] == 0

        shared_document = run_exchange(
            service,
            client,
            temp_path / "shared" / "bridge.sock",
            fake_loader,
            shared=True,
        )
        shared_selftest = shared_document["vulkan_shared_batch_selftest"]
        assert isinstance(shared_selftest, dict)
        assert shared_selftest["buffer_bytes"] == 4096
        assert shared_selftest["fill_word"] == 0xA5C3F00D
        assert shared_selftest["mismatched_words"] == 0

        benchmark_document = run_exchange(
            service,
            client,
            temp_path / "benchmark" / "bridge.sock",
            fake_loader,
            shared_iterations=4,
        )
        benchmark_selftest = benchmark_document["vulkan_shared_batch_selftest"]
        assert isinstance(benchmark_selftest, dict)
        assert benchmark_selftest["mismatched_words"] == 0
        benchmark = benchmark_document["shared_batch_benchmark"]
        assert isinstance(benchmark, dict)
        assert benchmark["warmup_iterations"] == 1
        assert benchmark["measured_iterations"] == 4
        assert benchmark["control_total_ns"] >= benchmark["control_min_ns"]
        assert benchmark["control_max_ns"] >= benchmark["control_mean_ns"]
        assert benchmark["submit_wait_total_ns"] >= benchmark["submit_wait_min_ns"]
        assert benchmark["submit_wait_max_ns"] >= benchmark["submit_wait_mean_ns"]
        assert benchmark["mismatched_words"] == 0

        lifecycle_document = run_exchange(
            service,
            client,
            temp_path / "lifecycle" / "bridge.sock",
            lifecycle=True,
        )
        assert lifecycle_document["service_flags"] & 4
        activity = lifecycle_document["activity_status"]
        assert isinstance(activity, dict)
        assert activity["ingress_configured"] is True
        assert activity["authenticated_event_count"] == 6
        assert activity["rejected_event_count"] == 1
        assert activity["last_sequence"] == 6
        assert activity["last_event"] == 9
        assert activity["created"] is True
        assert activity["started"] is True
        assert activity["resumed"] is True
        assert activity["window_present"] is True
        assert activity["renderer_ready"] is True
        assert activity["focused"] is True
        assert activity["destroyed"] is False
        assert activity["width"] == 2800
        assert activity["height"] == 1752
        assert activity["activity_pid"] == 12345

    print("PASS: bridge handshake integration")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
