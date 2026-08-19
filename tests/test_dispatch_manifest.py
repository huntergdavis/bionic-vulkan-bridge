#!/usr/bin/env python3

import json
import pathlib
import subprocess
import sys
import tempfile


def main() -> None:
    analyzer = pathlib.Path(sys.argv[1]).resolve()
    registry = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory() as directory:
        trace = pathlib.Path(directory) / "trace.tsv"
        trace.write_text(
            "1\t100\t101\t1\tI\t1\tvkCreateInstance\n"
            "1\t100\t101\t2\tI\t1\tvkDestroySurfaceKHR\n"
            "1\t100\t102\t3\tD\t1\tvkCreateBuffer\n"
            "1\t100\t102\t4\tD\t0\twine_vkAcquireKeyedMutex\n"
        )
        result = subprocess.run(
            [sys.executable, analyzer, trace, registry],
            check=True,
            text=True,
            capture_output=True,
        )
        document = json.loads(result.stdout)
        assert document["summary"] == {
            "dispatch_scope_counts": {
                "device": 1,
                "global": 1,
                "instance": 1,
                "private": 1,
            },
            "record_count": 4,
            "resolved_name_count": 3,
            "thread_count": 2,
            "unique_name_count": 4,
        }
        assert document["processes"] == [
            {
                "pid": 100,
                "record_count": 4,
                "resolved_name_count": 3,
                "thread_ids": [101, 102],
                "unique_name_count": 4,
            }
        ]
        commands = {entry["name"]: entry for entry in document["commands"]}
        assert commands["vkCreateInstance"]["dispatch_scope"] == "global"
        assert commands["vkDestroySurfaceKHR"]["dispatch_scope"] == "instance"
        assert commands["vkCreateBuffer"]["dispatch_scope"] == "device"
        assert commands["wine_vkAcquireKeyedMutex"]["dispatch_scope"] == "private"
        assert commands["vkCreateBuffer"]["first_parameter_type"] == "VkDevice"
        assert commands["vkCreateBuffer"]["resolved_stages"] == ["D"]

    print("PASS: Vulkan dispatch manifest")


if __name__ == "__main__":
    main()
