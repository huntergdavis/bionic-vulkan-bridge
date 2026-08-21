#!/usr/bin/env python3

"""Run a global-dispatch client against a synthetic authenticated Activity.

This harness owns the real bridge service, supplies only the Activity lifecycle
and one-time frame-setup sink that are unavailable in standalone Termux, and
keeps every received descriptor open until the client and service have exited.
It does not import, render, or display any frame.
"""

from __future__ import annotations

import argparse
import array
import json
import os
import pathlib
import re
import secrets
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
from typing import Any


LIFECYCLE_MAGIC = 0x314C5642
LIFECYCLE_VERSION = 1
LIFECYCLE_RECORD = struct.Struct("<IHHIIIIQ32s")
LIFECYCLE_ACK = struct.Struct("<IHHIi")
FRAME_SETUP_MAGIC = 0x31544642
FRAME_SETUP_VERSION = 1
FRAME_SETUP_BYTES = 128
MAX_FRAME_IMAGES = 4
FRAME_SETUP_PREFIX = struct.Struct("<IHHIIIIIIQ")
PEER_CREDENTIALS = struct.Struct("3i")
ACTIVITY_EVENTS = (1, 2, 3, 7, 11, 9)


class HarnessFailure(RuntimeError):
    pass


def receive_exact(channel: socket.socket, byte_count: int) -> bytes:
    output = bytearray()
    while len(output) < byte_count:
        chunk = channel.recv(byte_count - len(output))
        if not chunk:
            raise HarnessFailure("connection closed before fixed record completed")
        output.extend(chunk)
    return bytes(output)


def send_lifecycle(
    port: int,
    token: bytes,
    event: int,
    sequence: int,
    width: int,
    height: int,
    timeout: float,
) -> None:
    record_width = width if event in (7, 11) else 0
    record_height = height if event in (7, 11) else 0
    record = LIFECYCLE_RECORD.pack(
        LIFECYCLE_MAGIC,
        LIFECYCLE_VERSION,
        event,
        sequence,
        record_width,
        record_height,
        os.getpid(),
        time.monotonic_ns(),
        token,
    )
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as channel:
        channel.sendall(record)
        acknowledgement = receive_exact(channel, LIFECYCLE_ACK.size)
    magic, version, reserved, accepted_sequence, status_code = (
        LIFECYCLE_ACK.unpack(acknowledgement)
    )
    if (magic, version, reserved, accepted_sequence, status_code) != (
        LIFECYCLE_MAGIC,
        LIFECYCLE_VERSION,
        0,
        sequence,
        0,
    ):
        raise HarnessFailure(
            f"Activity lifecycle event {event} was rejected: "
            f"sequence={accepted_sequence} status={status_code}"
        )


def decode_frame_setup(wire: bytes, descriptor_count: int) -> dict[str, Any]:
    if len(wire) != FRAME_SETUP_BYTES:
        raise HarnessFailure(f"frame setup length is {len(wire)}, expected 128")
    (
        magic,
        version,
        header_bytes,
        image_count,
        width,
        height,
        image_format,
        image_usage,
        flags,
        generation,
    ) = FRAME_SETUP_PREFIX.unpack_from(wire)
    allocation_sizes = list(struct.unpack_from("<4Q", wire, 40))
    memory_type_indices = list(struct.unpack_from("<4I", wire, 72))
    reserved = struct.unpack_from("<10I", wire, 88)
    if (magic, version, header_bytes) != (
        FRAME_SETUP_MAGIC,
        FRAME_SETUP_VERSION,
        FRAME_SETUP_BYTES,
    ):
        raise HarnessFailure("frame setup has an invalid magic/version/header")
    if not 2 <= image_count <= MAX_FRAME_IMAGES:
        raise HarnessFailure(f"frame setup image count is invalid: {image_count}")
    if width == 0 or height == 0 or image_format == 0 or image_usage == 0:
        raise HarnessFailure("frame setup has zero required image metadata")
    if flags != 0 or generation == 0 or any(reserved):
        raise HarnessFailure("frame setup has unsupported flags/generation/reserved data")
    if any(size == 0 for size in allocation_sizes[:image_count]):
        raise HarnessFailure("active frame setup allocation size is zero")
    if any(size != 0 for size in allocation_sizes[image_count:]):
        raise HarnessFailure("inactive frame setup allocation size is nonzero")
    if descriptor_count != image_count + 1:
        raise HarnessFailure(
            f"frame setup carried {descriptor_count} FDs for {image_count} images"
        )
    return {
        "received": True,
        "image_count": image_count,
        "width": width,
        "height": height,
        "format": image_format,
        "image_usage": image_usage,
        "generation": generation,
        "allocation_sizes": allocation_sizes[:image_count],
        "memory_type_indices": memory_type_indices[:image_count],
        "descriptor_count": descriptor_count,
    }


class FrameSetupSink:
    def __init__(self, socket_name: str, timeout: float):
        self._listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._listener.bind("\0" + socket_name)
        self._listener.listen(1)
        self._listener.settimeout(0.2)
        self._timeout = timeout
        self._stop = threading.Event()
        self._done = threading.Event()
        self._thread = threading.Thread(target=self._receive, daemon=True)
        self.descriptors: list[int] = []
        self.setup: dict[str, Any] | None = None
        self.error: BaseException | None = None

    def start(self) -> None:
        self._thread.start()

    def _receive(self) -> None:
        deadline = time.monotonic() + self._timeout
        connection: socket.socket | None = None
        received_descriptors: list[int] = []
        try:
            while not self._stop.is_set() and time.monotonic() < deadline:
                try:
                    connection, _ = self._listener.accept()
                    break
                except socket.timeout:
                    continue
            if connection is None:
                raise HarnessFailure("timed out waiting for Activity frame setup")
            connection.settimeout(self._timeout)
            credentials = connection.getsockopt(
                socket.SOL_SOCKET,
                getattr(socket, "SO_PEERCRED", 17),
                PEER_CREDENTIALS.size,
            )
            _, peer_uid, _ = PEER_CREDENTIALS.unpack(credentials)
            if peer_uid != os.geteuid():
                raise HarnessFailure(
                    f"frame setup peer UID {peer_uid} does not match {os.geteuid()}"
                )
            wire, ancillary, message_flags, _ = connection.recvmsg(
                FRAME_SETUP_BYTES + 1,
                socket.CMSG_SPACE((MAX_FRAME_IMAGES + 1) * array.array("i").itemsize),
            )
            if message_flags & (socket.MSG_CTRUNC | socket.MSG_TRUNC):
                raise HarnessFailure("frame setup record or FD bundle was truncated")
            for level, kind, value in ancillary:
                if level != socket.SOL_SOCKET or kind != socket.SCM_RIGHTS:
                    raise HarnessFailure("frame setup carried unsupported ancillary data")
                descriptors = array.array("i")
                descriptors.frombytes(value[: len(value) - len(value) % descriptors.itemsize])
                received_descriptors.extend(descriptors)
            if len(wire) < FRAME_SETUP_BYTES:
                wire += receive_exact(connection, FRAME_SETUP_BYTES - len(wire))
            if len(wire) != FRAME_SETUP_BYTES:
                raise HarnessFailure("frame setup included trailing payload bytes")
            for descriptor in received_descriptors:
                os.fstat(descriptor)
            self.setup = decode_frame_setup(wire, len(received_descriptors))
            self.descriptors = received_descriptors
            received_descriptors = []
        except BaseException as error:  # Propagate thread failures to the owner.
            self.error = error
        finally:
            if connection is not None:
                connection.close()
            for descriptor in received_descriptors:
                try:
                    os.close(descriptor)
                except OSError:
                    pass
            self._done.set()

    def wait(self) -> dict[str, Any]:
        if not self._done.wait(self._timeout):
            raise HarnessFailure("Activity frame setup receiver did not complete")
        if self.error is not None:
            raise HarnessFailure(str(self.error)) from self.error
        if self.setup is None:
            raise HarnessFailure("Activity frame setup receiver returned no metadata")
        return self.setup

    def close(self) -> None:
        self._stop.set()
        self._listener.close()
        self._thread.join(timeout=1.0)
        for descriptor in self.descriptors:
            try:
                os.close(descriptor)
            except OSError:
                pass
        self.descriptors.clear()


def stop_process(process: subprocess.Popen[Any] | None) -> None:
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=2.0)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2.0)


def wait_for_service(
    process: subprocess.Popen[Any],
    stdout_path: pathlib.Path,
    control_socket: pathlib.Path,
    timeout: float,
) -> int:
    deadline = time.monotonic() + timeout
    expression = re.compile(r"activity_port=(\d+)\s*$", re.MULTILINE)
    while time.monotonic() < deadline:
        output = stdout_path.read_text(errors="replace")
        match = expression.search(output)
        if match is not None and control_socket.exists():
            port = int(match.group(1))
            if not 0 < port <= 65535:
                raise HarnessFailure(f"service returned invalid Activity port {port}")
            if not stat.S_ISSOCK(control_socket.stat().st_mode):
                raise HarnessFailure("service control path is not a socket")
            return port
        return_code = process.poll()
        if return_code is not None:
            raise HarnessFailure(f"bridge service exited before ready: {return_code}")
        time.sleep(0.02)
    raise HarnessFailure("timed out waiting for bridge service readiness")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--service", required=True)
    parser.add_argument("--service-loader")
    parser.add_argument("--runtime-parent")
    parser.add_argument("--width", type=int, default=2800)
    parser.add_argument("--height", type=int, default=1752)
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--service-stdout", required=True)
    parser.add_argument("--service-stderr", required=True)
    parser.add_argument("--client-stdout", required=True)
    parser.add_argument("--client-stderr", required=True)
    parser.add_argument("--result-json", required=True)
    parser.add_argument("client_command", nargs=argparse.REMAINDER)
    arguments = parser.parse_args()
    if arguments.client_command[:1] == ["--"]:
        arguments.client_command = arguments.client_command[1:]
    if not arguments.client_command:
        parser.error("a client command is required after --")
    if arguments.width <= 0 or arguments.height <= 0:
        parser.error("width and height must be positive")
    if arguments.timeout <= 0:
        parser.error("timeout must be positive")
    return arguments


def run(arguments: argparse.Namespace) -> int:
    service = str(pathlib.Path(arguments.service).resolve(strict=True))
    loader = (
        str(pathlib.Path(arguments.service_loader).resolve(strict=True))
        if arguments.service_loader
        else None
    )
    output_paths = {
        name: pathlib.Path(getattr(arguments, name.replace("-", "_"))).resolve()
        for name in (
            "service-stdout",
            "service-stderr",
            "client-stdout",
            "client-stderr",
            "result-json",
        )
    }
    for path in output_paths.values():
        path.parent.mkdir(parents=True, exist_ok=True)

    runtime_parent = (
        pathlib.Path(arguments.runtime_parent).resolve(strict=True)
        if arguments.runtime_parent
        else None
    )
    runtime_directory = pathlib.Path(
        tempfile.mkdtemp(prefix="bvb-global-activity-", dir=runtime_parent)
    )
    control_socket = runtime_directory / "bridge.sock"
    token = secrets.token_bytes(32)
    frame_socket_name = f"bvb-global-frame-{os.getpid()}-{secrets.token_hex(8)}"
    frame_sink = FrameSetupSink(frame_socket_name, arguments.timeout)
    service_process: subprocess.Popen[Any] | None = None
    client_process: subprocess.Popen[Any] | None = None
    service_stdout_handle = None
    service_stderr_handle = None
    client_stdout_handle = None
    client_stderr_handle = None
    frame_setup: dict[str, Any] | None = None
    result: dict[str, Any] = {
        "schema_version": 1,
        "result": "fail",
        "synthetic_activity": True,
        "authenticated_activity_events": list(ACTIVITY_EVENTS),
        "authenticated_event_count": 0,
        "requested_width": arguments.width,
        "requested_height": arguments.height,
        "activity_frame_setup": {"received": False},
        "client_exit": None,
        "service_exit": None,
        "visible_frame_claim": False,
        "fps_claim": False,
        "visibility_boundary": (
            "one-time frame FD receipt only; no image import, rendering, or display"
        ),
    }
    failure: BaseException | None = None
    previous_handlers: dict[int, Any] = {}

    def interrupted(signum: int, _frame: Any) -> None:
        raise HarnessFailure(f"interrupted by signal {signum}")

    try:
        for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            previous_handlers[signum] = signal.signal(signum, interrupted)
        service_stdout_handle = output_paths["service-stdout"].open("wb")
        service_stderr_handle = output_paths["service-stderr"].open("wb")
        service_command = [service, "--socket", str(control_socket), "--once"]
        if loader is not None:
            service_command.extend(["--loader", loader])
        service_command.extend(
            [
                "--activity-port",
                "0",
                "--activity-token",
                token.hex(),
                "--activity-frame-socket",
                frame_socket_name,
            ]
        )
        frame_sink.start()
        service_process = subprocess.Popen(
            service_command,
            stdout=service_stdout_handle,
            stderr=service_stderr_handle,
        )
        activity_port = wait_for_service(
            service_process,
            output_paths["service-stdout"],
            control_socket,
            arguments.timeout,
        )
        for sequence, event in enumerate(ACTIVITY_EVENTS, start=1):
            send_lifecycle(
                activity_port,
                token,
                event,
                sequence,
                arguments.width,
                arguments.height,
                arguments.timeout,
            )
        result["authenticated_event_count"] = len(ACTIVITY_EVENTS)

        client_environment = os.environ.copy()
        client_environment["BVB_BRIDGE_SOCKET"] = str(control_socket)
        client_stdout_handle = output_paths["client-stdout"].open("wb")
        client_stderr_handle = output_paths["client-stderr"].open("wb")
        client_process = subprocess.Popen(
            arguments.client_command,
            stdout=client_stdout_handle,
            stderr=client_stderr_handle,
            env=client_environment,
        )
        try:
            result["client_exit"] = client_process.wait(timeout=arguments.timeout)
        except subprocess.TimeoutExpired as error:
            raise HarnessFailure("global-dispatch client timed out") from error
        if result["client_exit"] != 0:
            raise HarnessFailure(
                f"global-dispatch client exited {result['client_exit']}"
            )
        frame_setup = frame_sink.wait()
        result["activity_frame_setup"] = frame_setup
        if (
            frame_setup["width"],
            frame_setup["height"],
            frame_setup["image_count"],
        ) != (arguments.width, arguments.height, 3):
            raise HarnessFailure("frame setup does not match the global WSI request")
        try:
            result["service_exit"] = service_process.wait(timeout=arguments.timeout)
        except subprocess.TimeoutExpired as error:
            raise HarnessFailure("bridge service did not exit after its client") from error
        if result["service_exit"] != 0:
            raise HarnessFailure(f"bridge service exited {result['service_exit']}")
        if output_paths["client-stderr"].stat().st_size != 0:
            raise HarnessFailure("global-dispatch client emitted stderr")
        if output_paths["service-stderr"].stat().st_size != 0:
            raise HarnessFailure("bridge service emitted stderr")
        result["result"] = "pass"
    except BaseException as error:
        failure = error
        result["error"] = str(error)
    finally:
        stop_process(client_process)
        stop_process(service_process)
        if client_process is not None:
            result["client_exit"] = client_process.returncode
        if service_process is not None:
            result["service_exit"] = service_process.returncode
        frame_sink.close()
        for handle in (
            client_stdout_handle,
            client_stderr_handle,
            service_stdout_handle,
            service_stderr_handle,
        ):
            if handle is not None:
                handle.close()
        if control_socket.exists():
            mode = control_socket.lstat().st_mode
            if not stat.S_ISSOCK(mode):
                failure = failure or HarnessFailure(
                    "refusing to remove non-socket control path"
                )
            else:
                control_socket.unlink()
        try:
            runtime_directory.rmdir()
        except OSError as error:
            failure = failure or HarnessFailure(
                f"could not remove owned runtime directory: {error}"
            )
        for signum, handler in previous_handlers.items():
            signal.signal(signum, handler)
        if failure is not None:
            result["result"] = "fail"
            result["error"] = str(failure)
        output_paths["result-json"].write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n"
        )

    if failure is not None:
        print(f"global Activity harness: {failure}", file=sys.stderr)
        return 1
    print(
        "global_activity_harness=PASS "
        f"events={result['authenticated_event_count']} "
        f"frame_fds={frame_setup['descriptor_count']} "
        "visible_frame_claim=false fps_claim=false"
    )
    return 0


def main() -> int:
    return run(parse_arguments())


if __name__ == "__main__":
    raise SystemExit(main())
