#!/data/data/com.termux/files/usr/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out_dir="$project_dir/out/visible-host"
staging_dir="$out_dir/staging"
native_dir="$staging_dir/lib/arm64-v8a"
manifest="$project_dir/android/visible-host/AndroidManifest.xml"
source_file="$project_dir/android/visible-host/native_main.c"
vulkan_headers="$project_dir/build/_deps/vulkanheaders-src/include"
framework_resources=/system/framework/framework-res.apk
unsigned_apk="$out_dir/bvb-visible-host-unsigned.apk"
aligned_apk="$out_dir/bvb-visible-host-aligned.apk"
signed_apk="$out_dir/bvb-visible-host-debug.apk"
keystore="$out_dir/debug.keystore"

: "${PREFIX:?PREFIX must name the Termux prefix}"

for command_name in clang aapt zipalign apksigner keytool readelf; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        printf 'missing required command: %s\n' "$command_name" >&2
        exit 2
    fi
done
for required_file in "$manifest" "$source_file" "$framework_resources" \
    "$vulkan_headers/vulkan/vulkan.h" /system/lib64/libandroid.so \
    /system/lib64/liblog.so /system/lib64/libvulkan.so; do
    if [ ! -f "$required_file" ]; then
        printf 'missing required file: %s\n' "$required_file" >&2
        exit 2
    fi
done

mkdir -p "$native_dir"
clang -std=c17 -O2 -fPIC -fvisibility=hidden -Wall -Wextra -Werror \
    -shared -Wl,-soname,libbvb-visible-host.so \
    -I"$vulkan_headers" \
    "$source_file" \
    /system/lib64/libandroid.so /system/lib64/liblog.so \
    /system/lib64/libvulkan.so \
    -o "$native_dir/libbvb-visible-host.so"

aapt package -f -M "$manifest" -I "$framework_resources" -F "$unsigned_apk"
(
    cd "$staging_dir"
    aapt add "$unsigned_apk" lib/arm64-v8a/libbvb-visible-host.so >/dev/null
)
zipalign -f 4 "$unsigned_apk" "$aligned_apk"

if [ ! -f "$keystore" ]; then
    keytool -genkeypair -keystore "$keystore" -storepass android \
        -alias androiddebugkey -keypass android \
        -dname 'CN=BVB Android Debug,O=huntergdavis,C=US' \
        -keyalg RSA -keysize 2048 -validity 10000 -noprompt >/dev/null 2>&1
fi
apksigner sign --ks "$keystore" --ks-key-alias androiddebugkey \
    --ks-pass pass:android --key-pass pass:android \
    --out "$signed_apk" "$aligned_apk"
apksigner verify --verbose --print-certs "$signed_apk"

printf 'VISIBLE_HOST_APK=%s\n' "$signed_apk"
file "$native_dir/libbvb-visible-host.so"
readelf -d "$native_dir/libbvb-visible-host.so" | sed -n '/NEEDED/p'
aapt dump badging "$signed_apk" | sed -n '1,8p'
