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
helper_apk="$out_dir/e020-shared-region-client.apk"

for command_name in am aapt chmod cmake cp env grun gcc od python readelf \
    sed tr; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        printf 'missing required command: %s\n' "$command_name" >&2
        exit 2
    fi
done
for required_file in "$build_dir/bvb-bridge-service" "$manifest" \
    "$signed_apk" "$project_dir/src/lifecycle.c" \
    "$project_dir/src/protocol.c" "$project_dir/src/transport.c" \
    "$project_dir/src/handle.c" "$project_dir/src/command_batch.c" \
    "$project_dir/src/bridge_client.c"; do
    if [ ! -f "$required_file" ]; then
        printf 'missing required file: %s\n' "$required_file" >&2
        exit 2
    fi
done
if ! aapt list "$signed_apk" | grep -qx 'classes.dex'; then
    printf 'visible host APK is missing classes.dex\n' >&2
    exit 2
fi
if ! pm path "$package_name" >/dev/null 2>&1; then
    printf 'visible-host package is not installed: %s\n' "$package_name" >&2
    exit 2
fi
expected_version=$(sed -n \
    's/.*android:versionCode="\([0-9][0-9]*\)".*/\1/p' "$manifest")
installed_version=$(pm list packages --show-versioncode 2>/dev/null | \
    sed -n "s/^package:$package_name versionCode:\([0-9][0-9]*\).*$/\1/p")
if [ -z "$expected_version" ] || [ -z "$installed_version" ] || \
    [ "$installed_version" != "$expected_version" ]; then
    printf 'visible-host version mismatch: installed=%s expected=%s\n' \
        "${installed_version:-unknown}" "${expected_version:-unknown}" >&2
    exit 2
fi

mkdir -p "$out_dir"
cmake --build "$build_dir" --parallel --target bvb-bridge-service
glibc_client="$out_dir/bvb-bridge-client-glibc"
grun -s gcc -std=c17 -O3 -DNDEBUG -Wall -Wextra -Werror \
    -I"$project_dir/include" \
    "$project_dir/src/lifecycle.c" "$project_dir/src/protocol.c" \
    "$project_dir/src/transport.c" "$project_dir/src/handle.c" \
    "$project_dir/src/command_batch.c" "$project_dir/src/bridge_client.c" \
    -o "$glibc_client"
if ! readelf -l "$glibc_client" | \
    grep -q '/glibc/lib/ld-linux-aarch64.so.1'; then
    printf 'control client does not use Termux glibc\n' >&2
    exit 3
fi

case "$helper_apk" in
    "$out_dir/e020-shared-region-client.apk") ;;
    *)
        printf 'unexpected helper APK path: %s\n' "$helper_apk" >&2
        exit 2
        ;;
esac
if [ -e "$helper_apk" ]; then
    chmod 0600 "$helper_apk"
fi
cp "$signed_apk" "$helper_apk"
chmod 0400 "$helper_apk"
runtime_dir=$(mktemp -d "$runtime_parent/bvb-e020.XXXXXX")
control_socket="$runtime_dir/bridge.sock"
service_stdout="$out_dir/e020-service.stdout"
service_stderr="$out_dir/e020-service.stderr"
launch_stdout="$out_dir/e020-activity-launch.stdout"
wrong_json="$out_dir/e020-wrong-token.json"
wrong_stdout="$out_dir/e020-wrong-token.stdout"
wrong_stderr="$out_dir/e020-wrong-token.stderr"
valid_json="$out_dir/e020-valid-token.json"
valid_stdout="$out_dir/e020-valid-token.stdout"
valid_stderr="$out_dir/e020-valid-token.stderr"
status_json="$out_dir/e020-activity-status.json"
evidence_json="$out_dir/e020-binder-fd-gate.json"
token=$(od -An -N32 -tx1 /dev/urandom | tr -d ' \n')
wrong_token=ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
service_pid=
cleanup() {
    if [ -n "$service_pid" ] && kill -0 "$service_pid" 2>/dev/null; then
        kill "$service_pid" 2>/dev/null || true
        wait "$service_pid" 2>/dev/null || true
    fi
    rmdir "$runtime_dir" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

for output in "$service_stdout" "$service_stderr" "$launch_stdout" \
    "$wrong_json" "$wrong_stdout" "$wrong_stderr" "$valid_json" \
    "$valid_stdout" "$valid_stderr" "$status_json"; do
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

am start -W -n "$package_name/$activity_name" \
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

run_helper() {
    helper_token=$1
    result_path=$2
    stdout_path=$3
    stderr_path=$4
    env -u LD_LIBRARY_PATH -u LD_PRELOAD CLASSPATH="$helper_apk" \
        /system/bin/app_process -Xnoimage-dex2oat / "$client_class" \
        "$helper_token" "$result_path" > "$stdout_path" 2> "$stderr_path"
}

if run_helper "$wrong_token" "$wrong_json" "$wrong_stdout" \
    "$wrong_stderr"; then
    printf 'wrong provider capability unexpectedly succeeded\n' >&2
    exit 6
fi
if ! run_helper "$token" "$valid_json" "$valid_stdout" "$valid_stderr"; then
    printf 'valid provider capability failed\n' >&2
    sed -n '1,160p' "$valid_stderr" >&2
    cat "$valid_json" >&2
    exit 6
fi

grun "$glibc_client" --socket "$control_socket" --activity-status \
    > "$status_json"
wait "$service_pid"
service_pid=

python - "$wrong_json" "$valid_json" "$status_json" \
    "$service_stdout" "$evidence_json" <<'PY'
import json
import pathlib
import re
import sys

wrong_path, valid_path, status_path, service_path, evidence_path = map(
    pathlib.Path, sys.argv[1:]
)
wrong = json.loads(wrong_path.read_text())
valid = json.loads(valid_path.read_text())
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
assert wrong["result"] == "fail"
assert wrong["native_status"] == -13
assert valid == {
    "result": "pass",
    "region_bytes": 4096,
    "marker": "BVB_E020_SHARED_REGION",
}
assert activity["ingress_configured"] is True
assert activity["rejected_event_count"] == 0
assert activity["authenticated_event_count"] == len(events)
assert {1, 2, 3, 7, 9, 10, 11}.issubset(event_codes)
document = {
    "schema_version": 1,
    "gate": "E020",
    "result": "pass",
    "provider": "broadcast_binder_callback_parcel_file_descriptor",
    "caller_uid": "termux",
    "wrong_capability": wrong,
    "valid_capability": valid,
    "authenticated_lifecycle_events": events,
    "activity_status": activity,
}
evidence_path.write_text(json.dumps(document, indent=2) + "\n")
print(json.dumps(document, indent=2))
print("binder_shared_region_fd=PASS")
PY

printf 'evidence_json=%s\n' "$evidence_json"
printf 'valid_stderr=%s\n' "$valid_stderr"
printf 'wrong_stderr=%s\n' "$wrong_stderr"
printf 'service_stdout=%s\n' "$service_stdout"
printf 'service_stderr=%s\n' "$service_stderr"
