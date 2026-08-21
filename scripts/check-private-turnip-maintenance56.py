#!/usr/bin/env python3
"""Validate a BVB private Turnip maintenance5/6 build without executing it."""

import argparse
import pathlib
import re
import subprocess
import sys


def fail(message: str) -> None:
    raise SystemExit(f"error: {message}")


def read(path: pathlib.Path) -> str:
    try:
        return path.read_text()
    except OSError as error:
        fail(f"could not read {path}: {error}")


def command(*arguments: str) -> str:
    try:
        return subprocess.run(
            arguments,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError) as error:
        fail(f"command failed ({' '.join(arguments)}): {error}")


def require(pattern: str, value: str, description: str) -> None:
    if re.search(pattern, value, re.MULTILINE) is None:
        fail(description)


def check_elf(path: pathlib.Path, needs_resolver: bool) -> None:
    header = command("readelf", "-h", str(path))
    require(r"Machine:\s+AArch64", header, f"{path} is not AArch64 ELF")
    dynamic = command("readelf", "-d", str(path))
    for forbidden in ("libc.so.6", "libstdc++.so.6", "libgcc_s.so.1"):
        if forbidden in dynamic:
            fail(f"{path} has a glibc/Linux dependency: {forbidden}")
    if needs_resolver:
        symbols = command("readelf", "-Ws", str(path))
        require(
            r"GLOBAL\s+DEFAULT\s+\d+\s+vk_icdGetInstanceProcAddr(?:\s|$)",
            symbols,
            "private ICD resolver is not a global exported symbol",
        )
        strings = command("strings", str(path))
        for extension in ("VK_KHR_maintenance5", "VK_KHR_maintenance6"):
            if extension not in strings:
                fail(f"built driver does not contain {extension}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=pathlib.Path)
    parser.add_argument("--generated", required=True, type=pathlib.Path)
    parser.add_argument("--driver", required=True, type=pathlib.Path)
    parser.add_argument("--probe", required=True, type=pathlib.Path)
    arguments = parser.parse_args()

    extension_source = read(
        arguments.source / "src/vulkan/util/vk_extensions.py"
    )
    require(
        r'^\s*"VK_KHR_maintenance5": 34,$',
        extension_source,
        "maintenance5 is not narrowly enabled at API 34",
    )
    require(
        r'^\s*"VK_KHR_maintenance6": 34,$',
        extension_source,
        "maintenance6 is not narrowly enabled at API 34",
    )
    require(
        r'^\s*"VK_KHR_maintenance7": 36,$',
        extension_source,
        "maintenance7 policy changed unexpectedly",
    )

    turnip_source = read(
        arguments.source / "src/freedreno/vulkan/tu_device.cc"
    )
    for expected in (
        ".KHR_maintenance5 = tu_is_vk_1_1(device),",
        ".KHR_maintenance6 = tu_is_vk_1_1(device),",
        "features->maintenance5 = true;",
        "features->maintenance6 = true;",
    ):
        if expected not in turnip_source:
            fail(f"Turnip support contract is missing: {expected}")

    generated = read(arguments.generated)
    for extension in ("maintenance5", "maintenance6"):
        require(
            rf"^\s*\.KHR_{extension} = ANDROID_API_LEVEL >= 34,$",
            generated,
            f"generated Android allowlist does not expose KHR_{extension} at API 34",
        )
    for extension, api in (("maintenance4", 33), ("maintenance7", 36)):
        require(
            rf"^\s*\.KHR_{extension} = ANDROID_API_LEVEL >= {api},$",
            generated,
            f"adjacent KHR_{extension} policy changed unexpectedly",
        )

    check_elf(arguments.driver, needs_resolver=True)
    check_elf(arguments.probe, needs_resolver=False)
    print(
        "PASS: API34 private Turnip build exposes only the maintenance5/6 "
        "strict-policy exceptions; AArch64 artifact contracts pass"
    )


if __name__ == "__main__":
    main()
