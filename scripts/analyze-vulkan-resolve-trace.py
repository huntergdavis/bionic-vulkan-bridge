#!/usr/bin/env python3

"""Turn a Vulkan resolution trace into deterministic dispatch metadata."""

from __future__ import annotations

import argparse
import hashlib
import json
from collections import Counter, defaultdict
from pathlib import Path
import xml.etree.ElementTree as ET


VALID_STAGES = {"L", "I", "D"}


def parse_trace(path: Path) -> tuple[list[dict[str, object]], set[str]]:
    records: list[dict[str, object]] = []
    formats: set[str] = set()
    for line_number, line in enumerate(path.read_text().splitlines(), 1):
        fields = line.split("\t")
        if len(fields) == 3:
            stage, resolved, name = fields
            record: dict[str, object] = {
                "pid": None,
                "tid": None,
                "sequence": None,
                "stage": stage,
                "resolved": resolved,
                "name": name,
            }
            trace_format = "legacy"
        elif len(fields) == 7 and fields[0] == "1":
            _, pid, tid, sequence, stage, resolved, name = fields
            try:
                record = {
                    "pid": int(pid),
                    "tid": int(tid),
                    "sequence": int(sequence),
                    "stage": stage,
                    "resolved": resolved,
                    "name": name,
                }
            except ValueError as error:
                raise ValueError(f"{path}:{line_number}: invalid integer") from error
            if min(record["pid"], record["tid"], record["sequence"]) <= 0:
                raise ValueError(f"{path}:{line_number}: non-positive identity")
            trace_format = "1"
        else:
            raise ValueError(f"{path}:{line_number}: invalid trace record")
        if stage not in VALID_STAGES or resolved not in {"0", "1"} or not name:
            raise ValueError(f"{path}:{line_number}: invalid trace values")
        record["resolved"] = resolved == "1"
        records.append(record)
        formats.add(trace_format)
    if not records:
        raise ValueError(f"{path}: empty trace")
    return records, formats


def registry_commands(registry: Path) -> dict[str, dict[str, object]]:
    root = ET.parse(registry).getroot()
    handles: dict[str, dict[str, str | bool | None]] = {}
    for node in root.findall("./types/type[@category='handle']"):
        name_node = node.find("name")
        if name_node is None or not name_node.text:
            continue
        handles[name_node.text] = {
            "parent": node.get("parent"),
            "dispatchable": "VK_DEFINE_NON_DISPATCHABLE_HANDLE"
            not in "".join(node.itertext()),
        }

    commands: dict[str, dict[str, object]] = {}
    aliases: list[tuple[str, str]] = []
    for node in root.findall("./commands/command"):
        alias = node.get("alias")
        if alias:
            name = node.get("name")
            if name:
                aliases.append((name, alias))
            continue
        name_node = node.find("./proto/name")
        type_node = node.find("./proto/type")
        if name_node is None or not name_node.text:
            continue
        parameters = node.findall("param")
        first_type_node = parameters[0].find("type") if parameters else None
        first_type = (
            first_type_node.text
            if first_type_node is not None and first_type_node.text
            else None
        )
        scope = "global"
        cursor = first_type
        visited: set[str] = set()
        while cursor in handles and cursor not in visited:
            visited.add(cursor)
            if cursor in {"VkInstance", "VkPhysicalDevice"}:
                scope = "instance"
                break
            if cursor == "VkDevice":
                scope = "device"
                break
            parent = handles[cursor]["parent"]
            cursor = parent.split(",", 1)[0] if isinstance(parent, str) else None
        commands[name_node.text] = {
            "canonical_name": name_node.text,
            "return_type": type_node.text if type_node is not None else None,
            "first_parameter_type": first_type,
            "dispatch_scope": scope,
        }

    unresolved = aliases
    while unresolved:
        remaining: list[tuple[str, str]] = []
        changed = False
        for name, alias in unresolved:
            if alias not in commands:
                remaining.append((name, alias))
                continue
            commands[name] = {**commands[alias], "canonical_name": alias}
            changed = True
        if not changed:
            break
        unresolved = remaining
    return commands


def analyze(trace: Path, registry: Path) -> dict[str, object]:
    raw = trace.read_bytes()
    records, formats = parse_trace(trace)
    commands = registry_commands(registry)
    grouped: dict[str, list[dict[str, object]]] = defaultdict(list)
    for record in records:
        grouped[str(record["name"])].append(record)

    entries = []
    scope_counts: Counter[str] = Counter()
    for name in sorted(grouped):
        name_records = grouped[name]
        metadata = commands.get(name)
        if metadata is None:
            metadata = {
                "canonical_name": None,
                "return_type": None,
                "first_parameter_type": None,
                "dispatch_scope": "private" if name.startswith("wine_vk") else "unknown",
            }
        scope = str(metadata["dispatch_scope"])
        scope_counts[scope] += 1
        stage_counts: Counter[str] = Counter(
            str(record["stage"]) for record in name_records
        )
        resolved_stages = sorted(
            {
                str(record["stage"])
                for record in name_records
                if bool(record["resolved"])
            }
        )
        entries.append(
            {
                "name": name,
                **metadata,
                "lookup_count": len(name_records),
                "stage_counts": dict(sorted(stage_counts.items())),
                "resolved_stages": resolved_stages,
            }
        )

    process_ids = sorted(
        {int(record["pid"]) for record in records if record["pid"] is not None}
    )
    thread_ids = sorted(
        {int(record["tid"]) for record in records if record["tid"] is not None}
    )
    resolved_names = {
        str(record["name"]) for record in records if bool(record["resolved"])
    }
    return {
        "schema_version": 1,
        "source": {
            "name": trace.name,
            "bytes": len(raw),
            "sha256": hashlib.sha256(raw).hexdigest(),
            "trace_formats": sorted(formats),
        },
        "summary": {
            "record_count": len(records),
            "unique_name_count": len(grouped),
            "resolved_name_count": len(resolved_names),
            "process_ids": process_ids,
            "thread_count": len(thread_ids),
            "dispatch_scope_counts": dict(sorted(scope_counts.items())),
        },
        "commands": entries,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("registry", type=Path)
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    document = analyze(arguments.trace, arguments.registry)
    rendered = json.dumps(document, indent=2, sort_keys=True) + "\n"
    if arguments.output:
        arguments.output.write_text(rendered)
    else:
        print(rendered, end="")


if __name__ == "__main__":
    main()
