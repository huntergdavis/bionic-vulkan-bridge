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
    BVB_OPCODE_VULKAN_BATCH_SELFTEST = 5,
    BVB_OPCODE_SHARED_BATCH_SETUP = 6,
    BVB_OPCODE_SHARED_BATCH_EXECUTE = 7,
    BVB_OPCODE_VISIBLE_BATCH_SETUP = 8,
    BVB_OPCODE_VISIBLE_BATCH_EXECUTE = 9,
    BVB_OPCODE_VISIBLE_BATCH_INLINE = 10,
    BVB_OPCODE_VULKAN_GLOBAL_INFO = 11,
    BVB_OPCODE_VULKAN_INSTANCE_CREATE = 12,
    BVB_OPCODE_VULKAN_INSTANCE_DESTROY = 13,
    BVB_OPCODE_VULKAN_PHYSICAL_DEVICES = 14,
    BVB_OPCODE_VULKAN_PHYSICAL_DEVICE_PROPERTIES = 15,
    BVB_OPCODE_VULKAN_QUEUE_FAMILY_PROPERTIES = 16,
    BVB_OPCODE_VULKAN_MEMORY_PROPERTIES = 17,
    BVB_OPCODE_VULKAN_DEVICE_EXTENSIONS = 18,
    BVB_HELLO_REQUEST_SIZE = 8,
    BVB_HELLO_RESPONSE_SIZE = 16,
    BVB_VULKAN_CAPS_PREFIX_SIZE = 16,
    BVB_VULKAN_CAPS_DEVICE_SIZE = 296,
    BVB_VULKAN_SELFTEST_SIZE = 64,
    BVB_ACTIVITY_STATUS_SIZE = 56,
    BVB_SHARED_BATCH_SETUP_SIZE = 16,
    BVB_SHARED_BATCH_EXECUTE_SIZE = 24,
    BVB_VISIBLE_BATCH_SETUP_SIZE =
        BVB_LIFECYCLE_TOKEN_SIZE + BVB_SHARED_BATCH_SETUP_SIZE,
    BVB_VISIBLE_BATCH_EXECUTE_SIZE =
        BVB_LIFECYCLE_TOKEN_SIZE + BVB_SHARED_BATCH_EXECUTE_SIZE,
    BVB_VISIBLE_BATCH_INLINE_PREFIX_SIZE = BVB_LIFECYCLE_TOKEN_SIZE,
    BVB_VISIBLE_BATCH_INLINE_MAX_BYTES =
        BVB_PROTOCOL_MAX_PAYLOAD - BVB_VISIBLE_BATCH_INLINE_PREFIX_SIZE,
    BVB_VULKAN_GLOBAL_INFO_SIZE = 24,
    BVB_VULKAN_INSTANCE_CREATE_REQUEST_SIZE = 16,
    BVB_VULKAN_INSTANCE_CREATE_RESPONSE_SIZE = 16,
    BVB_VULKAN_INSTANCE_ID_SIZE = 8,
    BVB_VULKAN_PHYSICAL_DEVICE_ID_SIZE = 8,
    BVB_VULKAN_DEVICE_EXTENSION_QUERY_SIZE = 16,
    BVB_VULKAN_PHYSICAL_DEVICES_PREFIX_SIZE = 8,
    BVB_VULKAN_MAX_PHYSICAL_DEVICES = 8,
    BVB_SHARED_BATCH_MIN_BYTES = 4096,
    BVB_SHARED_BATCH_MAX_BYTES = 16 * 1024 * 1024,
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

struct bvb_shared_batch_setup {
    uint32_t region_bytes;
    uint64_t generation;
};

struct bvb_shared_batch_execute {
    uint64_t generation;
    uint32_t offset;
    uint32_t length;
    uint64_t sequence;
};

struct bvb_visible_batch_setup {
    uint8_t token[BVB_LIFECYCLE_TOKEN_SIZE];
    struct bvb_shared_batch_setup shared;
};

struct bvb_visible_batch_execute {
    uint8_t token[BVB_LIFECYCLE_TOKEN_SIZE];
    struct bvb_shared_batch_execute shared;
};

struct bvb_vulkan_global_info {
    uint32_t loader_api_version;
    uint32_t native_extension_count;
    uint32_t native_layer_count;
    uint32_t exposed_extension_count;
    uint32_t exposed_layer_count;
};

struct bvb_vulkan_instance_create_request {
    uint32_t api_version;
    uint32_t flags;
    uint32_t enabled_layer_count;
    uint32_t enabled_extension_count;
};

struct bvb_vulkan_instance_create_response {
    int32_t vulkan_result;
    uint64_t instance_id;
};

struct bvb_vulkan_physical_devices {
    int32_t vulkan_result;
    uint32_t count;
    uint64_t ids[BVB_VULKAN_MAX_PHYSICAL_DEVICES];
};

struct bvb_vulkan_device_extension_query {
    uint64_t physical_device_id;
    uint32_t first;
    uint32_t max_count;
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
int bvb_protocol_encode_shared_batch_setup(
    uint8_t output[BVB_SHARED_BATCH_SETUP_SIZE],
    const struct bvb_shared_batch_setup *setup);
int bvb_protocol_decode_shared_batch_setup(
    const uint8_t input[BVB_SHARED_BATCH_SETUP_SIZE],
    struct bvb_shared_batch_setup *setup);
int bvb_protocol_encode_shared_batch_execute(
    uint8_t output[BVB_SHARED_BATCH_EXECUTE_SIZE],
    const struct bvb_shared_batch_execute *execute);
int bvb_protocol_decode_shared_batch_execute(
    const uint8_t input[BVB_SHARED_BATCH_EXECUTE_SIZE],
    struct bvb_shared_batch_execute *execute);
int bvb_protocol_encode_visible_batch_setup(
    uint8_t output[BVB_VISIBLE_BATCH_SETUP_SIZE],
    const struct bvb_visible_batch_setup *setup);
int bvb_protocol_decode_visible_batch_setup(
    const uint8_t input[BVB_VISIBLE_BATCH_SETUP_SIZE],
    struct bvb_visible_batch_setup *setup);
int bvb_protocol_encode_visible_batch_execute(
    uint8_t output[BVB_VISIBLE_BATCH_EXECUTE_SIZE],
    const struct bvb_visible_batch_execute *execute);
int bvb_protocol_decode_visible_batch_execute(
    const uint8_t input[BVB_VISIBLE_BATCH_EXECUTE_SIZE],
    struct bvb_visible_batch_execute *execute);
int bvb_protocol_encode_vulkan_global_info(
    uint8_t output[BVB_VULKAN_GLOBAL_INFO_SIZE],
    const struct bvb_vulkan_global_info *info);
int bvb_protocol_decode_vulkan_global_info(
    const uint8_t input[BVB_VULKAN_GLOBAL_INFO_SIZE],
    struct bvb_vulkan_global_info *info);
int bvb_protocol_encode_vulkan_instance_create_request(
    uint8_t output[BVB_VULKAN_INSTANCE_CREATE_REQUEST_SIZE],
    const struct bvb_vulkan_instance_create_request *request);
int bvb_protocol_decode_vulkan_instance_create_request(
    const uint8_t input[BVB_VULKAN_INSTANCE_CREATE_REQUEST_SIZE],
    struct bvb_vulkan_instance_create_request *request);
int bvb_protocol_encode_vulkan_instance_create_response(
    uint8_t output[BVB_VULKAN_INSTANCE_CREATE_RESPONSE_SIZE],
    const struct bvb_vulkan_instance_create_response *response);
int bvb_protocol_decode_vulkan_instance_create_response(
    const uint8_t input[BVB_VULKAN_INSTANCE_CREATE_RESPONSE_SIZE],
    struct bvb_vulkan_instance_create_response *response);
int bvb_protocol_encode_vulkan_instance_id(
    uint8_t output[BVB_VULKAN_INSTANCE_ID_SIZE], uint64_t instance_id);
int bvb_protocol_decode_vulkan_instance_id(
    const uint8_t input[BVB_VULKAN_INSTANCE_ID_SIZE], uint64_t *instance_id);
int bvb_protocol_encode_vulkan_physical_devices(
    uint8_t output[BVB_PROTOCOL_MAX_PAYLOAD],
    const struct bvb_vulkan_physical_devices *devices,
    uint32_t *output_length);
int bvb_protocol_decode_vulkan_physical_devices(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_physical_devices *devices);
int bvb_protocol_encode_vulkan_physical_device_id(
    uint8_t output[BVB_VULKAN_PHYSICAL_DEVICE_ID_SIZE],
    uint64_t physical_device_id);
int bvb_protocol_decode_vulkan_physical_device_id(
    const uint8_t input[BVB_VULKAN_PHYSICAL_DEVICE_ID_SIZE],
    uint64_t *physical_device_id);
int bvb_protocol_encode_vulkan_device_extension_query(
    uint8_t output[BVB_VULKAN_DEVICE_EXTENSION_QUERY_SIZE],
    const struct bvb_vulkan_device_extension_query *query);
int bvb_protocol_decode_vulkan_device_extension_query(
    const uint8_t input[BVB_VULKAN_DEVICE_EXTENSION_QUERY_SIZE],
    struct bvb_vulkan_device_extension_query *query);

void bvb_wire_put_u16(uint8_t *output, uint16_t value);
void bvb_wire_put_u32(uint8_t *output, uint32_t value);
void bvb_wire_put_i32(uint8_t *output, int32_t value);
void bvb_wire_put_u64(uint8_t *output, uint64_t value);
uint16_t bvb_wire_get_u16(const uint8_t *input);
uint32_t bvb_wire_get_u32(const uint8_t *input);
int32_t bvb_wire_get_i32(const uint8_t *input);
uint64_t bvb_wire_get_u64(const uint8_t *input);

#endif
