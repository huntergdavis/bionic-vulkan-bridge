#!/usr/bin/env sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="$project_dir/build"
out_dir="$project_dir/out/triangle-dispatch-glibc"
generated_dir="$out_dir/generated"
registry="$build_dir/_deps/vulkanheaders-src/registry/vk.xml"
manifest="$project_dir/docs/evidence/e011-tombraider-vulkan-dispatch-manifest.json"
additional_dispatch="$project_dir/config/e066-device-buffer-requirements-dispatch.txt"
dispatch_gate=E066

for command_name in cmake cp grun gcc python readelf; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        printf 'missing required command: %s\n' "$command_name" >&2
        exit 2
    fi
done

if [ ! -f "$registry" ]; then
    cmake -S "$project_dir" -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
fi
if [ ! -f "$registry" ]; then
    printf 'pinned Vulkan registry is unavailable: %s\n' "$registry" >&2
    exit 3
fi

mkdir -p "$generated_dir"
python "$project_dir/scripts/generate-triangle-dispatch.py" \
    "$registry" "$manifest" \
    "$generated_dir/bvb_triangle_dispatch.inc"
python "$project_dir/scripts/generate-dxvk-dispatch-policy.py" \
    "$registry" "$manifest" \
    "$generated_dir/bvb_triangle_dispatch.inc" \
    "$generated_dir/bvb_dxvk_dispatch_policy.inc" \
    "$generated_dir/bvb_dxvk_dispatch_policy.json" \
    --additional-executable \
    "$additional_dispatch" \
    --gate "$dispatch_gate"
python "$project_dir/scripts/generate-vulkan-discovery-wire.py" \
    "$registry" \
    "$generated_dir/bvb_vulkan_discovery_wire.inc"

library="$out_dir/libvulkan-bvb-glibc.so"
icd_manifest="$out_dir/bvb_icd.aarch64.json"
test_binary="$out_dir/bvb-triangle-dispatch-test-glibc"

grun -s gcc -std=c17 -O3 -DNDEBUG -fPIC -fvisibility=hidden \
    -Wall -Wextra -Werror -shared \
    -I"$project_dir/include" -I"$generated_dir" \
    -I"$build_dir/_deps/vulkanheaders-src/include" \
    "$project_dir/src/protocol.c" \
    "$project_dir/src/vulkan_descriptor_wire.c" \
    "$project_dir/src/vulkan_pipeline_wire.c" \
    "$project_dir/src/transport.c" \
    "$project_dir/src/handle.c" \
    "$project_dir/src/command_batch.c" \
    "$project_dir/src/vulkan_discovery.c" \
    "$project_dir/src/wsi_frame_ring.c" \
    "$project_dir/src/dxvk_dispatch_policy.c" \
    "$project_dir/src/global_dispatch.c" \
    "$project_dir/src/triangle_dispatch.c" \
    -pthread \
    -Wl,-Bsymbolic-functions \
    -Wl,-soname,libvulkan-bvb-glibc.so \
    -o "$library"
cp "$project_dir/config/bvb_icd.aarch64.json" "$icd_manifest"

grun -s gcc -std=c17 -O3 -DNDEBUG -Wall -Wextra -Werror \
    -I"$project_dir/include" \
    -I"$build_dir/_deps/vulkanheaders-src/include" \
    "$project_dir/tests/triangle_dispatch.c" \
    "$project_dir/src/protocol.c" \
    "$project_dir/src/handle.c" \
    "$project_dir/src/command_batch.c" \
    -L"$out_dir" -Wl,-rpath,"$out_dir" -lvulkan-bvb-glibc \
    -o "$test_binary"

if ! readelf -l "$test_binary" | grep -q '/glibc/lib/ld-linux-aarch64.so.1'; then
    printf 'triangle dispatch test does not use Termux glibc\n' >&2
    exit 4
fi
exports=$(readelf --wide --dyn-syms "$library" | \
    awk '$7 != "UND" && $8 ~ /^(vkGet(Instance|Device)ProcAddr|vk_icd|bvb_triangle_|bvb_(instance|physical_device)_proxy_id)/ {print $8}' | \
    sort -u)
export_count=$(printf '%s\n' "$exports" | sed '/^$/d' | wc -l)
if [ "$export_count" -ne 11 ]; then
    printf 'unexpected triangle dispatch export surface:\n%s\n' "$exports" >&2
    exit 4
fi

grun "$test_binary" \
    > "$out_dir/test.stdout" \
    2> "$out_dir/test.stderr"
grep -qx 'PASS: generated executable triangle dispatch' \
    "$out_dir/test.stdout"

printf 'triangle_dispatch_glibc=PASS\n'
printf 'library=%s\n' "$library"
printf 'icd_manifest=%s\n' "$icd_manifest"
printf 'test_binary=%s\n' "$test_binary"
printf 'exports=%s\n' "$(printf '%s' "$exports" | tr '\n' ',')"
