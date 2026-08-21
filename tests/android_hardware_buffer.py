#!/usr/bin/env python3

"""Tiny ctypes adapter used only by Android-hosted transport contracts."""

from __future__ import annotations

import ctypes
import errno
import pathlib
import select
import socket
import time


SYSTEM_ANDROID_LIBRARY = pathlib.Path("/system/lib64/libandroid.so")


class AndroidHardwareBufferApi:
    def __init__(self) -> None:
        self.library = ctypes.CDLL(str(SYSTEM_ANDROID_LIBRARY))
        self.receive = self.library.AHardwareBuffer_recvHandleFromUnixSocket
        self.receive.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_void_p)]
        self.receive.restype = ctypes.c_int
        self.release = self.library.AHardwareBuffer_release
        self.release.argtypes = [ctypes.c_void_p]
        self.release.restype = None

    def receive_many(
        self, connection: socket.socket, count: int
    ) -> list[ctypes.c_void_p]:
        buffers: list[ctypes.c_void_p] = []
        timeout = connection.gettimeout()
        deadline = None if timeout is None else time.monotonic() + timeout
        try:
            for _ in range(count):
                while True:
                    buffer = ctypes.c_void_p()
                    result = self.receive(
                        connection.fileno(), ctypes.byref(buffer)
                    )
                    if result == 0 and buffer.value:
                        break
                    if result not in (-errno.EAGAIN, -errno.EWOULDBLOCK):
                        raise RuntimeError(
                            "AHardwareBuffer receive failed: "
                            f"result={result}"
                        )
                    remaining = (
                        None
                        if deadline is None
                        else max(0.0, deadline - time.monotonic())
                    )
                    readable, _, _ = select.select(
                        [connection], [], [], remaining
                    )
                    if not readable:
                        raise TimeoutError(
                            "timed out receiving AHardwareBuffer handle"
                        )
                buffers.append(buffer)
        except BaseException:
            self.release_many(buffers)
            raise
        return buffers

    def release_many(self, buffers: list[ctypes.c_void_p]) -> None:
        for buffer in buffers:
            self.release(buffer)
        buffers.clear()


def load_android_hardware_buffer_api() -> AndroidHardwareBufferApi | None:
    if not SYSTEM_ANDROID_LIBRARY.is_file():
        return None
    return AndroidHardwareBufferApi()
