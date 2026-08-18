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

    puts("protocol: PASS");
    return EXIT_SUCCESS;
}
