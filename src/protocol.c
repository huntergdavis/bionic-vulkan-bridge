#include <bvb/protocol.h>

#include <errno.h>
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
        header->opcode > BVB_OPCODE_VULKAN_CAPS) {
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
