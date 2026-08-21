#!/usr/bin/env python3

"""Validate E079 generated coverage and compact evidence provenance."""

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
    if len(sys.argv) != 12:
        raise SystemExit(
            "usage: test_first_rejection_architecture.py "
            "<evidence> <generator> <generated> <vk.xml> <manifest> "
            "<triangle.inc> <config> <first.c> <global.c> <triangle.c> "
            "<glibc-builder>"
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

    first_source = first_source_path.read_text()
    require(first_source, 'getenv("BVB_FIRST_REJECTION_DIAGNOSTIC")', "first source")
    require(first_source, 'strcmp(value, "1") == 0', "first source")
    require(first_source, "if (!bvb_first_state.snapshot.emitted)", "first source")
    require(first_source, '"BVB_FIRST_REJECTION schema=1', "first source")
    require(first_source, "return raw;", "disabled resolver behavior")

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

    if evidence["claims"]["resolver_order_is_evidence"]:
        raise AssertionError("E079 evidence must not promote resolver order")
    if evidence["claims"]["next_real_entry_known"]:
        raise AssertionError("host-only E079 cannot claim the real next entry")
    if evidence["verification"]["full_host_suite"] != "55/55 contracts passed":
        raise AssertionError("full-suite evidence count is stale")
    print("PASS: E079 generated diagnostic coverage and evidence provenance")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
