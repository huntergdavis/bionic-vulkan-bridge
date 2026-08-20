#!/data/data/com.termux/files/usr/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="$project_dir/build"
out_dir="$project_dir/out"
package_name=io.github.huntergdavis.bvb.visiblehost
activity_name=io.github.huntergdavis.bvb.visiblehost.VisibleHostActivity
manifest="$project_dir/android/visible-host/AndroidManifest.xml"

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
cmake -S "$project_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build "$build_dir" --parallel --target \
    bvb-external-image-binder bvb-bridge-service bvb-bridge-client
for binary in "$build_dir/bvb-external-image-binder" \
    "$build_dir/bvb-bridge-service" "$build_dir/bvb-bridge-client"; do
    readelf -l "$binary" | grep -Fq '/system/bin/linker64' || {
        printf 'required executable is not Android Bionic: %s\n' "$binary" >&2
        exit 3
    }
done

runtime_dir=$(mktemp -d "${TMPDIR:-/tmp}/bvb-e040.XXXXXX")
control_socket="$runtime_dir/bridge.sock"
token=$(od -An -N32 -tx1 /dev/urandom | tr -d ' \n')
wrong_token=ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
service_stdout="$out_dir/e040-service.stdout"
service_stderr="$out_dir/e040-service.stderr"
valid_json="$out_dir/e040-valid.json"
valid_stderr="$out_dir/e040-valid.stderr"
wrong_json="$out_dir/e040-wrong-token.json"
wrong_stderr="$out_dir/e040-wrong-token.stderr"
status_json="$out_dir/e040-activity-status.json"
evidence="$out_dir/e040-native-binder-image-channel.json"
service_pid=
cleanup() {
    if [ -n "$service_pid" ] && kill -0 "$service_pid" 2>/dev/null; then
        kill "$service_pid" 2>/dev/null || true
        wait "$service_pid" 2>/dev/null || true
    fi
    rmdir "$runtime_dir" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM
for output in "$service_stdout" "$service_stderr" "$valid_json" \
    "$valid_stderr" "$wrong_json" "$wrong_stderr" "$status_json"; do
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
    [ -n "$port" ] && break
    attempt=$((attempt + 1))
    [ "$attempt" -lt 100 ] || exit 4
    sleep 0.05
done
am start -S -n "$package_name/$activity_name" \
    --ei bvb_activity_port "$port" --es bvb_activity_token "$token" \
    >/dev/null
attempt=0
while ! grep -q 'activity_event=11 ' "$service_stdout"; do
    if grep -q 'activity_event=12 ' "$service_stdout"; then
        printf 'Activity reported renderer failure\n' >&2
        exit 5
    fi
    attempt=$((attempt + 1))
    [ "$attempt" -lt 200 ] || exit 5
    sleep 0.05
done

"$build_dir/bvb-external-image-binder" --token "$token" \
    > "$valid_json" 2> "$valid_stderr"
if "$build_dir/bvb-external-image-binder" --token "$wrong_token" \
    > "$wrong_json" 2> "$wrong_stderr"; then
    printf 'wrong native Binder capability unexpectedly succeeded\n' >&2
    exit 6
fi
"$build_dir/bvb-bridge-client" --socket "$control_socket" --activity-status \
    > "$status_json"
wait "$service_pid"
service_pid=

python - "$valid_json" "$valid_stderr" "$wrong_json" "$status_json" \
    "$service_stdout" "$evidence" <<'PY'
import json
import pathlib
import sys

valid_path, valid_error_path, wrong_path, status_path, service_path, evidence_path = map(
    pathlib.Path, sys.argv[1:]
)
valid = json.loads(valid_path.read_text())
wrong = json.loads(wrong_path.read_text())
status = json.loads(status_path.read_text())
service = service_path.read_text()
assert valid["gate"] == "E040" and valid["result"] == "pass"
assert valid["transport"] == "native_binder_setup_then_connected_socket"
assert valid["client_binder_calls_setup"] == 1
assert valid["binder_calls_steady_state"] == 0 and valid["java_calls"] == 0
assert valid["channel_acknowledged"] is True
assert valid["descriptor_count"] == 3
assert valid["width"] == 64 and valid["height"] == 64
assert valid["format"] == 37 and valid["expected_color"] == 0xFFFF00FF
assert valid["mismatched_pixels"] == 0
assert wrong["result"] == "fail" and wrong["native_status"] == -13
assert valid_error_path.stat().st_size == 0
assert "activity_event=11 " in service
document = {
    "schema_version": 1,
    "gate": "E040",
    "result": "pass",
    "target": "Galaxy Tab S8+ Android Bionic Adreno 730",
    "transport": "one native Binder setup transaction then connected native socket",
    "client_binder_calls_setup": 1,
    "binder_calls_steady_state": 0,
    "java_calls": 0,
    "external_image_import": valid,
    "wrong_capability": wrong,
    "activity_status": status["activity_status"],
    "renderer_ready_seen": True,
    "client_stderr_bytes": 0,
}
evidence_path.write_text(json.dumps(document, indent=2) + "\n")
print(json.dumps(document, indent=2))
print("e040_native_binder_image_channel=PASS")
PY
printf 'evidence=%s\n' "$evidence"
