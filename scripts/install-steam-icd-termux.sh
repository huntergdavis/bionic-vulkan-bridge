#!/data/data/com.termux/files/usr/bin/bash

set -euo pipefail
umask 077

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
steam_base=${STEAM_ARM64_BASE:-$HOME/steam-arm64}
source_dir=${BVB_GLIBC_OUTPUT_DIR:-$project_dir/out/triangle-dispatch-glibc}
source_library=$source_dir/libvulkan-bvb-glibc.so
source_service=${BVB_SERVICE_BINARY:-$project_dir/build/bvb-bridge-service}
install_root=$steam_base/bvb
library_dir=$install_root/lib
binary_dir=$install_root/bin
manifest_dir=$install_root/icd.d
library=$library_dir/libvulkan-bvb-glibc.so
service=$binary_dir/bvb-bridge-service
manifest=$manifest_dir/bvb_icd.aarch64.json
stamp=$install_root/install.sha256

fail() {
    printf 'install-steam-icd-termux: %s\n' "$*" >&2
    exit 1
}

[[ $steam_base == /* && -d $steam_base && ! -L $steam_base ]] ||
    fail "Steam base is unavailable or unsafe: $steam_base"
[[ -f $source_library && ! -L $source_library ]] ||
    fail "glibc ICD is unavailable or unsafe: $source_library"
[[ -x $source_service && ! -L $source_service ]] ||
    fail "Bionic service is unavailable or unsafe: $source_service"
readelf -h "$source_library" | grep -Fq 'AArch64' ||
    fail 'glibc ICD is not AArch64'
readelf -l "$source_library" | grep -Fq '/lib/ld-linux-aarch64.so.1' &&
    fail 'glibc ICD unexpectedly has a program interpreter'
readelf -l "$source_service" | grep -Fq '/system/bin/linker64' ||
    fail 'bridge service is not Android Bionic'

mkdir -p "$library_dir" "$binary_dir" "$manifest_dir"
[[ ! -L $install_root && ! -L $library_dir && ! -L $binary_dir &&
    ! -L $manifest_dir ]] || fail 'refusing symlinked install directory'
install -m 700 "$source_library" "$library.tmp"
install -m 700 "$source_service" "$service.tmp"
printf '%s\n' \
    '{' \
    '  "file_format_version": "1.0.1",' \
    '  "ICD": {' \
    "    \"library_path\": \"$library\"," \
    '    "api_version": "1.1.0",' \
    '    "library_arch": "64"' \
    '  }' \
    '}' >"$manifest.tmp"
mv -f -- "$library.tmp" "$library"
mv -f -- "$service.tmp" "$service"
mv -f -- "$manifest.tmp" "$manifest"
{
    sha256sum "$library" "$service" "$manifest"
} >"$stamp.tmp"
mv -f -- "$stamp.tmp" "$stamp"

printf 'BVB Steam ICD installed: manifest=%s service=%s\n' \
    "$manifest" "$service"
