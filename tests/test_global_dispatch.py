#!/usr/bin/env python3

import os
import array
import mmap
import pathlib
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

from android_hardware_buffer import load_android_hardware_buffer_api


def main() -> int:
    if len(sys.argv) not in (4, 5):
        raise SystemExit(
            "usage: test_global_dispatch.py SERVICE CLIENT FAKE_LOADER "
            "[strict-fake|hardware|shared-command-stream|"
            "shared-command-stream-concurrency|"
            "shared-command-stream-non-success|strict-mapped-memory|"
            "shared-mapped-memory|shared-noncoherent-memory|"
            "shared-memory-unmap-lost-ack|first-rejection-command|"
            "first-rejection-wsi|loader-tls-lifetime]"
        )
    service, client, loader = map(
        lambda value: str(pathlib.Path(value).resolve()), sys.argv[1:4]
    )
    validation_mode = sys.argv[4] if len(sys.argv) == 5 else "strict-fake"
    hardware_buffer_api = load_android_hardware_buffer_api()
    if validation_mode not in (
        "strict-fake", "hardware", "shared-command-stream",
        "shared-command-stream-concurrency",
        "shared-command-stream-non-success",
        "strict-mapped-memory", "shared-mapped-memory",
        "shared-noncoherent-memory", "shared-memory-unmap-lost-ack",
        "first-rejection-command", "first-rejection-wsi",
        "loader-tls-lifetime",
    ):
        raise SystemExit(f"unsupported validation mode: {validation_mode}")
    with tempfile.TemporaryDirectory(prefix="bvb-e034-") as temporary:
        socket_path = pathlib.Path(temporary) / "runtime" / "bridge.sock"
        token = bytes.fromhex(
            "00112233445566778899aabbccddeeff"
            "fedcba98765432100123456789abcdef"
        )
        activity_frame_name = f"bvb-e060-global-{os.getpid()}"
        activity_frame_listener = socket.socket(socket.AF_UNIX,
                                                socket.SOCK_STREAM)
        activity_frame_listener.bind("\0" + activity_frame_name)
        activity_frame_listener.listen(1)
        activity_frame_listener.settimeout(2.0)
        server_environment = os.environ.copy()
        server_environment["BVB_FAKE_HIDE_SWAPCHAIN"] = "1"
        if validation_mode not in (
            "shared-command-stream-concurrency", "first-rejection-command",
            "first-rejection-wsi",
        ):
            server_environment["BVB_FAKE_REQUIRE_INIT_IMAGE_COMMANDS"] = "1"
        if validation_mode == "shared-command-stream":
            server_environment["BVB_FAKE_REQUIRE_ANIMATED_WSI"] = "1"
        if validation_mode == "hardware":
            server_environment["BVB_FAKE_REAL_HARDWARE_VALUES"] = "1"
        if validation_mode == "shared-command-stream-non-success":
            server_environment["BVB_FAKE_QUEUE_SUBMIT2_FAIL_AT"] = "3"
        if validation_mode in (
            "shared-mapped-memory", "shared-noncoherent-memory",
            "shared-memory-unmap-lost-ack",
        ):
            server_environment["BVB_FAKE_REQUIRE_MEMORY_MIRROR"] = "1"
        if validation_mode == "shared-noncoherent-memory":
            server_environment["BVB_FAKE_NONCOHERENT_MEMORY"] = "1"
        if validation_mode == "strict-mapped-memory":
            server_environment["BVB_FAKE_KEEP_MEMORY_MAPPED"] = "1"
        if validation_mode == "loader-tls-lifetime":
            server_environment["BVB_FAKE_INSTALL_TLS_DESTRUCTOR"] = "1"
        server_command = [
                service,
                "--socket",
                str(socket_path),
                "--loader",
                loader,
                "--activity-port",
                "0",
                "--activity-token",
                token.hex(),
                "--activity-frame-socket",
                activity_frame_name,
            ]
        if validation_mode != "loader-tls-lifetime":
            server_command.append("--once")
        server = subprocess.Popen(
            server_command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=server_environment,
        )
        try:
            deadline = time.monotonic() + 5.0
            while not socket_path.exists() and time.monotonic() < deadline:
                if server.poll() is not None:
                    break
                time.sleep(0.01)
            assert socket_path.exists(), server.communicate(timeout=1.0)
            ready = server.stdout.readline()
            assert "ready socket=" in ready
            activity_port = int(ready.split("activity_port=", 1)[1].strip())
            assert 0 < activity_port <= 65535

            def send_event(event: int, sequence: int,
                           width: int = 0, height: int = 0) -> None:
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
                    token,
                )
                with socket.create_connection(
                    ("127.0.0.1", activity_port), timeout=1.0
                ) as connection:
                    connection.sendall(record)
                    ack = connection.recv(16)
                magic, version, reserved, accepted_sequence, status = (
                    struct.unpack("<IHHIi", ack)
                )
                assert magic == 0x314C5642
                assert version == 1
                assert reserved == 0
                assert accepted_sequence == sequence
                assert status == 0

            send_event(1, 1)
            send_event(2, 2)
            send_event(3, 3)
            send_event(7, 4, 2800, 1752)
            send_event(11, 5, 2800, 1752)
            send_event(9, 6)
            environment = os.environ.copy()
            environment["BVB_BRIDGE_SOCKET"] = str(socket_path)
            if validation_mode == "hardware":
                environment["BVB_GLOBAL_DISPATCH_HARDWARE"] = "1"
            else:
                environment.pop("BVB_GLOBAL_DISPATCH_HARDWARE", None)
            shared_command_stream = validation_mode.startswith(
                "shared-command-stream"
            ) or validation_mode == "first-rejection-command"
            if shared_command_stream:
                environment["BVB_COMMAND_STREAM"] = "shared"
            else:
                environment.pop("BVB_COMMAND_STREAM", None)
            if validation_mode == "shared-command-stream-non-success":
                environment["BVB_EXPECT_STREAM_SUBMIT_FAILURE"] = "1"
            if validation_mode == "shared-command-stream":
                environment["BVB_TEST_ANIMATED_WSI"] = "1"
            if validation_mode == "shared-command-stream-concurrency":
                environment["BVB_TEST_CONCURRENT_COMMAND_STREAMS"] = "1"
            if validation_mode.startswith("first-rejection-"):
                environment["BVB_FIRST_REJECTION_DIAGNOSTIC"] = "1"

            if validation_mode in (
                "shared-mapped-memory", "shared-noncoherent-memory",
                "shared-memory-unmap-lost-ack",
            ):
                environment["BVB_MAPPED_MEMORY"] = "shared"
            else:
                environment["BVB_MAPPED_MEMORY"] = "strict"
            if validation_mode in (
                "strict-mapped-memory", "shared-mapped-memory",
                "shared-noncoherent-memory", "shared-memory-unmap-lost-ack",
            ):
                environment["BVB_TEST_KEEP_MEMORY_MAPPED"] = "1"
            else:
                environment.pop("BVB_TEST_KEEP_MEMORY_MAPPED", None)
            if validation_mode == "shared-noncoherent-memory":
                environment["BVB_TEST_NONCOHERENT_MEMORY"] = "1"
            else:
                environment.pop("BVB_TEST_NONCOHERENT_MEMORY", None)
            if validation_mode == "shared-memory-unmap-lost-ack":
                environment["BVB_TEST_DROP_MEMORY_UNMAP_ACK"] = "1"
            else:
                environment.pop("BVB_TEST_DROP_MEMORY_UNMAP_ACK", None)

            sink_result = {}

            def consume_activity_frames(expected_frames: int) -> None:
                received_fds = array.array("i")
                received_hardware_buffers = []
                ring_mapping = None
                try:
                    frame_connection, _ = activity_frame_listener.accept()
                    with frame_connection:
                        setup, ancillary, _, _ = frame_connection.recvmsg(
                            128, socket.CMSG_SPACE(4 * struct.calcsize("i"))
                        )
                        assert len(setup) == 128
                        for level, kind, value in ancillary:
                            if (level == socket.SOL_SOCKET and
                                    kind == socket.SCM_RIGHTS):
                                received_fds.frombytes(
                                    value[
                                        : len(value)
                                        - len(value) % received_fds.itemsize
                                    ]
                                )
                        magic, version, header_bytes, image_count = (
                            struct.unpack_from("<IHHI", setup)
                        )
                        setup_flags = struct.unpack_from("<I", setup, 28)[0]
                        if hardware_buffer_api is not None:
                            assert setup_flags == 2
                            received_hardware_buffers = (
                                hardware_buffer_api.receive_many(
                                    frame_connection, image_count
                                )
                            )
                    assert magic == 0x31544642
                    assert version == 1
                    assert header_bytes == 128
                    assert image_count == 3
                    assert len(received_fds) == (
                        1 if hardware_buffer_api is not None
                        else image_count + 1
                    )
                    assert len(received_hardware_buffers) == (
                        image_count if hardware_buffer_api is not None else 0
                    )
                    ring_mapping = mmap.mmap(
                        received_fds[-1], 4096, access=mmap.ACCESS_WRITE
                    )
                    ring_magic, ring_version, control_bytes, slot_count = (
                        struct.unpack_from("<IHHI", ring_mapping)
                    )
                    assert ring_magic == 0x31525742
                    assert ring_version == 1
                    assert control_bytes == 128
                    assert slot_count == image_count
                    observed_slots = []
                    for expected_sequence in range(1, expected_frames + 1):
                        deadline = time.monotonic() + 5.0
                        selected_slot = None
                        while selected_slot is None and time.monotonic() < deadline:
                            for slot in range(slot_count):
                                state = struct.unpack_from(
                                    "<I", ring_mapping, 44 + slot * 4
                                )[0]
                                sequence = struct.unpack_from(
                                    "<I", ring_mapping, 60 + slot * 4
                                )[0]
                                if state == 2 and sequence == expected_sequence:
                                    selected_slot = slot
                                    break
                            if selected_slot is None:
                                producer_status, consumer_status = (
                                    struct.unpack_from("<ii", ring_mapping, 36)
                                )
                                assert producer_status >= 0
                                assert consumer_status >= 0
                                time.sleep(0.001)
                        assert selected_slot is not None
                        observed_slots.append(selected_slot)
                        # Mirror bvb_wsi_frame_ring_release publication order:
                        # retire the sequence, release the slot, then advance the
                        # ordered consumer sequence observed by the producer.
                        struct.pack_into(
                            "<I", ring_mapping, 60 + selected_slot * 4, 0
                        )
                        struct.pack_into(
                            "<I", ring_mapping, 44 + selected_slot * 4, 0
                        )
                        struct.pack_into(
                            "<I", ring_mapping, 32, expected_sequence
                        )
                    sink_result["setup"] = setup
                    sink_result["slots"] = observed_slots
                except BaseException as error:
                    sink_result["error"] = error
                finally:
                    if ring_mapping is not None:
                        ring_mapping.close()
                    for descriptor in received_fds:
                        os.close(descriptor)
                    if hardware_buffer_api is not None:
                        hardware_buffer_api.release_many(
                            received_hardware_buffers
                        )

            diagnostic_mode = validation_mode.startswith("first-rejection-")
            diagnostic_wsi = validation_mode == "first-rejection-wsi"
            expected_frame_count = (
                4 if validation_mode == "shared-command-stream" else
                0 if validation_mode == "first-rejection-command" else 1
            )
            sink_thread = None
            if not diagnostic_mode:
                sink_thread = threading.Thread(
                    target=consume_activity_frames,
                    args=(expected_frame_count,),
                    daemon=True,
                )
                sink_thread.start()
            client_arguments = [client]
            if validation_mode == "first-rejection-command":
                client_arguments.append("command")
            elif diagnostic_wsi:
                client_arguments.append("wsi")
            completed = subprocess.run(
                client_arguments,
                check=False,
                capture_output=True,
                text=True,
                timeout=5.0,
                env=environment,
            )
            if sink_thread is not None:
                sink_thread.join(timeout=5.0)
                assert not sink_thread.is_alive()
            if completed.returncode != 0:
                _, service_stderr = server.communicate(timeout=5.0)
                raise AssertionError(
                    f"{completed.stderr}service stderr: {service_stderr}"
                )
            if sink_thread is not None:
                if "error" in sink_result:
                    raise sink_result["error"]
                assert len(sink_result["setup"]) == 128
                assert sink_result["slots"] == (
                    [0, 1, 2, 0]
                    if validation_mode == "shared-command-stream"
                    else [] if validation_mode == "first-rejection-command"
                    else [0]
                )
            if validation_mode.startswith("first-rejection-"):
                records = [
                    line for line in completed.stderr.splitlines()
                    if line.startswith("BVB_FIRST_REJECTION ")
                ]
                assert len(records) == 1, completed.stderr
                assert len(records[0].encode()) + 1 <= os.pathconf(
                    ".", "PC_PIPE_BUF"
                )
                fields = dict(
                    item.split("=", 1) for item in records[0].split()[1:]
                )
                assert fields["schema"] == "1"
                if validation_mode == "first-rejection-command":
                    assert completed.stdout.startswith(
                        "PASS: E079a real command poison command_buffer="
                    )
                    assert fields["category"] == "command_poison"
                    assert fields["entry"] == "vkCmdDispatch"
                    assert fields["canonical"] == "vkCmdDispatch"
                    assert fields["scope"] == "device"
                    assert fields["reason"] == "diagnostic_stub_invoked"
                    assert fields["result"] == str(-8)
                    assert fields["command_poisons"] == "1"
                    assert fields["command_end_failures"] == "1"
                    assert int(fields["command_buffer"]) >> 56 == 11
                    assert int(fields["command_sequence"]) > 0
                    assert fields["end_poison"] == "1"
                else:
                    assert completed.stdout == (
                        "PASS: E079a protected WSI negative VkResult recorded\n"
                    )
                    assert fields["category"] == "implemented_rejection"
                    assert fields["entry"] == "vkCreateXlibSurfaceKHR"
                    assert fields["canonical"] == "vkCreateXlibSurfaceKHR"
                    assert fields["scope"] == "instance"
                    assert fields["reason"] == "negative_vkresult"
                    assert fields["result"] == str(-3)
                    assert fields["argc"] == "4"
                    assert fields["pointer_mask"] == "0x0000000000000008"
                    assert fields["implemented_rejections"] == "1"
                    assert fields["end_poison"] == "0"
                server_stdout, server_stderr = server.communicate(timeout=5.0)
                assert server.returncode == 0, server_stderr
                assert server_stdout.splitlines() == [
                    "bvb-bridge-service: activity_event=1 sequence=1 "
                    "pid=12345 width=0 height=0",
                    "bvb-bridge-service: activity_event=2 sequence=2 "
                    "pid=12345 width=0 height=0",
                    "bvb-bridge-service: activity_event=3 sequence=3 "
                    "pid=12345 width=0 height=0",
                    "bvb-bridge-service: activity_event=7 sequence=4 "
                    "pid=12345 width=2800 height=1752",
                    "bvb-bridge-service: activity_event=11 sequence=5 "
                    "pid=12345 width=2800 height=1752",
                    "bvb-bridge-service: activity_event=9 sequence=6 "
                    "pid=12345 width=0 height=0",
                ]
                assert server_stderr == ""
                assert not socket_path.exists()
                print(f"PASS: E079a global validation mode={validation_mode}")
                return 0
            assert completed.stderr == ""
            if validation_mode == "shared-memory-unmap-lost-ack":
                assert completed.stdout == (
                    "PASS: E077 unmap lost-ack local_release=1 "
                    "connection_poisoned=1 poison_retry_rtts=0 "
                    "unmap_opcode=109\n"
                )
                print(f"PASS: E077 global validation mode={validation_mode}")
                return 0
            assert completed.stdout.startswith("PASS: global Vulkan discovery")
            expected_client_mode = (
                "shared-command-stream" if shared_command_stream
                else "strict-fake" if validation_mode == "loader-tls-lifetime"
                else validation_mode
            )
            assert f"validation_mode={expected_client_mode}" in completed.stdout
            assert "exposed_extensions=7 exposed_layers=0" in completed.stdout
            assert "sampler_anisotropy=1" in completed.stdout
            assert "empty_submit=0 queue_wait=0 device_wait=0" in completed.stdout
            assert "command_submit=0 pool_reset=0" in completed.stdout
            assert (
                "concurrent_streams=2 concurrent_commands=512"
                if validation_mode == "shared-command-stream-concurrency"
                else "concurrent_streams=0 concurrent_commands=0"
            ) in completed.stdout
            assert (
                "concurrent_registry_reads=2 collision_registry_reads=4 "
                "rerecord_registry_reads=1 pool_reset_registry_reads=1 "
                "stale_resource_rejected=1 stale_native_replay_blocked=1"
                if validation_mode == "shared-command-stream-concurrency"
                else "concurrent_registry_reads=0 collision_registry_reads=0 "
                     "rerecord_registry_reads=0 pool_reset_registry_reads=0 "
                     "stale_resource_rejected=0 stale_native_replay_blocked=0"
            ) in completed.stdout
            assert (
                "recording_rtts=0" if shared_command_stream
                else "recording_rtts=41"
            ) in completed.stdout
            assert (
                "animated_frames=4 animated_reused_image=1 "
                "animated_recording_rtts=0"
                if validation_mode == "shared-command-stream"
                else "animated_frames=0 animated_reused_image=0 "
                     "animated_recording_rtts=0"
            ) in completed.stdout
            if validation_mode == "shared-command-stream":
                for sequence, color, slot in (
                    (1, "red", 0),
                    (2, "green", 1),
                    (3, "blue", 2),
                    (4, "white", 0),
                ):
                    assert (
                        f"E076_FRAME_EXPECTED sequence={sequence} "
                        f"color={color} slot={slot}"
                    ) in completed.stdout
            image_match = re.search(r"\bimage=(\d+)\b", completed.stdout)
            assert image_match is not None
            assert int(image_match.group(1)) >> 56 == 7
            assert "image_bytes=16384" in completed.stdout
            assert "mapped_bytes=4096 mapped_mismatches=0" in completed.stdout
            assert (
                "memory_rtts=1,1,1,1,1"
                if validation_mode in (
                    "shared-mapped-memory", "shared-noncoherent-memory"
                )
                else "memory_rtts=2,2,2,2,3"
                if validation_mode == "strict-mapped-memory"
                else "memory_rtts=2,2,2,2,1"
            ) in completed.stdout
            assert (
                "memory_opcodes=106,107,108,109,47"
                if validation_mode in (
                    "shared-mapped-memory", "shared-noncoherent-memory"
                )
                else "memory_opcodes=49,48,49,48,47"
                if validation_mode == "strict-mapped-memory"
                else "memory_opcodes=49,48,49,48,105"
                if validation_mode in (
                    "shared-command-stream",
                    "shared-command-stream-concurrency",
                    "shared-command-stream-non-success",
                )
                else "memory_opcodes=49,48,49,48,47"
            ) in completed.stdout
            assert (
                "ineligible_memory_rtts=2,2 "
                "ineligible_memory_opcodes=49,48"
                if validation_mode in (
                    "shared-mapped-memory", "shared-noncoherent-memory"
                )
                else "ineligible_memory_rtts=0,0 "
                     "ineligible_memory_opcodes=0,0"
            ) in completed.stdout
            assert "fill_words=1024 mismatches=0" in completed.stdout
            assert "fence_before=1 fenced_submit=0 fence_after=0" in completed.stdout
            assert "fence_wait=0 fence_reset=0 fence_after_reset=1" in completed.stdout
            if validation_mode != "hardware":
                assert f"api={0x00400000 | (4 << 12) | 354}" in completed.stdout
                assert 'device=BVB Fake Adreno 730 "quoted"' in completed.stdout
                assert f"device_api={0x00400000 | (3 << 12) | 275}" in completed.stdout
                assert "driver=16909060 vendor=20803 device_id=1840" in completed.stdout
                assert "max_push_constants=256 image_format_max=4096,2048" in completed.stdout
                assert "queues=2 memory_types=1 memory_heaps=2 device_extensions=8" in completed.stdout
                assert f"logical_device={0x0300000000000001}" in completed.stdout
                assert f"queue={0x0400000000000001}" in completed.stdout
                command_serial = (
                    2 if validation_mode == "shared-command-stream" else 1
                )
                assert (
                    f"command_pool={0x0A00000000000000 | command_serial}"
                    in completed.stdout
                )
                assert (
                    f"command_buffer={0x0B00000000000000 | command_serial}"
                    in completed.stdout
                )
                assert f"buffer={0x1300000000000001}" in completed.stdout
                assert f"memory={0x0900000000000001}" in completed.stdout
                assert "buffer_requirements2=4096,256,1" in completed.stdout
                assert f"buffer_address={0x123456780000}" in completed.stdout
                assert f"image_view={0x0800000000000002}" in completed.stdout
                assert "image_allocation_bytes=19623936" in completed.stdout
                assert "image_dedicated=1,1" in completed.stdout
                assert f"fence={0x1200000000000001}" in completed.stdout
            else:
                assert "max_push_constants=512 image_format_max=16384,8192" in completed.stdout
                assert "buffer_requirements2=4096,4,1" in completed.stdout
                assert f"buffer_address={0xabcdef010000}" in completed.stdout
                assert "image_allocation_bytes=16384" in completed.stdout
                assert "image_dedicated=0,0" in completed.stdout
                for field, object_type in (
                    ("instance_one", 1),
                    ("instance_two", 1),
                    ("physical_device", 2),
                    ("logical_device", 3),
                    ("queue", 4),
                    ("image_view", 8),
                    ("memory", 9),
                    ("command_pool", 10),
                    ("command_buffer", 11),
                    ("fence", 18),
                    ("buffer", 19),
                ):
                    field_match = re.search(
                        rf"\b{field}=(\d+)\b", completed.stdout
                    )
                    assert field_match is not None
                    assert int(field_match.group(1)) >> 56 == object_type
            if validation_mode == "loader-tls-lifetime":
                time.sleep(0.25)
                assert server.poll() is None, server.communicate(timeout=1.0)
                server.terminate()
                server_stdout, server_stderr = server.communicate(timeout=5.0)
                assert server.returncode == -15, server_stderr
            else:
                server_stdout, server_stderr = server.communicate(timeout=5.0)
                assert server.returncode == 0, server_stderr
            assert server_stdout.splitlines() == [
                "bvb-bridge-service: activity_event=1 sequence=1 "
                "pid=12345 width=0 height=0",
                "bvb-bridge-service: activity_event=2 sequence=2 "
                "pid=12345 width=0 height=0",
                "bvb-bridge-service: activity_event=3 sequence=3 "
                "pid=12345 width=0 height=0",
                "bvb-bridge-service: activity_event=7 sequence=4 "
                "pid=12345 width=2800 height=1752",
                "bvb-bridge-service: activity_event=11 sequence=5 "
                "pid=12345 width=2800 height=1752",
                "bvb-bridge-service: activity_event=9 sequence=6 "
                "pid=12345 width=0 height=0",
            ]
            expected_server_stderr = (
                "bvb: queue submit2 failed: invalid or cross-device shared "
                "command stream\n"
                if validation_mode == "shared-command-stream-concurrency"
                else ""
            )
            if validation_mode == "loader-tls-lifetime":
                assert server_stderr == "" or server_stderr.endswith(
                    "failed: Connection reset by peer\n"
                )
            else:
                assert server_stderr == expected_server_stderr
                assert not socket_path.exists()
        finally:
            activity_frame_listener.close()
            if server.poll() is None:
                server.terminate()
                server.wait(timeout=5.0)
    print(f"PASS: E070 global validation mode={validation_mode}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
