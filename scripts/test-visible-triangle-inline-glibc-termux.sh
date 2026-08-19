#!/data/data/com.termux/files/usr/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BVB_VISIBLE_MODE=loopback-inline \
    exec "$script_dir/test-visible-triangle-glibc-termux.sh" "$@"
