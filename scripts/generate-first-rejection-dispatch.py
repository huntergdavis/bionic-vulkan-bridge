#!/usr/bin/env python3

"""Generate exact-signature, opt-in first-rejection Vulkan dispatch wrappers."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET


ENTRY_PATTERN = re.compile(
    r"^BVB_TRIANGLE_DISPATCH_ENTRY\((vk[A-Za-z0-9_]+),"
)
SCOPES = {
    "global": "BVB_DXVK_SCOPE_GLOBAL",
    "instance": "BVB_DXVK_SCOPE_INSTANCE",
    "device": "BVB_DXVK_SCOPE_DEVICE",
}


def compact_declaration(node: ET.Element) -> str:
    return " ".join("".join(node.itertext()).split())


def registry_commands(registry: Path) -> tuple[dict[str, dict[str, object]], set[str]]:
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
            parameter_apis = parameter.get("api")
            if parameter_apis is not None and "vulkan" not in parameter_apis.split(","):
                continue
            declaration = compact_declaration(parameter)
            parameters.append(
                {
                    "type": parameter.findtext("type"),
                    "name": parameter.findtext("name"),
                    "declaration": declaration,
                    "pointer": "*" in declaration or "[" in declaration
                    or str(parameter.findtext("type")).startswith("PFN_"),
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

    platform_protect = {
        node.get("name"): node.get("protect")
        for node in root.findall("./platforms/platform")
    }
    protected: set[str] = set()
    for extension in root.findall("./extensions/extension"):
        protect = extension.get("protect") or platform_protect.get(
            extension.get("platform")
        )
        if not protect:
            continue
        for command in extension.findall("./require/command"):
            name = command.get("name")
            if name:
                protected.add(name)
    return commands, protected


def executable_names(triangle_dispatch: Path, additional: Path) -> set[str]:
    names = {
        match.group(1)
        for line in triangle_dispatch.read_text().splitlines()
        if (match := ENTRY_PATTERN.match(line)) is not None
    }
    names.update(
        line.strip()
        for line in additional.read_text().splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    )
    return names


def c_string(value: str) -> str:
    return json.dumps(value)


def shape(parameters: list[dict[str, object]]) -> str:
    fields = []
    for parameter in parameters:
        suffix = "ptr" if parameter["pointer"] else "value"
        fields.append(f"{parameter['type']}_{suffix}")
    result = ",".join(fields)
    if len(result) > 384:
        raise ValueError(f"diagnostic shape exceeds bound: {result}")
    return result


def pointer_mask(parameters: list[dict[str, object]]) -> str:
    terms = [
        f"(({parameter['name']} != NULL) ? (UINT64_C(1) << {index}) : 0U)"
        for index, parameter in enumerate(parameters)
        if parameter["pointer"]
    ]
    return " | ".join(terms) if terms else "UINT64_C(0)"


def zero_return(return_type: str) -> str:
    if return_type == "void":
        return "return;"
    if return_type == "VkResult":
        return "return VK_ERROR_FEATURE_NOT_PRESENT;"
    return f"return ({return_type})0;"


def target_missing_return(return_type: str) -> str:
    if return_type == "void":
        return "return;"
    if return_type == "VkResult":
        return "return VK_ERROR_INITIALIZATION_FAILED;"
    return f"return ({return_type})0;"


def generate(
    registry: Path, manifest: Path, triangle_dispatch: Path,
    additional: Path,
) -> str:
    commands, protected = registry_commands(registry)
    executable = executable_names(triangle_dispatch, additional)
    evidence = json.loads(manifest.read_text())
    entries = []
    resolved_protected: set[str] = set()
    for record in evidence["commands"]:
        name = record.get("name")
        scope = record.get("dispatch_scope")
        if (
            isinstance(name, str)
            and record.get("resolved_stages")
            and scope in SCOPES
            and name in protected
        ):
            resolved_protected.add(name)
        if (
            not isinstance(name, str)
            or not record.get("resolved_stages")
            or scope not in SCOPES
            or name in protected
            or name not in commands
        ):
            continue
        metadata = commands[name]
        if metadata["canonical"] != record.get("canonical_name"):
            raise ValueError(f"canonical mismatch for {name}")
        parameters = metadata["parameters"]
        if len(parameters) > 64:
            raise ValueError(f"too many parameters for {name}")
        entries.append(
            {
                "name": name,
                "canonical": metadata["canonical"],
                "scope": scope,
                "return_type": metadata["return_type"],
                "parameters": parameters,
                "required": name not in executable,
            }
        )
    entries.sort(key=lambda entry: str(entry["name"]))
    if not entries:
        raise ValueError("no diagnostic dispatch entries generated")

    lines = [
        "/* Generated; do not edit. */",
        f"/* entry_count={len(entries)} protected_registry={len(protected)} "
        f"resolved_protected_skipped={len(resolved_protected)} */",
    ]
    for entry in entries:
        name = str(entry["name"])
        return_type = str(entry["return_type"])
        parameters = entry["parameters"]
        declarations = ", ".join(
            str(parameter["declaration"]) for parameter in parameters
        ) or "void"
        arguments = ", ".join(str(parameter["name"]) for parameter in parameters)
        mask = pointer_mask(parameters)
        shape_value = shape(parameters)
        lines.extend(
            [
                f"static _Atomic(PFN_{name}) bvb_first_target_{name};",
                f"static VKAPI_ATTR {return_type} VKAPI_CALL",
                f"bvb_first_proxy_{name}({declarations}) {{",
                f"    PFN_{name} target = atomic_load(&bvb_first_target_{name});",
                "    bvb_first_rejection_note_executable_invocation();",
                "    if (target == NULL) {",
                "        bvb_first_rejection_record(",
                f"            \"executable_target_missing\", {c_string(name)},",
                f"            {c_string(str(entry['canonical']))},",
                f"            {c_string(str(entry['scope']))},",
                "            \"diagnostic_proxy_target_missing\",",
                "            VK_ERROR_INITIALIZATION_FAILED,",
                f"            {len(parameters)}U, {mask}, {c_string(shape_value)},",
                "            0U, 0U, false);",
                f"        {target_missing_return(return_type)}",
                "    }",
            ]
        )
        if return_type == "void":
            lines.extend([f"    target({arguments});", "}"])
        elif return_type == "VkResult":
            lines.extend(
                [
                    f"    const VkResult result = target({arguments});",
                    "    if (result < 0)",
                    "        bvb_first_rejection_record(",
                    f"            \"implemented_rejection\", {c_string(name)},",
                    f"            {c_string(str(entry['canonical']))},",
                    f"            {c_string(str(entry['scope']))},",
                    "            \"negative_vkresult\", result,",
                    f"            {len(parameters)}U, {mask}, {c_string(shape_value)},",
                    "            0U, 0U, false);",
                    "    return result;",
                    "}",
                ]
            )
        else:
            lines.extend([f"    return target({arguments});", "}"])

        if entry["required"]:
            lines.extend(
                [
                    f"static VKAPI_ATTR {return_type} VKAPI_CALL",
                    f"bvb_first_stub_{name}({declarations}) {{",
                ]
            )
            for parameter in parameters:
                lines.append(f"    (void){parameter['name']};")
            lines.extend(
                [
                    "    bvb_first_rejection_note_stub_invocation();",
                    "    bvb_first_rejection_record(",
                    f"        \"required_unimplemented\", {c_string(name)},",
                    f"        {c_string(str(entry['canonical']))},",
                    f"        {c_string(str(entry['scope']))},",
                    "        \"diagnostic_stub_invoked\",",
                    "        VK_ERROR_FEATURE_NOT_PRESENT,",
                    f"        {len(parameters)}U, {mask}, {c_string(shape_value)},",
                    "        0U, 0U, false);",
                    f"    {zero_return(return_type)}",
                    "}",
                ]
            )

    lines.append(
        "static PFN_vkVoidFunction bvb_first_generated_wrap("
        "const char *name, enum bvb_dxvk_dispatch_scope scope, "
        "PFN_vkVoidFunction raw) {"
    )
    for entry in entries:
        name = str(entry["name"])
        lines.extend(
            [
                f"    if (scope == {SCOPES[str(entry['scope'])]} &&",
                f"        strcmp(name, {c_string(name)}) == 0) {{",
                f"        PFN_{name} typed = NULL;",
                "        memcpy(&typed, &raw, sizeof(typed));",
                f"        atomic_store(&bvb_first_target_{name}, typed);",
                f"        typed = bvb_first_proxy_{name};",
                "        PFN_vkVoidFunction erased = NULL;",
                "        memcpy(&erased, &typed, sizeof(erased));",
                "        return erased;",
                "    }",
            ]
        )
    lines.extend(["    return raw;", "}"])

    lines.append(
        "static PFN_vkVoidFunction bvb_first_generated_required_stub("
        "const char *name, enum bvb_dxvk_dispatch_scope scope) {"
    )
    for entry in entries:
        if not entry["required"]:
            continue
        name = str(entry["name"])
        lines.extend(
            [
                f"    if (scope == {SCOPES[str(entry['scope'])]} &&",
                f"        strcmp(name, {c_string(name)}) == 0) {{",
                f"        PFN_{name} typed = bvb_first_stub_{name};",
                "        PFN_vkVoidFunction erased = NULL;",
                "        memcpy(&erased, &typed, sizeof(erased));",
                "        return erased;",
                "    }",
            ]
        )
    lines.extend(["    return NULL;", "}"])
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("registry", type=Path)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("triangle_dispatch", type=Path)
    parser.add_argument("additional_executable", type=Path)
    parser.add_argument("output", type=Path)
    arguments = parser.parse_args()
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(
        generate(
            arguments.registry, arguments.manifest,
            arguments.triangle_dispatch, arguments.additional_executable,
        )
    )


if __name__ == "__main__":
    main()
