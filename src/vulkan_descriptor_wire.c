#include <bvb/handle.h>
#include <bvb/vulkan_descriptor_wire.h>

#include <errno.h>
#include <string.h>

static int wire_id_is(uint64_t value, enum bvb_object_type type) {
    return (uint8_t)(value >> BVB_HANDLE_TYPE_SHIFT) == (uint8_t)type &&
        (value & BVB_HANDLE_SERIAL_MASK) != 0U;
}

int bvb_protocol_encode_vulkan_descriptor_set_layout_create_request(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_descriptor_set_layout_create_request *request,
    uint32_t *output_length) {
    if (output == NULL || request == NULL || output_length == NULL ||
        !wire_id_is(request->device_id, BVB_OBJECT_DEVICE) ||
        request->binding_count > BVB_VULKAN_MAX_DESCRIPTOR_LAYOUT_BINDINGS ||
        request->has_binding_flags > 1U) {
        return -EINVAL;
    }
    const uint32_t length = BVB_VULKAN_DESCRIPTOR_LAYOUT_PREFIX_SIZE +
        request->binding_count * BVB_VULKAN_DESCRIPTOR_LAYOUT_BINDING_SIZE;
    memset(output, 0, length);
    bvb_wire_put_u64(output, request->device_id);
    bvb_wire_put_u32(output + 8, request->flags);
    bvb_wire_put_u32(output + 12, request->binding_count);
    bvb_wire_put_u32(output + 16, request->has_binding_flags);
    uint32_t cursor = BVB_VULKAN_DESCRIPTOR_LAYOUT_PREFIX_SIZE;
    for (uint32_t index = 0U; index < request->binding_count; ++index) {
        const struct bvb_vulkan_descriptor_layout_binding *binding =
            &request->bindings[index];
        if (binding->descriptor_count == 0U ||
            binding->stage_flags == 0U ||
            (!request->has_binding_flags && binding->binding_flags != 0U)) {
            return -EINVAL;
        }
        for (uint32_t prior = 0U; prior < index; ++prior) {
            if (request->bindings[prior].binding == binding->binding) {
                return -EINVAL;
            }
        }
        bvb_wire_put_u32(output + cursor, binding->binding);
        bvb_wire_put_u32(output + cursor + 4, binding->descriptor_type);
        bvb_wire_put_u32(output + cursor + 8, binding->descriptor_count);
        bvb_wire_put_u32(output + cursor + 12, binding->stage_flags);
        bvb_wire_put_u32(output + cursor + 16, binding->binding_flags);
        cursor += BVB_VULKAN_DESCRIPTOR_LAYOUT_BINDING_SIZE;
    }
    *output_length = length;
    return 0;
}

int bvb_protocol_decode_vulkan_descriptor_set_layout_create_request(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_descriptor_set_layout_create_request *request) {
    if (input == NULL || request == NULL ||
        input_length < BVB_VULKAN_DESCRIPTOR_LAYOUT_PREFIX_SIZE ||
        bvb_wire_get_u32(input + 20) != 0U) {
        return -EINVAL;
    }
    struct bvb_vulkan_descriptor_set_layout_create_request decoded = {
        .device_id = bvb_wire_get_u64(input),
        .flags = bvb_wire_get_u32(input + 8),
        .binding_count = bvb_wire_get_u32(input + 12),
        .has_binding_flags = bvb_wire_get_u32(input + 16),
    };
    if (decoded.binding_count > BVB_VULKAN_MAX_DESCRIPTOR_LAYOUT_BINDINGS ||
        input_length != BVB_VULKAN_DESCRIPTOR_LAYOUT_PREFIX_SIZE +
            decoded.binding_count *
                BVB_VULKAN_DESCRIPTOR_LAYOUT_BINDING_SIZE) {
        return -EPROTO;
    }
    uint32_t cursor = BVB_VULKAN_DESCRIPTOR_LAYOUT_PREFIX_SIZE;
    for (uint32_t index = 0U; index < decoded.binding_count; ++index) {
        decoded.bindings[index] =
            (struct bvb_vulkan_descriptor_layout_binding){
                .binding = bvb_wire_get_u32(input + cursor),
                .descriptor_type = bvb_wire_get_u32(input + cursor + 4),
                .descriptor_count = bvb_wire_get_u32(input + cursor + 8),
                .stage_flags = bvb_wire_get_u32(input + cursor + 12),
                .binding_flags = bvb_wire_get_u32(input + cursor + 16),
            };
        cursor += BVB_VULKAN_DESCRIPTOR_LAYOUT_BINDING_SIZE;
    }
    uint8_t validation[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t validation_length = 0U;
    if (bvb_protocol_encode_vulkan_descriptor_set_layout_create_request(
            validation, &decoded, &validation_length) != 0) {
        return -EPROTO;
    }
    *request = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_descriptor_pool_create_request(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_descriptor_pool_create_request *request,
    uint32_t *output_length) {
    if (output == NULL || request == NULL || output_length == NULL ||
        !wire_id_is(request->device_id, BVB_OBJECT_DEVICE) ||
        request->max_sets == 0U || request->pool_size_count == 0U ||
        request->pool_size_count > BVB_VULKAN_MAX_DESCRIPTOR_POOL_SIZES) {
        return -EINVAL;
    }
    const uint32_t length = BVB_VULKAN_DESCRIPTOR_POOL_PREFIX_SIZE +
        request->pool_size_count * BVB_VULKAN_DESCRIPTOR_POOL_SIZE_RECORD_SIZE;
    memset(output, 0, length);
    bvb_wire_put_u64(output, request->device_id);
    bvb_wire_put_u32(output + 8, request->flags);
    bvb_wire_put_u32(output + 12, request->max_sets);
    bvb_wire_put_u32(output + 16, request->pool_size_count);
    uint32_t cursor = BVB_VULKAN_DESCRIPTOR_POOL_PREFIX_SIZE;
    for (uint32_t index = 0U; index < request->pool_size_count; ++index) {
        if (request->pool_sizes[index].descriptor_count == 0U) {
            return -EINVAL;
        }
        for (uint32_t prior = 0U; prior < index; ++prior) {
            if (request->pool_sizes[prior].descriptor_type ==
                request->pool_sizes[index].descriptor_type) {
                return -EINVAL;
            }
        }
        bvb_wire_put_u32(
            output + cursor, request->pool_sizes[index].descriptor_type);
        bvb_wire_put_u32(
            output + cursor + 4,
            request->pool_sizes[index].descriptor_count);
        cursor += BVB_VULKAN_DESCRIPTOR_POOL_SIZE_RECORD_SIZE;
    }
    *output_length = length;
    return 0;
}

int bvb_protocol_decode_vulkan_descriptor_pool_create_request(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_descriptor_pool_create_request *request) {
    if (input == NULL || request == NULL ||
        input_length < BVB_VULKAN_DESCRIPTOR_POOL_PREFIX_SIZE ||
        bvb_wire_get_u32(input + 20) != 0U) {
        return -EINVAL;
    }
    struct bvb_vulkan_descriptor_pool_create_request decoded = {
        .device_id = bvb_wire_get_u64(input),
        .flags = bvb_wire_get_u32(input + 8),
        .max_sets = bvb_wire_get_u32(input + 12),
        .pool_size_count = bvb_wire_get_u32(input + 16),
    };
    if (decoded.pool_size_count > BVB_VULKAN_MAX_DESCRIPTOR_POOL_SIZES ||
        input_length != BVB_VULKAN_DESCRIPTOR_POOL_PREFIX_SIZE +
            decoded.pool_size_count *
                BVB_VULKAN_DESCRIPTOR_POOL_SIZE_RECORD_SIZE) {
        return -EPROTO;
    }
    uint32_t cursor = BVB_VULKAN_DESCRIPTOR_POOL_PREFIX_SIZE;
    for (uint32_t index = 0U; index < decoded.pool_size_count; ++index) {
        decoded.pool_sizes[index] = (struct bvb_vulkan_descriptor_pool_size){
            .descriptor_type = bvb_wire_get_u32(input + cursor),
            .descriptor_count = bvb_wire_get_u32(input + cursor + 4),
        };
        cursor += BVB_VULKAN_DESCRIPTOR_POOL_SIZE_RECORD_SIZE;
    }
    uint8_t validation[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t validation_length = 0U;
    if (bvb_protocol_encode_vulkan_descriptor_pool_create_request(
            validation, &decoded, &validation_length) != 0) {
        return -EPROTO;
    }
    *request = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_descriptor_pool_reset_request(
    uint8_t output[BVB_VULKAN_DESCRIPTOR_POOL_RESET_REQUEST_SIZE],
    const struct bvb_vulkan_descriptor_pool_reset_request *request) {
    if (output == NULL || request == NULL ||
        !wire_id_is(request->descriptor_pool_id,
                    BVB_OBJECT_DESCRIPTOR_POOL) ||
        request->flags != 0U) {
        return -EINVAL;
    }
    memset(output, 0, BVB_VULKAN_DESCRIPTOR_POOL_RESET_REQUEST_SIZE);
    bvb_wire_put_u64(output, request->descriptor_pool_id);
    bvb_wire_put_u32(output + 8, request->flags);
    return 0;
}

int bvb_protocol_decode_vulkan_descriptor_pool_reset_request(
    const uint8_t input[BVB_VULKAN_DESCRIPTOR_POOL_RESET_REQUEST_SIZE],
    struct bvb_vulkan_descriptor_pool_reset_request *request) {
    if (input == NULL || request == NULL || bvb_wire_get_u32(input + 12) != 0U)
        return -EINVAL;
    const struct bvb_vulkan_descriptor_pool_reset_request decoded = {
        .descriptor_pool_id = bvb_wire_get_u64(input),
        .flags = bvb_wire_get_u32(input + 8),
    };
    uint8_t validation[BVB_VULKAN_DESCRIPTOR_POOL_RESET_REQUEST_SIZE];
    if (bvb_protocol_encode_vulkan_descriptor_pool_reset_request(
            validation, &decoded) != 0)
        return -EPROTO;
    *request = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_descriptor_set_allocate_request(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_descriptor_set_allocate_request *request,
    uint32_t *output_length) {
    if (output == NULL || request == NULL || output_length == NULL ||
        !wire_id_is(request->descriptor_pool_id,
                    BVB_OBJECT_DESCRIPTOR_POOL) ||
        request->descriptor_set_count == 0U ||
        request->descriptor_set_count >
            BVB_VULKAN_MAX_DESCRIPTOR_SETS_PER_ALLOCATE) {
        return -EINVAL;
    }
    const uint32_t length = BVB_VULKAN_DESCRIPTOR_SET_ALLOCATE_PREFIX_SIZE +
        request->descriptor_set_count * sizeof(uint64_t);
    memset(output, 0, length);
    bvb_wire_put_u64(output, request->descriptor_pool_id);
    bvb_wire_put_u32(output + 8, request->descriptor_set_count);
    for (uint32_t index = 0U; index < request->descriptor_set_count; ++index) {
        if (!wire_id_is(request->set_layout_ids[index],
                        BVB_OBJECT_DESCRIPTOR_SET_LAYOUT)) {
            return -EINVAL;
        }
        bvb_wire_put_u64(
            output + BVB_VULKAN_DESCRIPTOR_SET_ALLOCATE_PREFIX_SIZE +
                index * sizeof(uint64_t),
            request->set_layout_ids[index]);
    }
    *output_length = length;
    return 0;
}

int bvb_protocol_decode_vulkan_descriptor_set_allocate_request(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_descriptor_set_allocate_request *request) {
    if (input == NULL || request == NULL ||
        input_length < BVB_VULKAN_DESCRIPTOR_SET_ALLOCATE_PREFIX_SIZE ||
        bvb_wire_get_u32(input + 12) != 0U) {
        return -EINVAL;
    }
    struct bvb_vulkan_descriptor_set_allocate_request decoded = {
        .descriptor_pool_id = bvb_wire_get_u64(input),
        .descriptor_set_count = bvb_wire_get_u32(input + 8),
    };
    if (decoded.descriptor_set_count == 0U ||
        decoded.descriptor_set_count >
            BVB_VULKAN_MAX_DESCRIPTOR_SETS_PER_ALLOCATE ||
        input_length != BVB_VULKAN_DESCRIPTOR_SET_ALLOCATE_PREFIX_SIZE +
            decoded.descriptor_set_count * sizeof(uint64_t)) {
        return -EPROTO;
    }
    for (uint32_t index = 0U; index < decoded.descriptor_set_count; ++index) {
        decoded.set_layout_ids[index] = bvb_wire_get_u64(
            input + BVB_VULKAN_DESCRIPTOR_SET_ALLOCATE_PREFIX_SIZE +
            index * sizeof(uint64_t));
    }
    uint8_t validation[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t validation_length = 0U;
    if (bvb_protocol_encode_vulkan_descriptor_set_allocate_request(
            validation, &decoded, &validation_length) != 0) {
        return -EPROTO;
    }
    *request = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_descriptor_set_allocate_response(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_descriptor_set_allocate_response *response,
    uint32_t *output_length) {
    if (output == NULL || response == NULL || output_length == NULL ||
        response->descriptor_set_count >
            BVB_VULKAN_MAX_DESCRIPTOR_SETS_PER_ALLOCATE ||
        (response->vulkan_result == 0 &&
         response->descriptor_set_count == 0U) ||
        (response->vulkan_result != 0 &&
         response->descriptor_set_count != 0U)) {
        return -EINVAL;
    }
    const uint32_t length =
        BVB_VULKAN_DESCRIPTOR_SET_ALLOCATE_RESPONSE_PREFIX_SIZE +
        response->descriptor_set_count * sizeof(uint64_t);
    memset(output, 0, length);
    bvb_wire_put_i32(output, response->vulkan_result);
    bvb_wire_put_u32(output + 4, response->descriptor_set_count);
    for (uint32_t index = 0U; index < response->descriptor_set_count; ++index) {
        if (!wire_id_is(response->descriptor_set_ids[index],
                        BVB_OBJECT_DESCRIPTOR_SET)) {
            return -EINVAL;
        }
        bvb_wire_put_u64(
            output + BVB_VULKAN_DESCRIPTOR_SET_ALLOCATE_RESPONSE_PREFIX_SIZE +
                index * sizeof(uint64_t),
            response->descriptor_set_ids[index]);
    }
    *output_length = length;
    return 0;
}

int bvb_protocol_decode_vulkan_descriptor_set_allocate_response(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_descriptor_set_allocate_response *response) {
    if (input == NULL || response == NULL ||
        input_length <
            BVB_VULKAN_DESCRIPTOR_SET_ALLOCATE_RESPONSE_PREFIX_SIZE) {
        return -EINVAL;
    }
    struct bvb_vulkan_descriptor_set_allocate_response decoded = {
        .vulkan_result = bvb_wire_get_i32(input),
        .descriptor_set_count = bvb_wire_get_u32(input + 4),
    };
    if (decoded.descriptor_set_count >
            BVB_VULKAN_MAX_DESCRIPTOR_SETS_PER_ALLOCATE ||
        input_length !=
            BVB_VULKAN_DESCRIPTOR_SET_ALLOCATE_RESPONSE_PREFIX_SIZE +
                decoded.descriptor_set_count * sizeof(uint64_t)) {
        return -EPROTO;
    }
    for (uint32_t index = 0U; index < decoded.descriptor_set_count; ++index) {
        decoded.descriptor_set_ids[index] = bvb_wire_get_u64(
            input +
                BVB_VULKAN_DESCRIPTOR_SET_ALLOCATE_RESPONSE_PREFIX_SIZE +
                index * sizeof(uint64_t));
    }
    uint8_t validation[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t validation_length = 0U;
    if (bvb_protocol_encode_vulkan_descriptor_set_allocate_response(
            validation, &decoded, &validation_length) != 0) {
        return -EPROTO;
    }
    *response = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_descriptor_transaction_allocate_request(
    uint8_t output[BVB_VULKAN_DESCRIPTOR_TRANSACTION_ALLOCATE_MAX_SIZE],
    const struct bvb_vulkan_descriptor_transaction_allocate_request *request,
    uint32_t *output_length) {
    if (output == NULL || request == NULL || output_length == NULL ||
        request->journal_generation == 0U ||
        request->journal_sequence == 0U ||
        request->journal_length > BVB_DESCRIPTOR_JOURNAL_REGION_BYTES ||
        request->journal_record_count > BVB_DESCRIPTOR_JOURNAL_MAX_RECORDS ||
        ((request->journal_length == 0U) !=
         (request->journal_record_count == 0U))) {
        return -EINVAL;
    }
    uint8_t allocation_wire[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t allocation_length = 0U;
    int result = bvb_protocol_encode_vulkan_descriptor_set_allocate_request(
        allocation_wire, &request->allocation, &allocation_length);
    if (result != 0 ||
        allocation_length > BVB_VULKAN_DESCRIPTOR_TRANSACTION_ALLOCATE_MAX_SIZE -
                                BVB_VULKAN_DESCRIPTOR_TRANSACTION_ALLOCATE_PREFIX_SIZE) {
        return result != 0 ? result : -E2BIG;
    }
    bvb_wire_put_u64(output, request->journal_generation);
    bvb_wire_put_u64(output + 8, request->journal_sequence);
    bvb_wire_put_u32(output + 16, request->journal_length);
    bvb_wire_put_u32(output + 20, request->journal_record_count);
    bvb_wire_put_u32(output + 24, allocation_length);
    bvb_wire_put_u32(output + 28, 0U);
    memcpy(output + BVB_VULKAN_DESCRIPTOR_TRANSACTION_ALLOCATE_PREFIX_SIZE,
           allocation_wire, allocation_length);
    *output_length =
        BVB_VULKAN_DESCRIPTOR_TRANSACTION_ALLOCATE_PREFIX_SIZE +
        allocation_length;
    return 0;
}

int bvb_protocol_decode_vulkan_descriptor_transaction_allocate_request(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_descriptor_transaction_allocate_request *request) {
    if (input == NULL || request == NULL ||
        input_length < BVB_VULKAN_DESCRIPTOR_TRANSACTION_ALLOCATE_PREFIX_SIZE +
                           BVB_VULKAN_DESCRIPTOR_SET_ALLOCATE_PREFIX_SIZE) {
        return -EINVAL;
    }
    const uint32_t allocation_length = bvb_wire_get_u32(input + 24);
    struct bvb_vulkan_descriptor_transaction_allocate_request decoded = {
        .journal_generation = bvb_wire_get_u64(input),
        .journal_sequence = bvb_wire_get_u64(input + 8),
        .journal_length = bvb_wire_get_u32(input + 16),
        .journal_record_count = bvb_wire_get_u32(input + 20),
    };
    if (decoded.journal_generation == 0U ||
        decoded.journal_sequence == 0U ||
        decoded.journal_length > BVB_DESCRIPTOR_JOURNAL_REGION_BYTES ||
        decoded.journal_record_count > BVB_DESCRIPTOR_JOURNAL_MAX_RECORDS ||
        ((decoded.journal_length == 0U) !=
         (decoded.journal_record_count == 0U)) ||
        bvb_wire_get_u32(input + 28) != 0U ||
        allocation_length >
            BVB_VULKAN_DESCRIPTOR_TRANSACTION_ALLOCATE_MAX_SIZE -
                BVB_VULKAN_DESCRIPTOR_TRANSACTION_ALLOCATE_PREFIX_SIZE ||
        input_length !=
            BVB_VULKAN_DESCRIPTOR_TRANSACTION_ALLOCATE_PREFIX_SIZE +
                allocation_length) {
        return -EPROTO;
    }
    int result = bvb_protocol_decode_vulkan_descriptor_set_allocate_request(
        input + BVB_VULKAN_DESCRIPTOR_TRANSACTION_ALLOCATE_PREFIX_SIZE,
        allocation_length, &decoded.allocation);
    if (result != 0) return result;
    uint8_t canonical[BVB_VULKAN_DESCRIPTOR_TRANSACTION_ALLOCATE_MAX_SIZE];
    uint32_t canonical_length = 0U;
    result = bvb_protocol_encode_vulkan_descriptor_transaction_allocate_request(
        canonical, &decoded, &canonical_length);
    if (result != 0 || canonical_length != input_length ||
        memcmp(canonical, input, input_length) != 0) {
        return -EPROTO;
    }
    *request = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_sampler_create_request(
    uint8_t output[BVB_VULKAN_SAMPLER_CREATE_REQUEST_SIZE],
    const struct bvb_vulkan_sampler_create_request *request) {
    if (output == NULL || request == NULL ||
        !wire_id_is(request->device_id, BVB_OBJECT_DEVICE) ||
        request->anisotropy_enable > 1U || request->compare_enable > 1U ||
        request->unnormalized_coordinates > 1U) {
        return -EINVAL;
    }
    memset(output, 0, BVB_VULKAN_SAMPLER_CREATE_REQUEST_SIZE);
    bvb_wire_put_u64(output, request->device_id);
    const uint32_t values[16] = {
        request->flags, request->mag_filter, request->min_filter,
        request->mipmap_mode, request->address_mode_u,
        request->address_mode_v, request->address_mode_w,
        request->mip_lod_bias_bits, request->anisotropy_enable,
        request->max_anisotropy_bits, request->compare_enable,
        request->compare_op, request->min_lod_bits, request->max_lod_bits,
        request->border_color, request->unnormalized_coordinates,
    };
    for (uint32_t index = 0U; index < 16U; ++index) {
        bvb_wire_put_u32(output + 8 + index * sizeof(uint32_t), values[index]);
    }
    return 0;
}

int bvb_protocol_decode_vulkan_sampler_create_request(
    const uint8_t input[BVB_VULKAN_SAMPLER_CREATE_REQUEST_SIZE],
    struct bvb_vulkan_sampler_create_request *request) {
    if (input == NULL || request == NULL) return -EINVAL;
    const struct bvb_vulkan_sampler_create_request decoded = {
        .device_id = bvb_wire_get_u64(input),
        .flags = bvb_wire_get_u32(input + 8),
        .mag_filter = bvb_wire_get_u32(input + 12),
        .min_filter = bvb_wire_get_u32(input + 16),
        .mipmap_mode = bvb_wire_get_u32(input + 20),
        .address_mode_u = bvb_wire_get_u32(input + 24),
        .address_mode_v = bvb_wire_get_u32(input + 28),
        .address_mode_w = bvb_wire_get_u32(input + 32),
        .mip_lod_bias_bits = bvb_wire_get_u32(input + 36),
        .anisotropy_enable = bvb_wire_get_u32(input + 40),
        .max_anisotropy_bits = bvb_wire_get_u32(input + 44),
        .compare_enable = bvb_wire_get_u32(input + 48),
        .compare_op = bvb_wire_get_u32(input + 52),
        .min_lod_bits = bvb_wire_get_u32(input + 56),
        .max_lod_bits = bvb_wire_get_u32(input + 60),
        .border_color = bvb_wire_get_u32(input + 64),
        .unnormalized_coordinates = bvb_wire_get_u32(input + 68),
    };
    uint8_t validation[BVB_VULKAN_SAMPLER_CREATE_REQUEST_SIZE];
    if (bvb_protocol_encode_vulkan_sampler_create_request(
            validation, &decoded) != 0) {
        return -EPROTO;
    }
    *request = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_descriptor_update_request(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_descriptor_update_request *request,
    uint32_t *output_length) {
    if (output == NULL || request == NULL || output_length == NULL ||
        !wire_id_is(request->device_id, BVB_OBJECT_DEVICE) ||
        request->write_count == 0U ||
        request->write_count > BVB_VULKAN_MAX_DESCRIPTOR_WRITES ||
        request->sampler_count == 0U ||
        request->sampler_count > BVB_VULKAN_MAX_DESCRIPTOR_SAMPLERS) {
        return -EINVAL;
    }
    const uint32_t length = BVB_VULKAN_DESCRIPTOR_UPDATE_PREFIX_SIZE +
        request->write_count * BVB_VULKAN_DESCRIPTOR_WRITE_SIZE +
        request->sampler_count * sizeof(uint64_t);
    memset(output, 0, length);
    bvb_wire_put_u64(output, request->device_id);
    bvb_wire_put_u32(output + 8, request->write_count);
    bvb_wire_put_u32(output + 12, request->sampler_count);
    uint32_t cursor = BVB_VULKAN_DESCRIPTOR_UPDATE_PREFIX_SIZE;
    for (uint32_t index = 0U; index < request->write_count; ++index) {
        const struct bvb_vulkan_descriptor_write *write =
            &request->writes[index];
        if (!wire_id_is(write->descriptor_set_id,
                        BVB_OBJECT_DESCRIPTOR_SET) ||
            write->descriptor_count == 0U ||
            write->first_sampler >= request->sampler_count ||
            write->descriptor_count >
                request->sampler_count - write->first_sampler) {
            return -EINVAL;
        }
        bvb_wire_put_u64(output + cursor, write->descriptor_set_id);
        bvb_wire_put_u32(output + cursor + 8, write->dst_binding);
        bvb_wire_put_u32(output + cursor + 12, write->dst_array_element);
        bvb_wire_put_u32(output + cursor + 16, write->descriptor_count);
        bvb_wire_put_u32(output + cursor + 20, write->descriptor_type);
        bvb_wire_put_u32(output + cursor + 24, write->first_sampler);
        cursor += BVB_VULKAN_DESCRIPTOR_WRITE_SIZE;
    }
    for (uint32_t index = 0U; index < request->sampler_count; ++index) {
        if (!wire_id_is(request->sampler_ids[index], BVB_OBJECT_SAMPLER)) {
            return -EINVAL;
        }
        bvb_wire_put_u64(output + cursor, request->sampler_ids[index]);
        cursor += sizeof(uint64_t);
    }
    *output_length = length;
    return 0;
}

int bvb_protocol_decode_vulkan_descriptor_update_request(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_descriptor_update_request *request) {
    if (input == NULL || request == NULL ||
        input_length < BVB_VULKAN_DESCRIPTOR_UPDATE_PREFIX_SIZE) {
        return -EINVAL;
    }
    struct bvb_vulkan_descriptor_update_request decoded = {
        .device_id = bvb_wire_get_u64(input),
        .write_count = bvb_wire_get_u32(input + 8),
        .sampler_count = bvb_wire_get_u32(input + 12),
    };
    if (decoded.write_count > BVB_VULKAN_MAX_DESCRIPTOR_WRITES ||
        decoded.sampler_count > BVB_VULKAN_MAX_DESCRIPTOR_SAMPLERS ||
        input_length != BVB_VULKAN_DESCRIPTOR_UPDATE_PREFIX_SIZE +
            decoded.write_count * BVB_VULKAN_DESCRIPTOR_WRITE_SIZE +
            decoded.sampler_count * sizeof(uint64_t)) {
        return -EPROTO;
    }
    uint32_t cursor = BVB_VULKAN_DESCRIPTOR_UPDATE_PREFIX_SIZE;
    for (uint32_t index = 0U; index < decoded.write_count; ++index) {
        decoded.writes[index] = (struct bvb_vulkan_descriptor_write){
            .descriptor_set_id = bvb_wire_get_u64(input + cursor),
            .dst_binding = bvb_wire_get_u32(input + cursor + 8),
            .dst_array_element = bvb_wire_get_u32(input + cursor + 12),
            .descriptor_count = bvb_wire_get_u32(input + cursor + 16),
            .descriptor_type = bvb_wire_get_u32(input + cursor + 20),
            .first_sampler = bvb_wire_get_u32(input + cursor + 24),
        };
        if (bvb_wire_get_u32(input + cursor + 28) != 0U) return -EPROTO;
        cursor += BVB_VULKAN_DESCRIPTOR_WRITE_SIZE;
    }
    for (uint32_t index = 0U; index < decoded.sampler_count; ++index) {
        decoded.sampler_ids[index] = bvb_wire_get_u64(input + cursor);
        cursor += sizeof(uint64_t);
    }
    uint8_t validation[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t validation_length = 0U;
    if (bvb_protocol_encode_vulkan_descriptor_update_request(
            validation, &decoded, &validation_length) != 0) {
        return -EPROTO;
    }
    *request = decoded;
    return 0;
}

static int descriptor_template_entry_is_bounded(
    const struct bvb_vulkan_descriptor_update_template_entry *entry) {
    if (entry == NULL || entry->descriptor_count == 0U ||
        entry->descriptor_type > 10U || entry->stride == 0U ||
        entry->offset >= BVB_VULKAN_MAX_DESCRIPTOR_UPDATE_TEMPLATE_DATA_SIZE ||
        entry->stride > BVB_VULKAN_MAX_DESCRIPTOR_UPDATE_TEMPLATE_DATA_SIZE) {
        return 0;
    }
    const uint64_t remaining = (uint64_t)entry->descriptor_count - 1U;
    return remaining <=
        (BVB_VULKAN_MAX_DESCRIPTOR_UPDATE_TEMPLATE_DATA_SIZE - 1U -
         entry->offset) / entry->stride;
}

int bvb_protocol_encode_vulkan_descriptor_update_template_create_request(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_descriptor_update_template_create_request *request,
    uint32_t *output_length) {
    if (output == NULL || request == NULL || output_length == NULL ||
        !wire_id_is(request->device_id, BVB_OBJECT_DEVICE) ||
        !wire_id_is(request->descriptor_set_layout_id,
                    BVB_OBJECT_DESCRIPTOR_SET_LAYOUT) ||
        request->flags != 0U || request->entry_count == 0U ||
        request->entry_count >
            BVB_VULKAN_MAX_DESCRIPTOR_UPDATE_TEMPLATE_ENTRIES ||
        request->template_type != 0U || request->set != 0U ||
        request->pipeline_layout_id != 0U ||
        request->pipeline_bind_point != 0U) {
        return -EINVAL;
    }
    const uint32_t length =
        BVB_VULKAN_DESCRIPTOR_UPDATE_TEMPLATE_PREFIX_SIZE +
        request->entry_count *
            BVB_VULKAN_DESCRIPTOR_UPDATE_TEMPLATE_ENTRY_SIZE;
    memset(output, 0, length);
    bvb_wire_put_u64(output, request->device_id);
    bvb_wire_put_u32(output + 8, request->flags);
    bvb_wire_put_u32(output + 12, request->entry_count);
    bvb_wire_put_u32(output + 16, request->template_type);
    bvb_wire_put_u32(output + 20, request->set);
    bvb_wire_put_u64(output + 24, request->descriptor_set_layout_id);
    bvb_wire_put_u64(output + 32, request->pipeline_layout_id);
    bvb_wire_put_u32(output + 40, request->pipeline_bind_point);
    uint32_t cursor = BVB_VULKAN_DESCRIPTOR_UPDATE_TEMPLATE_PREFIX_SIZE;
    for (uint32_t index = 0U; index < request->entry_count; ++index) {
        const struct bvb_vulkan_descriptor_update_template_entry *entry =
            &request->entries[index];
        if (!descriptor_template_entry_is_bounded(entry)) return -EINVAL;
        bvb_wire_put_u32(output + cursor, entry->dst_binding);
        bvb_wire_put_u32(output + cursor + 4, entry->dst_array_element);
        bvb_wire_put_u32(output + cursor + 8, entry->descriptor_count);
        bvb_wire_put_u32(output + cursor + 12, entry->descriptor_type);
        bvb_wire_put_u64(output + cursor + 16, entry->offset);
        bvb_wire_put_u64(output + cursor + 24, entry->stride);
        cursor += BVB_VULKAN_DESCRIPTOR_UPDATE_TEMPLATE_ENTRY_SIZE;
    }
    *output_length = length;
    return 0;
}

int bvb_protocol_decode_vulkan_descriptor_update_template_create_request(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_descriptor_update_template_create_request *request) {
    if (input == NULL || request == NULL ||
        input_length < BVB_VULKAN_DESCRIPTOR_UPDATE_TEMPLATE_PREFIX_SIZE ||
        bvb_wire_get_u32(input + 44) != 0U) {
        return -EINVAL;
    }
    struct bvb_vulkan_descriptor_update_template_create_request decoded = {
        .device_id = bvb_wire_get_u64(input),
        .flags = bvb_wire_get_u32(input + 8),
        .entry_count = bvb_wire_get_u32(input + 12),
        .template_type = bvb_wire_get_u32(input + 16),
        .set = bvb_wire_get_u32(input + 20),
        .descriptor_set_layout_id = bvb_wire_get_u64(input + 24),
        .pipeline_layout_id = bvb_wire_get_u64(input + 32),
        .pipeline_bind_point = bvb_wire_get_u32(input + 40),
    };
    if (decoded.entry_count == 0U ||
        decoded.entry_count >
            BVB_VULKAN_MAX_DESCRIPTOR_UPDATE_TEMPLATE_ENTRIES ||
        input_length != BVB_VULKAN_DESCRIPTOR_UPDATE_TEMPLATE_PREFIX_SIZE +
            decoded.entry_count *
                BVB_VULKAN_DESCRIPTOR_UPDATE_TEMPLATE_ENTRY_SIZE) {
        return -EPROTO;
    }
    uint32_t cursor = BVB_VULKAN_DESCRIPTOR_UPDATE_TEMPLATE_PREFIX_SIZE;
    for (uint32_t index = 0U; index < decoded.entry_count; ++index) {
        decoded.entries[index] =
            (struct bvb_vulkan_descriptor_update_template_entry){
                .dst_binding = bvb_wire_get_u32(input + cursor),
                .dst_array_element = bvb_wire_get_u32(input + cursor + 4),
                .descriptor_count = bvb_wire_get_u32(input + cursor + 8),
                .descriptor_type = bvb_wire_get_u32(input + cursor + 12),
                .offset = bvb_wire_get_u64(input + cursor + 16),
                .stride = bvb_wire_get_u64(input + cursor + 24),
            };
        cursor += BVB_VULKAN_DESCRIPTOR_UPDATE_TEMPLATE_ENTRY_SIZE;
    }
    uint8_t validation[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t validation_length = 0U;
    if (bvb_protocol_encode_vulkan_descriptor_update_template_create_request(
            validation, &decoded, &validation_length) != 0 ||
        validation_length != input_length ||
        memcmp(validation, input, input_length) != 0) {
        return -EPROTO;
    }
    *request = decoded;
    return 0;
}

static int descriptor_template_value_is_canonical(
    const struct bvb_vulkan_descriptor_template_value *value) {
    if (value == NULL || value->descriptor_type > 10U) return 0;
    switch (value->descriptor_type) {
    case 0U:
        return (value->object_id == 0U ||
                wire_id_is(value->object_id, BVB_OBJECT_SAMPLER)) &&
            value->auxiliary_object_id == 0U && value->image_layout == 0U &&
            value->offset == 0U && value->range == 0U;
    case 1U:
        return (value->object_id == 0U ||
                wire_id_is(value->object_id, BVB_OBJECT_IMAGE_VIEW)) &&
            (value->auxiliary_object_id == 0U ||
             wire_id_is(value->auxiliary_object_id, BVB_OBJECT_SAMPLER)) &&
            value->offset == 0U && value->range == 0U;
    case 2U:
    case 3U:
    case 10U:
        return (value->object_id == 0U ||
                wire_id_is(value->object_id, BVB_OBJECT_IMAGE_VIEW)) &&
            value->auxiliary_object_id == 0U && value->offset == 0U &&
            value->range == 0U;
    case 4U:
    case 5U:
        return value->object_id == 0U && value->auxiliary_object_id == 0U &&
            value->image_layout == 0U && value->offset == 0U &&
            value->range == 0U;
    case 6U:
    case 7U:
    case 8U:
    case 9U:
        return (value->object_id == 0U ||
                wire_id_is(value->object_id, BVB_OBJECT_BUFFER)) &&
            value->auxiliary_object_id == 0U && value->image_layout == 0U;
    default:
        return 0;
    }
}

int bvb_protocol_encode_vulkan_descriptor_template_update_request(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_descriptor_template_update_request *request,
    uint32_t *output_length) {
    if (output == NULL || request == NULL || output_length == NULL ||
        !wire_id_is(request->device_id, BVB_OBJECT_DEVICE) ||
        !wire_id_is(request->descriptor_set_id, BVB_OBJECT_DESCRIPTOR_SET) ||
        !wire_id_is(request->descriptor_update_template_id,
                    BVB_OBJECT_DESCRIPTOR_UPDATE_TEMPLATE) ||
        request->value_count == 0U ||
        request->value_count > BVB_VULKAN_MAX_DESCRIPTOR_TEMPLATE_VALUES) {
        return -EINVAL;
    }
    const uint32_t length = BVB_VULKAN_DESCRIPTOR_TEMPLATE_UPDATE_PREFIX_SIZE +
        request->value_count * BVB_VULKAN_DESCRIPTOR_TEMPLATE_VALUE_SIZE;
    memset(output, 0, length);
    bvb_wire_put_u64(output, request->device_id);
    bvb_wire_put_u64(output + 8, request->descriptor_set_id);
    bvb_wire_put_u64(output + 16,
                     request->descriptor_update_template_id);
    bvb_wire_put_u32(output + 24, request->value_count);
    uint32_t cursor = BVB_VULKAN_DESCRIPTOR_TEMPLATE_UPDATE_PREFIX_SIZE;
    for (uint32_t index = 0U; index < request->value_count; ++index) {
        const struct bvb_vulkan_descriptor_template_value *value =
            &request->values[index];
        if (!descriptor_template_value_is_canonical(value)) return -EINVAL;
        bvb_wire_put_u32(output + cursor, value->descriptor_type);
        bvb_wire_put_u32(output + cursor + 4, value->image_layout);
        bvb_wire_put_u64(output + cursor + 8, value->object_id);
        bvb_wire_put_u64(output + cursor + 16, value->auxiliary_object_id);
        bvb_wire_put_u64(output + cursor + 24, value->offset);
        bvb_wire_put_u64(output + cursor + 32, value->range);
        cursor += BVB_VULKAN_DESCRIPTOR_TEMPLATE_VALUE_SIZE;
    }
    *output_length = length;
    return 0;
}

int bvb_protocol_decode_vulkan_descriptor_template_update_request(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_descriptor_template_update_request *request) {
    if (input == NULL || request == NULL ||
        input_length < BVB_VULKAN_DESCRIPTOR_TEMPLATE_UPDATE_PREFIX_SIZE ||
        bvb_wire_get_u32(input + 28) != 0U) return -EINVAL;
    struct bvb_vulkan_descriptor_template_update_request decoded = {
        .device_id = bvb_wire_get_u64(input),
        .descriptor_set_id = bvb_wire_get_u64(input + 8),
        .descriptor_update_template_id = bvb_wire_get_u64(input + 16),
        .value_count = bvb_wire_get_u32(input + 24),
    };
    if (decoded.value_count == 0U ||
        decoded.value_count > BVB_VULKAN_MAX_DESCRIPTOR_TEMPLATE_VALUES ||
        input_length != BVB_VULKAN_DESCRIPTOR_TEMPLATE_UPDATE_PREFIX_SIZE +
            decoded.value_count * BVB_VULKAN_DESCRIPTOR_TEMPLATE_VALUE_SIZE) {
        return -EPROTO;
    }
    uint32_t cursor = BVB_VULKAN_DESCRIPTOR_TEMPLATE_UPDATE_PREFIX_SIZE;
    for (uint32_t index = 0U; index < decoded.value_count; ++index) {
        decoded.values[index] =
            (struct bvb_vulkan_descriptor_template_value){
                .descriptor_type = bvb_wire_get_u32(input + cursor),
                .image_layout = bvb_wire_get_u32(input + cursor + 4),
                .object_id = bvb_wire_get_u64(input + cursor + 8),
                .auxiliary_object_id = bvb_wire_get_u64(input + cursor + 16),
                .offset = bvb_wire_get_u64(input + cursor + 24),
                .range = bvb_wire_get_u64(input + cursor + 32),
            };
        cursor += BVB_VULKAN_DESCRIPTOR_TEMPLATE_VALUE_SIZE;
    }
    uint8_t validation[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t validation_length = 0U;
    if (bvb_protocol_encode_vulkan_descriptor_template_update_request(
            validation, &decoded, &validation_length) != 0 ||
        validation_length != input_length ||
        memcmp(validation, input, input_length) != 0) return -EPROTO;
    *request = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_bind_descriptor_sets_request(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_bind_descriptor_sets_request *request,
    uint32_t *output_length) {
    if (output == NULL || request == NULL || output_length == NULL ||
        !wire_id_is(request->command_buffer_id, BVB_OBJECT_COMMAND_BUFFER) ||
        !wire_id_is(request->pipeline_layout_id, BVB_OBJECT_PIPELINE_LAYOUT) ||
        request->pipeline_bind_point > 1U ||
        request->descriptor_set_count == 0U ||
        request->descriptor_set_count > BVB_VULKAN_MAX_BOUND_DESCRIPTOR_SETS ||
        request->dynamic_offset_count > BVB_VULKAN_MAX_DYNAMIC_OFFSETS) {
        return -EINVAL;
    }
    const uint32_t length = BVB_VULKAN_BIND_DESCRIPTOR_SETS_PREFIX_SIZE +
        request->descriptor_set_count * sizeof(uint64_t) +
        request->dynamic_offset_count * sizeof(uint32_t);
    memset(output, 0, length);
    bvb_wire_put_u64(output, request->command_buffer_id);
    bvb_wire_put_u64(output + 8, request->pipeline_layout_id);
    bvb_wire_put_u32(output + 16, request->pipeline_bind_point);
    bvb_wire_put_u32(output + 20, request->first_set);
    bvb_wire_put_u32(output + 24, request->descriptor_set_count);
    bvb_wire_put_u32(output + 28, request->dynamic_offset_count);
    uint32_t cursor = BVB_VULKAN_BIND_DESCRIPTOR_SETS_PREFIX_SIZE;
    for (uint32_t index = 0U; index < request->descriptor_set_count; ++index) {
        if (!wire_id_is(request->descriptor_set_ids[index],
                        BVB_OBJECT_DESCRIPTOR_SET)) return -EINVAL;
        bvb_wire_put_u64(output + cursor, request->descriptor_set_ids[index]);
        cursor += sizeof(uint64_t);
    }
    for (uint32_t index = 0U; index < request->dynamic_offset_count; ++index) {
        bvb_wire_put_u32(output + cursor, request->dynamic_offsets[index]);
        cursor += sizeof(uint32_t);
    }
    *output_length = length;
    return 0;
}

int bvb_protocol_decode_vulkan_bind_descriptor_sets_request(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_bind_descriptor_sets_request *request) {
    if (input == NULL || request == NULL ||
        input_length < BVB_VULKAN_BIND_DESCRIPTOR_SETS_PREFIX_SIZE) {
        return -EINVAL;
    }
    struct bvb_vulkan_bind_descriptor_sets_request decoded = {
        .command_buffer_id = bvb_wire_get_u64(input),
        .pipeline_layout_id = bvb_wire_get_u64(input + 8),
        .pipeline_bind_point = bvb_wire_get_u32(input + 16),
        .first_set = bvb_wire_get_u32(input + 20),
        .descriptor_set_count = bvb_wire_get_u32(input + 24),
        .dynamic_offset_count = bvb_wire_get_u32(input + 28),
    };
    if (decoded.descriptor_set_count == 0U ||
        decoded.descriptor_set_count > BVB_VULKAN_MAX_BOUND_DESCRIPTOR_SETS ||
        decoded.dynamic_offset_count > BVB_VULKAN_MAX_DYNAMIC_OFFSETS ||
        input_length != BVB_VULKAN_BIND_DESCRIPTOR_SETS_PREFIX_SIZE +
            decoded.descriptor_set_count * sizeof(uint64_t) +
            decoded.dynamic_offset_count * sizeof(uint32_t)) return -EPROTO;
    uint32_t cursor = BVB_VULKAN_BIND_DESCRIPTOR_SETS_PREFIX_SIZE;
    for (uint32_t index = 0U; index < decoded.descriptor_set_count; ++index) {
        decoded.descriptor_set_ids[index] = bvb_wire_get_u64(input + cursor);
        cursor += sizeof(uint64_t);
    }
    for (uint32_t index = 0U; index < decoded.dynamic_offset_count; ++index) {
        decoded.dynamic_offsets[index] = bvb_wire_get_u32(input + cursor);
        cursor += sizeof(uint32_t);
    }
    uint8_t validation[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t validation_length = 0U;
    if (bvb_protocol_encode_vulkan_bind_descriptor_sets_request(
            validation, &decoded, &validation_length) != 0 ||
        validation_length != input_length ||
        memcmp(validation, input, input_length) != 0) return -EPROTO;
    *request = decoded;
    return 0;
}
