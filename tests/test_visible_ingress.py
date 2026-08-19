#!/usr/bin/env python3

import json
import os
import pathlib
import subprocess
import sys


def run_contract(server_path: str, client_path: str, tcp: bool) -> None:
    socket_name = f"bvb-ingress-contract-{os.getpid()}"
    token = bytes(range(1, 33)).hex()
    server_transport = "--tcp" if tcp else socket_name
    server = subprocess.Popen(
        [server_path, server_transport, token, "1280", "720"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        assert server.stdout is not None
        ready = server.stdout.readline().strip()
        if tcp:
            prefix, port_text = ready.split()
            assert prefix == "READY"
            transport_arguments = ["--tcp-port", port_text]
        else:
            assert ready == "READY"
            transport_arguments = ["--socket-name", socket_name]
        rejected = subprocess.run(
            [
                client_path,
                *transport_arguments,
                "--token",
                bytes([0xFF] * 32).hex(),
                "--width",
                "1280",
                "--height",
                "720",
            ],
            capture_output=True,
            text=True,
            timeout=10.0,
            check=False,
        )
        assert rejected.returncode != 0
        assert "Permission denied (-13)" in rejected.stderr, rejected.stderr

        client = subprocess.run(
            [
                client_path,
                *transport_arguments,
                "--token",
                token,
                "--width",
                "1280",
                "--height",
                "720",
            ],
            capture_output=True,
            text=True,
            timeout=10.0,
            check=False,
        )
        assert client.returncode == 0, client.stderr
        client_document = json.loads(client.stdout)
        assert client_document["batch_bytes"] == 200
        assert client_document["commands"] == 6
        if tcp:
            assert client_document["transport"] == "loopback_tcp_inline"
            assert client_document["packet_bytes"] == 256
            assert client_document["round_trip_ns"] > 0

        server_stdout, server_stderr = server.communicate(timeout=10.0)
        assert server.returncode == 0, server_stderr
        server_document = json.loads(server_stdout)
        assert server_document == {
            "batch_bytes": 200,
            "sequence": 1,
            "commands": 6,
        }
    finally:
        if server.poll() is None:
            server.terminate()
            server.wait(timeout=5.0)


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_visible_ingress.py SERVER CLIENT")
    server_path = str(pathlib.Path(sys.argv[1]).resolve())
    client_path = str(pathlib.Path(sys.argv[2]).resolve())
    run_contract(server_path, client_path, tcp=False)
    run_contract(server_path, client_path, tcp=True)
    print("PASS: authenticated Unix/memfd and TCP/inline visible ingress")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
