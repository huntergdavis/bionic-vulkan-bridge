#!/data/data/com.termux/files/usr/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="$project_dir/build"
out_dir="$project_dir/out"
object_dir="$out_dir/e022-relay-objects"
relay_binary="$out_dir/bvb-shared-region-relay-glibc"
vulkan_headers="$build_dir/_deps/vulkanheaders-src/include"

for command_name in cmake file grun gcc readelf sha256sum; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        printf 'missing required command: %s\n' "$command_name" >&2
        exit 2
    fi
done
for required_file in "$project_dir/src/lifecycle.c" \
    "$project_dir/src/protocol.c" "$project_dir/src/transport.c" \
    "$project_dir/src/handle.c" "$project_dir/src/command_batch.c" \
    "$project_dir/src/triangle_dispatch.c" \
    "$project_dir/src/triangle_batch_builder.c" \
    "$project_dir/src/shared_region_relay.c" \
    "$vulkan_headers/vulkan/vulkan.h"; do
    if [ ! -f "$required_file" ]; then
        printf 'missing required file: %s\n' "$required_file" >&2
        exit 2
    fi
done

mkdir -p "$object_dir"
cmake --build "$build_dir" --parallel --target bvb-triangle-batch-builder
for source_name in lifecycle protocol transport handle command_batch \
    triangle_dispatch triangle_batch_builder shared_region_relay; do
    grun -s gcc -std=c17 -O3 -DNDEBUG -Wall -Wextra -Werror \
        -I"$project_dir/include" -I"$build_dir/generated" \
        -I"$vulkan_headers" -c "$project_dir/src/$source_name.c" \
        -o "$object_dir/$source_name.o"
    printf 'relay_object=%s\n' "$source_name"
done
grun -s gcc \
    "$object_dir/lifecycle.o" "$object_dir/protocol.o" \
    "$object_dir/transport.o" "$object_dir/handle.o" \
    "$object_dir/command_batch.o" "$object_dir/triangle_dispatch.o" \
    "$object_dir/triangle_batch_builder.o" \
    "$object_dir/shared_region_relay.o" -o "$relay_binary"
if ! readelf -l "$relay_binary" | \
    grep -q '/glibc/lib/ld-linux-aarch64.so.1'; then
    printf 'relay does not use the expected Termux glibc interpreter\n' >&2
    exit 3
fi
printf 'RELAY_BINARY=%s\n' "$relay_binary"
file "$relay_binary"
sha256sum "$relay_binary"
