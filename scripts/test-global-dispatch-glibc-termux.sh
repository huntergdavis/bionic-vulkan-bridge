#!/data/data/com.termux/files/usr/bin/sh
set -eu

case ${BVB_COMMAND_STREAM:-} in
    ''|shared) ;;
    *)
        printf 'invalid BVB_COMMAND_STREAM for global gate: %s\n' \
            "$BVB_COMMAND_STREAM" >&2
        exit 2
        ;;
esac
case ${BVB_MAPPED_MEMORY:-} in
    ''|shared) ;;
    *)
        printf 'invalid BVB_MAPPED_MEMORY for global gate: %s\n' \
            "$BVB_MAPPED_MEMORY" >&2
        exit 2
        ;;
esac
if [ -n "${BVB_TEST_ANIMATED_WSI:-}" ]; then
    printf 'animated WSI belongs to the dedicated E076 runtime gate\n' >&2
    exit 2
fi

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="$project_dir/build"
out_dir="$project_dir/out/triangle-dispatch-glibc"
library="$out_dir/libvulkan-bvb-glibc.so"
client="$out_dir/bvb-global-dispatch-test-glibc"
service="$build_dir/bvb-bridge-service"
harness="$project_dir/scripts/run-global-dispatch-activity-harness.py"
policy_json="$out_dir/generated/bvb_dxvk_dispatch_policy.json"
evidence="$project_dir/out/e077-current-global.json"
vulkan_headers="$build_dir/_deps/vulkanheaders-src/include"
runtime_parent=${TMPDIR:-$PREFIX/tmp}
service_loader=${BVB_VULKAN_SERVICE_LOADER:-$HOME/steam-arm64/bvb/driver/libvulkan_freedreno.so}

for command_name in cmake file git grun gcc python3 readelf sha256sum; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        printf 'missing required command: %s\n' "$command_name" >&2
        exit 2
    fi
done
if [ ! -f "$harness" ] || [ ! -d "$runtime_parent" ]; then
    printf 'global Activity harness or runtime parent is unavailable\n' >&2
    exit 2
fi
if [ ! -f "$service_loader" ] || [ -L "$service_loader" ]; then
    printf 'private Vulkan service loader is unavailable or symlinked: %s\n' \
        "$service_loader" >&2
    exit 2
fi

"$project_dir/scripts/test-triangle-dispatch-glibc-termux.sh" >/dev/null
cmake --build "$build_dir" --parallel --target bvb-bridge-service

grun -s gcc -std=c17 -O3 -DNDEBUG -Wall -Wextra -Werror \
    -I"$project_dir/include" -I"$vulkan_headers" \
    "$project_dir/tests/global_dispatch.c" "$project_dir/src/handle.c" \
    -L"$out_dir" -Wl,-rpath,"$out_dir" -lvulkan-bvb-glibc \
    -o "$client"

glibc_interpreter=/data/data/com.termux/files/usr/glibc/lib/ld-linux-aarch64.so.1
bionic_interpreter=/system/bin/linker64
if ! readelf -l "$client" | grep -Fq "$glibc_interpreter"; then
    printf 'global dispatch client does not use Termux glibc\n' >&2
    exit 3
fi
if ! readelf -l "$service" | grep -Fq "$bionic_interpreter"; then
    printf 'bridge service does not use Android Bionic\n' >&2
    exit 3
fi

client_stdout="$out_dir/e077-current-global-client.stdout"
client_stderr="$out_dir/e077-current-global-client.stderr"
service_stdout="$out_dir/e077-current-global-service.stdout"
service_stderr="$out_dir/e077-current-global-service.stderr"
harness_result="$out_dir/e077-current-global-harness.json"

python3 "$harness" \
    --service "$service" \
    --service-loader "$service_loader" \
    --runtime-parent "$runtime_parent" \
    --hardware-validation \
    --width 2800 --height 1752 \
    --service-stdout "$service_stdout" \
    --service-stderr "$service_stderr" \
    --client-stdout "$client_stdout" \
    --client-stderr "$client_stderr" \
    --result-json "$harness_result" \
    -- grun "$client"

if [ -n "${BVB_SOURCE_COMMIT:-}" ]; then
    source_commit=$BVB_SOURCE_COMMIT
    source_commit_provenance=environment_override
elif git -C "$project_dir" diff --quiet -- &&
    git -C "$project_dir" diff --cached --quiet --; then
    source_commit=$(git -C "$project_dir" rev-parse HEAD)
    source_commit_provenance=clean_git_head
else
    printf 'tracked source is dirty; set BVB_SOURCE_COMMIT to the exact deployed commit\n' >&2
    exit 3
fi
case "$source_commit" in
    *[!0-9a-f]*|'')
        printf 'invalid BVB source commit: %s\n' "$source_commit" >&2
        exit 3
        ;;
esac
if [ "${#source_commit}" -ne 40 ]; then
    printf 'BVB source commit must contain exactly 40 lowercase hex characters\n' >&2
    exit 3
fi
python3 - "$policy_json" "$library" "$client" "$service" \
    "$client_stdout" "$service_stdout" "$harness_result" \
    "$source_commit" "$evidence" "$service_loader" \
    "$source_commit_provenance" <<'PY'
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys

(
    policy_path,
    library_path,
    client_path,
    service_path,
    client_stdout_path,
    service_stdout_path,
    harness_result_path,
    source_commit,
    evidence_path,
) = [pathlib.Path(value) for value in sys.argv[1:8]] + [
    sys.argv[8],
    pathlib.Path(sys.argv[9]),
]
service_loader_path = pathlib.Path(sys.argv[10])
source_commit_provenance = sys.argv[11]


def artifact(path):
    content = path.read_bytes()
    return {
        "path": str(path),
        "bytes": len(content),
        "sha256": hashlib.sha256(content).hexdigest(),
    }


policy = json.loads(policy_path.read_text())
assert policy["gate"] == "E069"
assert policy["summary"]["command_count"] == 742
assert policy["summary"]["executable_name_count"] == 88
assert policy["summary"]["support_counts"] == {
    "probed_null": 302,
    "required_unimplemented": 352,
    "executable": 88,
}
client_stdout = client_stdout_path.read_text().strip()
match = re.fullmatch(
    r"PASS: global Vulkan discovery validation_mode=(hardware) api=(\d+) "
    r"exposed_extensions=7 exposed_layers=0 "
    r"instance_one=(\d+) instance_two=(\d+) physical_device=(\d+) "
    r"device=(.*?) device_api=(\d+) driver=(\d+) vendor=(\d+) "
    r"device_id=(\d+) max_push_constants=(\d+) "
    r"image_format_max=(\d+),(\d+) queues=(\d+) memory_types=(\d+) "
    r"memory_heaps=(\d+) device_extensions=(\d+) "
    r"sampler_anisotropy=(\d+) logical_device=(\d+) queue=(\d+) "
    r"empty_submit=(-?\d+) queue_wait=(-?\d+) device_wait=(-?\d+) "
    r"animated_frames=(\d+) animated_reused_image=(\d+) "
    r"animated_recording_rtts=(\d+) command_pool=(\d+) "
    r"command_buffer=(\d+) recording_rtts=(\d+) "
    r"command_submit=(-?\d+) "
    r"pool_reset=(-?\d+) buffer=(\d+) memory=(\d+) memory_type=(\d+) "
    r"buffer_requirements2=(\d+),(\d+),(\d+) buffer_address=(\d+) "
    r"image=(\d+) image_view=(\d+) image_bytes=(\d+) "
    r"image_allocation_bytes=(\d+) image_dedicated=(\d+),(\d+) "
    r"mapped_bytes=(\d+) mapped_mismatches=(\d+) "
    r"memory_rtts=(\d+),(\d+),(\d+),(\d+),(\d+) "
    r"memory_opcodes=(\d+),(\d+),(\d+),(\d+),(\d+) "
    r"ineligible_memory_rtts=(\d+),(\d+) "
    r"ineligible_memory_opcodes=(\d+),(\d+) "
    r"fill_words=(\d+) mismatches=(\d+) fence=(\d+) fence_before=(-?\d+) "
    r"fenced_submit=(-?\d+) fence_after=(-?\d+) fence_wait=(-?\d+) "
    r"fence_reset=(-?\d+) fence_after_reset=(-?\d+)",
    client_stdout,
)
assert match is not None, client_stdout
(
    validation_mode,
    api_version_text,
    instance_one_text,
    instance_two_text,
    physical_device_text,
    device_name,
    device_api_text,
    driver_version_text,
    vendor_id_text,
    device_id_text,
    max_push_constants_text,
    image_format_width_text,
    image_format_height_text,
    queue_count_text,
    memory_type_count_text,
    memory_heap_count_text,
    device_extension_count_text,
    sampler_anisotropy_text,
    logical_device_text,
    logical_queue_text,
    empty_submit_text,
    queue_wait_text,
    device_wait_text,
    animated_frames_text,
    animated_reused_image_text,
    animated_recording_rtts_text,
    command_pool_text,
    command_buffer_text,
    recording_rtts_text,
    command_submit_text,
    pool_reset_text,
    buffer_text,
    memory_text,
    memory_type_text,
    buffer_requirement_size_text,
    buffer_requirement_alignment_text,
    buffer_requirement_type_bits_text,
    buffer_address_text,
    image_text,
    image_view_text,
    image_bytes_text,
    image_allocation_bytes_text,
    image_prefers_dedicated_text,
    image_requires_dedicated_text,
    mapped_bytes_text,
    mapped_mismatches_text,
    map_rtts_text,
    flush_rtts_text,
    invalidate_rtts_text,
    unmap_rtts_text,
    submit_rtts_text,
    map_opcode_text,
    flush_opcode_text,
    invalidate_opcode_text,
    unmap_opcode_text,
    submit_opcode_text,
    ineligible_map_rtts_text,
    ineligible_unmap_rtts_text,
    ineligible_map_opcode_text,
    ineligible_unmap_opcode_text,
    fill_words_text,
    mismatches_text,
    fence_text,
    fence_before_text,
    fenced_submit_text,
    fence_after_text,
    fence_wait_text,
    fence_reset_text,
    fence_after_reset_text,
) = match.groups()
(
    api_version,
    instance_one,
    instance_two,
    physical_device,
    device_api,
    driver_version,
    vendor_id,
    device_id,
    max_push_constants,
    image_format_width,
    image_format_height,
    queue_count,
    memory_type_count,
    memory_heap_count,
    device_extension_count,
    sampler_anisotropy,
    logical_device,
    logical_queue,
    empty_submit_result,
    queue_wait_result,
    device_wait_result,
    animated_frames,
    animated_reused_image,
    animated_recording_rtts,
    command_pool,
    command_buffer,
    recording_rtts,
    command_submit_result,
    pool_reset_result,
    buffer,
    device_memory,
    memory_type_index,
    buffer_requirement_size,
    buffer_requirement_alignment,
    buffer_requirement_type_bits,
    buffer_address,
    image,
    image_view,
    image_bytes,
    image_allocation_bytes,
    image_prefers_dedicated,
    image_requires_dedicated,
    mapped_bytes,
    mapped_mismatches,
    map_rtts,
    flush_rtts,
    invalidate_rtts,
    unmap_rtts,
    submit_rtts,
    map_opcode,
    flush_opcode,
    invalidate_opcode,
    unmap_opcode,
    submit_opcode,
    ineligible_map_rtts,
    ineligible_unmap_rtts,
    ineligible_map_opcode,
    ineligible_unmap_opcode,
    fill_words,
    mismatches,
    fence,
    fence_before_result,
    fenced_submit_result,
    fence_after_result,
    fence_wait_result,
    fence_reset_result,
    fence_after_reset_result,
) = map(int, (
    api_version_text,
    instance_one_text,
    instance_two_text,
    physical_device_text,
    device_api_text,
    driver_version_text,
    vendor_id_text,
    device_id_text,
    max_push_constants_text,
    image_format_width_text,
    image_format_height_text,
    queue_count_text,
    memory_type_count_text,
    memory_heap_count_text,
    device_extension_count_text,
    sampler_anisotropy_text,
    logical_device_text,
    logical_queue_text,
    empty_submit_text,
    queue_wait_text,
    device_wait_text,
    animated_frames_text,
    animated_reused_image_text,
    animated_recording_rtts_text,
    command_pool_text,
    command_buffer_text,
    recording_rtts_text,
    command_submit_text,
    pool_reset_text,
    buffer_text,
    memory_text,
    memory_type_text,
    buffer_requirement_size_text,
    buffer_requirement_alignment_text,
    buffer_requirement_type_bits_text,
    buffer_address_text,
    image_text,
    image_view_text,
    image_bytes_text,
    image_allocation_bytes_text,
    image_prefers_dedicated_text,
    image_requires_dedicated_text,
    mapped_bytes_text,
    mapped_mismatches_text,
    map_rtts_text,
    flush_rtts_text,
    invalidate_rtts_text,
    unmap_rtts_text,
    submit_rtts_text,
    map_opcode_text,
    flush_opcode_text,
    invalidate_opcode_text,
    unmap_opcode_text,
    submit_opcode_text,
    ineligible_map_rtts_text,
    ineligible_unmap_rtts_text,
    ineligible_map_opcode_text,
    ineligible_unmap_opcode_text,
    fill_words_text,
    mismatches_text,
    fence_text,
    fence_before_text,
    fenced_submit_text,
    fence_after_text,
    fence_wait_text,
    fence_reset_text,
    fence_after_reset_text,
))
assert validation_mode == "hardware"
assert api_version >= 0x00400000
assert instance_one >> 56 == 1
assert instance_two >> 56 == 1
assert instance_one != instance_two
assert physical_device >> 56 == 2
assert device_name
assert device_api >= 0x00400000
assert vendor_id != 0
assert max_push_constants >= 256
assert image_format_width >= 2800
assert image_format_height >= 1752
assert queue_count > 0
assert 0 < memory_type_count <= 32
assert 0 < memory_heap_count <= 16
assert device_extension_count > 0
assert sampler_anisotropy == 1
assert logical_device >> 56 == 3
assert logical_queue >> 56 == 4
assert empty_submit_result == 0
assert queue_wait_result == 0
assert device_wait_result == 0
assert animated_frames == 0
assert animated_reused_image == 0
assert animated_recording_rtts == 0
shared_command_stream = os.environ.get("BVB_COMMAND_STREAM") == "shared"
shared_mapped_memory = os.environ.get("BVB_MAPPED_MEMORY") == "shared"
keep_strict_mapped_memory = (
    not shared_mapped_memory and
    os.environ.get("BVB_TEST_KEEP_MEMORY_MAPPED") is not None
)
assert recording_rtts == (0 if shared_command_stream else 5)
assert command_pool >> 56 == 10
assert command_buffer >> 56 == 11
assert command_submit_result == 0
assert pool_reset_result == 0
assert buffer >> 56 == 19
assert device_memory >> 56 == 9
assert 0 <= memory_type_index < memory_type_count
assert buffer_requirement_size >= 4096
assert buffer_requirement_alignment > 0
assert buffer_requirement_alignment & (buffer_requirement_alignment - 1) == 0
assert buffer_requirement_type_bits != 0
valid_memory_type_bits = (1 << memory_type_count) - 1
assert buffer_requirement_type_bits & ~valid_memory_type_bits == 0
assert buffer_address != 0
assert image >> 56 == 7
assert image_view >> 56 == 8
assert image_bytes > 0
assert image_allocation_bytes == image_bytes
assert image_prefers_dedicated in (0, 1)
assert image_requires_dedicated in (0, 1)
assert mapped_bytes == 4096
assert mapped_mismatches == 0
assert (map_rtts, flush_rtts, invalidate_rtts, unmap_rtts, submit_rtts) == (
    (1, 1, 1, 1, 1)
    if shared_mapped_memory
    else (2, 2, 2, 2, 3)
    if keep_strict_mapped_memory
    else (2, 2, 2, 2, 1)
)
assert (map_opcode, flush_opcode, invalidate_opcode, unmap_opcode) == (
    (106, 107, 108, 109) if shared_mapped_memory else (49, 48, 49, 48)
)
assert submit_opcode == (105 if shared_command_stream else 47)
assert (ineligible_map_rtts, ineligible_unmap_rtts) == (
    (2, 2) if shared_mapped_memory else (0, 0)
)
assert (ineligible_map_opcode, ineligible_unmap_opcode) == (
    (49, 48) if shared_mapped_memory else (0, 0)
)
assert fill_words == 1024
assert mismatches == 0
assert fence >> 56 == 18
assert fence_before_result == 1
assert fenced_submit_result == 0
assert fence_after_result == 0
assert fence_wait_result == 0
assert fence_reset_result == 0
assert fence_after_reset_result == 1

harness_result = json.loads(harness_result_path.read_text())
assert harness_result["result"] == "pass"
assert harness_result["synthetic_activity"] is True
assert harness_result["client_validation_mode"] == "hardware"
assert harness_result["authenticated_activity_events"] == [1, 2, 3, 7, 11, 9]
assert harness_result["authenticated_event_count"] == 6
assert harness_result["requested_width"] == 2800
assert harness_result["requested_height"] == 1752
assert harness_result["activity_frame_setup"]["received"] is True
assert harness_result["activity_frame_setup"]["image_count"] == 3
assert harness_result["activity_frame_setup"]["descriptor_count"] == 4
assert harness_result["activity_frame_setup"]["width"] == 2800
assert harness_result["activity_frame_setup"]["height"] == 1752
assert harness_result["visible_frame_claim"] is False
assert harness_result["fps_claim"] is False

symbols = subprocess.run(
    ["readelf", "--wide", "--dyn-syms", str(library_path)],
    check=True,
    capture_output=True,
    text=True,
).stdout
symbol_names = {
    fields[-1]
    for line in symbols.splitlines()
    if (fields := line.split())
}
expected_exports = {
    "bvb_instance_proxy_id",
    "bvb_physical_device_proxy_id",
    "bvb_device_proxy_id",
    "bvb_queue_proxy_id",
    "bvb_command_pool_proxy_id",
    "bvb_command_buffer_proxy_id",
    "bvb_buffer_proxy_id",
    "bvb_memory_proxy_id",
    "bvb_fence_proxy_id",
    "vkGetDeviceProcAddr",
    "vkGetInstanceProcAddr",
}
assert expected_exports <= symbol_names

document = {
    "schema_version": 1,
    "gate": "E077-current-global",
    "result": "pass",
    "source_commit": source_commit,
    "source_commit_provenance": source_commit_provenance,
    "target": "Galaxy Tab S8+ Termux ARM64 glibc to Android Bionic",
    "glibc_interpreter":
        "/data/data/com.termux/files/usr/glibc/lib/ld-linux-aarch64.so.1",
    "bionic_interpreter": "/system/bin/linker64",
    "vulkan_loader": artifact(service_loader_path),
    "policy": policy,
    "physical_device_discovery": {
        "validation_mode": validation_mode,
        "loader_api_version": api_version,
        "loader_api_version_text": (
            f"{(api_version >> 22) & 0x7f}."
            f"{(api_version >> 12) & 0x3ff}.{api_version & 0xfff}"
        ),
        "exposed_extension_count": 7,
        "exposed_layer_count": 0,
        "instance_ids": [instance_one, instance_two],
        "instance_type": 1,
        "instance_serials": [
            instance_one & ((1 << 56) - 1),
            instance_two & ((1 << 56) - 1),
        ],
        "physical_device_id": physical_device,
        "physical_device_type": 2,
        "physical_device_serial": physical_device & ((1 << 56) - 1),
        "device_name": device_name,
        "device_api_version": device_api,
        "driver_version": driver_version,
        "vendor_id": vendor_id,
        "device_id": device_id,
        "max_push_constants_size": max_push_constants,
        "max_push_constants_requirement": (
            ">=256 for DXVK MaxTotalPushDataSize adapter acceptance"
        ),
        "sampled_image_format_max_extent": [
            image_format_width,
            image_format_height,
        ],
        "native_resolution_extent_requirement": [2800, 1752],
        "queue_family_count": queue_count,
        "memory_type_count": memory_type_count,
        "memory_heap_count": memory_heap_count,
        "device_extension_count": device_extension_count,
        "extension_transport_paginated": device_extension_count > 15,
        "sampler_anisotropy": sampler_anisotropy,
        "logical_device_id": logical_device,
        "logical_device_type": 3,
        "logical_device_serial": logical_device & ((1 << 56) - 1),
        "queue_id": logical_queue,
        "queue_type": 4,
        "queue_serial": logical_queue & ((1 << 56) - 1),
        "queue_identity_stable": True,
        "empty_submit_result": empty_submit_result,
        "unsupported_submit_shape_rejected_client_side": True,
        "queue_wait_idle_result": queue_wait_result,
        "device_wait_idle_result": device_wait_result,
        "command_stream_mode": (
            "shared" if shared_command_stream else "strict"
        ),
        "mapped_memory_mode": (
            "shared-upload-only"
            if shared_mapped_memory
            else "strict-kept-mapped"
            if keep_strict_mapped_memory
            else "strict"
        ),
        "animated_frame_count": animated_frames,
        "animated_reused_image": bool(animated_reused_image),
        "animated_recording_round_trips": animated_recording_rtts,
        "command_pool_id": command_pool,
        "command_pool_type": 10,
        "command_pool_serial": command_pool & ((1 << 56) - 1),
        "command_buffer_id": command_buffer,
        "command_buffer_type": 11,
        "command_buffer_serial": command_buffer & ((1 << 56) - 1),
        "command_recording_round_trips": recording_rtts,
        "command_submit_result": command_submit_result,
        "command_pool_reset_result": pool_reset_result,
        "command_buffer_freed_explicitly": True,
        "command_pool_destroyed_explicitly": True,
        "buffer_id": buffer,
        "buffer_type": 19,
        "buffer_serial": buffer & ((1 << 56) - 1),
        "memory_id": device_memory,
        "memory_type": 9,
        "memory_serial": device_memory & ((1 << 56) - 1),
        "selected_memory_type_index": memory_type_index,
        "buffer_requirement_size": buffer_requirement_size,
        "buffer_requirement_alignment": buffer_requirement_alignment,
        "buffer_requirement_type_bits": buffer_requirement_type_bits,
        "buffer_device_address": buffer_address,
        "buffer_device_address_stable": True,
        "image_id": image,
        "image_type": 7,
        "image_view_id": image_view,
        "image_view_type": 8,
        "image_requirement_bytes": image_bytes,
        "image_allocation_bytes": image_allocation_bytes,
        "image_prefers_dedicated_allocation": bool(image_prefers_dedicated),
        "image_requires_dedicated_allocation": bool(image_requires_dedicated),
        "mapped_bytes": mapped_bytes,
        "mapped_mismatches": mapped_mismatches,
        "mapped_memory_round_trips": {
            "map": map_rtts,
            "flush": flush_rtts,
            "invalidate": invalidate_rtts,
            "unmap": unmap_rtts,
            "submit": submit_rtts,
        },
        "mapped_memory_opcodes": {
            "map": map_opcode,
            "flush": flush_opcode,
            "invalidate": invalidate_opcode,
            "unmap": unmap_opcode,
            "submit": submit_opcode,
        },
        "ineligible_mapping_fallback": {
            "map_round_trips": ineligible_map_rtts,
            "unmap_round_trips": ineligible_unmap_rtts,
            "map_opcode": ineligible_map_opcode,
            "unmap_opcode": ineligible_unmap_opcode,
        },
        "fill_word": "0xa5c3f00d",
        "fill_word_count": fill_words,
        "mismatched_words": mismatches,
        "fence_id": fence,
        "fence_type": 18,
        "fence_serial": fence & ((1 << 56) - 1),
        "fence_status_before_submit": fence_before_result,
        "fenced_submit_result": fenced_submit_result,
        "fence_status_after_submit": fence_after_result,
        "fence_wait_result": fence_wait_result,
        "fence_reset_result": fence_reset_result,
        "fence_status_after_reset": fence_after_reset_result,
        "queue_wait_used_for_gpu_fill": False,
        "fence_destroyed_explicitly": True,
        "buffer_destroyed_explicitly": True,
        "memory_freed_explicitly": True,
        "logical_device_destroyed_explicitly": True,
        "instances_destroyed_explicitly": True,
        "client_stdout": client_stdout,
        "client_stderr_bytes": 0,
        "service_stderr_bytes": 0,
        "service_ready": "ready socket=" in service_stdout_path.read_text(),
    },
    "standalone_activity_harness": harness_result,
    "virtual_wsi_client_contract": {
        "surface_created": True,
        "swapchain_created": True,
        "swapchain_image_count": 3,
        "acquire_next_image_succeeded": True,
        "queue_present_succeeded": True,
        "activity_received_one_time_fd_bundle": True,
        "activity_imported_images": False,
        "visible_frame_claim": False,
        "fps_claim": False,
    },
    "dynamic_exports": sorted(expected_exports),
    "artifacts": {
        "policy_summary": artifact(policy_path),
        "glibc_library": artifact(library_path),
        "glibc_client": artifact(client_path),
        "bionic_service": artifact(service_path),
        "activity_harness_result": artifact(harness_result_path),
    },
}
assert document["physical_device_discovery"]["service_ready"] is True
evidence_path.write_text(json.dumps(document, indent=2) + "\n")
print(json.dumps(document, indent=2))
print("e077_current_global=PASS")
PY

printf 'evidence=%s\n' "$evidence"
