#!/data/data/com.termux/files/usr/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out_dir="$project_dir/out/triangle-dispatch-glibc"
policy_json="$out_dir/generated/bvb_dxvk_dispatch_policy.json"
policy_include="$out_dir/generated/bvb_dxvk_dispatch_policy.inc"
library="$out_dir/libvulkan-bvb-glibc.so"
test_binary="$out_dir/bvb-triangle-dispatch-test-glibc"
test_stdout="$out_dir/test.stdout"
test_stderr="$out_dir/test.stderr"
evidence="$project_dir/out/e024-dxvk-dispatch-policy.json"

for command_name in python readelf; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        printf 'missing required command: %s\n' "$command_name" >&2
        exit 2
    fi
done

"$project_dir/scripts/test-triangle-dispatch-glibc-termux.sh"

python - "$policy_json" "$policy_include" "$library" "$test_binary" \
    "$test_stdout" "$test_stderr" "$evidence" <<'PY'
import hashlib
import json
import pathlib
import subprocess
import sys

(
    policy_path,
    include_path,
    library_path,
    binary_path,
    stdout_path,
    stderr_path,
    evidence_path,
) = map(pathlib.Path, sys.argv[1:])


def artifact(path):
    content = path.read_bytes()
    return {
        "path": str(path),
        "bytes": len(content),
        "sha256": hashlib.sha256(content).hexdigest(),
    }


policy = json.loads(policy_path.read_text())
assert policy["gate"] == "E024"
assert policy["summary"] == {
    "command_count": 742,
    "resolved_name_count": 440,
    "executable_name_count": 8,
    "support_counts": {
        "probed_null": 302,
        "required_unimplemented": 432,
        "executable": 8,
    },
    "dispatch_scope_counts": {
        "global": 4,
        "instance": 101,
        "device": 635,
        "private": 2,
    },
}
assert stdout_path.read_text() == "PASS: generated executable triangle dispatch\n"
assert stderr_path.read_bytes() == b""

program_headers = subprocess.run(
    ["readelf", "-l", str(binary_path)],
    check=True,
    capture_output=True,
    text=True,
).stdout
interpreter = "/data/data/com.termux/files/usr/glibc/lib/ld-linux-aarch64.so.1"
assert interpreter in program_headers

symbol_table = subprocess.run(
    ["readelf", "--wide", "--dyn-syms", str(library_path)],
    check=True,
    capture_output=True,
    text=True,
).stdout
expected_exports = sorted(
    {
        "bvb_dxvk_dispatch_policy_at",
        "bvb_dxvk_dispatch_policy_count",
        "bvb_dxvk_dispatch_policy_lookup",
        "bvb_triangle_command_buffer_create",
        "bvb_triangle_command_buffer_destroy",
        "bvb_triangle_command_buffer_finish",
        "bvb_triangle_command_buffer_status",
        "vkGetDeviceProcAddr",
    }
)
symbol_names = {
    fields[-1]
    for line in symbol_table.splitlines()
    if (fields := line.split())
}
exports = sorted(set(expected_exports) & symbol_names)
assert exports == expected_exports

document = {
    "schema_version": 1,
    "gate": "E024",
    "result": "pass",
    "target": "Termux ARM64 glibc",
    "interpreter": interpreter,
    "policy": policy,
    "runtime_validation": {
        "queried_entry_count": 742,
        "resolver_non_null_count": 8,
        "resolver_null_required_count": 432,
        "resolver_null_probed_count": 302,
        "dynamic_exports": exports,
        "test_stdout": stdout_path.read_text().strip(),
        "test_stderr_bytes": 0,
    },
    "artifacts": {
        "generated_policy": artifact(include_path),
        "policy_summary": artifact(policy_path),
        "glibc_library": artifact(library_path),
        "glibc_test": artifact(binary_path),
    },
}
evidence_path.write_text(json.dumps(document, indent=2) + "\n")
print(json.dumps(document, indent=2))
print("e024_dxvk_dispatch_policy=PASS")
PY

printf 'evidence=%s\n' "$evidence"
