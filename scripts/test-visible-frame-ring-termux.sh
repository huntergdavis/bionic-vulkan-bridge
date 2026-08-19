#!/data/data/com.termux/files/usr/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BVB_VISIBLE_GATE=E023 \
BVB_VISIBLE_FRAMES=64 \
BVB_VISIBLE_RING_SLOTS=4 \
    exec "$project_dir/scripts/test-brokered-visible-triangle-termux.sh"
