#!/usr/bin/env bash
set -euo pipefail

project_dir="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)"
exec python3 "$project_dir/scripts/test-activity-frame-import-v40-termux.py" \
  --animated-rgbw "$@"
