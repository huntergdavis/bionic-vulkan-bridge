#!/data/data/com.termux/files/usr/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="$project_dir/build"
out_dir="$project_dir/out"
package_name=io.github.huntergdavis.bvb.visiblehost
activity_name=android.app.NativeActivity
client_class=io.github.huntergdavis.bvb.visiblehost.SharedRegionClient
manifest="$project_dir/android/visible-host/AndroidManifest.xml"
signed_apk="$out_dir/visible-host/bvb-visible-host-debug.apk"
helper_apk="$out_dir/e036-shared-region-client.apk"
receiver="$build_dir/bvb-external-memory-receiver"
control_client="$build_dir/bvb-bridge-client"

for command_name in am aapt cmake cp env logcat od pm python readelf sed sleep tr; do
    command -v "$command_name" >/dev/null 2>&1 || {
        printf 'missing required command: %s\n' "$command_name" >&2
        exit 2
    }
done
for required_file in "$manifest" "$signed_apk"; do
    [ -f "$required_file" ] || {
        printf 'missing required file: %s\n' "$required_file" >&2
        exit 2
    }
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
cmake -S "$project_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build "$build_dir" --parallel --target \
    bvb-external-memory-receiver bvb-bridge-service bvb-bridge-client
for binary in "$receiver" "$control_client" "$build_dir/bvb-bridge-service"; do
    if ! readelf -l "$binary" | grep -Fq '/system/bin/linker64'; then
        printf 'required executable is not Android Bionic: %s\n' "$binary" >&2
        exit 3
    fi
done
cp "$signed_apk" "$helper_apk"
chmod 0400 "$helper_apk"

runtime_dir=$(mktemp -d "${TMPDIR:-/tmp}/bvb-e036.XXXXXX")
control_socket="$runtime_dir/bridge.sock"
socket_name="bvb-e036-$(od -An -N8 -tx1 /dev/urandom | tr -d ' \n')"
token=$(od -An -N32 -tx1 /dev/urandom | tr -d ' \n')
wrong_token=ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
service_stdout="$out_dir/e036-service.stdout"
service_stderr="$out_dir/e036-service.stderr"
receiver_stdout="$out_dir/e036-receiver.stdout"
receiver_stderr="$out_dir/e036-receiver.stderr"
wrong_json="$out_dir/e036-wrong-token.json"
wrong_stderr="$out_dir/e036-wrong-token.stderr"
valid_json="$out_dir/e036-valid-token.json"
valid_stderr="$out_dir/e036-valid-token.stderr"
status_json="$out_dir/e036-activity-status.json"
app_log="$out_dir/e036-visible-host.logcat"
evidence="$out_dir/e036-external-memory-broker.json"
service_pid=
receiver_pid=
cleanup() {
    if [ -n "$receiver_pid" ] && kill -0 "$receiver_pid" 2>/dev/null; then
        kill "$receiver_pid" 2>/dev/null || true
        wait "$receiver_pid" 2>/dev/null || true
    fi
    if [ -n "$service_pid" ] && kill -0 "$service_pid" 2>/dev/null; then
        kill "$service_pid" 2>/dev/null || true
        wait "$service_pid" 2>/dev/null || true
    fi
    rmdir "$runtime_dir" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM
for output in "$service_stdout" "$service_stderr" "$receiver_stdout" \
    "$receiver_stderr" "$wrong_json" "$wrong_stderr" "$valid_json" \
    "$valid_stderr" "$status_json" "$app_log"; do
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
    [ "$attempt" -lt 100 ] || { printf 'lifecycle readiness timed out\n' >&2; exit 4; }
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
    [ "$attempt" -lt 200 ] || { printf 'renderer readiness timed out\n' >&2; exit 5; }
    sleep 0.05
done

if env -u LD_LIBRARY_PATH -u LD_PRELOAD CLASSPATH="$helper_apk" \
    /system/bin/app_process -Xnoimage-dex2oat / "$client_class" \
    "$wrong_token" "$wrong_json" unused-e036-socket external \
    >/dev/null 2> "$wrong_stderr"; then
    printf 'wrong external-memory capability unexpectedly succeeded\n' >&2
    exit 6
fi

"$receiver" --socket "$socket_name" \
    > "$receiver_stdout" 2> "$receiver_stderr" &
receiver_pid=$!
attempt=0
while ! grep -q '^bvb-external-memory-receiver: ready ' "$receiver_stdout"; do
    kill -0 "$receiver_pid" 2>/dev/null || {
        printf 'external-memory receiver exited before readiness\n' >&2
        cat "$receiver_stderr" >&2
        exit 6
    }
    attempt=$((attempt + 1))
    [ "$attempt" -lt 100 ] || { printf 'receiver readiness timed out\n' >&2; exit 6; }
    sleep 0.05
done
env -u LD_LIBRARY_PATH -u LD_PRELOAD CLASSPATH="$helper_apk" \
    /system/bin/app_process -Xnoimage-dex2oat / "$client_class" \
    "$token" "$valid_json" "$socket_name" external \
    >/dev/null 2> "$valid_stderr"
wait "$receiver_pid"
receiver_pid=
"$control_client" --socket "$control_socket" --activity-status \
    > "$status_json"
wait "$service_pid"
service_pid=
activity_pid=$(sed -n \
    's/.*activity_event=7 .* pid=\([0-9][0-9]*\).*/\1/p' \
    "$service_stdout" | tail -n 1)
logcat -d --pid "$activity_pid" -v threadtime > "$app_log" 2>/dev/null || true

python - "$wrong_json" "$valid_json" "$receiver_stdout" "$receiver_stderr" \
    "$status_json" "$app_log" "$evidence" <<'PY'
import json
import pathlib
import sys

wrong_path, valid_path, receiver_path, receiver_error_path, status_path, log_path, evidence_path = map(
    pathlib.Path, sys.argv[1:]
)
wrong = json.loads(wrong_path.read_text())
valid = json.loads(valid_path.read_text())
receiver = json.loads(next(line for line in receiver_path.read_text().splitlines() if line.startswith("{")))
status = json.loads(status_path.read_text())
assert wrong["result"] == "fail" and wrong["native_status"] == -13
assert valid["result"] == "pass"
assert valid["descriptor_kind"] == "opaque_fd"
assert valid["allocation_size"] >= 4096
assert valid["buffer_bytes"] == 4096
assert receiver["gate"] == "E036" and receiver["result"] == "pass"
assert receiver["loader_path"] == "/system/lib64/libvulkan.so"
assert receiver["allocation_size"] == valid["allocation_size"]
assert receiver["memory_type_index"] == valid["memory_type_index"]
assert receiver["buffer_bytes"] == 4096
assert receiver["mismatched_bytes"] == 0
assert receiver_error_path.stat().st_size == 0
assert status["activity_status"]["renderer_ready"] is True
document = {
    "schema_version": 1,
    "gate": "E036",
    "result": "pass",
    "target": "Galaxy Tab S8+ Android Bionic Adreno 730",
    "producer": "visible Android Activity renderer VkDevice",
    "consumer": "Termux Bionic Vulkan process",
    "cross_uid_transport": "Binder ParcelFileDescriptor then LocalSocket SCM_RIGHTS",
    "wrong_capability": wrong,
    "binder_helper": valid,
    "external_memory_import": receiver,
    "activity_status": status["activity_status"],
    "renderer_export_log_seen": "E036_EXPORT_PASS" in log_path.read_text(),
    "receiver_stderr_bytes": 0,
}
evidence_path.write_text(json.dumps(document, indent=2) + "\n")
print(json.dumps(document, indent=2))
print("e036_external_memory_broker=PASS")
PY
printf 'evidence=%s\n' "$evidence"
