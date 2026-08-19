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


if __name__ == "__main__":
    main()
