#!/usr/bin/env python3

import array
import ctypes
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
MFD_ALLOW_SEALING = 0x0002


def create_memfd(name: str) -> int:
    if hasattr(os, "memfd_create"):
        return os.memfd_create(name, MFD_ALLOW_SEALING)
    libc = ctypes.CDLL(None, use_errno=True)
    native_memfd_create = libc.memfd_create
    native_memfd_create.argtypes = [ctypes.c_char_p, ctypes.c_uint]
    native_memfd_create.restype = ctypes.c_int
    descriptor = native_memfd_create(name.encode(), MFD_ALLOW_SEALING)
    if descriptor < 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))
    return descriptor


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


def run_contract(relay_path: str, frames: int, ring_slots: int) -> None:
    socket_name = f"bvb-relay-contract-{os.getpid()}-{frames}-{ring_slots}"
    visible_listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    visible_listener.bind(("127.0.0.1", 0))
    visible_listener.listen(1)
    visible_port = visible_listener.getsockname()[1]
    region_fd = create_memfd("bvb-relay-contract")
    os.ftruncate(region_fd, 4096)
    region = mmap.mmap(region_fd, 4096)
    marker = b"BVB_E020_SHARED_REGION binder_parcel_fd=PASS\n"
    region[:len(marker)] = marker
    fcntl.fcntl(
        region_fd,
        fcntl.F_ADD_SEALS,
        fcntl.F_SEAL_GROW | fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_SEAL,
    )
    arguments = [
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
    ]
    if frames != 1 or ring_slots != 1:
        arguments.extend(
            ["--frames", str(frames), "--ring-slots", str(ring_slots)]
        )
    relay = subprocess.Popen(
        arguments,
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
                for frame in range(frames):
                    sequence = frame + 1
                    offset = 64 + (frame % ring_slots) * 256
                    request_id = 0xE022 + frame
                    request_header = read_exact(visible, 24)
                    fields = struct.unpack("<IHHHHIIi", request_header)
                    assert fields == (
                        MAGIC,
                        1,
                        1,
                        9,
                        0,
                        request_id,
                        56,
                        0,
                    )
                    payload = read_exact(visible, 56)
                    token = payload[:32]
                    execute = struct.unpack("<QIIQ", payload[32:])
                    assert token == TOKEN
                    assert execute == (1, offset, 200, sequence)
                    batch_header = struct.unpack(
                        "<IHHIIQQ", region[offset:offset + 32]
                    )
                    assert batch_header == (
                        0x43425642,
                        1,
                        0,
                        200,
                        6,
                        0x0B00000000000001,
                        sequence,
                    )
                    visible.sendall(header(2, 9, request_id, 0))
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
        assert document["sequence"] == frames
        assert document["frames"] == frames
        assert document["ring_slots"] == ring_slots
        assert document["batch_stride"] == 256
        assert document["receive_validate_ns"] > 0
        assert document["execute_round_trip_ns"] > 0
        assert document["round_trip_min_ns"] > 0
        assert document["round_trip_p50_ns"] >= document["round_trip_min_ns"]
        assert document["round_trip_p95_ns"] >= document["round_trip_p50_ns"]
        assert document["round_trip_max_ns"] >= document["round_trip_p95_ns"]
        assert document["execute_total_ns"] >= frames * document["round_trip_min_ns"]
        assert document["receive_to_present_ns"] >= document["execute_round_trip_ns"]
    finally:
        visible_listener.close()
        region.close()
        os.close(region_fd)
        if relay.poll() is None:
            relay.terminate()
            relay.wait(timeout=5.0)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_shared_region_relay.py RELAY")
    relay_path = str(pathlib.Path(sys.argv[1]).resolve())
    run_contract(relay_path, frames=1, ring_slots=1)
    run_contract(relay_path, frames=6, ring_slots=4)
    print(
        "PASS: Binder/SCM_RIGHTS region drives one-shot and persistent "
        "ring visible replay"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
