#!/data/data/com.termux/files/usr/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="$project_dir/build"
out_dir="$project_dir/out"
binary="$build_dir/bvb-vulkan-external-memory-selftest"
stdout_path="$out_dir/e035-external-memory.stdout"
stderr_path="$out_dir/e035-external-memory.stderr"
evidence="$out_dir/e035-external-memory.json"

for command_name in cmake git python readelf sha256sum; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        printf 'missing required command: %s\n' "$command_name" >&2
        exit 2
    fi
done

mkdir -p "$out_dir"
cmake -S "$project_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build "$build_dir" --parallel --target \
    bvb-vulkan-external-memory-selftest
if ! readelf -l "$binary" | grep -Fq '/system/bin/linker64'; then
    printf 'external-memory selftest is not Android Bionic\n' >&2
    exit 3
fi

"$binary" >"$stdout_path" 2>"$stderr_path"
if [ -s "$stderr_path" ]; then
    printf 'E035 emitted unexpected stderr\n' >&2
    exit 4
fi

source_commit=$(git -C "$project_dir" rev-parse HEAD)
python - "$stdout_path" "$stderr_path" "$binary" "$source_commit" \
    "$evidence" <<'PY'
import hashlib
import json
import pathlib
import sys

stdout_path, stderr_path, binary_path = map(pathlib.Path, sys.argv[1:4])
source_commit = sys.argv[4]
evidence_path = pathlib.Path(sys.argv[5])


def artifact(path):
    content = path.read_bytes()
    return {
        "path": str(path),
        "bytes": len(content),
        "sha256": hashlib.sha256(content).hexdigest(),
    }


result = json.loads(stdout_path.read_text())
assert result["schema_version"] == 1
assert result["gate"] == "E035"
assert result["loader_path"] == "/system/lib64/libvulkan.so"
assert result["handle_type"] == "opaque_fd"
assert result["logical_device_count"] == 2
assert result["external_memory_features"] & 0x2
assert result["external_memory_features"] & 0x4
assert result["compatible_handle_types"] & 0x1
assert result["export_from_imported_handle_types"] & 0x1
assert result["memory_property_flags"] & 0x2
assert result["buffer_bytes"] == 4096
assert result["mismatched_bytes"] == 0
assert stderr_path.stat().st_size == 0

document = {
    "schema_version": 1,
    "gate": "E035",
    "result": "pass",
    "source_commit": source_commit,
    "target": "Galaxy Tab S8+ Android Bionic Adreno 730",
    "vulkan_loader": "/system/lib64/libvulkan.so",
    "bionic_interpreter": "/system/bin/linker64",
    "external_memory": result,
    "fd_ownership": {
        "export_transfers_to_application": True,
        "successful_import_transfers_to_destination_driver": True,
        "source_and_destination_logical_devices_distinct": True,
    },
    "stderr_bytes": 0,
    "artifacts": {
        "bionic_external_memory_selftest": artifact(binary_path),
    },
}
evidence_path.write_text(json.dumps(document, indent=2) + "\n")
print(json.dumps(document, indent=2))
print("e035_external_memory=PASS")
PY

printf 'evidence=%s\n' "$evidence"
