#!/usr/bin/env bash

set -euo pipefail

CDPATH=''
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
: "${PREFIX:?run this script from Termux}"

compiler=${BVB_GLIBC_CC:-$PREFIX/glibc/bin/gcc}
headers=$project_dir/build/_deps/vulkanheaders-src/include
output_dir=$project_dir/out/glibc
output=$output_dir/libbvb-vulkan-resolve-trace.so

[[ -x $compiler ]] || {
    printf 'missing glibc compiler: %s\n' "$compiler" >&2
    exit 1
}
[[ -f $headers/vulkan/vulkan.h ]] || {
    printf 'missing pinned Vulkan headers; run scripts/build-termux.sh first\n' >&2
    exit 1
}
mkdir -p -- "$output_dir"

"$compiler" \
    -std=c17 -O2 -DNDEBUG -fPIC -fvisibility=hidden \
    -Wall -Wextra -Werror \
    -I"$headers" \
    -shared "$project_dir/src/vulkan_resolve_trace.c" \
    -Wl,--no-undefined,-z,relro,-z,now \
    -ldl -o "$output"

file "$output"
printf 'Built: %s\n' "$output"
