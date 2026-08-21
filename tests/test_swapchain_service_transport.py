#!/usr/bin/env python3

import array
import mmap
import os
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import time

from android_hardware_buffer import load_android_hardware_buffer_api


MAGIC = 0x31425642
HEADER = struct.Struct("<IHHHHIIi")


def packet(opcode: int, request_id: int, payload: bytes = b"") -> bytes:
    return HEADER.pack(MAGIC, 1, 1, opcode, 0, request_id, len(payload), 0) + payload


def receive_exact(connection: socket.socket, length: int) -> bytes:
    output = bytearray()
    while len(output) < length:
        part = connection.recv(length - len(output))
        assert part
        output.extend(part)
    return bytes(output)


def receive_response(connection: socket.socket, opcode: int, request_id: int):
    header = receive_exact(connection, HEADER.size)
    magic, version, kind, returned_opcode, reserved, returned_id, length, status = (
        HEADER.unpack(header)
    )
    assert (magic, version, kind, returned_opcode, reserved, returned_id) == (
        MAGIC,
        1,
        2,
        opcode,
        0,
        request_id,
    )
    return status, receive_exact(connection, length)


def receive_fd_response(connection: socket.socket, opcode: int, request_id: int):
    data, ancillary, flags, _ = connection.recvmsg(
        HEADER.size + 4096, socket.CMSG_SPACE(8 * array.array("i").itemsize)
    )
    assert not flags & (socket.MSG_CTRUNC | socket.MSG_TRUNC)
    while len(data) < HEADER.size:
        data += receive_exact(connection, HEADER.size - len(data))
    header = data[: HEADER.size]
    magic, version, kind, returned_opcode, reserved, returned_id, length, status = (
        HEADER.unpack(header)
    )
    assert (magic, version, kind, returned_opcode, reserved, returned_id) == (
        MAGIC,
        1,
        2,
        opcode,
        0,
        request_id,
    )
    payload = bytearray(data[HEADER.size :])
    if len(payload) < length:
        payload.extend(receive_exact(connection, length - len(payload)))
    assert len(payload) == length
    descriptors: list[int] = []
    for level, kind, value in ancillary:
        if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
            received = array.array("i")
            received.frombytes(value[: len(value) - len(value) % received.itemsize])
            descriptors.extend(received)
    return status, bytes(payload), descriptors


def send_lifecycle(port: int, token: bytes, event: int, sequence: int) -> None:
    width = 64 if event in (7, 11) else 0
    height = 64 if event in (7, 11) else 0
    record = struct.pack(
        "<IHHIIIIQ32s",
        0x314C5642,
        1,
        event,
        sequence,
        width,
        height,
        54321,
        1234567890 + sequence,
        token,
    )
    with socket.create_connection(("127.0.0.1", port), timeout=2.0) as channel:
        channel.sendall(record)
        acknowledgement = receive_exact(channel, 16)
    assert struct.unpack("<IHHIi", acknowledgement)[-1] == 0


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_swapchain_service_transport.py SERVICE LOADER")
    service = str(pathlib.Path(sys.argv[1]).resolve())
    loader = str(pathlib.Path(sys.argv[2]).resolve())
    token = bytes.fromhex(
        "00112233445566778899aabbccddeeff"
        "fedcba98765432100123456789abcdef"
    )
    hardware_buffer_api = load_android_hardware_buffer_api()
    with tempfile.TemporaryDirectory(prefix="bvb-wsi-service-") as temporary:
        socket_path = pathlib.Path(temporary) / "runtime" / "bridge.sock"
        activity_frame_socket = f"bvb-activity-frame-host-{os.getpid()}"
        activity_frame_listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        activity_frame_listener.bind("\0" + activity_frame_socket)
        activity_frame_listener.listen(1)
        activity_frame_listener.settimeout(2.0)
        server = subprocess.Popen(
            [
                service,
                "--socket",
                str(socket_path),
                "--once",
                "--loader",
                loader,
                "--activity-port",
                "0",
                "--activity-token",
                token.hex(),
                "--activity-frame-socket",
                activity_frame_socket,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        connection = None
        descriptors: list[int] = []
        activity_descriptors: list[int] = []
        activity_hardware_buffers = []
        try:
            assert server.stdout is not None
            ready = server.stdout.readline()
            assert "ready socket=" in ready and "activity_port=" in ready
            port = int(ready.rsplit("activity_port=", 1)[1])
            for sequence, event in enumerate((1, 3, 7, 11), start=1):
                send_lifecycle(port, token, event, sequence)

            connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            connection.settimeout(2.0)
            connection.connect(str(socket_path))
            connection.sendall(packet(1, 1, struct.pack("<HHI", 1, 1, 0)))
            status, hello = receive_response(connection, 1, 1)
            assert status == 0 and len(hello) == 16
            assert struct.unpack_from("<H", hello)[0] == 1
            assert struct.unpack_from("<I", hello, 4)[0] & 4

            instance_payload = struct.pack("<IIII", 4198400, 0, 0, 0)
            connection.sendall(packet(12, 2, instance_payload))
            status, created_instance = receive_response(connection, 12, 2)
            assert status == 0 and len(created_instance) == 16
            vulkan_result, _, instance_id = struct.unpack("<iIQ", created_instance)
            assert vulkan_result == 0 and instance_id >> 56 == 1

            connection.sendall(packet(14, 3, struct.pack("<Q", instance_id)))
            status, physical_devices = receive_response(connection, 14, 3)
            assert status == 0
            vulkan_result, count = struct.unpack_from("<iI", physical_devices)
            assert vulkan_result == 0 and count == 1
            physical_id = struct.unpack_from("<Q", physical_devices, 8)[0]
            assert physical_id >> 56 == 2

            priority_bits = struct.unpack("<I", struct.pack("<f", 1.0))[0]
            extension_list = [
                b"VK_KHR_external_memory",
                b"VK_KHR_external_memory_fd",
            ]
            if hardware_buffer_api is not None:
                extension_list.append(
                    b"VK_ANDROID_external_memory_android_hardware_buffer"
                )
            extension_names = b"".join(
                name.ljust(128, b"\0") for name in extension_list
            )
            device_payload = struct.pack(
                "<QIIIIII", physical_id, 0, 0, 1, priority_bits, 0,
                len(extension_list),
            ) + extension_names
            connection.sendall(packet(57, 4, device_payload))
            status, created_device = receive_response(connection, 57, 4)
            assert status == 0 and len(created_device) == 16
            vulkan_result, _, device_id = struct.unpack("<iIQ", created_device)
            assert vulkan_result == 0 and device_id >> 56 == 3

            generation = 0xE053000000000002
            prepare_payload = struct.pack(
                "<QIIIIIIQ", device_id, 64, 64, 37, 0x10, 3, 0, generation
            )
            connection.sendall(packet(64, 5, prepare_payload))
            activity_connection, _ = activity_frame_listener.accept()
            with activity_connection:
                setup_data, setup_ancillary, setup_flags, _ = activity_connection.recvmsg(
                    128, socket.CMSG_SPACE(5 * array.array("i").itemsize)
                )
                assert not setup_flags & (socket.MSG_CTRUNC | socket.MSG_TRUNC)
                if len(setup_data) < 128:
                    setup_data += receive_exact(activity_connection, 128 - len(setup_data))
                for level, kind, value in setup_ancillary:
                    if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                        received = array.array("i")
                        received.frombytes(
                            value[: len(value) - len(value) % received.itemsize]
                        )
                        activity_descriptors.extend(received)
                setup_image_count = struct.unpack_from("<I", setup_data, 8)[0]
                setup_wire_flags = struct.unpack_from("<I", setup_data, 28)[0]
                if hardware_buffer_api is not None:
                    assert setup_wire_flags == 2
                    activity_hardware_buffers = hardware_buffer_api.receive_many(
                        activity_connection, setup_image_count
                    )
            assert struct.unpack_from("<IHH", setup_data) == (0x31544642, 1, 128)
            setup_image_count, setup_width, setup_height, setup_format, setup_usage, setup_flags = struct.unpack_from(
                "<IIIIII", setup_data, 8
            )
            setup_generation = struct.unpack_from("<Q", setup_data, 32)[0]
            assert (setup_image_count, setup_width, setup_height, setup_format,
                    setup_usage, setup_flags, setup_generation) == (
                3, 64, 64, 37, 0x10,
                2 if hardware_buffer_api is not None else 1,
                generation,
            )
            assert len(activity_descriptors) == (
                1 if hardware_buffer_api is not None else setup_image_count + 1
            )
            assert len(activity_hardware_buffers) == (
                setup_image_count if hardware_buffer_api is not None else 0
            )
            status, prepared, descriptors = receive_fd_response(connection, 64, 5)
            assert status == 0 and len(prepared) == 128
            vulkan_result, image_count, swapchain_id, returned_generation, control_bytes, prepared_flags = struct.unpack_from(
                "<iIQQII", prepared
            )
            assert vulkan_result == 0 and image_count == 3
            assert swapchain_id >> 56 == 6
            assert returned_generation == generation
            assert control_bytes == 4096
            assert prepared_flags == (
                1 if hardware_buffer_api is not None else 0
            )
            assert len(descriptors) == (
                1 if hardware_buffer_api is not None else image_count + 1
            )
            for index in range(image_count):
                image_id, allocation_size, memory_type, image_reserved = struct.unpack_from(
                    "<QQII", prepared, 32 + index * 24
                )
                assert image_id >> 56 == 7
                if hardware_buffer_api is None:
                    assert allocation_size == os.fstat(descriptors[index]).st_size
                    assert allocation_size == os.fstat(
                        activity_descriptors[index]
                    ).st_size
                assert memory_type == 0 and image_reserved == 0
            assert os.fstat(descriptors[-1]).st_size == 4096
            assert os.fstat(activity_descriptors[-1]).st_size == 4096
            with mmap.mmap(descriptors[-1], 4096) as control:
                magic, version, header_bytes, slots, ring_flags, ring_generation = struct.unpack_from(
                    "<IHHIIQ", control
                )
                assert (magic, version, header_bytes, slots, ring_flags) == (
                    0x31525742,
                    1,
                    128,
                    3,
                    0,
                )
                assert ring_generation == generation

            for descriptor in descriptors:
                os.close(descriptor)
            descriptors = []
            for descriptor in activity_descriptors:
                os.close(descriptor)
            activity_descriptors = []
            if hardware_buffer_api is not None:
                hardware_buffer_api.release_many(activity_hardware_buffers)
            connection.sendall(packet(65, 6, struct.pack("<Q", swapchain_id)))
            status, destroyed = receive_response(connection, 65, 6)
            assert status == 0 and destroyed == b""
            connection.close()
            connection = None
            server_stdout, server_stderr = server.communicate(timeout=5.0)
            assert server.returncode == 0, (server_stdout, server_stderr)
        finally:
            for descriptor in descriptors:
                os.close(descriptor)
            for descriptor in activity_descriptors:
                os.close(descriptor)
            if hardware_buffer_api is not None:
                hardware_buffer_api.release_many(activity_hardware_buffers)
            activity_frame_listener.close()
            if connection is not None:
                connection.close()
            if server.poll() is None:
                server.terminate()
                server.wait(timeout=5.0)
    print("PASS: authenticated service exported persistent WSI image ring")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
