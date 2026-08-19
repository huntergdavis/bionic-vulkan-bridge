#!/data/data/com.termux/files/usr/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
frames=${BVB_ROTATING_TRIANGLE_FRAMES:-4096}
BVB_VISIBLE_GATE=E033 \
BVB_VISIBLE_FRAMES="$frames" \
BVB_VISIBLE_RING_SLOTS=4 \
    exec "$project_dir/scripts/test-brokered-visible-triangle-termux.sh"
