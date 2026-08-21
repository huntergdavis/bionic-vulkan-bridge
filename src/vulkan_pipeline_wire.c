#include <bvb/handle.h>
#include <bvb/vulkan_pipeline_wire.h>

#include <errno.h>
#include <string.h>

static int wire_id_is(uint64_t value, enum bvb_object_type type) {
    return (uint8_t)(value >> BVB_HANDLE_TYPE_SHIFT) == (uint8_t)type &&
        (value & BVB_HANDLE_SERIAL_MASK) != 0U;
}

int bvb_protocol_encode_vulkan_pipeline_layout_create_request(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_pipeline_layout_create_request *request,
    uint32_t *output_length) {
    if (output == NULL || request == NULL || output_length == NULL ||
        !wire_id_is(request->device_id, BVB_OBJECT_DEVICE) ||
        request->set_layout_count > BVB_VULKAN_MAX_PIPELINE_SET_LAYOUTS ||
        request->push_constant_range_count >
            BVB_VULKAN_MAX_PIPELINE_PUSH_CONSTANT_RANGES) {
        return -EINVAL;
    }
    const uint32_t length = BVB_VULKAN_PIPELINE_LAYOUT_CREATE_PREFIX_SIZE +
        request->set_layout_count * sizeof(uint64_t) +
        request->push_constant_range_count *
            BVB_VULKAN_PIPELINE_PUSH_CONSTANT_RANGE_SIZE;
    memset(output, 0, length);
    bvb_wire_put_u64(output, request->device_id);
    bvb_wire_put_u32(output + 8U, request->flags);
    bvb_wire_put_u32(output + 12U, request->set_layout_count);
    bvb_wire_put_u32(output + 16U, request->push_constant_range_count);
    uint32_t cursor = BVB_VULKAN_PIPELINE_LAYOUT_CREATE_PREFIX_SIZE;
    for (uint32_t index = 0U; index < request->set_layout_count; ++index) {
        if (request->set_layout_ids[index] != 0U &&
            !wire_id_is(request->set_layout_ids[index],
                        BVB_OBJECT_DESCRIPTOR_SET_LAYOUT)) {
            return -EINVAL;
        }
        bvb_wire_put_u64(output + cursor, request->set_layout_ids[index]);
        cursor += sizeof(uint64_t);
    }
    for (uint32_t index = 0U;
         index < request->push_constant_range_count; ++index) {
        const struct bvb_vulkan_pipeline_push_constant_range *range =
            &request->push_constant_ranges[index];
        if (range->stage_flags == 0U || range->size == 0U ||
            (range->offset & 3U) != 0U || (range->size & 3U) != 0U ||
            range->offset > UINT32_MAX - range->size) {
            return -EINVAL;
        }
        bvb_wire_put_u32(output + cursor, range->stage_flags);
        bvb_wire_put_u32(output + cursor + 4U, range->offset);
        bvb_wire_put_u32(output + cursor + 8U, range->size);
        cursor += BVB_VULKAN_PIPELINE_PUSH_CONSTANT_RANGE_SIZE;
    }
    *output_length = length;
    return 0;
}

int bvb_protocol_decode_vulkan_pipeline_layout_create_request(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_pipeline_layout_create_request *request) {
    if (input == NULL || request == NULL ||
        input_length < BVB_VULKAN_PIPELINE_LAYOUT_CREATE_PREFIX_SIZE) {
        return -EINVAL;
    }
    if (bvb_wire_get_u32(input + 20U) != 0U) return -EPROTO;
    struct bvb_vulkan_pipeline_layout_create_request decoded = {
        .device_id = bvb_wire_get_u64(input),
        .flags = bvb_wire_get_u32(input + 8U),
        .set_layout_count = bvb_wire_get_u32(input + 12U),
        .push_constant_range_count = bvb_wire_get_u32(input + 16U),
    };
    if (decoded.set_layout_count > BVB_VULKAN_MAX_PIPELINE_SET_LAYOUTS ||
        decoded.push_constant_range_count >
            BVB_VULKAN_MAX_PIPELINE_PUSH_CONSTANT_RANGES ||
        input_length != BVB_VULKAN_PIPELINE_LAYOUT_CREATE_PREFIX_SIZE +
            decoded.set_layout_count * sizeof(uint64_t) +
            decoded.push_constant_range_count *
                BVB_VULKAN_PIPELINE_PUSH_CONSTANT_RANGE_SIZE) {
        return -EPROTO;
    }
    uint32_t cursor = BVB_VULKAN_PIPELINE_LAYOUT_CREATE_PREFIX_SIZE;
    for (uint32_t index = 0U; index < decoded.set_layout_count; ++index) {
        decoded.set_layout_ids[index] = bvb_wire_get_u64(input + cursor);
        cursor += sizeof(uint64_t);
    }
    for (uint32_t index = 0U;
         index < decoded.push_constant_range_count; ++index) {
        decoded.push_constant_ranges[index] =
            (struct bvb_vulkan_pipeline_push_constant_range){
                .stage_flags = bvb_wire_get_u32(input + cursor),
                .offset = bvb_wire_get_u32(input + cursor + 4U),
                .size = bvb_wire_get_u32(input + cursor + 8U),
            };
        cursor += BVB_VULKAN_PIPELINE_PUSH_CONSTANT_RANGE_SIZE;
    }
    uint8_t validation[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t validation_length = 0U;
    if (bvb_protocol_encode_vulkan_pipeline_layout_create_request(
            validation, &decoded, &validation_length) != 0 ||
        validation_length != input_length ||
        memcmp(validation, input, input_length) != 0) {
        return -EPROTO;
    }
    *request = decoded;
    return 0;
}
