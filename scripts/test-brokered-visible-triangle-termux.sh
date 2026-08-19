#!/data/data/com.termux/files/usr/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="$project_dir/build"
out_dir="$project_dir/out"
runtime_parent=${TMPDIR:-/tmp}
package_name=io.github.huntergdavis.bvb.visiblehost
activity_name=android.app.NativeActivity
client_class=io.github.huntergdavis.bvb.visiblehost.SharedRegionClient
manifest="$project_dir/android/visible-host/AndroidManifest.xml"
signed_apk="$out_dir/visible-host/bvb-visible-host-debug.apk"
helper_apk="$out_dir/e022-shared-region-client.apk"
relay_builder="$project_dir/scripts/build-shared-region-relay-glibc-termux.sh"
relay_client="$out_dir/bvb-shared-region-relay-glibc"
control_client="$build_dir/bvb-bridge-client"
screencap_command=/system/bin/screencap
gate=${BVB_VISIBLE_GATE:-E022}
frames=${BVB_VISIBLE_FRAMES:-1}
ring_slots=${BVB_VISIBLE_RING_SLOTS:-1}
gate_lower=$(printf '%s' "$gate" | tr '[:upper:]' '[:lower:]')

case "$frames:$ring_slots" in
    *[!0-9:]*|0:*|*:0)
        printf 'invalid frame-ring settings: frames=%s ring_slots=%s\n' \
            "$frames" "$ring_slots" >&2
        exit 2
        ;;
esac

for command_name in am aapt chmod cmake cp env grun logcat od pm python \
    readelf sed sha256sum sleep tr; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        printf 'missing required command: %s\n' "$command_name" >&2
        exit 2
    fi
done
if [ ! -x "$screencap_command" ]; then
    printf 'missing required command: %s\n' "$screencap_command" >&2
    exit 2
fi
for required_file in "$manifest" "$signed_apk" "$relay_builder"; do
    if [ ! -f "$required_file" ]; then
        printf 'missing required file: %s\n' "$required_file" >&2
        exit 2
    fi
done
expected_version=$(sed -n \
    's/.*android:versionCode="\([0-9][0-9]*\)".*/\1/p' "$manifest")
installed_version=$(pm list packages --show-versioncode 2>/dev/null | \
    sed -n "s/^package:$package_name versionCode:\([0-9][0-9]*\).*$/\1/p")
if [ "$installed_version" != "$expected_version" ]; then
    printf 'visible-host version mismatch: installed=%s expected=%s\n' \
        "${installed_version:-missing}" "${expected_version:-missing}" >&2
    exit 2
fi

mkdir -p "$out_dir"
"$relay_builder"
cmake --build "$build_dir" --parallel --target \
    bvb-bridge-service bvb-bridge-client
if [ ! -x "$relay_client" ] || [ ! -x "$control_client" ]; then
    printf 'required native client was not built\n' >&2
    exit 3
fi
if [ -e "$helper_apk" ]; then
    chmod 0600 "$helper_apk"
fi
cp "$signed_apk" "$helper_apk"
chmod 0400 "$helper_apk"

runtime_dir=$(mktemp -d "$runtime_parent/bvb-$gate_lower.XXXXXX")
control_socket="$runtime_dir/bridge.sock"
service_stdout="$out_dir/$gate_lower-service.stdout"
service_stderr="$out_dir/$gate_lower-service.stderr"
launch_stdout="$out_dir/$gate_lower-activity-launch.stdout"
wrong_json="$out_dir/$gate_lower-wrong-token.json"
wrong_stdout="$out_dir/$gate_lower-wrong-token.stdout"
wrong_stderr="$out_dir/$gate_lower-wrong-token.stderr"
valid_json="$out_dir/$gate_lower-valid-token.json"
valid_stdout="$out_dir/$gate_lower-valid-token.stdout"
valid_stderr="$out_dir/$gate_lower-valid-token.stderr"
relay_stdout="$out_dir/$gate_lower-relay.stdout"
relay_stderr="$out_dir/$gate_lower-relay.stderr"
status_json="$out_dir/$gate_lower-activity-status.json"
evidence_json="$out_dir/$gate_lower-brokered-visible-gate.json"
app_log="$out_dir/$gate_lower-visible-host.logcat"
screenshot="$out_dir/$gate_lower-visible-triangle.png"
token=$(od -An -N32 -tx1 /dev/urandom | tr -d ' \n')
wrong_token=ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
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
service_pid=
relay_pid=
cleanup() {
    if [ -n "$relay_pid" ] && kill -0 "$relay_pid" 2>/dev/null; then
        kill "$relay_pid" 2>/dev/null || true
        wait "$relay_pid" 2>/dev/null || true
    fi
    if [ -n "$service_pid" ] && kill -0 "$service_pid" 2>/dev/null; then
        kill "$service_pid" 2>/dev/null || true
        wait "$service_pid" 2>/dev/null || true
    fi
    rmdir "$runtime_dir" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM
for output in "$service_stdout" "$service_stderr" "$launch_stdout" \
    "$wrong_json" "$wrong_stdout" "$wrong_stderr" "$valid_json" \
    "$valid_stdout" "$valid_stderr" "$relay_stdout" "$relay_stderr" \
    "$status_json" "$app_log"; do
    : > "$output"
done

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

am start -S -W -n "$package_name/$activity_name" \
    --ei bvb_activity_port "$port" \
    --es bvb_activity_token "$token" \
    --ei bvb_visible_port "$visible_port" \
    --ei bvb_visible_frames "$frames" > "$launch_stdout"
attempt=0
window_line=
while [ -z "$window_line" ] && kill -0 "$service_pid" 2>/dev/null; do
    if grep -q 'activity_event=12 ' "$service_stdout"; then
        printf 'Activity reported renderer failure before batch handoff\n' >&2
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
        exit 5
    fi
    sleep 0.05
done
if [ -z "$window_line" ]; then
    printf 'Activity lifecycle service exited before reporting a window\n' >&2
    exit 5
fi
set -- $window_line
activity_pid=$1
width=$2
height=$3

run_helper() {
    helper_token=$1
    result_path=$2
    stdout_path=$3
    stderr_path=$4
    relay_socket=${5:-}
    if [ -n "$relay_socket" ]; then
        env -u LD_LIBRARY_PATH -u LD_PRELOAD CLASSPATH="$helper_apk" \
            /system/bin/app_process -Xnoimage-dex2oat / "$client_class" \
            "$helper_token" "$result_path" "$relay_socket" \
            > "$stdout_path" 2> "$stderr_path"
    else
        env -u LD_LIBRARY_PATH -u LD_PRELOAD CLASSPATH="$helper_apk" \
            /system/bin/app_process -Xnoimage-dex2oat / "$client_class" \
            "$helper_token" "$result_path" \
            > "$stdout_path" 2> "$stderr_path"
    fi
}
if run_helper "$wrong_token" "$wrong_json" "$wrong_stdout" \
    "$wrong_stderr"; then
    printf 'wrong provider capability unexpectedly succeeded\n' >&2
    exit 6
fi

relay_socket="bvb-$gate_lower-$(printf '%.16s' "$token")"
grun "$relay_client" --socket "$relay_socket" \
    --visible-port "$visible_port" --token "$token" \
    --width "$width" --height "$height" \
    --frames "$frames" --ring-slots "$ring_slots" \
    > "$relay_stdout" 2> "$relay_stderr" &
relay_pid=$!
attempt=0
while kill -0 "$relay_pid" 2>/dev/null; do
    if grep -q '^bvb-shared-region-relay: ready socket=.* mode=visible$' \
        "$relay_stdout"; then
        break
    fi
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 100 ]; then
        printf 'glibc visible relay readiness timed out\n' >&2
        exit 6
    fi
    sleep 0.05
done
if ! kill -0 "$relay_pid" 2>/dev/null; then
    printf 'glibc visible relay exited before becoming ready\n' >&2
    cat "$relay_stderr" >&2
    exit 6
fi
if ! run_helper "$token" "$valid_json" "$valid_stdout" "$valid_stderr" \
    "$relay_socket"; then
    printf 'valid provider/visible replay failed\n' >&2
    cat "$valid_stderr" >&2
    cat "$valid_json" >&2
    exit 7
fi
if ! wait "$relay_pid"; then
    relay_pid=
    printf 'glibc visible relay failed\n' >&2
    cat "$relay_stderr" >&2
    exit 7
fi
relay_pid=
"$screencap_command" -p "$screenshot" 2>/dev/null || true
if grep -q 'activity_event=12 ' "$service_stdout"; then
    printf 'Activity reported renderer failure after batch handoff\n' >&2
    exit 7
fi
"$control_client" --socket "$control_socket" --activity-status \
    > "$status_json"
wait "$service_pid"
service_pid=
logcat -d --pid "$activity_pid" -v threadtime > "$app_log" 2>/dev/null || true

python - "$wrong_json" "$valid_json" "$relay_stdout" "$status_json" \
    "$service_stdout" "$evidence_json" "$screenshot" "$gate" \
    "$frames" "$ring_slots" <<'PY'
import hashlib
import json
import pathlib
import re
import sys

wrong_path, valid_path, relay_path, status_path, service_path, evidence_path, screenshot_path = map(
    pathlib.Path, sys.argv[1:8]
)
gate = sys.argv[8]
frames = int(sys.argv[9])
ring_slots = int(sys.argv[10])
wrong = json.loads(wrong_path.read_text())
valid = json.loads(valid_path.read_text())
relay = json.loads(next(
    line for line in relay_path.read_text().splitlines() if line.startswith("{")
))
status = json.loads(status_path.read_text())
activity = status["activity_status"]
event_pattern = re.compile(
    r"activity_event=(\d+) sequence=(\d+) pid=(\d+) "
    r"width=(\d+) height=(\d+)"
)
events = [
    {
        "event": int(match.group(1)),
        "sequence": int(match.group(2)),
        "pid": int(match.group(3)),
        "width": int(match.group(4)),
        "height": int(match.group(5)),
    }
    for match in event_pattern.finditer(service_path.read_text())
]
event_codes = {record["event"] for record in events}
window_events = [record for record in events if record["event"] == 7]
assert wrong == {
    "result": "fail",
    "stage": "request_region",
    "native_status": -13,
}
assert valid["result"] == "pass"
assert valid["binder_region_received"] is True
assert valid["relay"] == "same_uid_scm_rights"
assert valid["relay_round_trip_ns"] > 0
assert relay["result"] == "pass"
assert relay["transport"] == "binder_scm_rights_then_loopback_metadata"
assert relay["region_bytes"] == 4096
assert relay["writable_mapping"] is True
assert window_events
assert relay["width"] == window_events[-1]["width"]
assert relay["height"] == window_events[-1]["height"]
assert relay["batch_offset"] == 64
assert relay["batch_bytes"] == 224
assert relay["commands"] == 7
assert relay["sequence"] == frames
assert relay["frames"] == frames
assert relay["ring_slots"] == ring_slots
assert relay["batch_stride"] == 256
assert relay["receive_validate_ns"] > 0
assert relay["execute_round_trip_ns"] > 0
assert relay["receive_to_present_ns"] >= relay["execute_round_trip_ns"]
assert activity["rejected_event_count"] == 0
assert activity["authenticated_event_count"] == len(events)
assert {1, 2, 3, 7}.issubset(event_codes)
assert 12 not in event_codes
baseline_ns = 14618177
screenshot = None
if screenshot_path.is_file() and screenshot_path.stat().st_size > 0:
    screenshot = {
        "path": str(screenshot_path),
        "bytes": screenshot_path.stat().st_size,
        "sha256": hashlib.sha256(screenshot_path.read_bytes()).hexdigest(),
    }
document = {
    "schema_version": 1,
    "gate": gate,
    "result": "pass",
    "source_libc": "glibc",
    "host_libc": "bionic",
    "transport": "binder_scm_rights_shared_mapping_then_loopback_metadata",
    "wrong_capability": wrong,
    "binder_helper": valid,
    "glibc_relay": relay,
    "activity_status": activity,
    "authenticated_lifecycle_events": events,
    "e019_inline_baseline_round_trip_ns": baseline_ns,
    "execute_vs_e019_percent": round(
        (relay["execute_round_trip_ns"] - baseline_ns) / baseline_ns * 100.0,
        2,
    ),
    "full_relay_vs_e019_percent": round(
        (valid["relay_round_trip_ns"] - baseline_ns) / baseline_ns * 100.0,
        2,
    ),
    "screenshot": screenshot,
}
evidence_path.write_text(json.dumps(document, indent=2) + "\n")
print(json.dumps(document, indent=2))
print(f"{gate.lower()}_brokered_visible=PASS")
PY

if grep -q "${gate}_PASS" "$app_log"; then
    printf 'app_log_%s_pass=PASS\n' "$gate_lower"
else
    printf '%s\n' \
        "warning: ${gate}_PASS was not readable through logcat; protocol and lifecycle gates passed" \
        >&2
fi
printf 'evidence_json=%s\n' "$evidence_json"
printf 'relay_stdout=%s\n' "$relay_stdout"
printf 'relay_stderr=%s\n' "$relay_stderr"
printf 'activity_status_json=%s\n' "$status_json"
printf 'service_stdout=%s\n' "$service_stdout"
printf 'service_stderr=%s\n' "$service_stderr"
printf 'app_log=%s\n' "$app_log"
printf 'screenshot=%s\n' "$screenshot"
