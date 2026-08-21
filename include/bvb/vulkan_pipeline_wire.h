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

#endif
