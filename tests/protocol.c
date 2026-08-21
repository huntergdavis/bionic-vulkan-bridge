#include <bvb/protocol.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #expression);                                               \
            return EXIT_FAILURE;                                                \
        }                                                                       \
    } while (0)

int main(void) {
    const struct bvb_protocol_header header = {
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_HELLO,
        .request_id = 0x78563412U,
        .payload_length = BVB_HELLO_REQUEST_SIZE,
        .status = 0,
    };
    uint8_t wire[BVB_PROTOCOL_HEADER_SIZE] = {0};
    CHECK(bvb_protocol_encode_header(wire, &header) == 0);
    const uint8_t expected[BVB_PROTOCOL_HEADER_SIZE] = {
        0x42, 0x56, 0x42, 0x31, 0x01, 0x00, 0x01, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x12, 0x34, 0x56, 0x78,
        0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    CHECK(memcmp(wire, expected, sizeof(expected)) == 0);

    struct bvb_protocol_header decoded;
    CHECK(bvb_protocol_decode_header(wire, &decoded) == 0);
    CHECK(decoded.version == header.version);
    CHECK(decoded.request_id == header.request_id);
    CHECK(decoded.payload_length == header.payload_length);

    wire[0] ^= 1U;
    CHECK(bvb_protocol_decode_header(wire, &decoded) == -EPROTO);
    wire[0] ^= 1U;
    wire[10] = 1U;
    CHECK(bvb_protocol_decode_header(wire, &decoded) == -EPROTO);
    wire[10] = 0U;
    bvb_wire_put_u32(wire + 16, BVB_PROTOCOL_MAX_PAYLOAD + 1U);
    CHECK(bvb_protocol_decode_header(wire, &decoded) == -EMSGSIZE);

    const struct bvb_protocol_header inline_header = {
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VISIBLE_BATCH_INLINE,
        .request_id = 0x10203040U,
        .payload_length = BVB_VISIBLE_BATCH_INLINE_PREFIX_SIZE + 200U,
    };
    CHECK(bvb_protocol_encode_header(wire, &inline_header) == 0);
    CHECK(bvb_protocol_decode_header(wire, &decoded) == 0);
    CHECK(decoded.opcode == BVB_OPCODE_VISIBLE_BATCH_INLINE);
    CHECK(decoded.payload_length ==
          BVB_VISIBLE_BATCH_INLINE_PREFIX_SIZE + 200U);

    const struct bvb_protocol_header global_header = {
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_INSTANCE_CREATE,
        .request_id = 0x50607080U,
        .payload_length = BVB_VULKAN_INSTANCE_CREATE_REQUEST_SIZE,
    };
    CHECK(bvb_protocol_encode_header(wire, &global_header) == 0);
    CHECK(bvb_protocol_decode_header(wire, &decoded) == 0);
    CHECK(decoded.opcode == BVB_OPCODE_VULKAN_INSTANCE_CREATE);

    const struct bvb_protocol_header last_opcode_header = {
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_LAST,
        .request_id = 0x90a0b0c0U,
        .payload_length = BVB_VULKAN_MEMORY_IO_PREFIX_SIZE,
    };
    CHECK(bvb_protocol_encode_header(wire, &last_opcode_header) == 0);
    CHECK(bvb_protocol_decode_header(wire, &decoded) == 0);
    CHECK(decoded.opcode == BVB_OPCODE_VULKAN_CORE_FEATURES);

    const struct bvb_hello_request hello = {
        .minimum_version = 1,
        .maximum_version = 3,
        .client_flags = 0x89abcdefU,
    };
    uint8_t hello_wire[BVB_HELLO_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_hello_request(hello_wire, &hello) == 0);
    struct bvb_hello_request hello_decoded;
    CHECK(bvb_protocol_decode_hello_request(hello_wire, &hello_decoded) == 0);
    CHECK(hello_decoded.minimum_version == 1);
    CHECK(hello_decoded.maximum_version == 3);
    CHECK(hello_decoded.client_flags == 0x89abcdefU);

    const struct bvb_hello_response response = {
        .negotiated_version = 1,
        .service_flags = BVB_SERVICE_BIONIC |
                         BVB_SERVICE_ANDROID_VULKAN_LOADER,
        .pointer_bits = 64,
        .page_size = 4096,
    };
    uint8_t response_wire[BVB_HELLO_RESPONSE_SIZE];
    CHECK(bvb_protocol_encode_hello_response(response_wire, &response) == 0);
    struct bvb_hello_response response_decoded;
    CHECK(bvb_protocol_decode_hello_response(response_wire, &response_decoded) ==
          0);
    CHECK(response_decoded.negotiated_version == 1);
    CHECK(response_decoded.service_flags == 3U);
    CHECK(response_decoded.pointer_bits == 64U);
    CHECK(response_decoded.page_size == 4096U);

    const struct bvb_external_memory_import_request external_import = {
        .allocation_size = 8192U,
        .memory_type_index = 4U,
        .buffer_bytes = 4096U,
    };
    uint8_t external_import_wire[BVB_EXTERNAL_MEMORY_IMPORT_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_external_memory_import_request(
              external_import_wire, &external_import) == 0);
    struct bvb_external_memory_import_request external_import_decoded;
    CHECK(bvb_protocol_decode_external_memory_import_request(
              external_import_wire, &external_import_decoded) == 0);
    CHECK(external_import_decoded.allocation_size == 8192U);
    CHECK(external_import_decoded.memory_type_index == 4U);
    CHECK(external_import_decoded.buffer_bytes == 4096U);
    bvb_wire_put_u32(external_import_wire + 8, 32U);
    CHECK(bvb_protocol_decode_external_memory_import_request(
              external_import_wire, &external_import_decoded) == -EPROTO);

    const struct bvb_external_sync_import_request external_sync = {
        .allocation_size = 8192U,
        .memory_type_index = 4U,
        .buffer_bytes = 4096U,
        .expected_fill_word = UINT32_C(0xe037c0de),
    };
    uint8_t external_sync_wire[BVB_EXTERNAL_SYNC_IMPORT_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_external_sync_import_request(
              external_sync_wire, &external_sync) == 0);
    struct bvb_external_sync_import_request external_sync_decoded;
    CHECK(bvb_protocol_decode_external_sync_import_request(
              external_sync_wire, &external_sync_decoded) == 0);
    CHECK(external_sync_decoded.allocation_size == 8192U);
    CHECK(external_sync_decoded.memory_type_index == 4U);
    CHECK(external_sync_decoded.buffer_bytes == 4096U);
    CHECK(external_sync_decoded.expected_fill_word == UINT32_C(0xe037c0de));
    bvb_wire_put_u32(external_sync_wire + 20, 1U);
    CHECK(bvb_protocol_decode_external_sync_import_request(
              external_sync_wire, &external_sync_decoded) == -EINVAL);

    const struct bvb_external_image_import_request external_image = {
        .allocation_size = 65536U,
        .memory_type_index = 2U,
        .width = 64U,
        .height = 64U,
        .format = 37U,
        .expected_color = UINT32_C(0xffff00ff),
    };
    uint8_t external_image_wire[BVB_EXTERNAL_IMAGE_IMPORT_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_external_image_import_request(
              external_image_wire, &external_image) == 0);
    struct bvb_external_image_import_request external_image_decoded;
    CHECK(bvb_protocol_decode_external_image_import_request(
              external_image_wire, &external_image_decoded) == 0);
    CHECK(external_image_decoded.allocation_size == 65536U);
    CHECK(external_image_decoded.memory_type_index == 2U);
    CHECK(external_image_decoded.width == 64U);
    CHECK(external_image_decoded.height == 64U);
    CHECK(external_image_decoded.format == 37U);
    CHECK(external_image_decoded.expected_color == UINT32_C(0xffff00ff));
    bvb_wire_put_u32(external_image_wire + 28, 1U);
    CHECK(bvb_protocol_decode_external_image_import_request(
              external_image_wire, &external_image_decoded) == -EINVAL);

    const struct bvb_vulkan_global_info global_info = {
        .loader_api_version = UINT32_C(0x00403000),
        .native_extension_count = 14U,
        .native_layer_count = 2U,
        .exposed_extension_count = 0U,
        .exposed_layer_count = 0U,
    };
    uint8_t global_info_wire[BVB_VULKAN_GLOBAL_INFO_SIZE];
    CHECK(bvb_protocol_encode_vulkan_global_info(global_info_wire,
                                                 &global_info) == 0);
    struct bvb_vulkan_global_info global_info_decoded;
    CHECK(bvb_protocol_decode_vulkan_global_info(global_info_wire,
                                                 &global_info_decoded) == 0);
    CHECK(global_info_decoded.loader_api_version ==
          global_info.loader_api_version);
    CHECK(global_info_decoded.native_extension_count == 14U);
    CHECK(global_info_decoded.exposed_extension_count == 0U);
    global_info_wire[20] = 1U;
    CHECK(bvb_protocol_decode_vulkan_global_info(global_info_wire,
                                                 &global_info_decoded) ==
          -EPROTO);
    global_info_wire[20] = 0U;

    const struct bvb_vulkan_instance_create_request create_request = {
        .api_version = UINT32_C(0x00401000),
    };
    uint8_t create_request_wire[BVB_VULKAN_INSTANCE_CREATE_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_instance_create_request(
              create_request_wire, &create_request) == 0);
    struct bvb_vulkan_instance_create_request create_request_decoded;
    CHECK(bvb_protocol_decode_vulkan_instance_create_request(
              create_request_wire, &create_request_decoded) == 0);
    CHECK(create_request_decoded.api_version == create_request.api_version);

    struct bvb_vulkan_instance_create_extended_request extended_instance = {
        .base = create_request,
    };
    extended_instance.base.enabled_extension_count = 1U;
    memcpy(extended_instance.enabled_extensions[0],
           "VK_KHR_get_physical_device_properties2",
           sizeof("VK_KHR_get_physical_device_properties2"));
    uint8_t extended_instance_wire[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t extended_instance_length = 0U;
    CHECK(bvb_protocol_encode_vulkan_instance_create_extended_request(
              extended_instance_wire, &extended_instance,
              &extended_instance_length) == 0);
    struct bvb_vulkan_instance_create_extended_request
        extended_instance_decoded;
    CHECK(bvb_protocol_decode_vulkan_instance_create_extended_request(
              extended_instance_wire, extended_instance_length,
              &extended_instance_decoded) == 0);
    CHECK(extended_instance_decoded.base.enabled_extension_count == 1U);
    CHECK(strcmp(extended_instance_decoded.enabled_extensions[0],
                 "VK_KHR_get_physical_device_properties2") == 0);

    const struct bvb_vulkan_instance_create_response create_response = {
        .vulkan_result = 0,
        .instance_id = UINT64_C(0x0100000000000001),
    };
    uint8_t create_response_wire[BVB_VULKAN_INSTANCE_CREATE_RESPONSE_SIZE];
    CHECK(bvb_protocol_encode_vulkan_instance_create_response(
              create_response_wire, &create_response) == 0);
    struct bvb_vulkan_instance_create_response create_response_decoded;
    CHECK(bvb_protocol_decode_vulkan_instance_create_response(
              create_response_wire, &create_response_decoded) == 0);
    CHECK(create_response_decoded.vulkan_result == 0);
    CHECK(create_response_decoded.instance_id == create_response.instance_id);

    uint8_t instance_id_wire[BVB_VULKAN_INSTANCE_ID_SIZE];
    CHECK(bvb_protocol_encode_vulkan_instance_id(
              instance_id_wire, create_response.instance_id) == 0);
    uint64_t instance_id_decoded = 0U;
    CHECK(bvb_protocol_decode_vulkan_instance_id(
              instance_id_wire, &instance_id_decoded) == 0);
    CHECK(instance_id_decoded == create_response.instance_id);

    const struct bvb_vulkan_physical_devices physical_devices = {
        .vulkan_result = 0,
        .count = 2U,
        .ids = {
            UINT64_C(0x0200000000000001),
            UINT64_C(0x0200000000000002),
        },
    };
    uint8_t physical_wire[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t physical_length = 0U;
    CHECK(bvb_protocol_encode_vulkan_physical_devices(
              physical_wire, &physical_devices, &physical_length) == 0);
    CHECK(physical_length == 24U);
    struct bvb_vulkan_physical_devices physical_decoded;
    CHECK(bvb_protocol_decode_vulkan_physical_devices(
              physical_wire, physical_length, &physical_decoded) == 0);
    CHECK(physical_decoded.count == 2U);
    CHECK(physical_decoded.ids[1] == physical_devices.ids[1]);

    uint8_t physical_id_wire[BVB_VULKAN_PHYSICAL_DEVICE_ID_SIZE];
    CHECK(bvb_protocol_encode_vulkan_physical_device_id(
              physical_id_wire, physical_devices.ids[0]) == 0);
    uint64_t physical_id_decoded = 0U;
    CHECK(bvb_protocol_decode_vulkan_physical_device_id(
              physical_id_wire, &physical_id_decoded) == 0);
    CHECK(physical_id_decoded == physical_devices.ids[0]);
    CHECK(bvb_protocol_decode_vulkan_instance_id(
              physical_id_wire, &instance_id_decoded) == -EPROTO);

    const struct bvb_vulkan_format_query format_query = {
        .physical_device_id = physical_devices.ids[0],
        .format = 37U,
    };
    uint8_t format_query_wire[BVB_VULKAN_FORMAT_QUERY_SIZE];
    CHECK(bvb_protocol_encode_vulkan_format_query(
              format_query_wire, &format_query) == 0);
    struct bvb_vulkan_format_query format_query_decoded;
    CHECK(bvb_protocol_decode_vulkan_format_query(
              format_query_wire, &format_query_decoded) == 0);
    CHECK(format_query_decoded.physical_device_id ==
          format_query.physical_device_id);
    CHECK(format_query_decoded.format == format_query.format);
    format_query_wire[12] = 1U;
    CHECK(bvb_protocol_decode_vulkan_format_query(
              format_query_wire, &format_query_decoded) == -EPROTO);
    format_query_wire[12] = 0U;

    const struct bvb_vulkan_format_properties format_properties = {
        .linear_tiling_features = 1U,
        .optimal_tiling_features = 2U,
        .buffer_features = 4U,
    };
    uint8_t format_properties_wire[BVB_VULKAN_FORMAT_PROPERTIES_SIZE];
    CHECK(bvb_protocol_encode_vulkan_format_properties(
              format_properties_wire, &format_properties) == 0);
    struct bvb_vulkan_format_properties format_properties_decoded;
    CHECK(bvb_protocol_decode_vulkan_format_properties(
              format_properties_wire, &format_properties_decoded) == 0);
    CHECK(format_properties_decoded.linear_tiling_features == 1U);
    CHECK(format_properties_decoded.optimal_tiling_features == 2U);
    CHECK(format_properties_decoded.buffer_features == 4U);

    const struct bvb_vulkan_image_format_query image_format_query = {
        .physical_device_id = physical_devices.ids[0],
        .format = 37U,
        .type = 1U,
        .tiling = 0U,
        .usage = 20U,
        .flags = 2U,
    };
    uint8_t image_format_query_wire[BVB_VULKAN_IMAGE_FORMAT_QUERY_SIZE];
    CHECK(bvb_protocol_encode_vulkan_image_format_query(
              image_format_query_wire, &image_format_query) == 0);
    struct bvb_vulkan_image_format_query image_format_query_decoded;
    CHECK(bvb_protocol_decode_vulkan_image_format_query(
              image_format_query_wire, &image_format_query_decoded) == 0);
    CHECK(image_format_query_decoded.physical_device_id ==
          image_format_query.physical_device_id);
    CHECK(image_format_query_decoded.format == 37U);
    CHECK(image_format_query_decoded.type == 1U);
    CHECK(image_format_query_decoded.usage == 20U);

    const struct bvb_vulkan_image_format_properties image_format_properties = {
        .vulkan_result = 0,
        .max_extent_width = 4096U,
        .max_extent_height = 2048U,
        .max_extent_depth = 1U,
        .max_mip_levels = 12U,
        .max_array_layers = 256U,
        .sample_counts = 5U,
        .max_resource_size = UINT64_C(0x100000000),
    };
    uint8_t image_format_properties_wire[
        BVB_VULKAN_IMAGE_FORMAT_PROPERTIES_SIZE];
    CHECK(bvb_protocol_encode_vulkan_image_format_properties(
              image_format_properties_wire, &image_format_properties) == 0);
    struct bvb_vulkan_image_format_properties image_format_properties_decoded;
    CHECK(bvb_protocol_decode_vulkan_image_format_properties(
              image_format_properties_wire,
              &image_format_properties_decoded) == 0);
    CHECK(image_format_properties_decoded.vulkan_result == 0);
    CHECK(image_format_properties_decoded.max_extent_width == 4096U);
    CHECK(image_format_properties_decoded.max_array_layers == 256U);
    CHECK(image_format_properties_decoded.max_resource_size ==
          UINT64_C(0x100000000));

    const struct bvb_vulkan_external_buffer_query external_buffer_query = {
        .physical_device_id = physical_devices.ids[0],
        .flags = 2U,
        .usage = 20U,
        .handle_type = 1U,
    };
    uint8_t external_buffer_query_wire[
        BVB_VULKAN_EXTERNAL_BUFFER_QUERY_SIZE];
    CHECK(bvb_protocol_encode_vulkan_external_buffer_query(
              external_buffer_query_wire, &external_buffer_query) == 0);
    struct bvb_vulkan_external_buffer_query external_buffer_query_decoded;
    CHECK(bvb_protocol_decode_vulkan_external_buffer_query(
              external_buffer_query_wire,
              &external_buffer_query_decoded) == 0);
    CHECK(external_buffer_query_decoded.physical_device_id ==
          physical_devices.ids[0]);
    CHECK(external_buffer_query_decoded.flags == 2U);
    CHECK(external_buffer_query_decoded.usage == 20U);
    CHECK(external_buffer_query_decoded.handle_type == 1U);
    external_buffer_query_wire[20] = 1U;
    CHECK(bvb_protocol_decode_vulkan_external_buffer_query(
              external_buffer_query_wire,
              &external_buffer_query_decoded) == -EPROTO);
    external_buffer_query_wire[20] = 0U;

    const struct bvb_vulkan_external_buffer_properties
        external_buffer_properties = {
            .external_memory_features = 6U,
            .export_from_imported_handle_types = 1U,
            .compatible_handle_types = 3U,
        };
    uint8_t external_buffer_properties_wire[
        BVB_VULKAN_EXTERNAL_BUFFER_PROPERTIES_SIZE];
    CHECK(bvb_protocol_encode_vulkan_external_buffer_properties(
              external_buffer_properties_wire,
              &external_buffer_properties) == 0);
    struct bvb_vulkan_external_buffer_properties
        external_buffer_properties_decoded;
    CHECK(bvb_protocol_decode_vulkan_external_buffer_properties(
              external_buffer_properties_wire,
              &external_buffer_properties_decoded) == 0);
    CHECK(external_buffer_properties_decoded.external_memory_features == 6U);
    CHECK(external_buffer_properties_decoded.compatible_handle_types == 3U);

    const struct bvb_vulkan_external_semaphore_query
        external_semaphore_query = {
            .physical_device_id = physical_devices.ids[0],
            .handle_type = 16U,
        };
    uint8_t external_semaphore_query_wire[
        BVB_VULKAN_EXTERNAL_SEMAPHORE_QUERY_SIZE];
    CHECK(bvb_protocol_encode_vulkan_external_semaphore_query(
              external_semaphore_query_wire,
              &external_semaphore_query) == 0);
    struct bvb_vulkan_external_semaphore_query
        external_semaphore_query_decoded;
    CHECK(bvb_protocol_decode_vulkan_external_semaphore_query(
              external_semaphore_query_wire,
              &external_semaphore_query_decoded) == 0);
    CHECK(external_semaphore_query_decoded.physical_device_id ==
          physical_devices.ids[0]);
    CHECK(external_semaphore_query_decoded.handle_type == 16U);
    external_semaphore_query_wire[12] = 1U;
    CHECK(bvb_protocol_decode_vulkan_external_semaphore_query(
              external_semaphore_query_wire,
              &external_semaphore_query_decoded) == -EPROTO);
    external_semaphore_query_wire[12] = 0U;

    const struct bvb_vulkan_external_semaphore_properties
        external_semaphore_properties = {
            .export_from_imported_handle_types = 16U,
            .compatible_handle_types = 48U,
            .external_semaphore_features = 3U,
        };
    uint8_t external_semaphore_properties_wire[
        BVB_VULKAN_EXTERNAL_SEMAPHORE_PROPERTIES_SIZE];
    CHECK(bvb_protocol_encode_vulkan_external_semaphore_properties(
              external_semaphore_properties_wire,
              &external_semaphore_properties) == 0);
    struct bvb_vulkan_external_semaphore_properties
        external_semaphore_properties_decoded;
    CHECK(bvb_protocol_decode_vulkan_external_semaphore_properties(
              external_semaphore_properties_wire,
              &external_semaphore_properties_decoded) == 0);
    CHECK(external_semaphore_properties_decoded.compatible_handle_types ==
          48U);
    CHECK(external_semaphore_properties_decoded
              .external_semaphore_features == 3U);

    const struct bvb_vulkan_core_features core_features = {
        .shader_draw_parameters = 1U,
        .buffer_device_address = 1U,
        .descriptor_indexing = 1U,
    };
    uint8_t core_features_wire[BVB_VULKAN_CORE_FEATURES_SIZE];
    CHECK(bvb_protocol_encode_vulkan_core_features(
              core_features_wire, &core_features) == 0);
    struct bvb_vulkan_core_features core_features_decoded;
    CHECK(bvb_protocol_decode_vulkan_core_features(
              core_features_wire, &core_features_decoded) == 0);
    CHECK(core_features_decoded.shader_draw_parameters == 1U);
    CHECK(core_features_decoded.buffer_device_address == 1U);
    CHECK(core_features_decoded.descriptor_indexing == 1U);
    core_features_wire[4] = 2U;
    CHECK(bvb_protocol_decode_vulkan_core_features(
              core_features_wire, &core_features_decoded) == -EPROTO);

    const struct bvb_vulkan_device_extension_query extension_query = {
        .physical_device_id = physical_devices.ids[0],
        .first = 15U,
        .max_count = 15U,
    };
    uint8_t extension_query_wire[BVB_VULKAN_DEVICE_EXTENSION_QUERY_SIZE];
    CHECK(bvb_protocol_encode_vulkan_device_extension_query(
              extension_query_wire, &extension_query) == 0);
    struct bvb_vulkan_device_extension_query extension_query_decoded;
    CHECK(bvb_protocol_decode_vulkan_device_extension_query(
              extension_query_wire, &extension_query_decoded) == 0);
    CHECK(extension_query_decoded.physical_device_id ==
          extension_query.physical_device_id);
    CHECK(extension_query_decoded.first == 15U);
    CHECK(extension_query_decoded.max_count == 15U);

    const struct bvb_vulkan_device_create_request device_create_request = {
        .physical_device_id = physical_devices.ids[0],
        .queue_family_index = 2U,
        .queue_count = 1U,
        .queue_priority_bits = UINT32_C(0x3f800000),
    };
    uint8_t device_create_request_wire[BVB_VULKAN_DEVICE_CREATE_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_device_create_request(
              device_create_request_wire, &device_create_request) == 0);
    struct bvb_vulkan_device_create_request device_create_request_decoded;
    CHECK(bvb_protocol_decode_vulkan_device_create_request(
              device_create_request_wire, &device_create_request_decoded) == 0);
    CHECK(device_create_request_decoded.physical_device_id ==
          physical_devices.ids[0]);
    CHECK(device_create_request_decoded.queue_family_index == 2U);
    CHECK(device_create_request_decoded.queue_count == 1U);
    CHECK(device_create_request_decoded.queue_priority_bits ==
          UINT32_C(0x3f800000));

    struct bvb_vulkan_device_create_extended_request extended_create = {
        .base = device_create_request,
    };
    extended_create.base.enabled_extension_count = 2U;
    memcpy(extended_create.enabled_extensions[0], "VK_KHR_swapchain",
           sizeof("VK_KHR_swapchain"));
    memcpy(extended_create.enabled_extensions[1], "VK_KHR_maintenance1",
           sizeof("VK_KHR_maintenance1"));
    uint8_t extended_create_wire[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t extended_create_length = 0U;
    CHECK(bvb_protocol_encode_vulkan_device_create_extended_request(
              extended_create_wire, &extended_create,
              &extended_create_length) == 0);
    CHECK(extended_create_length ==
          BVB_VULKAN_DEVICE_CREATE_REQUEST_SIZE +
              2U * BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE);
    struct bvb_vulkan_device_create_extended_request extended_decoded;
    CHECK(bvb_protocol_decode_vulkan_device_create_extended_request(
              extended_create_wire, extended_create_length,
              &extended_decoded) == 0);
    CHECK(extended_decoded.base.enabled_extension_count == 2U);
    CHECK(strcmp(extended_decoded.enabled_extensions[0],
                 "VK_KHR_swapchain") == 0);
    CHECK(strcmp(extended_decoded.enabled_extensions[1],
                 "VK_KHR_maintenance1") == 0);
    extended_create_wire[extended_create_length - 1U] = 1U;
    CHECK(bvb_protocol_decode_vulkan_device_create_extended_request(
              extended_create_wire, extended_create_length,
              &extended_decoded) == -EPROTO);

    const struct bvb_vulkan_device_create_response device_create_response = {
        .vulkan_result = 0,
        .device_id = UINT64_C(0x0300000000000001),
    };
    uint8_t device_create_response_wire[
        BVB_VULKAN_DEVICE_CREATE_RESPONSE_SIZE];
    CHECK(bvb_protocol_encode_vulkan_device_create_response(
              device_create_response_wire, &device_create_response) == 0);
    struct bvb_vulkan_device_create_response device_create_response_decoded;
    CHECK(bvb_protocol_decode_vulkan_device_create_response(
              device_create_response_wire, &device_create_response_decoded) ==
          0);
    CHECK(device_create_response_decoded.device_id ==
          device_create_response.device_id);

    uint8_t device_id_wire[BVB_VULKAN_DEVICE_ID_SIZE];
    CHECK(bvb_protocol_encode_vulkan_device_id(
              device_id_wire, device_create_response.device_id) == 0);
    uint64_t device_id_decoded = 0U;
    CHECK(bvb_protocol_decode_vulkan_device_id(
              device_id_wire, &device_id_decoded) == 0);
    CHECK(device_id_decoded == device_create_response.device_id);
    CHECK(bvb_protocol_decode_vulkan_physical_device_id(
              device_id_wire, &physical_id_decoded) == -EPROTO);

    const struct bvb_vulkan_device_queue_request queue_request = {
        .device_id = device_create_response.device_id,
        .queue_family_index = 2U,
        .queue_index = 0U,
    };
    uint8_t queue_request_wire[BVB_VULKAN_DEVICE_QUEUE_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_device_queue_request(
              queue_request_wire, &queue_request) == 0);
    struct bvb_vulkan_device_queue_request queue_request_decoded;
    CHECK(bvb_protocol_decode_vulkan_device_queue_request(
              queue_request_wire, &queue_request_decoded) == 0);
    CHECK(queue_request_decoded.device_id == queue_request.device_id);
    CHECK(queue_request_decoded.queue_family_index == 2U);
    CHECK(queue_request_decoded.queue_index == 0U);

    const uint64_t queue_id = UINT64_C(0x0400000000000001);
    uint8_t queue_id_wire[BVB_VULKAN_QUEUE_ID_SIZE];
    CHECK(bvb_protocol_encode_vulkan_queue_id(queue_id_wire, queue_id) == 0);
    uint64_t queue_id_decoded = 0U;
    CHECK(bvb_protocol_decode_vulkan_queue_id(
              queue_id_wire, &queue_id_decoded) == 0);
    CHECK(queue_id_decoded == queue_id);
    CHECK(bvb_protocol_decode_vulkan_device_id(
              queue_id_wire, &device_id_decoded) == -EPROTO);

    uint8_t vulkan_result_wire[BVB_VULKAN_RESULT_SIZE];
    CHECK(bvb_protocol_encode_vulkan_result(
              vulkan_result_wire, -4) == 0);
    int32_t vulkan_result_decoded = 0;
    CHECK(bvb_protocol_decode_vulkan_result(
              vulkan_result_wire, &vulkan_result_decoded) == 0);
    CHECK(vulkan_result_decoded == -4);

    const struct bvb_vulkan_command_pool_create_request pool_create = {
        .device_id = UINT64_C(0x0300000000000001),
        .flags = 3U,
        .queue_family_index = 2U,
    };
    uint8_t pool_create_wire[BVB_VULKAN_COMMAND_POOL_CREATE_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_command_pool_create_request(
              pool_create_wire, &pool_create) == 0);
    struct bvb_vulkan_command_pool_create_request pool_create_decoded;
    CHECK(bvb_protocol_decode_vulkan_command_pool_create_request(
              pool_create_wire, &pool_create_decoded) == 0);
    CHECK(pool_create_decoded.device_id == pool_create.device_id);
    CHECK(pool_create_decoded.flags == 3U);
    CHECK(pool_create_decoded.queue_family_index == 2U);

    const struct bvb_vulkan_command_pool_create_response pool_created = {
        .vulkan_result = 0,
        .command_pool_id = UINT64_C(0x0a00000000000001),
    };
    uint8_t pool_created_wire[BVB_VULKAN_COMMAND_POOL_CREATE_RESPONSE_SIZE];
    CHECK(bvb_protocol_encode_vulkan_command_pool_create_response(
              pool_created_wire, &pool_created) == 0);
    struct bvb_vulkan_command_pool_create_response pool_created_decoded;
    CHECK(bvb_protocol_decode_vulkan_command_pool_create_response(
              pool_created_wire, &pool_created_decoded) == 0);
    CHECK(pool_created_decoded.command_pool_id == pool_created.command_pool_id);

    const struct bvb_vulkan_command_pool_reset_request pool_reset = {
        .command_pool_id = pool_created.command_pool_id,
        .flags = 1U,
    };
    uint8_t pool_reset_wire[BVB_VULKAN_COMMAND_POOL_RESET_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_command_pool_reset_request(
              pool_reset_wire, &pool_reset) == 0);
    struct bvb_vulkan_command_pool_reset_request pool_reset_decoded;
    CHECK(bvb_protocol_decode_vulkan_command_pool_reset_request(
              pool_reset_wire, &pool_reset_decoded) == 0);
    CHECK(pool_reset_decoded.command_pool_id == pool_reset.command_pool_id);
    CHECK(pool_reset_decoded.flags == 1U);

    const struct bvb_vulkan_command_buffer_allocate_request buffer_allocate = {
        .command_pool_id = pool_created.command_pool_id,
        .level = 0U,
        .count = 1U,
    };
    uint8_t buffer_allocate_wire[
        BVB_VULKAN_COMMAND_BUFFER_ALLOCATE_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_command_buffer_allocate_request(
              buffer_allocate_wire, &buffer_allocate) == 0);
    struct bvb_vulkan_command_buffer_allocate_request buffer_allocate_decoded;
    CHECK(bvb_protocol_decode_vulkan_command_buffer_allocate_request(
              buffer_allocate_wire, &buffer_allocate_decoded) == 0);
    CHECK(buffer_allocate_decoded.command_pool_id ==
          buffer_allocate.command_pool_id);
    CHECK(buffer_allocate_decoded.level == 0U);
    CHECK(buffer_allocate_decoded.count == 1U);

    const struct bvb_vulkan_command_buffer_allocate_response buffer_allocated = {
        .vulkan_result = 0,
        .command_buffer_id = UINT64_C(0x0b00000000000001),
    };
    uint8_t buffer_allocated_wire[
        BVB_VULKAN_COMMAND_BUFFER_ALLOCATE_RESPONSE_SIZE];
    CHECK(bvb_protocol_encode_vulkan_command_buffer_allocate_response(
              buffer_allocated_wire, &buffer_allocated) == 0);
    struct bvb_vulkan_command_buffer_allocate_response buffer_allocated_decoded;
    CHECK(bvb_protocol_decode_vulkan_command_buffer_allocate_response(
              buffer_allocated_wire, &buffer_allocated_decoded) == 0);
    CHECK(buffer_allocated_decoded.command_buffer_id ==
          buffer_allocated.command_buffer_id);

    const struct bvb_vulkan_command_buffer_free_request buffer_free = {
        .command_pool_id = pool_created.command_pool_id,
        .command_buffer_id = buffer_allocated.command_buffer_id,
    };
    uint8_t buffer_free_wire[BVB_VULKAN_COMMAND_BUFFER_FREE_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_command_buffer_free_request(
              buffer_free_wire, &buffer_free) == 0);
    struct bvb_vulkan_command_buffer_free_request buffer_free_decoded;
    CHECK(bvb_protocol_decode_vulkan_command_buffer_free_request(
              buffer_free_wire, &buffer_free_decoded) == 0);
    CHECK(buffer_free_decoded.command_pool_id == buffer_free.command_pool_id);
    CHECK(buffer_free_decoded.command_buffer_id ==
          buffer_free.command_buffer_id);

    const struct bvb_vulkan_command_buffer_begin_request buffer_begin = {
        .command_buffer_id = buffer_allocated.command_buffer_id,
        .flags = 1U,
    };
    uint8_t buffer_begin_wire[BVB_VULKAN_COMMAND_BUFFER_BEGIN_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_command_buffer_begin_request(
              buffer_begin_wire, &buffer_begin) == 0);
    struct bvb_vulkan_command_buffer_begin_request buffer_begin_decoded;
    CHECK(bvb_protocol_decode_vulkan_command_buffer_begin_request(
              buffer_begin_wire, &buffer_begin_decoded) == 0);
    CHECK(buffer_begin_decoded.command_buffer_id ==
          buffer_begin.command_buffer_id);
    CHECK(buffer_begin_decoded.flags == 1U);

    uint8_t command_buffer_id_wire[BVB_VULKAN_COMMAND_BUFFER_ID_SIZE];
    CHECK(bvb_protocol_encode_vulkan_command_buffer_id(
              command_buffer_id_wire, buffer_allocated.command_buffer_id) == 0);
    uint64_t command_buffer_id_decoded = 0U;
    CHECK(bvb_protocol_decode_vulkan_command_buffer_id(
              command_buffer_id_wire, &command_buffer_id_decoded) == 0);
    CHECK(command_buffer_id_decoded == buffer_allocated.command_buffer_id);

    const struct bvb_vulkan_queue_submit_command_request command_submit = {
        .queue_id = queue_id,
        .command_buffer_id = buffer_allocated.command_buffer_id,
    };
    uint8_t command_submit_wire[
        BVB_VULKAN_QUEUE_SUBMIT_COMMAND_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_queue_submit_command_request(
              command_submit_wire, &command_submit) == 0);
    struct bvb_vulkan_queue_submit_command_request command_submit_decoded;
    CHECK(bvb_protocol_decode_vulkan_queue_submit_command_request(
              command_submit_wire, &command_submit_decoded) == 0);
    CHECK(command_submit_decoded.queue_id == command_submit.queue_id);
    CHECK(command_submit_decoded.command_buffer_id ==
          command_submit.command_buffer_id);

    const struct bvb_vulkan_buffer_create_request buffer_create = {
        .device_id = UINT64_C(0x0300000000000001),
        .size = 4096U,
        .usage = 2U,
    };
    uint8_t buffer_create_wire[BVB_VULKAN_BUFFER_CREATE_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_buffer_create_request(
              buffer_create_wire, &buffer_create) == 0);
    struct bvb_vulkan_buffer_create_request buffer_create_decoded;
    CHECK(bvb_protocol_decode_vulkan_buffer_create_request(
              buffer_create_wire, &buffer_create_decoded) == 0);
    CHECK(buffer_create_decoded.size == 4096U);
    CHECK(buffer_create_decoded.usage == 2U);
    const struct bvb_vulkan_object_create_response buffer_created = {
        .vulkan_result = 0,
        .object_id = UINT64_C(0x1300000000000001),
    };
    uint8_t object_created_wire[BVB_VULKAN_OBJECT_CREATE_RESPONSE_SIZE];
    CHECK(bvb_protocol_encode_vulkan_object_create_response(
              object_created_wire, &buffer_created, 19U) == 0);
    struct bvb_vulkan_object_create_response object_created_decoded;
    CHECK(bvb_protocol_decode_vulkan_object_create_response(
              object_created_wire, &object_created_decoded, 19U) == 0);
    CHECK(object_created_decoded.object_id == buffer_created.object_id);
    const struct bvb_vulkan_buffer_requirements buffer_requirements = {
        .size = 4096U,
        .alignment = 64U,
        .memory_type_bits = 3U,
    };
    uint8_t buffer_requirements_wire[BVB_VULKAN_BUFFER_REQUIREMENTS_SIZE];
    CHECK(bvb_protocol_encode_vulkan_buffer_requirements(
              buffer_requirements_wire, &buffer_requirements) == 0);
    struct bvb_vulkan_buffer_requirements buffer_requirements_decoded;
    CHECK(bvb_protocol_decode_vulkan_buffer_requirements(
              buffer_requirements_wire, &buffer_requirements_decoded) == 0);
    CHECK(buffer_requirements_decoded.alignment == 64U);
    const struct bvb_vulkan_memory_allocate_request memory_allocate = {
        .device_id = buffer_create.device_id,
        .allocation_size = 4096U,
        .memory_type_index = 1U,
    };
    uint8_t memory_allocate_wire[BVB_VULKAN_MEMORY_ALLOCATE_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_memory_allocate_request(
              memory_allocate_wire, &memory_allocate) == 0);
    struct bvb_vulkan_memory_allocate_request memory_allocate_decoded;
    CHECK(bvb_protocol_decode_vulkan_memory_allocate_request(
              memory_allocate_wire, &memory_allocate_decoded) == 0);
    CHECK(memory_allocate_decoded.memory_type_index == 1U);
    const struct bvb_vulkan_buffer_bind_request buffer_bind = {
        .buffer_id = buffer_created.object_id,
        .memory_id = UINT64_C(0x0900000000000001),
    };
    uint8_t buffer_bind_wire[BVB_VULKAN_BUFFER_BIND_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_buffer_bind_request(
              buffer_bind_wire, &buffer_bind) == 0);
    struct bvb_vulkan_buffer_bind_request buffer_bind_decoded;
    CHECK(bvb_protocol_decode_vulkan_buffer_bind_request(
              buffer_bind_wire, &buffer_bind_decoded) == 0);
    CHECK(buffer_bind_decoded.memory_id == buffer_bind.memory_id);
    const struct bvb_vulkan_command_buffer_fill_request buffer_fill = {
        .command_buffer_id = buffer_allocated.command_buffer_id,
        .buffer_id = buffer_created.object_id,
        .size = 4096U,
        .data = UINT32_C(0xa5c3f00d),
    };
    uint8_t buffer_fill_wire[BVB_VULKAN_COMMAND_BUFFER_FILL_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_command_buffer_fill_request(
              buffer_fill_wire, &buffer_fill) == 0);
    struct bvb_vulkan_command_buffer_fill_request buffer_fill_decoded;
    CHECK(bvb_protocol_decode_vulkan_command_buffer_fill_request(
              buffer_fill_wire, &buffer_fill_decoded) == 0);
    CHECK(buffer_fill_decoded.data == UINT32_C(0xa5c3f00d));
    const struct bvb_vulkan_memory_verify_fill_request verify_fill = {
        .memory_id = buffer_bind.memory_id,
        .size = 4096U,
        .expected_word = UINT32_C(0xa5c3f00d),
    };
    uint8_t verify_fill_wire[BVB_VULKAN_MEMORY_VERIFY_FILL_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_memory_verify_fill_request(
              verify_fill_wire, &verify_fill) == 0);
    struct bvb_vulkan_memory_verify_fill_request verify_fill_decoded;
    CHECK(bvb_protocol_decode_vulkan_memory_verify_fill_request(
              verify_fill_wire, &verify_fill_decoded) == 0);
    CHECK(verify_fill_decoded.expected_word == UINT32_C(0xa5c3f00d));

    const struct bvb_vulkan_fence_create_request fence_create = {
        .device_id = UINT64_C(0x0300000000000001),
        .flags = 1U,
    };
    uint8_t fence_create_wire[BVB_VULKAN_FENCE_CREATE_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_fence_create_request(
              fence_create_wire, &fence_create) == 0);
    struct bvb_vulkan_fence_create_request fence_create_decoded;
    CHECK(bvb_protocol_decode_vulkan_fence_create_request(
              fence_create_wire, &fence_create_decoded) == 0);
    CHECK(fence_create_decoded.flags == 1U);
    const struct bvb_vulkan_fence_wait_request fence_wait = {
        .fence_id = UINT64_C(0x1200000000000001),
        .timeout = UINT64_C(123456789),
        .wait_all = 1U,
    };
    uint8_t fence_wait_wire[BVB_VULKAN_FENCE_WAIT_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_fence_wait_request(
              fence_wait_wire, &fence_wait) == 0);
    struct bvb_vulkan_fence_wait_request fence_wait_decoded;
    CHECK(bvb_protocol_decode_vulkan_fence_wait_request(
              fence_wait_wire, &fence_wait_decoded) == 0);
    CHECK(fence_wait_decoded.timeout == UINT64_C(123456789));
    CHECK(fence_wait_decoded.wait_all == 1U);
    const struct bvb_vulkan_queue_submit_command_fence_request fenced_submit = {
        .queue_id = UINT64_C(0x0400000000000001),
        .command_buffer_id = UINT64_C(0x0b00000000000001),
        .fence_id = UINT64_C(0x1200000000000001),
    };
    uint8_t fenced_submit_wire[
        BVB_VULKAN_QUEUE_SUBMIT_COMMAND_FENCE_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_queue_submit_command_fence_request(
              fenced_submit_wire, &fenced_submit) == 0);
    struct bvb_vulkan_queue_submit_command_fence_request fenced_submit_decoded;
    CHECK(bvb_protocol_decode_vulkan_queue_submit_command_fence_request(
              fenced_submit_wire, &fenced_submit_decoded) == 0);
    CHECK(fenced_submit_decoded.fence_id == fenced_submit.fence_id);

    uint8_t memory_bytes[BVB_VULKAN_MEMORY_IO_MAX_BYTES];
    for (size_t index = 0U; index < sizeof(memory_bytes); ++index) {
        memory_bytes[index] = (uint8_t)(index ^ (index >> 8));
    }
    const struct bvb_vulkan_memory_io_request memory_io = {
        .memory_id = buffer_bind.memory_id,
        .offset = 12U,
        .length = sizeof(memory_bytes),
    };
    uint8_t memory_io_wire[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t memory_io_length = 0U;
    CHECK(bvb_protocol_encode_vulkan_memory_write_request(
              memory_io_wire, &memory_io, memory_bytes,
              &memory_io_length) == 0);
    CHECK(memory_io_length == BVB_PROTOCOL_MAX_PAYLOAD);
    struct bvb_vulkan_memory_io_request memory_io_decoded;
    const uint8_t *memory_data_decoded = NULL;
    CHECK(bvb_protocol_decode_vulkan_memory_write_request(
              memory_io_wire, memory_io_length, &memory_io_decoded,
              &memory_data_decoded) == 0);
    CHECK(memory_io_decoded.offset == memory_io.offset);
    CHECK(memcmp(memory_data_decoded, memory_bytes,
                 sizeof(memory_bytes)) == 0);
    memory_io_wire[20] = 1U;
    CHECK(bvb_protocol_decode_vulkan_memory_write_request(
              memory_io_wire, memory_io_length, &memory_io_decoded,
              &memory_data_decoded) == -EPROTO);
    memory_io_wire[20] = 0U;
    CHECK(bvb_protocol_decode_vulkan_memory_write_request(
              memory_io_wire, memory_io_length - 1U, &memory_io_decoded,
              &memory_data_decoded) == -EPROTO);
    CHECK(bvb_protocol_encode_vulkan_memory_read_request(
              memory_io_wire, &memory_io) == 0);
    CHECK(bvb_protocol_decode_vulkan_memory_read_request(
              memory_io_wire, &memory_io_decoded) == 0);
    CHECK(memory_io_decoded.length == sizeof(memory_bytes));
    const struct bvb_vulkan_memory_io_response memory_io_response = {
        .vulkan_result = 0,
        .length = sizeof(memory_bytes),
    };
    CHECK(bvb_protocol_encode_vulkan_memory_io_response(
              memory_io_wire, &memory_io_response, memory_bytes,
              &memory_io_length) == 0);
    CHECK(memory_io_length ==
          BVB_VULKAN_MEMORY_IO_RESPONSE_PREFIX_SIZE + sizeof(memory_bytes));
    struct bvb_vulkan_memory_io_response memory_io_response_decoded;
    CHECK(bvb_protocol_decode_vulkan_memory_io_response(
              memory_io_wire, memory_io_length, &memory_io_response_decoded,
              &memory_data_decoded) == 0);
    CHECK(memory_io_response_decoded.length == sizeof(memory_bytes));
    CHECK(memcmp(memory_data_decoded, memory_bytes,
                 sizeof(memory_bytes)) == 0);

    const struct bvb_shared_batch_setup shared_setup = {
        .region_bytes = 4096U,
        .generation = UINT64_C(0x1122334455667788),
    };
    uint8_t shared_setup_wire[BVB_SHARED_BATCH_SETUP_SIZE];
    CHECK(bvb_protocol_encode_shared_batch_setup(shared_setup_wire,
                                                 &shared_setup) == 0);
    CHECK(bvb_wire_get_u32(shared_setup_wire) == 4096U);
    struct bvb_shared_batch_setup shared_setup_decoded;
    CHECK(bvb_protocol_decode_shared_batch_setup(shared_setup_wire,
                                                 &shared_setup_decoded) == 0);
    CHECK(shared_setup_decoded.region_bytes == 4096U);
    CHECK(shared_setup_decoded.generation ==
          UINT64_C(0x1122334455667788));
    shared_setup_wire[4] = 1U;
    CHECK(bvb_protocol_decode_shared_batch_setup(shared_setup_wire,
                                                 &shared_setup_decoded) ==
          -EPROTO);
    shared_setup_wire[4] = 0U;

    const struct bvb_shared_batch_execute shared_execute = {
        .generation = UINT64_C(0x1122334455667788),
        .offset = 64U,
        .length = 104U,
        .sequence = 7U,
    };
    uint8_t shared_execute_wire[BVB_SHARED_BATCH_EXECUTE_SIZE];
    CHECK(bvb_protocol_encode_shared_batch_execute(shared_execute_wire,
                                                   &shared_execute) == 0);
    struct bvb_shared_batch_execute shared_execute_decoded;
    CHECK(bvb_protocol_decode_shared_batch_execute(shared_execute_wire,
                                                   &shared_execute_decoded) ==
          0);
    CHECK(shared_execute_decoded.generation == shared_execute.generation);
    CHECK(shared_execute_decoded.offset == 64U);
    CHECK(shared_execute_decoded.length == 104U);
    CHECK(shared_execute_decoded.sequence == 7U);

    struct bvb_visible_batch_setup visible_setup = {
        .shared = shared_setup,
    };
    struct bvb_visible_batch_execute visible_execute = {
        .shared = shared_execute,
    };
    for (size_t index = 0U; index < BVB_LIFECYCLE_TOKEN_SIZE; ++index) {
        visible_setup.token[index] = (uint8_t)(index + 1U);
        visible_execute.token[index] = (uint8_t)(index + 1U);
    }
    uint8_t visible_setup_wire[BVB_VISIBLE_BATCH_SETUP_SIZE];
    CHECK(bvb_protocol_encode_visible_batch_setup(visible_setup_wire,
                                                  &visible_setup) == 0);
    CHECK(memcmp(visible_setup_wire, visible_setup.token,
                 BVB_LIFECYCLE_TOKEN_SIZE) == 0);
    struct bvb_visible_batch_setup visible_setup_decoded;
    CHECK(bvb_protocol_decode_visible_batch_setup(visible_setup_wire,
                                                  &visible_setup_decoded) == 0);
    CHECK(visible_setup_decoded.shared.generation ==
          shared_setup.generation);
    CHECK(memcmp(visible_setup_decoded.token, visible_setup.token,
                 BVB_LIFECYCLE_TOKEN_SIZE) == 0);

    uint8_t visible_execute_wire[BVB_VISIBLE_BATCH_EXECUTE_SIZE];
    CHECK(bvb_protocol_encode_visible_batch_execute(visible_execute_wire,
                                                    &visible_execute) == 0);
    struct bvb_visible_batch_execute visible_execute_decoded;
    CHECK(bvb_protocol_decode_visible_batch_execute(
              visible_execute_wire, &visible_execute_decoded) == 0);
    CHECK(visible_execute_decoded.shared.offset == shared_execute.offset);
    CHECK(visible_execute_decoded.shared.length == shared_execute.length);
    CHECK(visible_execute_decoded.shared.sequence == shared_execute.sequence);
    memset(visible_setup.token, 0, sizeof(visible_setup.token));
    CHECK(bvb_protocol_encode_visible_batch_setup(visible_setup_wire,
                                                  &visible_setup) == -EINVAL);
    memset(visible_execute_wire, 0, BVB_LIFECYCLE_TOKEN_SIZE);
    CHECK(bvb_protocol_decode_visible_batch_execute(
              visible_execute_wire, &visible_execute_decoded) == -EPROTO);

    struct bvb_vulkan_caps caps;
    memset(&caps, 0, sizeof(caps));
    caps.loader_api_version = 0x00401000U;
    caps.instance_extension_count = 14;
    caps.physical_device_count = 1;
    caps.included_device_count = 1;
    caps.devices[0].api_version = 0x00401080U;
    caps.devices[0].driver_version = 0x81234567U;
    caps.devices[0].vendor_id = 0x5143U;
    caps.devices[0].device_id = 0x07030001U;
    caps.devices[0].device_type = 1;
    caps.devices[0].queue_family_count = 2;
    caps.devices[0].memory_heap_count = 2;
    caps.devices[0].device_local_bytes = UINT64_C(7914782720);
    (void)snprintf(caps.devices[0].name, sizeof(caps.devices[0].name),
                   "Adreno (TM) 730");

    uint8_t caps_wire[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t caps_length = 0;
    CHECK(bvb_protocol_encode_vulkan_caps(caps_wire, &caps, &caps_length) == 0);
    CHECK(caps_length == BVB_VULKAN_CAPS_PREFIX_SIZE +
                             BVB_VULKAN_CAPS_DEVICE_SIZE);
    CHECK(bvb_wire_get_u32(caps_wire + 8) == 1U);
    CHECK(bvb_wire_get_u32(caps_wire + 16 + 8) == 0x5143U);
    struct bvb_vulkan_caps caps_decoded;
    CHECK(bvb_protocol_decode_vulkan_caps(caps_wire, caps_length,
                                          &caps_decoded) == 0);
    CHECK(caps_decoded.loader_api_version == caps.loader_api_version);
    CHECK(caps_decoded.devices[0].device_local_bytes ==
          caps.devices[0].device_local_bytes);
    CHECK(strcmp(caps_decoded.devices[0].name, "Adreno (TM) 730") == 0);
    caps_wire[16 + 28] = 1U;
    CHECK(bvb_protocol_decode_vulkan_caps(caps_wire, caps_length,
                                          &caps_decoded) == -EPROTO);

    const struct bvb_vulkan_selftest_result selftest = {
        .instance_extension_count = 14,
        .device_extension_count = 90,
        .instance_extension_flags = UINT64_C(0x3b),
        .device_extension_flags = UINT64_C(0xff),
        .queue_family_index = 0,
        .queue_flags = 27,
        .memory_type_index = 6,
        .memory_property_flags = 15,
        .buffer_bytes = 4096,
        .fill_word = UINT32_C(0xa5c3f00d),
        .mismatched_words = 0,
        .submit_wait_elapsed_ns = UINT64_C(3298542),
    };
    uint8_t selftest_wire[BVB_VULKAN_SELFTEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_selftest(selftest_wire, &selftest) == 0);
    struct bvb_vulkan_selftest_result selftest_decoded;
    CHECK(bvb_protocol_decode_vulkan_selftest(selftest_wire,
                                              &selftest_decoded) == 0);
    CHECK(selftest_decoded.device_extension_count == 90U);
    CHECK(selftest_decoded.device_extension_flags == UINT64_C(0xff));
    CHECK(selftest_decoded.fill_word == UINT32_C(0xa5c3f00d));
    CHECK(selftest_decoded.submit_wait_elapsed_ns == UINT64_C(3298542));
    selftest_wire[52] = 1U;
    CHECK(bvb_protocol_decode_vulkan_selftest(selftest_wire,
                                              &selftest_decoded) == -EPROTO);

    const char *token_hex =
        "00112233445566778899aabbccddeeff"
        "fedcba98765432100123456789abcdef";
    uint8_t token[BVB_LIFECYCLE_TOKEN_SIZE];
    CHECK(bvb_lifecycle_token_from_hex(token_hex, token) == 0);
    CHECK(token[0] == 0x00U);
    CHECK(token[15] == 0xffU);
    CHECK(token[16] == 0xfeU);
    CHECK(token[31] == 0xefU);
    CHECK(bvb_lifecycle_token_from_hex("short", token) == -EINVAL);

    struct bvb_lifecycle_record lifecycle = {
        .event = BVB_LIFECYCLE_EVENT_RENDERER_READY,
        .sequence = 7,
        .width = 2800,
        .height = 1752,
        .activity_pid = 12345,
        .monotonic_ns = UINT64_C(9876543210),
    };
    CHECK(bvb_lifecycle_token_from_hex(token_hex, lifecycle.token) == 0);
    uint8_t lifecycle_wire[BVB_LIFECYCLE_RECORD_SIZE];
    CHECK(bvb_lifecycle_encode_record(lifecycle_wire, &lifecycle) == 0);
    CHECK(bvb_wire_get_u32(lifecycle_wire) == BVB_LIFECYCLE_MAGIC);
    struct bvb_lifecycle_record lifecycle_decoded;
    CHECK(bvb_lifecycle_decode_record(lifecycle_wire, &lifecycle_decoded) == 0);
    CHECK(lifecycle_decoded.event == BVB_LIFECYCLE_EVENT_RENDERER_READY);
    CHECK(lifecycle_decoded.sequence == 7U);
    CHECK(lifecycle_decoded.width == 2800U);
    CHECK(lifecycle_decoded.height == 1752U);
    CHECK(lifecycle_decoded.activity_pid == 12345U);
    CHECK(lifecycle_decoded.monotonic_ns == UINT64_C(9876543210));
    CHECK(memcmp(lifecycle_decoded.token, lifecycle.token,
                 BVB_LIFECYCLE_TOKEN_SIZE) == 0);
    lifecycle_wire[0] ^= 1U;
    CHECK(bvb_lifecycle_decode_record(lifecycle_wire, &lifecycle_decoded) ==
          -EPROTO);

    const struct bvb_lifecycle_ack ack = {
        .sequence = 7,
        .status = -EACCES,
    };
    uint8_t ack_wire[BVB_LIFECYCLE_ACK_SIZE];
    CHECK(bvb_lifecycle_encode_ack(ack_wire, &ack) == 0);
    struct bvb_lifecycle_ack ack_decoded;
    CHECK(bvb_lifecycle_decode_ack(ack_wire, &ack_decoded) == 0);
    CHECK(ack_decoded.sequence == 7U);
    CHECK(ack_decoded.status == -EACCES);

    const struct bvb_activity_status activity_status = {
        .ingress_configured = 1,
        .authenticated_event_count = 7,
        .rejected_event_count = 1,
        .last_sequence = 7,
        .last_event = BVB_LIFECYCLE_EVENT_RENDERER_READY,
        .state_flags = BVB_ACTIVITY_CREATED | BVB_ACTIVITY_STARTED |
                       BVB_ACTIVITY_RESUMED | BVB_ACTIVITY_WINDOW_PRESENT |
                       BVB_ACTIVITY_RENDERER_READY | BVB_ACTIVITY_FOCUSED,
        .width = 2800,
        .height = 1752,
        .activity_pid = 12345,
        .last_event_monotonic_ns = UINT64_C(9876543210),
        .last_event_received_ns = UINT64_C(9876543999),
    };
    uint8_t activity_status_wire[BVB_ACTIVITY_STATUS_SIZE];
    CHECK(bvb_protocol_encode_activity_status(activity_status_wire,
                                              &activity_status) == 0);
    struct bvb_activity_status activity_status_decoded;
    CHECK(bvb_protocol_decode_activity_status(activity_status_wire,
                                              &activity_status_decoded) == 0);
    CHECK(activity_status_decoded.authenticated_event_count == 7U);
    CHECK(activity_status_decoded.rejected_event_count == 1U);
    CHECK(activity_status_decoded.state_flags == activity_status.state_flags);
    CHECK(activity_status_decoded.width == 2800U);
    CHECK(activity_status_decoded.last_event_received_ns ==
          UINT64_C(9876543999));

    puts("protocol: PASS");
    return EXIT_SUCCESS;
}
