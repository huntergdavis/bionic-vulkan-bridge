#!/usr/bin/env python3

import os
import pathlib
import stat
import subprocess
import sys
import tempfile


def main() -> None:
    tracer = str(pathlib.Path(sys.argv[1]).resolve())
    fake_vulkan = str(pathlib.Path(sys.argv[2]).resolve())
    consumer = str(pathlib.Path(sys.argv[3]).resolve())
    with tempfile.TemporaryDirectory() as directory:
        trace = pathlib.Path(directory) / "resolve.tsv"
        environment = os.environ.copy()
        environment["BVB_VULKAN_TRACE_FILE"] = str(trace)
        environment["LD_PRELOAD"] = ":".join(
            value for value in (tracer, environment.get("LD_PRELOAD")) if value
        )
        result = subprocess.run(
            [consumer, fake_vulkan],
            env=environment,
            check=True,
            text=True,
            capture_output=True,
        )
        assert result.stdout == "PASS: Vulkan resolution passthrough\n"
        assert result.stderr == ""
        assert stat.S_IMODE(trace.stat().st_mode) == 0o600

        records = [tuple(line.split("\t", 2)) for line in trace.read_text().splitlines()]
        required = {
            ("L", "1", "vkGetInstanceProcAddr"),
            ("I", "1", "vkCreateInstance"),
            ("I", "0", "vkDefinitelyMissing"),
            ("I", "1", "vkGetInstanceProcAddr"),
            ("I", "1", "vkGetDeviceProcAddr"),
            ("D", "1", "vkCreateBuffer"),
            ("D", "0", "vkDefinitelyMissing"),
            ("D", "1", "vkGetDeviceProcAddr"),
            ("L", "1", "vkGetDeviceProcAddr"),
        }
        assert required.issubset(set(records)), (required - set(records), records)
        assert all(len(record) == 3 for record in records)

    print("PASS: Vulkan entry-point resolution trace")


if __name__ == "__main__":
    main()
