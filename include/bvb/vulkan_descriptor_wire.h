#ifndef BVB_VULKAN_DESCRIPTOR_WIRE_H
#define BVB_VULKAN_DESCRIPTOR_WIRE_H

#include <bvb/protocol.h>

#include <stdint.h>

enum {
    BVB_VULKAN_DESCRIPTOR_LAYOUT_PREFIX_SIZE = 24,
    BVB_VULKAN_DESCRIPTOR_LAYOUT_BINDING_SIZE = 20,
    BVB_VULKAN_MAX_DESCRIPTOR_LAYOUT_BINDINGS = 32,
    BVB_VULKAN_DESCRIPTOR_POOL_PREFIX_SIZE = 24,
    BVB_VULKAN_DESCRIPTOR_POOL_SIZE_RECORD_SIZE = 8,
    BVB_VULKAN_MAX_DESCRIPTOR_POOL_SIZES = 16,
    BVB_VULKAN_DESCRIPTOR_SET_ALLOCATE_PREFIX_SIZE = 16,
    BVB_VULKAN_DESCRIPTOR_SET_ALLOCATE_RESPONSE_PREFIX_SIZE = 8,
    BVB_VULKAN_MAX_DESCRIPTOR_SETS_PER_ALLOCATE = 16,
    BVB_VULKAN_SAMPLER_CREATE_REQUEST_SIZE = 72,
    BVB_VULKAN_DESCRIPTOR_UPDATE_PREFIX_SIZE = 16,
    BVB_VULKAN_DESCRIPTOR_WRITE_SIZE = 32,
    BVB_VULKAN_MAX_DESCRIPTOR_WRITES = 32,
    BVB_VULKAN_MAX_DESCRIPTOR_SAMPLERS = 128,
    BVB_VULKAN_DESCRIPTOR_UPDATE_TEMPLATE_PREFIX_SIZE = 48,
    BVB_VULKAN_DESCRIPTOR_UPDATE_TEMPLATE_ENTRY_SIZE = 32,
    BVB_VULKAN_MAX_DESCRIPTOR_UPDATE_TEMPLATE_ENTRIES = 32,
    BVB_VULKAN_MAX_DESCRIPTOR_UPDATE_TEMPLATE_DATA_SIZE = 1U << 20,
};

struct bvb_vulkan_descriptor_layout_binding {
    uint32_t binding;
    uint32_t descriptor_type;
    uint32_t descriptor_count;
    uint32_t stage_flags;
    uint32_t binding_flags;
};

struct bvb_vulkan_descriptor_set_layout_create_request {
    uint64_t device_id;
    uint32_t flags;
    uint32_t binding_count;
    uint32_t has_binding_flags;
    struct bvb_vulkan_descriptor_layout_binding
        bindings[BVB_VULKAN_MAX_DESCRIPTOR_LAYOUT_BINDINGS];
};

struct bvb_vulkan_descriptor_pool_size {
    uint32_t descriptor_type;
    uint32_t descriptor_count;
};

struct bvb_vulkan_descriptor_pool_create_request {
    uint64_t device_id;
    uint32_t flags;
    uint32_t max_sets;
    uint32_t pool_size_count;
    struct bvb_vulkan_descriptor_pool_size
        pool_sizes[BVB_VULKAN_MAX_DESCRIPTOR_POOL_SIZES];
};

struct bvb_vulkan_descriptor_set_allocate_request {
    uint64_t descriptor_pool_id;
    uint32_t descriptor_set_count;
    uint64_t set_layout_ids[BVB_VULKAN_MAX_DESCRIPTOR_SETS_PER_ALLOCATE];
};

struct bvb_vulkan_descriptor_set_allocate_response {
    int32_t vulkan_result;
    uint32_t descriptor_set_count;
    uint64_t descriptor_set_ids[BVB_VULKAN_MAX_DESCRIPTOR_SETS_PER_ALLOCATE];
};

struct bvb_vulkan_sampler_create_request {
    uint64_t device_id;
    uint32_t flags;
    uint32_t mag_filter;
    uint32_t min_filter;
    uint32_t mipmap_mode;
    uint32_t address_mode_u;
    uint32_t address_mode_v;
    uint32_t address_mode_w;
    uint32_t mip_lod_bias_bits;
    uint32_t anisotropy_enable;
    uint32_t max_anisotropy_bits;
    uint32_t compare_enable;
    uint32_t compare_op;
    uint32_t min_lod_bits;
    uint32_t max_lod_bits;
    uint32_t border_color;
    uint32_t unnormalized_coordinates;
};

struct bvb_vulkan_descriptor_write {
    uint64_t descriptor_set_id;
    uint32_t dst_binding;
    uint32_t dst_array_element;
    uint32_t descriptor_count;
    uint32_t descriptor_type;
    uint32_t first_sampler;
};

struct bvb_vulkan_descriptor_update_request {
    uint64_t device_id;
    uint32_t write_count;
    uint32_t sampler_count;
    struct bvb_vulkan_descriptor_write
        writes[BVB_VULKAN_MAX_DESCRIPTOR_WRITES];
    uint64_t sampler_ids[BVB_VULKAN_MAX_DESCRIPTOR_SAMPLERS];
};

struct bvb_vulkan_descriptor_update_template_entry {
    uint32_t dst_binding;
    uint32_t dst_array_element;
    uint32_t descriptor_count;
    uint32_t descriptor_type;
    uint64_t offset;
    uint64_t stride;
};

struct bvb_vulkan_descriptor_update_template_create_request {
    uint64_t device_id;
    uint32_t flags;
    uint32_t entry_count;
    uint32_t template_type;
    uint32_t set;
    uint64_t descriptor_set_layout_id;
    uint64_t pipeline_layout_id;
    uint32_t pipeline_bind_point;
    struct bvb_vulkan_descriptor_update_template_entry
        entries[BVB_VULKAN_MAX_DESCRIPTOR_UPDATE_TEMPLATE_ENTRIES];
};

int bvb_protocol_encode_vulkan_descriptor_set_layout_create_request(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_descriptor_set_layout_create_request *request,
    uint32_t *output_length);
int bvb_protocol_decode_vulkan_descriptor_set_layout_create_request(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_descriptor_set_layout_create_request *request);
int bvb_protocol_encode_vulkan_descriptor_pool_create_request(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_descriptor_pool_create_request *request,
    uint32_t *output_length);
int bvb_protocol_decode_vulkan_descriptor_pool_create_request(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_descriptor_pool_create_request *request);
int bvb_protocol_encode_vulkan_descriptor_set_allocate_request(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_descriptor_set_allocate_request *request,
    uint32_t *output_length);
int bvb_protocol_decode_vulkan_descriptor_set_allocate_request(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_descriptor_set_allocate_request *request);
int bvb_protocol_encode_vulkan_descriptor_set_allocate_response(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_descriptor_set_allocate_response *response,
    uint32_t *output_length);
int bvb_protocol_decode_vulkan_descriptor_set_allocate_response(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_descriptor_set_allocate_response *response);
int bvb_protocol_encode_vulkan_sampler_create_request(
    uint8_t output[BVB_VULKAN_SAMPLER_CREATE_REQUEST_SIZE],
    const struct bvb_vulkan_sampler_create_request *request);
int bvb_protocol_decode_vulkan_sampler_create_request(
    const uint8_t input[BVB_VULKAN_SAMPLER_CREATE_REQUEST_SIZE],
    struct bvb_vulkan_sampler_create_request *request);
int bvb_protocol_encode_vulkan_descriptor_update_request(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_descriptor_update_request *request,
    uint32_t *output_length);
int bvb_protocol_decode_vulkan_descriptor_update_request(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_descriptor_update_request *request);
int bvb_protocol_encode_vulkan_descriptor_update_template_create_request(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_descriptor_update_template_create_request *request,
    uint32_t *output_length);
int bvb_protocol_decode_vulkan_descriptor_update_template_create_request(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_descriptor_update_template_create_request *request);

#endif
