#!/data/data/com.termux/files/usr/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out_dir="$project_dir/out/visible-host"
staging_dir="$out_dir/staging"
native_dir="$staging_dir/lib/arm64-v8a"
manifest="$project_dir/android/visible-host/AndroidManifest.xml"
source_file="$project_dir/android/visible-host/native_main.c"
java_dir="$project_dir/android/visible-host/java/io/github/huntergdavis/bvb/visiblehost"
activity_java="$java_dir/VisibleHostActivity.java"
provider_java="$java_dir/SharedRegionProvider.java"
client_java="$java_dir/SharedRegionClient.java"
receiver_java="$java_dir/SharedRegionReceiver.java"
frame_transport_client_java="$java_dir/FrameTransportClient.java"
lifecycle_source="$project_dir/src/lifecycle.c"
frame_sync_source="$project_dir/src/frame_sync.c"
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
java_classes="$out_dir/java-classes"
dex_dir="$out_dir/dex"
tool_dir="$out_dir/android-tools"
r8_version=9.4.14
r8_jar="$tool_dir/r8-$r8_version.jar"
r8_url="https://dl.google.com/dl/android/maven2/com/android/tools/r8/$r8_version/r8-$r8_version.jar"
r8_sha256=05373121003e75e7bc5bc139501913531e1821ac3890cef40732e341a37c8bad
android_jar_version=4.1.1.4
android_jar="$tool_dir/android-$android_jar_version.jar"
android_jar_url="https://repo1.maven.org/maven2/com/google/android/android/$android_jar_version/android-$android_jar_version.jar"
android_jar_sha256=84072541cbb711eff89f7277100ff854929a446dba7ceb1b195c340e0b4fd3cb
framework_resources=/system/framework/framework-res.apk
unsigned_apk="$out_dir/bvb-visible-host-unsigned.apk"
aligned_apk="$out_dir/bvb-visible-host-aligned.apk"
signed_apk="$out_dir/bvb-visible-host-debug.apk"
keystore="$out_dir/debug.keystore"

: "${PREFIX:?PREFIX must name the Termux prefix}"

for command_name in clang aapt zipalign apksigner keytool readelf \
    glslangValidator python curl javac java sha256sum; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        printf 'missing required command: %s\n' "$command_name" >&2
        exit 2
    fi
done
for required_file in "$manifest" "$source_file" "$activity_java" \
    "$provider_java" \
    "$client_java" "$receiver_java" "$frame_transport_client_java" \
    "$lifecycle_source" \
    "$protocol_source" "$handle_source" "$batch_source" \
    "$transport_source" "$visible_batch_source" "$visible_ingress_source" \
    "$vertex_shader" "$fragment_shader" \
    "$project_dir/include/bvb/lifecycle.h" \
    "$project_dir/include/bvb/activity_frame_transport.h" \
    "$project_dir/include/bvb/wsi_frame_ring.h" \
    "$project_dir/include/bvb/command_batch.h" "$framework_resources" \
    "$vulkan_headers/vulkan/vulkan.h" /system/lib64/libandroid.so \
    /system/lib64/liblog.so /system/lib64/libvulkan.so \
    /system/lib64/libbinder_ndk.so \
    "$PREFIX/include/android/binder_ibinder.h" \
    "$PREFIX/include/android/binder_parcel.h"; do
    if [ ! -f "$required_file" ]; then
        printf 'missing required file: %s\n' "$required_file" >&2
        exit 2
    fi
done

fetch_pinned() {
    url=$1
    expected_sha256=$2
    output=$3
    if [ -f "$output" ]; then
        actual_sha256=$(sha256sum "$output" | sed 's/ .*//')
        if [ "$actual_sha256" = "$expected_sha256" ]; then
            return 0
        fi
        printf 'cached build tool has wrong SHA-256: %s\n' "$output" >&2
        return 3
    fi
    temporary="$output.part.$$"
    if ! curl -fsSL "$url" -o "$temporary"; then
        rm -f "$temporary"
        return 3
    fi
    actual_sha256=$(sha256sum "$temporary" | sed 's/ .*//')
    if [ "$actual_sha256" != "$expected_sha256" ]; then
        printf 'downloaded build tool has wrong SHA-256: %s\n' "$url" >&2
        rm -f "$temporary"
        return 3
    fi
    mv "$temporary" "$output"
}

mkdir -p "$native_dir"
mkdir -p "$shader_dir"
mkdir -p "$java_classes"
mkdir -p "$dex_dir"
mkdir -p "$tool_dir"
fetch_pinned "$r8_url" "$r8_sha256" "$r8_jar"
fetch_pinned "$android_jar_url" "$android_jar_sha256" "$android_jar"

javac --release 8 -classpath "$android_jar" -d "$java_classes" \
    "$activity_java" "$provider_java" "$client_java" "$receiver_java" \
    "$frame_transport_client_java"
rm -f "$dex_dir/classes.dex"
java -cp "$r8_jar" com.android.tools.r8.D8 \
    --release --no-desugaring --min-api 24 --lib "$android_jar" \
    --output "$dex_dir" \
    "$java_classes/io/github/huntergdavis/bvb/visiblehost/VisibleHostActivity.class" \
    "$java_classes/io/github/huntergdavis/bvb/visiblehost/SharedRegionProvider.class" \
    "$java_classes/io/github/huntergdavis/bvb/visiblehost/SharedRegionProvider\$ExternalMemoryResult.class" \
    "$java_classes/io/github/huntergdavis/bvb/visiblehost/SharedRegionClient.class" \
    "$java_classes/io/github/huntergdavis/bvb/visiblehost/SharedRegionReceiver.class" \
    "$java_classes/io/github/huntergdavis/bvb/visiblehost/FrameTransportClient.class"
if [ ! -f "$dex_dir/classes.dex" ]; then
    printf 'D8 did not produce classes.dex\n' >&2
    exit 4
fi

glslangValidator -V --target-env vulkan1.1 -S vert \
    -o "$shader_dir/triangle.vert.spv" "$vertex_shader" >/dev/null
glslangValidator -V --target-env vulkan1.1 -S frag \
    -o "$shader_dir/triangle.frag.spv" "$fragment_shader" >/dev/null
python "$project_dir/scripts/embed-spirv.py" \
    "$shader_dir/triangle.vert.spv" "$shader_dir/triangle.frag.spv" \
    "$shader_include"

clang --target=aarch64-linux-android29 \
    -std=c17 -O3 -DNDEBUG -fPIC -fvisibility=hidden \
    -Wall -Wextra -Werror -pthread \
    -shared -Wl,-soname,libbvb-visible-host.so \
    -I"$project_dir/include" -I"$vulkan_headers" -I"$shader_dir" \
    "$source_file" "$lifecycle_source" "$frame_sync_source" \
    "$project_dir/src/wsi_frame_ring.c" \
    "$project_dir/src/activity_frame_transport.c" \
    "$protocol_source" \
    "$handle_source" "$batch_source" "$transport_source" \
    "$visible_batch_source" "$visible_ingress_source" \
    /system/lib64/libandroid.so /system/lib64/liblog.so \
    /system/lib64/libvulkan.so /system/lib64/libbinder_ndk.so \
    -o "$native_dir/libbvb-visible-host.so"

aapt package -f -M "$manifest" -I "$framework_resources" -F "$unsigned_apk"
(
    cd "$staging_dir"
    aapt add "$unsigned_apk" lib/arm64-v8a/libbvb-visible-host.so >/dev/null
)
(
    cd "$dex_dir"
    aapt add "$unsigned_apk" classes.dex >/dev/null
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
if ! aapt list "$signed_apk" | grep -qx 'classes.dex'; then
    printf 'visible host APK is missing classes.dex\n' >&2
    exit 4
fi
