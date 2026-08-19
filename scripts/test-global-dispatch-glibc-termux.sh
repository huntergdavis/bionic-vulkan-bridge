#!/data/data/com.termux/files/usr/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="$project_dir/build"
out_dir="$project_dir/out/triangle-dispatch-glibc"
library="$out_dir/libvulkan-bvb-glibc.so"
client="$out_dir/bvb-global-dispatch-test-glibc"
service="$build_dir/bvb-bridge-service"
policy_json="$out_dir/generated/bvb_dxvk_dispatch_policy.json"
evidence="$project_dir/out/e026-instance-vulkan-bootstrap.json"
vulkan_headers="$build_dir/_deps/vulkanheaders-src/include"
runtime_parent=${TMPDIR:-$PREFIX/tmp}
runtime_dir=
service_pid=

cleanup() {
    if [ -n "$service_pid" ] && kill -0 "$service_pid" 2>/dev/null; then
        kill "$service_pid" 2>/dev/null || true
        wait "$service_pid" 2>/dev/null || true
    fi
    if [ -n "$runtime_dir" ] && [ -d "$runtime_dir" ] &&
        [ ! -L "$runtime_dir" ]; then
        case "$runtime_dir" in
            "$runtime_parent"/bvb-e026.*) rmdir "$runtime_dir" 2>/dev/null || true ;;
        esac
    fi
}
trap cleanup EXIT HUP INT TERM

for command_name in cmake file git grun gcc python readelf sha256sum; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        printf 'missing required command: %s\n' "$command_name" >&2
        exit 2
    fi
done

"$project_dir/scripts/test-triangle-dispatch-glibc-termux.sh" >/dev/null
cmake --build "$build_dir" --parallel --target bvb-bridge-service

grun -s gcc -std=c17 -O3 -DNDEBUG -Wall -Wextra -Werror \
    -I"$project_dir/include" -I"$vulkan_headers" \
    "$project_dir/tests/global_dispatch.c" "$project_dir/src/handle.c" \
    -L"$out_dir" -Wl,-rpath,"$out_dir" -lvulkan-bvb-glibc \
    -o "$client"

glibc_interpreter=/data/data/com.termux/files/usr/glibc/lib/ld-linux-aarch64.so.1
bionic_interpreter=/system/bin/linker64
if ! readelf -l "$client" | grep -Fq "$glibc_interpreter"; then
    printf 'global dispatch client does not use Termux glibc\n' >&2
    exit 3
fi
if ! readelf -l "$service" | grep -Fq "$bionic_interpreter"; then
    printf 'bridge service does not use Android Bionic\n' >&2
    exit 3
fi

runtime_dir=$(mktemp -d "$runtime_parent/bvb-e026.XXXXXX")
case "$runtime_dir" in
    "$runtime_parent"/bvb-e026.*) ;;
    *) printf 'unexpected runtime directory: %s\n' "$runtime_dir" >&2; exit 3 ;;
esac
control_socket="$runtime_dir/bridge.sock"
client_stdout="$out_dir/e026-client.stdout"
client_stderr="$out_dir/e026-client.stderr"
service_stdout="$out_dir/e026-service.stdout"
service_stderr="$out_dir/e026-service.stderr"

"$service" --socket "$control_socket" --once \
    >"$service_stdout" 2>"$service_stderr" &
service_pid=$!
ready=0
for attempt in 1 2 3 4 5 6 7 8 9 10; do
    if [ -S "$control_socket" ]; then
        ready=1
        break
    fi
    sleep 0.1
done
if [ "$ready" -ne 1 ]; then
    printf 'bridge service did not create its control socket\n' >&2
    exit 4
fi

BVB_BRIDGE_SOCKET="$control_socket" grun "$client" \
    >"$client_stdout" 2>"$client_stderr"
wait "$service_pid"
service_pid=
if [ -s "$client_stderr" ] || [ -s "$service_stderr" ]; then
    printf 'E026 emitted unexpected stderr\n' >&2
    exit 5
fi

source_commit=$(git -C "$project_dir" rev-parse HEAD)
python - "$policy_json" "$library" "$client" "$service" \
    "$client_stdout" "$service_stdout" "$source_commit" "$evidence" <<'PY'
import hashlib
import json
import pathlib
import re
import subprocess
import sys

(
    policy_path,
    library_path,
    client_path,
    service_path,
    client_stdout_path,
    service_stdout_path,
    source_commit,
    evidence_path,
) = [pathlib.Path(value) for value in sys.argv[1:7]] + [
    sys.argv[7],
    pathlib.Path(sys.argv[8]),
]


def artifact(path):
    content = path.read_bytes()
    return {
        "path": str(path),
        "bytes": len(content),
        "sha256": hashlib.sha256(content).hexdigest(),
    }


policy = json.loads(policy_path.read_text())
assert policy["gate"] == "E026"
assert policy["summary"]["command_count"] == 742
assert policy["summary"]["executable_name_count"] == 14
assert policy["summary"]["support_counts"] == {
    "probed_null": 302,
    "required_unimplemented": 426,
    "executable": 14,
}
client_stdout = client_stdout_path.read_text().strip()
match = re.fullmatch(
    r"PASS: global Vulkan bootstrap api=(\d+) "
    r"exposed_extensions=0 exposed_layers=0 "
    r"instance_one=(\d+) instance_two=(\d+) physical_device=(\d+)",
    client_stdout,
)
assert match is not None, client_stdout
api_version, instance_one, instance_two, physical_device = map(
    int, match.groups()
)
assert api_version == 0x00404000
assert instance_one == 0x0100000000000001
assert instance_two == 0x0100000000000002
assert physical_device == 0x0200000000000001

symbols = subprocess.run(
    ["readelf", "--wide", "--dyn-syms", str(library_path)],
    check=True,
    capture_output=True,
    text=True,
).stdout
symbol_names = {
    fields[-1]
    for line in symbols.splitlines()
    if (fields := line.split())
}
expected_exports = {
    "bvb_instance_proxy_id",
    "bvb_physical_device_proxy_id",
    "vkGetDeviceProcAddr",
    "vkGetInstanceProcAddr",
}
assert expected_exports <= symbol_names

document = {
    "schema_version": 1,
    "gate": "E026",
    "result": "pass",
    "source_commit": source_commit,
    "target": "Galaxy Tab S8+ Termux ARM64 glibc to Android Bionic",
    "glibc_interpreter":
        "/data/data/com.termux/files/usr/glibc/lib/ld-linux-aarch64.so.1",
    "bionic_interpreter": "/system/bin/linker64",
    "vulkan_loader": "/system/lib64/libvulkan.so",
    "policy": policy,
    "global_bootstrap": {
        "loader_api_version": api_version,
        "loader_api_version_text": "1.4.0",
        "exposed_extension_count": 0,
        "exposed_layer_count": 0,
        "instance_ids": [instance_one, instance_two],
        "instance_type": 1,
        "instance_serials": [1, 2],
        "physical_device_id": physical_device,
        "physical_device_type": 2,
        "physical_device_serial": 1,
        "instances_destroyed_explicitly": True,
        "client_stdout": client_stdout,
        "client_stderr_bytes": 0,
        "service_stderr_bytes": 0,
        "service_ready": "ready socket=" in service_stdout_path.read_text(),
    },
    "dynamic_exports": sorted(expected_exports),
    "artifacts": {
        "policy_summary": artifact(policy_path),
        "glibc_library": artifact(library_path),
        "glibc_client": artifact(client_path),
        "bionic_service": artifact(service_path),
    },
}
assert document["global_bootstrap"]["service_ready"] is True
evidence_path.write_text(json.dumps(document, indent=2) + "\n")
print(json.dumps(document, indent=2))
print("e026_instance_vulkan_bootstrap=PASS")
PY

printf 'evidence=%s\n' "$evidence"
