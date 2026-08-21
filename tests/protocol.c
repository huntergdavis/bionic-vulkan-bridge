#include <bvb/protocol.h>
#include <bvb/vulkan_descriptor_wire.h>
#include <bvb/vulkan_pipeline_wire.h>

#include <vulkan/vulkan.h>

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
    CHECK(BVB_OPCODE_VULKAN_IMAGE_CREATE == 90);
    CHECK(BVB_OPCODE_VULKAN_IMAGE_VIEW_DESTROY == 95);
    CHECK(BVB_OPCODE_VULKAN_IMAGE_REQUIREMENTS_2 == 96);
    CHECK(BVB_OPCODE_VULKAN_MEMORY_ALLOCATE_EXTENDED == 97);
    CHECK(BVB_OPCODE_VULKAN_DEVICE_BUFFER_REQUIREMENTS == 76);
    CHECK(BVB_OPCODE_VULKAN_BUFFER_REQUIREMENTS_2 == 78);
    CHECK(BVB_OPCODE_VULKAN_BUFFER_DEVICE_ADDRESS == 79);
    CHECK(BVB_OPCODE_VULKAN_SWAPCHAIN_ACQUIRE == 100);
    CHECK(BVB_OPCODE_VULKAN_SWAPCHAIN_PRESENT == 101);
    CHECK(BVB_OPCODE_VULKAN_COMMAND_BUFFER_IMAGE_BARRIER == 102);
    CHECK(BVB_OPCODE_VULKAN_COMMAND_BUFFER_CLEAR_COLOR_IMAGE == 103);
    CHECK(BVB_OPCODE_VULKAN_COMMAND_STREAM_SETUP == 104);
    CHECK(BVB_OPCODE_VULKAN_QUEUE_SUBMIT_2_STREAM == 105);
    CHECK(BVB_OPCODE_VULKAN_MEMORY_MIRROR_SETUP == 106);
    CHECK(BVB_OPCODE_VULKAN_MEMORY_MIRROR_FLUSH == 107);
    CHECK(BVB_OPCODE_VULKAN_MEMORY_MIRROR_INVALIDATE == 108);
    CHECK(BVB_OPCODE_VULKAN_MEMORY_MIRROR_UNMAP == 109);
    CHECK(BVB_OPCODE_VULKAN_DESCRIPTOR_UPDATE_TEMPLATE_CREATE == 110);
    CHECK(decoded.opcode ==
          BVB_OPCODE_VULKAN_DESCRIPTOR_UPDATE_TEMPLATE_CREATE);

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

    const struct bvb_vulkan_swapchain_prepare_request swapchain_request = {
        .device_id = UINT64_C(0x0300000000000007),
        .width = 1280U,
        .height = 720U,
        .format = 44U,
        .image_usage = 0x10U,
        .min_image_count = 3U,
        .generation = UINT64_C(0x123456789abcdef0),
    };
    uint8_t swapchain_request_wire[
        BVB_VULKAN_SWAPCHAIN_PREPARE_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_swapchain_prepare_request(
              swapchain_request_wire, &swapchain_request) == 0);
    struct bvb_vulkan_swapchain_prepare_request swapchain_request_decoded;
    CHECK(bvb_protocol_decode_vulkan_swapchain_prepare_request(
              swapchain_request_wire, &swapchain_request_decoded) == 0);
    CHECK(swapchain_request_decoded.device_id ==
          swapchain_request.device_id);
    CHECK(swapchain_request_decoded.min_image_count == 3U);
    CHECK(swapchain_request_decoded.generation ==
          swapchain_request.generation);

    const struct bvb_vulkan_swapchain_prepare_response swapchain_response = {
        .vulkan_result = 0,
        .image_count = 3U,
        .swapchain_id = UINT64_C(0x0600000000000004),
        .generation = swapchain_request.generation,
        .control_region_bytes = BVB_WSI_FRAME_RING_REGION_BYTES,
        .images = {
            {UINT64_C(0x070000000000000a), 65536U, 2U},
            {UINT64_C(0x070000000000000b), 65536U, 2U},
            {UINT64_C(0x070000000000000c), 65536U, 2U},
        },
    };
    uint8_t swapchain_response_wire[
        BVB_VULKAN_SWAPCHAIN_PREPARE_RESPONSE_SIZE];
    CHECK(bvb_protocol_encode_vulkan_swapchain_prepare_response(
              swapchain_response_wire, &swapchain_response) == 0);
    struct bvb_vulkan_swapchain_prepare_response swapchain_response_decoded;
    CHECK(bvb_protocol_decode_vulkan_swapchain_prepare_response(
              swapchain_response_wire, &swapchain_response_decoded) == 0);
    CHECK(swapchain_response_decoded.image_count == 3U);
    CHECK(swapchain_response_decoded.images[2].image_id ==
          swapchain_response.images[2].image_id);
    CHECK(swapchain_response_decoded.images[2].allocation_size == 65536U);
    bvb_wire_put_u32(swapchain_response_wire + 52, 1U);
    CHECK(bvb_protocol_decode_vulkan_swapchain_prepare_response(
              swapchain_response_wire, &swapchain_response_decoded) ==
          -EPROTO);

    const struct bvb_vulkan_swapchain_acquire_request acquire_request = {
        .device_id = UINT64_C(0x0300000000000007),
        .swapchain_id = UINT64_C(0x0600000000000004),
        .timeout_ns = UINT64_MAX,
        .semaphore_id = UINT64_C(0x1100000000000002),
    };
    uint8_t acquire_wire[BVB_VULKAN_SWAPCHAIN_ACQUIRE_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_swapchain_acquire_request(
              acquire_wire, &acquire_request) == 0);
    struct bvb_vulkan_swapchain_acquire_request acquire_decoded;
    CHECK(bvb_protocol_decode_vulkan_swapchain_acquire_request(
              acquire_wire, &acquire_decoded) == 0);
    CHECK(acquire_decoded.swapchain_id == acquire_request.swapchain_id);
    CHECK(acquire_decoded.semaphore_id == acquire_request.semaphore_id);
    const struct bvb_vulkan_swapchain_acquire_response acquire_response = {
        .vulkan_result = 0,
        .image_index = 2U,
    };
    uint8_t acquire_response_wire[
        BVB_VULKAN_SWAPCHAIN_ACQUIRE_RESPONSE_SIZE];
    CHECK(bvb_protocol_encode_vulkan_swapchain_acquire_response(
              acquire_response_wire, &acquire_response) == 0);
    struct bvb_vulkan_swapchain_acquire_response acquire_response_decoded;
    CHECK(bvb_protocol_decode_vulkan_swapchain_acquire_response(
              acquire_response_wire, &acquire_response_decoded) == 0);
    CHECK(acquire_response_decoded.image_index == 2U);

    const struct bvb_vulkan_swapchain_present_request present_request = {
        .queue_id = UINT64_C(0x0400000000000001),
        .swapchain_id = UINT64_C(0x0600000000000004),
        .image_index = 2U,
        .wait_semaphore_count = 2U,
        .wait_semaphore_ids = {UINT64_C(0x1100000000000002),
                               UINT64_C(0x1100000000000003)},
    };
    uint8_t present_wire[BVB_VULKAN_SWAPCHAIN_PRESENT_MAX_SIZE];
    uint32_t present_wire_length = 0U;
    CHECK(bvb_protocol_encode_vulkan_swapchain_present_request(
              present_wire, &present_request, &present_wire_length) == 0);
    CHECK(present_wire_length ==
          BVB_VULKAN_SWAPCHAIN_PRESENT_PREFIX_SIZE + 2U * sizeof(uint64_t));
    struct bvb_vulkan_swapchain_present_request present_decoded;
    CHECK(bvb_protocol_decode_vulkan_swapchain_present_request(
              present_wire, present_wire_length, &present_decoded) == 0);
    CHECK(present_decoded.image_index == 2U);
    CHECK(present_decoded.wait_semaphore_ids[1] ==
          present_request.wait_semaphore_ids[1]);
    const struct bvb_vulkan_swapchain_present_response present_response = {
        .vulkan_result = 0,
        .sequence = 19U,
    };
    uint8_t present_response_wire[
        BVB_VULKAN_SWAPCHAIN_PRESENT_RESPONSE_SIZE];
    CHECK(bvb_protocol_encode_vulkan_swapchain_present_response(
              present_response_wire, &present_response) == 0);
    struct bvb_vulkan_swapchain_present_response present_response_decoded;
    CHECK(bvb_protocol_decode_vulkan_swapchain_present_response(
              present_response_wire, &present_response_decoded) == 0);
    CHECK(present_response_decoded.sequence == 19U);

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
        .descriptor_binding_sampled_image_update_after_bind = 1U,
        .descriptor_binding_update_unused_while_pending = 1U,
        .descriptor_binding_partially_bound = 1U,
        .host_query_reset = 1U,
        .runtime_descriptor_array = 1U,
        .sampler_mirror_clamp_to_edge = 1U,
        .scalar_block_layout = 1U,
        .timeline_semaphore = 1U,
        .uniform_buffer_standard_layout = 1U,
        .vulkan_memory_model = 1U,
        .compute_full_subgroups = 1U,
        .dynamic_rendering = 1U,
        .maintenance4 = 1U,
        .shader_demote_to_helper_invocation = 1U,
        .shader_zero_initialize_workgroup_memory = 1U,
        .subgroup_size_control = 1U,
        .synchronization2 = 1U,
        .depth_clip_enable = 1U,
        .robust_buffer_access2 = 1U,
        .null_descriptor = 1U,
        .maintenance5 = 1U,
        .maintenance6 = 1U,
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
    CHECK(core_features_decoded
              .descriptor_binding_sampled_image_update_after_bind == 1U);
    CHECK(core_features_decoded
              .descriptor_binding_update_unused_while_pending == 1U);
    CHECK(core_features_decoded.descriptor_binding_partially_bound == 1U);
    CHECK(core_features_decoded.host_query_reset == 1U);
    CHECK(core_features_decoded.runtime_descriptor_array == 1U);
    CHECK(core_features_decoded.sampler_mirror_clamp_to_edge == 1U);
    CHECK(core_features_decoded.scalar_block_layout == 1U);
    CHECK(core_features_decoded.timeline_semaphore == 1U);
    CHECK(core_features_decoded.uniform_buffer_standard_layout == 1U);
    CHECK(core_features_decoded.vulkan_memory_model == 1U);
    CHECK(core_features_decoded.compute_full_subgroups == 1U);
    CHECK(core_features_decoded.dynamic_rendering == 1U);
    CHECK(core_features_decoded.maintenance4 == 1U);
    CHECK(core_features_decoded.shader_demote_to_helper_invocation == 1U);
    CHECK(core_features_decoded
              .shader_zero_initialize_workgroup_memory == 1U);
    CHECK(core_features_decoded.subgroup_size_control == 1U);
    CHECK(core_features_decoded.synchronization2 == 1U);
    CHECK(core_features_decoded.depth_clip_enable == 1U);
    CHECK(core_features_decoded.robust_buffer_access2 == 1U);
    CHECK(core_features_decoded.null_descriptor == 1U);
    CHECK(core_features_decoded.maintenance5 == 1U);
    CHECK(core_features_decoded.maintenance6 == 1U);
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

    struct bvb_vulkan_device_create_packed_request packed_create = {
        .physical_device_id = physical_devices.ids[0],
        .queue_create_info_count = 2U,
        .queue_priority_count = 2U,
        .enabled_extension_count = 58U,
        .enabled_feature_structs =
            BVB_VULKAN_DEVICE_FEATURE_STRUCT_MASK,
        .enabled_features = {
            .shader_draw_parameters = 1U,
            .timeline_semaphore = 1U,
            .dynamic_rendering = 1U,
            .depth_clip_enable = 1U,
            .null_descriptor = 1U,
            .maintenance5 = 1U,
            .maintenance6 = 1U,
        },
        .enabled_base_features = {
            .values = {[0] = 1U, [4] = 1U, [19] = 1U},
        },
        .queue_create_infos = {
            {.queue_family_index = 0U, .queue_count = 1U,
             .first_priority = 0U},
            {.queue_family_index = 1U, .queue_count = 1U,
             .first_priority = 1U},
        },
        .queue_priority_bits = {
            UINT32_C(0x3f400000), UINT32_C(0x3e800000),
        },
    };
    for (uint32_t index = 0U;
         index < packed_create.enabled_extension_count; ++index) {
        CHECK(snprintf(packed_create.enabled_extensions[index],
                       BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE,
                       "VK_BVB_scale_extension_%02u", index) > 0);
    }
    uint8_t packed_create_wire[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t packed_create_length = 0U;
    CHECK(bvb_protocol_encode_vulkan_device_create_packed_request(
              packed_create_wire, &packed_create,
              &packed_create_length) == 0);
    CHECK(packed_create_length < BVB_PROTOCOL_MAX_PAYLOAD);
    struct bvb_vulkan_device_create_packed_request packed_decoded;
    CHECK(bvb_protocol_decode_vulkan_device_create_packed_request(
              packed_create_wire, packed_create_length,
              &packed_decoded) == 0);
    CHECK(packed_decoded.queue_create_info_count == 2U);
    CHECK(packed_decoded.queue_priority_count == 2U);
    CHECK(packed_decoded.enabled_extension_count == 58U);
    CHECK(packed_decoded.enabled_feature_structs ==
          BVB_VULKAN_DEVICE_FEATURE_STRUCT_MASK);
    CHECK(packed_decoded.enabled_features.shader_draw_parameters == 1U);
    CHECK(packed_decoded.enabled_features.timeline_semaphore == 1U);
    CHECK(packed_decoded.enabled_features.dynamic_rendering == 1U);
    CHECK(packed_decoded.enabled_features.depth_clip_enable == 1U);
    CHECK(packed_decoded.enabled_features.null_descriptor == 1U);
    CHECK(packed_decoded.enabled_features.maintenance5 == 1U);
    CHECK(packed_decoded.enabled_features.maintenance6 == 1U);
    CHECK(packed_decoded.enabled_base_features.values[0] == 1U);
    CHECK(packed_decoded.enabled_base_features.values[4] == 1U);
    CHECK(packed_decoded.enabled_base_features.values[19] == 1U);
    CHECK(packed_decoded.queue_create_infos[1].queue_family_index == 1U);
    CHECK(packed_decoded.queue_create_infos[1].first_priority == 1U);
    CHECK(packed_decoded.queue_priority_bits[1] == UINT32_C(0x3e800000));
    CHECK(strcmp(packed_decoded.enabled_extensions[57],
                 "VK_BVB_scale_extension_57") == 0);
    CHECK(bvb_protocol_decode_vulkan_device_create_packed_request(
              packed_create_wire, packed_create_length - 1U,
              &packed_decoded) == -EPROTO);
    bvb_wire_put_u32(
        packed_create_wire + BVB_VULKAN_DEVICE_CREATE_PACKED_PREFIX_SIZE,
        2U);
    CHECK(bvb_protocol_decode_vulkan_device_create_packed_request(
              packed_create_wire, packed_create_length,
              &packed_decoded) == -EPROTO);
    bvb_wire_put_u32(
        packed_create_wire + BVB_VULKAN_DEVICE_CREATE_PACKED_PREFIX_SIZE,
        1U);
    bvb_wire_put_u32(
        packed_create_wire + BVB_VULKAN_DEVICE_CREATE_PACKED_PREFIX_SIZE +
            BVB_VULKAN_CORE_FEATURES_SIZE,
        2U);
    CHECK(bvb_protocol_decode_vulkan_device_create_packed_request(
              packed_create_wire, packed_create_length,
              &packed_decoded) == -EPROTO);
    bvb_wire_put_u32(
        packed_create_wire + BVB_VULKAN_DEVICE_CREATE_PACKED_PREFIX_SIZE +
            BVB_VULKAN_CORE_FEATURES_SIZE,
        1U);
    bvb_wire_put_u32(
        packed_create_wire + BVB_VULKAN_DEVICE_CREATE_PACKED_PREFIX_SIZE +
            BVB_VULKAN_CORE_FEATURES_SIZE +
            BVB_VULKAN_BASE_FEATURES_SIZE +
            BVB_VULKAN_DEVICE_QUEUE_CREATE_INFO_SIZE + 12U,
        2U);
    CHECK(bvb_protocol_decode_vulkan_device_create_packed_request(
              packed_create_wire, packed_create_length,
              &packed_decoded) == -EPROTO);

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
    const struct bvb_vulkan_device_buffer_requirements_request
        device_buffer_requirements_request = {
            .device_id = buffer_create.device_id,
            .size = 65536U,
            .flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharing_mode = VK_SHARING_MODE_CONCURRENT,
            .queue_family_index_count = 2U,
            .queue_family_indices = {1U, 3U},
        };
    uint8_t device_buffer_requirements_request_wire[
        BVB_VULKAN_DEVICE_BUFFER_REQUIREMENTS_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_device_buffer_requirements_request(
              device_buffer_requirements_request_wire,
              &device_buffer_requirements_request) == 0);
    struct bvb_vulkan_device_buffer_requirements_request
        device_buffer_requirements_request_decoded;
    CHECK(bvb_protocol_decode_vulkan_device_buffer_requirements_request(
              device_buffer_requirements_request_wire,
              &device_buffer_requirements_request_decoded) == 0);
    CHECK(device_buffer_requirements_request_decoded.size == 65536U);
    CHECK(device_buffer_requirements_request_decoded.queue_family_indices[1] ==
          3U);
    device_buffer_requirements_request_wire[40] = 1U;
    CHECK(bvb_protocol_decode_vulkan_device_buffer_requirements_request(
              device_buffer_requirements_request_wire,
              &device_buffer_requirements_request_decoded) == -EPROTO);
    device_buffer_requirements_request_wire[40] = 0U;
    device_buffer_requirements_request_wire[36] = 1U;
    CHECK(bvb_protocol_decode_vulkan_device_buffer_requirements_request(
              device_buffer_requirements_request_wire,
              &device_buffer_requirements_request_decoded) == -EPROTO);
    device_buffer_requirements_request_wire[36] = 3U;
    const struct bvb_vulkan_device_buffer_requirements_response
        device_buffer_requirements_response = {
            .memory = {
                .size = 65792U,
                .alignment = 256U,
                .memory_type_bits = 5U,
            },
            .prefers_dedicated_allocation = 0U,
            .requires_dedicated_allocation = 1U,
        };
    uint8_t device_buffer_requirements_response_wire[
        BVB_VULKAN_DEVICE_BUFFER_REQUIREMENTS_RESPONSE_SIZE];
    CHECK(bvb_protocol_encode_vulkan_device_buffer_requirements_response(
              device_buffer_requirements_response_wire,
              &device_buffer_requirements_response) == 0);
    struct bvb_vulkan_device_buffer_requirements_response
        device_buffer_requirements_response_decoded;
    CHECK(bvb_protocol_decode_vulkan_device_buffer_requirements_response(
              device_buffer_requirements_response_wire,
              &device_buffer_requirements_response_decoded) == 0);
    CHECK(device_buffer_requirements_response_decoded.memory.size == 65792U);
    CHECK(device_buffer_requirements_response_decoded
              .requires_dedicated_allocation == 1U);
    device_buffer_requirements_response_wire[24] = 2U;
    CHECK(bvb_protocol_decode_vulkan_device_buffer_requirements_response(
              device_buffer_requirements_response_wire,
              &device_buffer_requirements_response_decoded) == -EPROTO);
    const struct bvb_vulkan_buffer_requirements_2_request
        buffer_requirements_2_request = {
            .buffer_id = buffer_created.object_id,
            .pnext_flags =
                BVB_VULKAN_BUFFER_REQUIREMENTS_2_PNEXT_DEDICATED,
        };
    uint8_t buffer_requirements_2_request_wire[
        BVB_VULKAN_BUFFER_REQUIREMENTS_2_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_buffer_requirements_2_request(
              buffer_requirements_2_request_wire,
              &buffer_requirements_2_request) == 0);
    struct bvb_vulkan_buffer_requirements_2_request
        buffer_requirements_2_request_decoded;
    CHECK(bvb_protocol_decode_vulkan_buffer_requirements_2_request(
              buffer_requirements_2_request_wire,
              &buffer_requirements_2_request_decoded) == 0);
    CHECK(buffer_requirements_2_request_decoded.buffer_id ==
          buffer_created.object_id);
    buffer_requirements_2_request_wire[12] = 1U;
    CHECK(bvb_protocol_decode_vulkan_buffer_requirements_2_request(
              buffer_requirements_2_request_wire,
              &buffer_requirements_2_request_decoded) == -EPROTO);
    buffer_requirements_2_request_wire[12] = 0U;
    bvb_wire_put_u32(buffer_requirements_2_request_wire + 8,
                     UINT32_C(0x80000000));
    CHECK(bvb_protocol_decode_vulkan_buffer_requirements_2_request(
              buffer_requirements_2_request_wire,
              &buffer_requirements_2_request_decoded) == -EPROTO);
    const struct bvb_vulkan_buffer_requirements_2_response
        buffer_requirements_2_response = {
            .size = 4096U,
            .alignment = 256U,
            .memory_type_bits = 1U,
            .pnext_flags =
                BVB_VULKAN_BUFFER_REQUIREMENTS_2_PNEXT_DEDICATED,
            .prefers_dedicated = 1U,
        };
    uint8_t buffer_requirements_2_response_wire[
        BVB_VULKAN_BUFFER_REQUIREMENTS_2_RESPONSE_SIZE];
    CHECK(bvb_protocol_encode_vulkan_buffer_requirements_2_response(
              buffer_requirements_2_response_wire,
              &buffer_requirements_2_response) == 0);
    struct bvb_vulkan_buffer_requirements_2_response
        buffer_requirements_2_response_decoded;
    CHECK(bvb_protocol_decode_vulkan_buffer_requirements_2_response(
              buffer_requirements_2_response_wire,
              &buffer_requirements_2_response_decoded) == 0);
    CHECK(buffer_requirements_2_response_decoded.prefers_dedicated == 1U);
    bvb_wire_put_u32(buffer_requirements_2_response_wire + 24, 2U);
    CHECK(bvb_protocol_decode_vulkan_buffer_requirements_2_response(
              buffer_requirements_2_response_wire,
              &buffer_requirements_2_response_decoded) == -EPROTO);
    CHECK(bvb_protocol_encode_vulkan_buffer_requirements_2_response(
              buffer_requirements_2_response_wire,
              &buffer_requirements_2_response) == 0);
    bvb_wire_put_u32(buffer_requirements_2_response_wire + 20, 0U);
    CHECK(bvb_protocol_decode_vulkan_buffer_requirements_2_response(
              buffer_requirements_2_response_wire,
              &buffer_requirements_2_response_decoded) == -EPROTO);
    bvb_wire_put_u32(buffer_requirements_2_response_wire + 20,
                     UINT32_C(0x80000000));
    bvb_wire_put_u32(buffer_requirements_2_response_wire + 24, 0U);
    CHECK(bvb_protocol_decode_vulkan_buffer_requirements_2_response(
              buffer_requirements_2_response_wire,
              &buffer_requirements_2_response_decoded) == -EPROTO);
    const struct bvb_vulkan_buffer_device_address_request address_request = {
        .buffer_id = buffer_created.object_id,
    };
    uint8_t address_request_wire[
        BVB_VULKAN_BUFFER_DEVICE_ADDRESS_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_buffer_device_address_request(
              address_request_wire, &address_request) == 0);
    struct bvb_vulkan_buffer_device_address_request address_request_decoded;
    CHECK(bvb_protocol_decode_vulkan_buffer_device_address_request(
              address_request_wire, &address_request_decoded) == 0);
    CHECK(address_request_decoded.buffer_id == buffer_created.object_id);
    bvb_wire_put_u64(address_request_wire, buffer_create.device_id);
    CHECK(bvb_protocol_decode_vulkan_buffer_device_address_request(
              address_request_wire, &address_request_decoded) == -EPROTO);
    const struct bvb_vulkan_buffer_device_address_response address_response = {
        .device_address = UINT64_C(0x123456780000),
    };
    uint8_t address_response_wire[
        BVB_VULKAN_BUFFER_DEVICE_ADDRESS_RESPONSE_SIZE];
    CHECK(bvb_protocol_encode_vulkan_buffer_device_address_response(
              address_response_wire, &address_response) == 0);
    struct bvb_vulkan_buffer_device_address_response address_decoded;
    CHECK(bvb_protocol_decode_vulkan_buffer_device_address_response(
              address_response_wire, &address_decoded) == 0);
    CHECK(address_decoded.device_address == UINT64_C(0x123456780000));
    memset(address_response_wire, 0, sizeof(address_response_wire));
    CHECK(bvb_protocol_decode_vulkan_buffer_device_address_response(
              address_response_wire, &address_decoded) == -EPROTO);
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
    struct bvb_vulkan_memory_allocate_extended_request memory_extended = {
        .device_id = buffer_create.device_id,
        .allocation_size = UINT64_C(19623936),
        .dedicated_image_id = UINT64_C(0x0700000000000001),
        .memory_type_index = 0U,
        .pnext_flags = BVB_VULKAN_MEMORY_ALLOCATE_PNEXT_DEDICATED_IMAGE |
                       BVB_VULKAN_MEMORY_ALLOCATE_PNEXT_FLAGS,
        .allocation_flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    uint8_t memory_extended_wire[
        BVB_VULKAN_MEMORY_ALLOCATE_EXTENDED_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_memory_allocate_extended_request(
              memory_extended_wire, &memory_extended) == 0);
    struct bvb_vulkan_memory_allocate_extended_request
        memory_extended_decoded;
    CHECK(bvb_protocol_decode_vulkan_memory_allocate_extended_request(
              memory_extended_wire, &memory_extended_decoded) == 0);
    CHECK(memory_extended_decoded.allocation_size == UINT64_C(19623936));
    CHECK(memory_extended_decoded.dedicated_image_id ==
          UINT64_C(0x0700000000000001));
    bvb_wire_put_u32(memory_extended_wire + 28, UINT32_C(0x80000000));
    CHECK(bvb_protocol_decode_vulkan_memory_allocate_extended_request(
              memory_extended_wire, &memory_extended_decoded) == -EPROTO);
    memory_extended.allocation_size =
        (uint64_t)BVB_VULKAN_MAX_MEMORY_ALLOCATION_SIZE + 1U;
    CHECK(bvb_protocol_encode_vulkan_memory_allocate_extended_request(
              memory_extended_wire, &memory_extended) == -EINVAL);
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
    const struct bvb_vulkan_image_create_request image_create = {
        .device_id = buffer_create.device_id,
        .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
        .image_type = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent_width = 64U,
        .extent_height = 64U,
        .extent_depth = 1U,
        .mip_levels = 1U,
        .array_layers = 1U,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharing_mode = VK_SHARING_MODE_EXCLUSIVE,
        .initial_layout = VK_IMAGE_LAYOUT_UNDEFINED,
        .pnext_flags = BVB_VULKAN_IMAGE_CREATE_PNEXT_FORMAT_LIST |
                       BVB_VULKAN_IMAGE_CREATE_PNEXT_STENCIL_USAGE,
        .view_format_count = 1U,
        .stencil_usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .view_formats = {VK_FORMAT_R8G8B8A8_UNORM},
    };
    uint8_t image_create_wire[BVB_VULKAN_IMAGE_CREATE_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_image_create_request(
              image_create_wire, &image_create) == 0);
    CHECK(bvb_wire_get_u32(image_create_wire + 76) == 0U);
    struct bvb_vulkan_image_create_request image_create_decoded;
    CHECK(bvb_protocol_decode_vulkan_image_create_request(
              image_create_wire, &image_create_decoded) == 0);
    CHECK(image_create_decoded.extent_width == 64U);
    CHECK(image_create_decoded.view_format_count == 1U);
    CHECK(image_create_decoded.view_formats[0] ==
          VK_FORMAT_R8G8B8A8_UNORM);
    image_create_wire[76] = 1U;
    CHECK(bvb_protocol_decode_vulkan_image_create_request(
              image_create_wire, &image_create_decoded) == -EPROTO);
    image_create_wire[76] = 0U;
    bvb_wire_put_u32(image_create_wire + 68,
                     BVB_VULKAN_IMAGE_MAX_VIEW_FORMATS + 1U);
    CHECK(bvb_protocol_decode_vulkan_image_create_request(
              image_create_wire, &image_create_decoded) == -EPROTO);
    CHECK(bvb_protocol_encode_vulkan_image_create_request(
              image_create_wire, &image_create) == 0);
    const struct bvb_vulkan_object_create_response image_created = {
        .vulkan_result = VK_SUCCESS,
        .object_id = UINT64_C(0x0700000000000001),
    };
    CHECK(bvb_protocol_encode_vulkan_object_create_response(
              object_created_wire, &image_created, 7U) == 0);
    const struct bvb_vulkan_image_requirements image_requirements = {
        .size = 16384U,
        .alignment = 4096U,
        .memory_type_bits = 1U,
    };
    uint8_t image_requirements_wire[BVB_VULKAN_IMAGE_REQUIREMENTS_SIZE];
    CHECK(bvb_protocol_encode_vulkan_image_requirements(
              image_requirements_wire, &image_requirements) == 0);
    struct bvb_vulkan_image_requirements image_requirements_decoded;
    CHECK(bvb_protocol_decode_vulkan_image_requirements(
              image_requirements_wire, &image_requirements_decoded) == 0);
    CHECK(image_requirements_decoded.size == 16384U);
    const struct bvb_vulkan_image_requirements_2_request requirements_2 = {
        .image_id = image_created.object_id,
        .pnext_flags =
            BVB_VULKAN_IMAGE_REQUIREMENTS_2_PNEXT_DEDICATED,
    };
    uint8_t requirements_2_request_wire[
        BVB_VULKAN_IMAGE_REQUIREMENTS_2_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_image_requirements_2_request(
              requirements_2_request_wire, &requirements_2) == 0);
    struct bvb_vulkan_image_requirements_2_request requirements_2_decoded;
    CHECK(bvb_protocol_decode_vulkan_image_requirements_2_request(
              requirements_2_request_wire, &requirements_2_decoded) == 0);
    CHECK(requirements_2_decoded.image_id == image_created.object_id);
    requirements_2_request_wire[12] = 1U;
    CHECK(bvb_protocol_decode_vulkan_image_requirements_2_request(
              requirements_2_request_wire, &requirements_2_decoded) ==
          -EPROTO);
    const struct bvb_vulkan_image_requirements_2_response
        requirements_2_response = {
            .size = UINT64_C(19623936),
            .alignment = 4096U,
            .memory_type_bits = 1U,
            .pnext_flags =
                BVB_VULKAN_IMAGE_REQUIREMENTS_2_PNEXT_DEDICATED,
            .prefers_dedicated = 1U,
            .requires_dedicated = 1U,
        };
    uint8_t requirements_2_response_wire[
        BVB_VULKAN_IMAGE_REQUIREMENTS_2_RESPONSE_SIZE];
    CHECK(bvb_protocol_encode_vulkan_image_requirements_2_response(
              requirements_2_response_wire, &requirements_2_response) == 0);
    struct bvb_vulkan_image_requirements_2_response
        requirements_2_response_decoded;
    CHECK(bvb_protocol_decode_vulkan_image_requirements_2_response(
              requirements_2_response_wire,
              &requirements_2_response_decoded) == 0);
    CHECK(requirements_2_response_decoded.requires_dedicated == 1U);
    bvb_wire_put_u32(requirements_2_response_wire + 24, 2U);
    CHECK(bvb_protocol_decode_vulkan_image_requirements_2_response(
              requirements_2_response_wire,
              &requirements_2_response_decoded) == -EPROTO);
    const struct bvb_vulkan_image_bind_request image_bind = {
        .image_id = image_created.object_id,
        .memory_id = buffer_bind.memory_id,
        .offset = 4096U,
    };
    uint8_t image_bind_wire[BVB_VULKAN_IMAGE_BIND_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_image_bind_request(
              image_bind_wire, &image_bind) == 0);
    struct bvb_vulkan_image_bind_request image_bind_decoded;
    CHECK(bvb_protocol_decode_vulkan_image_bind_request(
              image_bind_wire, &image_bind_decoded) == 0);
    CHECK(image_bind_decoded.offset == 4096U);
    const struct bvb_vulkan_image_view_create_request image_view_create = {
        .device_id = image_create.device_id,
        .image_id = image_created.object_id,
        .view_type = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .component_r = VK_COMPONENT_SWIZZLE_IDENTITY,
        .component_g = VK_COMPONENT_SWIZZLE_IDENTITY,
        .component_b = VK_COMPONENT_SWIZZLE_IDENTITY,
        .component_a = VK_COMPONENT_SWIZZLE_IDENTITY,
        .aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT,
        .level_count = 1U,
        .layer_count = 1U,
        .pnext_flags = BVB_VULKAN_IMAGE_VIEW_CREATE_PNEXT_USAGE,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
    };
    uint8_t image_view_create_wire[BVB_VULKAN_IMAGE_VIEW_CREATE_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_image_view_create_request(
              image_view_create_wire, &image_view_create) == 0);
    struct bvb_vulkan_image_view_create_request image_view_create_decoded;
    CHECK(bvb_protocol_decode_vulkan_image_view_create_request(
              image_view_create_wire, &image_view_create_decoded) == 0);
    CHECK(image_view_create_decoded.image_id == image_created.object_id);
    bvb_wire_put_u32(image_view_create_wire + 64, UINT32_C(0x80000000));
    CHECK(bvb_protocol_decode_vulkan_image_view_create_request(
              image_view_create_wire, &image_view_create_decoded) == -EPROTO);
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
    const struct bvb_vulkan_command_buffer_clear_color_image_request
        clear_color_image = {
            .command_buffer_id = buffer_allocated.command_buffer_id,
            .image_id = image_created.object_id,
        };
    uint8_t clear_color_image_wire[
        BVB_VULKAN_COMMAND_BUFFER_CLEAR_COLOR_IMAGE_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_command_buffer_clear_color_image_request(
              clear_color_image_wire, &clear_color_image) == 0);
    struct bvb_vulkan_command_buffer_clear_color_image_request
        clear_color_image_decoded;
    CHECK(bvb_protocol_decode_vulkan_command_buffer_clear_color_image_request(
              clear_color_image_wire, &clear_color_image_decoded) == 0);
    CHECK(clear_color_image_decoded.command_buffer_id ==
          buffer_allocated.command_buffer_id);
    CHECK(clear_color_image_decoded.image_id == image_created.object_id);
    bvb_wire_put_u64(clear_color_image_wire + 8U,
                     UINT64_C(0x1400000000000001));
    CHECK(bvb_protocol_decode_vulkan_command_buffer_clear_color_image_request(
              clear_color_image_wire, &clear_color_image_decoded) ==
          -EPROTO);

    const struct bvb_vulkan_command_buffer_image_barrier_request
        init_image_barriers = {
            .command_buffer_id = buffer_allocated.command_buffer_id,
            .image_count = 2U,
            .image_ids = {
                image_created.object_id,
                UINT64_C(0x0700000000000002),
            },
        };
    uint8_t init_image_barriers_wire[
        BVB_VULKAN_COMMAND_BUFFER_IMAGE_BARRIER_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_command_buffer_image_barrier_request(
              init_image_barriers_wire, &init_image_barriers) == 0);
    struct bvb_vulkan_command_buffer_image_barrier_request
        init_image_barriers_decoded;
    CHECK(bvb_protocol_decode_vulkan_command_buffer_image_barrier_request(
              init_image_barriers_wire, &init_image_barriers_decoded) == 0);
    CHECK(init_image_barriers_decoded.command_buffer_id ==
          buffer_allocated.command_buffer_id);
    CHECK(init_image_barriers_decoded.image_count == 2U);
    CHECK(init_image_barriers_decoded.image_ids[0] ==
          image_created.object_id);
    CHECK(init_image_barriers_decoded.image_ids[1] ==
          UINT64_C(0x0700000000000002));
    CHECK(init_image_barriers_decoded.image_ids[2] == 0U);
    bvb_wire_put_u64(init_image_barriers_wire + 32U,
                     UINT64_C(0x0700000000000003));
    CHECK(bvb_protocol_decode_vulkan_command_buffer_image_barrier_request(
              init_image_barriers_wire, &init_image_barriers_decoded) ==
          -EPROTO);
    bvb_wire_put_u64(init_image_barriers_wire + 32U, 0U);
    bvb_wire_put_u64(init_image_barriers_wire + 24U,
                     image_created.object_id);
    CHECK(bvb_protocol_decode_vulkan_command_buffer_image_barrier_request(
              init_image_barriers_wire, &init_image_barriers_decoded) ==
          -EPROTO);
    bvb_wire_put_u64(init_image_barriers_wire + 24U,
                     UINT64_C(0x0700000000000002));
    bvb_wire_put_u32(init_image_barriers_wire + 12U, 1U);
    CHECK(bvb_protocol_decode_vulkan_command_buffer_image_barrier_request(
              init_image_barriers_wire, &init_image_barriers_decoded) ==
          -EPROTO);
    bvb_wire_put_u32(init_image_barriers_wire + 12U, 0U);
    bvb_wire_put_u32(init_image_barriers_wire + 8U,
                     BVB_VULKAN_INIT_IMAGE_MAX_BARRIERS + 1U);
    CHECK(bvb_protocol_decode_vulkan_command_buffer_image_barrier_request(
              init_image_barriers_wire, &init_image_barriers_decoded) ==
          -EPROTO);
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

    const struct bvb_vulkan_semaphore_create_request semaphore_create = {
        .device_id = UINT64_C(0x0300000000000001),
        .initial_value = UINT64_C(7),
        .semaphore_type = 1U,
    };
    uint8_t semaphore_create_wire[
        BVB_VULKAN_SEMAPHORE_CREATE_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_semaphore_create_request(
              semaphore_create_wire, &semaphore_create) == 0);
    struct bvb_vulkan_semaphore_create_request semaphore_create_decoded;
    CHECK(bvb_protocol_decode_vulkan_semaphore_create_request(
              semaphore_create_wire, &semaphore_create_decoded) == 0);
    CHECK(semaphore_create_decoded.initial_value == UINT64_C(7));
    CHECK(semaphore_create_decoded.semaphore_type == 1U);
    const struct bvb_vulkan_semaphore_signal_request semaphore_signal = {
        .device_id = semaphore_create.device_id,
        .semaphore_id = UINT64_C(0x1100000000000001),
        .value = UINT64_C(11),
    };
    uint8_t semaphore_signal_wire[
        BVB_VULKAN_SEMAPHORE_SIGNAL_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_semaphore_signal_request(
              semaphore_signal_wire, &semaphore_signal) == 0);
    struct bvb_vulkan_semaphore_signal_request semaphore_signal_decoded;
    CHECK(bvb_protocol_decode_vulkan_semaphore_signal_request(
              semaphore_signal_wire, &semaphore_signal_decoded) == 0);
    CHECK(semaphore_signal_decoded.value == UINT64_C(11));
    const struct bvb_vulkan_semaphore_wait_request semaphore_wait = {
        .device_id = semaphore_create.device_id,
        .timeout = UINT64_C(1234),
        .flags = 1U,
        .semaphore_count = 2U,
        .semaphores = {
            {UINT64_C(0x1100000000000001), UINT64_C(11)},
            {UINT64_C(0x1100000000000002), UINT64_C(13)},
        },
    };
    uint8_t semaphore_wait_wire[BVB_VULKAN_SEMAPHORE_WAIT_MAX_SIZE];
    uint32_t semaphore_wait_length = 0U;
    CHECK(bvb_protocol_encode_vulkan_semaphore_wait_request(
              semaphore_wait_wire, &semaphore_wait,
              &semaphore_wait_length) == 0);
    CHECK(semaphore_wait_length ==
          BVB_VULKAN_SEMAPHORE_WAIT_PREFIX_SIZE +
              2U * BVB_VULKAN_SEMAPHORE_WAIT_RECORD_SIZE);
    struct bvb_vulkan_semaphore_wait_request semaphore_wait_decoded;
    CHECK(bvb_protocol_decode_vulkan_semaphore_wait_request(
              semaphore_wait_wire, semaphore_wait_length,
              &semaphore_wait_decoded) == 0);
    CHECK(semaphore_wait_decoded.semaphore_count == 2U);
    CHECK(semaphore_wait_decoded.semaphores[1].value == UINT64_C(13));
    struct bvb_vulkan_semaphore_counter_response counter_response = {
        .vulkan_result = 0,
        .value = UINT64_C(13),
    };
    uint8_t counter_wire[BVB_VULKAN_SEMAPHORE_COUNTER_RESPONSE_SIZE];
    CHECK(bvb_protocol_encode_vulkan_semaphore_counter_response(
              counter_wire, &counter_response) == 0);
    struct bvb_vulkan_semaphore_counter_response counter_decoded;
    CHECK(bvb_protocol_decode_vulkan_semaphore_counter_response(
              counter_wire, &counter_decoded) == 0);
    CHECK(counter_decoded.value == UINT64_C(13));
    const struct bvb_vulkan_queue_submit_2_request submit_2 = {
        .queue_id = UINT64_C(0x0400000000000001),
        .fence_id = UINT64_C(0x1200000000000001),
        .wait_count = 1U,
        .command_count = 1U,
        .signal_count = 1U,
        .waits = {{UINT64_C(0x1100000000000001), UINT64_C(11),
                   UINT64_C(0x10000), 0U}},
        .commands = {{
            .command_buffer_id = UINT64_C(0x0b00000000000001),
            .stream_generation = UINT64_C(0x8877665544332211),
            .stream_sequence = UINT64_C(17),
            .stream_offset = BVB_COMMAND_STREAM_SLOT_BYTES,
            .stream_length = 256U,
            .device_mask = 0U,
            .stream_flags = BVB_VULKAN_SUBMIT_2_COMMAND_SHARED_STREAM,
        }},
        .signals = {{UINT64_C(0x1100000000000002), UINT64_C(13),
                     UINT64_C(0x10000), 0U}},
    };
    const struct bvb_vulkan_queue_submit_2_request submit_2_strict = {
        .queue_id = submit_2.queue_id,
        .fence_id = submit_2.fence_id,
        .wait_count = 1U,
        .command_count = 1U,
        .signal_count = 1U,
        .waits = {{UINT64_C(0x1100000000000001), UINT64_C(11),
                   UINT64_C(0x10000), 0U}},
        .commands = {{
            .command_buffer_id = UINT64_C(0x0b00000000000001),
            .device_mask = 7U,
        }},
        .signals = {{UINT64_C(0x1100000000000002), UINT64_C(13),
                     UINT64_C(0x10000), 0U}},
    };
    uint8_t submit_2_wire[BVB_VULKAN_SUBMIT_2_MAX_SIZE];
    uint32_t submit_2_length = 0U;
    CHECK(bvb_protocol_encode_vulkan_queue_submit_2_request(
              submit_2_wire, &submit_2_strict, &submit_2_length) == 0);
    CHECK(submit_2_length == BVB_VULKAN_SUBMIT_2_PREFIX_SIZE +
          2U * BVB_VULKAN_SUBMIT_2_SEMAPHORE_RECORD_SIZE +
          BVB_VULKAN_SUBMIT_2_COMMAND_RECORD_SIZE);
    struct bvb_vulkan_queue_submit_2_request submit_2_decoded;
    CHECK(bvb_protocol_decode_vulkan_queue_submit_2_request(
              submit_2_wire, submit_2_length, &submit_2_decoded) == 0);
    CHECK(submit_2_decoded.command_count == 1U);
    CHECK(submit_2_decoded.waits[0].value == UINT64_C(11));
    CHECK(submit_2_decoded.signals[0].value == UINT64_C(13));
    CHECK(submit_2_decoded.commands[0].stream_generation == 0U);
    CHECK(submit_2_decoded.commands[0].stream_flags == 0U);
    CHECK(submit_2_decoded.commands[0].device_mask == 7U);
    const uint32_t strict_submit_command_offset =
        BVB_VULKAN_SUBMIT_2_PREFIX_SIZE +
        BVB_VULKAN_SUBMIT_2_SEMAPHORE_RECORD_SIZE;
    CHECK(bvb_wire_get_u32(
              submit_2_wire + strict_submit_command_offset + 8) == 7U);
    CHECK(bvb_wire_get_u32(
              submit_2_wire + strict_submit_command_offset + 12) == 0U);
    bvb_wire_put_u32(submit_2_wire + strict_submit_command_offset + 12, 1U);
    CHECK(bvb_protocol_decode_vulkan_queue_submit_2_request(
              submit_2_wire, submit_2_length, &submit_2_decoded) == -EPROTO);
    CHECK(bvb_protocol_encode_vulkan_queue_submit_2_request(
              submit_2_wire, &submit_2, &submit_2_length) == -EINVAL);

    uint8_t submit_2_stream_wire[BVB_VULKAN_SUBMIT_2_STREAM_MAX_SIZE];
    CHECK(bvb_protocol_encode_vulkan_queue_submit_2_stream_request(
              submit_2_stream_wire, &submit_2, &submit_2_length) == 0);
    CHECK(submit_2_length == BVB_VULKAN_SUBMIT_2_PREFIX_SIZE +
          2U * BVB_VULKAN_SUBMIT_2_SEMAPHORE_RECORD_SIZE +
          BVB_VULKAN_SUBMIT_2_STREAM_COMMAND_RECORD_SIZE);
    CHECK(bvb_protocol_decode_vulkan_queue_submit_2_stream_request(
              submit_2_stream_wire, submit_2_length, &submit_2_decoded) == 0);
    CHECK(submit_2_decoded.commands[0].stream_generation ==
          UINT64_C(0x8877665544332211));
    CHECK(submit_2_decoded.commands[0].stream_sequence == UINT64_C(17));
    CHECK(submit_2_decoded.commands[0].stream_offset ==
          BVB_COMMAND_STREAM_SLOT_BYTES);
    CHECK(submit_2_decoded.commands[0].stream_length == 256U);
    const uint32_t submit_command_offset =
        BVB_VULKAN_SUBMIT_2_PREFIX_SIZE +
        BVB_VULKAN_SUBMIT_2_SEMAPHORE_RECORD_SIZE;
    uint8_t corrupted_submit_2[BVB_VULKAN_SUBMIT_2_STREAM_MAX_SIZE];
    memcpy(corrupted_submit_2, submit_2_stream_wire, submit_2_length);
    bvb_wire_put_u64(corrupted_submit_2 + submit_command_offset + 8, 0U);
    CHECK(bvb_protocol_decode_vulkan_queue_submit_2_stream_request(
              corrupted_submit_2, submit_2_length, &submit_2_decoded) ==
          -EPROTO);
    memcpy(corrupted_submit_2, submit_2_stream_wire, submit_2_length);
    bvb_wire_put_u32(corrupted_submit_2 + submit_command_offset + 24, 1U);
    CHECK(bvb_protocol_decode_vulkan_queue_submit_2_stream_request(
              corrupted_submit_2, submit_2_length, &submit_2_decoded) ==
          -EPROTO);
    memcpy(corrupted_submit_2, submit_2_stream_wire, submit_2_length);
    bvb_wire_put_u32(corrupted_submit_2 + submit_command_offset + 36, 2U);
    CHECK(bvb_protocol_decode_vulkan_queue_submit_2_stream_request(
              corrupted_submit_2, submit_2_length, &submit_2_decoded) ==
          -EPROTO);

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

    const struct bvb_vulkan_memory_mirror_setup_request mirror_setup = {
        .device_id = UINT64_C(0x0300000000000001),
        .memory_id = UINT64_C(0x0900000000000001),
        .generation = UINT64_C(0x8877665544332211),
        .offset = 256U,
        .length = 8192U,
    };
    uint8_t mirror_setup_wire[BVB_VULKAN_MEMORY_MIRROR_SETUP_SIZE];
    CHECK(bvb_protocol_encode_vulkan_memory_mirror_setup_request(
              mirror_setup_wire, &mirror_setup) == 0);
    struct bvb_vulkan_memory_mirror_setup_request mirror_setup_decoded;
    CHECK(bvb_protocol_decode_vulkan_memory_mirror_setup_request(
              mirror_setup_wire, &mirror_setup_decoded) == 0);
    CHECK(mirror_setup_decoded.generation == mirror_setup.generation);
    CHECK(mirror_setup_decoded.offset == 256U);
    bvb_wire_put_u64(mirror_setup_wire + 16, 0U);
    CHECK(bvb_protocol_decode_vulkan_memory_mirror_setup_request(
              mirror_setup_wire, &mirror_setup_decoded) == -EPROTO);
    CHECK(bvb_protocol_encode_vulkan_memory_mirror_setup_request(
              mirror_setup_wire, &mirror_setup) == 0);
    bvb_wire_put_u64(mirror_setup_wire + 32,
                     (uint64_t)BVB_VULKAN_MAX_MEMORY_ALLOCATION_SIZE + 1U);
    CHECK(bvb_protocol_decode_vulkan_memory_mirror_setup_request(
              mirror_setup_wire, &mirror_setup_decoded) == -EPROTO);

    const struct bvb_vulkan_memory_mirror_range_request mirror_range = {
        .device_id = mirror_setup.device_id,
        .memory_id = mirror_setup.memory_id,
        .generation = mirror_setup.generation,
        .offset = 512U,
        .size = 4096U,
    };
    uint8_t mirror_range_wire[BVB_VULKAN_MEMORY_MIRROR_RANGE_SIZE];
    CHECK(bvb_protocol_encode_vulkan_memory_mirror_range_request(
              mirror_range_wire, &mirror_range) == 0);
    struct bvb_vulkan_memory_mirror_range_request mirror_range_decoded;
    CHECK(bvb_protocol_decode_vulkan_memory_mirror_range_request(
              mirror_range_wire, &mirror_range_decoded) == 0);
    CHECK(mirror_range_decoded.size == 4096U);
    mirror_range_wire[7] = 4U;
    CHECK(bvb_protocol_decode_vulkan_memory_mirror_range_request(
              mirror_range_wire, &mirror_range_decoded) == -EPROTO);

    const struct bvb_vulkan_memory_mirror_unmap_request mirror_unmap = {
        .device_id = mirror_setup.device_id,
        .memory_id = mirror_setup.memory_id,
        .generation = mirror_setup.generation,
    };
    uint8_t mirror_unmap_wire[BVB_VULKAN_MEMORY_MIRROR_UNMAP_SIZE];
    CHECK(bvb_protocol_encode_vulkan_memory_mirror_unmap_request(
              mirror_unmap_wire, &mirror_unmap) == 0);
    struct bvb_vulkan_memory_mirror_unmap_request mirror_unmap_decoded;
    CHECK(bvb_protocol_decode_vulkan_memory_mirror_unmap_request(
              mirror_unmap_wire, &mirror_unmap_decoded) == 0);
    CHECK(mirror_unmap_decoded.generation == mirror_unmap.generation);
    bvb_wire_put_u64(mirror_unmap_wire + 8,
                     UINT64_C(0x1300000000000001));
    CHECK(bvb_protocol_decode_vulkan_memory_mirror_unmap_request(
              mirror_unmap_wire, &mirror_unmap_decoded) == -EPROTO);

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

    const struct bvb_vulkan_descriptor_set_layout_create_request
        descriptor_layout_request = {
            .device_id = UINT64_C(0x0300000000000001),
            .flags = 2U,
            .binding_count = 1U,
            .has_binding_flags = 1U,
            .bindings = {{
                .binding = 3U,
                .descriptor_type = 0U,
                .descriptor_count = 4096U,
                .stage_flags = 31U,
                .binding_flags = 7U,
            }},
        };
    uint8_t descriptor_wire[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t descriptor_wire_length = 0U;
    CHECK(bvb_protocol_encode_vulkan_descriptor_set_layout_create_request(
              descriptor_wire, &descriptor_layout_request,
              &descriptor_wire_length) == 0);
    struct bvb_vulkan_descriptor_set_layout_create_request
        descriptor_layout_decoded;
    CHECK(bvb_protocol_decode_vulkan_descriptor_set_layout_create_request(
              descriptor_wire, descriptor_wire_length,
              &descriptor_layout_decoded) == 0);
    CHECK(descriptor_layout_decoded.bindings[0].descriptor_count == 4096U);
    CHECK(descriptor_layout_decoded.bindings[0].binding_flags == 7U);

    const struct bvb_vulkan_descriptor_pool_create_request
        descriptor_pool_request = {
            .device_id = UINT64_C(0x0300000000000001),
            .flags = 2U,
            .max_sets = 1U,
            .pool_size_count = 1U,
            .pool_sizes = {{.descriptor_type = 0U,
                            .descriptor_count = 4096U}},
        };
    CHECK(bvb_protocol_encode_vulkan_descriptor_pool_create_request(
              descriptor_wire, &descriptor_pool_request,
              &descriptor_wire_length) == 0);
    struct bvb_vulkan_descriptor_pool_create_request descriptor_pool_decoded;
    CHECK(bvb_protocol_decode_vulkan_descriptor_pool_create_request(
              descriptor_wire, descriptor_wire_length,
              &descriptor_pool_decoded) == 0);
    CHECK(descriptor_pool_decoded.pool_sizes[0].descriptor_count == 4096U);

    const struct bvb_vulkan_descriptor_set_allocate_request
        descriptor_allocate_request = {
            .descriptor_pool_id = UINT64_C(0x1500000000000001),
            .descriptor_set_count = 2U,
            .set_layout_ids = {UINT64_C(0x1400000000000001),
                               UINT64_C(0x1400000000000002)},
        };
    CHECK(bvb_protocol_encode_vulkan_descriptor_set_allocate_request(
              descriptor_wire, &descriptor_allocate_request,
              &descriptor_wire_length) == 0);
    struct bvb_vulkan_descriptor_set_allocate_request
        descriptor_allocate_decoded;
    CHECK(bvb_protocol_decode_vulkan_descriptor_set_allocate_request(
              descriptor_wire, descriptor_wire_length,
              &descriptor_allocate_decoded) == 0);
    CHECK(descriptor_allocate_decoded.descriptor_set_count == 2U);
    CHECK(descriptor_allocate_decoded.set_layout_ids[1] ==
          UINT64_C(0x1400000000000002));

    const struct bvb_vulkan_descriptor_set_allocate_response
        descriptor_allocate_response = {
            .vulkan_result = 0,
            .descriptor_set_count = 2U,
            .descriptor_set_ids = {UINT64_C(0x1600000000000001),
                                   UINT64_C(0x1600000000000002)},
        };
    CHECK(bvb_protocol_encode_vulkan_descriptor_set_allocate_response(
              descriptor_wire, &descriptor_allocate_response,
              &descriptor_wire_length) == 0);
    struct bvb_vulkan_descriptor_set_allocate_response
        descriptor_allocate_response_decoded;
    CHECK(bvb_protocol_decode_vulkan_descriptor_set_allocate_response(
              descriptor_wire, descriptor_wire_length,
              &descriptor_allocate_response_decoded) == 0);
    CHECK(descriptor_allocate_response_decoded.descriptor_set_ids[0] ==
          UINT64_C(0x1600000000000001));

    const struct bvb_vulkan_sampler_create_request sampler_request = {
        .device_id = UINT64_C(0x0300000000000001),
        .mag_filter = 1U,
        .min_filter = 0U,
        .mipmap_mode = 1U,
        .address_mode_v = 2U,
        .address_mode_w = 1U,
        .mip_lod_bias_bits = UINT32_C(0x3e800000),
        .anisotropy_enable = 1U,
        .max_anisotropy_bits = UINT32_C(0x41000000),
        .compare_enable = 1U,
        .compare_op = 3U,
        .max_lod_bits = UINT32_C(0x41400000),
        .border_color = 4U,
    };
    uint8_t sampler_wire[BVB_VULKAN_SAMPLER_CREATE_REQUEST_SIZE];
    CHECK(bvb_protocol_encode_vulkan_sampler_create_request(
              sampler_wire, &sampler_request) == 0);
    struct bvb_vulkan_sampler_create_request sampler_decoded;
    CHECK(bvb_protocol_decode_vulkan_sampler_create_request(
              sampler_wire, &sampler_decoded) == 0);
    CHECK(sampler_decoded.max_anisotropy_bits == UINT32_C(0x41000000));

    const struct bvb_vulkan_descriptor_update_request update_request = {
        .device_id = UINT64_C(0x0300000000000001),
        .write_count = 1U,
        .sampler_count = 2U,
        .writes = {{
            .descriptor_set_id = UINT64_C(0x1600000000000001),
            .dst_binding = 0U,
            .dst_array_element = 7U,
            .descriptor_count = 2U,
            .descriptor_type = 0U,
            .first_sampler = 0U,
        }},
        .sampler_ids = {UINT64_C(0x1700000000000001),
                        UINT64_C(0x1700000000000002)},
    };
    CHECK(bvb_protocol_encode_vulkan_descriptor_update_request(
              descriptor_wire, &update_request,
              &descriptor_wire_length) == 0);
    struct bvb_vulkan_descriptor_update_request update_decoded;
    CHECK(bvb_protocol_decode_vulkan_descriptor_update_request(
              descriptor_wire, descriptor_wire_length,
              &update_decoded) == 0);
    CHECK(update_decoded.writes[0].dst_array_element == 7U);
    CHECK(update_decoded.sampler_ids[1] == UINT64_C(0x1700000000000002));
    descriptor_wire[BVB_VULKAN_DESCRIPTOR_UPDATE_PREFIX_SIZE + 28U] = 1U;
    CHECK(bvb_protocol_decode_vulkan_descriptor_update_request(
              descriptor_wire, descriptor_wire_length,
              &update_decoded) == -EPROTO);

    const struct bvb_vulkan_descriptor_update_template_create_request
        template_request = {
            .device_id = UINT64_C(0x0300000000000001),
            .entry_count = 4U,
            .descriptor_set_layout_id = UINT64_C(0x1400000000000001),
            .entries = {
                {.dst_binding = 0U, .descriptor_count = 1U,
                 .descriptor_type = 7U, .offset = 0U, .stride = 24U},
                {.dst_binding = 1U, .descriptor_count = 1U,
                 .descriptor_type = 7U, .offset = 24U, .stride = 24U},
                {.dst_binding = 2U, .descriptor_count = 1U,
                 .descriptor_type = 4U, .offset = 48U, .stride = 24U},
                {.dst_binding = 3U, .descriptor_count = 1U,
                 .descriptor_type = 2U, .offset = 72U, .stride = 24U},
            },
        };
    CHECK(
        bvb_protocol_encode_vulkan_descriptor_update_template_create_request(
            descriptor_wire, &template_request,
            &descriptor_wire_length) == 0);
    CHECK(descriptor_wire_length ==
          BVB_VULKAN_DESCRIPTOR_UPDATE_TEMPLATE_PREFIX_SIZE +
              4U * BVB_VULKAN_DESCRIPTOR_UPDATE_TEMPLATE_ENTRY_SIZE);
    struct bvb_vulkan_descriptor_update_template_create_request
        template_decoded;
    CHECK(
        bvb_protocol_decode_vulkan_descriptor_update_template_create_request(
            descriptor_wire, descriptor_wire_length,
            &template_decoded) == 0);
    CHECK(template_decoded.entries[3].descriptor_type == 2U);
    CHECK(template_decoded.entries[3].offset == 72U);
    descriptor_wire[44U] = 1U;
    CHECK(
        bvb_protocol_decode_vulkan_descriptor_update_template_create_request(
            descriptor_wire, descriptor_wire_length,
            &template_decoded) == -EINVAL);

    const struct bvb_vulkan_pipeline_layout_create_request
        pipeline_layout_request = {
            .device_id = UINT64_C(0x0300000000000001),
            .flags = 2U,
            .set_layout_count = 3U,
            .push_constant_range_count = 1U,
            .set_layout_ids = {UINT64_C(0x1400000000000001),
                               UINT64_C(0x1400000000000002), 0U},
            .push_constant_ranges = {{
                .stage_flags = 25U,
                .offset = 0U,
                .size = 160U,
            }},
        };
    uint8_t pipeline_layout_wire[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t pipeline_layout_wire_length = 0U;
    CHECK(bvb_protocol_encode_vulkan_pipeline_layout_create_request(
              pipeline_layout_wire, &pipeline_layout_request,
              &pipeline_layout_wire_length) == 0);
    CHECK(pipeline_layout_wire_length ==
          BVB_VULKAN_PIPELINE_LAYOUT_CREATE_PREFIX_SIZE + 3U * 8U +
              BVB_VULKAN_PIPELINE_PUSH_CONSTANT_RANGE_SIZE);
    struct bvb_vulkan_pipeline_layout_create_request
        pipeline_layout_decoded;
    CHECK(bvb_protocol_decode_vulkan_pipeline_layout_create_request(
              pipeline_layout_wire, pipeline_layout_wire_length,
              &pipeline_layout_decoded) == 0);
    CHECK(pipeline_layout_decoded.set_layout_ids[2] == 0U);
    CHECK(pipeline_layout_decoded.push_constant_ranges[0].stage_flags == 25U);
    CHECK(pipeline_layout_decoded.push_constant_ranges[0].size == 160U);
    pipeline_layout_wire[20U] = 1U;
    CHECK(bvb_protocol_decode_vulkan_pipeline_layout_create_request(
              pipeline_layout_wire, pipeline_layout_wire_length,
              &pipeline_layout_decoded) == -EPROTO);
    pipeline_layout_wire[20U] = 0U;
    bvb_wire_put_u64(pipeline_layout_wire +
                         BVB_VULKAN_PIPELINE_LAYOUT_CREATE_PREFIX_SIZE,
                     UINT64_C(0x1700000000000001));
    CHECK(bvb_protocol_decode_vulkan_pipeline_layout_create_request(
              pipeline_layout_wire, pipeline_layout_wire_length,
              &pipeline_layout_decoded) == -EPROTO);

    const struct bvb_vulkan_graphics_pipeline_create_request
        graphics_pipeline_request = {
            .device_id = UINT64_C(0x0300000000000001),
            .pipeline_layout_id = UINT64_C(0x0f00000000000001),
            .flags_2 = UINT64_C(0x00000800),
            .library_flags = 2U,
            .shader_stage = 16U,
            .dynamic_state_count = 9U,
            .shader_word_count = 5U,
            .dynamic_states = {1000267006U, 1000267007U, 1000267008U,
                               1000267000U, 1000267001U, 1000267002U,
                               1000267005U, 1000267005U, 1000267004U},
            .shader_words = {UINT32_C(0x07230203), UINT32_C(0x00010600),
                             UINT32_C(0x0008000b), UINT32_C(0x00000006),
                             UINT32_C(0x00000000)},
        };
    uint8_t graphics_pipeline_wire[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t graphics_pipeline_wire_length = 0U;
    CHECK(bvb_protocol_encode_vulkan_graphics_pipeline_create_request(
              graphics_pipeline_wire, &graphics_pipeline_request,
              &graphics_pipeline_wire_length) == 0);
    CHECK(graphics_pipeline_wire_length ==
          BVB_VULKAN_GRAPHICS_PIPELINE_CREATE_PREFIX_SIZE + 14U * 4U);
    struct bvb_vulkan_graphics_pipeline_create_request
        graphics_pipeline_decoded;
    CHECK(bvb_protocol_decode_vulkan_graphics_pipeline_create_request(
              graphics_pipeline_wire, graphics_pipeline_wire_length,
              &graphics_pipeline_decoded) == 0);
    CHECK(graphics_pipeline_decoded.pipeline_layout_id ==
          UINT64_C(0x0f00000000000001));
    CHECK(graphics_pipeline_decoded.dynamic_states[7] == 1000267005U);
    CHECK(graphics_pipeline_decoded.shader_words[1] ==
          UINT32_C(0x00010600));
    graphics_pipeline_wire[40U] = 1U;
    CHECK(bvb_protocol_decode_vulkan_graphics_pipeline_create_request(
              graphics_pipeline_wire, graphics_pipeline_wire_length,
              &graphics_pipeline_decoded) == -EPROTO);
    graphics_pipeline_wire[40U] = 0U;
    bvb_wire_put_u64(graphics_pipeline_wire + 8U,
                     UINT64_C(0x1000000000000001));
    CHECK(bvb_protocol_decode_vulkan_graphics_pipeline_create_request(
              graphics_pipeline_wire, graphics_pipeline_wire_length,
              &graphics_pipeline_decoded) == -EPROTO);

    puts("protocol: PASS");
    return EXIT_SUCCESS;
}
