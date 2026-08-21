#include <bvb/command_batch.h>
#include <bvb/protocol.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

_Static_assert(sizeof(struct bvb_vulkan_base_features) ==
                   BVB_VULKAN_BASE_FEATURES_SIZE,
               "base feature wire size changed");

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

int bvb_protocol_encode_vulkan_core_features(
    uint8_t output[BVB_VULKAN_CORE_FEATURES_SIZE],
    const struct bvb_vulkan_core_features *features) {
    if (output == NULL || features == NULL ||
        features->shader_draw_parameters > 1U ||
        features->buffer_device_address > 1U ||
        features->descriptor_indexing > 1U ||
        features->descriptor_binding_sampled_image_update_after_bind > 1U ||
        features->descriptor_binding_update_unused_while_pending > 1U ||
        features->descriptor_binding_partially_bound > 1U ||
        features->host_query_reset > 1U ||
        features->runtime_descriptor_array > 1U ||
        features->sampler_mirror_clamp_to_edge > 1U ||
        features->scalar_block_layout > 1U ||
        features->timeline_semaphore > 1U ||
        features->uniform_buffer_standard_layout > 1U ||
        features->vulkan_memory_model > 1U ||
        features->compute_full_subgroups > 1U ||
        features->dynamic_rendering > 1U ||
        features->maintenance4 > 1U ||
        features->shader_demote_to_helper_invocation > 1U ||
        features->shader_zero_initialize_workgroup_memory > 1U ||
        features->subgroup_size_control > 1U ||
        features->synchronization2 > 1U ||
        features->depth_clip_enable > 1U ||
        features->robust_buffer_access2 > 1U ||
        features->null_descriptor > 1U ||
        features->maintenance5 > 1U ||
        features->maintenance6 > 1U) {
        return -EINVAL;
    }
    bvb_wire_put_u32(output, features->shader_draw_parameters);
    bvb_wire_put_u32(output + 4, features->buffer_device_address);
    bvb_wire_put_u32(output + 8, features->descriptor_indexing);
    bvb_wire_put_u32(
        output + 12,
        features->descriptor_binding_sampled_image_update_after_bind);
    bvb_wire_put_u32(
        output + 16,
        features->descriptor_binding_update_unused_while_pending);
    bvb_wire_put_u32(
        output + 20, features->descriptor_binding_partially_bound);
    bvb_wire_put_u32(output + 24, features->host_query_reset);
    bvb_wire_put_u32(output + 28, features->runtime_descriptor_array);
    bvb_wire_put_u32(output + 32, features->sampler_mirror_clamp_to_edge);
    bvb_wire_put_u32(output + 36, features->scalar_block_layout);
    bvb_wire_put_u32(output + 40, features->timeline_semaphore);
    bvb_wire_put_u32(output + 44, features->uniform_buffer_standard_layout);
    bvb_wire_put_u32(output + 48, features->vulkan_memory_model);
    bvb_wire_put_u32(output + 52, features->compute_full_subgroups);
    bvb_wire_put_u32(output + 56, features->dynamic_rendering);
    bvb_wire_put_u32(output + 60, features->maintenance4);
    bvb_wire_put_u32(
        output + 64, features->shader_demote_to_helper_invocation);
    bvb_wire_put_u32(
        output + 68,
        features->shader_zero_initialize_workgroup_memory);
    bvb_wire_put_u32(output + 72, features->subgroup_size_control);
    bvb_wire_put_u32(output + 76, features->synchronization2);
    bvb_wire_put_u32(output + 80, features->depth_clip_enable);
    bvb_wire_put_u32(output + 84, features->robust_buffer_access2);
    bvb_wire_put_u32(output + 88, features->null_descriptor);
    bvb_wire_put_u32(output + 92, features->maintenance5);
    bvb_wire_put_u32(output + 96, features->maintenance6);
    return 0;
}

int bvb_protocol_decode_vulkan_core_features(
    const uint8_t input[BVB_VULKAN_CORE_FEATURES_SIZE],
    struct bvb_vulkan_core_features *features) {
    if (input == NULL || features == NULL) {
        return -EINVAL;
    }
    const uint32_t shader_draw_parameters = bvb_wire_get_u32(input);
    const uint32_t buffer_device_address = bvb_wire_get_u32(input + 4);
    const uint32_t descriptor_indexing = bvb_wire_get_u32(input + 8);
    const uint32_t descriptor_binding_sampled_image_update_after_bind =
        bvb_wire_get_u32(input + 12);
    const uint32_t descriptor_binding_update_unused_while_pending =
        bvb_wire_get_u32(input + 16);
    const uint32_t descriptor_binding_partially_bound =
        bvb_wire_get_u32(input + 20);
    const uint32_t host_query_reset = bvb_wire_get_u32(input + 24);
    const uint32_t runtime_descriptor_array =
        bvb_wire_get_u32(input + 28);
    const uint32_t sampler_mirror_clamp_to_edge =
        bvb_wire_get_u32(input + 32);
    const uint32_t scalar_block_layout = bvb_wire_get_u32(input + 36);
    const uint32_t timeline_semaphore = bvb_wire_get_u32(input + 40);
    const uint32_t uniform_buffer_standard_layout =
        bvb_wire_get_u32(input + 44);
    const uint32_t vulkan_memory_model = bvb_wire_get_u32(input + 48);
    const uint32_t compute_full_subgroups = bvb_wire_get_u32(input + 52);
    const uint32_t dynamic_rendering = bvb_wire_get_u32(input + 56);
    const uint32_t maintenance4 = bvb_wire_get_u32(input + 60);
    const uint32_t shader_demote_to_helper_invocation =
        bvb_wire_get_u32(input + 64);
    const uint32_t shader_zero_initialize_workgroup_memory =
        bvb_wire_get_u32(input + 68);
    const uint32_t subgroup_size_control = bvb_wire_get_u32(input + 72);
    const uint32_t synchronization2 = bvb_wire_get_u32(input + 76);
    const uint32_t depth_clip_enable = bvb_wire_get_u32(input + 80);
    const uint32_t robust_buffer_access2 = bvb_wire_get_u32(input + 84);
    const uint32_t null_descriptor = bvb_wire_get_u32(input + 88);
    const uint32_t maintenance5 = bvb_wire_get_u32(input + 92);
    const uint32_t maintenance6 = bvb_wire_get_u32(input + 96);
    if (shader_draw_parameters > 1U || buffer_device_address > 1U ||
        descriptor_indexing > 1U ||
        descriptor_binding_sampled_image_update_after_bind > 1U ||
        descriptor_binding_update_unused_while_pending > 1U ||
        descriptor_binding_partially_bound > 1U || host_query_reset > 1U ||
        runtime_descriptor_array > 1U ||
        sampler_mirror_clamp_to_edge > 1U || scalar_block_layout > 1U ||
        timeline_semaphore > 1U || uniform_buffer_standard_layout > 1U ||
        vulkan_memory_model > 1U || compute_full_subgroups > 1U ||
        dynamic_rendering > 1U || maintenance4 > 1U ||
        shader_demote_to_helper_invocation > 1U ||
        shader_zero_initialize_workgroup_memory > 1U ||
        subgroup_size_control > 1U || synchronization2 > 1U) {
        return -EPROTO;
    }
    if (depth_clip_enable > 1U || robust_buffer_access2 > 1U ||
        null_descriptor > 1U || maintenance5 > 1U || maintenance6 > 1U) {
        return -EPROTO;
    }
    *features = (struct bvb_vulkan_core_features){
        .shader_draw_parameters = shader_draw_parameters,
        .buffer_device_address = buffer_device_address,
        .descriptor_indexing = descriptor_indexing,
        .descriptor_binding_sampled_image_update_after_bind =
            descriptor_binding_sampled_image_update_after_bind,
        .descriptor_binding_update_unused_while_pending =
            descriptor_binding_update_unused_while_pending,
        .descriptor_binding_partially_bound =
            descriptor_binding_partially_bound,
        .host_query_reset = host_query_reset,
        .runtime_descriptor_array = runtime_descriptor_array,
        .sampler_mirror_clamp_to_edge = sampler_mirror_clamp_to_edge,
        .scalar_block_layout = scalar_block_layout,
        .timeline_semaphore = timeline_semaphore,
        .uniform_buffer_standard_layout = uniform_buffer_standard_layout,
        .vulkan_memory_model = vulkan_memory_model,
        .compute_full_subgroups = compute_full_subgroups,
        .dynamic_rendering = dynamic_rendering,
        .maintenance4 = maintenance4,
        .shader_demote_to_helper_invocation =
            shader_demote_to_helper_invocation,
        .shader_zero_initialize_workgroup_memory =
            shader_zero_initialize_workgroup_memory,
        .subgroup_size_control = subgroup_size_control,
        .synchronization2 = synchronization2,
        .depth_clip_enable = depth_clip_enable,
        .robust_buffer_access2 = robust_buffer_access2,
        .null_descriptor = null_descriptor,
        .maintenance5 = maintenance5,
        .maintenance6 = maintenance6,
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

static int validate_packed_device_create_counts(
    const struct bvb_vulkan_device_create_packed_request *request) {
    if (!wire_id_is_type(request->physical_device_id, 2U) ||
        request->queue_create_info_count == 0U ||
        request->queue_create_info_count >
            BVB_VULKAN_MAX_DEVICE_QUEUE_CREATE_INFOS ||
        request->queue_priority_count == 0U ||
        request->queue_priority_count >
            BVB_VULKAN_MAX_DEVICE_QUEUE_PRIORITIES ||
        request->enabled_extension_count >
            BVB_VULKAN_MAX_DEVICE_CREATE_EXTENSIONS ||
        (request->enabled_feature_structs &
         ~BVB_VULKAN_DEVICE_FEATURE_STRUCT_MASK) != 0U) {
        return -EPROTO;
    }
    for (uint32_t index = 0U;
         index < request->queue_create_info_count; ++index) {
        const struct bvb_vulkan_device_queue_create_info *info =
            &request->queue_create_infos[index];
        if (info->queue_count == 0U ||
            info->first_priority >= request->queue_priority_count ||
            info->queue_count >
                request->queue_priority_count - info->first_priority) {
            return -EPROTO;
        }
    }
    return 0;
}

static bool packed_device_feature_groups_match_mask(
    const struct bvb_vulkan_device_create_packed_request *request) {
    const struct bvb_vulkan_core_features *features =
        &request->enabled_features;
    if ((request->enabled_feature_structs &
         BVB_VULKAN_DEVICE_FEATURE_VULKAN_11) == 0U &&
        features->shader_draw_parameters != 0U) {
        return false;
    }
    if ((request->enabled_feature_structs &
         BVB_VULKAN_DEVICE_FEATURE_VULKAN_12) == 0U &&
        (features->buffer_device_address != 0U ||
         features->descriptor_indexing != 0U ||
         features->descriptor_binding_sampled_image_update_after_bind != 0U ||
         features->descriptor_binding_update_unused_while_pending != 0U ||
         features->descriptor_binding_partially_bound != 0U ||
         features->host_query_reset != 0U ||
         features->runtime_descriptor_array != 0U ||
         features->sampler_mirror_clamp_to_edge != 0U ||
         features->scalar_block_layout != 0U ||
         features->timeline_semaphore != 0U ||
         features->uniform_buffer_standard_layout != 0U ||
         features->vulkan_memory_model != 0U)) {
        return false;
    }
    if ((request->enabled_feature_structs &
         BVB_VULKAN_DEVICE_FEATURE_VULKAN_13) == 0U &&
        (features->compute_full_subgroups != 0U ||
         features->dynamic_rendering != 0U ||
         features->maintenance4 != 0U ||
         features->shader_demote_to_helper_invocation != 0U ||
         features->shader_zero_initialize_workgroup_memory != 0U ||
         features->subgroup_size_control != 0U ||
         features->synchronization2 != 0U)) {
        return false;
    }
    if ((request->enabled_feature_structs &
         BVB_VULKAN_DEVICE_FEATURE_DEPTH_CLIP_ENABLE) == 0U &&
        features->depth_clip_enable != 0U) {
        return false;
    }
    if ((request->enabled_feature_structs &
         BVB_VULKAN_DEVICE_FEATURE_ROBUSTNESS_2) == 0U &&
        (features->robust_buffer_access2 != 0U ||
         features->null_descriptor != 0U)) {
        return false;
    }
    if ((request->enabled_feature_structs &
         BVB_VULKAN_DEVICE_FEATURE_MAINTENANCE_5) == 0U &&
        features->maintenance5 != 0U) {
        return false;
    }
    if ((request->enabled_feature_structs &
         BVB_VULKAN_DEVICE_FEATURE_MAINTENANCE_6) == 0U &&
        features->maintenance6 != 0U) {
        return false;
    }
    for (size_t index = 0U;
         index < sizeof(request->enabled_base_features.values) /
                 sizeof(request->enabled_base_features.values[0]);
         ++index) {
        if (request->enabled_base_features.values[index] > 1U ||
            ((request->enabled_feature_structs &
              BVB_VULKAN_DEVICE_FEATURE_BASE) == 0U &&
             request->enabled_base_features.values[index] != 0U)) {
            return false;
        }
    }
    return true;
}

static void encode_vulkan_base_features(
    uint8_t output[BVB_VULKAN_BASE_FEATURES_SIZE],
    const struct bvb_vulkan_base_features *features) {
    for (size_t index = 0U;
         index < sizeof(features->values) / sizeof(features->values[0]);
         ++index) {
        bvb_wire_put_u32(
            output + index * sizeof(uint32_t), features->values[index]);
    }
}

static int decode_vulkan_base_features(
    const uint8_t input[BVB_VULKAN_BASE_FEATURES_SIZE],
    struct bvb_vulkan_base_features *features) {
    for (size_t index = 0U;
         index < sizeof(features->values) / sizeof(features->values[0]);
         ++index) {
        features->values[index] =
            bvb_wire_get_u32(input + index * sizeof(uint32_t));
        if (features->values[index] > 1U) {
            return -EPROTO;
        }
    }
    return 0;
}

int bvb_protocol_encode_vulkan_device_create_packed_request(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_device_create_packed_request *request,
    uint32_t *output_length) {
    if (output == NULL || request == NULL || output_length == NULL ||
        validate_packed_device_create_counts(request) != 0 ||
        !packed_device_feature_groups_match_mask(request)) {
        return -EINVAL;
    }
    uint32_t length = BVB_VULKAN_DEVICE_CREATE_PACKED_PREFIX_SIZE +
        (request->enabled_feature_structs == 0U
             ? 0U : BVB_VULKAN_CORE_FEATURES_SIZE) +
        ((request->enabled_feature_structs &
          BVB_VULKAN_DEVICE_FEATURE_BASE) == 0U
             ? 0U : BVB_VULKAN_BASE_FEATURES_SIZE) +
        request->queue_create_info_count *
            BVB_VULKAN_DEVICE_QUEUE_CREATE_INFO_SIZE +
        request->queue_priority_count * sizeof(uint32_t);
    size_t extension_lengths[BVB_VULKAN_MAX_DEVICE_CREATE_EXTENSIONS] = {0};
    for (uint32_t index = 0U;
         index < request->enabled_extension_count; ++index) {
        const char *name = request->enabled_extensions[index];
        const char *terminator = memchr(
            name, '\0', BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE);
        if (name[0] == '\0' || terminator == NULL) {
            return -EINVAL;
        }
        extension_lengths[index] = (size_t)(terminator - name) + 1U;
        if (extension_lengths[index] > BVB_PROTOCOL_MAX_PAYLOAD - length) {
            return -EMSGSIZE;
        }
        length += (uint32_t)extension_lengths[index];
    }
    memset(output, 0, length);
    bvb_wire_put_u64(output, request->physical_device_id);
    bvb_wire_put_u32(output + 8, request->flags);
    bvb_wire_put_u32(output + 12, request->queue_create_info_count);
    bvb_wire_put_u32(output + 16, request->queue_priority_count);
    bvb_wire_put_u32(output + 20, request->enabled_layer_count);
    bvb_wire_put_u32(output + 24, request->enabled_extension_count);
    bvb_wire_put_u32(output + 28, request->enabled_feature_structs);
    uint32_t cursor = BVB_VULKAN_DEVICE_CREATE_PACKED_PREFIX_SIZE;
    if (request->enabled_feature_structs != 0U) {
        if (bvb_protocol_encode_vulkan_core_features(
                output + cursor, &request->enabled_features) != 0) {
            return -EINVAL;
        }
        cursor += BVB_VULKAN_CORE_FEATURES_SIZE;
    }
    if ((request->enabled_feature_structs &
         BVB_VULKAN_DEVICE_FEATURE_BASE) != 0U) {
        encode_vulkan_base_features(
            output + cursor, &request->enabled_base_features);
        cursor += BVB_VULKAN_BASE_FEATURES_SIZE;
    }
    for (uint32_t index = 0U;
         index < request->queue_create_info_count; ++index) {
        const struct bvb_vulkan_device_queue_create_info *info =
            &request->queue_create_infos[index];
        bvb_wire_put_u32(output + cursor, info->flags);
        bvb_wire_put_u32(output + cursor + 4, info->queue_family_index);
        bvb_wire_put_u32(output + cursor + 8, info->queue_count);
        bvb_wire_put_u32(output + cursor + 12, info->first_priority);
        cursor += BVB_VULKAN_DEVICE_QUEUE_CREATE_INFO_SIZE;
    }
    for (uint32_t index = 0U;
         index < request->queue_priority_count; ++index) {
        bvb_wire_put_u32(output + cursor, request->queue_priority_bits[index]);
        cursor += sizeof(uint32_t);
    }
    for (uint32_t index = 0U;
         index < request->enabled_extension_count; ++index) {
        memcpy(output + cursor, request->enabled_extensions[index],
               extension_lengths[index]);
        cursor += (uint32_t)extension_lengths[index];
    }
    *output_length = cursor;
    return 0;
}

int bvb_protocol_decode_vulkan_device_create_packed_request(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_device_create_packed_request *request) {
    if (input == NULL || request == NULL ||
        input_length < BVB_VULKAN_DEVICE_CREATE_PACKED_PREFIX_SIZE ||
        input_length > BVB_PROTOCOL_MAX_PAYLOAD) {
        return -EINVAL;
    }
    struct bvb_vulkan_device_create_packed_request decoded = {
        .physical_device_id = bvb_wire_get_u64(input),
        .flags = bvb_wire_get_u32(input + 8),
        .queue_create_info_count = bvb_wire_get_u32(input + 12),
        .queue_priority_count = bvb_wire_get_u32(input + 16),
        .enabled_layer_count = bvb_wire_get_u32(input + 20),
        .enabled_extension_count = bvb_wire_get_u32(input + 24),
        .enabled_feature_structs = bvb_wire_get_u32(input + 28),
    };
    if (!wire_id_is_type(decoded.physical_device_id, 2U) ||
        decoded.queue_create_info_count == 0U ||
        decoded.queue_create_info_count >
            BVB_VULKAN_MAX_DEVICE_QUEUE_CREATE_INFOS ||
        decoded.queue_priority_count == 0U ||
        decoded.queue_priority_count >
            BVB_VULKAN_MAX_DEVICE_QUEUE_PRIORITIES ||
        decoded.enabled_extension_count >
            BVB_VULKAN_MAX_DEVICE_CREATE_EXTENSIONS) {
        return -EPROTO;
    }
    uint32_t cursor = BVB_VULKAN_DEVICE_CREATE_PACKED_PREFIX_SIZE;
    if (decoded.enabled_feature_structs != 0U) {
        if (BVB_VULKAN_CORE_FEATURES_SIZE > input_length - cursor ||
            bvb_protocol_decode_vulkan_core_features(
                input + cursor, &decoded.enabled_features) != 0) {
            return -EPROTO;
        }
        cursor += BVB_VULKAN_CORE_FEATURES_SIZE;
    }
    if ((decoded.enabled_feature_structs &
         BVB_VULKAN_DEVICE_FEATURE_BASE) != 0U) {
        if (BVB_VULKAN_BASE_FEATURES_SIZE > input_length - cursor ||
            decode_vulkan_base_features(
                input + cursor, &decoded.enabled_base_features) != 0) {
            return -EPROTO;
        }
        cursor += BVB_VULKAN_BASE_FEATURES_SIZE;
    }
    if (!packed_device_feature_groups_match_mask(&decoded)) {
        return -EPROTO;
    }
    const uint32_t fixed_tail =
        decoded.queue_create_info_count *
            BVB_VULKAN_DEVICE_QUEUE_CREATE_INFO_SIZE +
        decoded.queue_priority_count * sizeof(uint32_t);
    if (fixed_tail > input_length - cursor) {
        return -EPROTO;
    }
    for (uint32_t index = 0U;
         index < decoded.queue_create_info_count; ++index) {
        decoded.queue_create_infos[index] =
            (struct bvb_vulkan_device_queue_create_info){
                .flags = bvb_wire_get_u32(input + cursor),
                .queue_family_index = bvb_wire_get_u32(input + cursor + 4),
                .queue_count = bvb_wire_get_u32(input + cursor + 8),
                .first_priority = bvb_wire_get_u32(input + cursor + 12),
            };
        cursor += BVB_VULKAN_DEVICE_QUEUE_CREATE_INFO_SIZE;
    }
    for (uint32_t index = 0U;
         index < decoded.queue_priority_count; ++index) {
        decoded.queue_priority_bits[index] = bvb_wire_get_u32(input + cursor);
        cursor += sizeof(uint32_t);
    }
    if (validate_packed_device_create_counts(&decoded) != 0) {
        return -EPROTO;
    }
    for (uint32_t index = 0U;
         index < decoded.enabled_extension_count; ++index) {
        if (cursor == input_length) {
            return -EPROTO;
        }
        uint32_t available = input_length - cursor;
        if (available > BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE) {
            available = BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE;
        }
        const uint8_t *terminator = memchr(input + cursor, '\0', available);
        if (input[cursor] == '\0' || terminator == NULL) {
            return -EPROTO;
        }
        const size_t name_length =
            (size_t)(terminator - (input + cursor)) + 1U;
        memcpy(decoded.enabled_extensions[index], input + cursor, name_length);
        cursor += (uint32_t)name_length;
    }
    if (cursor != input_length) {
        return -EPROTO;
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

int bvb_protocol_encode_vulkan_swapchain_prepare_request(
    uint8_t output[BVB_VULKAN_SWAPCHAIN_PREPARE_REQUEST_SIZE],
    const struct bvb_vulkan_swapchain_prepare_request *request) {
    if (output == NULL || request == NULL ||
        !wire_id_is_type(request->device_id, BVB_OBJECT_DEVICE) ||
        request->width == 0U || request->height == 0U ||
        request->format == 0U || request->image_usage == 0U ||
        request->min_image_count < 2U ||
        request->min_image_count > BVB_WSI_FRAME_RING_MAX_SLOTS ||
        request->flags != 0U || request->generation == 0U) {
        return -EINVAL;
    }
    memset(output, 0, BVB_VULKAN_SWAPCHAIN_PREPARE_REQUEST_SIZE);
    bvb_wire_put_u64(output, request->device_id);
    bvb_wire_put_u32(output + 8, request->width);
    bvb_wire_put_u32(output + 12, request->height);
    bvb_wire_put_u32(output + 16, request->format);
    bvb_wire_put_u32(output + 20, request->image_usage);
    bvb_wire_put_u32(output + 24, request->min_image_count);
    bvb_wire_put_u32(output + 28, request->flags);
    bvb_wire_put_u64(output + 32, request->generation);
    return 0;
}

int bvb_protocol_decode_vulkan_swapchain_prepare_request(
    const uint8_t input[BVB_VULKAN_SWAPCHAIN_PREPARE_REQUEST_SIZE],
    struct bvb_vulkan_swapchain_prepare_request *request) {
    if (input == NULL || request == NULL) return -EINVAL;
    const struct bvb_vulkan_swapchain_prepare_request decoded = {
        .device_id = bvb_wire_get_u64(input),
        .width = bvb_wire_get_u32(input + 8),
        .height = bvb_wire_get_u32(input + 12),
        .format = bvb_wire_get_u32(input + 16),
        .image_usage = bvb_wire_get_u32(input + 20),
        .min_image_count = bvb_wire_get_u32(input + 24),
        .flags = bvb_wire_get_u32(input + 28),
        .generation = bvb_wire_get_u64(input + 32),
    };
    uint8_t validation[BVB_VULKAN_SWAPCHAIN_PREPARE_REQUEST_SIZE];
    if (bvb_protocol_encode_vulkan_swapchain_prepare_request(
            validation, &decoded) != 0) {
        return -EPROTO;
    }
    *request = decoded;
    return 0;
}

static int swapchain_response_is_valid(
    const struct bvb_vulkan_swapchain_prepare_response *response) {
    if (response == NULL) return -EINVAL;
    if (response->vulkan_result != 0) {
        if (response->image_count != 0U || response->swapchain_id != 0U ||
            response->generation != 0U ||
            response->control_region_bytes != 0U) {
            return -EINVAL;
        }
        for (uint32_t index = 0U;
             index < BVB_WSI_FRAME_RING_MAX_SLOTS; ++index) {
            if (response->images[index].image_id != 0U ||
                response->images[index].allocation_size != 0U ||
                response->images[index].memory_type_index != 0U) {
                return -EINVAL;
            }
        }
        return 0;
    }
    if (response->image_count < 2U ||
        response->image_count > BVB_WSI_FRAME_RING_MAX_SLOTS ||
        !wire_id_is_type(response->swapchain_id, BVB_OBJECT_SWAPCHAIN) ||
        response->generation == 0U ||
        response->control_region_bytes != BVB_WSI_FRAME_RING_REGION_BYTES) {
        return -EINVAL;
    }
    for (uint32_t index = 0U;
         index < BVB_WSI_FRAME_RING_MAX_SLOTS; ++index) {
        const struct bvb_vulkan_swapchain_image_record *image =
            &response->images[index];
        if (index < response->image_count) {
            if (!wire_id_is_type(image->image_id, BVB_OBJECT_IMAGE) ||
                image->allocation_size == 0U) return -EINVAL;
        } else if (image->image_id != 0U || image->allocation_size != 0U ||
                   image->memory_type_index != 0U) {
            return -EINVAL;
        }
    }
    return 0;
}

int bvb_protocol_encode_vulkan_swapchain_prepare_response(
    uint8_t output[BVB_VULKAN_SWAPCHAIN_PREPARE_RESPONSE_SIZE],
    const struct bvb_vulkan_swapchain_prepare_response *response) {
    if (output == NULL || swapchain_response_is_valid(response) != 0) {
        return -EINVAL;
    }
    memset(output, 0, BVB_VULKAN_SWAPCHAIN_PREPARE_RESPONSE_SIZE);
    bvb_wire_put_i32(output, response->vulkan_result);
    bvb_wire_put_u32(output + 4, response->image_count);
    bvb_wire_put_u64(output + 8, response->swapchain_id);
    bvb_wire_put_u64(output + 16, response->generation);
    bvb_wire_put_u32(output + 24, response->control_region_bytes);
    for (uint32_t index = 0U; index < response->image_count; ++index) {
        const uint32_t offset = 32U +
            index * BVB_VULKAN_SWAPCHAIN_IMAGE_RECORD_SIZE;
        bvb_wire_put_u64(output + offset, response->images[index].image_id);
        bvb_wire_put_u64(output + offset + 8,
                         response->images[index].allocation_size);
        bvb_wire_put_u32(output + offset + 16,
                         response->images[index].memory_type_index);
    }
    return 0;
}

int bvb_protocol_decode_vulkan_swapchain_prepare_response(
    const uint8_t input[BVB_VULKAN_SWAPCHAIN_PREPARE_RESPONSE_SIZE],
    struct bvb_vulkan_swapchain_prepare_response *response) {
    if (input == NULL || response == NULL ||
        bvb_wire_get_u32(input + 28) != 0U) return -EINVAL;
    struct bvb_vulkan_swapchain_prepare_response decoded = {
        .vulkan_result = bvb_wire_get_i32(input),
        .image_count = bvb_wire_get_u32(input + 4),
        .swapchain_id = bvb_wire_get_u64(input + 8),
        .generation = bvb_wire_get_u64(input + 16),
        .control_region_bytes = bvb_wire_get_u32(input + 24),
    };
    for (uint32_t index = 0U;
         index < BVB_WSI_FRAME_RING_MAX_SLOTS; ++index) {
        const uint32_t offset = 32U +
            index * BVB_VULKAN_SWAPCHAIN_IMAGE_RECORD_SIZE;
        decoded.images[index] = (struct bvb_vulkan_swapchain_image_record){
            .image_id = bvb_wire_get_u64(input + offset),
            .allocation_size = bvb_wire_get_u64(input + offset + 8),
            .memory_type_index = bvb_wire_get_u32(input + offset + 16),
        };
        if (bvb_wire_get_u32(input + offset + 20) != 0U) return -EPROTO;
    }
    if (swapchain_response_is_valid(&decoded) != 0) return -EPROTO;
    *response = decoded;
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
