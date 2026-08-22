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

int bvb_protocol_encode_vulkan_graphics_pipeline_create_request(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_graphics_pipeline_create_request *request,
    uint32_t *output_length) {
    if (output == NULL || request == NULL || output_length == NULL ||
        !wire_id_is(request->device_id, BVB_OBJECT_DEVICE) ||
        !wire_id_is(request->pipeline_layout_id,
                    BVB_OBJECT_PIPELINE_LAYOUT) ||
        request->dynamic_state_count == 0U ||
        request->dynamic_state_count >
            BVB_VULKAN_MAX_GRAPHICS_PIPELINE_DYNAMIC_STATES ||
        request->shader_word_count == 0U ||
        request->shader_word_count >
            BVB_VULKAN_MAX_GRAPHICS_PIPELINE_SHADER_WORDS ||
        request->shader_words[0] != UINT32_C(0x07230203)) {
        return -EINVAL;
    }
    const uint32_t length =
        BVB_VULKAN_GRAPHICS_PIPELINE_CREATE_PREFIX_SIZE +
        (request->dynamic_state_count + request->shader_word_count) *
            sizeof(uint32_t);
    memset(output, 0, length);
    bvb_wire_put_u64(output, request->device_id);
    bvb_wire_put_u64(output + 8U, request->pipeline_layout_id);
    bvb_wire_put_u64(output + 16U, request->flags_2);
    bvb_wire_put_u32(output + 24U, request->library_flags);
    bvb_wire_put_u32(output + 28U, request->shader_stage);
    bvb_wire_put_u32(output + 32U, request->dynamic_state_count);
    bvb_wire_put_u32(output + 36U, request->shader_word_count);
    uint32_t cursor = BVB_VULKAN_GRAPHICS_PIPELINE_CREATE_PREFIX_SIZE;
    for (uint32_t index = 0U; index < request->dynamic_state_count; ++index) {
        bvb_wire_put_u32(output + cursor, request->dynamic_states[index]);
        cursor += sizeof(uint32_t);
    }
    for (uint32_t index = 0U; index < request->shader_word_count; ++index) {
        bvb_wire_put_u32(output + cursor, request->shader_words[index]);
        cursor += sizeof(uint32_t);
    }
    *output_length = length;
    return 0;
}

int bvb_protocol_decode_vulkan_graphics_pipeline_create_request(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_graphics_pipeline_create_request *request) {
    if (input == NULL || request == NULL ||
        input_length < BVB_VULKAN_GRAPHICS_PIPELINE_CREATE_PREFIX_SIZE) {
        return -EINVAL;
    }
    if (bvb_wire_get_u32(input + 40U) != 0U ||
        bvb_wire_get_u32(input + 44U) != 0U) {
        return -EPROTO;
    }
    struct bvb_vulkan_graphics_pipeline_create_request decoded = {
        .device_id = bvb_wire_get_u64(input),
        .pipeline_layout_id = bvb_wire_get_u64(input + 8U),
        .flags_2 = bvb_wire_get_u64(input + 16U),
        .library_flags = bvb_wire_get_u32(input + 24U),
        .shader_stage = bvb_wire_get_u32(input + 28U),
        .dynamic_state_count = bvb_wire_get_u32(input + 32U),
        .shader_word_count = bvb_wire_get_u32(input + 36U),
    };
    if (decoded.dynamic_state_count == 0U ||
        decoded.dynamic_state_count >
            BVB_VULKAN_MAX_GRAPHICS_PIPELINE_DYNAMIC_STATES ||
        decoded.shader_word_count == 0U ||
        decoded.shader_word_count >
            BVB_VULKAN_MAX_GRAPHICS_PIPELINE_SHADER_WORDS ||
        input_length != BVB_VULKAN_GRAPHICS_PIPELINE_CREATE_PREFIX_SIZE +
            (decoded.dynamic_state_count + decoded.shader_word_count) *
                sizeof(uint32_t)) {
        return -EPROTO;
    }
    uint32_t cursor = BVB_VULKAN_GRAPHICS_PIPELINE_CREATE_PREFIX_SIZE;
    for (uint32_t index = 0U; index < decoded.dynamic_state_count; ++index) {
        decoded.dynamic_states[index] = bvb_wire_get_u32(input + cursor);
        cursor += sizeof(uint32_t);
    }
    for (uint32_t index = 0U; index < decoded.shader_word_count; ++index) {
        decoded.shader_words[index] = bvb_wire_get_u32(input + cursor);
        cursor += sizeof(uint32_t);
    }
    uint8_t validation[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t validation_length = 0U;
    if (bvb_protocol_encode_vulkan_graphics_pipeline_create_request(
            validation, &decoded, &validation_length) != 0 ||
        validation_length != input_length ||
        memcmp(validation, input, input_length) != 0) {
        return -EPROTO;
    }
    *request = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_builtin_graphics_pipeline_create_request(
    uint8_t output[BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_REQUEST_SIZE],
    const struct bvb_vulkan_builtin_graphics_pipeline_create_request *request) {
    if (output == NULL || request == NULL ||
        !wire_id_is(request->device_id, BVB_OBJECT_DEVICE) ||
        !wire_id_is(request->pipeline_layout_id,
                    BVB_OBJECT_PIPELINE_LAYOUT) ||
        !((request->schema ==
               BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_BLOB_VERSION &&
           request->blob_bytes ==
               BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_BLOB_SIZE) ||
          (request->schema ==
               BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_BLOB_VERSION &&
           request->blob_bytes >=
               BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_BLOB_HEADER_SIZE &&
           request->blob_bytes <=
               BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_BLOB_MAX_SIZE &&
           (request->blob_bytes & 7U) == 0U))) {
        return -EINVAL;
    }
    memset(output, 0, BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_REQUEST_SIZE);
    bvb_wire_put_u64(output, request->device_id);
    bvb_wire_put_u64(output + 8U, request->pipeline_layout_id);
    bvb_wire_put_u32(output + 16U, request->blob_bytes);
    bvb_wire_put_u32(output + 20U, request->schema);
    return 0;
}

int bvb_protocol_decode_vulkan_builtin_graphics_pipeline_create_request(
    const uint8_t input[BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_REQUEST_SIZE],
    struct bvb_vulkan_builtin_graphics_pipeline_create_request *request) {
    if (input == NULL || request == NULL ||
        bvb_wire_get_u32(input + 24U) != 0U ||
        bvb_wire_get_u32(input + 28U) != 0U) {
        return -EPROTO;
    }
    const struct bvb_vulkan_builtin_graphics_pipeline_create_request decoded = {
        .device_id = bvb_wire_get_u64(input),
        .pipeline_layout_id = bvb_wire_get_u64(input + 8U),
        .blob_bytes = bvb_wire_get_u32(input + 16U),
        .schema = bvb_wire_get_u32(input + 20U),
    };
    uint8_t validation[BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_REQUEST_SIZE];
    if (bvb_protocol_encode_vulkan_builtin_graphics_pipeline_create_request(
            validation, &decoded) != 0 ||
        memcmp(validation, input, sizeof(validation)) != 0) {
        return -EPROTO;
    }
    *request = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_builtin_graphics_pipeline_blob(
    uint8_t output[BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_BLOB_SIZE],
    const uint32_t *vertex_words, const uint32_t *fragment_words,
    const uint8_t *specialization_entries,
    const uint8_t *specialization_data) {
    if (output == NULL || vertex_words == NULL || fragment_words == NULL ||
        specialization_entries == NULL || specialization_data == NULL ||
        vertex_words[0] != UINT32_C(0x07230203) ||
        fragment_words[0] != UINT32_C(0x07230203)) {
        return -EINVAL;
    }
    const uint32_t vertex_offset =
        BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_BLOB_HEADER_SIZE;
    const uint32_t fragment_offset = vertex_offset +
        BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_VERTEX_CODE_SIZE;
    const uint32_t specialization_offset = fragment_offset +
        BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_FRAGMENT_CODE_SIZE;
    const uint32_t data_offset = specialization_offset +
        BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_ENTRY_COUNT *
            BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_ENTRY_SIZE;
    memset(output, 0, BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_BLOB_SIZE);
    bvb_wire_put_u32(output,
                     BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_BLOB_MAGIC);
    bvb_wire_put_u32(output + 4U,
                     BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_BLOB_VERSION);
    bvb_wire_put_u32(output + 8U,
                     BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_BLOB_SIZE);
    bvb_wire_put_u32(output + 12U, vertex_offset);
    bvb_wire_put_u32(
        output + 16U, BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_VERTEX_CODE_SIZE);
    bvb_wire_put_u32(output + 20U, fragment_offset);
    bvb_wire_put_u32(
        output + 24U, BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_FRAGMENT_CODE_SIZE);
    bvb_wire_put_u32(output + 28U, specialization_offset);
    bvb_wire_put_u32(
        output + 32U, BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_ENTRY_COUNT);
    bvb_wire_put_u32(output + 36U, data_offset);
    bvb_wire_put_u32(
        output + 40U, BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_DATA_SIZE);
    for (uint32_t index = 0U;
         index < BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_VERTEX_CODE_SIZE / 4U;
         ++index) {
        bvb_wire_put_u32(output + vertex_offset + index * 4U,
                         vertex_words[index]);
    }
    for (uint32_t index = 0U;
         index < BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_FRAGMENT_CODE_SIZE / 4U;
         ++index) {
        bvb_wire_put_u32(output + fragment_offset + index * 4U,
                         fragment_words[index]);
    }
    memcpy(output + specialization_offset, specialization_entries,
           BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_ENTRY_COUNT *
               BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_ENTRY_SIZE);
    memcpy(output + data_offset, specialization_data,
           BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_DATA_SIZE);
    return 0;
}

int bvb_protocol_decode_vulkan_builtin_graphics_pipeline_blob(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_builtin_graphics_pipeline_blob_view *view) {
    if (input == NULL || view == NULL ||
        input_length != BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_BLOB_SIZE ||
        bvb_wire_get_u32(input) !=
            BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_BLOB_MAGIC ||
        bvb_wire_get_u32(input + 4U) !=
            BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_BLOB_VERSION ||
        bvb_wire_get_u32(input + 8U) != input_length ||
        bvb_wire_get_u32(input + 12U) !=
            BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_BLOB_HEADER_SIZE ||
        bvb_wire_get_u32(input + 16U) !=
            BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_VERTEX_CODE_SIZE ||
        bvb_wire_get_u32(input + 20U) !=
            BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_BLOB_HEADER_SIZE +
                BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_VERTEX_CODE_SIZE ||
        bvb_wire_get_u32(input + 24U) !=
            BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_FRAGMENT_CODE_SIZE ||
        bvb_wire_get_u32(input + 28U) !=
            BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_BLOB_HEADER_SIZE +
                BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_VERTEX_CODE_SIZE +
                BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_FRAGMENT_CODE_SIZE ||
        bvb_wire_get_u32(input + 32U) !=
            BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_ENTRY_COUNT ||
        bvb_wire_get_u32(input + 36U) !=
            BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_BLOB_SIZE -
                BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_DATA_SIZE ||
        bvb_wire_get_u32(input + 40U) !=
            BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_DATA_SIZE ||
        bvb_wire_get_u32(input + 44U) != 0U ||
        bvb_wire_get_u64(input + 48U) != 0U ||
        bvb_wire_get_u64(input + 56U) != 0U) {
        return -EPROTO;
    }
    const uint32_t vertex_offset = bvb_wire_get_u32(input + 12U);
    const uint32_t fragment_offset = bvb_wire_get_u32(input + 20U);
    const uint32_t specialization_offset = bvb_wire_get_u32(input + 28U);
    const uint32_t data_offset = bvb_wire_get_u32(input + 36U);
    if (bvb_wire_get_u32(input + vertex_offset) != UINT32_C(0x07230203) ||
        bvb_wire_get_u32(input + fragment_offset) != UINT32_C(0x07230203)) {
        return -EPROTO;
    }
    for (uint32_t index = 0U;
         index < BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_ENTRY_COUNT;
         ++index) {
        const uint8_t *entry = input + specialization_offset +
            index * BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_ENTRY_SIZE;
        const uint32_t offset = bvb_wire_get_u32(entry + 4U);
        const uint64_t size = bvb_wire_get_u64(entry + 8U);
        if (size == 0U || size > UINT32_MAX ||
            offset > BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_DATA_SIZE ||
            size > BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_DATA_SIZE -
                offset) {
            return -EPROTO;
        }
    }
    *view = (struct bvb_vulkan_builtin_graphics_pipeline_blob_view){
        .vertex_words = (const uint32_t *)(input + vertex_offset),
        .fragment_words = (const uint32_t *)(input + fragment_offset),
        .specialization_entries = input + specialization_offset,
        .specialization_data = input + data_offset,
    };
    return 0;
}
