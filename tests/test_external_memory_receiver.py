#!/usr/bin/env python3
import array
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_external_memory_receiver.py RECEIVER LOADER")
    receiver, loader = map(os.path.abspath, sys.argv[1:])
    socket_name = f"bvb-e036-host-{os.getpid()}"
    process = subprocess.Popen(
        [receiver, "--socket", socket_name, "--loader", loader],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    assert process.stdout.readline().startswith("bvb-external-memory-receiver: ready")
    descriptor = os.memfd_create("bvb-e036-host", os.MFD_CLOEXEC)
    os.ftruncate(descriptor, 4096)
    pattern = bytes((index ^ (index >> 4) ^ 0x5A) & 0xFF for index in range(4096))
    os.pwrite(descriptor, pattern, 0)
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.connect("\0" + socket_name)
    header = struct.pack("<IHHHHIIi", 0x31425642, 1, 1, 50, 0,
                         0xE036, 16, 0)
    payload = struct.pack("<QII", 4096, 0, 4096)
    rights = array.array("i", [descriptor])
    client.sendmsg([header + payload], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                                         rights.tobytes())])
    response = b""
    while len(response) < 24:
        response += client.recv(24 - len(response))
    client.close()
    os.close(descriptor)
    fields = struct.unpack("<IHHHHIIi", response)
    assert fields == (0x31425642, 1, 2, 50, 0, 0xE036, 0, 0)
    output, error = process.communicate(timeout=10)
    assert process.returncode == 0, error
    document = json.loads(output)
    assert document["gate"] == "E036"
    assert document["result"] == "pass"
    assert document["buffer_bytes"] == 4096
    assert document["mismatched_bytes"] == 0

    sync_socket_name = f"bvb-e037-host-{os.getpid()}"
    sync_process = subprocess.Popen(
        [receiver, "--socket", sync_socket_name, "--loader", loader],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    assert sync_process.stdout.readline().startswith(
        "bvb-external-memory-receiver: ready"
    )
    sync_memory = os.memfd_create("bvb-e037-memory", os.MFD_CLOEXEC)
    sync_semaphore = os.memfd_create("bvb-e037-semaphore", os.MFD_CLOEXEC)
    os.ftruncate(sync_memory, 4096)
    expected_fill_word = 0xE037C0DE
    os.pwrite(sync_memory, struct.pack("<I", expected_fill_word) * 1024, 0)
    sync_client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sync_client.connect("\0" + sync_socket_name)
    sync_header = struct.pack(
        "<IHHHHIIi", 0x31425642, 1, 1, 51, 0, 0xE037, 24, 0
    )
    sync_payload = struct.pack(
        "<QIIII", 4096, 0, 4096, expected_fill_word, 0
    )
    sync_rights = array.array("i", [sync_memory, sync_semaphore])
    sync_client.sendmsg(
        [sync_header + sync_payload],
        [(socket.SOL_SOCKET, socket.SCM_RIGHTS, sync_rights.tobytes())],
    )
    sync_response = b""
    while len(sync_response) < 24:
        sync_response += sync_client.recv(24 - len(sync_response))
    sync_client.close()
    os.close(sync_memory)
    os.close(sync_semaphore)
    sync_fields = struct.unpack("<IHHHHIIi", sync_response)
    assert sync_fields == (0x31425642, 1, 2, 51, 0, 0xE037, 0, 0)
    sync_output, sync_error = sync_process.communicate(timeout=10)
    assert sync_process.returncode == 0, sync_error
    sync_document = json.loads(sync_output)
    assert sync_document["gate"] == "E037"
    assert sync_document["result"] == "pass"
    assert sync_document["descriptor_count"] == 2
    assert sync_document["buffer_bytes"] == 4096
    assert sync_document["expected_fill_word"] == expected_fill_word
    assert sync_document["mismatched_words"] == 0
    assert sync_document["gpu_wait_elapsed_ns"] >= 0


if __name__ == "__main__":
    main()
