#!/usr/bin/env python3

import array
import fcntl
import json
import mmap
import os
import pathlib
import socket
import struct
import subprocess
import sys


MAGIC = 0x31425642
TOKEN = bytes(range(1, 33))


def header(kind: int, opcode: int, request_id: int, length: int,
           status: int = 0) -> bytes:
    return struct.pack(
        "<IHHHHIIi", MAGIC, 1, kind, opcode, 0, request_id, length, status
    )


def read_exact(connection: socket.socket, length: int) -> bytes:
    result = b""
    while len(result) < length:
        part = connection.recv(length - len(result))
        if not part:
            raise AssertionError("socket ended early")
        result += part
    return result


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_shared_region_relay.py RELAY")
    relay_path = str(pathlib.Path(sys.argv[1]).resolve())
    socket_name = f"bvb-relay-contract-{os.getpid()}"
    visible_listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    visible_listener.bind(("127.0.0.1", 0))
    visible_listener.listen(1)
    visible_port = visible_listener.getsockname()[1]
    region_fd = os.memfd_create("bvb-relay-contract", os.MFD_ALLOW_SEALING)
    os.ftruncate(region_fd, 4096)
    region = mmap.mmap(region_fd, 4096)
    marker = b"BVB_E020_SHARED_REGION binder_parcel_fd=PASS\n"
    region[:len(marker)] = marker
    fcntl.fcntl(
        region_fd,
        fcntl.F_ADD_SEALS,
        fcntl.F_SEAL_GROW | fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_SEAL,
    )
    relay = subprocess.Popen(
        [
            relay_path,
            "--socket",
            socket_name,
            "--visible-port",
            str(visible_port),
            "--token",
            TOKEN.hex(),
            "--width",
            "1280",
            "--height",
            "720",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        assert relay.stdout is not None
        ready = relay.stdout.readline().strip()
        assert ready == (
            f"bvb-shared-region-relay: ready socket={socket_name} mode=visible"
        )
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sender:
            sender.connect("\0" + socket_name)
            hello = struct.pack("<HHI", 1, 1, 0)
            sender.sendmsg(
                [header(1, 1, 0xE021, len(hello)) + hello],
                [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                  array.array("i", [region_fd]))],
            )
            visible, _ = visible_listener.accept()
            with visible:
                request_header = read_exact(visible, 24)
                fields = struct.unpack("<IHHHHIIi", request_header)
                assert fields == (MAGIC, 1, 1, 9, 0, 0xE022, 56, 0)
                payload = read_exact(visible, 56)
                token = payload[:32]
                generation, offset, length, sequence = struct.unpack(
                    "<QIIQ", payload[32:]
                )
                assert token == TOKEN
                assert (generation, offset, length, sequence) == (1, 64, 200, 1)
                batch_header = struct.unpack("<IHHIIQQ", region[64:96])
                assert batch_header == (
                    0x43425642,
                    1,
                    0,
                    200,
                    6,
                    0x0B00000000000001,
                    1,
                )
                visible.sendall(header(2, 9, 0xE022, 0))
            response = read_exact(sender, 24)
            assert struct.unpack("<IHHHHIIi", response) == (
                MAGIC,
                1,
                2,
                1,
                0,
                0xE021,
                0,
                0,
            )
        relay_stdout, relay_stderr = relay.communicate(timeout=10.0)
        assert relay.returncode == 0, relay_stderr
        document = json.loads(relay_stdout)
        assert document["result"] == "pass"
        assert document["transport"] == (
            "binder_scm_rights_then_loopback_metadata"
        )
        assert document["region_bytes"] == 4096
        assert document["batch_offset"] == 64
        assert document["batch_bytes"] == 200
        assert document["commands"] == 6
        assert document["sequence"] == 1
        assert document["receive_validate_ns"] > 0
        assert document["execute_round_trip_ns"] > 0
        assert document["receive_to_present_ns"] >= document["execute_round_trip_ns"]
    finally:
        visible_listener.close()
        region.close()
        os.close(region_fd)
        if relay.poll() is None:
            relay.terminate()
            relay.wait(timeout=5.0)
    print("PASS: Binder/SCM_RIGHTS region drives metadata-only visible replay")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
