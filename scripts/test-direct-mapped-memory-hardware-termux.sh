#!/data/data/com.termux/files/usr/bin/sh
set -eu

# shellcheck disable=SC1007
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="$project_dir/build"
glibc_dir="$project_dir/out/triangle-dispatch-glibc"
service="$build_dir/bvb-bridge-service"
service_loader=${BVB_VULKAN_SERVICE_LOADER:-$HOME/steam-arm64/bvb/driver/libvulkan_freedreno.so}
runtime_parent=${TMPDIR:-$PREFIX/tmp}
result_root="$project_dir/out/e139-direct-memory-hardware"

for command_name in cmake file git grun gcc python3 readelf sha256sum; do
    command -v "$command_name" >/dev/null 2>&1 || {
        printf 'missing required command: %s\n' "$command_name" >&2
        exit 2
    }
done
test -f "$service_loader" && test ! -L "$service_loader" || {
    printf 'private Vulkan service loader is unavailable or symlinked: %s\n' \
        "$service_loader" >&2
    exit 2
}
test -d "$runtime_parent" || {
    printf 'runtime parent is unavailable: %s\n' "$runtime_parent" >&2
    exit 2
}

"$project_dir/scripts/test-triangle-dispatch-glibc-termux.sh" >/dev/null
cmake --build "$build_dir" --parallel --target bvb-bridge-service
client="$glibc_dir/bvb-direct-mapped-memory-hardware-glibc"
grun -s gcc -std=c17 -O3 -DNDEBUG -Wall -Wextra -Werror \
    -I"$project_dir/include" \
    -I"$build_dir/_deps/vulkanheaders-src/include" \
    "$project_dir/tests/direct_mapped_memory_hardware.c" \
    -L"$glibc_dir" -Wl,-rpath,"$glibc_dir" -lvulkan-bvb-glibc \
    -o "$client"
readelf -l "$client" | grep -Fq \
    '/data/data/com.termux/files/usr/glibc/lib/ld-linux-aarch64.so.1' || {
    printf 'direct-memory client does not use Termux glibc\n' >&2
    exit 3
}
readelf -l "$service" | grep -Fq '/system/bin/linker64' || {
    printf 'bridge service does not use Android Bionic\n' >&2
    exit 3
}

mkdir -p "$result_root"
run_dir=$(mktemp -d "$result_root/run.XXXXXX")
runtime=$(mktemp -d "$runtime_parent/bvb-e139-direct.XXXXXX")
test -d "$runtime" && test ! -L "$runtime" || exit 3
socket="$runtime/s"
service_stdout="$run_dir/service.stdout"
service_stderr="$run_dir/service.stderr"
client_stdout="$run_dir/client.stdout"
client_stderr="$run_dir/client.stderr"
service_pid=
cleanup() {
    if [ -n "$service_pid" ] && kill -0 "$service_pid" 2>/dev/null; then
        kill "$service_pid" 2>/dev/null || true
        index=0
        while kill -0 "$service_pid" 2>/dev/null && [ "$index" -lt 50 ]; do
            sleep 0.02
            index=$((index + 1))
        done
        if kill -0 "$service_pid" 2>/dev/null; then
            kill -KILL "$service_pid" 2>/dev/null || true
        fi
        wait "$service_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT HUP INT TERM
"$service" --socket "$socket" --once --loader "$service_loader" \
    >"$service_stdout" 2>"$service_stderr" &
service_pid=$!
index=0
while [ ! -S "$socket" ] && kill -0 "$service_pid" 2>/dev/null && \
    [ "$index" -lt 500 ]; do
    sleep 0.02
    index=$((index + 1))
done
test -S "$socket" || {
    printf 'bridge service did not create its control socket\n' >&2
    exit 4
}

set +e
BVB_BRIDGE_SOCKET="$socket" BVB_MAPPED_MEMORY=direct grun "$client" \
    >"$client_stdout" 2>"$client_stderr"
client_status=$?
set -e
index=0
while kill -0 "$service_pid" 2>/dev/null && [ "$index" -lt 500 ]; do
    sleep 0.02
    index=$((index + 1))
done
if kill -0 "$service_pid" 2>/dev/null; then
    printf 'bridge service remained live after the client exited\n' >&2
    exit 4
fi
set +e
wait "$service_pid"
service_status=$?
set -e
service_pid=
test "$client_status" -eq 0
test "$service_status" -eq 0
test ! -s "$client_stderr"
test ! -s "$service_stderr"
grep -qx 'direct_memory=PASS bytes=4096 mismatches=0 map_rtts=1 flush_rtts=0 invalidate_rtts=0 unmap_rtts=1' \
    "$client_stdout"

if [ -n "${BVB_SOURCE_COMMIT:-}" ]; then
    source_commit=$BVB_SOURCE_COMMIT
else
    git -C "$project_dir" diff --quiet --
    git -C "$project_dir" diff --cached --quiet --
    source_commit=$(git -C "$project_dir" rev-parse HEAD)
fi
case "$source_commit" in
    *[!0-9a-f]*|'') exit 3 ;;
esac
test "${#source_commit}" -eq 40
evidence="$run_dir/result.json"
python3 - "$evidence" "$source_commit" "$client" "$service" \
    "$service_loader" "$client_stdout" "$service_stdout" <<'PY'
import hashlib
import json
import pathlib
import sys

output, commit, client, service, loader, client_stdout, service_stdout = sys.argv[1:]

def artifact(value: str) -> dict[str, object]:
    path = pathlib.Path(value)
    return {
        "path": str(path),
        "bytes": path.stat().st_size,
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
    }

document = {
    "schema_version": 1,
    "gate": "E139-tablet-direct-memory",
    "result": "pass",
    "source_commit": commit,
    "transport": "Termux glibc ICD to Android Bionic private Turnip",
    "artifacts": {
        "client": artifact(client),
        "service": artifact(service),
        "private_turnip": artifact(loader),
    },
    "mapping": {
        "bytes": 4096,
        "native_mismatches": 0,
        "round_trips": {
            "map": 1,
            "flush": 0,
            "invalidate": 0,
            "unmap": 1,
        },
        "map_opcode": 125,
        "unmap_opcode": 109,
    },
    "client_stdout": artifact(client_stdout),
    "service_stdout": artifact(service_stdout),
    "claims": {
        "real_private_turnip": True,
        "cross_libc_direct_mapping": True,
        "tomb_raider": False,
        "fps": None,
    },
}
pathlib.Path(output).write_text(json.dumps(document, indent=2, sort_keys=True) + "\n")
PY
printf 'direct_mapped_memory_hardware=PASS\n'
printf 'evidence=%s\n' "$evidence"
