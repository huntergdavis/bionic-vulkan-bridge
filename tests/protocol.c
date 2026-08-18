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

    puts("protocol: PASS");
    return EXIT_SUCCESS;
}

