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

static int wire_id_is_type(uint64_t wire_id, uint8_t type) {
    return (uint8_t)(wire_id >> 56) == type &&
           (wire_id & UINT64_C(0x00ffffffffffffff)) != 0U;
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
        header->opcode > BVB_OPCODE_LAST) {
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

int bvb_protocol_encode_vulkan_global_info(
    uint8_t output[BVB_VULKAN_GLOBAL_INFO_SIZE],
    const struct bvb_vulkan_global_info *info) {
    if (output == NULL || info == NULL || info->loader_api_version == 0U ||
        info->exposed_extension_count > info->native_extension_count ||
        info->exposed_layer_count > info->native_layer_count) {
        return -EINVAL;
    }
    memset(output, 0, BVB_VULKAN_GLOBAL_INFO_SIZE);
    bvb_wire_put_u32(output, info->loader_api_version);
    bvb_wire_put_u32(output + 4, info->native_extension_count);
    bvb_wire_put_u32(output + 8, info->native_layer_count);
    bvb_wire_put_u32(output + 12, info->exposed_extension_count);
    bvb_wire_put_u32(output + 16, info->exposed_layer_count);
    return 0;
}

int bvb_protocol_decode_vulkan_global_info(
    const uint8_t input[BVB_VULKAN_GLOBAL_INFO_SIZE],
    struct bvb_vulkan_global_info *info) {
    if (input == NULL || info == NULL) {
        return -EINVAL;
    }
    const struct bvb_vulkan_global_info decoded = {
        .loader_api_version = bvb_wire_get_u32(input),
        .native_extension_count = bvb_wire_get_u32(input + 4),
        .native_layer_count = bvb_wire_get_u32(input + 8),
        .exposed_extension_count = bvb_wire_get_u32(input + 12),
        .exposed_layer_count = bvb_wire_get_u32(input + 16),
    };
    if (decoded.loader_api_version == 0U ||
        decoded.exposed_extension_count > decoded.native_extension_count ||
        decoded.exposed_layer_count > decoded.native_layer_count ||
        bvb_wire_get_u32(input + 20) != 0U) {
        return -EPROTO;
    }
    *info = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_instance_create_request(
    uint8_t output[BVB_VULKAN_INSTANCE_CREATE_REQUEST_SIZE],
    const struct bvb_vulkan_instance_create_request *request) {
    if (output == NULL || request == NULL) {
        return -EINVAL;
    }
    bvb_wire_put_u32(output, request->api_version);
    bvb_wire_put_u32(output + 4, request->flags);
    bvb_wire_put_u32(output + 8, request->enabled_layer_count);
    bvb_wire_put_u32(output + 12, request->enabled_extension_count);
    return 0;
}

int bvb_protocol_decode_vulkan_instance_create_request(
    const uint8_t input[BVB_VULKAN_INSTANCE_CREATE_REQUEST_SIZE],
    struct bvb_vulkan_instance_create_request *request) {
    if (input == NULL || request == NULL) {
        return -EINVAL;
    }
    *request = (struct bvb_vulkan_instance_create_request){
        .api_version = bvb_wire_get_u32(input),
        .flags = bvb_wire_get_u32(input + 4),
        .enabled_layer_count = bvb_wire_get_u32(input + 8),
        .enabled_extension_count = bvb_wire_get_u32(input + 12),
    };
    return 0;
}

int bvb_protocol_encode_vulkan_instance_create_extended_request(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_instance_create_extended_request *request,
    uint32_t *output_length) {
    if (output == NULL || request == NULL || output_length == NULL ||
        request->base.enabled_extension_count == 0U ||
        request->base.enabled_extension_count >
            BVB_VULKAN_MAX_ENABLED_EXTENSIONS) {
        return -EINVAL;
    }
    const uint32_t length = BVB_VULKAN_INSTANCE_CREATE_REQUEST_SIZE +
        request->base.enabled_extension_count *
            BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE;
    memset(output, 0, length);
    int result = bvb_protocol_encode_vulkan_instance_create_request(
        output, &request->base);
    for (uint32_t index = 0U;
         result == 0 && index < request->base.enabled_extension_count;
         ++index) {
        const char *name = request->enabled_extensions[index];
        const char *terminator = memchr(
            name, '\0', BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE);
        if (name[0] == '\0' || terminator == NULL) {
            result = -EINVAL;
            break;
        }
        const size_t name_length = (size_t)(terminator - name) + 1U;
        memcpy(output + BVB_VULKAN_INSTANCE_CREATE_REQUEST_SIZE +
                   index * BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE,
               name, name_length);
    }
    if (result == 0) {
        *output_length = length;
    }
    return result;
}

int bvb_protocol_decode_vulkan_instance_create_extended_request(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_instance_create_extended_request *request) {
    if (input == NULL || request == NULL ||
        input_length < BVB_VULKAN_INSTANCE_CREATE_REQUEST_SIZE) {
        return -EINVAL;
    }
    struct bvb_vulkan_instance_create_extended_request decoded = {0};
    int result = bvb_protocol_decode_vulkan_instance_create_request(
        input, &decoded.base);
    if (result != 0 || decoded.base.enabled_extension_count == 0U ||
        decoded.base.enabled_extension_count >
            BVB_VULKAN_MAX_ENABLED_EXTENSIONS) {
        return result != 0 ? result : -EPROTO;
    }
    const uint32_t expected = BVB_VULKAN_INSTANCE_CREATE_REQUEST_SIZE +
        decoded.base.enabled_extension_count *
            BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE;
    if (input_length != expected) {
        return -EPROTO;
    }
    for (uint32_t index = 0U;
         index < decoded.base.enabled_extension_count; ++index) {
        const uint8_t *slot =
            input + BVB_VULKAN_INSTANCE_CREATE_REQUEST_SIZE +
            index * BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE;
        const uint8_t *terminator = memchr(
            slot, '\0', BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE);
        if (slot[0] == '\0' || terminator == NULL) {
            return -EPROTO;
        }
        for (const uint8_t *padding = terminator + 1;
             padding < slot + BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE;
             ++padding) {
            if (*padding != 0U) {
                return -EPROTO;
            }
        }
        memcpy(decoded.enabled_extensions[index], slot,
               BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE);
    }
    *request = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_instance_create_response(
    uint8_t output[BVB_VULKAN_INSTANCE_CREATE_RESPONSE_SIZE],
    const struct bvb_vulkan_instance_create_response *response) {
    if (output == NULL || response == NULL ||
        (response->vulkan_result == 0 && response->instance_id == 0U) ||
        (response->vulkan_result != 0 && response->instance_id != 0U)) {
        return -EINVAL;
    }
    memset(output, 0, BVB_VULKAN_INSTANCE_CREATE_RESPONSE_SIZE);
    bvb_wire_put_i32(output, response->vulkan_result);
    bvb_wire_put_u64(output + 8, response->instance_id);
    return 0;
}

int bvb_protocol_decode_vulkan_instance_create_response(
    const uint8_t input[BVB_VULKAN_INSTANCE_CREATE_RESPONSE_SIZE],
    struct bvb_vulkan_instance_create_response *response) {
    if (input == NULL || response == NULL ||
        bvb_wire_get_u32(input + 4) != 0U) {
        return input == NULL || response == NULL ? -EINVAL : -EPROTO;
    }
    const struct bvb_vulkan_instance_create_response decoded = {
        .vulkan_result = bvb_wire_get_i32(input),
        .instance_id = bvb_wire_get_u64(input + 8),
    };
    if ((decoded.vulkan_result == 0 && decoded.instance_id == 0U) ||
        (decoded.vulkan_result != 0 && decoded.instance_id != 0U)) {
        return -EPROTO;
    }
    *response = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_instance_id(
    uint8_t output[BVB_VULKAN_INSTANCE_ID_SIZE], uint64_t instance_id) {
    if (output == NULL || !wire_id_is_type(instance_id, 1U)) {
        return -EINVAL;
    }
    bvb_wire_put_u64(output, instance_id);
    return 0;
}

int bvb_protocol_decode_vulkan_instance_id(
    const uint8_t input[BVB_VULKAN_INSTANCE_ID_SIZE], uint64_t *instance_id) {
    if (input == NULL || instance_id == NULL) {
        return -EINVAL;
    }
    uint64_t decoded = bvb_wire_get_u64(input);
    if (!wire_id_is_type(decoded, 1U)) {
        return -EPROTO;
    }
    *instance_id = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_physical_devices(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_physical_devices *devices,
    uint32_t *output_length) {
    if (output == NULL || devices == NULL || output_length == NULL ||
        devices->count > BVB_VULKAN_MAX_PHYSICAL_DEVICES) {
        return -EINVAL;
    }
    for (uint32_t index = 0U; index < devices->count; ++index) {
        if (!wire_id_is_type(devices->ids[index], 2U)) {
            return -EINVAL;
        }
    }
    const uint32_t length = BVB_VULKAN_PHYSICAL_DEVICES_PREFIX_SIZE +
                            devices->count * 8U;
    memset(output, 0, length);
    bvb_wire_put_i32(output, devices->vulkan_result);
    bvb_wire_put_u32(output + 4, devices->count);
    for (uint32_t index = 0U; index < devices->count; ++index) {
        bvb_wire_put_u64(output + BVB_VULKAN_PHYSICAL_DEVICES_PREFIX_SIZE +
                             index * 8U,
                         devices->ids[index]);
    }
    *output_length = length;
    return 0;
}

int bvb_protocol_decode_vulkan_physical_devices(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_physical_devices *devices) {
    if (input == NULL || devices == NULL ||
        input_length < BVB_VULKAN_PHYSICAL_DEVICES_PREFIX_SIZE) {
        return -EINVAL;
    }
    const uint32_t count = bvb_wire_get_u32(input + 4);
    if (count > BVB_VULKAN_MAX_PHYSICAL_DEVICES ||
        input_length != BVB_VULKAN_PHYSICAL_DEVICES_PREFIX_SIZE + count * 8U) {
        return -EPROTO;
    }
    struct bvb_vulkan_physical_devices decoded = {
        .vulkan_result = bvb_wire_get_i32(input),
        .count = count,
    };
    for (uint32_t index = 0U; index < count; ++index) {
        decoded.ids[index] = bvb_wire_get_u64(
            input + BVB_VULKAN_PHYSICAL_DEVICES_PREFIX_SIZE + index * 8U);
        if (!wire_id_is_type(decoded.ids[index], 2U)) {
            return -EPROTO;
        }
    }
    *devices = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_physical_device_id(
    uint8_t output[BVB_VULKAN_PHYSICAL_DEVICE_ID_SIZE],
    uint64_t physical_device_id) {
    if (output == NULL || !wire_id_is_type(physical_device_id, 2U)) {
        return -EINVAL;
    }
    bvb_wire_put_u64(output, physical_device_id);
    return 0;
}

int bvb_protocol_decode_vulkan_physical_device_id(
    const uint8_t input[BVB_VULKAN_PHYSICAL_DEVICE_ID_SIZE],
    uint64_t *physical_device_id) {
    if (input == NULL || physical_device_id == NULL) {
        return -EINVAL;
    }
    const uint64_t decoded = bvb_wire_get_u64(input);
    if (!wire_id_is_type(decoded, 2U)) {
        return -EPROTO;
    }
    *physical_device_id = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_format_query(
    uint8_t output[BVB_VULKAN_FORMAT_QUERY_SIZE],
    const struct bvb_vulkan_format_query *query) {
    if (output == NULL || query == NULL ||
        !wire_id_is_type(query->physical_device_id, 2U)) {
        return -EINVAL;
    }
    memset(output, 0, BVB_VULKAN_FORMAT_QUERY_SIZE);
    bvb_wire_put_u64(output, query->physical_device_id);
    bvb_wire_put_u32(output + 8, query->format);
    return 0;
}

int bvb_protocol_decode_vulkan_format_query(
    const uint8_t input[BVB_VULKAN_FORMAT_QUERY_SIZE],
    struct bvb_vulkan_format_query *query) {
    if (input == NULL || query == NULL) {
        return -EINVAL;
    }
    const struct bvb_vulkan_format_query decoded = {
        .physical_device_id = bvb_wire_get_u64(input),
        .format = bvb_wire_get_u32(input + 8),
    };
    if (!wire_id_is_type(decoded.physical_device_id, 2U) ||
        bvb_wire_get_u32(input + 12) != 0U) {
        return -EPROTO;
    }
    *query = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_format_properties(
    uint8_t output[BVB_VULKAN_FORMAT_PROPERTIES_SIZE],
    const struct bvb_vulkan_format_properties *properties) {
    if (output == NULL || properties == NULL) {
        return -EINVAL;
    }
    bvb_wire_put_u32(output, properties->linear_tiling_features);
    bvb_wire_put_u32(output + 4, properties->optimal_tiling_features);
    bvb_wire_put_u32(output + 8, properties->buffer_features);
    return 0;
}

int bvb_protocol_decode_vulkan_format_properties(
    const uint8_t input[BVB_VULKAN_FORMAT_PROPERTIES_SIZE],
    struct bvb_vulkan_format_properties *properties) {
    if (input == NULL || properties == NULL) {
        return -EINVAL;
    }
    *properties = (struct bvb_vulkan_format_properties){
        .linear_tiling_features = bvb_wire_get_u32(input),
        .optimal_tiling_features = bvb_wire_get_u32(input + 4),
        .buffer_features = bvb_wire_get_u32(input + 8),
    };
    return 0;
}

int bvb_protocol_encode_vulkan_image_format_query(
    uint8_t output[BVB_VULKAN_IMAGE_FORMAT_QUERY_SIZE],
    const struct bvb_vulkan_image_format_query *query) {
    if (output == NULL || query == NULL ||
        !wire_id_is_type(query->physical_device_id, 2U)) {
        return -EINVAL;
    }
    memset(output, 0, BVB_VULKAN_IMAGE_FORMAT_QUERY_SIZE);
    bvb_wire_put_u64(output, query->physical_device_id);
    bvb_wire_put_u32(output + 8, query->format);
    bvb_wire_put_u32(output + 12, query->type);
    bvb_wire_put_u32(output + 16, query->tiling);
    bvb_wire_put_u32(output + 20, query->usage);
    bvb_wire_put_u32(output + 24, query->flags);
    return 0;
}

int bvb_protocol_decode_vulkan_image_format_query(
    const uint8_t input[BVB_VULKAN_IMAGE_FORMAT_QUERY_SIZE],
    struct bvb_vulkan_image_format_query *query) {
    if (input == NULL || query == NULL) {
        return -EINVAL;
    }
    const struct bvb_vulkan_image_format_query decoded = {
        .physical_device_id = bvb_wire_get_u64(input),
        .format = bvb_wire_get_u32(input + 8),
        .type = bvb_wire_get_u32(input + 12),
        .tiling = bvb_wire_get_u32(input + 16),
        .usage = bvb_wire_get_u32(input + 20),
        .flags = bvb_wire_get_u32(input + 24),
    };
    if (!wire_id_is_type(decoded.physical_device_id, 2U) ||
        bvb_wire_get_u32(input + 28) != 0U) {
        return -EPROTO;
    }
    *query = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_image_format_properties(
    uint8_t output[BVB_VULKAN_IMAGE_FORMAT_PROPERTIES_SIZE],
    const struct bvb_vulkan_image_format_properties *properties) {
    if (output == NULL || properties == NULL) {
        return -EINVAL;
    }
    memset(output, 0, BVB_VULKAN_IMAGE_FORMAT_PROPERTIES_SIZE);
    bvb_wire_put_i32(output, properties->vulkan_result);
    bvb_wire_put_u32(output + 8, properties->max_extent_width);
    bvb_wire_put_u32(output + 12, properties->max_extent_height);
    bvb_wire_put_u32(output + 16, properties->max_extent_depth);
    bvb_wire_put_u32(output + 20, properties->max_mip_levels);
    bvb_wire_put_u32(output + 24, properties->max_array_layers);
    bvb_wire_put_u32(output + 28, properties->sample_counts);
    bvb_wire_put_u64(output + 32, properties->max_resource_size);
    return 0;
}

int bvb_protocol_decode_vulkan_image_format_properties(
    const uint8_t input[BVB_VULKAN_IMAGE_FORMAT_PROPERTIES_SIZE],
    struct bvb_vulkan_image_format_properties *properties) {
    if (input == NULL || properties == NULL) {
        return -EINVAL;
    }
    if (bvb_wire_get_u32(input + 4) != 0U) {
        return -EPROTO;
    }
    *properties = (struct bvb_vulkan_image_format_properties){
        .vulkan_result = bvb_wire_get_i32(input),
        .max_extent_width = bvb_wire_get_u32(input + 8),
        .max_extent_height = bvb_wire_get_u32(input + 12),
        .max_extent_depth = bvb_wire_get_u32(input + 16),
        .max_mip_levels = bvb_wire_get_u32(input + 20),
        .max_array_layers = bvb_wire_get_u32(input + 24),
        .sample_counts = bvb_wire_get_u32(input + 28),
        .max_resource_size = bvb_wire_get_u64(input + 32),
    };
    return 0;
}

int bvb_protocol_encode_vulkan_external_buffer_query(
    uint8_t output[BVB_VULKAN_EXTERNAL_BUFFER_QUERY_SIZE],
    const struct bvb_vulkan_external_buffer_query *query) {
    if (output == NULL || query == NULL ||
        !wire_id_is_type(query->physical_device_id, 2U)) {
        return -EINVAL;
    }
    memset(output, 0, BVB_VULKAN_EXTERNAL_BUFFER_QUERY_SIZE);
    bvb_wire_put_u64(output, query->physical_device_id);
    bvb_wire_put_u32(output + 8, query->flags);
    bvb_wire_put_u32(output + 12, query->usage);
    bvb_wire_put_u32(output + 16, query->handle_type);
    return 0;
}

int bvb_protocol_decode_vulkan_external_buffer_query(
    const uint8_t input[BVB_VULKAN_EXTERNAL_BUFFER_QUERY_SIZE],
    struct bvb_vulkan_external_buffer_query *query) {
    if (input == NULL || query == NULL) {
        return -EINVAL;
    }
    const struct bvb_vulkan_external_buffer_query decoded = {
        .physical_device_id = bvb_wire_get_u64(input),
        .flags = bvb_wire_get_u32(input + 8),
        .usage = bvb_wire_get_u32(input + 12),
        .handle_type = bvb_wire_get_u32(input + 16),
    };
    if (!wire_id_is_type(decoded.physical_device_id, 2U) ||
        bvb_wire_get_u32(input + 20) != 0U) {
        return -EPROTO;
    }
    *query = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_external_buffer_properties(
    uint8_t output[BVB_VULKAN_EXTERNAL_BUFFER_PROPERTIES_SIZE],
    const struct bvb_vulkan_external_buffer_properties *properties) {
    if (output == NULL || properties == NULL) {
        return -EINVAL;
    }
    bvb_wire_put_u32(output, properties->external_memory_features);
    bvb_wire_put_u32(output + 4,
                     properties->export_from_imported_handle_types);
    bvb_wire_put_u32(output + 8, properties->compatible_handle_types);
    return 0;
}

int bvb_protocol_decode_vulkan_external_buffer_properties(
    const uint8_t input[BVB_VULKAN_EXTERNAL_BUFFER_PROPERTIES_SIZE],
    struct bvb_vulkan_external_buffer_properties *properties) {
    if (input == NULL || properties == NULL) {
        return -EINVAL;
    }
    *properties = (struct bvb_vulkan_external_buffer_properties){
        .external_memory_features = bvb_wire_get_u32(input),
        .export_from_imported_handle_types = bvb_wire_get_u32(input + 4),
        .compatible_handle_types = bvb_wire_get_u32(input + 8),
    };
    return 0;
}

int bvb_protocol_encode_vulkan_external_semaphore_query(
    uint8_t output[BVB_VULKAN_EXTERNAL_SEMAPHORE_QUERY_SIZE],
    const struct bvb_vulkan_external_semaphore_query *query) {
    if (output == NULL || query == NULL ||
        !wire_id_is_type(query->physical_device_id, 2U)) {
        return -EINVAL;
    }
    memset(output, 0, BVB_VULKAN_EXTERNAL_SEMAPHORE_QUERY_SIZE);
    bvb_wire_put_u64(output, query->physical_device_id);
    bvb_wire_put_u32(output + 8, query->handle_type);
    return 0;
}

int bvb_protocol_decode_vulkan_external_semaphore_query(
    const uint8_t input[BVB_VULKAN_EXTERNAL_SEMAPHORE_QUERY_SIZE],
    struct bvb_vulkan_external_semaphore_query *query) {
    if (input == NULL || query == NULL) {
        return -EINVAL;
    }
    const struct bvb_vulkan_external_semaphore_query decoded = {
        .physical_device_id = bvb_wire_get_u64(input),
        .handle_type = bvb_wire_get_u32(input + 8),
    };
    if (!wire_id_is_type(decoded.physical_device_id, 2U) ||
        bvb_wire_get_u32(input + 12) != 0U) {
        return -EPROTO;
    }
    *query = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_external_semaphore_properties(
    uint8_t output[BVB_VULKAN_EXTERNAL_SEMAPHORE_PROPERTIES_SIZE],
    const struct bvb_vulkan_external_semaphore_properties *properties) {
    if (output == NULL || properties == NULL) {
        return -EINVAL;
    }
    bvb_wire_put_u32(output,
                     properties->export_from_imported_handle_types);
    bvb_wire_put_u32(output + 4, properties->compatible_handle_types);
    bvb_wire_put_u32(output + 8, properties->external_semaphore_features);
    return 0;
}

int bvb_protocol_decode_vulkan_external_semaphore_properties(
    const uint8_t input[BVB_VULKAN_EXTERNAL_SEMAPHORE_PROPERTIES_SIZE],
    struct bvb_vulkan_external_semaphore_properties *properties) {
    if (input == NULL || properties == NULL) {
        return -EINVAL;
    }
    *properties = (struct bvb_vulkan_external_semaphore_properties){
        .export_from_imported_handle_types = bvb_wire_get_u32(input),
        .compatible_handle_types = bvb_wire_get_u32(input + 4),
        .external_semaphore_features = bvb_wire_get_u32(input + 8),
    };
    return 0;
}

int bvb_protocol_encode_vulkan_device_extension_query(
    uint8_t output[BVB_VULKAN_DEVICE_EXTENSION_QUERY_SIZE],
    const struct bvb_vulkan_device_extension_query *query) {
    if (output == NULL || query == NULL ||
        !wire_id_is_type(query->physical_device_id, 2U)) {
        return -EINVAL;
    }
    memset(output, 0, BVB_VULKAN_DEVICE_EXTENSION_QUERY_SIZE);
    bvb_wire_put_u64(output, query->physical_device_id);
    bvb_wire_put_u32(output + 8, query->first);
    bvb_wire_put_u32(output + 12, query->max_count);
    return 0;
}

int bvb_protocol_decode_vulkan_device_extension_query(
    const uint8_t input[BVB_VULKAN_DEVICE_EXTENSION_QUERY_SIZE],
    struct bvb_vulkan_device_extension_query *query) {
    if (input == NULL || query == NULL) {
        return -EINVAL;
    }
    const struct bvb_vulkan_device_extension_query decoded = {
        .physical_device_id = bvb_wire_get_u64(input),
        .first = bvb_wire_get_u32(input + 8),
        .max_count = bvb_wire_get_u32(input + 12),
    };
    if (!wire_id_is_type(decoded.physical_device_id, 2U)) {
        return -EPROTO;
    }
    *query = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_device_create_request(
    uint8_t output[BVB_VULKAN_DEVICE_CREATE_REQUEST_SIZE],
    const struct bvb_vulkan_device_create_request *request) {
    if (output == NULL || request == NULL ||
        !wire_id_is_type(request->physical_device_id, 2U)) {
        return -EINVAL;
    }
    memset(output, 0, BVB_VULKAN_DEVICE_CREATE_REQUEST_SIZE);
    bvb_wire_put_u64(output, request->physical_device_id);
    bvb_wire_put_u32(output + 8, request->flags);
    bvb_wire_put_u32(output + 12, request->queue_family_index);
    bvb_wire_put_u32(output + 16, request->queue_count);
    bvb_wire_put_u32(output + 20, request->queue_priority_bits);
    bvb_wire_put_u32(output + 24, request->enabled_layer_count);
    bvb_wire_put_u32(output + 28, request->enabled_extension_count);
    return 0;
}

int bvb_protocol_decode_vulkan_device_create_request(
    const uint8_t input[BVB_VULKAN_DEVICE_CREATE_REQUEST_SIZE],
    struct bvb_vulkan_device_create_request *request) {
    if (input == NULL || request == NULL) {
        return -EINVAL;
    }
    const struct bvb_vulkan_device_create_request decoded = {
        .physical_device_id = bvb_wire_get_u64(input),
        .flags = bvb_wire_get_u32(input + 8),
        .queue_family_index = bvb_wire_get_u32(input + 12),
        .queue_count = bvb_wire_get_u32(input + 16),
        .queue_priority_bits = bvb_wire_get_u32(input + 20),
        .enabled_layer_count = bvb_wire_get_u32(input + 24),
        .enabled_extension_count = bvb_wire_get_u32(input + 28),
    };
    if (!wire_id_is_type(decoded.physical_device_id, 2U)) {
        return -EPROTO;
    }
    *request = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_device_create_extended_request(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_device_create_extended_request *request,
    uint32_t *output_length) {
    if (output == NULL || request == NULL || output_length == NULL ||
        request->base.enabled_extension_count == 0U ||
        request->base.enabled_extension_count >
            BVB_VULKAN_MAX_ENABLED_EXTENSIONS) {
        return -EINVAL;
    }
    const uint32_t length = BVB_VULKAN_DEVICE_CREATE_REQUEST_SIZE +
        request->base.enabled_extension_count *
            BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE;
    memset(output, 0, length);
    int result = bvb_protocol_encode_vulkan_device_create_request(
        output, &request->base);
    for (uint32_t index = 0U;
         result == 0 && index < request->base.enabled_extension_count;
         ++index) {
        const char *name = request->enabled_extensions[index];
        const char *terminator = memchr(
            name, '\0', BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE);
        if (name[0] == '\0' || terminator == NULL) {
            result = -EINVAL;
            break;
        }
        const size_t name_length = (size_t)(terminator - name) + 1U;
        memcpy(output + BVB_VULKAN_DEVICE_CREATE_REQUEST_SIZE +
                   index * BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE,
               name, name_length);
    }
    if (result == 0) {
        *output_length = length;
    }
    return result;
}

int bvb_protocol_decode_vulkan_device_create_extended_request(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_device_create_extended_request *request) {
    if (input == NULL || request == NULL ||
        input_length < BVB_VULKAN_DEVICE_CREATE_REQUEST_SIZE) {
        return -EINVAL;
    }
    struct bvb_vulkan_device_create_extended_request decoded = {0};
    int result = bvb_protocol_decode_vulkan_device_create_request(
        input, &decoded.base);
    if (result != 0 || decoded.base.enabled_extension_count == 0U ||
        decoded.base.enabled_extension_count >
            BVB_VULKAN_MAX_ENABLED_EXTENSIONS) {
        return result != 0 ? result : -EPROTO;
    }
    const uint32_t expected = BVB_VULKAN_DEVICE_CREATE_REQUEST_SIZE +
        decoded.base.enabled_extension_count *
            BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE;
    if (input_length != expected) {
        return -EPROTO;
    }
    for (uint32_t index = 0U;
         index < decoded.base.enabled_extension_count; ++index) {
        const uint8_t *slot =
            input + BVB_VULKAN_DEVICE_CREATE_REQUEST_SIZE +
            index * BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE;
        const uint8_t *terminator = memchr(
            slot, '\0', BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE);
        if (slot[0] == '\0' || terminator == NULL) {
            return -EPROTO;
        }
        for (const uint8_t *padding = terminator + 1;
             padding < slot + BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE;
             ++padding) {
            if (*padding != 0U) {
                return -EPROTO;
            }
        }
        memcpy(decoded.enabled_extensions[index], slot,
               BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE);
    }
    *request = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_device_create_response(
    uint8_t output[BVB_VULKAN_DEVICE_CREATE_RESPONSE_SIZE],
    const struct bvb_vulkan_device_create_response *response) {
    if (output == NULL || response == NULL ||
        (response->vulkan_result == 0 &&
         !wire_id_is_type(response->device_id, 3U)) ||
        (response->vulkan_result != 0 && response->device_id != 0U)) {
        return -EINVAL;
    }
    memset(output, 0, BVB_VULKAN_DEVICE_CREATE_RESPONSE_SIZE);
    bvb_wire_put_i32(output, response->vulkan_result);
    bvb_wire_put_u64(output + 8, response->device_id);
    return 0;
}

int bvb_protocol_decode_vulkan_device_create_response(
    const uint8_t input[BVB_VULKAN_DEVICE_CREATE_RESPONSE_SIZE],
    struct bvb_vulkan_device_create_response *response) {
    if (input == NULL || response == NULL || bvb_wire_get_u32(input + 4) != 0U) {
        return -EINVAL;
    }
    const struct bvb_vulkan_device_create_response decoded = {
        .vulkan_result = bvb_wire_get_i32(input),
        .device_id = bvb_wire_get_u64(input + 8),
    };
    if ((decoded.vulkan_result == 0 &&
         !wire_id_is_type(decoded.device_id, 3U)) ||
        (decoded.vulkan_result != 0 && decoded.device_id != 0U)) {
        return -EPROTO;
    }
    *response = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_device_id(
    uint8_t output[BVB_VULKAN_DEVICE_ID_SIZE], uint64_t device_id) {
    if (output == NULL || !wire_id_is_type(device_id, 3U)) {
        return -EINVAL;
    }
    bvb_wire_put_u64(output, device_id);
    return 0;
}

int bvb_protocol_decode_vulkan_device_id(
    const uint8_t input[BVB_VULKAN_DEVICE_ID_SIZE], uint64_t *device_id) {
    if (input == NULL || device_id == NULL) {
        return -EINVAL;
    }
    const uint64_t decoded = bvb_wire_get_u64(input);
    if (!wire_id_is_type(decoded, 3U)) {
        return -EPROTO;
    }
    *device_id = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_device_queue_request(
    uint8_t output[BVB_VULKAN_DEVICE_QUEUE_REQUEST_SIZE],
    const struct bvb_vulkan_device_queue_request *request) {
    if (output == NULL || request == NULL ||
        !wire_id_is_type(request->device_id, 3U)) {
        return -EINVAL;
    }
    memset(output, 0, BVB_VULKAN_DEVICE_QUEUE_REQUEST_SIZE);
    bvb_wire_put_u64(output, request->device_id);
    bvb_wire_put_u32(output + 8, request->queue_family_index);
    bvb_wire_put_u32(output + 12, request->queue_index);
    return 0;
}

int bvb_protocol_decode_vulkan_device_queue_request(
    const uint8_t input[BVB_VULKAN_DEVICE_QUEUE_REQUEST_SIZE],
    struct bvb_vulkan_device_queue_request *request) {
    if (input == NULL || request == NULL) {
        return -EINVAL;
    }
    const struct bvb_vulkan_device_queue_request decoded = {
        .device_id = bvb_wire_get_u64(input),
        .queue_family_index = bvb_wire_get_u32(input + 8),
        .queue_index = bvb_wire_get_u32(input + 12),
    };
    if (!wire_id_is_type(decoded.device_id, 3U)) {
        return -EPROTO;
    }
    *request = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_queue_id(
    uint8_t output[BVB_VULKAN_QUEUE_ID_SIZE], uint64_t queue_id) {
    if (output == NULL || !wire_id_is_type(queue_id, 4U)) {
        return -EINVAL;
    }
    bvb_wire_put_u64(output, queue_id);
    return 0;
}

int bvb_protocol_decode_vulkan_queue_id(
    const uint8_t input[BVB_VULKAN_QUEUE_ID_SIZE], uint64_t *queue_id) {
    if (input == NULL || queue_id == NULL) {
        return -EINVAL;
    }
    const uint64_t decoded = bvb_wire_get_u64(input);
    if (!wire_id_is_type(decoded, 4U)) {
        return -EPROTO;
    }
    *queue_id = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_result(
    uint8_t output[BVB_VULKAN_RESULT_SIZE], int32_t vulkan_result) {
    if (output == NULL) {
        return -EINVAL;
    }
    bvb_wire_put_i32(output, vulkan_result);
    return 0;
}

int bvb_protocol_decode_vulkan_result(
    const uint8_t input[BVB_VULKAN_RESULT_SIZE], int32_t *vulkan_result) {
    if (input == NULL || vulkan_result == NULL) {
        return -EINVAL;
    }
    *vulkan_result = bvb_wire_get_i32(input);
    return 0;
}

int bvb_protocol_encode_vulkan_command_pool_create_request(
    uint8_t output[BVB_VULKAN_COMMAND_POOL_CREATE_REQUEST_SIZE],
    const struct bvb_vulkan_command_pool_create_request *request) {
    if (output == NULL || request == NULL ||
        !wire_id_is_type(request->device_id, 3U)) {
        return -EINVAL;
    }
    bvb_wire_put_u64(output, request->device_id);
    bvb_wire_put_u32(output + 8, request->flags);
    bvb_wire_put_u32(output + 12, request->queue_family_index);
    return 0;
}

int bvb_protocol_decode_vulkan_command_pool_create_request(
    const uint8_t input[BVB_VULKAN_COMMAND_POOL_CREATE_REQUEST_SIZE],
    struct bvb_vulkan_command_pool_create_request *request) {
    if (input == NULL || request == NULL) {
        return -EINVAL;
    }
    const struct bvb_vulkan_command_pool_create_request decoded = {
        .device_id = bvb_wire_get_u64(input),
        .flags = bvb_wire_get_u32(input + 8),
        .queue_family_index = bvb_wire_get_u32(input + 12),
    };
    if (!wire_id_is_type(decoded.device_id, 3U)) {
        return -EPROTO;
    }
    *request = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_command_pool_create_response(
    uint8_t output[BVB_VULKAN_COMMAND_POOL_CREATE_RESPONSE_SIZE],
    const struct bvb_vulkan_command_pool_create_response *response) {
    if (output == NULL || response == NULL ||
        (response->vulkan_result == 0 &&
         !wire_id_is_type(response->command_pool_id, 10U)) ||
        (response->vulkan_result != 0 && response->command_pool_id != 0U)) {
        return -EINVAL;
    }
    memset(output, 0, BVB_VULKAN_COMMAND_POOL_CREATE_RESPONSE_SIZE);
    bvb_wire_put_i32(output, response->vulkan_result);
    bvb_wire_put_u64(output + 8, response->command_pool_id);
    return 0;
}

int bvb_protocol_decode_vulkan_command_pool_create_response(
    const uint8_t input[BVB_VULKAN_COMMAND_POOL_CREATE_RESPONSE_SIZE],
    struct bvb_vulkan_command_pool_create_response *response) {
    if (input == NULL || response == NULL) {
        return -EINVAL;
    }
    const struct bvb_vulkan_command_pool_create_response decoded = {
        .vulkan_result = bvb_wire_get_i32(input),
        .command_pool_id = bvb_wire_get_u64(input + 8),
    };
    if (bvb_wire_get_u32(input + 4) != 0U ||
        (decoded.vulkan_result == 0 &&
         !wire_id_is_type(decoded.command_pool_id, 10U)) ||
        (decoded.vulkan_result != 0 && decoded.command_pool_id != 0U)) {
        return -EPROTO;
    }
    *response = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_command_pool_id(
    uint8_t output[BVB_VULKAN_COMMAND_POOL_ID_SIZE], uint64_t command_pool_id) {
    if (output == NULL || !wire_id_is_type(command_pool_id, 10U)) {
        return -EINVAL;
    }
    bvb_wire_put_u64(output, command_pool_id);
    return 0;
}

int bvb_protocol_decode_vulkan_command_pool_id(
    const uint8_t input[BVB_VULKAN_COMMAND_POOL_ID_SIZE],
    uint64_t *command_pool_id) {
    if (input == NULL || command_pool_id == NULL) {
        return -EINVAL;
    }
    const uint64_t decoded = bvb_wire_get_u64(input);
    if (!wire_id_is_type(decoded, 10U)) {
        return -EPROTO;
    }
    *command_pool_id = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_command_pool_reset_request(
    uint8_t output[BVB_VULKAN_COMMAND_POOL_RESET_REQUEST_SIZE],
    const struct bvb_vulkan_command_pool_reset_request *request) {
    if (output == NULL || request == NULL ||
        !wire_id_is_type(request->command_pool_id, 10U)) {
        return -EINVAL;
    }
    memset(output, 0, BVB_VULKAN_COMMAND_POOL_RESET_REQUEST_SIZE);
    bvb_wire_put_u64(output, request->command_pool_id);
    bvb_wire_put_u32(output + 8, request->flags);
    return 0;
}

int bvb_protocol_decode_vulkan_command_pool_reset_request(
    const uint8_t input[BVB_VULKAN_COMMAND_POOL_RESET_REQUEST_SIZE],
    struct bvb_vulkan_command_pool_reset_request *request) {
    if (input == NULL || request == NULL) {
        return -EINVAL;
    }
    const struct bvb_vulkan_command_pool_reset_request decoded = {
        .command_pool_id = bvb_wire_get_u64(input),
        .flags = bvb_wire_get_u32(input + 8),
    };
    if (!wire_id_is_type(decoded.command_pool_id, 10U) ||
        bvb_wire_get_u32(input + 12) != 0U) {
        return -EPROTO;
    }
    *request = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_command_buffer_allocate_request(
    uint8_t output[BVB_VULKAN_COMMAND_BUFFER_ALLOCATE_REQUEST_SIZE],
    const struct bvb_vulkan_command_buffer_allocate_request *request) {
    if (output == NULL || request == NULL ||
        !wire_id_is_type(request->command_pool_id, 10U)) {
        return -EINVAL;
    }
    bvb_wire_put_u64(output, request->command_pool_id);
    bvb_wire_put_u32(output + 8, request->level);
    bvb_wire_put_u32(output + 12, request->count);
    return 0;
}

int bvb_protocol_decode_vulkan_command_buffer_allocate_request(
    const uint8_t input[BVB_VULKAN_COMMAND_BUFFER_ALLOCATE_REQUEST_SIZE],
    struct bvb_vulkan_command_buffer_allocate_request *request) {
    if (input == NULL || request == NULL) {
        return -EINVAL;
    }
    const struct bvb_vulkan_command_buffer_allocate_request decoded = {
        .command_pool_id = bvb_wire_get_u64(input),
        .level = bvb_wire_get_u32(input + 8),
        .count = bvb_wire_get_u32(input + 12),
    };
    if (!wire_id_is_type(decoded.command_pool_id, 10U)) {
        return -EPROTO;
    }
    *request = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_command_buffer_allocate_response(
    uint8_t output[BVB_VULKAN_COMMAND_BUFFER_ALLOCATE_RESPONSE_SIZE],
    const struct bvb_vulkan_command_buffer_allocate_response *response) {
    if (output == NULL || response == NULL ||
        (response->vulkan_result == 0 &&
         !wire_id_is_type(response->command_buffer_id, 11U)) ||
        (response->vulkan_result != 0 && response->command_buffer_id != 0U)) {
        return -EINVAL;
    }
    memset(output, 0, BVB_VULKAN_COMMAND_BUFFER_ALLOCATE_RESPONSE_SIZE);
    bvb_wire_put_i32(output, response->vulkan_result);
    bvb_wire_put_u64(output + 8, response->command_buffer_id);
    return 0;
}

int bvb_protocol_decode_vulkan_command_buffer_allocate_response(
    const uint8_t input[BVB_VULKAN_COMMAND_BUFFER_ALLOCATE_RESPONSE_SIZE],
    struct bvb_vulkan_command_buffer_allocate_response *response) {
    if (input == NULL || response == NULL) {
        return -EINVAL;
    }
    const struct bvb_vulkan_command_buffer_allocate_response decoded = {
        .vulkan_result = bvb_wire_get_i32(input),
        .command_buffer_id = bvb_wire_get_u64(input + 8),
    };
    if (bvb_wire_get_u32(input + 4) != 0U ||
        (decoded.vulkan_result == 0 &&
         !wire_id_is_type(decoded.command_buffer_id, 11U)) ||
        (decoded.vulkan_result != 0 && decoded.command_buffer_id != 0U)) {
        return -EPROTO;
    }
    *response = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_command_buffer_free_request(
    uint8_t output[BVB_VULKAN_COMMAND_BUFFER_FREE_REQUEST_SIZE],
    const struct bvb_vulkan_command_buffer_free_request *request) {
    if (output == NULL || request == NULL ||
        !wire_id_is_type(request->command_pool_id, 10U) ||
        !wire_id_is_type(request->command_buffer_id, 11U)) {
        return -EINVAL;
    }
    bvb_wire_put_u64(output, request->command_pool_id);
    bvb_wire_put_u64(output + 8, request->command_buffer_id);
    return 0;
}

int bvb_protocol_decode_vulkan_command_buffer_free_request(
    const uint8_t input[BVB_VULKAN_COMMAND_BUFFER_FREE_REQUEST_SIZE],
    struct bvb_vulkan_command_buffer_free_request *request) {
    if (input == NULL || request == NULL) {
        return -EINVAL;
    }
    const struct bvb_vulkan_command_buffer_free_request decoded = {
        .command_pool_id = bvb_wire_get_u64(input),
        .command_buffer_id = bvb_wire_get_u64(input + 8),
    };
    if (!wire_id_is_type(decoded.command_pool_id, 10U) ||
        !wire_id_is_type(decoded.command_buffer_id, 11U)) {
        return -EPROTO;
    }
    *request = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_command_buffer_begin_request(
    uint8_t output[BVB_VULKAN_COMMAND_BUFFER_BEGIN_REQUEST_SIZE],
    const struct bvb_vulkan_command_buffer_begin_request *request) {
    if (output == NULL || request == NULL ||
        !wire_id_is_type(request->command_buffer_id, 11U)) {
        return -EINVAL;
    }
    memset(output, 0, BVB_VULKAN_COMMAND_BUFFER_BEGIN_REQUEST_SIZE);
    bvb_wire_put_u64(output, request->command_buffer_id);
    bvb_wire_put_u32(output + 8, request->flags);
    return 0;
}

int bvb_protocol_decode_vulkan_command_buffer_begin_request(
    const uint8_t input[BVB_VULKAN_COMMAND_BUFFER_BEGIN_REQUEST_SIZE],
    struct bvb_vulkan_command_buffer_begin_request *request) {
    if (input == NULL || request == NULL) {
        return -EINVAL;
    }
    const struct bvb_vulkan_command_buffer_begin_request decoded = {
        .command_buffer_id = bvb_wire_get_u64(input),
        .flags = bvb_wire_get_u32(input + 8),
    };
    if (!wire_id_is_type(decoded.command_buffer_id, 11U) ||
        bvb_wire_get_u32(input + 12) != 0U) {
        return -EPROTO;
    }
    *request = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_command_buffer_id(
    uint8_t output[BVB_VULKAN_COMMAND_BUFFER_ID_SIZE],
    uint64_t command_buffer_id) {
    if (output == NULL || !wire_id_is_type(command_buffer_id, 11U)) {
        return -EINVAL;
    }
    bvb_wire_put_u64(output, command_buffer_id);
    return 0;
}

int bvb_protocol_decode_vulkan_command_buffer_id(
    const uint8_t input[BVB_VULKAN_COMMAND_BUFFER_ID_SIZE],
    uint64_t *command_buffer_id) {
    if (input == NULL || command_buffer_id == NULL) {
        return -EINVAL;
    }
    const uint64_t decoded = bvb_wire_get_u64(input);
    if (!wire_id_is_type(decoded, 11U)) {
        return -EPROTO;
    }
    *command_buffer_id = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_queue_submit_command_request(
    uint8_t output[BVB_VULKAN_QUEUE_SUBMIT_COMMAND_REQUEST_SIZE],
    const struct bvb_vulkan_queue_submit_command_request *request) {
    if (output == NULL || request == NULL ||
        !wire_id_is_type(request->queue_id, 4U) ||
        !wire_id_is_type(request->command_buffer_id, 11U)) {
        return -EINVAL;
    }
    bvb_wire_put_u64(output, request->queue_id);
    bvb_wire_put_u64(output + 8, request->command_buffer_id);
    return 0;
}

int bvb_protocol_decode_vulkan_queue_submit_command_request(
    const uint8_t input[BVB_VULKAN_QUEUE_SUBMIT_COMMAND_REQUEST_SIZE],
    struct bvb_vulkan_queue_submit_command_request *request) {
    if (input == NULL || request == NULL) {
        return -EINVAL;
    }
    const struct bvb_vulkan_queue_submit_command_request decoded = {
        .queue_id = bvb_wire_get_u64(input),
        .command_buffer_id = bvb_wire_get_u64(input + 8),
    };
    if (!wire_id_is_type(decoded.queue_id, 4U) ||
        !wire_id_is_type(decoded.command_buffer_id, 11U)) {
        return -EPROTO;
    }
    *request = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_buffer_create_request(
    uint8_t output[BVB_VULKAN_BUFFER_CREATE_REQUEST_SIZE],
    const struct bvb_vulkan_buffer_create_request *request) {
    if (output == NULL || request == NULL || request->size == 0U ||
        !wire_id_is_type(request->device_id, 3U)) return -EINVAL;
    bvb_wire_put_u64(output, request->device_id);
    bvb_wire_put_u64(output + 8, request->size);
    bvb_wire_put_u32(output + 16, request->usage);
    bvb_wire_put_u32(output + 20, request->flags);
    return 0;
}

int bvb_protocol_decode_vulkan_buffer_create_request(
    const uint8_t input[BVB_VULKAN_BUFFER_CREATE_REQUEST_SIZE],
    struct bvb_vulkan_buffer_create_request *request) {
    if (input == NULL || request == NULL) return -EINVAL;
    *request = (struct bvb_vulkan_buffer_create_request){
        .device_id = bvb_wire_get_u64(input),
        .size = bvb_wire_get_u64(input + 8),
        .usage = bvb_wire_get_u32(input + 16),
        .flags = bvb_wire_get_u32(input + 20),
    };
    return wire_id_is_type(request->device_id, 3U) && request->size != 0U
               ? 0 : -EPROTO;
}

int bvb_protocol_encode_vulkan_object_create_response(
    uint8_t output[BVB_VULKAN_OBJECT_CREATE_RESPONSE_SIZE],
    const struct bvb_vulkan_object_create_response *response,
    uint8_t expected_type) {
    if (output == NULL || response == NULL ||
        (response->vulkan_result == 0 &&
         !wire_id_is_type(response->object_id, expected_type)) ||
        (response->vulkan_result != 0 && response->object_id != 0U))
        return -EINVAL;
    memset(output, 0, BVB_VULKAN_OBJECT_CREATE_RESPONSE_SIZE);
    bvb_wire_put_i32(output, response->vulkan_result);
    bvb_wire_put_u64(output + 8, response->object_id);
    return 0;
}

int bvb_protocol_decode_vulkan_object_create_response(
    const uint8_t input[BVB_VULKAN_OBJECT_CREATE_RESPONSE_SIZE],
    struct bvb_vulkan_object_create_response *response,
    uint8_t expected_type) {
    if (input == NULL || response == NULL) return -EINVAL;
    *response = (struct bvb_vulkan_object_create_response){
        .vulkan_result = bvb_wire_get_i32(input),
        .object_id = bvb_wire_get_u64(input + 8),
    };
    if (bvb_wire_get_u32(input + 4) != 0U ||
        (response->vulkan_result == 0 &&
         !wire_id_is_type(response->object_id, expected_type)) ||
        (response->vulkan_result != 0 && response->object_id != 0U))
        return -EPROTO;
    return 0;
}

int bvb_protocol_encode_vulkan_object_id(
    uint8_t output[BVB_VULKAN_OBJECT_ID_SIZE], uint64_t object_id,
    uint8_t expected_type) {
    if (output == NULL || !wire_id_is_type(object_id, expected_type))
        return -EINVAL;
    bvb_wire_put_u64(output, object_id);
    return 0;
}

int bvb_protocol_decode_vulkan_object_id(
    const uint8_t input[BVB_VULKAN_OBJECT_ID_SIZE], uint64_t *object_id,
    uint8_t expected_type) {
    if (input == NULL || object_id == NULL) return -EINVAL;
    *object_id = bvb_wire_get_u64(input);
    return wire_id_is_type(*object_id, expected_type) ? 0 : -EPROTO;
}

int bvb_protocol_encode_vulkan_buffer_requirements(
    uint8_t output[BVB_VULKAN_BUFFER_REQUIREMENTS_SIZE],
    const struct bvb_vulkan_buffer_requirements *requirements) {
    if (output == NULL || requirements == NULL || requirements->size == 0U ||
        requirements->alignment == 0U ||
        requirements->memory_type_bits == 0U) return -EINVAL;
    memset(output, 0, BVB_VULKAN_BUFFER_REQUIREMENTS_SIZE);
    bvb_wire_put_u64(output, requirements->size);
    bvb_wire_put_u64(output + 8, requirements->alignment);
    bvb_wire_put_u32(output + 16, requirements->memory_type_bits);
    return 0;
}

int bvb_protocol_decode_vulkan_buffer_requirements(
    const uint8_t input[BVB_VULKAN_BUFFER_REQUIREMENTS_SIZE],
    struct bvb_vulkan_buffer_requirements *requirements) {
    if (input == NULL || requirements == NULL) return -EINVAL;
    *requirements = (struct bvb_vulkan_buffer_requirements){
        .size = bvb_wire_get_u64(input),
        .alignment = bvb_wire_get_u64(input + 8),
        .memory_type_bits = bvb_wire_get_u32(input + 16),
    };
    return requirements->size != 0U && requirements->alignment != 0U &&
                   requirements->memory_type_bits != 0U &&
                   bvb_wire_get_u32(input + 20) == 0U
               ? 0 : -EPROTO;
}

int bvb_protocol_encode_vulkan_memory_allocate_request(
    uint8_t output[BVB_VULKAN_MEMORY_ALLOCATE_REQUEST_SIZE],
    const struct bvb_vulkan_memory_allocate_request *request) {
    if (output == NULL || request == NULL || request->allocation_size == 0U ||
        !wire_id_is_type(request->device_id, 3U)) return -EINVAL;
    memset(output, 0, BVB_VULKAN_MEMORY_ALLOCATE_REQUEST_SIZE);
    bvb_wire_put_u64(output, request->device_id);
    bvb_wire_put_u64(output + 8, request->allocation_size);
    bvb_wire_put_u32(output + 16, request->memory_type_index);
    return 0;
}

int bvb_protocol_decode_vulkan_memory_allocate_request(
    const uint8_t input[BVB_VULKAN_MEMORY_ALLOCATE_REQUEST_SIZE],
    struct bvb_vulkan_memory_allocate_request *request) {
    if (input == NULL || request == NULL) return -EINVAL;
    *request = (struct bvb_vulkan_memory_allocate_request){
        .device_id = bvb_wire_get_u64(input),
        .allocation_size = bvb_wire_get_u64(input + 8),
        .memory_type_index = bvb_wire_get_u32(input + 16),
    };
    return wire_id_is_type(request->device_id, 3U) &&
                   request->allocation_size != 0U &&
                   bvb_wire_get_u32(input + 20) == 0U
               ? 0 : -EPROTO;
}

int bvb_protocol_encode_vulkan_buffer_bind_request(
    uint8_t output[BVB_VULKAN_BUFFER_BIND_REQUEST_SIZE],
    const struct bvb_vulkan_buffer_bind_request *request) {
    if (output == NULL || request == NULL ||
        !wire_id_is_type(request->buffer_id, 19U) ||
        !wire_id_is_type(request->memory_id, 9U)) return -EINVAL;
    bvb_wire_put_u64(output, request->buffer_id);
    bvb_wire_put_u64(output + 8, request->memory_id);
    bvb_wire_put_u64(output + 16, request->offset);
    return 0;
}

int bvb_protocol_decode_vulkan_buffer_bind_request(
    const uint8_t input[BVB_VULKAN_BUFFER_BIND_REQUEST_SIZE],
    struct bvb_vulkan_buffer_bind_request *request) {
    if (input == NULL || request == NULL) return -EINVAL;
    *request = (struct bvb_vulkan_buffer_bind_request){
        .buffer_id = bvb_wire_get_u64(input),
        .memory_id = bvb_wire_get_u64(input + 8),
        .offset = bvb_wire_get_u64(input + 16),
    };
    return wire_id_is_type(request->buffer_id, 19U) &&
                   wire_id_is_type(request->memory_id, 9U)
               ? 0 : -EPROTO;
}

int bvb_protocol_encode_vulkan_command_buffer_fill_request(
    uint8_t output[BVB_VULKAN_COMMAND_BUFFER_FILL_REQUEST_SIZE],
    const struct bvb_vulkan_command_buffer_fill_request *request) {
    if (output == NULL || request == NULL || request->size == 0U ||
        !wire_id_is_type(request->command_buffer_id, 11U) ||
        !wire_id_is_type(request->buffer_id, 19U)) return -EINVAL;
    memset(output, 0, BVB_VULKAN_COMMAND_BUFFER_FILL_REQUEST_SIZE);
    bvb_wire_put_u64(output, request->command_buffer_id);
    bvb_wire_put_u64(output + 8, request->buffer_id);
    bvb_wire_put_u64(output + 16, request->offset);
    bvb_wire_put_u64(output + 24, request->size);
    bvb_wire_put_u32(output + 32, request->data);
    return 0;
}

int bvb_protocol_decode_vulkan_command_buffer_fill_request(
    const uint8_t input[BVB_VULKAN_COMMAND_BUFFER_FILL_REQUEST_SIZE],
    struct bvb_vulkan_command_buffer_fill_request *request) {
    if (input == NULL || request == NULL) return -EINVAL;
    *request = (struct bvb_vulkan_command_buffer_fill_request){
        .command_buffer_id = bvb_wire_get_u64(input),
        .buffer_id = bvb_wire_get_u64(input + 8),
        .offset = bvb_wire_get_u64(input + 16),
        .size = bvb_wire_get_u64(input + 24),
        .data = bvb_wire_get_u32(input + 32),
    };
    return wire_id_is_type(request->command_buffer_id, 11U) &&
                   wire_id_is_type(request->buffer_id, 19U) &&
                   request->size != 0U && bvb_wire_get_u32(input + 36) == 0U
               ? 0 : -EPROTO;
}

int bvb_protocol_encode_vulkan_memory_verify_fill_request(
    uint8_t output[BVB_VULKAN_MEMORY_VERIFY_FILL_REQUEST_SIZE],
    const struct bvb_vulkan_memory_verify_fill_request *request) {
    if (output == NULL || request == NULL || request->size == 0U ||
        !wire_id_is_type(request->memory_id, 9U)) return -EINVAL;
    memset(output, 0, BVB_VULKAN_MEMORY_VERIFY_FILL_REQUEST_SIZE);
    bvb_wire_put_u64(output, request->memory_id);
    bvb_wire_put_u64(output + 8, request->offset);
    bvb_wire_put_u64(output + 16, request->size);
    bvb_wire_put_u32(output + 24, request->expected_word);
    return 0;
}

int bvb_protocol_decode_vulkan_memory_verify_fill_request(
    const uint8_t input[BVB_VULKAN_MEMORY_VERIFY_FILL_REQUEST_SIZE],
    struct bvb_vulkan_memory_verify_fill_request *request) {
    if (input == NULL || request == NULL) return -EINVAL;
    *request = (struct bvb_vulkan_memory_verify_fill_request){
        .memory_id = bvb_wire_get_u64(input),
        .offset = bvb_wire_get_u64(input + 8),
        .size = bvb_wire_get_u64(input + 16),
        .expected_word = bvb_wire_get_u32(input + 24),
    };
    return wire_id_is_type(request->memory_id, 9U) && request->size != 0U &&
                   bvb_wire_get_u32(input + 28) == 0U
               ? 0 : -EPROTO;
}

int bvb_protocol_encode_vulkan_memory_verify_fill_response(
    uint8_t output[BVB_VULKAN_MEMORY_VERIFY_FILL_RESPONSE_SIZE],
    const struct bvb_vulkan_memory_verify_fill_response *response) {
    if (output == NULL || response == NULL) return -EINVAL;
    bvb_wire_put_i32(output, response->vulkan_result);
    bvb_wire_put_u32(output + 4, response->mismatched_words);
    return 0;
}

int bvb_protocol_decode_vulkan_memory_verify_fill_response(
    const uint8_t input[BVB_VULKAN_MEMORY_VERIFY_FILL_RESPONSE_SIZE],
    struct bvb_vulkan_memory_verify_fill_response *response) {
    if (input == NULL || response == NULL) return -EINVAL;
    *response = (struct bvb_vulkan_memory_verify_fill_response){
        .vulkan_result = bvb_wire_get_i32(input),
        .mismatched_words = bvb_wire_get_u32(input + 4),
    };
    return 0;
}

int bvb_protocol_encode_vulkan_fence_create_request(
    uint8_t output[BVB_VULKAN_FENCE_CREATE_REQUEST_SIZE],
    const struct bvb_vulkan_fence_create_request *request) {
    if (output == NULL || request == NULL ||
        !wire_id_is_type(request->device_id, 3U)) return -EINVAL;
    memset(output, 0, BVB_VULKAN_FENCE_CREATE_REQUEST_SIZE);
    bvb_wire_put_u64(output, request->device_id);
    bvb_wire_put_u32(output + 8, request->flags);
    return 0;
}

int bvb_protocol_decode_vulkan_fence_create_request(
    const uint8_t input[BVB_VULKAN_FENCE_CREATE_REQUEST_SIZE],
    struct bvb_vulkan_fence_create_request *request) {
    if (input == NULL || request == NULL) return -EINVAL;
    *request = (struct bvb_vulkan_fence_create_request){
        .device_id = bvb_wire_get_u64(input),
        .flags = bvb_wire_get_u32(input + 8),
    };
    return wire_id_is_type(request->device_id, 3U) &&
                   bvb_wire_get_u32(input + 12) == 0U
               ? 0 : -EPROTO;
}

int bvb_protocol_encode_vulkan_fence_wait_request(
    uint8_t output[BVB_VULKAN_FENCE_WAIT_REQUEST_SIZE],
    const struct bvb_vulkan_fence_wait_request *request) {
    if (output == NULL || request == NULL || request->wait_all > 1U ||
        !wire_id_is_type(request->fence_id, 18U)) return -EINVAL;
    memset(output, 0, BVB_VULKAN_FENCE_WAIT_REQUEST_SIZE);
    bvb_wire_put_u64(output, request->fence_id);
    bvb_wire_put_u64(output + 8, request->timeout);
    bvb_wire_put_u32(output + 16, request->wait_all);
    return 0;
}

int bvb_protocol_decode_vulkan_fence_wait_request(
    const uint8_t input[BVB_VULKAN_FENCE_WAIT_REQUEST_SIZE],
    struct bvb_vulkan_fence_wait_request *request) {
    if (input == NULL || request == NULL) return -EINVAL;
    *request = (struct bvb_vulkan_fence_wait_request){
        .fence_id = bvb_wire_get_u64(input),
        .timeout = bvb_wire_get_u64(input + 8),
        .wait_all = bvb_wire_get_u32(input + 16),
    };
    return wire_id_is_type(request->fence_id, 18U) &&
                   request->wait_all <= 1U &&
                   bvb_wire_get_u32(input + 20) == 0U
               ? 0 : -EPROTO;
}

int bvb_protocol_encode_vulkan_queue_submit_command_fence_request(
    uint8_t output[BVB_VULKAN_QUEUE_SUBMIT_COMMAND_FENCE_REQUEST_SIZE],
    const struct bvb_vulkan_queue_submit_command_fence_request *request) {
    if (output == NULL || request == NULL ||
        !wire_id_is_type(request->queue_id, 4U) ||
        !wire_id_is_type(request->command_buffer_id, 11U) ||
        !wire_id_is_type(request->fence_id, 18U)) return -EINVAL;
    bvb_wire_put_u64(output, request->queue_id);
    bvb_wire_put_u64(output + 8, request->command_buffer_id);
    bvb_wire_put_u64(output + 16, request->fence_id);
    return 0;
}

int bvb_protocol_decode_vulkan_queue_submit_command_fence_request(
    const uint8_t input[BVB_VULKAN_QUEUE_SUBMIT_COMMAND_FENCE_REQUEST_SIZE],
    struct bvb_vulkan_queue_submit_command_fence_request *request) {
    if (input == NULL || request == NULL) return -EINVAL;
    *request = (struct bvb_vulkan_queue_submit_command_fence_request){
        .queue_id = bvb_wire_get_u64(input),
        .command_buffer_id = bvb_wire_get_u64(input + 8),
        .fence_id = bvb_wire_get_u64(input + 16),
    };
    return wire_id_is_type(request->queue_id, 4U) &&
                   wire_id_is_type(request->command_buffer_id, 11U) &&
                   wire_id_is_type(request->fence_id, 18U)
               ? 0 : -EPROTO;
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

int bvb_protocol_encode_external_memory_import_request(
    uint8_t output[BVB_EXTERNAL_MEMORY_IMPORT_REQUEST_SIZE],
    const struct bvb_external_memory_import_request *request) {
    if (output == NULL || request == NULL || request->allocation_size == 0U ||
        request->allocation_size > BVB_SHARED_BATCH_MAX_BYTES ||
        request->memory_type_index >= 32U || request->buffer_bytes == 0U ||
        request->buffer_bytes > request->allocation_size) {
        return -EINVAL;
    }
    bvb_wire_put_u64(output, request->allocation_size);
    bvb_wire_put_u32(output + 8, request->memory_type_index);
    bvb_wire_put_u32(output + 12, request->buffer_bytes);
    return 0;
}

int bvb_protocol_decode_external_memory_import_request(
    const uint8_t input[BVB_EXTERNAL_MEMORY_IMPORT_REQUEST_SIZE],
    struct bvb_external_memory_import_request *request) {
    if (input == NULL || request == NULL) {
        return -EINVAL;
    }
    const struct bvb_external_memory_import_request decoded = {
        .allocation_size = bvb_wire_get_u64(input),
        .memory_type_index = bvb_wire_get_u32(input + 8),
        .buffer_bytes = bvb_wire_get_u32(input + 12),
    };
    uint8_t validation[BVB_EXTERNAL_MEMORY_IMPORT_REQUEST_SIZE];
    if (bvb_protocol_encode_external_memory_import_request(validation,
                                                           &decoded) != 0) {
        return -EPROTO;
    }
    *request = decoded;
    return 0;
}

int bvb_protocol_encode_external_sync_import_request(
    uint8_t output[BVB_EXTERNAL_SYNC_IMPORT_REQUEST_SIZE],
    const struct bvb_external_sync_import_request *request) {
    if (output == NULL || request == NULL || request->allocation_size == 0U ||
        request->allocation_size > BVB_SHARED_BATCH_MAX_BYTES ||
        request->memory_type_index >= 32U || request->buffer_bytes == 0U ||
        request->buffer_bytes > request->allocation_size ||
        (request->buffer_bytes % sizeof(uint32_t)) != 0U) {
        return -EINVAL;
    }
    bvb_wire_put_u64(output, request->allocation_size);
    bvb_wire_put_u32(output + 8, request->memory_type_index);
    bvb_wire_put_u32(output + 12, request->buffer_bytes);
    bvb_wire_put_u32(output + 16, request->expected_fill_word);
    bvb_wire_put_u32(output + 20, 0U);
    return 0;
}

int bvb_protocol_decode_external_sync_import_request(
    const uint8_t input[BVB_EXTERNAL_SYNC_IMPORT_REQUEST_SIZE],
    struct bvb_external_sync_import_request *request) {
    if (input == NULL || request == NULL || bvb_wire_get_u32(input + 20) != 0U) {
        return -EINVAL;
    }
    const struct bvb_external_sync_import_request decoded = {
        .allocation_size = bvb_wire_get_u64(input),
        .memory_type_index = bvb_wire_get_u32(input + 8),
        .buffer_bytes = bvb_wire_get_u32(input + 12),
        .expected_fill_word = bvb_wire_get_u32(input + 16),
    };
    uint8_t validation[BVB_EXTERNAL_SYNC_IMPORT_REQUEST_SIZE];
    if (bvb_protocol_encode_external_sync_import_request(validation,
                                                         &decoded) != 0) {
        return -EPROTO;
    }
    *request = decoded;
    return 0;
}

int bvb_protocol_encode_external_image_import_request(
    uint8_t output[BVB_EXTERNAL_IMAGE_IMPORT_REQUEST_SIZE],
    const struct bvb_external_image_import_request *request) {
    if (output == NULL || request == NULL || request->allocation_size == 0U ||
        request->allocation_size > BVB_SHARED_BATCH_MAX_BYTES ||
        request->memory_type_index >= 32U || request->width == 0U ||
        request->height == 0U || request->width > 4096U ||
        request->height > 4096U || request->format == 0U ||
        request->width > BVB_SHARED_BATCH_MAX_BYTES / 4U / request->height) {
        return -EINVAL;
    }
    bvb_wire_put_u64(output, request->allocation_size);
    bvb_wire_put_u32(output + 8, request->memory_type_index);
    bvb_wire_put_u32(output + 12, request->width);
    bvb_wire_put_u32(output + 16, request->height);
    bvb_wire_put_u32(output + 20, request->format);
    bvb_wire_put_u32(output + 24, request->expected_color);
    bvb_wire_put_u32(output + 28, 0U);
    return 0;
}

int bvb_protocol_decode_external_image_import_request(
    const uint8_t input[BVB_EXTERNAL_IMAGE_IMPORT_REQUEST_SIZE],
    struct bvb_external_image_import_request *request) {
    if (input == NULL || request == NULL || bvb_wire_get_u32(input + 28) != 0U) {
        return -EINVAL;
    }
    const struct bvb_external_image_import_request decoded = {
        .allocation_size = bvb_wire_get_u64(input),
        .memory_type_index = bvb_wire_get_u32(input + 8),
        .width = bvb_wire_get_u32(input + 12),
        .height = bvb_wire_get_u32(input + 16),
        .format = bvb_wire_get_u32(input + 20),
        .expected_color = bvb_wire_get_u32(input + 24),
    };
    uint8_t validation[BVB_EXTERNAL_IMAGE_IMPORT_REQUEST_SIZE];
    if (bvb_protocol_encode_external_image_import_request(validation,
                                                          &decoded) != 0) {
        return -EPROTO;
    }
    *request = decoded;
    return 0;
}

static int validate_memory_io_request(
    const struct bvb_vulkan_memory_io_request *request) {
    return request != NULL && wire_id_is_type(request->memory_id, 9U) &&
                   request->length != 0U &&
                   request->length <= BVB_VULKAN_MEMORY_IO_MAX_BYTES &&
                   request->offset <= UINT64_MAX - request->length
               ? 0
               : -EINVAL;
}

int bvb_protocol_encode_vulkan_memory_write_request(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_memory_io_request *request,
    const uint8_t *data, uint32_t *output_length) {
    if (output == NULL || data == NULL || output_length == NULL ||
        validate_memory_io_request(request) != 0) {
        return -EINVAL;
    }
    bvb_wire_put_u64(output, request->memory_id);
    bvb_wire_put_u64(output + 8, request->offset);
    bvb_wire_put_u32(output + 16, request->length);
    bvb_wire_put_u32(output + 20, 0U);
    memcpy(output + BVB_VULKAN_MEMORY_IO_PREFIX_SIZE, data, request->length);
    *output_length = BVB_VULKAN_MEMORY_IO_PREFIX_SIZE + request->length;
    return 0;
}

int bvb_protocol_decode_vulkan_memory_write_request(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_memory_io_request *request, const uint8_t **data) {
    if (input == NULL || request == NULL || data == NULL ||
        input_length < BVB_VULKAN_MEMORY_IO_PREFIX_SIZE) {
        return -EINVAL;
    }
    const struct bvb_vulkan_memory_io_request decoded = {
        .memory_id = bvb_wire_get_u64(input),
        .offset = bvb_wire_get_u64(input + 8),
        .length = bvb_wire_get_u32(input + 16),
    };
    if (validate_memory_io_request(&decoded) != 0 ||
        bvb_wire_get_u32(input + 20) != 0U ||
        input_length != BVB_VULKAN_MEMORY_IO_PREFIX_SIZE + decoded.length) {
        return -EPROTO;
    }
    *request = decoded;
    *data = input + BVB_VULKAN_MEMORY_IO_PREFIX_SIZE;
    return 0;
}

int bvb_protocol_encode_vulkan_memory_read_request(
    uint8_t output[BVB_VULKAN_MEMORY_IO_PREFIX_SIZE],
    const struct bvb_vulkan_memory_io_request *request) {
    if (output == NULL || validate_memory_io_request(request) != 0) {
        return -EINVAL;
    }
    bvb_wire_put_u64(output, request->memory_id);
    bvb_wire_put_u64(output + 8, request->offset);
    bvb_wire_put_u32(output + 16, request->length);
    bvb_wire_put_u32(output + 20, 0U);
    return 0;
}

int bvb_protocol_decode_vulkan_memory_read_request(
    const uint8_t input[BVB_VULKAN_MEMORY_IO_PREFIX_SIZE],
    struct bvb_vulkan_memory_io_request *request) {
    if (input == NULL || request == NULL) {
        return -EINVAL;
    }
    const struct bvb_vulkan_memory_io_request decoded = {
        .memory_id = bvb_wire_get_u64(input),
        .offset = bvb_wire_get_u64(input + 8),
        .length = bvb_wire_get_u32(input + 16),
    };
    if (validate_memory_io_request(&decoded) != 0 ||
        bvb_wire_get_u32(input + 20) != 0U) {
        return -EPROTO;
    }
    *request = decoded;
    return 0;
}

int bvb_protocol_encode_vulkan_memory_io_response(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_memory_io_response *response,
    const uint8_t *data, uint32_t *output_length) {
    if (output == NULL || response == NULL || output_length == NULL ||
        response->length > BVB_VULKAN_MEMORY_IO_MAX_BYTES ||
        (response->length != 0U && data == NULL) ||
        (response->vulkan_result != 0 && response->length != 0U)) {
        return -EINVAL;
    }
    bvb_wire_put_i32(output, response->vulkan_result);
    bvb_wire_put_u32(output + 4, response->length);
    if (response->length != 0U) {
        memcpy(output + BVB_VULKAN_MEMORY_IO_RESPONSE_PREFIX_SIZE, data,
               response->length);
    }
    *output_length =
        BVB_VULKAN_MEMORY_IO_RESPONSE_PREFIX_SIZE + response->length;
    return 0;
}

int bvb_protocol_decode_vulkan_memory_io_response(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_memory_io_response *response, const uint8_t **data) {
    if (input == NULL || response == NULL || data == NULL ||
        input_length < BVB_VULKAN_MEMORY_IO_RESPONSE_PREFIX_SIZE) {
        return -EINVAL;
    }
    const struct bvb_vulkan_memory_io_response decoded = {
        .vulkan_result = bvb_wire_get_i32(input),
        .length = bvb_wire_get_u32(input + 4),
    };
    if (decoded.length > BVB_VULKAN_MEMORY_IO_MAX_BYTES ||
        (decoded.vulkan_result != 0 && decoded.length != 0U) ||
        input_length !=
            BVB_VULKAN_MEMORY_IO_RESPONSE_PREFIX_SIZE + decoded.length) {
        return -EPROTO;
    }
    *response = decoded;
    *data = input + BVB_VULKAN_MEMORY_IO_RESPONSE_PREFIX_SIZE;
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
