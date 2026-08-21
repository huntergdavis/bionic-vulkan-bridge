#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$script_dir/.." && pwd)
cache_root=${BVB_TURNIP_CACHE_ROOT:-/home/hunter/.cache/bvb-android-turnip}
build_root=${BVB_TURNIP_BUILD_ROOT:-$repo_root/build/private-turnip-maintenance56}
mesa_version=26.2.0
mesa_archive=$cache_root/mesa-$mesa_version.tar.xz
mesa_archive_sha256=efd4bb08cdb7c365a812cd4e6c9202ab55b2f22cdcd13c7d6c4f9647b799a4ef
ndk_root=$cache_root/android-ndk-r29
ndk_archive=$cache_root/android-ndk-r29-linux.zip
ndk_archive_sha1=87e2bb7e9be5d6a1c6cdf5ec40dd4e0c6d07c30b
source_dir=$build_root/mesa-$mesa_version
meson_build=$build_root/build-android-aarch64
artifact_dir=$build_root/artifacts
cross_file=$build_root/android-aarch64.ini
meson=$cache_root/venv/bin/meson
ninja_bin=${NINJA:-ninja}

# Mesa's configure-time generators require Mako, packaging, and PyYAML. Keep
# the pinned cache virtualenv first so Meson does not select a module-less
# system Python merely because it has the newest version number.
export PATH="$cache_root/venv/bin:$PATH"

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

require_file() {
    [ -f "$1" ] || fail "required file is missing: $1"
}

require_file "$mesa_archive"
require_file "$ndk_root/source.properties"
require_file "$meson"
require_file "$repo_root/patches/mesa-$mesa_version/0001-export-private-icd-resolver.patch"
require_file "$repo_root/patches/mesa-$mesa_version/0002-private-api34-maintenance56.patch"
require_file "$repo_root/tools/private_turnip_maintenance56_probe.c"
require_file "$repo_root/scripts/check-private-turnip-maintenance56.py"
command -v "$ninja_bin" >/dev/null || fail "ninja is not installed"

actual_archive_sha256=$(sha256sum "$mesa_archive" | awk '{print $1}')
[ "$actual_archive_sha256" = "$mesa_archive_sha256" ] ||
    fail "Mesa archive hash mismatch: $actual_archive_sha256"
grep -q '^Pkg.Revision = 29\.0\.14206865$' "$ndk_root/source.properties" ||
    fail "expected Android NDK r29 (29.0.14206865)"
if [ ! -d "$source_dir" ]; then
    require_file "$ndk_archive"
    actual_ndk_archive_sha1=$(sha1sum "$ndk_archive" | awk '{print $1}')
    [ "$actual_ndk_archive_sha1" = "$ndk_archive_sha1" ] ||
        fail "Android NDK archive hash mismatch: $actual_ndk_archive_sha1"
fi

mkdir -p "$build_root" "$artifact_dir"
if [ ! -d "$source_dir" ]; then
    tar -xf "$mesa_archive" -C "$build_root"
fi
[ -d "$source_dir/src/freedreno/vulkan" ] ||
    fail "unexpected Mesa source tree: $source_dir"

for patch_name in \
    0001-export-private-icd-resolver.patch \
    0002-private-api34-maintenance56.patch; do
    patch_path=$repo_root/patches/mesa-$mesa_version/$patch_name
    if patch --dry-run --batch --forward --ignore-whitespace -d "$source_dir" -p1 < "$patch_path" >/dev/null 2>&1; then
        patch --batch --forward --ignore-whitespace -d "$source_dir" -p1 < "$patch_path"
    elif patch --dry-run --batch --reverse --forward --ignore-whitespace -d "$source_dir" -p1 < "$patch_path" >/dev/null 2>&1; then
        printf 'Already applied: %s\n' "$patch_name"
    else
        fail "source tree is neither pristine nor correctly patched for $patch_name"
    fi
done

cross_contents=$(cat <<EOF
[constants]
ndk_path = '$ndk_root'

[binaries]
ar = ndk_path / 'toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar'
c = ['ccache', ndk_path / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android34-clang']
cpp = ['ccache', ndk_path / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android34-clang++', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '--start-no-unused-arguments', '-static-libstdc++', '--end-no-unused-arguments']
c_ld = 'lld'
cpp_ld = 'lld'
strip = ndk_path / 'toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip'

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'
EOF
)
if [ ! -f "$cross_file" ]; then
    printf '%s\n' "$cross_contents" > "$cross_file"
elif [ "$(cat "$cross_file")" != "$cross_contents" ]; then
    fail "existing cross file does not match requested NDK: $cross_file"
fi

if [ ! -f "$meson_build/build.ninja" ]; then
    "$meson" setup "$meson_build" "$source_dir" \
        --cross-file "$cross_file" \
        --buildtype release \
        -Dplatforms=android \
        -Dplatform-sdk-version=34 \
        -Dandroid-stub=true \
        -Dandroid-libbacktrace=disabled \
        -Degl=disabled \
        -Dgallium-drivers= \
        -Dvulkan-drivers=freedreno \
        -Dfreedreno-kmds=kgsl
fi

"$ninja_bin" -C "$meson_build" \
    src/freedreno/vulkan/libvulkan_freedreno.so

driver_source=$meson_build/src/freedreno/vulkan/libvulkan_freedreno.so
require_file "$driver_source"
install -m 0755 "$driver_source" "$artifact_dir/libvulkan_freedreno.so"

android_clang=$ndk_root/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android34-clang
require_file "$android_clang"
"$android_clang" \
    -std=c11 -O2 -g0 -fPIE -pie \
    -Wall -Wextra -Werror \
    -I "$source_dir/include" \
    "$repo_root/tools/private_turnip_maintenance56_probe.c" \
    -ldl \
    -o "$artifact_dir/bvb-private-turnip-maintenance56-probe"

python3 "$repo_root/scripts/check-private-turnip-maintenance56.py" \
    --source "$source_dir" \
    --generated "$meson_build/src/vulkan/util/vk_extensions.c" \
    --driver "$artifact_dir/libvulkan_freedreno.so" \
    --probe "$artifact_dir/bvb-private-turnip-maintenance56-probe"

driver_sha256=$(sha256sum "$artifact_dir/libvulkan_freedreno.so" | awk '{print $1}')
probe_sha256=$(sha256sum "$artifact_dir/bvb-private-turnip-maintenance56-probe" | awk '{print $1}')
python3 - "$artifact_dir/build-manifest.json" "$driver_sha256" "$probe_sha256" <<'PY'
import json
import pathlib
import sys

path, driver_sha256, probe_sha256 = sys.argv[1:]
manifest = {
    "schema_version": 1,
    "mesa_version": "26.2.0",
    "mesa_archive_sha256": "efd4bb08cdb7c365a812cd4e6c9202ab55b2f22cdcd13c7d6c4f9647b799a4ef",
    "android_ndk": "r29 (29.0.14206865)",
    "android_ndk_archive_sha1": "87e2bb7e9be5d6a1c6cdf5ec40dd4e0c6d07c30b",
    "android_api": 34,
    "android_strict": True,
    "private_extension_exceptions": [
        "VK_KHR_maintenance5",
        "VK_KHR_maintenance6",
    ],
    "driver_sha256": driver_sha256,
    "probe_sha256": probe_sha256,
}
pathlib.Path(path).write_text(json.dumps(manifest, indent=2) + "\n")
PY

printf 'Private Turnip artifact: %s\n' "$artifact_dir/libvulkan_freedreno.so"
printf 'Driver SHA256: %s\n' "$driver_sha256"
printf 'Native validation probe: %s\n' "$artifact_dir/bvb-private-turnip-maintenance56-probe"
printf 'Probe SHA256: %s\n' "$probe_sha256"
