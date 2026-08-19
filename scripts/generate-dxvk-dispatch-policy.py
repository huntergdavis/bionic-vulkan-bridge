#!/usr/bin/env python3

"""Generate the measured DXVK dispatch-availability policy."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET


ENTRY_PATTERN = re.compile(
    r"^BVB_TRIANGLE_DISPATCH_ENTRY\((vk[A-Za-z0-9_]+),"
)
SCOPE_CONSTANTS = {
    "global": "BVB_DXVK_SCOPE_GLOBAL",
    "instance": "BVB_DXVK_SCOPE_INSTANCE",
    "device": "BVB_DXVK_SCOPE_DEVICE",
    "private": "BVB_DXVK_SCOPE_PRIVATE",
}
SUPPORT_CONSTANTS = {
    "probed_null": "BVB_DXVK_SUPPORT_PROBED_NULL",
    "required_unimplemented": "BVB_DXVK_SUPPORT_REQUIRED_UNIMPLEMENTED",
    "executable": "BVB_DXVK_SUPPORT_EXECUTABLE",
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def registry_canonical_names(registry: Path) -> dict[str, str]:
    root = ET.parse(registry).getroot()
    canonical: dict[str, str] = {}
    pending: list[tuple[str, str]] = []
    for node in root.findall("./commands/command"):
        alias = node.get("alias")
        if alias:
            name = node.get("name")
            if name:
                pending.append((name, alias))
            continue
        name = node.findtext("./proto/name")
        if name:
            canonical[name] = name
    while pending:
        next_pending = []
        changed = False
        for name, alias in pending:
            target = canonical.get(alias)
            if target is None:
                next_pending.append((name, alias))
            else:
                canonical[name] = target
                changed = True
        if not changed:
            raise ValueError(f"unresolved registry aliases: {next_pending}")
        pending = next_pending
    return canonical


def executable_names(triangle_dispatch: Path) -> set[str]:
    names = {
        match.group(1)
        for line in triangle_dispatch.read_text().splitlines()
        if (match := ENTRY_PATTERN.match(line)) is not None
    }
    if not names:
        raise ValueError("triangle dispatch contains no executable entries")
    return names


def generate(
    registry: Path, manifest: Path, triangle_dispatch: Path
) -> tuple[str, dict[str, object]]:
    registry_names = registry_canonical_names(registry)
    evidence = json.loads(manifest.read_text())
    records = evidence.get("commands")
    if not isinstance(records, list):
        raise ValueError("E011 commands must be a list")
    executable = executable_names(triangle_dispatch)
    seen: set[str] = set()
    support_counts: dict[str, int] = {
        "probed_null": 0,
        "required_unimplemented": 0,
        "executable": 0,
    }
    scope_counts: dict[str, int] = {scope: 0 for scope in SCOPE_CONSTANTS}
    entries: list[dict[str, object]] = []
    for record in records:
        name = record.get("name")
        canonical = record.get("canonical_name")
        scope = record.get("dispatch_scope")
        lookup_count = record.get("lookup_count")
        resolved_stages = record.get("resolved_stages")
        if (
            not isinstance(name, str)
            or scope not in SCOPE_CONSTANTS
            or not isinstance(lookup_count, int)
            or lookup_count <= 0
            or not isinstance(resolved_stages, list)
            or name in seen
        ):
            raise ValueError(f"invalid E011 command record: {record!r}")
        seen.add(name)
        if scope == "private":
            if not name.startswith("wine_vk") or canonical is not None:
                raise ValueError(f"invalid private command: {name}")
            canonical = name
        elif not isinstance(canonical, str):
            raise ValueError(f"missing registry identity: {name}")
        elif registry_names.get(name) != canonical:
            raise ValueError(f"registry identity mismatch: {name}")
        resolved = len(resolved_stages) != 0
        if name in executable:
            if not resolved or scope != "device":
                raise ValueError(f"executable entry lacks device proof: {name}")
            support = "executable"
        elif resolved:
            support = "required_unimplemented"
        else:
            support = "probed_null"
        support_counts[support] += 1
        scope_counts[scope] += 1
        entries.append(
            {
                "name": name,
                "canonical": canonical,
                "scope": scope,
                "lookup_count": lookup_count,
                "support": support,
            }
        )
    missing_executable = executable - seen
    if missing_executable:
        raise ValueError(
            f"triangle entries absent from E011: {sorted(missing_executable)}"
        )
    entries.sort(key=lambda entry: str(entry["name"]))
    lines = [
        "/* Generated; do not edit. */",
        f"/* vk.xml sha256: {sha256(registry)} */",
        f"/* E011 manifest sha256: {sha256(manifest)} */",
        f"/* executable dispatch sha256: {sha256(triangle_dispatch)} */",
    ]
    for entry in entries:
        lines.append(
            "BVB_DXVK_DISPATCH_POLICY_ENTRY("
            f"{json.dumps(entry['name'])}, {json.dumps(entry['canonical'])}, "
            f"{SCOPE_CONSTANTS[str(entry['scope'])]}, "
            f"{entry['lookup_count']}U, "
            f"{SUPPORT_CONSTANTS[str(entry['support'])]})"
        )
    include = "\n".join(lines) + "\n"
    document: dict[str, object] = {
        "schema_version": 1,
        "gate": "E024",
        "source": {
            "vk_xml_sha256": sha256(registry),
            "e011_manifest_sha256": sha256(manifest),
            "triangle_dispatch_sha256": sha256(triangle_dispatch),
            "generated_policy_sha256": hashlib.sha256(
                include.encode()
            ).hexdigest(),
        },
        "summary": {
            "command_count": len(entries),
            "resolved_name_count": sum(
                support_counts[key]
                for key in ("required_unimplemented", "executable")
            ),
            "executable_name_count": support_counts["executable"],
            "support_counts": support_counts,
            "dispatch_scope_counts": scope_counts,
        },
        "executable_names": sorted(executable),
    }
    return include, document


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("registry", type=Path)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("triangle_dispatch", type=Path)
    parser.add_argument("output_include", type=Path)
    parser.add_argument("output_json", type=Path)
    arguments = parser.parse_args()
    include, document = generate(
        arguments.registry, arguments.manifest, arguments.triangle_dispatch
    )
    arguments.output_include.parent.mkdir(parents=True, exist_ok=True)
    arguments.output_json.parent.mkdir(parents=True, exist_ok=True)
    arguments.output_include.write_text(include)
    arguments.output_json.write_text(json.dumps(document, indent=2) + "\n")


if __name__ == "__main__":
    main()
