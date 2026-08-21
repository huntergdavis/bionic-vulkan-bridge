#ifndef BVB_PROTOCOL_H
#define BVB_PROTOCOL_H

#include <bvb/lifecycle.h>
#include <bvb/vulkan_caps.h>
#include <bvb/vulkan_selftest.h>
#include <bvb/wsi_frame_ring.h>

#include <stddef.h>
#include <stdint.h>

enum {
    BVB_PROTOCOL_MAGIC = 0x31425642,
    BVB_PROTOCOL_VERSION = 1,
    BVB_PROTOCOL_HEADER_SIZE = 24,
    BVB_PROTOCOL_MAX_PAYLOAD = 4096,
    BVB_PROTOCOL_REQUEST = 1,
    BVB_PROTOCOL_RESPONSE = 2,
    BVB_OPCODE_HELLO = 1,
    BVB_OPCODE_VULKAN_CAPS = 2,
    BVB_OPCODE_VULKAN_SELFTEST = 3,
    BVB_OPCODE_ACTIVITY_STATUS = 4,
    BVB_OPCODE_VULKAN_BATCH_SELFTEST = 5,
    BVB_OPCODE_SHARED_BATCH_SETUP = 6,
    BVB_OPCODE_SHARED_BATCH_EXECUTE = 7,
    BVB_OPCODE_VISIBLE_BATCH_SETUP = 8,
    BVB_OPCODE_VISIBLE_BATCH_EXECUTE = 9,
    BVB_OPCODE_VISIBLE_BATCH_INLINE = 10,
    BVB_OPCODE_VULKAN_GLOBAL_INFO = 11,
    BVB_OPCODE_VULKAN_INSTANCE_CREATE = 12,
    BVB_OPCODE_VULKAN_INSTANCE_DESTROY = 13,
    BVB_OPCODE_VULKAN_PHYSICAL_DEVICES = 14,
    BVB_OPCODE_VULKAN_PHYSICAL_DEVICE_PROPERTIES = 15,
    BVB_OPCODE_VULKAN_QUEUE_FAMILY_PROPERTIES = 16,
    BVB_OPCODE_VULKAN_MEMORY_PROPERTIES = 17,
    BVB_OPCODE_VULKAN_DEVICE_EXTENSIONS = 18,
    BVB_OPCODE_VULKAN_PHYSICAL_DEVICE_FEATURES = 19,
    BVB_OPCODE_VULKAN_DEVICE_CREATE = 20,
    BVB_OPCODE_VULKAN_DEVICE_DESTROY = 21,
    BVB_OPCODE_VULKAN_DEVICE_QUEUE = 22,
    BVB_OPCODE_VULKAN_QUEUE_SUBMIT_EMPTY = 23,
    BVB_OPCODE_VULKAN_QUEUE_WAIT_IDLE = 24,
    BVB_OPCODE_VULKAN_DEVICE_WAIT_IDLE = 25,
    BVB_OPCODE_VULKAN_COMMAND_POOL_CREATE = 26,
    BVB_OPCODE_VULKAN_COMMAND_POOL_DESTROY = 27,
    BVB_OPCODE_VULKAN_COMMAND_POOL_RESET = 28,
    BVB_OPCODE_VULKAN_COMMAND_BUFFER_ALLOCATE = 29,
    BVB_OPCODE_VULKAN_COMMAND_BUFFER_FREE = 30,
    BVB_OPCODE_VULKAN_COMMAND_BUFFER_BEGIN = 31,
    BVB_OPCODE_VULKAN_COMMAND_BUFFER_END = 32,
    BVB_OPCODE_VULKAN_QUEUE_SUBMIT_COMMAND = 33,
    BVB_OPCODE_VULKAN_BUFFER_CREATE = 34,
    BVB_OPCODE_VULKAN_BUFFER_DESTROY = 35,
    BVB_OPCODE_VULKAN_BUFFER_REQUIREMENTS = 36,
    BVB_OPCODE_VULKAN_MEMORY_ALLOCATE = 37,
    BVB_OPCODE_VULKAN_MEMORY_FREE = 38,
    BVB_OPCODE_VULKAN_BUFFER_BIND = 39,
    BVB_OPCODE_VULKAN_COMMAND_BUFFER_FILL = 40,
    BVB_OPCODE_VULKAN_MEMORY_VERIFY_FILL = 41,
    BVB_OPCODE_VULKAN_FENCE_CREATE = 42,
    BVB_OPCODE_VULKAN_FENCE_DESTROY = 43,
    BVB_OPCODE_VULKAN_FENCE_STATUS = 44,
    BVB_OPCODE_VULKAN_FENCE_WAIT = 45,
    BVB_OPCODE_VULKAN_FENCE_RESET = 46,
    BVB_OPCODE_VULKAN_QUEUE_SUBMIT_COMMAND_FENCE = 47,
    BVB_OPCODE_VULKAN_MEMORY_WRITE = 48,
    BVB_OPCODE_VULKAN_MEMORY_READ = 49,
    BVB_OPCODE_EXTERNAL_MEMORY_IMPORT_TEST = 50,
    BVB_OPCODE_EXTERNAL_SYNC_IMPORT_TEST = 51,
    BVB_OPCODE_EXTERNAL_IMAGE_IMPORT_TEST = 52,
    BVB_OPCODE_EXTERNAL_IMAGE_FENCED_IMPORT_TEST = 53,
    BVB_OPCODE_EXTERNAL_IMAGE_FRAME_RING_TEST = 54,
    BVB_OPCODE_VULKAN_FORMAT_PROPERTIES = 55,
    BVB_OPCODE_VULKAN_IMAGE_FORMAT_PROPERTIES = 56,
    BVB_OPCODE_VULKAN_DEVICE_CREATE_EXTENDED = 57,
    BVB_OPCODE_VULKAN_INSTANCE_EXTENSIONS = 58,
    BVB_OPCODE_VULKAN_INSTANCE_CREATE_EXTENDED = 59,
    BVB_OPCODE_VULKAN_EXTERNAL_BUFFER_PROPERTIES = 60,
    BVB_OPCODE_VULKAN_EXTERNAL_SEMAPHORE_PROPERTIES = 61,
    BVB_OPCODE_VULKAN_CORE_FEATURES = 62,
    BVB_OPCODE_VULKAN_DEVICE_CREATE_PACKED = 63,
    BVB_OPCODE_VULKAN_SWAPCHAIN_PREPARE = 64,
    BVB_OPCODE_VULKAN_SWAPCHAIN_DESTROY = 65,
    BVB_OPCODE_VULKAN_DESCRIPTOR_SET_LAYOUT_CREATE = 66,
    BVB_OPCODE_VULKAN_DESCRIPTOR_POOL_CREATE = 67,
    BVB_OPCODE_VULKAN_DESCRIPTOR_SET_ALLOCATE = 68,
    BVB_OPCODE_VULKAN_SAMPLER_CREATE = 69,
    BVB_OPCODE_VULKAN_DESCRIPTOR_UPDATE = 70,
    BVB_OPCODE_VULKAN_DESCRIPTOR_OBJECT_DESTROY = 71,
    BVB_OPCODE_VULKAN_PIPELINE_LAYOUT_CREATE = 72,
    BVB_OPCODE_VULKAN_PIPELINE_LAYOUT_DESTROY = 73,
    BVB_OPCODE_VULKAN_GRAPHICS_PIPELINE_CREATE = 74,
    BVB_OPCODE_VULKAN_PIPELINE_DESTROY = 75,
    BVB_OPCODE_VULKAN_SEMAPHORE_CREATE = 80,
    BVB_OPCODE_VULKAN_SEMAPHORE_DESTROY = 81,
    BVB_OPCODE_VULKAN_SEMAPHORE_COUNTER = 82,
    BVB_OPCODE_VULKAN_SEMAPHORE_WAIT = 83,
    BVB_OPCODE_VULKAN_SEMAPHORE_SIGNAL = 84,
    BVB_OPCODE_VULKAN_QUEUE_SUBMIT_2 = 85,
    BVB_OPCODE_VULKAN_IMAGE_CREATE = 90,
    BVB_OPCODE_VULKAN_IMAGE_DESTROY = 91,
    BVB_OPCODE_VULKAN_IMAGE_REQUIREMENTS = 92,
    BVB_OPCODE_VULKAN_IMAGE_BIND = 93,
    BVB_OPCODE_VULKAN_IMAGE_VIEW_CREATE = 94,
    BVB_OPCODE_VULKAN_IMAGE_VIEW_DESTROY = 95,
    BVB_OPCODE_VULKAN_IMAGE_REQUIREMENTS_2 = 96,
    BVB_OPCODE_VULKAN_MEMORY_ALLOCATE_EXTENDED = 97,
    BVB_OPCODE_VULKAN_SWAPCHAIN_ACQUIRE = 100,
    BVB_OPCODE_VULKAN_SWAPCHAIN_PRESENT = 101,
    BVB_OPCODE_LAST = BVB_OPCODE_VULKAN_SWAPCHAIN_PRESENT,
    BVB_HELLO_REQUEST_SIZE = 8,
    BVB_HELLO_RESPONSE_SIZE = 16,
    BVB_VULKAN_CAPS_PREFIX_SIZE = 16,
    BVB_VULKAN_CAPS_DEVICE_SIZE = 296,
    BVB_VULKAN_SELFTEST_SIZE = 64,
    BVB_ACTIVITY_STATUS_SIZE = 56,
    BVB_SHARED_BATCH_SETUP_SIZE = 16,
    BVB_SHARED_BATCH_EXECUTE_SIZE = 24,
    BVB_VISIBLE_BATCH_SETUP_SIZE =
        BVB_LIFECYCLE_TOKEN_SIZE + BVB_SHARED_BATCH_SETUP_SIZE,
    BVB_VISIBLE_BATCH_EXECUTE_SIZE =
        BVB_LIFECYCLE_TOKEN_SIZE + BVB_SHARED_BATCH_EXECUTE_SIZE,
    BVB_VISIBLE_BATCH_INLINE_PREFIX_SIZE = BVB_LIFECYCLE_TOKEN_SIZE,
    BVB_VISIBLE_BATCH_INLINE_MAX_BYTES =
        BVB_PROTOCOL_MAX_PAYLOAD - BVB_VISIBLE_BATCH_INLINE_PREFIX_SIZE,
    BVB_VULKAN_GLOBAL_INFO_SIZE = 24,
    BVB_VULKAN_INSTANCE_CREATE_REQUEST_SIZE = 16,
    BVB_VULKAN_INSTANCE_CREATE_RESPONSE_SIZE = 16,
    BVB_VULKAN_INSTANCE_ID_SIZE = 8,
    BVB_VULKAN_PHYSICAL_DEVICE_ID_SIZE = 8,
    BVB_VULKAN_FORMAT_QUERY_SIZE = 16,
    BVB_VULKAN_FORMAT_PROPERTIES_SIZE = 12,
    BVB_VULKAN_IMAGE_FORMAT_QUERY_SIZE = 32,
    BVB_VULKAN_IMAGE_FORMAT_PROPERTIES_SIZE = 40,
    BVB_VULKAN_EXTERNAL_BUFFER_QUERY_SIZE = 24,
    BVB_VULKAN_EXTERNAL_BUFFER_PROPERTIES_SIZE = 12,
    BVB_VULKAN_EXTERNAL_SEMAPHORE_QUERY_SIZE = 16,
    BVB_VULKAN_EXTERNAL_SEMAPHORE_PROPERTIES_SIZE = 12,
    BVB_VULKAN_CORE_FEATURES_SIZE = 100,
    BVB_VULKAN_BASE_FEATURES_SIZE = 55 * sizeof(uint32_t),
    BVB_VULKAN_DEVICE_EXTENSION_QUERY_SIZE = 16,
    BVB_VULKAN_DEVICE_CREATE_REQUEST_SIZE = 32,
    BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE = 128,
    BVB_VULKAN_MAX_ENABLED_EXTENSIONS = 24,
    BVB_VULKAN_DEVICE_CREATE_PACKED_PREFIX_SIZE = 32,
    BVB_VULKAN_DEVICE_QUEUE_CREATE_INFO_SIZE = 16,
    BVB_VULKAN_MAX_DEVICE_QUEUE_CREATE_INFOS = 8,
    BVB_VULKAN_MAX_DEVICE_QUEUE_PRIORITIES = 16,
    BVB_VULKAN_MAX_DEVICE_CREATE_EXTENSIONS = 64,
    BVB_VULKAN_DEVICE_CREATE_RESPONSE_SIZE = 16,
    BVB_VULKAN_DEVICE_ID_SIZE = 8,
    BVB_VULKAN_DEVICE_QUEUE_REQUEST_SIZE = 16,
    BVB_VULKAN_QUEUE_ID_SIZE = 8,
    BVB_VULKAN_RESULT_SIZE = 4,
    BVB_VULKAN_COMMAND_POOL_CREATE_REQUEST_SIZE = 16,
    BVB_VULKAN_COMMAND_POOL_CREATE_RESPONSE_SIZE = 16,
    BVB_VULKAN_COMMAND_POOL_ID_SIZE = 8,
    BVB_VULKAN_COMMAND_POOL_RESET_REQUEST_SIZE = 16,
    BVB_VULKAN_COMMAND_BUFFER_ALLOCATE_REQUEST_SIZE = 16,
    BVB_VULKAN_COMMAND_BUFFER_ALLOCATE_RESPONSE_SIZE = 16,
    BVB_VULKAN_COMMAND_BUFFER_FREE_REQUEST_SIZE = 16,
    BVB_VULKAN_COMMAND_BUFFER_BEGIN_REQUEST_SIZE = 16,
    BVB_VULKAN_COMMAND_BUFFER_ID_SIZE = 8,
    BVB_VULKAN_QUEUE_SUBMIT_COMMAND_REQUEST_SIZE = 16,
    BVB_VULKAN_BUFFER_CREATE_REQUEST_SIZE = 24,
    BVB_VULKAN_OBJECT_CREATE_RESPONSE_SIZE = 16,
    BVB_VULKAN_OBJECT_ID_SIZE = 8,
    BVB_VULKAN_BUFFER_REQUIREMENTS_SIZE = 24,
    BVB_VULKAN_MEMORY_ALLOCATE_REQUEST_SIZE = 24,
    BVB_VULKAN_MEMORY_ALLOCATE_EXTENDED_REQUEST_SIZE = 40,
    BVB_VULKAN_MAX_MEMORY_ALLOCATION_SIZE = 256U * 1024U * 1024U,
    BVB_VULKAN_BUFFER_BIND_REQUEST_SIZE = 24,
    BVB_VULKAN_IMAGE_MAX_QUEUE_FAMILIES = 8,
    BVB_VULKAN_IMAGE_MAX_VIEW_FORMATS = 16,
    BVB_VULKAN_IMAGE_CREATE_REQUEST_SIZE = 176,
    BVB_VULKAN_IMAGE_REQUIREMENTS_SIZE = 24,
    BVB_VULKAN_IMAGE_REQUIREMENTS_2_REQUEST_SIZE = 16,
    BVB_VULKAN_IMAGE_REQUIREMENTS_2_RESPONSE_SIZE = 32,
    BVB_VULKAN_IMAGE_BIND_REQUEST_SIZE = 24,
    BVB_VULKAN_IMAGE_VIEW_CREATE_REQUEST_SIZE = 72,
    BVB_VULKAN_COMMAND_BUFFER_FILL_REQUEST_SIZE = 40,
    BVB_VULKAN_MEMORY_VERIFY_FILL_REQUEST_SIZE = 32,
    BVB_VULKAN_MEMORY_VERIFY_FILL_RESPONSE_SIZE = 8,
    BVB_VULKAN_FENCE_CREATE_REQUEST_SIZE = 16,
    BVB_VULKAN_FENCE_WAIT_REQUEST_SIZE = 24,
    BVB_VULKAN_QUEUE_SUBMIT_COMMAND_FENCE_REQUEST_SIZE = 24,
    BVB_VULKAN_SEMAPHORE_CREATE_REQUEST_SIZE = 24,
    BVB_VULKAN_SEMAPHORE_COUNTER_RESPONSE_SIZE = 16,
    BVB_VULKAN_SEMAPHORE_SIGNAL_REQUEST_SIZE = 24,
    BVB_VULKAN_SEMAPHORE_WAIT_PREFIX_SIZE = 24,
    BVB_VULKAN_SEMAPHORE_WAIT_RECORD_SIZE = 16,
    BVB_VULKAN_MAX_SEMAPHORES_PER_WAIT = 16,
    BVB_VULKAN_SEMAPHORE_WAIT_MAX_SIZE =
        BVB_VULKAN_SEMAPHORE_WAIT_PREFIX_SIZE +
        BVB_VULKAN_MAX_SEMAPHORES_PER_WAIT *
            BVB_VULKAN_SEMAPHORE_WAIT_RECORD_SIZE,
    BVB_VULKAN_SUBMIT_2_PREFIX_SIZE = 32,
    BVB_VULKAN_SUBMIT_2_SEMAPHORE_RECORD_SIZE = 32,
    BVB_VULKAN_SUBMIT_2_COMMAND_RECORD_SIZE = 16,
    BVB_VULKAN_MAX_COMMAND_BUFFERS_PER_SUBMIT = 16,
    BVB_VULKAN_SUBMIT_2_MAX_SIZE = BVB_VULKAN_SUBMIT_2_PREFIX_SIZE +
        2 * BVB_VULKAN_MAX_SEMAPHORES_PER_WAIT *
            BVB_VULKAN_SUBMIT_2_SEMAPHORE_RECORD_SIZE +
        BVB_VULKAN_MAX_COMMAND_BUFFERS_PER_SUBMIT *
            BVB_VULKAN_SUBMIT_2_COMMAND_RECORD_SIZE,
    BVB_VULKAN_MEMORY_IO_PREFIX_SIZE = 24,
    BVB_VULKAN_MEMORY_IO_RESPONSE_PREFIX_SIZE = 8,
    BVB_EXTERNAL_MEMORY_IMPORT_REQUEST_SIZE = 16,
    BVB_EXTERNAL_SYNC_IMPORT_REQUEST_SIZE = 24,
    BVB_EXTERNAL_IMAGE_IMPORT_REQUEST_SIZE = 32,
    BVB_VULKAN_SWAPCHAIN_PREPARE_REQUEST_SIZE = 40,
    BVB_VULKAN_SWAPCHAIN_IMAGE_RECORD_SIZE = 24,
    BVB_VULKAN_SWAPCHAIN_PREPARE_RESPONSE_SIZE =
        32 + BVB_WSI_FRAME_RING_MAX_SLOTS *
                 BVB_VULKAN_SWAPCHAIN_IMAGE_RECORD_SIZE,
    BVB_VULKAN_SWAPCHAIN_ACQUIRE_REQUEST_SIZE = 40,
    BVB_VULKAN_SWAPCHAIN_ACQUIRE_RESPONSE_SIZE = 8,
    BVB_VULKAN_SWAPCHAIN_PRESENT_PREFIX_SIZE = 32,
    BVB_VULKAN_MAX_PRESENT_WAIT_SEMAPHORES = 16,
    BVB_VULKAN_SWAPCHAIN_PRESENT_MAX_SIZE =
        BVB_VULKAN_SWAPCHAIN_PRESENT_PREFIX_SIZE +
        BVB_VULKAN_MAX_PRESENT_WAIT_SEMAPHORES * sizeof(uint64_t),
    BVB_VULKAN_SWAPCHAIN_PRESENT_RESPONSE_SIZE = 8,
    BVB_VULKAN_MEMORY_IO_MAX_BYTES =
        BVB_PROTOCOL_MAX_PAYLOAD - BVB_VULKAN_MEMORY_IO_PREFIX_SIZE,
    BVB_VULKAN_PHYSICAL_DEVICES_PREFIX_SIZE = 8,
    BVB_VULKAN_MAX_PHYSICAL_DEVICES = 8,
    BVB_SHARED_BATCH_MIN_BYTES = 4096,
    BVB_SHARED_BATCH_MAX_BYTES = 16 * 1024 * 1024,
    BVB_SERVICE_BIONIC = 1U << 0,
    BVB_SERVICE_ANDROID_VULKAN_LOADER = 1U << 1,
    BVB_SERVICE_ACTIVITY_INGRESS = 1U << 2,
};

enum bvb_vulkan_device_feature_struct_bits {
    BVB_VULKAN_DEVICE_FEATURE_VULKAN_11 = 1U << 0,
    BVB_VULKAN_DEVICE_FEATURE_VULKAN_12 = 1U << 1,
    BVB_VULKAN_DEVICE_FEATURE_VULKAN_13 = 1U << 2,
    BVB_VULKAN_DEVICE_FEATURE_DEPTH_CLIP_ENABLE = 1U << 3,
    BVB_VULKAN_DEVICE_FEATURE_ROBUSTNESS_2 = 1U << 4,
    BVB_VULKAN_DEVICE_FEATURE_MAINTENANCE_5 = 1U << 5,
    BVB_VULKAN_DEVICE_FEATURE_MAINTENANCE_6 = 1U << 6,
    BVB_VULKAN_DEVICE_FEATURE_BASE = 1U << 7,
    BVB_VULKAN_DEVICE_FEATURE_STRUCT_MASK =
        BVB_VULKAN_DEVICE_FEATURE_VULKAN_11 |
        BVB_VULKAN_DEVICE_FEATURE_VULKAN_12 |
        BVB_VULKAN_DEVICE_FEATURE_VULKAN_13 |
        BVB_VULKAN_DEVICE_FEATURE_DEPTH_CLIP_ENABLE |
        BVB_VULKAN_DEVICE_FEATURE_ROBUSTNESS_2 |
        BVB_VULKAN_DEVICE_FEATURE_MAINTENANCE_5 |
        BVB_VULKAN_DEVICE_FEATURE_MAINTENANCE_6 |
        BVB_VULKAN_DEVICE_FEATURE_BASE,
};

enum bvb_vulkan_image_create_pnext_bits {
    BVB_VULKAN_IMAGE_CREATE_PNEXT_FORMAT_LIST = 1U << 0,
    BVB_VULKAN_IMAGE_CREATE_PNEXT_STENCIL_USAGE = 1U << 1,
    BVB_VULKAN_IMAGE_CREATE_PNEXT_MASK =
        BVB_VULKAN_IMAGE_CREATE_PNEXT_FORMAT_LIST |
        BVB_VULKAN_IMAGE_CREATE_PNEXT_STENCIL_USAGE,
};

enum bvb_vulkan_image_view_create_pnext_bits {
    BVB_VULKAN_IMAGE_VIEW_CREATE_PNEXT_USAGE = 1U << 0,
    BVB_VULKAN_IMAGE_VIEW_CREATE_PNEXT_MASK =
        BVB_VULKAN_IMAGE_VIEW_CREATE_PNEXT_USAGE,
};

enum bvb_vulkan_image_requirements_2_pnext_bits {
    BVB_VULKAN_IMAGE_REQUIREMENTS_2_PNEXT_DEDICATED = 1U << 0,
    BVB_VULKAN_IMAGE_REQUIREMENTS_2_PNEXT_MASK =
        BVB_VULKAN_IMAGE_REQUIREMENTS_2_PNEXT_DEDICATED,
};

enum bvb_vulkan_memory_allocate_pnext_bits {
    BVB_VULKAN_MEMORY_ALLOCATE_PNEXT_DEDICATED_IMAGE = 1U << 0,
    BVB_VULKAN_MEMORY_ALLOCATE_PNEXT_FLAGS = 1U << 1,
    BVB_VULKAN_MEMORY_ALLOCATE_PNEXT_MASK =
        BVB_VULKAN_MEMORY_ALLOCATE_PNEXT_DEDICATED_IMAGE |
        BVB_VULKAN_MEMORY_ALLOCATE_PNEXT_FLAGS,
};

struct bvb_protocol_header {
    uint16_t version;
    uint16_t kind;
    uint16_t opcode;
    uint32_t request_id;
    uint32_t payload_length;
    int32_t status;
};

struct bvb_protocol_packet {
    struct bvb_protocol_header header;
    uint8_t payload[BVB_PROTOCOL_MAX_PAYLOAD];
};

struct bvb_hello_request {
    uint16_t minimum_version;
    uint16_t maximum_version;
    uint32_t client_flags;
};

struct bvb_hello_response {
    uint16_t negotiated_version;
    uint32_t service_flags;
    uint32_t pointer_bits;
    uint32_t page_size;
};

struct bvb_external_memory_import_request {
    uint64_t allocation_size;
    uint32_t memory_type_index;
    uint32_t buffer_bytes;
};

struct bvb_external_sync_import_request {
    uint64_t allocation_size;
    uint32_t memory_type_index;
    uint32_t buffer_bytes;
    uint32_t expected_fill_word;
};

struct bvb_external_image_import_request {
    uint64_t allocation_size;
    uint32_t memory_type_index;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t expected_color;
};

struct bvb_vulkan_swapchain_prepare_request {
    uint64_t device_id;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t image_usage;
    uint32_t min_image_count;
    uint32_t flags;
    uint64_t generation;
};

struct bvb_vulkan_swapchain_image_record {
    uint64_t image_id;
    uint64_t allocation_size;
    uint32_t memory_type_index;
};

struct bvb_vulkan_swapchain_prepare_response {
    int32_t vulkan_result;
    uint32_t image_count;
    uint64_t swapchain_id;
    uint64_t generation;
    uint32_t control_region_bytes;
    struct bvb_vulkan_swapchain_image_record
        images[BVB_WSI_FRAME_RING_MAX_SLOTS];
};

struct bvb_vulkan_swapchain_acquire_request {
    uint64_t device_id;
    uint64_t swapchain_id;
    uint64_t timeout_ns;
    uint64_t semaphore_id;
    uint64_t fence_id;
};

struct bvb_vulkan_swapchain_acquire_response {
    int32_t vulkan_result;
    uint32_t image_index;
};

struct bvb_vulkan_swapchain_present_request {
    uint64_t queue_id;
    uint64_t swapchain_id;
    uint32_t image_index;
    uint32_t wait_semaphore_count;
    uint32_t flags;
    uint64_t wait_semaphore_ids[BVB_VULKAN_MAX_PRESENT_WAIT_SEMAPHORES];
};

struct bvb_vulkan_swapchain_present_response {
    int32_t vulkan_result;
    uint32_t sequence;
};

struct bvb_shared_batch_setup {
    uint32_t region_bytes;
    uint64_t generation;
};

struct bvb_shared_batch_execute {
    uint64_t generation;
    uint32_t offset;
    uint32_t length;
    uint64_t sequence;
};

struct bvb_visible_batch_setup {
    uint8_t token[BVB_LIFECYCLE_TOKEN_SIZE];
    struct bvb_shared_batch_setup shared;
};

struct bvb_visible_batch_execute {
    uint8_t token[BVB_LIFECYCLE_TOKEN_SIZE];
    struct bvb_shared_batch_execute shared;
};

struct bvb_vulkan_global_info {
    uint32_t loader_api_version;
    uint32_t native_extension_count;
    uint32_t native_layer_count;
    uint32_t exposed_extension_count;
    uint32_t exposed_layer_count;
};

struct bvb_vulkan_instance_create_request {
    uint32_t api_version;
    uint32_t flags;
    uint32_t enabled_layer_count;
    uint32_t enabled_extension_count;
};

struct bvb_vulkan_instance_create_extended_request {
    struct bvb_vulkan_instance_create_request base;
    char enabled_extensions[BVB_VULKAN_MAX_ENABLED_EXTENSIONS]
                           [BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE];
};

struct bvb_vulkan_instance_create_response {
    int32_t vulkan_result;
    uint64_t instance_id;
};

struct bvb_vulkan_physical_devices {
    int32_t vulkan_result;
    uint32_t count;
    uint64_t ids[BVB_VULKAN_MAX_PHYSICAL_DEVICES];
};

struct bvb_vulkan_format_query {
    uint64_t physical_device_id;
    uint32_t format;
};

struct bvb_vulkan_format_properties {
    uint32_t linear_tiling_features;
    uint32_t optimal_tiling_features;
    uint32_t buffer_features;
};

struct bvb_vulkan_image_format_query {
    uint64_t physical_device_id;
    uint32_t format;
    uint32_t type;
    uint32_t tiling;
    uint32_t usage;
    uint32_t flags;
};

struct bvb_vulkan_image_format_properties {
    int32_t vulkan_result;
    uint32_t max_extent_width;
    uint32_t max_extent_height;
    uint32_t max_extent_depth;
    uint32_t max_mip_levels;
    uint32_t max_array_layers;
    uint32_t sample_counts;
    uint64_t max_resource_size;
};

struct bvb_vulkan_external_buffer_query {
    uint64_t physical_device_id;
    uint32_t flags;
    uint32_t usage;
    uint32_t handle_type;
};

struct bvb_vulkan_external_buffer_properties {
    uint32_t external_memory_features;
    uint32_t export_from_imported_handle_types;
    uint32_t compatible_handle_types;
};

struct bvb_vulkan_external_semaphore_query {
    uint64_t physical_device_id;
    uint32_t handle_type;
};

struct bvb_vulkan_external_semaphore_properties {
    uint32_t export_from_imported_handle_types;
    uint32_t compatible_handle_types;
    uint32_t external_semaphore_features;
};

struct bvb_vulkan_core_features {
    uint32_t shader_draw_parameters;
    uint32_t buffer_device_address;
    uint32_t descriptor_indexing;
    uint32_t descriptor_binding_sampled_image_update_after_bind;
    uint32_t descriptor_binding_update_unused_while_pending;
    uint32_t descriptor_binding_partially_bound;
    uint32_t host_query_reset;
    uint32_t runtime_descriptor_array;
    uint32_t sampler_mirror_clamp_to_edge;
    uint32_t scalar_block_layout;
    uint32_t timeline_semaphore;
    uint32_t uniform_buffer_standard_layout;
    uint32_t vulkan_memory_model;
    uint32_t compute_full_subgroups;
    uint32_t dynamic_rendering;
    uint32_t maintenance4;
    uint32_t shader_demote_to_helper_invocation;
    uint32_t shader_zero_initialize_workgroup_memory;
    uint32_t subgroup_size_control;
    uint32_t synchronization2;
    uint32_t depth_clip_enable;
    uint32_t robust_buffer_access2;
    uint32_t null_descriptor;
    uint32_t maintenance5;
    uint32_t maintenance6;
};

struct bvb_vulkan_base_features {
    uint32_t values[55];
};

struct bvb_vulkan_device_extension_query {
    uint64_t physical_device_id;
    uint32_t first;
    uint32_t max_count;
};

struct bvb_vulkan_device_create_request {
    uint64_t physical_device_id;
    uint32_t flags;
    uint32_t queue_family_index;
    uint32_t queue_count;
    uint32_t queue_priority_bits;
    uint32_t enabled_layer_count;
    uint32_t enabled_extension_count;
};

struct bvb_vulkan_device_create_extended_request {
    struct bvb_vulkan_device_create_request base;
    char enabled_extensions[BVB_VULKAN_MAX_ENABLED_EXTENSIONS]
                           [BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE];
};

struct bvb_vulkan_device_queue_create_info {
    uint32_t flags;
    uint32_t queue_family_index;
    uint32_t queue_count;
    uint32_t first_priority;
};

struct bvb_vulkan_device_create_packed_request {
    uint64_t physical_device_id;
    uint32_t flags;
    uint32_t queue_create_info_count;
    uint32_t queue_priority_count;
    uint32_t enabled_layer_count;
    uint32_t enabled_extension_count;
    uint32_t enabled_feature_structs;
    struct bvb_vulkan_core_features enabled_features;
    struct bvb_vulkan_base_features enabled_base_features;
    struct bvb_vulkan_device_queue_create_info
        queue_create_infos[BVB_VULKAN_MAX_DEVICE_QUEUE_CREATE_INFOS];
    uint32_t queue_priority_bits[BVB_VULKAN_MAX_DEVICE_QUEUE_PRIORITIES];
    char enabled_extensions[BVB_VULKAN_MAX_DEVICE_CREATE_EXTENSIONS]
                           [BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE];
};

struct bvb_vulkan_device_create_response {
    int32_t vulkan_result;
    uint64_t device_id;
};

struct bvb_vulkan_device_queue_request {
    uint64_t device_id;
    uint32_t queue_family_index;
    uint32_t queue_index;
};

struct bvb_vulkan_command_pool_create_request {
    uint64_t device_id;
    uint32_t flags;
    uint32_t queue_family_index;
};

struct bvb_vulkan_command_pool_create_response {
    int32_t vulkan_result;
    uint64_t command_pool_id;
};

struct bvb_vulkan_command_pool_reset_request {
    uint64_t command_pool_id;
    uint32_t flags;
};

struct bvb_vulkan_command_buffer_allocate_request {
    uint64_t command_pool_id;
    uint32_t level;
    uint32_t count;
};

struct bvb_vulkan_command_buffer_allocate_response {
    int32_t vulkan_result;
    uint64_t command_buffer_id;
};

struct bvb_vulkan_command_buffer_free_request {
    uint64_t command_pool_id;
    uint64_t command_buffer_id;
};

struct bvb_vulkan_command_buffer_begin_request {
    uint64_t command_buffer_id;
    uint32_t flags;
};

struct bvb_vulkan_queue_submit_command_request {
    uint64_t queue_id;
    uint64_t command_buffer_id;
};

struct bvb_vulkan_buffer_create_request {
    uint64_t device_id;
    uint64_t size;
    uint32_t usage;
    uint32_t flags;
};

struct bvb_vulkan_object_create_response {
    int32_t vulkan_result;
    uint64_t object_id;
};

struct bvb_vulkan_buffer_requirements {
    uint64_t size;
    uint64_t alignment;
    uint32_t memory_type_bits;
};

struct bvb_vulkan_memory_allocate_request {
    uint64_t device_id;
    uint64_t allocation_size;
    uint32_t memory_type_index;
};

struct bvb_vulkan_memory_allocate_extended_request {
    uint64_t device_id;
    uint64_t allocation_size;
    uint64_t dedicated_image_id;
    uint32_t memory_type_index;
    uint32_t pnext_flags;
    uint32_t allocation_flags;
    uint32_t device_mask;
};

struct bvb_vulkan_buffer_bind_request {
    uint64_t buffer_id;
    uint64_t memory_id;
    uint64_t offset;
};

struct bvb_vulkan_image_create_request {
    uint64_t device_id;
    uint32_t flags;
    uint32_t image_type;
    uint32_t format;
    uint32_t extent_width;
    uint32_t extent_height;
    uint32_t extent_depth;
    uint32_t mip_levels;
    uint32_t array_layers;
    uint32_t samples;
    uint32_t tiling;
    uint32_t usage;
    uint32_t sharing_mode;
    uint32_t queue_family_index_count;
    uint32_t initial_layout;
    uint32_t pnext_flags;
    uint32_t view_format_count;
    uint32_t stencil_usage;
    uint32_t queue_family_indices[BVB_VULKAN_IMAGE_MAX_QUEUE_FAMILIES];
    uint32_t view_formats[BVB_VULKAN_IMAGE_MAX_VIEW_FORMATS];
};

struct bvb_vulkan_image_requirements {
    uint64_t size;
    uint64_t alignment;
    uint32_t memory_type_bits;
};

struct bvb_vulkan_image_requirements_2_request {
    uint64_t image_id;
    uint32_t pnext_flags;
};

struct bvb_vulkan_image_requirements_2_response {
    uint64_t size;
    uint64_t alignment;
    uint32_t memory_type_bits;
    uint32_t pnext_flags;
    uint32_t prefers_dedicated;
    uint32_t requires_dedicated;
};

struct bvb_vulkan_image_bind_request {
    uint64_t image_id;
    uint64_t memory_id;
    uint64_t offset;
};

struct bvb_vulkan_image_view_create_request {
    uint64_t device_id;
    uint64_t image_id;
    uint32_t flags;
    uint32_t view_type;
    uint32_t format;
    uint32_t component_r;
    uint32_t component_g;
    uint32_t component_b;
    uint32_t component_a;
    uint32_t aspect_mask;
    uint32_t base_mip_level;
    uint32_t level_count;
    uint32_t base_array_layer;
    uint32_t layer_count;
    uint32_t pnext_flags;
    uint32_t usage;
};

struct bvb_vulkan_command_buffer_fill_request {
    uint64_t command_buffer_id;
    uint64_t buffer_id;
    uint64_t offset;
    uint64_t size;
    uint32_t data;
};

struct bvb_vulkan_memory_verify_fill_request {
    uint64_t memory_id;
    uint64_t offset;
    uint64_t size;
    uint32_t expected_word;
};

struct bvb_vulkan_memory_verify_fill_response {
    int32_t vulkan_result;
    uint32_t mismatched_words;
};

struct bvb_vulkan_fence_create_request {
    uint64_t device_id;
    uint32_t flags;
};

struct bvb_vulkan_fence_wait_request {
    uint64_t fence_id;
    uint64_t timeout;
    uint32_t wait_all;
};

struct bvb_vulkan_queue_submit_command_fence_request {
    uint64_t queue_id;
    uint64_t command_buffer_id;
    uint64_t fence_id;
};

struct bvb_vulkan_semaphore_create_request {
    uint64_t device_id;
    uint64_t initial_value;
    uint32_t semaphore_type;
    uint32_t flags;
};

struct bvb_vulkan_semaphore_counter_response {
    int32_t vulkan_result;
    uint64_t value;
};

struct bvb_vulkan_semaphore_signal_request {
    uint64_t device_id;
    uint64_t semaphore_id;
    uint64_t value;
};

struct bvb_vulkan_semaphore_wait_record {
    uint64_t semaphore_id;
    uint64_t value;
};

struct bvb_vulkan_semaphore_wait_request {
    uint64_t device_id;
    uint64_t timeout;
    uint32_t flags;
    uint32_t semaphore_count;
    struct bvb_vulkan_semaphore_wait_record
        semaphores[BVB_VULKAN_MAX_SEMAPHORES_PER_WAIT];
};

struct bvb_vulkan_submit_2_semaphore_record {
    uint64_t semaphore_id;
    uint64_t value;
    uint64_t stage_mask;
    uint32_t device_index;
};

struct bvb_vulkan_submit_2_command_record {
    uint64_t command_buffer_id;
    uint32_t device_mask;
};

struct bvb_vulkan_queue_submit_2_request {
    uint64_t queue_id;
    uint64_t fence_id;
    uint32_t flags;
    uint32_t wait_count;
    uint32_t command_count;
    uint32_t signal_count;
    struct bvb_vulkan_submit_2_semaphore_record
        waits[BVB_VULKAN_MAX_SEMAPHORES_PER_WAIT];
    struct bvb_vulkan_submit_2_command_record
        commands[BVB_VULKAN_MAX_COMMAND_BUFFERS_PER_SUBMIT];
    struct bvb_vulkan_submit_2_semaphore_record
        signals[BVB_VULKAN_MAX_SEMAPHORES_PER_WAIT];
};

struct bvb_vulkan_memory_io_request {
    uint64_t memory_id;
    uint64_t offset;
    uint32_t length;
};

struct bvb_vulkan_memory_io_response {
    int32_t vulkan_result;
    uint32_t length;
};

/*
 * The fixed-width wire format is explicitly little-endian; C structure layout
 * never crosses the libc/process boundary.
 *
 * Header (24 bytes): magic:u32, version:u16, kind:u16, opcode:u16,
 * reserved:u16, request_id:u32, payload_length:u32, status:i32.
 */
int bvb_protocol_encode_header(
    uint8_t output[BVB_PROTOCOL_HEADER_SIZE],
    const struct bvb_protocol_header *header);
int bvb_protocol_decode_header(
    const uint8_t input[BVB_PROTOCOL_HEADER_SIZE],
    struct bvb_protocol_header *header);

int bvb_protocol_encode_hello_request(
    uint8_t output[BVB_HELLO_REQUEST_SIZE],
    const struct bvb_hello_request *request);
int bvb_protocol_decode_hello_request(
    const uint8_t input[BVB_HELLO_REQUEST_SIZE],
    struct bvb_hello_request *request);
int bvb_protocol_encode_hello_response(
    uint8_t output[BVB_HELLO_RESPONSE_SIZE],
    const struct bvb_hello_response *response);
int bvb_protocol_decode_hello_response(
    const uint8_t input[BVB_HELLO_RESPONSE_SIZE],
    struct bvb_hello_response *response);
int bvb_protocol_encode_external_memory_import_request(
    uint8_t output[BVB_EXTERNAL_MEMORY_IMPORT_REQUEST_SIZE],
    const struct bvb_external_memory_import_request *request);
int bvb_protocol_decode_external_memory_import_request(
    const uint8_t input[BVB_EXTERNAL_MEMORY_IMPORT_REQUEST_SIZE],
    struct bvb_external_memory_import_request *request);
int bvb_protocol_encode_external_sync_import_request(
    uint8_t output[BVB_EXTERNAL_SYNC_IMPORT_REQUEST_SIZE],
    const struct bvb_external_sync_import_request *request);
int bvb_protocol_decode_external_sync_import_request(
    const uint8_t input[BVB_EXTERNAL_SYNC_IMPORT_REQUEST_SIZE],
    struct bvb_external_sync_import_request *request);
int bvb_protocol_encode_external_image_import_request(
    uint8_t output[BVB_EXTERNAL_IMAGE_IMPORT_REQUEST_SIZE],
    const struct bvb_external_image_import_request *request);
int bvb_protocol_decode_external_image_import_request(
    const uint8_t input[BVB_EXTERNAL_IMAGE_IMPORT_REQUEST_SIZE],
    struct bvb_external_image_import_request *request);
int bvb_protocol_encode_vulkan_swapchain_prepare_request(
    uint8_t output[BVB_VULKAN_SWAPCHAIN_PREPARE_REQUEST_SIZE],
    const struct bvb_vulkan_swapchain_prepare_request *request);
int bvb_protocol_decode_vulkan_swapchain_prepare_request(
    const uint8_t input[BVB_VULKAN_SWAPCHAIN_PREPARE_REQUEST_SIZE],
    struct bvb_vulkan_swapchain_prepare_request *request);
int bvb_protocol_encode_vulkan_swapchain_prepare_response(
    uint8_t output[BVB_VULKAN_SWAPCHAIN_PREPARE_RESPONSE_SIZE],
    const struct bvb_vulkan_swapchain_prepare_response *response);
int bvb_protocol_decode_vulkan_swapchain_prepare_response(
    const uint8_t input[BVB_VULKAN_SWAPCHAIN_PREPARE_RESPONSE_SIZE],
    struct bvb_vulkan_swapchain_prepare_response *response);
int bvb_protocol_encode_vulkan_swapchain_acquire_request(
    uint8_t output[BVB_VULKAN_SWAPCHAIN_ACQUIRE_REQUEST_SIZE],
    const struct bvb_vulkan_swapchain_acquire_request *request);
int bvb_protocol_decode_vulkan_swapchain_acquire_request(
    const uint8_t input[BVB_VULKAN_SWAPCHAIN_ACQUIRE_REQUEST_SIZE],
    struct bvb_vulkan_swapchain_acquire_request *request);
int bvb_protocol_encode_vulkan_swapchain_acquire_response(
    uint8_t output[BVB_VULKAN_SWAPCHAIN_ACQUIRE_RESPONSE_SIZE],
    const struct bvb_vulkan_swapchain_acquire_response *response);
int bvb_protocol_decode_vulkan_swapchain_acquire_response(
    const uint8_t input[BVB_VULKAN_SWAPCHAIN_ACQUIRE_RESPONSE_SIZE],
    struct bvb_vulkan_swapchain_acquire_response *response);
int bvb_protocol_encode_vulkan_swapchain_present_request(
    uint8_t output[BVB_VULKAN_SWAPCHAIN_PRESENT_MAX_SIZE],
    const struct bvb_vulkan_swapchain_present_request *request,
    uint32_t *output_length);
int bvb_protocol_decode_vulkan_swapchain_present_request(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_swapchain_present_request *request);
int bvb_protocol_encode_vulkan_swapchain_present_response(
    uint8_t output[BVB_VULKAN_SWAPCHAIN_PRESENT_RESPONSE_SIZE],
    const struct bvb_vulkan_swapchain_present_response *response);
int bvb_protocol_decode_vulkan_swapchain_present_response(
    const uint8_t input[BVB_VULKAN_SWAPCHAIN_PRESENT_RESPONSE_SIZE],
    struct bvb_vulkan_swapchain_present_response *response);
int bvb_protocol_encode_vulkan_caps(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_caps *caps,
    uint32_t *output_length);
int bvb_protocol_decode_vulkan_caps(
    const uint8_t *input,
    uint32_t input_length,
    struct bvb_vulkan_caps *caps);
int bvb_protocol_encode_vulkan_selftest(
    uint8_t output[BVB_VULKAN_SELFTEST_SIZE],
    const struct bvb_vulkan_selftest_result *result);
int bvb_protocol_decode_vulkan_selftest(
    const uint8_t input[BVB_VULKAN_SELFTEST_SIZE],
    struct bvb_vulkan_selftest_result *result);
int bvb_protocol_encode_activity_status(
    uint8_t output[BVB_ACTIVITY_STATUS_SIZE],
    const struct bvb_activity_status *status);
int bvb_protocol_decode_activity_status(
    const uint8_t input[BVB_ACTIVITY_STATUS_SIZE],
    struct bvb_activity_status *status);
int bvb_protocol_encode_shared_batch_setup(
    uint8_t output[BVB_SHARED_BATCH_SETUP_SIZE],
    const struct bvb_shared_batch_setup *setup);
int bvb_protocol_decode_shared_batch_setup(
    const uint8_t input[BVB_SHARED_BATCH_SETUP_SIZE],
    struct bvb_shared_batch_setup *setup);
int bvb_protocol_encode_shared_batch_execute(
    uint8_t output[BVB_SHARED_BATCH_EXECUTE_SIZE],
    const struct bvb_shared_batch_execute *execute);
int bvb_protocol_decode_shared_batch_execute(
    const uint8_t input[BVB_SHARED_BATCH_EXECUTE_SIZE],
    struct bvb_shared_batch_execute *execute);
int bvb_protocol_encode_visible_batch_setup(
    uint8_t output[BVB_VISIBLE_BATCH_SETUP_SIZE],
    const struct bvb_visible_batch_setup *setup);
int bvb_protocol_decode_visible_batch_setup(
    const uint8_t input[BVB_VISIBLE_BATCH_SETUP_SIZE],
    struct bvb_visible_batch_setup *setup);
int bvb_protocol_encode_visible_batch_execute(
    uint8_t output[BVB_VISIBLE_BATCH_EXECUTE_SIZE],
    const struct bvb_visible_batch_execute *execute);
int bvb_protocol_decode_visible_batch_execute(
    const uint8_t input[BVB_VISIBLE_BATCH_EXECUTE_SIZE],
    struct bvb_visible_batch_execute *execute);
int bvb_protocol_encode_vulkan_global_info(
    uint8_t output[BVB_VULKAN_GLOBAL_INFO_SIZE],
    const struct bvb_vulkan_global_info *info);
int bvb_protocol_decode_vulkan_global_info(
    const uint8_t input[BVB_VULKAN_GLOBAL_INFO_SIZE],
    struct bvb_vulkan_global_info *info);
int bvb_protocol_encode_vulkan_instance_create_request(
    uint8_t output[BVB_VULKAN_INSTANCE_CREATE_REQUEST_SIZE],
    const struct bvb_vulkan_instance_create_request *request);
int bvb_protocol_decode_vulkan_instance_create_request(
    const uint8_t input[BVB_VULKAN_INSTANCE_CREATE_REQUEST_SIZE],
    struct bvb_vulkan_instance_create_request *request);
int bvb_protocol_encode_vulkan_instance_create_extended_request(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_instance_create_extended_request *request,
    uint32_t *output_length);
int bvb_protocol_decode_vulkan_instance_create_extended_request(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_instance_create_extended_request *request);
int bvb_protocol_encode_vulkan_instance_create_response(
    uint8_t output[BVB_VULKAN_INSTANCE_CREATE_RESPONSE_SIZE],
    const struct bvb_vulkan_instance_create_response *response);
int bvb_protocol_decode_vulkan_instance_create_response(
    const uint8_t input[BVB_VULKAN_INSTANCE_CREATE_RESPONSE_SIZE],
    struct bvb_vulkan_instance_create_response *response);
int bvb_protocol_encode_vulkan_instance_id(
    uint8_t output[BVB_VULKAN_INSTANCE_ID_SIZE], uint64_t instance_id);
int bvb_protocol_decode_vulkan_instance_id(
    const uint8_t input[BVB_VULKAN_INSTANCE_ID_SIZE], uint64_t *instance_id);
int bvb_protocol_encode_vulkan_physical_devices(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_physical_devices *devices,
    uint32_t *output_length);
int bvb_protocol_decode_vulkan_physical_devices(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_physical_devices *devices);
int bvb_protocol_encode_vulkan_physical_device_id(
    uint8_t output[BVB_VULKAN_PHYSICAL_DEVICE_ID_SIZE],
    uint64_t physical_device_id);
int bvb_protocol_decode_vulkan_physical_device_id(
    const uint8_t input[BVB_VULKAN_PHYSICAL_DEVICE_ID_SIZE],
    uint64_t *physical_device_id);
int bvb_protocol_encode_vulkan_format_query(
    uint8_t output[BVB_VULKAN_FORMAT_QUERY_SIZE],
    const struct bvb_vulkan_format_query *query);
int bvb_protocol_decode_vulkan_format_query(
    const uint8_t input[BVB_VULKAN_FORMAT_QUERY_SIZE],
    struct bvb_vulkan_format_query *query);
int bvb_protocol_encode_vulkan_format_properties(
    uint8_t output[BVB_VULKAN_FORMAT_PROPERTIES_SIZE],
    const struct bvb_vulkan_format_properties *properties);
int bvb_protocol_decode_vulkan_format_properties(
    const uint8_t input[BVB_VULKAN_FORMAT_PROPERTIES_SIZE],
    struct bvb_vulkan_format_properties *properties);
int bvb_protocol_encode_vulkan_image_format_query(
    uint8_t output[BVB_VULKAN_IMAGE_FORMAT_QUERY_SIZE],
    const struct bvb_vulkan_image_format_query *query);
int bvb_protocol_decode_vulkan_image_format_query(
    const uint8_t input[BVB_VULKAN_IMAGE_FORMAT_QUERY_SIZE],
    struct bvb_vulkan_image_format_query *query);
int bvb_protocol_encode_vulkan_image_format_properties(
    uint8_t output[BVB_VULKAN_IMAGE_FORMAT_PROPERTIES_SIZE],
    const struct bvb_vulkan_image_format_properties *properties);
int bvb_protocol_decode_vulkan_image_format_properties(
    const uint8_t input[BVB_VULKAN_IMAGE_FORMAT_PROPERTIES_SIZE],
    struct bvb_vulkan_image_format_properties *properties);
int bvb_protocol_encode_vulkan_external_buffer_query(
    uint8_t output[BVB_VULKAN_EXTERNAL_BUFFER_QUERY_SIZE],
    const struct bvb_vulkan_external_buffer_query *query);
int bvb_protocol_decode_vulkan_external_buffer_query(
    const uint8_t input[BVB_VULKAN_EXTERNAL_BUFFER_QUERY_SIZE],
    struct bvb_vulkan_external_buffer_query *query);
int bvb_protocol_encode_vulkan_external_buffer_properties(
    uint8_t output[BVB_VULKAN_EXTERNAL_BUFFER_PROPERTIES_SIZE],
    const struct bvb_vulkan_external_buffer_properties *properties);
int bvb_protocol_decode_vulkan_external_buffer_properties(
    const uint8_t input[BVB_VULKAN_EXTERNAL_BUFFER_PROPERTIES_SIZE],
    struct bvb_vulkan_external_buffer_properties *properties);
int bvb_protocol_encode_vulkan_external_semaphore_query(
    uint8_t output[BVB_VULKAN_EXTERNAL_SEMAPHORE_QUERY_SIZE],
    const struct bvb_vulkan_external_semaphore_query *query);
int bvb_protocol_decode_vulkan_external_semaphore_query(
    const uint8_t input[BVB_VULKAN_EXTERNAL_SEMAPHORE_QUERY_SIZE],
    struct bvb_vulkan_external_semaphore_query *query);
int bvb_protocol_encode_vulkan_external_semaphore_properties(
    uint8_t output[BVB_VULKAN_EXTERNAL_SEMAPHORE_PROPERTIES_SIZE],
    const struct bvb_vulkan_external_semaphore_properties *properties);
int bvb_protocol_decode_vulkan_external_semaphore_properties(
    const uint8_t input[BVB_VULKAN_EXTERNAL_SEMAPHORE_PROPERTIES_SIZE],
    struct bvb_vulkan_external_semaphore_properties *properties);
int bvb_protocol_encode_vulkan_core_features(
    uint8_t output[BVB_VULKAN_CORE_FEATURES_SIZE],
    const struct bvb_vulkan_core_features *features);
int bvb_protocol_decode_vulkan_core_features(
    const uint8_t input[BVB_VULKAN_CORE_FEATURES_SIZE],
    struct bvb_vulkan_core_features *features);
int bvb_protocol_encode_vulkan_device_extension_query(
    uint8_t output[BVB_VULKAN_DEVICE_EXTENSION_QUERY_SIZE],
    const struct bvb_vulkan_device_extension_query *query);
int bvb_protocol_decode_vulkan_device_extension_query(
    const uint8_t input[BVB_VULKAN_DEVICE_EXTENSION_QUERY_SIZE],
    struct bvb_vulkan_device_extension_query *query);
int bvb_protocol_encode_vulkan_device_create_request(
    uint8_t output[BVB_VULKAN_DEVICE_CREATE_REQUEST_SIZE],
    const struct bvb_vulkan_device_create_request *request);
int bvb_protocol_decode_vulkan_device_create_request(
    const uint8_t input[BVB_VULKAN_DEVICE_CREATE_REQUEST_SIZE],
    struct bvb_vulkan_device_create_request *request);
int bvb_protocol_encode_vulkan_device_create_extended_request(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_device_create_extended_request *request,
    uint32_t *output_length);
int bvb_protocol_decode_vulkan_device_create_extended_request(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_device_create_extended_request *request);
int bvb_protocol_encode_vulkan_device_create_packed_request(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_device_create_packed_request *request,
    uint32_t *output_length);
int bvb_protocol_decode_vulkan_device_create_packed_request(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_device_create_packed_request *request);
int bvb_protocol_encode_vulkan_device_create_response(
    uint8_t output[BVB_VULKAN_DEVICE_CREATE_RESPONSE_SIZE],
    const struct bvb_vulkan_device_create_response *response);
int bvb_protocol_decode_vulkan_device_create_response(
    const uint8_t input[BVB_VULKAN_DEVICE_CREATE_RESPONSE_SIZE],
    struct bvb_vulkan_device_create_response *response);
int bvb_protocol_encode_vulkan_device_id(
    uint8_t output[BVB_VULKAN_DEVICE_ID_SIZE], uint64_t device_id);
int bvb_protocol_decode_vulkan_device_id(
    const uint8_t input[BVB_VULKAN_DEVICE_ID_SIZE], uint64_t *device_id);
int bvb_protocol_encode_vulkan_device_queue_request(
    uint8_t output[BVB_VULKAN_DEVICE_QUEUE_REQUEST_SIZE],
    const struct bvb_vulkan_device_queue_request *request);
int bvb_protocol_decode_vulkan_device_queue_request(
    const uint8_t input[BVB_VULKAN_DEVICE_QUEUE_REQUEST_SIZE],
    struct bvb_vulkan_device_queue_request *request);
int bvb_protocol_encode_vulkan_queue_id(
    uint8_t output[BVB_VULKAN_QUEUE_ID_SIZE], uint64_t queue_id);
int bvb_protocol_decode_vulkan_queue_id(
    const uint8_t input[BVB_VULKAN_QUEUE_ID_SIZE], uint64_t *queue_id);
int bvb_protocol_encode_vulkan_result(
    uint8_t output[BVB_VULKAN_RESULT_SIZE], int32_t vulkan_result);
int bvb_protocol_decode_vulkan_result(
    const uint8_t input[BVB_VULKAN_RESULT_SIZE], int32_t *vulkan_result);
int bvb_protocol_encode_vulkan_command_pool_create_request(
    uint8_t output[BVB_VULKAN_COMMAND_POOL_CREATE_REQUEST_SIZE],
    const struct bvb_vulkan_command_pool_create_request *request);
int bvb_protocol_decode_vulkan_command_pool_create_request(
    const uint8_t input[BVB_VULKAN_COMMAND_POOL_CREATE_REQUEST_SIZE],
    struct bvb_vulkan_command_pool_create_request *request);
int bvb_protocol_encode_vulkan_command_pool_create_response(
    uint8_t output[BVB_VULKAN_COMMAND_POOL_CREATE_RESPONSE_SIZE],
    const struct bvb_vulkan_command_pool_create_response *response);
int bvb_protocol_decode_vulkan_command_pool_create_response(
    const uint8_t input[BVB_VULKAN_COMMAND_POOL_CREATE_RESPONSE_SIZE],
    struct bvb_vulkan_command_pool_create_response *response);
int bvb_protocol_encode_vulkan_command_pool_id(
    uint8_t output[BVB_VULKAN_COMMAND_POOL_ID_SIZE], uint64_t command_pool_id);
int bvb_protocol_decode_vulkan_command_pool_id(
    const uint8_t input[BVB_VULKAN_COMMAND_POOL_ID_SIZE],
    uint64_t *command_pool_id);
int bvb_protocol_encode_vulkan_command_pool_reset_request(
    uint8_t output[BVB_VULKAN_COMMAND_POOL_RESET_REQUEST_SIZE],
    const struct bvb_vulkan_command_pool_reset_request *request);
int bvb_protocol_decode_vulkan_command_pool_reset_request(
    const uint8_t input[BVB_VULKAN_COMMAND_POOL_RESET_REQUEST_SIZE],
    struct bvb_vulkan_command_pool_reset_request *request);
int bvb_protocol_encode_vulkan_command_buffer_allocate_request(
    uint8_t output[BVB_VULKAN_COMMAND_BUFFER_ALLOCATE_REQUEST_SIZE],
    const struct bvb_vulkan_command_buffer_allocate_request *request);
int bvb_protocol_decode_vulkan_command_buffer_allocate_request(
    const uint8_t input[BVB_VULKAN_COMMAND_BUFFER_ALLOCATE_REQUEST_SIZE],
    struct bvb_vulkan_command_buffer_allocate_request *request);
int bvb_protocol_encode_vulkan_command_buffer_allocate_response(
    uint8_t output[BVB_VULKAN_COMMAND_BUFFER_ALLOCATE_RESPONSE_SIZE],
    const struct bvb_vulkan_command_buffer_allocate_response *response);
int bvb_protocol_decode_vulkan_command_buffer_allocate_response(
    const uint8_t input[BVB_VULKAN_COMMAND_BUFFER_ALLOCATE_RESPONSE_SIZE],
    struct bvb_vulkan_command_buffer_allocate_response *response);
int bvb_protocol_encode_vulkan_command_buffer_free_request(
    uint8_t output[BVB_VULKAN_COMMAND_BUFFER_FREE_REQUEST_SIZE],
    const struct bvb_vulkan_command_buffer_free_request *request);
int bvb_protocol_decode_vulkan_command_buffer_free_request(
    const uint8_t input[BVB_VULKAN_COMMAND_BUFFER_FREE_REQUEST_SIZE],
    struct bvb_vulkan_command_buffer_free_request *request);
int bvb_protocol_encode_vulkan_command_buffer_begin_request(
    uint8_t output[BVB_VULKAN_COMMAND_BUFFER_BEGIN_REQUEST_SIZE],
    const struct bvb_vulkan_command_buffer_begin_request *request);
int bvb_protocol_decode_vulkan_command_buffer_begin_request(
    const uint8_t input[BVB_VULKAN_COMMAND_BUFFER_BEGIN_REQUEST_SIZE],
    struct bvb_vulkan_command_buffer_begin_request *request);
int bvb_protocol_encode_vulkan_command_buffer_id(
    uint8_t output[BVB_VULKAN_COMMAND_BUFFER_ID_SIZE],
    uint64_t command_buffer_id);
int bvb_protocol_decode_vulkan_command_buffer_id(
    const uint8_t input[BVB_VULKAN_COMMAND_BUFFER_ID_SIZE],
    uint64_t *command_buffer_id);
int bvb_protocol_encode_vulkan_queue_submit_command_request(
    uint8_t output[BVB_VULKAN_QUEUE_SUBMIT_COMMAND_REQUEST_SIZE],
    const struct bvb_vulkan_queue_submit_command_request *request);
int bvb_protocol_decode_vulkan_queue_submit_command_request(
    const uint8_t input[BVB_VULKAN_QUEUE_SUBMIT_COMMAND_REQUEST_SIZE],
    struct bvb_vulkan_queue_submit_command_request *request);
int bvb_protocol_encode_vulkan_buffer_create_request(
    uint8_t output[BVB_VULKAN_BUFFER_CREATE_REQUEST_SIZE],
    const struct bvb_vulkan_buffer_create_request *request);
int bvb_protocol_decode_vulkan_buffer_create_request(
    const uint8_t input[BVB_VULKAN_BUFFER_CREATE_REQUEST_SIZE],
    struct bvb_vulkan_buffer_create_request *request);
int bvb_protocol_encode_vulkan_object_create_response(
    uint8_t output[BVB_VULKAN_OBJECT_CREATE_RESPONSE_SIZE],
    const struct bvb_vulkan_object_create_response *response,
    uint8_t expected_type);
int bvb_protocol_decode_vulkan_object_create_response(
    const uint8_t input[BVB_VULKAN_OBJECT_CREATE_RESPONSE_SIZE],
    struct bvb_vulkan_object_create_response *response,
    uint8_t expected_type);
int bvb_protocol_encode_vulkan_object_id(
    uint8_t output[BVB_VULKAN_OBJECT_ID_SIZE], uint64_t object_id,
    uint8_t expected_type);
int bvb_protocol_decode_vulkan_object_id(
    const uint8_t input[BVB_VULKAN_OBJECT_ID_SIZE], uint64_t *object_id,
    uint8_t expected_type);
int bvb_protocol_encode_vulkan_buffer_requirements(
    uint8_t output[BVB_VULKAN_BUFFER_REQUIREMENTS_SIZE],
    const struct bvb_vulkan_buffer_requirements *requirements);
int bvb_protocol_decode_vulkan_buffer_requirements(
    const uint8_t input[BVB_VULKAN_BUFFER_REQUIREMENTS_SIZE],
    struct bvb_vulkan_buffer_requirements *requirements);
int bvb_protocol_encode_vulkan_memory_allocate_request(
    uint8_t output[BVB_VULKAN_MEMORY_ALLOCATE_REQUEST_SIZE],
    const struct bvb_vulkan_memory_allocate_request *request);
int bvb_protocol_decode_vulkan_memory_allocate_request(
    const uint8_t input[BVB_VULKAN_MEMORY_ALLOCATE_REQUEST_SIZE],
    struct bvb_vulkan_memory_allocate_request *request);
int bvb_protocol_encode_vulkan_memory_allocate_extended_request(
    uint8_t output[BVB_VULKAN_MEMORY_ALLOCATE_EXTENDED_REQUEST_SIZE],
    const struct bvb_vulkan_memory_allocate_extended_request *request);
int bvb_protocol_decode_vulkan_memory_allocate_extended_request(
    const uint8_t input[BVB_VULKAN_MEMORY_ALLOCATE_EXTENDED_REQUEST_SIZE],
    struct bvb_vulkan_memory_allocate_extended_request *request);
int bvb_protocol_encode_vulkan_buffer_bind_request(
    uint8_t output[BVB_VULKAN_BUFFER_BIND_REQUEST_SIZE],
    const struct bvb_vulkan_buffer_bind_request *request);
int bvb_protocol_decode_vulkan_buffer_bind_request(
    const uint8_t input[BVB_VULKAN_BUFFER_BIND_REQUEST_SIZE],
    struct bvb_vulkan_buffer_bind_request *request);
int bvb_protocol_encode_vulkan_image_create_request(
    uint8_t output[BVB_VULKAN_IMAGE_CREATE_REQUEST_SIZE],
    const struct bvb_vulkan_image_create_request *request);
int bvb_protocol_decode_vulkan_image_create_request(
    const uint8_t input[BVB_VULKAN_IMAGE_CREATE_REQUEST_SIZE],
    struct bvb_vulkan_image_create_request *request);
int bvb_protocol_encode_vulkan_image_requirements(
    uint8_t output[BVB_VULKAN_IMAGE_REQUIREMENTS_SIZE],
    const struct bvb_vulkan_image_requirements *requirements);
int bvb_protocol_decode_vulkan_image_requirements(
    const uint8_t input[BVB_VULKAN_IMAGE_REQUIREMENTS_SIZE],
    struct bvb_vulkan_image_requirements *requirements);
int bvb_protocol_encode_vulkan_image_requirements_2_request(
    uint8_t output[BVB_VULKAN_IMAGE_REQUIREMENTS_2_REQUEST_SIZE],
    const struct bvb_vulkan_image_requirements_2_request *request);
int bvb_protocol_decode_vulkan_image_requirements_2_request(
    const uint8_t input[BVB_VULKAN_IMAGE_REQUIREMENTS_2_REQUEST_SIZE],
    struct bvb_vulkan_image_requirements_2_request *request);
int bvb_protocol_encode_vulkan_image_requirements_2_response(
    uint8_t output[BVB_VULKAN_IMAGE_REQUIREMENTS_2_RESPONSE_SIZE],
    const struct bvb_vulkan_image_requirements_2_response *response);
int bvb_protocol_decode_vulkan_image_requirements_2_response(
    const uint8_t input[BVB_VULKAN_IMAGE_REQUIREMENTS_2_RESPONSE_SIZE],
    struct bvb_vulkan_image_requirements_2_response *response);
int bvb_protocol_encode_vulkan_image_bind_request(
    uint8_t output[BVB_VULKAN_IMAGE_BIND_REQUEST_SIZE],
    const struct bvb_vulkan_image_bind_request *request);
int bvb_protocol_decode_vulkan_image_bind_request(
    const uint8_t input[BVB_VULKAN_IMAGE_BIND_REQUEST_SIZE],
    struct bvb_vulkan_image_bind_request *request);
int bvb_protocol_encode_vulkan_image_view_create_request(
    uint8_t output[BVB_VULKAN_IMAGE_VIEW_CREATE_REQUEST_SIZE],
    const struct bvb_vulkan_image_view_create_request *request);
int bvb_protocol_decode_vulkan_image_view_create_request(
    const uint8_t input[BVB_VULKAN_IMAGE_VIEW_CREATE_REQUEST_SIZE],
    struct bvb_vulkan_image_view_create_request *request);
int bvb_protocol_encode_vulkan_command_buffer_fill_request(
    uint8_t output[BVB_VULKAN_COMMAND_BUFFER_FILL_REQUEST_SIZE],
    const struct bvb_vulkan_command_buffer_fill_request *request);
int bvb_protocol_decode_vulkan_command_buffer_fill_request(
    const uint8_t input[BVB_VULKAN_COMMAND_BUFFER_FILL_REQUEST_SIZE],
    struct bvb_vulkan_command_buffer_fill_request *request);
int bvb_protocol_encode_vulkan_memory_verify_fill_request(
    uint8_t output[BVB_VULKAN_MEMORY_VERIFY_FILL_REQUEST_SIZE],
    const struct bvb_vulkan_memory_verify_fill_request *request);
int bvb_protocol_decode_vulkan_memory_verify_fill_request(
    const uint8_t input[BVB_VULKAN_MEMORY_VERIFY_FILL_REQUEST_SIZE],
    struct bvb_vulkan_memory_verify_fill_request *request);
int bvb_protocol_encode_vulkan_memory_verify_fill_response(
    uint8_t output[BVB_VULKAN_MEMORY_VERIFY_FILL_RESPONSE_SIZE],
    const struct bvb_vulkan_memory_verify_fill_response *response);
int bvb_protocol_decode_vulkan_memory_verify_fill_response(
    const uint8_t input[BVB_VULKAN_MEMORY_VERIFY_FILL_RESPONSE_SIZE],
    struct bvb_vulkan_memory_verify_fill_response *response);
int bvb_protocol_encode_vulkan_fence_create_request(
    uint8_t output[BVB_VULKAN_FENCE_CREATE_REQUEST_SIZE],
    const struct bvb_vulkan_fence_create_request *request);
int bvb_protocol_decode_vulkan_fence_create_request(
    const uint8_t input[BVB_VULKAN_FENCE_CREATE_REQUEST_SIZE],
    struct bvb_vulkan_fence_create_request *request);
int bvb_protocol_encode_vulkan_fence_wait_request(
    uint8_t output[BVB_VULKAN_FENCE_WAIT_REQUEST_SIZE],
    const struct bvb_vulkan_fence_wait_request *request);
int bvb_protocol_decode_vulkan_fence_wait_request(
    const uint8_t input[BVB_VULKAN_FENCE_WAIT_REQUEST_SIZE],
    struct bvb_vulkan_fence_wait_request *request);
int bvb_protocol_encode_vulkan_queue_submit_command_fence_request(
    uint8_t output[BVB_VULKAN_QUEUE_SUBMIT_COMMAND_FENCE_REQUEST_SIZE],
    const struct bvb_vulkan_queue_submit_command_fence_request *request);
int bvb_protocol_decode_vulkan_queue_submit_command_fence_request(
    const uint8_t input[BVB_VULKAN_QUEUE_SUBMIT_COMMAND_FENCE_REQUEST_SIZE],
    struct bvb_vulkan_queue_submit_command_fence_request *request);
int bvb_protocol_encode_vulkan_semaphore_create_request(
    uint8_t output[BVB_VULKAN_SEMAPHORE_CREATE_REQUEST_SIZE],
    const struct bvb_vulkan_semaphore_create_request *request);
int bvb_protocol_decode_vulkan_semaphore_create_request(
    const uint8_t input[BVB_VULKAN_SEMAPHORE_CREATE_REQUEST_SIZE],
    struct bvb_vulkan_semaphore_create_request *request);
int bvb_protocol_encode_vulkan_semaphore_counter_response(
    uint8_t output[BVB_VULKAN_SEMAPHORE_COUNTER_RESPONSE_SIZE],
    const struct bvb_vulkan_semaphore_counter_response *response);
int bvb_protocol_decode_vulkan_semaphore_counter_response(
    const uint8_t input[BVB_VULKAN_SEMAPHORE_COUNTER_RESPONSE_SIZE],
    struct bvb_vulkan_semaphore_counter_response *response);
int bvb_protocol_encode_vulkan_semaphore_signal_request(
    uint8_t output[BVB_VULKAN_SEMAPHORE_SIGNAL_REQUEST_SIZE],
    const struct bvb_vulkan_semaphore_signal_request *request);
int bvb_protocol_decode_vulkan_semaphore_signal_request(
    const uint8_t input[BVB_VULKAN_SEMAPHORE_SIGNAL_REQUEST_SIZE],
    struct bvb_vulkan_semaphore_signal_request *request);
int bvb_protocol_encode_vulkan_semaphore_wait_request(
    uint8_t output[BVB_VULKAN_SEMAPHORE_WAIT_MAX_SIZE],
    const struct bvb_vulkan_semaphore_wait_request *request,
    uint32_t *output_length);
int bvb_protocol_decode_vulkan_semaphore_wait_request(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_semaphore_wait_request *request);
int bvb_protocol_encode_vulkan_queue_submit_2_request(
    uint8_t output[BVB_VULKAN_SUBMIT_2_MAX_SIZE],
    const struct bvb_vulkan_queue_submit_2_request *request,
    uint32_t *output_length);
int bvb_protocol_decode_vulkan_queue_submit_2_request(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_queue_submit_2_request *request);
int bvb_protocol_encode_vulkan_memory_write_request(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_memory_io_request *request,
    const uint8_t *data, uint32_t *output_length);
int bvb_protocol_decode_vulkan_memory_write_request(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_memory_io_request *request, const uint8_t **data);
int bvb_protocol_encode_vulkan_memory_read_request(
    uint8_t output[BVB_VULKAN_MEMORY_IO_PREFIX_SIZE],
    const struct bvb_vulkan_memory_io_request *request);
int bvb_protocol_decode_vulkan_memory_read_request(
    const uint8_t input[BVB_VULKAN_MEMORY_IO_PREFIX_SIZE],
    struct bvb_vulkan_memory_io_request *request);
int bvb_protocol_encode_vulkan_memory_io_response(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_memory_io_response *response,
    const uint8_t *data, uint32_t *output_length);
int bvb_protocol_decode_vulkan_memory_io_response(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_memory_io_response *response, const uint8_t **data);

void bvb_wire_put_u16(uint8_t *output, uint16_t value);
void bvb_wire_put_u32(uint8_t *output, uint32_t value);
void bvb_wire_put_i32(uint8_t *output, int32_t value);
void bvb_wire_put_u64(uint8_t *output, uint64_t value);
uint16_t bvb_wire_get_u16(const uint8_t *input);
uint32_t bvb_wire_get_u32(const uint8_t *input);
int32_t bvb_wire_get_i32(const uint8_t *input);
uint64_t bvb_wire_get_u64(const uint8_t *input);

#endif
