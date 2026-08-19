#!/usr/bin/env python3

"""Generate the first executable Vulkan dispatch table from vk.xml + E011."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET


CANONICAL_COMMANDS = (
    "vkCmdBeginRendering",
    "vkCmdBindPipeline",
    "vkCmdSetViewport",
    "vkCmdSetScissor",
    "vkCmdDraw",
    "vkCmdEndRendering",
)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def registry_commands(registry: Path) -> dict[str, dict[str, object]]:
    root = ET.parse(registry).getroot()
    commands: dict[str, dict[str, object]] = {}
    aliases: list[tuple[str, str]] = []
    for node in root.findall("./commands/command"):
        alias = node.get("alias")
        if alias:
            name = node.get("name")
            if name:
                aliases.append((name, alias))
            continue
        name = node.findtext("./proto/name")
        if not name:
            continue
        parameters = []
        for parameter in node.findall("param"):
            parameters.append(
                {
                    "type": parameter.findtext("type"),
                    "name": parameter.findtext("name"),
                    "declaration": " ".join("".join(parameter.itertext()).split()),
                }
            )
        commands[name] = {
            "canonical": name,
            "return_type": node.findtext("./proto/type"),
            "parameters": parameters,
        }
    pending = aliases
    while pending:
        next_pending = []
        changed = False
        for name, alias in pending:
            if alias not in commands:
                next_pending.append((name, alias))
                continue
            commands[name] = {**commands[alias], "canonical": alias}
            changed = True
        if not changed:
            raise ValueError(f"unresolved registry aliases: {next_pending}")
        pending = next_pending
    return commands


def generate(registry: Path, manifest: Path) -> str:
    commands = registry_commands(registry)
    evidence = json.loads(manifest.read_text())
    observed = {entry["name"]: entry for entry in evidence["commands"]}

    entries: list[tuple[str, str]] = []
    for canonical in CANONICAL_COMMANDS:
        metadata = commands.get(canonical)
        if metadata is None:
            raise ValueError(f"registry is missing {canonical}")
        parameters = metadata["parameters"]
        if metadata["return_type"] != "void" or not parameters:
            raise ValueError(f"unexpected return/parameter shape for {canonical}")
        if parameters[0]["type"] != "VkCommandBuffer" or parameters[0]["name"] != "commandBuffer":
            raise ValueError(f"{canonical} is not command-buffer dispatched")
        entries.append((canonical, canonical))

    for name, metadata in commands.items():
        canonical = str(metadata["canonical"])
        if name != canonical and canonical in CANONICAL_COMMANDS and name in observed:
            entries.append((name, canonical))

    entries.sort()
    for name, canonical in entries:
        record = observed.get(name)
        if record is None or record.get("canonical_name") != canonical:
            raise ValueError(f"E011 does not prove registry identity for {name}")
        if record.get("dispatch_scope") != "device" or "D" not in record.get(
            "resolved_stages", []
        ):
            raise ValueError(f"E011 does not prove device resolution for {name}")

    lines = [
        "/* Generated; do not edit. */",
        f"/* vk.xml sha256: {sha256(registry)} */",
        f"/* E011 manifest sha256: {sha256(manifest)} */",
    ]
    for name, canonical in entries:
        lines.append(
            "BVB_TRIANGLE_DISPATCH_ENTRY("
            f"{name}, bvb_bridge_{canonical}, PFN_{name})"
        )
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("registry", type=Path)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("output", type=Path)
    arguments = parser.parse_args()
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(generate(arguments.registry, arguments.manifest))


if __name__ == "__main__":
    main()
