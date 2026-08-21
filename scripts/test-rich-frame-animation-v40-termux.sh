#!/usr/bin/env bash
set -euo pipefail

project_dir="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)"
staged_apk=${BVB_V40_STAGED_APK:-/sdcard/Download/bvb-visible-host-v40-d724b847.apk}
service=${BVB_E077_SERVICE:-$project_dir/build/bvb-bridge-service}
client=${BVB_E077_CLIENT:-$project_dir/out/triangle-dispatch-glibc/bvb-global-dispatch-test-glibc}
bridge_icd=${BVB_E077_ICD:-$project_dir/out/triangle-dispatch-glibc/libvulkan-bvb-glibc.so}

exec python3 "$project_dir/scripts/test-activity-frame-import-v40-termux.py" \
  --animated-rgbw \
  --staged-apk "$staged_apk" \
  --service "$service" \
  --client "$client" \
  --bridge-icd "$bridge_icd" \
  --skip-build \
  "$@" \
  --expected-service-sha256 214e8b112ade7a727af6748c8e4cd029f4f273ca38fef018613ca6481773a9a8 \
  --expected-client-sha256 50a2589e2b166e8e3b796eda7839fb49c78a23a8506f357d553710fd44819024 \
  --expected-icd-sha256 e6479b4abb15cca258a44d72a674c93900ea716e7030a1643409e8bcc7049d2f
