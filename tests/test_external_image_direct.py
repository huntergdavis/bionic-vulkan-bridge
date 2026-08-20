#!/usr/bin/env python3
import array
import json
import os
import socket
import struct
import subprocess
import sys


def image_response():
    width = 64
    height = 64
    expected_color = 0xFFFF00FF
    image_bytes = width * height * 4
    image_memory = os.memfd_create("bvb-e039-image", os.MFD_CLOEXEC)
    image_semaphore = os.memfd_create("bvb-e039-semaphore", os.MFD_CLOEXEC)
    os.ftruncate(image_memory, image_bytes)
    os.pwrite(
        image_memory,
        struct.pack("<I", expected_color) * (width * height),
        0,
    )
    response = struct.pack("<iQIII", 0, image_bytes, 0, width, height)
    return response, image_memory, image_semaphore, width, height


def assert_result(client, width, height, transport):
    output, error = client.communicate(timeout=10)
    assert client.returncode == 0, error
    document = json.loads(output)
    assert document["gate"] == "E039"
    assert document["result"] == "pass"
    assert document["transport"] == transport
    assert document["binder_calls"] == 0
    assert document["java_calls"] == 0
    assert document["channel_acknowledged"] is True
    assert document["width"] == width and document["height"] == height
    assert document["mismatched_pixels"] == 0


def test_stream(client_path, loader, token):
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind("\0bvb-visible-external-memory")
    listener.listen(1)
    client = subprocess.Popen(
        [client_path, "--token", token, "--loader", loader],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    connection, _ = listener.accept()
    request = b""
    while len(request) < 65:
        request += connection.recv(65 - len(request))
    assert request == token.encode("ascii") + b"P"
    response, image_memory, image_semaphore, width, height = image_response()
    rights = array.array("i", [image_memory, image_semaphore])
    connection.sendmsg(
        [response], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, rights.tobytes())]
    )
    os.close(image_memory)
    os.close(image_semaphore)
    assert connection.recv(1) == b"\xA5"
    connection.sendall(struct.pack("<iI", 0, 0xE039C0DE))
    connection.close()
    listener.close()
    assert_result(
        client, width, height, "direct_native_capability_socket_scm_rights"
    )


def test_datagram(client_path, loader, token):
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    listener.bind("\0bvb-visible-external-memory-dgram")
    client = subprocess.Popen(
        [client_path, "--token", token, "--loader", loader, "--datagram"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    request, client_address = listener.recvfrom(65)
    assert request == token.encode("ascii") + b"D"
    response, image_memory, image_semaphore, width, height = image_response()
    rights = array.array("i", [image_memory, image_semaphore])
    listener.sendmsg(
        [response],
        [(socket.SOL_SOCKET, socket.SCM_RIGHTS, rights.tobytes())],
        0,
        client_address,
    )
    os.close(image_memory)
    os.close(image_semaphore)
    acknowledgement, acknowledgement_address = listener.recvfrom(1)
    assert acknowledgement == b"\xA5"
    assert acknowledgement_address == client_address
    listener.sendto(struct.pack("<iI", 0, 0xE039C0DE), client_address)
    listener.close()
    assert_result(
        client, width, height, "direct_native_capability_datagram_scm_rights"
    )


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_external_image_direct.py CLIENT LOADER")
    client_path, loader = map(os.path.abspath, sys.argv[1:])
    token = "39" * 32
    test_stream(client_path, loader, token)
    test_datagram(client_path, loader, token)


if __name__ == "__main__":
    main()
