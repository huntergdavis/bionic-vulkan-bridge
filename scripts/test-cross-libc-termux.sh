#!/usr/bin/env sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="$project_dir/build"
out_dir="$project_dir/out"
runtime_parent=${TMPDIR:-/tmp}

for command_name in cmake grun gcc readelf python mktemp; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        printf 'missing required command: %s\n' "$command_name" >&2
        exit 2
    fi
done

cmake -S "$project_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=ON
cmake --build "$build_dir" --parallel
mkdir -p "$out_dir"

glibc_client="$out_dir/bvb-bridge-client-glibc"
grun -s gcc -std=c17 -O2 -Wall -Wextra -Werror \
    -I"$project_dir/include" \
    "$project_dir/src/protocol.c" \
    "$project_dir/src/transport.c" \
    "$project_dir/src/bridge_client.c" \
    -o "$glibc_client"

if ! readelf -l "$glibc_client" | grep -q '/glibc/lib/ld-linux-aarch64.so.1'; then
    printf 'client does not use the expected Termux glibc interpreter\n' >&2
    exit 3
fi

runtime_dir=$(mktemp -d "$runtime_parent/bvb-cross-libc.XXXXXX")
socket_path="$runtime_dir/bridge.sock"
service_pid=
cleanup() {
    if [ -n "$service_pid" ] && kill -0 "$service_pid" 2>/dev/null; then
        kill "$service_pid" 2>/dev/null || true
        wait "$service_pid" 2>/dev/null || true
    fi
    rmdir "$runtime_dir" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

"$build_dir/bvb-bridge-service" --socket "$socket_path" --once \
    > "$out_dir/cross-libc-service.stdout" \
    2> "$out_dir/cross-libc-service.stderr" &
service_pid=$!

attempt=0
while [ ! -S "$socket_path" ] && kill -0 "$service_pid" 2>/dev/null; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 100 ]; then
        printf 'Bionic service readiness timed out\n' >&2
        exit 4
    fi
    sleep 0.05
done
if [ ! -S "$socket_path" ]; then
    printf 'Bionic service exited before creating its socket\n' >&2
    exit 4
fi

start_ns=$(date +%s%N)
grun "$glibc_client" --socket "$socket_path" \
    --vulkan-caps --vulkan-selftest \
    > "$out_dir/cross-libc-handshake.json" \
    2> "$out_dir/cross-libc-client.stderr"
end_ns=$(date +%s%N)
wait "$service_pid"
service_pid=

"$build_dir/bvb-vulkan-probe" > "$out_dir/direct-vulkan-caps.json"
"$build_dir/bvb-vulkan-selftest" > "$out_dir/direct-vulkan-selftest.json"

python - \
    "$out_dir/cross-libc-handshake.json" \
    "$out_dir/direct-vulkan-caps.json" \
    "$out_dir/direct-vulkan-selftest.json" <<'PY'
import json
import pathlib
import sys

bridged = json.loads(pathlib.Path(sys.argv[1]).read_text())
direct = json.loads(pathlib.Path(sys.argv[2]).read_text())
direct_selftest = json.loads(pathlib.Path(sys.argv[3]).read_text())
assert bridged["schema_version"] == 1
assert bridged["protocol_version"] == 1
assert bridged["bionic_service"] is True
assert bridged["android_vulkan_loader"] is True
assert bridged["pointer_bits"] == 64
assert bridged["page_size"] > 0

caps = bridged["vulkan_caps"]
assert caps["loader_api_version"] == direct["loader_api_version"]["raw"]
assert caps["instance_extension_count"] == direct["instance_extension_count"]
assert caps["physical_device_count"] == direct["physical_device_count"]
assert len(caps["physical_devices"]) == len(direct["physical_devices"])
plain_fields = (
    "name",
    "driver_version",
    "vendor_id",
    "device_id",
    "device_type",
    "queue_family_count",
    "memory_heap_count",
    "device_local_bytes",
)
for bridged_device, direct_device in zip(
    caps["physical_devices"], direct["physical_devices"], strict=True
):
    assert bridged_device["api_version"] == direct_device["api_version"]["raw"]
    for field in plain_fields:
        assert bridged_device[field] == direct_device[field], field

print(json.dumps(bridged, indent=2))
print("capability_parity=PASS")
selftest = bridged["vulkan_selftest"]
selftest_fields = (
    "instance_extension_count",
    "known_instance_extensions",
    "device_extension_count",
    "known_device_extensions",
    "queue_family_index",
    "queue_flags",
    "memory_type_index",
    "memory_property_flags",
    "buffer_bytes",
    "fill_word",
    "mismatched_words",
)
for field in selftest_fields:
    assert selftest[field] == direct_selftest[field], field
assert selftest["mismatched_words"] == 0
print("command_selftest_parity=PASS")
print(
    "bridged_submit_wait_elapsed_ns="
    f"{selftest['submit_wait_elapsed_ns']}"
)
print(
    "direct_submit_wait_elapsed_ns="
    f"{direct_selftest['submit_wait_elapsed_ns']}"
)
PY

printf 'bridge_caps_elapsed_ns=%s\n' "$((end_ns - start_ns))"
printf 'glibc_client=%s\n' "$glibc_client"
