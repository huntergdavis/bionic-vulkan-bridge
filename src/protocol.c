#include <bvb/command_batch.h>
#include <bvb/protocol.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

void bvb_wire_put_u16(uint8_t *output, uint16_t value) {
    output[0] = (uint8_t)(value & 0xffU);
    output[1] = (uint8_t)(value >> 8);
}

void bvb_wire_put_u32(uint8_t *output, uint32_t value) {
    output[0] = (uint8_t)(value & 0xffU);
    output[1] = (uint8_t)((value >> 8) & 0xffU);
    output[2] = (uint8_t)((value >> 16) & 0xffU);
    output[3] = (uint8_t)(value >> 24);
}

void bvb_wire_put_i32(uint8_t *output, int32_t value) {
    bvb_wire_put_u32(output, (uint32_t)value);
}

void bvb_wire_put_u64(uint8_t *output, uint64_t value) {
    bvb_wire_put_u32(output, (uint32_t)value);
    bvb_wire_put_u32(output + 4, (uint32_t)(value >> 32));
}

uint16_t bvb_wire_get_u16(const uint8_t *input) {
    return (uint16_t)((uint16_t)input[0] | ((uint16_t)input[1] << 8));
}

uint32_t bvb_wire_get_u32(const uint8_t *input) {
    return (uint32_t)input[0] | ((uint32_t)input[1] << 8) |
           ((uint32_t)input[2] << 16) | ((uint32_t)input[3] << 24);
}

int32_t bvb_wire_get_i32(const uint8_t *input) {
    return (int32_t)bvb_wire_get_u32(input);
}

uint64_t bvb_wire_get_u64(const uint8_t *input) {
    return (uint64_t)bvb_wire_get_u32(input) |
           ((uint64_t)bvb_wire_get_u32(input + 4) << 32);
}

static int header_is_valid(const struct bvb_protocol_header *header) {
    if (header == NULL || header->version != BVB_PROTOCOL_VERSION) {
        return -EPROTO;
    }
    if (header->kind != BVB_PROTOCOL_REQUEST &&
        header->kind != BVB_PROTOCOL_RESPONSE) {
        return -EPROTO;
    }
    if (header->opcode < BVB_OPCODE_HELLO ||
        header->opcode > BVB_OPCODE_VISIBLE_BATCH_EXECUTE) {
        return -EPROTO;
    }
    if (header->payload_length > BVB_PROTOCOL_MAX_PAYLOAD) {
        return -EMSGSIZE;
    }
    if (header->kind == BVB_PROTOCOL_REQUEST && header->status != 0) {
        return -EPROTO;
    }
    return 0;
}

int bvb_protocol_encode_header(
    uint8_t output[BVB_PROTOCOL_HEADER_SIZE],
    const struct bvb_protocol_header *header) {
    if (output == NULL || header == NULL) {
        return -EINVAL;
    }
    int result = header_is_valid(header);
    if (result != 0) {
        return result;
    }

    bvb_wire_put_u32(output, BVB_PROTOCOL_MAGIC);
    bvb_wire_put_u16(output + 4, header->version);
    bvb_wire_put_u16(output + 6, header->kind);
    bvb_wire_put_u16(output + 8, header->opcode);
    bvb_wire_put_u16(output + 10, 0);
    bvb_wire_put_u32(output + 12, header->request_id);
    bvb_wire_put_u32(output + 16, header->payload_length);
    bvb_wire_put_i32(output + 20, header->status);
    return 0;
}

int bvb_protocol_decode_header(
    const uint8_t input[BVB_PROTOCOL_HEADER_SIZE],
    struct bvb_protocol_header *header) {
    if (input == NULL || header == NULL) {
        return -EINVAL;
    }
    if (bvb_wire_get_u32(input) != BVB_PROTOCOL_MAGIC ||
        bvb_wire_get_u16(input + 10) != 0U) {
        return -EPROTO;
    }

    const struct bvb_protocol_header decoded = {
        .version = bvb_wire_get_u16(input + 4),
        .kind = bvb_wire_get_u16(input + 6),
        .opcode = bvb_wire_get_u16(input + 8),
        .request_id = bvb_wire_get_u32(input + 12),
        .payload_length = bvb_wire_get_u32(input + 16),
        .status = bvb_wire_get_i32(input + 20),
    };
    int result = header_is_valid(&decoded);
    if (result != 0) {
        return result;
    }
    *header = decoded;
    return 0;
}

int bvb_protocol_encode_hello_request(
    uint8_t output[BVB_HELLO_REQUEST_SIZE],
    const struct bvb_hello_request *request) {
    if (output == NULL || request == NULL || request->minimum_version == 0U ||
        request->minimum_version > request->maximum_version) {
        return -EINVAL;
    }
    bvb_wire_put_u16(output, request->minimum_version);
    bvb_wire_put_u16(output + 2, request->maximum_version);
    bvb_wire_put_u32(output + 4, request->client_flags);
    return 0;
}

int bvb_protocol_decode_hello_request(
    const uint8_t input[BVB_HELLO_REQUEST_SIZE],
    struct bvb_hello_request *request) {
    if (input == NULL || request == NULL) {
        return -EINVAL;
    }
    const struct bvb_hello_request decoded = {
        .minimum_version = bvb_wire_get_u16(input),
        .maximum_version = bvb_wire_get_u16(input + 2),
        .client_flags = bvb_wire_get_u32(input + 4),
    };
    if (decoded.minimum_version == 0U ||
        decoded.minimum_version > decoded.maximum_version) {
        return -EPROTO;
    }
    *request = decoded;
    return 0;
}

int bvb_protocol_encode_hello_response(
    uint8_t output[BVB_HELLO_RESPONSE_SIZE],
    const struct bvb_hello_response *response) {
    if (output == NULL || response == NULL ||
        response->negotiated_version == 0U || response->pointer_bits == 0U ||
        response->page_size == 0U) {
        return -EINVAL;
    }
    bvb_wire_put_u16(output, response->negotiated_version);
    bvb_wire_put_u16(output + 2, 0);
    bvb_wire_put_u32(output + 4, response->service_flags);
    bvb_wire_put_u32(output + 8, response->pointer_bits);
    bvb_wire_put_u32(output + 12, response->page_size);
    return 0;
}

int bvb_protocol_decode_hello_response(
    const uint8_t input[BVB_HELLO_RESPONSE_SIZE],
    struct bvb_hello_response *response) {
    if (input == NULL || response == NULL) {
        return -EINVAL;
    }
    const struct bvb_hello_response decoded = {
        .negotiated_version = bvb_wire_get_u16(input),
        .service_flags = bvb_wire_get_u32(input + 4),
        .pointer_bits = bvb_wire_get_u32(input + 8),
        .page_size = bvb_wire_get_u32(input + 12),
    };
    if (bvb_wire_get_u16(input + 2) != 0U ||
        decoded.negotiated_version == 0U || decoded.pointer_bits == 0U ||
        decoded.page_size == 0U) {
        return -EPROTO;
    }
    *response = decoded;
    return 0;
}

int bvb_protocol_encode_shared_batch_setup(
    uint8_t output[BVB_SHARED_BATCH_SETUP_SIZE],
    const struct bvb_shared_batch_setup *setup) {
    if (output == NULL || setup == NULL || setup->generation == 0U ||
        setup->region_bytes < BVB_SHARED_BATCH_MIN_BYTES ||
        setup->region_bytes > BVB_SHARED_BATCH_MAX_BYTES) {
        return -EINVAL;
    }
    bvb_wire_put_u32(output, setup->region_bytes);
    bvb_wire_put_u32(output + 4, 0U);
    bvb_wire_put_u64(output + 8, setup->generation);
    return 0;
}

int bvb_protocol_decode_shared_batch_setup(
    const uint8_t input[BVB_SHARED_BATCH_SETUP_SIZE],
    struct bvb_shared_batch_setup *setup) {
    if (input == NULL || setup == NULL) {
        return -EINVAL;
    }
    const struct bvb_shared_batch_setup decoded = {
        .region_bytes = bvb_wire_get_u32(input),
        .generation = bvb_wire_get_u64(input + 8),
    };
    if (bvb_wire_get_u32(input + 4) != 0U || decoded.generation == 0U ||
        decoded.region_bytes < BVB_SHARED_BATCH_MIN_BYTES ||
        decoded.region_bytes > BVB_SHARED_BATCH_MAX_BYTES) {
        return -EPROTO;
    }
    *setup = decoded;
    return 0;
}

int bvb_protocol_encode_shared_batch_execute(
    uint8_t output[BVB_SHARED_BATCH_EXECUTE_SIZE],
    const struct bvb_shared_batch_execute *execute) {
    if (output == NULL || execute == NULL || execute->generation == 0U ||
        execute->length < BVB_COMMAND_BATCH_HEADER_SIZE ||
        execute->length > BVB_COMMAND_BATCH_MAX_BYTES ||
        execute->sequence == 0U) {
        return -EINVAL;
    }
    bvb_wire_put_u64(output, execute->generation);
    bvb_wire_put_u32(output + 8, execute->offset);
    bvb_wire_put_u32(output + 12, execute->length);
    bvb_wire_put_u64(output + 16, execute->sequence);
    return 0;
}

int bvb_protocol_decode_shared_batch_execute(
    const uint8_t input[BVB_SHARED_BATCH_EXECUTE_SIZE],
    struct bvb_shared_batch_execute *execute) {
    if (input == NULL || execute == NULL) {
        return -EINVAL;
    }
    const struct bvb_shared_batch_execute decoded = {
        .generation = bvb_wire_get_u64(input),
        .offset = bvb_wire_get_u32(input + 8),
        .length = bvb_wire_get_u32(input + 12),
        .sequence = bvb_wire_get_u64(input + 16),
    };
    if (decoded.generation == 0U ||
        decoded.length < BVB_COMMAND_BATCH_HEADER_SIZE ||
        decoded.length > BVB_COMMAND_BATCH_MAX_BYTES ||
        decoded.sequence == 0U) {
        return -EPROTO;
    }
    *execute = decoded;
    return 0;
}

static bool token_is_nonzero(
    const uint8_t token[BVB_LIFECYCLE_TOKEN_SIZE]) {
    uint8_t combined = 0U;
    for (size_t index = 0U; index < BVB_LIFECYCLE_TOKEN_SIZE; ++index) {
        combined |= token[index];
    }
    return combined != 0U;
}

int bvb_protocol_encode_visible_batch_setup(
    uint8_t output[BVB_VISIBLE_BATCH_SETUP_SIZE],
    const struct bvb_visible_batch_setup *setup) {
    if (output == NULL || setup == NULL || !token_is_nonzero(setup->token)) {
        return -EINVAL;
    }
    int result = bvb_protocol_encode_shared_batch_setup(
        output + BVB_LIFECYCLE_TOKEN_SIZE, &setup->shared);
    if (result == 0) {
        memcpy(output, setup->token, BVB_LIFECYCLE_TOKEN_SIZE);
    }
    return result;
}

int bvb_protocol_decode_visible_batch_setup(
    const uint8_t input[BVB_VISIBLE_BATCH_SETUP_SIZE],
    struct bvb_visible_batch_setup *setup) {
    if (input == NULL || setup == NULL) {
        return -EINVAL;
    }
    if (!token_is_nonzero(input)) {
        return -EPROTO;
    }
    struct bvb_visible_batch_setup decoded;
    memset(&decoded, 0, sizeof(decoded));
    memcpy(decoded.token, input, BVB_LIFECYCLE_TOKEN_SIZE);
    int result = bvb_protocol_decode_shared_batch_setup(
        input + BVB_LIFECYCLE_TOKEN_SIZE, &decoded.shared);
    if (result != 0) {
        return result;
    }
    *setup = decoded;
    return 0;
}

int bvb_protocol_encode_visible_batch_execute(
    uint8_t output[BVB_VISIBLE_BATCH_EXECUTE_SIZE],
    const struct bvb_visible_batch_execute *execute) {
    if (output == NULL || execute == NULL ||
        !token_is_nonzero(execute->token)) {
        return -EINVAL;
    }
    int result = bvb_protocol_encode_shared_batch_execute(
        output + BVB_LIFECYCLE_TOKEN_SIZE, &execute->shared);
    if (result == 0) {
        memcpy(output, execute->token, BVB_LIFECYCLE_TOKEN_SIZE);
    }
    return result;
}

int bvb_protocol_decode_visible_batch_execute(
    const uint8_t input[BVB_VISIBLE_BATCH_EXECUTE_SIZE],
    struct bvb_visible_batch_execute *execute) {
    if (input == NULL || execute == NULL) {
        return -EINVAL;
    }
    if (!token_is_nonzero(input)) {
        return -EPROTO;
    }
    struct bvb_visible_batch_execute decoded;
    memset(&decoded, 0, sizeof(decoded));
    memcpy(decoded.token, input, BVB_LIFECYCLE_TOKEN_SIZE);
    int result = bvb_protocol_decode_shared_batch_execute(
        input + BVB_LIFECYCLE_TOKEN_SIZE, &decoded.shared);
    if (result != 0) {
        return result;
    }
    *execute = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_caps(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_caps *caps,
    uint32_t *output_length) {
    if (output == NULL || caps == NULL || output_length == NULL ||
        caps->included_device_count > BVB_VULKAN_MAX_DEVICES ||
        caps->included_device_count > caps->physical_device_count) {
        return -EINVAL;
    }
    uint32_t length = BVB_VULKAN_CAPS_PREFIX_SIZE +
                      caps->included_device_count * BVB_VULKAN_CAPS_DEVICE_SIZE;
    if (length > BVB_PROTOCOL_MAX_PAYLOAD) {
        return -EMSGSIZE;
    }

    memset(output, 0, length);
    bvb_wire_put_u32(output, caps->loader_api_version);
    bvb_wire_put_u32(output + 4, caps->instance_extension_count);
    bvb_wire_put_u32(output + 8, caps->physical_device_count);
    bvb_wire_put_u32(output + 12, caps->included_device_count);

    for (uint32_t index = 0; index < caps->included_device_count; ++index) {
        const struct bvb_vulkan_device_caps *device = &caps->devices[index];
        uint8_t *record = output + BVB_VULKAN_CAPS_PREFIX_SIZE +
                          index * BVB_VULKAN_CAPS_DEVICE_SIZE;
        size_t name_length = 0;
        while (name_length < BVB_VULKAN_DEVICE_NAME_SIZE &&
               device->name[name_length] != '\0') {
            ++name_length;
        }
        if (name_length == BVB_VULKAN_DEVICE_NAME_SIZE) {
            return -EINVAL;
        }
        bvb_wire_put_u32(record, device->api_version);
        bvb_wire_put_u32(record + 4, device->driver_version);
        bvb_wire_put_u32(record + 8, device->vendor_id);
        bvb_wire_put_u32(record + 12, device->device_id);
        bvb_wire_put_u32(record + 16, device->device_type);
        bvb_wire_put_u32(record + 20, device->queue_family_count);
        bvb_wire_put_u32(record + 24, device->memory_heap_count);
        bvb_wire_put_u32(record + 28, 0);
        bvb_wire_put_u64(record + 32, device->device_local_bytes);
        memcpy(record + 40, device->name, name_length + 1U);
    }
    *output_length = length;
    return 0;
}

int bvb_protocol_decode_vulkan_caps(
    const uint8_t *input,
    uint32_t input_length,
    struct bvb_vulkan_caps *caps) {
    if (input == NULL || caps == NULL ||
        input_length < BVB_VULKAN_CAPS_PREFIX_SIZE) {
        return -EINVAL;
    }
    uint32_t included_device_count = bvb_wire_get_u32(input + 12);
    if (included_device_count > BVB_VULKAN_MAX_DEVICES) {
        return -EMSGSIZE;
    }
    uint32_t expected_length = BVB_VULKAN_CAPS_PREFIX_SIZE +
                               included_device_count *
                                   BVB_VULKAN_CAPS_DEVICE_SIZE;
    if (input_length != expected_length) {
        return -EPROTO;
    }

    struct bvb_vulkan_caps decoded;
    memset(&decoded, 0, sizeof(decoded));
    decoded.loader_api_version = bvb_wire_get_u32(input);
    decoded.instance_extension_count = bvb_wire_get_u32(input + 4);
    decoded.physical_device_count = bvb_wire_get_u32(input + 8);
    decoded.included_device_count = included_device_count;
    if (decoded.included_device_count > decoded.physical_device_count) {
        return -EPROTO;
    }

    for (uint32_t index = 0; index < included_device_count; ++index) {
        const uint8_t *record = input + BVB_VULKAN_CAPS_PREFIX_SIZE +
                                index * BVB_VULKAN_CAPS_DEVICE_SIZE;
        if (bvb_wire_get_u32(record + 28) != 0U ||
            memchr(record + 40, '\0', BVB_VULKAN_DEVICE_NAME_SIZE) == NULL) {
            return -EPROTO;
        }
        struct bvb_vulkan_device_caps *device = &decoded.devices[index];
        device->api_version = bvb_wire_get_u32(record);
        device->driver_version = bvb_wire_get_u32(record + 4);
        device->vendor_id = bvb_wire_get_u32(record + 8);
        device->device_id = bvb_wire_get_u32(record + 12);
        device->device_type = bvb_wire_get_u32(record + 16);
        device->queue_family_count = bvb_wire_get_u32(record + 20);
        device->memory_heap_count = bvb_wire_get_u32(record + 24);
        device->device_local_bytes = bvb_wire_get_u64(record + 32);
        memcpy(device->name, record + 40, BVB_VULKAN_DEVICE_NAME_SIZE);
    }
    *caps = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_selftest(
    uint8_t output[BVB_VULKAN_SELFTEST_SIZE],
    const struct bvb_vulkan_selftest_result *result) {
    if (output == NULL || result == NULL || result->buffer_bytes == 0U) {
        return -EINVAL;
    }
    memset(output, 0, BVB_VULKAN_SELFTEST_SIZE);
    bvb_wire_put_u32(output, result->instance_extension_count);
    bvb_wire_put_u32(output + 4, result->device_extension_count);
    bvb_wire_put_u64(output + 8, result->instance_extension_flags);
    bvb_wire_put_u64(output + 16, result->device_extension_flags);
    bvb_wire_put_u32(output + 24, result->queue_family_index);
    bvb_wire_put_u32(output + 28, result->queue_flags);
    bvb_wire_put_u32(output + 32, result->memory_type_index);
    bvb_wire_put_u32(output + 36, result->memory_property_flags);
    bvb_wire_put_u32(output + 40, result->buffer_bytes);
    bvb_wire_put_u32(output + 44, result->fill_word);
    bvb_wire_put_u32(output + 48, result->mismatched_words);
    bvb_wire_put_u32(output + 52, 0);
    bvb_wire_put_u64(output + 56, result->submit_wait_elapsed_ns);
    return 0;
}

int bvb_protocol_decode_vulkan_selftest(
    const uint8_t input[BVB_VULKAN_SELFTEST_SIZE],
    struct bvb_vulkan_selftest_result *result) {
    if (input == NULL || result == NULL) {
        return -EINVAL;
    }
    if (bvb_wire_get_u32(input + 52) != 0U) {
        return -EPROTO;
    }
    const struct bvb_vulkan_selftest_result decoded = {
        .instance_extension_count = bvb_wire_get_u32(input),
        .device_extension_count = bvb_wire_get_u32(input + 4),
        .instance_extension_flags = bvb_wire_get_u64(input + 8),
        .device_extension_flags = bvb_wire_get_u64(input + 16),
        .queue_family_index = bvb_wire_get_u32(input + 24),
        .queue_flags = bvb_wire_get_u32(input + 28),
        .memory_type_index = bvb_wire_get_u32(input + 32),
        .memory_property_flags = bvb_wire_get_u32(input + 36),
        .buffer_bytes = bvb_wire_get_u32(input + 40),
        .fill_word = bvb_wire_get_u32(input + 44),
        .mismatched_words = bvb_wire_get_u32(input + 48),
        .submit_wait_elapsed_ns = bvb_wire_get_u64(input + 56),
    };
    if (decoded.buffer_bytes == 0U) {
        return -EPROTO;
    }
    *result = decoded;
    return 0;
}

int bvb_protocol_encode_activity_status(
    uint8_t output[BVB_ACTIVITY_STATUS_SIZE],
    const struct bvb_activity_status *status) {
    if (output == NULL || status == NULL || status->ingress_configured > 1U) {
        return -EINVAL;
    }
    memset(output, 0, BVB_ACTIVITY_STATUS_SIZE);
    bvb_wire_put_u32(output, status->ingress_configured);
    bvb_wire_put_u32(output + 4, status->authenticated_event_count);
    bvb_wire_put_u32(output + 8, status->rejected_event_count);
    bvb_wire_put_u32(output + 12, status->last_sequence);
    bvb_wire_put_u32(output + 16, status->last_event);
    bvb_wire_put_u32(output + 20, status->state_flags);
    bvb_wire_put_u32(output + 24, status->width);
    bvb_wire_put_u32(output + 28, status->height);
    bvb_wire_put_u32(output + 32, status->activity_pid);
    bvb_wire_put_u32(output + 36, 0U);
    bvb_wire_put_u64(output + 40, status->last_event_monotonic_ns);
    bvb_wire_put_u64(output + 48, status->last_event_received_ns);
    return 0;
}

int bvb_protocol_decode_activity_status(
    const uint8_t input[BVB_ACTIVITY_STATUS_SIZE],
    struct bvb_activity_status *status) {
    if (input == NULL || status == NULL) {
        return -EINVAL;
    }
    struct bvb_activity_status decoded = {
        .ingress_configured = bvb_wire_get_u32(input),
        .authenticated_event_count = bvb_wire_get_u32(input + 4),
        .rejected_event_count = bvb_wire_get_u32(input + 8),
        .last_sequence = bvb_wire_get_u32(input + 12),
        .last_event = bvb_wire_get_u32(input + 16),
        .state_flags = bvb_wire_get_u32(input + 20),
        .width = bvb_wire_get_u32(input + 24),
        .height = bvb_wire_get_u32(input + 28),
        .activity_pid = bvb_wire_get_u32(input + 32),
        .last_event_monotonic_ns = bvb_wire_get_u64(input + 40),
        .last_event_received_ns = bvb_wire_get_u64(input + 48),
    };
    if (decoded.ingress_configured > 1U || bvb_wire_get_u32(input + 36) != 0U ||
        decoded.last_event > BVB_LIFECYCLE_EVENT_RENDERER_FAILED) {
        return -EPROTO;
    }
    *status = decoded;
    return 0;
}
