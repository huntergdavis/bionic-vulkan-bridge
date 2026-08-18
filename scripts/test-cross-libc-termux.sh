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
    > "$out_dir/cross-libc-handshake.json" \
    2> "$out_dir/cross-libc-client.stderr"
end_ns=$(date +%s%N)
wait "$service_pid"
service_pid=

python - "$out_dir/cross-libc-handshake.json" <<'PY'
import json
import pathlib
import sys

document = json.loads(pathlib.Path(sys.argv[1]).read_text())
assert document["schema_version"] == 1
assert document["protocol_version"] == 1
assert document["bionic_service"] is True
assert document["android_vulkan_loader"] is True
assert document["pointer_bits"] == 64
assert document["page_size"] > 0
print(json.dumps(document, indent=2))
PY

printf 'handshake_elapsed_ns=%s\n' "$((end_ns - start_ns))"
printf 'glibc_client=%s\n' "$glibc_client"

