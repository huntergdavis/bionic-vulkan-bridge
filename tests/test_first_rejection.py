#!/usr/bin/env python3

"""Exercise strict-default and opt-in first-real-rejection diagnostics."""

from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


PREFIX = "BVB_FIRST_REJECTION "


def run(binary: Path, mode: str, selector: str | None) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    environment.pop("BVB_FIRST_REJECTION_DIAGNOSTIC", None)
    if selector is not None:
        environment["BVB_FIRST_REJECTION_DIAGNOSTIC"] = selector
    return subprocess.run(
        [str(binary), mode],
        check=False,
        capture_output=True,
        text=True,
        env=environment,
        timeout=20,
    )


def require_pass(result: subprocess.CompletedProcess[str], marker: str) -> None:
    if result.returncode != 0 or marker not in result.stdout:
        raise AssertionError(
            f"diagnostic contract failed ({result.returncode})\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )


def record(result: subprocess.CompletedProcess[str]) -> dict[str, str]:
    lines = [line for line in result.stderr.splitlines() if line.startswith(PREFIX)]
    if len(lines) != 1:
        raise AssertionError(f"expected one bounded record, got {lines!r}")
    fields: dict[str, str] = {}
    for item in lines[0][len(PREFIX):].split():
        key, value = item.split("=", 1)
        fields[key] = value
    if fields.get("schema") != "1":
        raise AssertionError(f"unexpected diagnostic schema: {fields!r}")
    return fields


def require_bounded_record(result: subprocess.CompletedProcess[str]) -> None:
    lines = [line for line in result.stderr.splitlines() if line.startswith(PREFIX)]
    if len(lines) != 1:
        raise AssertionError(f"expected one record line, got {lines!r}")
    pipe_buf = os.pathconf(".", "PC_PIPE_BUF")
    if len(lines[0].encode()) + 1 > pipe_buf:
        raise AssertionError(
            f"record exceeds PIPE_BUF: {len(lines[0].encode()) + 1} > {pipe_buf}"
        )


def require_single_stderr_write(binary: Path) -> None:
    strace = shutil.which("strace")
    if strace is None:
        raise AssertionError("strace is required for the one-write contract")
    environment = os.environ.copy()
    environment["BVB_FIRST_REJECTION_DIAGNOSTIC"] = "1"
    with tempfile.TemporaryDirectory(prefix="bvb-e079a-strace-") as temporary:
        trace = Path(temporary) / "write.trace"
        result = subprocess.run(
            [strace, "-qq", "-e", "trace=write", "-o", str(trace),
             str(binary), "required"],
            check=False,
            capture_output=True,
            text=True,
            env=environment,
            timeout=20,
        )
        require_pass(result, "PASS: first invoked required entry is reported once")
        require_bounded_record(result)
        writes = [
            line for line in trace.read_text().splitlines()
            if re.match(r"^write\(2,", line)
        ]
        if len(writes) != 1:
            raise AssertionError(f"expected exactly one write(2), got {writes!r}")
        match = re.search(r", (\d+)\)\s+=\s+(\d+)$", writes[0])
        if match is None or match.group(1) != match.group(2):
            raise AssertionError(f"partial or unparsable diagnostic write: {writes[0]}")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_first_rejection.py <test-binary>")
    binary = Path(sys.argv[1])

    for selector in (None, "0", "true", "2"):
        result = run(binary, "default", selector)
        require_pass(result, "PASS: default diagnostic preserves NULL dispatch")
        if PREFIX in result.stderr:
            raise AssertionError(
                f"default/invalid selector emitted a record: {result.stderr}"
            )

    required = run(binary, "required", "1")
    require_pass(required, "PASS: first invoked required entry is reported once")
    fields = record(required)
    expected = {
        "category": "required_unimplemented",
        "entry": "vkCreateComputePipelines",
        "canonical": "vkCreateComputePipelines",
        "scope": "device",
        "reason": "diagnostic_stub_invoked",
        "result": str(-8),
        "argc": "6",
        "pointer_mask": "0x0000000000000000",
        "stub_queries": "2",
        "stub_invocations": "1",
        "end_poison": "0",
    }
    for key, value in expected.items():
        if fields.get(key) != value:
            raise AssertionError(f"required record {key}: {fields.get(key)!r} != {value!r}")

    implemented = run(binary, "implemented", "1")
    require_pass(
        implemented, "PASS: first implemented negative VkResult is reported"
    )
    fields = record(implemented)
    expected = {
        "category": "implemented_rejection",
        "entry": "vkCreateInstance",
        "canonical": "vkCreateInstance",
        "scope": "global",
        "reason": "negative_vkresult",
        "result": str(-3),
        "argc": "3",
        "pointer_mask": "0x0000000000000005",
        "executable_invocations": "1",
        "implemented_rejections": "1",
        "end_poison": "0",
    }
    for key, value in expected.items():
        if fields.get(key) != value:
            raise AssertionError(
                f"implemented record {key}: {fields.get(key)!r} != {value!r}"
            )

    poison = run(binary, "poison", "1")
    require_pass(poison, "PASS: first void command rejection is reported at End")
    fields = record(poison)
    expected = {
        "category": "command_poison",
        "entry": "vkCmdBeginRendering",
        "canonical": "vkCmdBeginRendering",
        "scope": "device",
        "reason": "unsupported_rendering_info",
        "result": str(-95),
        "argc": "0",
        "pointer_mask": "0x0000000000000000",
        "executable_invocations": "1",
        "command_poisons": "1",
        "command_end_failures": "1",
        "command_sequence": "73",
        "end_poison": "1",
    }
    for key, value in expected.items():
        if fields.get(key) != value:
            raise AssertionError(f"poison record {key}: {fields.get(key)!r} != {value!r}")

    race = run(binary, "race", "1")
    require_pass(race, "PASS: concurrent stubs select one complete winner")
    fields = record(race)
    if fields.get("entry") not in (
        "vkCreateComputePipelines", "vkCreateRenderPass"
    ):
        raise AssertionError(f"unexpected concurrent winner: {fields!r}")
    if fields.get("stub_queries") != "2" or fields.get("stub_invocations") != "1":
        raise AssertionError(f"winner counters were not linearized: {fields!r}")

    void_exit = run(binary, "void-exit", "1")
    if void_exit.returncode != 86:
        raise AssertionError(
            f"required non-command void stub did not fail closed: "
            f"{void_exit.returncode}\n{void_exit.stderr}"
        )
    fields = record(void_exit)
    if fields.get("entry") != "vkGetDeviceQueue2" or fields.get(
        "category"
    ) != "required_unimplemented":
        raise AssertionError(f"wrong non-command void record: {fields!r}")

    for result in (required, implemented, poison, race, void_exit):
        require_bounded_record(result)
    require_single_stderr_write(binary)

    print("PASS: E079a strict-default, single-winner, one-write diagnostics")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
