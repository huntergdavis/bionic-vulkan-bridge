#!/usr/bin/env python3

"""Validate E079a fail-closed diagnostic architecture and provenance."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import sys


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(text: str, needle: str, context: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing {needle!r} in {context}")


def main() -> int:
    if len(sys.argv) != 16:
        raise SystemExit(
            "usage: test_first_rejection_architecture.py "
            "<evidence> <generator> <generated> <vk.xml> <manifest> "
            "<triangle.inc> <config> <first.c> <global.c> <triangle.c> "
            "<glibc-builder> <first-test.c> <first-test.py> "
            "<global-test.c> <global-test.py>"
        )
    (
        evidence_path,
        generator_path,
        generated_path,
        registry_path,
        manifest_path,
        triangle_include_path,
        config_path,
        first_source_path,
        global_source_path,
        triangle_source_path,
        glibc_builder_path,
        first_test_path,
        first_test_driver_path,
        global_test_path,
        global_test_driver_path,
    ) = map(Path, sys.argv[1:])
    evidence = json.loads(evidence_path.read_text())
    generation = evidence["generation"]
    expected_hashes = {
        generator_path: generation["generator_sha256"],
        generated_path: generation["generated_include_sha256"],
        registry_path: generation["vk_xml_sha256"],
        manifest_path: generation["e011_manifest_sha256"],
        triangle_include_path: generation["triangle_dispatch_sha256"],
        config_path: generation["active_config_sha256"],
    }
    for path, expected in expected_hashes.items():
        actual = digest(path)
        if actual != expected:
            raise AssertionError(f"SHA-256 mismatch for {path}: {actual} != {expected}")

    generated = generated_path.read_text()
    marker = (
        f"entry_count={generation['generated_entry_count']} "
        f"protected_registry={generation['platform_protected_registry_count']} "
        "resolved_protected_skipped="
        f"{generation['resolved_platform_protected_skipped']}"
    )
    require(generated, marker, "generated dispatch")
    proxy_definitions = re.findall(
        r"^bvb_first_proxy_(vk[A-Za-z0-9_]+)\(", generated, re.MULTILINE
    )
    if len(proxy_definitions) != generation["generated_entry_count"]:
        raise AssertionError(
            f"proxy coverage {len(proxy_definitions)} != "
            f"{generation['generated_entry_count']}"
        )
    if len(set(proxy_definitions)) != len(proxy_definitions):
        raise AssertionError("generated proxy names are not unique")
    require(generated, "bvb_first_stub_vkCmdDispatch(", "generated dispatch")
    require(generated, '"diagnostic_stub_invoked"', "generated dispatch")
    require(generated, '"negative_vkresult"', "generated dispatch")
    require(generated, "PFN erasure width mismatch:", "generated dispatch")
    command_stub = generated.split("bvb_first_stub_vkCmdDispatch(", 1)[1].split(
        "static _Atomic", 1
    )[0]
    require(
        command_stub,
        "bvb_global_diagnostic_poison_command(",
        "required command stub",
    )
    if "bvb_first_rejection_record_stub(" in command_stub:
        raise AssertionError("command stub records before End owns poison context")
    noncommand_void = generated.split(
        "bvb_first_stub_vkGetDeviceQueue2(", 1
    )[1].split("static _Atomic", 1)[0]
    require(
        noncommand_void,
        "_Exit(BVB_FIRST_REJECTION_VOID_EXIT_STATUS);",
        "required non-command void stub",
    )

    first_source = first_source_path.read_text()
    require(first_source, 'getenv("BVB_FIRST_REJECTION_DIAGNOSTIC")', "first source")
    require(first_source, 'strcmp(value, "1") == 0', "first source")
    require(first_source, '"BVB_FIRST_REJECTION schema=1', "first source")
    require(first_source, "return raw;", "disabled resolver behavior")
    require(first_source, "BVB_FIRST_WINNER_EMITTING", "winner state")
    require(first_source, "const struct bvb_first_rejection_snapshot winner",
            "immutable winner")
    require(first_source, "write(STDERR_FILENO", "single record syscall")
    require(first_source, "BVB_FIRST_REJECTION_RECORD_MAX <= PIPE_BUF",
            "atomic pipe bound")
    require(first_source, "BVB_FIRST_REJECTION_FIXED_FIELDS_MAX",
            "worst-case record-content bound")
    require(first_source, "bvb_first_proxy_allocate_descriptor_sets",
            "retry-aware descriptor diagnostic")
    require(first_source, "negative_vkresult_after_retry",
            "consecutive descriptor failure diagnostic")
    for forbidden in ("fprintf(stderr", "flockfile(", "fflush("):
        if forbidden in first_source:
            raise AssertionError(f"forbidden output primitive in diagnostic: {forbidden}")

    global_source = global_source_path.read_text()
    require(global_source, "bvb_first_rejection_wrap(", "global resolver")
    require(
        global_source,
        "bvb_first_rejection_required_stub(",
        "global required-null resolver",
    )
    require(
        global_source,
        "bvb_first_rejection_record_command_poison(",
        "global End poison",
    )
    require(
        global_source,
        "create_virtual_surface_diagnostic(",
        "protected surface rejection",
    )
    require(
        global_source,
        '"vkCreateXlibSurfaceKHR"',
        "protected Xlib rejection",
    )
    triangle_source = triangle_source_path.read_text()
    require(
        triangle_source,
        "bvb_first_rejection_record_command_poison(",
        "triangle finish poison",
    )

    glibc_builder = glibc_builder_path.read_text()
    require(
        glibc_builder,
        'scripts/generate-first-rejection-dispatch.py"',
        "Termux glibc ICD builder",
    )
    require(
        glibc_builder,
        '"$generated_dir/bvb_first_rejection_dispatch.inc"',
        "Termux glibc ICD builder",
    )
    require(
        glibc_builder,
        '"$project_dir/src/first_rejection.c"',
        "Termux glibc ICD builder",
    )

    first_test = first_test_path.read_text()
    require(first_test, "pthread_barrier_wait", "two-thread winner test")
    require(first_test, "vkGetDeviceQueue2", "non-command void exit test")
    require(first_test, "check_retryable_descriptor_rejection",
            "descriptor retry contract")
    first_test_driver = first_test_driver_path.read_text()
    require(first_test_driver, '"trace=write"', "one-write strace test")
    require(first_test_driver, 'void_exit.returncode != 86',
            "fail-closed exit test")
    global_test = global_test_path.read_text()
    require(global_test, "dispatch(command_buffer, 1U, 1U, 1U);",
            "real non-null command poison")
    require(global_test, "vkCreateXlibSurfaceKHR", "protected WSI negative test")
    global_test_driver = global_test_driver_path.read_text()
    require(global_test_driver, 'fields["command_sequence"]',
            "End context assertion")
    require(global_test_driver, 'fields["pointer_mask"] == "0x0000000000000008"',
            "protected WSI pointer-presence assertion")

    if evidence["gate"] != "E079a":
        raise AssertionError("architecture contract requires E079a evidence")
    if evidence["supersedes"] != {
        "gate": "E079",
        "deployment_status": "must_not_deploy",
        "reasons": [
            "required non-command void stubs could return after an unsupported call",
            "generated command stubs could emit before End supplied real command-buffer poison context",
        ],
    }:
        raise AssertionError("E079a evidence must supersede unsafe E079 deployment")
    if evidence["claims"]["resolver_order_is_evidence"]:
        raise AssertionError("E079a evidence must not promote resolver order")
    if evidence["claims"]["next_real_entry_known"]:
        raise AssertionError("host-only E079 cannot claim the real next entry")
    if evidence["verification"]["full_host_suite"] != "74/74 contracts passed":
        raise AssertionError("full-suite evidence count is stale")
    print("PASS: E079a fail-closed atomic diagnostic architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
