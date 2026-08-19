#!/data/data/com.termux/files/usr/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="$project_dir/build"
out_dir="$project_dir/out"
runtime_parent=${TMPDIR:-/tmp}
package_name=io.github.huntergdavis.bvb.visiblehost
activity_name=android.app.NativeActivity
manifest="$project_dir/android/visible-host/AndroidManifest.xml"

for command_name in am grun gcc od python readelf sed tr; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        printf 'missing required command: %s\n' "$command_name" >&2
        exit 2
    fi
done
for required_file in "$build_dir/bvb-bridge-service" \
    "$manifest" \
    "$project_dir/src/lifecycle.c" "$project_dir/src/protocol.c" \
    "$project_dir/src/transport.c" "$project_dir/src/handle.c" \
    "$project_dir/src/command_batch.c" \
    "$project_dir/src/bridge_client.c"; do
    if [ ! -f "$required_file" ]; then
        printf 'missing required file: %s\n' "$required_file" >&2
        exit 2
    fi
done
if ! pm path "$package_name" >/dev/null 2>&1; then
    printf 'visible-host package is not installed: %s\n' "$package_name" >&2
    exit 2
fi
expected_version=$(sed -n \
    's/.*android:versionCode="\([0-9][0-9]*\)".*/\1/p' "$manifest")
installed_version=$(pm list packages --show-versioncode 2>/dev/null | \
    sed -n "s/^package:$package_name versionCode:\([0-9][0-9]*\).*$/\1/p")
if [ -z "$expected_version" ] || [ -z "$installed_version" ]; then
    printf 'could not resolve visible-host version codes\n' >&2
    exit 2
fi
if [ "$installed_version" != "$expected_version" ]; then
    printf 'visible-host version mismatch: installed=%s expected=%s\n' \
        "$installed_version" "$expected_version" >&2
    exit 2
fi

mkdir -p "$out_dir"
glibc_client="$out_dir/bvb-bridge-client-glibc"
grun -s gcc -std=c17 -O2 -Wall -Wextra -Werror \
    -I"$project_dir/include" \
    "$project_dir/src/lifecycle.c" \
    "$project_dir/src/protocol.c" \
    "$project_dir/src/transport.c" \
    "$project_dir/src/handle.c" \
    "$project_dir/src/command_batch.c" \
    "$project_dir/src/bridge_client.c" \
    -o "$glibc_client"
if ! readelf -l "$glibc_client" | grep -q '/glibc/lib/ld-linux-aarch64.so.1'; then
    printf 'client does not use the expected Termux glibc interpreter\n' >&2
    exit 3
fi

runtime_dir=$(mktemp -d "$runtime_parent/bvb-e010.XXXXXX")
socket_path="$runtime_dir/bridge.sock"
service_stdout="$out_dir/e010-service.stdout"
service_stderr="$out_dir/e010-service.stderr"
status_json="$out_dir/e010-activity-status.json"
launch_stdout="$out_dir/e010-activity-launch.stdout"
token=$(od -An -N32 -tx1 /dev/urandom | tr -d ' \n')
service_pid=
cleanup() {
    if [ -n "$service_pid" ] && kill -0 "$service_pid" 2>/dev/null; then
        kill "$service_pid" 2>/dev/null || true
        wait "$service_pid" 2>/dev/null || true
    fi
    rmdir "$runtime_dir" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

: > "$service_stdout"
: > "$service_stderr"
"$build_dir/bvb-bridge-service" --socket "$socket_path" --once \
    --activity-port 0 --activity-token "$token" \
    > "$service_stdout" 2> "$service_stderr" &
service_pid=$!

attempt=0
port=
while [ -z "$port" ] && kill -0 "$service_pid" 2>/dev/null; do
    port=$(sed -n 's/.*activity_port=\([0-9][0-9]*\)$/\1/p' \
        "$service_stdout" | tail -n 1)
    if [ -n "$port" ]; then
        break
    fi
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 100 ]; then
        printf 'activity ingress readiness timed out\n' >&2
        exit 4
    fi
    sleep 0.05
done
if [ -z "$port" ] || [ ! -S "$socket_path" ]; then
    printf 'Bionic service exited before becoming ready\n' >&2
    exit 4
fi

python - "$port" <<'PY'
import socket
import struct
import sys
import time

port = int(sys.argv[1])
record = struct.pack(
    "<IHHIIIIQ32s",
    0x314C5642,
    1,
    1,
    1,
    0,
    0,
    12345,
    time.monotonic_ns(),
    bytes(32),
)
with socket.create_connection(("127.0.0.1", port), timeout=1.0) as connection:
    connection.sendall(record)
    chunks = []
    remaining = 16
    while remaining:
        chunk = connection.recv(remaining)
        if not chunk:
            raise RuntimeError("short lifecycle ACK")
        chunks.append(chunk)
        remaining -= len(chunk)
magic, version, reserved, sequence, status = struct.unpack(
    "<IHHIi", b"".join(chunks)
)
assert (magic, version, reserved, sequence, status) == (
    0x314C5642,
    1,
    0,
    1,
    -13,
)
print("invalid_token_rejection=PASS")
PY

am start -n "$package_name/$activity_name" \
    --ei bvb_activity_port "$port" \
    --es bvb_activity_token "$token" > "$launch_stdout"

attempt=0
while kill -0 "$service_pid" 2>/dev/null; do
    if grep -q 'activity_event=12 ' "$service_stdout"; then
        printf 'Activity reported renderer failure\n' >&2
        sed -n '/activity_event=/p' "$service_stdout" >&2
        exit 5
    fi
    if grep -q 'activity_event=11 ' "$service_stdout" && \
        grep -q 'activity_event=9 ' "$service_stdout"; then
        break
    fi
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 200 ]; then
        printf 'Activity lifecycle events timed out\n' >&2
        sed -n '/activity_event=/p' "$service_stdout" >&2
        exit 5
    fi
    sleep 0.05
done

grun "$glibc_client" --socket "$socket_path" --activity-status \
    > "$status_json"
wait "$service_pid"
service_pid=

python - "$status_json" <<'PY'
import json
import pathlib
import sys

document = json.loads(pathlib.Path(sys.argv[1]).read_text())
assert document["bionic_service"] is True
assert document["service_flags"] & 4
activity = document["activity_status"]
assert activity["ingress_configured"] is True
assert activity["authenticated_event_count"] >= 6
assert activity["rejected_event_count"] == 1
assert activity["created"] is True
assert activity["started"] is True
assert activity["resumed"] is True
assert activity["window_present"] is True
assert activity["renderer_ready"] is True
assert activity["focused"] is True
assert activity["destroyed"] is False
assert activity["width"] > 0
assert activity["height"] > 0
assert activity["activity_pid"] > 0
assert activity["last_event_monotonic_ns"] > 0
assert activity["last_event_received_ns"] > 0
print(json.dumps(document, indent=2))
print("activity_lifecycle_handoff=PASS")
PY

printf 'activity_status_json=%s\n' "$status_json"
printf 'service_stdout=%s\n' "$service_stdout"
printf 'service_stderr=%s\n' "$service_stderr"
