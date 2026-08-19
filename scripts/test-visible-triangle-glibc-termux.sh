#!/data/data/com.termux/files/usr/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="$project_dir/build"
out_dir="$project_dir/out"
triangle_out="$out_dir/visible-triangle-glibc"
runtime_parent=${TMPDIR:-/tmp}
package_name=io.github.huntergdavis.bvb.visiblehost
activity_name=android.app.NativeActivity
manifest="$project_dir/android/visible-host/AndroidManifest.xml"
triangle_client="$triangle_out/bvb-visible-triangle-client-glibc"
glibc_control_client="$out_dir/bvb-bridge-client-glibc"
visible_mode=${BVB_VISIBLE_MODE:-abstract}
case "$visible_mode" in
    abstract)
        gate_lower=e018
        gate_upper=E018
        ;;
    loopback-inline)
        gate_lower=e019
        gate_upper=E019
        ;;
    *)
        printf 'unsupported BVB_VISIBLE_MODE: %s\n' "$visible_mode" >&2
        exit 2
        ;;
esac

for command_name in am cmake grun gcc logcat od python readelf sed tr; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        printf 'missing required command: %s\n' "$command_name" >&2
        exit 2
    fi
done
for required_file in "$build_dir/bvb-bridge-service" "$manifest" \
    "$triangle_client" "$project_dir/src/lifecycle.c" \
    "$project_dir/src/protocol.c" "$project_dir/src/transport.c" \
    "$project_dir/src/handle.c" "$project_dir/src/command_batch.c" \
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
cmake --build "$build_dir" --parallel --target bvb-bridge-service
grun -s gcc -std=c17 -O3 -DNDEBUG -Wall -Wextra -Werror \
    -I"$project_dir/include" \
    "$project_dir/src/lifecycle.c" \
    "$project_dir/src/protocol.c" \
    "$project_dir/src/transport.c" \
    "$project_dir/src/handle.c" \
    "$project_dir/src/command_batch.c" \
    "$project_dir/src/bridge_client.c" \
    -o "$glibc_control_client"
if ! readelf -l "$glibc_control_client" | \
    grep -q '/glibc/lib/ld-linux-aarch64.so.1'; then
    printf 'control client does not use the expected Termux glibc interpreter\n' \
        >&2
    exit 3
fi

runtime_dir=$(mktemp -d "$runtime_parent/bvb-$gate_lower.XXXXXX")
control_socket="$runtime_dir/bridge.sock"
service_stdout="$out_dir/$gate_lower-service.stdout"
service_stderr="$out_dir/$gate_lower-service.stderr"
launch_stdout="$out_dir/$gate_lower-activity-launch.stdout"
triangle_json="$out_dir/$gate_lower-triangle-client.json"
triangle_stderr="$out_dir/$gate_lower-triangle-client.stderr"
status_json="$out_dir/$gate_lower-activity-status.json"
evidence_json="$out_dir/$gate_lower-visible-glibc-gate.json"
app_log="$out_dir/$gate_lower-visible-host.logcat"
token=$(od -An -N32 -tx1 /dev/urandom | tr -d ' \n')
visible_socket="bvb-$gate_lower-$$"
visible_port=
if [ "$visible_mode" = loopback-inline ]; then
    visible_port=$(python - <<'PY'
import socket

listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
listener.bind(("127.0.0.1", 0))
print(listener.getsockname()[1])
listener.close()
PY
)
    case "$visible_port" in
        ''|*[!0-9]*)
            printf 'could not reserve a loopback port\n' >&2
            exit 3
            ;;
    esac
fi
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
: > "$launch_stdout"
: > "$triangle_json"
: > "$triangle_stderr"
: > "$status_json"
: > "$app_log"
"$build_dir/bvb-bridge-service" --socket "$control_socket" --once \
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
        printf 'activity lifecycle ingress readiness timed out\n' >&2
        exit 4
    fi
    sleep 0.05
done
if [ -z "$port" ] || [ ! -S "$control_socket" ]; then
    printf 'Bionic service exited before becoming ready\n' >&2
    exit 4
fi

if [ "$visible_mode" = loopback-inline ]; then
    am start -S -W -n "$package_name/$activity_name" \
        --ei bvb_activity_port "$port" \
        --es bvb_activity_token "$token" \
        --ei bvb_visible_port "$visible_port" > "$launch_stdout"
else
    am start -S -W -n "$package_name/$activity_name" \
        --ei bvb_activity_port "$port" \
        --es bvb_activity_token "$token" \
        --es bvb_visible_socket "$visible_socket" > "$launch_stdout"
fi

attempt=0
window_line=
while [ -z "$window_line" ] && kill -0 "$service_pid" 2>/dev/null; do
    if grep -q 'activity_event=12 ' "$service_stdout"; then
        printf 'Activity reported renderer failure before batch handoff\n' >&2
        sed -n '/activity_event=/p' "$service_stdout" >&2
        exit 5
    fi
    window_line=$(sed -n \
        's/.*activity_event=7 .* pid=\([0-9][0-9]*\) width=\([0-9][0-9]*\) height=\([0-9][0-9]*\).*/\1 \2 \3/p' \
        "$service_stdout" | tail -n 1)
    if [ -n "$window_line" ]; then
        break
    fi
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 100 ]; then
        printf 'Activity window dimensions timed out\n' >&2
        sed -n '/activity_event=/p' "$service_stdout" >&2
        exit 5
    fi
    sleep 0.05
done
if [ -z "$window_line" ]; then
    printf 'Activity lifecycle service exited before reporting a window\n' >&2
    sed -n '/activity_event=/p' "$service_stdout" >&2
    exit 5
fi
set -- $window_line
activity_pid=$1
width=$2
height=$3

if [ "$visible_mode" = loopback-inline ]; then
    if ! grun "$triangle_client" \
        --tcp-port "$visible_port" --token "$token" \
        --width "$width" --height "$height" \
        > "$triangle_json" 2> "$triangle_stderr"; then
        cat "$triangle_stderr" >&2
        exit 7
    fi
else
    if ! grun "$triangle_client" \
        --socket-name "$visible_socket" --token "$token" \
        --width "$width" --height "$height" \
        > "$triangle_json" 2> "$triangle_stderr"; then
        cat "$triangle_stderr" >&2
        exit 7
    fi
fi

attempt=0
while kill -0 "$service_pid" 2>/dev/null; do
    if grep -q 'activity_event=12 ' "$service_stdout"; then
        printf 'Activity reported renderer failure after batch handoff\n' >&2
        sed -n '/activity_event=/p' "$service_stdout" >&2
        exit 6
    fi
    if grep -q 'activity_event=11 ' "$service_stdout" && \
        grep -q 'activity_event=9 ' "$service_stdout"; then
        break
    fi
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 100 ]; then
        printf 'external renderer lifecycle completion timed out\n' >&2
        sed -n '/activity_event=/p' "$service_stdout" >&2
        exit 6
    fi
    sleep 0.05
done
if ! grep -q 'activity_event=11 ' "$service_stdout" || \
    ! grep -q 'activity_event=9 ' "$service_stdout"; then
    printf 'Activity lifecycle service exited before renderer completion\n' >&2
    sed -n '/activity_event=/p' "$service_stdout" >&2
    exit 6
fi

grun "$glibc_control_client" --socket "$control_socket" --activity-status \
    > "$status_json"
wait "$service_pid"
service_pid=
logcat -d --pid "$activity_pid" -v threadtime > "$app_log" 2>/dev/null || true

python - "$triangle_json" "$status_json" "$evidence_json" \
    "$visible_mode" "$gate_upper" <<'PY'
import json
import pathlib
import sys

triangle_path, status_path, evidence_path = map(pathlib.Path, sys.argv[1:4])
visible_mode, gate = sys.argv[4:6]
triangle = json.loads(triangle_path.read_text())
status = json.loads(status_path.read_text())
activity = status["activity_status"]
assert triangle["commands"] == 6
assert triangle["batch_bytes"] == 200
assert triangle["width"] == activity["width"]
assert triangle["height"] == activity["height"]
assert activity["renderer_ready"] is True
assert activity["window_present"] is True
assert activity["focused"] is True
document = {
    "schema_version": 1,
    "gate": gate,
    "source_libc": "glibc",
    "host_libc": "bionic",
    "transport": (
        "loopback_tcp_inline"
        if visible_mode == "loopback-inline"
        else "abstract_unix_scm_rights_memfd"
    ),
    "triangle_client": triangle,
    "activity_status": activity,
}
if visible_mode == "loopback-inline":
    assert triangle["transport"] == "loopback_tcp_inline"
    assert triangle["packet_bytes"] == 256
    assert triangle["round_trip_ns"] > 0
else:
    assert triangle["execute_round_trip_ns"] > 0
evidence_path.write_text(json.dumps(document, indent=2) + "\n")
print(json.dumps(document, indent=2))
print(f"visible_triangle_{visible_mode}=PASS")
PY

if grep -q "${gate_upper}_PASS" "$app_log"; then
    printf 'app_log_external_pass=PASS\n'
else
    printf 'warning: %s_PASS was not readable through logcat; protocol and ' \
        "$gate_upper" >&2
    printf 'lifecycle gates passed\n' >&2
fi
printf 'evidence_json=%s\n' "$evidence_json"
printf 'triangle_json=%s\n' "$triangle_json"
printf 'triangle_stderr=%s\n' "$triangle_stderr"
printf 'activity_status_json=%s\n' "$status_json"
printf 'service_stdout=%s\n' "$service_stdout"
printf 'service_stderr=%s\n' "$service_stderr"
printf 'app_log=%s\n' "$app_log"
