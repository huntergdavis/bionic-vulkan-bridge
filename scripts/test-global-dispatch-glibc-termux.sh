#!/data/data/com.termux/files/usr/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="$project_dir/build"
out_dir="$project_dir/out/triangle-dispatch-glibc"
library="$out_dir/libvulkan-bvb-glibc.so"
client="$out_dir/bvb-global-dispatch-test-glibc"
service="$build_dir/bvb-bridge-service"
policy_json="$out_dir/generated/bvb_dxvk_dispatch_policy.json"
evidence="$project_dir/out/e034-mapped-memory.json"
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
            "$runtime_parent"/bvb-e034.*) rmdir "$runtime_dir" 2>/dev/null || true ;;
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

runtime_dir=$(mktemp -d "$runtime_parent/bvb-e034.XXXXXX")
case "$runtime_dir" in
    "$runtime_parent"/bvb-e034.*) ;;
    *) printf 'unexpected runtime directory: %s\n' "$runtime_dir" >&2; exit 3 ;;
esac
control_socket="$runtime_dir/bridge.sock"
client_stdout="$out_dir/e034-client.stdout"
client_stderr="$out_dir/e034-client.stderr"
service_stdout="$out_dir/e034-service.stdout"
service_stderr="$out_dir/e034-service.stderr"

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
    printf 'E034 emitted unexpected stderr\n' >&2
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
assert policy["gate"] == "E065"
assert policy["summary"]["command_count"] == 742
assert policy["summary"]["executable_name_count"] == 83
assert policy["summary"]["support_counts"] == {
    "probed_null": 302,
    "required_unimplemented": 357,
    "executable": 83,
}
client_stdout = client_stdout_path.read_text().strip()
match = re.fullmatch(
    r"PASS: global Vulkan discovery api=(\d+) "
    r"exposed_extensions=0 exposed_layers=0 "
    r"instance_one=(\d+) instance_two=(\d+) physical_device=(\d+) "
    r"device=(.*?) device_api=(\d+) driver=(\d+) vendor=(\d+) "
    r"device_id=(\d+) queues=(\d+) memory_types=(\d+) "
    r"memory_heaps=(\d+) device_extensions=(\d+) "
    r"sampler_anisotropy=(\d+) logical_device=(\d+) queue=(\d+) "
    r"empty_submit=(-?\d+) queue_wait=(-?\d+) device_wait=(-?\d+) "
    r"command_pool=(\d+) command_buffer=(\d+) command_submit=(-?\d+) "
    r"pool_reset=(-?\d+) buffer=(\d+) memory=(\d+) memory_type=(\d+) "
    r"mapped_bytes=(\d+) mapped_mismatches=(\d+) "
    r"fill_words=(\d+) mismatches=(\d+) fence=(\d+) fence_before=(-?\d+) "
    r"fenced_submit=(-?\d+) fence_after=(-?\d+) fence_wait=(-?\d+) "
    r"fence_reset=(-?\d+) fence_after_reset=(-?\d+)",
    client_stdout,
)
assert match is not None, client_stdout
(
    api_version_text,
    instance_one_text,
    instance_two_text,
    physical_device_text,
    device_name,
    device_api_text,
    driver_version_text,
    vendor_id_text,
    device_id_text,
    queue_count_text,
    memory_type_count_text,
    memory_heap_count_text,
    device_extension_count_text,
    sampler_anisotropy_text,
    logical_device_text,
    logical_queue_text,
    empty_submit_text,
    queue_wait_text,
    device_wait_text,
    command_pool_text,
    command_buffer_text,
    command_submit_text,
    pool_reset_text,
    buffer_text,
    memory_text,
    memory_type_text,
    mapped_bytes_text,
    mapped_mismatches_text,
    fill_words_text,
    mismatches_text,
    fence_text,
    fence_before_text,
    fenced_submit_text,
    fence_after_text,
    fence_wait_text,
    fence_reset_text,
    fence_after_reset_text,
) = match.groups()
(
    api_version,
    instance_one,
    instance_two,
    physical_device,
    device_api,
    driver_version,
    vendor_id,
    device_id,
    queue_count,
    memory_type_count,
    memory_heap_count,
    device_extension_count,
    sampler_anisotropy,
    logical_device,
    logical_queue,
    empty_submit_result,
    queue_wait_result,
    device_wait_result,
    command_pool,
    command_buffer,
    command_submit_result,
    pool_reset_result,
    buffer,
    device_memory,
    memory_type_index,
    mapped_bytes,
    mapped_mismatches,
    fill_words,
    mismatches,
    fence,
    fence_before_result,
    fenced_submit_result,
    fence_after_result,
    fence_wait_result,
    fence_reset_result,
    fence_after_reset_result,
) = map(int, (
    api_version_text,
    instance_one_text,
    instance_two_text,
    physical_device_text,
    device_api_text,
    driver_version_text,
    vendor_id_text,
    device_id_text,
    queue_count_text,
    memory_type_count_text,
    memory_heap_count_text,
    device_extension_count_text,
    sampler_anisotropy_text,
    logical_device_text,
    logical_queue_text,
    empty_submit_text,
    queue_wait_text,
    device_wait_text,
    command_pool_text,
    command_buffer_text,
    command_submit_text,
    pool_reset_text,
    buffer_text,
    memory_text,
    memory_type_text,
    mapped_bytes_text,
    mapped_mismatches_text,
    fill_words_text,
    mismatches_text,
    fence_text,
    fence_before_text,
    fenced_submit_text,
    fence_after_text,
    fence_wait_text,
    fence_reset_text,
    fence_after_reset_text,
))
assert api_version == 0x00404000
assert instance_one == 0x0100000000000001
assert instance_two == 0x0100000000000002
assert physical_device == 0x0200000000000001
assert device_name
assert device_api >= 0x00400000
assert vendor_id != 0
assert queue_count > 0
assert 0 < memory_type_count <= 32
assert 0 < memory_heap_count <= 16
assert device_extension_count > 15
assert sampler_anisotropy == 1
assert logical_device == 0x0300000000000001
assert logical_queue == 0x0400000000000001
assert empty_submit_result == 0
assert queue_wait_result == 0
assert device_wait_result == 0
assert command_pool == 0x0A00000000000001
assert command_buffer == 0x0B00000000000001
assert command_submit_result == 0
assert pool_reset_result == 0
assert buffer == 0x1300000000000001
assert device_memory == 0x0900000000000001
assert 0 <= memory_type_index < memory_type_count
assert mapped_bytes == 4096
assert mapped_mismatches == 0
assert fill_words == 1024
assert mismatches == 0
assert fence == 0x1200000000000001
assert fence_before_result == 1
assert fenced_submit_result == 0
assert fence_after_result == 0
assert fence_wait_result == 0
assert fence_reset_result == 0
assert fence_after_reset_result == 1

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
    "bvb_device_proxy_id",
    "bvb_queue_proxy_id",
    "bvb_command_pool_proxy_id",
    "bvb_command_buffer_proxy_id",
    "bvb_buffer_proxy_id",
    "bvb_memory_proxy_id",
    "bvb_fence_proxy_id",
    "vkGetDeviceProcAddr",
    "vkGetInstanceProcAddr",
}
assert expected_exports <= symbol_names

document = {
    "schema_version": 1,
    "gate": "E034",
    "result": "pass",
    "source_commit": source_commit,
    "target": "Galaxy Tab S8+ Termux ARM64 glibc to Android Bionic",
    "glibc_interpreter":
        "/data/data/com.termux/files/usr/glibc/lib/ld-linux-aarch64.so.1",
    "bionic_interpreter": "/system/bin/linker64",
    "vulkan_loader": "/system/lib64/libvulkan.so",
    "policy": policy,
    "physical_device_discovery": {
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
        "device_name": device_name,
        "device_api_version": device_api,
        "driver_version": driver_version,
        "vendor_id": vendor_id,
        "device_id": device_id,
        "queue_family_count": queue_count,
        "memory_type_count": memory_type_count,
        "memory_heap_count": memory_heap_count,
        "device_extension_count": device_extension_count,
        "extension_transport_paginated": device_extension_count > 15,
        "sampler_anisotropy": sampler_anisotropy,
        "logical_device_id": logical_device,
        "logical_device_type": 3,
        "logical_device_serial": 1,
        "queue_id": logical_queue,
        "queue_type": 4,
        "queue_serial": 1,
        "queue_identity_stable": True,
        "empty_submit_result": empty_submit_result,
        "unsupported_submit_shape_rejected_client_side": True,
        "queue_wait_idle_result": queue_wait_result,
        "device_wait_idle_result": device_wait_result,
        "command_pool_id": command_pool,
        "command_pool_type": 10,
        "command_pool_serial": 1,
        "command_buffer_id": command_buffer,
        "command_buffer_type": 11,
        "command_buffer_serial": 1,
        "command_submit_result": command_submit_result,
        "command_pool_reset_result": pool_reset_result,
        "command_buffer_freed_explicitly": True,
        "command_pool_destroyed_explicitly": True,
        "buffer_id": buffer,
        "buffer_type": 19,
        "buffer_serial": 1,
        "memory_id": device_memory,
        "memory_type": 9,
        "memory_serial": 1,
        "selected_memory_type_index": memory_type_index,
        "mapped_bytes": mapped_bytes,
        "mapped_mismatches": mapped_mismatches,
        "fill_word": "0xa5c3f00d",
        "fill_word_count": fill_words,
        "mismatched_words": mismatches,
        "fence_id": fence,
        "fence_type": 18,
        "fence_serial": 1,
        "fence_status_before_submit": fence_before_result,
        "fenced_submit_result": fenced_submit_result,
        "fence_status_after_submit": fence_after_result,
        "fence_wait_result": fence_wait_result,
        "fence_reset_result": fence_reset_result,
        "fence_status_after_reset": fence_after_reset_result,
        "queue_wait_used_for_gpu_fill": False,
        "fence_destroyed_explicitly": True,
        "buffer_destroyed_explicitly": True,
        "memory_freed_explicitly": True,
        "logical_device_destroyed_explicitly": True,
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
assert document["physical_device_discovery"]["service_ready"] is True
evidence_path.write_text(json.dumps(document, indent=2) + "\n")
print(json.dumps(document, indent=2))
print("e034_mapped_memory=PASS")
PY

printf 'evidence=%s\n' "$evidence"
