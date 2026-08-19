#!/data/data/com.termux/files/usr/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BVB_SHARED_REGION_RELAY=1 \
    exec "$script_dir/test-shared-region-provider-termux.sh" "$@"
