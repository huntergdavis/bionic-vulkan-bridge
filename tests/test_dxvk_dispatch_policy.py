#!/usr/bin/env python3

import hashlib
import json
import pathlib
import re
import subprocess
import sys
import tempfile


ENTRY_PATTERN = re.compile(
    r'^BVB_DXVK_DISPATCH_POLICY_ENTRY\('
    r'"([^"]+)", "([^"]+)", '
    r'(BVB_DXVK_SCOPE_[A-Z]+), ([0-9]+)U, '
    r'(BVB_DXVK_SUPPORT_[A-Z_]+)\)$'
)
SCOPES = {
    "global": "BVB_DXVK_SCOPE_GLOBAL",
    "instance": "BVB_DXVK_SCOPE_INSTANCE",
    "device": "BVB_DXVK_SCOPE_DEVICE",
    "private": "BVB_DXVK_SCOPE_PRIVATE",
}


def main() -> int:
    if len(sys.argv) != 7:
        raise SystemExit(
            "usage: test_dxvk_dispatch_policy.py GENERATOR REGISTRY "
            "MANIFEST TRIANGLE_INCLUDE ADDITIONAL_LIST POLICY_JSON"
        )
    generator, registry, manifest_path, triangle_include, additional, policy_json = map(
        pathlib.Path, sys.argv[1:]
    )
    with tempfile.TemporaryDirectory() as temporary:
        generated_include = pathlib.Path(temporary) / "policy.inc"
        generated_json = pathlib.Path(temporary) / "policy.json"
        subprocess.run(
            [
                sys.executable,
                str(generator),
                str(registry),
                str(manifest_path),
                str(triangle_include),
                str(generated_include),
                str(generated_json),
                "--additional-executable",
                str(additional),
                "--gate",
                "E033",
            ],
            check=True,
        )
        assert generated_json.read_bytes() == policy_json.read_bytes()

        document = json.loads(generated_json.read_text())
        summary = document["summary"]
        assert summary == {
            "command_count": 742,
            "resolved_name_count": 440,
            "executable_name_count": 46,
            "support_counts": {
                "probed_null": 302,
                "required_unimplemented": 394,
                "executable": 46,
            },
            "dispatch_scope_counts": {
                "global": 4,
                "instance": 101,
                "device": 635,
                "private": 2,
            },
        }
        include_bytes = generated_include.read_bytes()
        assert document["source"]["generated_policy_sha256"] == (
            hashlib.sha256(include_bytes).hexdigest()
        )
        parsed = []
        for line in generated_include.read_text().splitlines():
            match = ENTRY_PATTERN.match(line)
            if match:
                parsed.append(match.groups())
        assert len(parsed) == 742
        assert [entry[0] for entry in parsed] == sorted(
            entry[0] for entry in parsed
        )

        manifest = json.loads(manifest_path.read_text())
        records = {entry["name"]: entry for entry in manifest["commands"]}
        assert set(records) == {entry[0] for entry in parsed}
        executable = set(document["executable_names"])
        for name, canonical, scope, count, support in parsed:
            record = records[name]
            expected_canonical = record["canonical_name"] or name
            assert canonical == expected_canonical
            assert scope == SCOPES[record["dispatch_scope"]]
            assert int(count) == record["lookup_count"]
            if name in executable:
                expected_support = "BVB_DXVK_SUPPORT_EXECUTABLE"
            elif record["resolved_stages"]:
                expected_support = (
                    "BVB_DXVK_SUPPORT_REQUIRED_UNIMPLEMENTED"
                )
            else:
                expected_support = "BVB_DXVK_SUPPORT_PROBED_NULL"
            assert support == expected_support

    print("PASS: generated E033 DXVK dispatch policy")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
