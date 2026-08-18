#ifndef BVB_PROTOCOL_H
#define BVB_PROTOCOL_H

#include <bvb/lifecycle.h>
#include <bvb/vulkan_caps.h>
#include <bvb/vulkan_selftest.h>

#include <stddef.h>
#include <stdint.h>

enum {
    BVB_PROTOCOL_MAGIC = 0x31425642,
    BVB_PROTOCOL_VERSION = 1,
    BVB_PROTOCOL_HEADER_SIZE = 24,
    BVB_PROTOCOL_MAX_PAYLOAD = 4096,
    BVB_PROTOCOL_REQUEST = 1,
    BVB_PROTOCOL_RESPONSE = 2,
    BVB_OPCODE_HELLO = 1,
    BVB_OPCODE_VULKAN_CAPS = 2,
    BVB_OPCODE_VULKAN_SELFTEST = 3,
    BVB_OPCODE_ACTIVITY_STATUS = 4,
    BVB_HELLO_REQUEST_SIZE = 8,
    BVB_HELLO_RESPONSE_SIZE = 16,
    BVB_VULKAN_CAPS_PREFIX_SIZE = 16,
    BVB_VULKAN_CAPS_DEVICE_SIZE = 296,
    BVB_VULKAN_SELFTEST_SIZE = 64,
    BVB_ACTIVITY_STATUS_SIZE = 56,
    BVB_SERVICE_BIONIC = 1U << 0,
    BVB_SERVICE_ANDROID_VULKAN_LOADER = 1U << 1,
    BVB_SERVICE_ACTIVITY_INGRESS = 1U << 2,
};

struct bvb_protocol_header {
    uint16_t version;
    uint16_t kind;
    uint16_t opcode;
    uint32_t request_id;
    uint32_t payload_length;
    int32_t status;
};

struct bvb_protocol_packet {
    struct bvb_protocol_header header;
    uint8_t payload[BVB_PROTOCOL_MAX_PAYLOAD];
};

struct bvb_hello_request {
    uint16_t minimum_version;
    uint16_t maximum_version;
    uint32_t client_flags;
};

struct bvb_hello_response {
    uint16_t negotiated_version;
    uint32_t service_flags;
    uint32_t pointer_bits;
    uint32_t page_size;
};

/*
 * The fixed-width wire format is explicitly little-endian; C structure layout
 * never crosses the libc/process boundary.
 *
 * Header (24 bytes): magic:u32, version:u16, kind:u16, opcode:u16,
 * reserved:u16, request_id:u32, payload_length:u32, status:i32.
 */
int bvb_protocol_encode_header(
    uint8_t output[BVB_PROTOCOL_HEADER_SIZE],
    const struct bvb_protocol_header *header);
int bvb_protocol_decode_header(
    const uint8_t input[BVB_PROTOCOL_HEADER_SIZE],
    struct bvb_protocol_header *header);

int bvb_protocol_encode_hello_request(
    uint8_t output[BVB_HELLO_REQUEST_SIZE],
    const struct bvb_hello_request *request);
int bvb_protocol_decode_hello_request(
    const uint8_t input[BVB_HELLO_REQUEST_SIZE],
    struct bvb_hello_request *request);
int bvb_protocol_encode_hello_response(
    uint8_t output[BVB_HELLO_RESPONSE_SIZE],
    const struct bvb_hello_response *response);
int bvb_protocol_decode_hello_response(
    const uint8_t input[BVB_HELLO_RESPONSE_SIZE],
    struct bvb_hello_response *response);
int bvb_protocol_encode_vulkan_caps(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_caps *caps,
    uint32_t *output_length);
int bvb_protocol_decode_vulkan_caps(
    const uint8_t *input,
    uint32_t input_length,
    struct bvb_vulkan_caps *caps);
int bvb_protocol_encode_vulkan_selftest(
    uint8_t output[BVB_VULKAN_SELFTEST_SIZE],
    const struct bvb_vulkan_selftest_result *result);
int bvb_protocol_decode_vulkan_selftest(
    const uint8_t input[BVB_VULKAN_SELFTEST_SIZE],
    struct bvb_vulkan_selftest_result *result);
int bvb_protocol_encode_activity_status(
    uint8_t output[BVB_ACTIVITY_STATUS_SIZE],
    const struct bvb_activity_status *status);
int bvb_protocol_decode_activity_status(
    const uint8_t input[BVB_ACTIVITY_STATUS_SIZE],
    struct bvb_activity_status *status);

void bvb_wire_put_u16(uint8_t *output, uint16_t value);
void bvb_wire_put_u32(uint8_t *output, uint32_t value);
void bvb_wire_put_i32(uint8_t *output, int32_t value);
void bvb_wire_put_u64(uint8_t *output, uint64_t value);
uint16_t bvb_wire_get_u16(const uint8_t *input);
uint32_t bvb_wire_get_u32(const uint8_t *input);
int32_t bvb_wire_get_i32(const uint8_t *input);
uint64_t bvb_wire_get_u64(const uint8_t *input);

#endif
