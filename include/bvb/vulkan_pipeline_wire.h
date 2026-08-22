#ifndef BVB_VULKAN_PIPELINE_WIRE_H
#define BVB_VULKAN_PIPELINE_WIRE_H

#include <bvb/protocol.h>

#include <stdint.h>

enum {
    BVB_VULKAN_PIPELINE_LAYOUT_CREATE_PREFIX_SIZE = 24,
    BVB_VULKAN_PIPELINE_PUSH_CONSTANT_RANGE_SIZE = 12,
    BVB_VULKAN_MAX_PIPELINE_SET_LAYOUTS = 8,
    BVB_VULKAN_MAX_PIPELINE_PUSH_CONSTANT_RANGES = 4,
    BVB_VULKAN_GRAPHICS_PIPELINE_CREATE_PREFIX_SIZE = 48,
    BVB_VULKAN_MAX_GRAPHICS_PIPELINE_DYNAMIC_STATES = 16,
    BVB_VULKAN_MAX_GRAPHICS_PIPELINE_SHADER_WORDS = 256,
    BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_REQUEST_SIZE = 32,
    BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_BLOB_HEADER_SIZE = 64,
    BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_ENTRY_SIZE = 16,
    BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_BLOB_MAGIC = 0x31475042,
    BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_BLOB_VERSION = 1,
    BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_VERTEX_CODE_SIZE = 1252,
    BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_FRAGMENT_CODE_SIZE = 15656,
    BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_ENTRY_COUNT = 8,
    BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_DATA_SIZE = 32,
    BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_BLOB_SIZE =
        BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_BLOB_HEADER_SIZE +
        BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_VERTEX_CODE_SIZE +
        BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_FRAGMENT_CODE_SIZE +
        BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_ENTRY_COUNT *
            BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_ENTRY_SIZE +
        BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_DATA_SIZE,
    BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_E103_BLOB_VERSION = 2,
    BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_BLOB_HEADER_SIZE = 88,
    BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_BLOB_MAGIC = 0x32475042,
    BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_BLOB_VERSION = 3,
    BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_BLOB_MAX_SIZE = 256U * 1024U,
    BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_STAGES = 5,
    BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_VERTEX_BINDINGS = 16,
    BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_VERTEX_ATTRIBUTES = 32,
    BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_COLOR_ATTACHMENTS = 8,
    BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_DYNAMIC_STATES = 32,
    BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_VIEWPORTS = 16,
    BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_SPEC_ENTRIES = 32,
    BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_SPEC_BYTES = 256,
};

struct bvb_vulkan_pipeline_push_constant_range {
    uint32_t stage_flags;
    uint32_t offset;
    uint32_t size;
};

struct bvb_vulkan_pipeline_layout_create_request {
    uint64_t device_id;
    uint32_t flags;
    uint32_t set_layout_count;
    uint32_t push_constant_range_count;
    uint64_t set_layout_ids[BVB_VULKAN_MAX_PIPELINE_SET_LAYOUTS];
    struct bvb_vulkan_pipeline_push_constant_range
        push_constant_ranges[BVB_VULKAN_MAX_PIPELINE_PUSH_CONSTANT_RANGES];
};

struct bvb_vulkan_graphics_pipeline_create_request {
    uint64_t device_id;
    uint64_t pipeline_layout_id;
    uint64_t flags_2;
    uint32_t library_flags;
    uint32_t shader_stage;
    uint32_t dynamic_state_count;
    uint32_t shader_word_count;
    uint32_t dynamic_states[BVB_VULKAN_MAX_GRAPHICS_PIPELINE_DYNAMIC_STATES];
    uint32_t shader_words[BVB_VULKAN_MAX_GRAPHICS_PIPELINE_SHADER_WORDS];
};

struct bvb_vulkan_builtin_graphics_pipeline_create_request {
    uint64_t device_id;
    uint64_t pipeline_layout_id;
    uint32_t blob_bytes;
    uint32_t schema;
};

struct bvb_vulkan_builtin_graphics_pipeline_blob_view {
    const uint32_t *vertex_words;
    const uint32_t *fragment_words;
    const uint8_t *specialization_entries;
    const uint8_t *specialization_data;
};

int bvb_protocol_encode_vulkan_pipeline_layout_create_request(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_pipeline_layout_create_request *request,
    uint32_t *output_length);
int bvb_protocol_decode_vulkan_pipeline_layout_create_request(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_pipeline_layout_create_request *request);
int bvb_protocol_encode_vulkan_graphics_pipeline_create_request(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_graphics_pipeline_create_request *request,
    uint32_t *output_length);
int bvb_protocol_decode_vulkan_graphics_pipeline_create_request(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_graphics_pipeline_create_request *request);
int bvb_protocol_encode_vulkan_builtin_graphics_pipeline_create_request(
    uint8_t output[BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_REQUEST_SIZE],
    const struct bvb_vulkan_builtin_graphics_pipeline_create_request *request);
int bvb_protocol_decode_vulkan_builtin_graphics_pipeline_create_request(
    const uint8_t input[BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_REQUEST_SIZE],
    struct bvb_vulkan_builtin_graphics_pipeline_create_request *request);
int bvb_protocol_encode_vulkan_builtin_graphics_pipeline_blob(
    uint8_t output[BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_BLOB_SIZE],
    const uint32_t *vertex_words, const uint32_t *fragment_words,
    const uint8_t *specialization_entries,
    const uint8_t *specialization_data);
int bvb_protocol_decode_vulkan_builtin_graphics_pipeline_blob(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_builtin_graphics_pipeline_blob_view *view);

#endif
