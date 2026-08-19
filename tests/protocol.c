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
