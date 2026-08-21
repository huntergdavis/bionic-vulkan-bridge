#!/usr/bin/env python3

"""Exercise strict-default and opt-in first-real-rejection diagnostics."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys


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
        "entry": "vkCmdDispatch",
        "canonical": "vkCmdDispatch",
        "scope": "device",
        "reason": "diagnostic_stub_invoked",
        "result": str(-8),
        "argc": "4",
        "pointer_mask": "0x0000000000000000",
        "stub_queries": "1",
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

    print("PASS: strict-default and bounded first-rejection modes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
