#!/data/data/com.termux/files/usr/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="$project_dir/build"
out_dir="$project_dir/out/triangle-dispatch-glibc"
library="$out_dir/libvulkan-bvb-glibc.so"
icd_manifest="$out_dir/bvb_icd.aarch64.json"
client="$out_dir/bvb-icd-loader-test-glibc"
service="$build_dir/bvb-bridge-service"
vulkan_headers="$build_dir/_deps/vulkanheaders-src/include"
steam_loader_dir="$HOME/steam-arm64/client/steamrtarm64"
steam_loader="$steam_loader_dir/libvulkan.so"
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
            "$runtime_parent"/bvb-e050.*) rmdir "$runtime_dir" 2>/dev/null || true ;;
        esac
    fi
}
trap cleanup EXIT HUP INT TERM

for command_name in cat cmake grun gcc grep mktemp readelf sed; do
    command -v "$command_name" >/dev/null 2>&1 || {
        printf 'missing required command: %s\n' "$command_name" >&2
        exit 2
    }
done
[ -f "$steam_loader" ] || {
    printf 'Steam glibc Vulkan loader is unavailable: %s\n' "$steam_loader" >&2
    exit 2
}

"$project_dir/scripts/test-triangle-dispatch-glibc-termux.sh" >/dev/null
cmake --build "$build_dir" --parallel --target bvb-bridge-service
[ -f "$library" ] && [ -f "$icd_manifest" ] || {
    printf 'BVB ICD artifacts were not generated\n' >&2
    exit 3
}

grun -s gcc -std=c17 -O3 -DNDEBUG -Wall -Wextra -Werror \
    -I"$vulkan_headers" "$project_dir/tests/icd_loader.c" \
    -L"$steam_loader_dir" -Wl,-rpath,"$steam_loader_dir" -lvulkan \
    -o "$client"
if ! readelf -l "$client" | grep -q '/glibc/lib/ld-linux-aarch64.so.1'; then
    printf 'ICD loader client is not glibc AArch64\n' >&2
    exit 3
fi
if ! readelf --wide --dyn-syms "$library" | \
    grep -q 'vk_icdNegotiateLoaderICDInterfaceVersion'; then
    printf 'BVB library is missing ICD negotiation export\n' >&2
    exit 3
fi

runtime_dir=$(mktemp -d "$runtime_parent/bvb-e050.XXXXXX")
control_socket="$runtime_dir/bridge.sock"
client_stdout="$out_dir/e048-icd-client.stdout"
client_stderr="$out_dir/e048-icd-client.stderr"
service_stdout="$out_dir/e048-service.stdout"
service_stderr="$out_dir/e048-service.stderr"
"$service" --socket "$control_socket" --once \
    >"$service_stdout" 2>"$service_stderr" &
service_pid=$!
attempt=0
while [ ! -S "$control_socket" ]; do
    attempt=$((attempt + 1))
    [ "$attempt" -lt 100 ] || {
        printf 'bridge service readiness timed out\n' >&2
        exit 4
    }
    sleep 0.05
done

BVB_BRIDGE_SOCKET="$control_socket" \
VK_DRIVER_FILES="$icd_manifest" VK_ICD_FILENAMES="$icd_manifest" \
    grun "$client" >"$client_stdout" 2>"$client_stderr"
wait "$service_pid"
service_pid=
if [ -s "$client_stderr" ] || [ -s "$service_stderr" ]; then
    printf 'E050 emitted unexpected stderr\n' >&2
    sed -n '1,160p' "$client_stderr" >&2
    sed -n '1,160p' "$service_stderr" >&2
    exit 5
fi
grep -q '^PASS: Vulkan loader selected BVB ICD ' "$client_stdout" || {
    printf 'E050 loader result is missing\n' >&2
    exit 5
}
cat "$client_stdout"
