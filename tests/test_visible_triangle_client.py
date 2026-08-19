#!/usr/bin/env python3

import array
import json
import mmap
import os
import pathlib
import socket
import struct
import subprocess
import sys


PROTOCOL_MAGIC = 0x31425642
PROTOCOL_VERSION = 1
PROTOCOL_REQUEST = 1
PROTOCOL_RESPONSE = 2
VISIBLE_SETUP = 8
VISIBLE_EXECUTE = 9
TOKEN = bytes(range(1, 33))
WIDTH = 1280
HEIGHT = 720


def receive_exact(connection: socket.socket, length: int) -> bytes:
    output = bytearray()
    while len(output) < length:
        chunk = connection.recv(length - len(output))
        assert chunk, f"unexpected EOF after {len(output)} of {length} bytes"
        output.extend(chunk)
    return bytes(output)


def receive_request(
    connection: socket.socket, descriptor_expected: bool
) -> tuple[int, int, bytes, int | None]:
    descriptor_array = array.array("i")
    header_prefix, ancillary, flags, _address = connection.recvmsg(
        24, socket.CMSG_SPACE(descriptor_array.itemsize)
    )
    assert flags == 0
    header = header_prefix + receive_exact(connection, 24 - len(header_prefix))
    (
        magic,
        version,
        kind,
        opcode,
        reserved,
        request_id,
        payload_length,
        status,
    ) = struct.unpack("<IHHHHIIi", header)
    assert magic == PROTOCOL_MAGIC
    assert version == PROTOCOL_VERSION
    assert kind == PROTOCOL_REQUEST
    assert reserved == 0
    assert status == 0
    payload = receive_exact(connection, payload_length)

    received_fds: list[int] = []
    for level, message_type, data in ancillary:
        assert level == socket.SOL_SOCKET
        assert message_type == socket.SCM_RIGHTS
        descriptor_array.frombytes(
            data[: len(data) - len(data) % descriptor_array.itemsize]
        )
        received_fds.extend(descriptor_array)
        descriptor_array = array.array("i")
    if descriptor_expected:
        assert len(received_fds) == 1
        descriptor = received_fds[0]
    else:
        assert not received_fds
        descriptor = None
    return opcode, request_id, payload, descriptor


def send_response(connection: socket.socket, opcode: int, request_id: int) -> None:
    connection.sendall(
        struct.pack(
            "<IHHHHIIi",
            PROTOCOL_MAGIC,
            PROTOCOL_VERSION,
            PROTOCOL_RESPONSE,
            opcode,
            0,
            request_id,
            0,
            0,
        )
    )


def decode_batch(mapping: mmap.mmap, offset: int, length: int) -> None:
    batch = mapping[offset : offset + length]
    (
        magic,
        version,
        reserved,
        byte_length,
        command_count,
        command_buffer_id,
        sequence,
    ) = struct.unpack_from("<IHHIIQQ", batch)
    assert magic == 0x43425642
    assert version == 1
    assert reserved == 0
    assert byte_length == length == 200
    assert command_count == 6
    assert command_buffer_id == (11 << 56) | 1
    assert sequence == 1

    records: list[tuple[int, bytes]] = []
    record_offset = 32
    while record_offset < len(batch):
        opcode, flags, payload_length = struct.unpack_from(
            "<HHI", batch, record_offset
        )
        assert flags == 0
        payload_start = record_offset + 8
        payload_end = payload_start + payload_length
        assert payload_end <= len(batch)
        records.append((opcode, batch[payload_start:payload_end]))
        record_offset = payload_end
    assert record_offset == len(batch)
    assert [(opcode, len(payload)) for opcode, payload in records] == [
        (1, 48),
        (2, 16),
        (3, 24),
        (4, 16),
        (5, 16),
        (6, 0),
    ]

    begin = records[0][1]
    image_view_id, width, height = struct.unpack_from("<QII", begin)
    assert image_view_id == (8 << 56) | 1
    assert (width, height) == (WIDTH, HEIGHT)
    assert struct.unpack_from("<ffff", begin, 32) == (
        0.25,
        0.019999999552965164,
        0.019999999552965164,
        1.0,
    )
    (pipeline_id,) = struct.unpack_from("<Q", records[1][1])
    assert pipeline_id == (16 << 56) | 1
    viewport = struct.unpack("<ffffff", records[2][1])
    assert viewport == (0.0, 0.0, float(WIDTH), float(HEIGHT), 0.0, 1.0)
    assert struct.unpack("<iiII", records[3][1]) == (0, 0, WIDTH, HEIGHT)
    assert struct.unpack("<IIII", records[4][1]) == (3, 1, 0, 0)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_visible_triangle_client.py CLIENT")
    client = str(pathlib.Path(sys.argv[1]).resolve())
    socket_name = f"bvb-visible-test-{os.getpid()}"
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind("\0" + socket_name)
    listener.listen(1)
    memory_fd = -1
    mapping: mmap.mmap | None = None
    process = subprocess.Popen(
        [
            client,
            "--socket-name",
            socket_name,
            "--token",
            TOKEN.hex(),
            "--width",
            str(WIDTH),
            "--height",
            str(HEIGHT),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        listener.settimeout(5.0)
        connection, _address = listener.accept()
        with connection:
            connection.settimeout(5.0)
            opcode, request_id, setup, received_fd = receive_request(
                connection, descriptor_expected=True
            )
            assert opcode == VISIBLE_SETUP
            assert len(setup) == 48
            assert setup[:32] == TOKEN
            region_bytes, setup_reserved, generation = struct.unpack_from(
                "<IIQ", setup, 32
            )
            assert region_bytes == 4096
            assert setup_reserved == 0
            assert generation != 0
            assert received_fd is not None
            memory_fd = received_fd
            assert os.fstat(memory_fd).st_size == region_bytes
            mapping = mmap.mmap(memory_fd, region_bytes, access=mmap.ACCESS_READ)
            send_response(connection, opcode, request_id)

            opcode, request_id, execute, received_fd = receive_request(
                connection, descriptor_expected=False
            )
            assert opcode == VISIBLE_EXECUTE
            assert received_fd is None
            assert len(execute) == 56
            assert execute[:32] == TOKEN
            execute_generation, offset, length, sequence = struct.unpack_from(
                "<QIIQ", execute, 32
            )
            assert execute_generation == generation
            assert offset == 64
            assert sequence == 1
            decode_batch(mapping, offset, length)
            send_response(connection, opcode, request_id)

        stdout, stderr = process.communicate(timeout=5.0)
        completed = subprocess.CompletedProcess(
            process.args, process.returncode, stdout, stderr
        )
        assert completed.returncode == 0, completed.stderr
        document = json.loads(completed.stdout)
        assert document["socket_name"] == socket_name
        assert document["width"] == WIDTH
        assert document["height"] == HEIGHT
        assert document["region_bytes"] == 4096
        assert document["batch_offset"] == 64
        assert document["batch_bytes"] == 200
        assert document["commands"] == 6
        assert document["sequence"] == 1
        assert document["setup_packet_bytes"] == 72
        assert document["execute_packet_bytes"] == 80
        assert document["execute_round_trip_ns"] > 0
        print("PASS: glibc-facing visible triangle transport")
        return 0
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5.0)
        if mapping is not None:
            mapping.close()
        if memory_fd >= 0:
            os.close(memory_fd)
        listener.close()


if __name__ == "__main__":
    raise SystemExit(main())
