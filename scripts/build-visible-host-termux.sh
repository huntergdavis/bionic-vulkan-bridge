#!/data/data/com.termux/files/usr/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out_dir="$project_dir/out/visible-host"
staging_dir="$out_dir/staging"
native_dir="$staging_dir/lib/arm64-v8a"
manifest="$project_dir/android/visible-host/AndroidManifest.xml"
source_file="$project_dir/android/visible-host/native_main.c"
lifecycle_source="$project_dir/src/lifecycle.c"
protocol_source="$project_dir/src/protocol.c"
handle_source="$project_dir/src/handle.c"
batch_source="$project_dir/src/command_batch.c"
transport_source="$project_dir/src/transport.c"
visible_batch_source="$project_dir/src/visible_batch.c"
visible_ingress_source="$project_dir/src/visible_ingress.c"
vulkan_headers="$project_dir/build/_deps/vulkanheaders-src/include"
vertex_shader="$project_dir/android/visible-host/shaders/triangle.vert"
fragment_shader="$project_dir/android/visible-host/shaders/triangle.frag"
shader_dir="$out_dir/shaders"
shader_include="$shader_dir/triangle_shaders.inc"
framework_resources=/system/framework/framework-res.apk
unsigned_apk="$out_dir/bvb-visible-host-unsigned.apk"
aligned_apk="$out_dir/bvb-visible-host-aligned.apk"
signed_apk="$out_dir/bvb-visible-host-debug.apk"
keystore="$out_dir/debug.keystore"

: "${PREFIX:?PREFIX must name the Termux prefix}"

for command_name in clang aapt zipalign apksigner keytool readelf \
    glslangValidator python; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        printf 'missing required command: %s\n' "$command_name" >&2
        exit 2
    fi
done
for required_file in "$manifest" "$source_file" "$lifecycle_source" \
    "$protocol_source" "$handle_source" "$batch_source" \
    "$transport_source" "$visible_batch_source" "$visible_ingress_source" \
    "$vertex_shader" "$fragment_shader" \
    "$project_dir/include/bvb/lifecycle.h" \
    "$project_dir/include/bvb/command_batch.h" "$framework_resources" \
    "$vulkan_headers/vulkan/vulkan.h" /system/lib64/libandroid.so \
    /system/lib64/liblog.so /system/lib64/libvulkan.so; do
    if [ ! -f "$required_file" ]; then
        printf 'missing required file: %s\n' "$required_file" >&2
        exit 2
    fi
done

mkdir -p "$native_dir"
mkdir -p "$shader_dir"
glslangValidator -V --target-env vulkan1.1 -S vert \
    -o "$shader_dir/triangle.vert.spv" "$vertex_shader" >/dev/null
glslangValidator -V --target-env vulkan1.1 -S frag \
    -o "$shader_dir/triangle.frag.spv" "$fragment_shader" >/dev/null
python "$project_dir/scripts/embed-spirv.py" \
    "$shader_dir/triangle.vert.spv" "$shader_dir/triangle.frag.spv" \
    "$shader_include"

clang -std=c17 -O3 -DNDEBUG -fPIC -fvisibility=hidden \
    -Wall -Wextra -Werror -pthread \
    -shared -Wl,-soname,libbvb-visible-host.so \
    -I"$project_dir/include" -I"$vulkan_headers" -I"$shader_dir" \
    "$source_file" "$lifecycle_source" "$protocol_source" \
    "$handle_source" "$batch_source" "$transport_source" \
    "$visible_batch_source" "$visible_ingress_source" \
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
