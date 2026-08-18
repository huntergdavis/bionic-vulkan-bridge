#include <bvb/protocol.h>
#include <bvb/transport.h>
#include <bvb/vulkan_caps.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct client_options {
    const char *socket_path;
    bool request_vulkan_caps;
};

static void usage(const char *program) {
    fprintf(stderr,
            "usage: %s --socket ABSOLUTE_PATH [--vulkan-caps]\n",
            program);
}

static int parse_arguments(int argc, char **argv,
                           struct client_options *options) {
    memset(options, 0, sizeof(*options));
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--socket") == 0 && index + 1 < argc &&
            argv[index + 1][0] == '/') {
            options->socket_path = argv[++index];
        } else if (strcmp(argv[index], "--vulkan-caps") == 0) {
            options->request_vulkan_caps = true;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (options->socket_path == NULL) {
        usage(argv[0]);
        return 2;
    }
    return 0;
}

static int exchange(int socket_fd,
                    const struct bvb_protocol_packet *request,
                    struct bvb_protocol_packet *response) {
    int result = bvb_transport_send(socket_fd, request);
    if (result != 0) {
        return result;
    }
    memset(response, 0, sizeof(*response));
    result = bvb_transport_receive(socket_fd, response);
    if (result != 0) {
        return result;
    }
    if (response->header.kind != BVB_PROTOCOL_RESPONSE ||
        response->header.opcode != request->header.opcode ||
        response->header.request_id != request->header.request_id) {
        return -1;
    }
    return 0;
}

static void print_json_string(const char *value) {
    const unsigned char *cursor = (const unsigned char *)value;
    putchar('"');
    while (*cursor != '\0') {
        switch (*cursor) {
        case '"':
            fputs("\\\"", stdout);
            break;
        case '\\':
            fputs("\\\\", stdout);
            break;
        case '\b':
            fputs("\\b", stdout);
            break;
        case '\f':
            fputs("\\f", stdout);
            break;
        case '\n':
            fputs("\\n", stdout);
            break;
        case '\r':
            fputs("\\r", stdout);
            break;
        case '\t':
            fputs("\\t", stdout);
            break;
        default:
            if (*cursor < 0x20U) {
                printf("\\u%04x", (unsigned int)*cursor);
            } else {
                putchar((int)*cursor);
            }
            break;
        }
        ++cursor;
    }
    putchar('"');
}

static void print_document(const struct bvb_protocol_packet *hello_packet,
                           const struct bvb_hello_response *hello,
                           const struct bvb_vulkan_caps *caps) {
    printf("{\"schema_version\":1,\"protocol_version\":%u,"
           "\"request_id\":%" PRIu32 ",\"service_flags\":%" PRIu32
           ",\"bionic_service\":%s,\"android_vulkan_loader\":%s,"
           "\"pointer_bits\":%" PRIu32 ",\"page_size\":%" PRIu32,
           (unsigned int)hello->negotiated_version,
           hello_packet->header.request_id,
           hello->service_flags,
           (hello->service_flags & BVB_SERVICE_BIONIC) != 0U ? "true"
                                                              : "false",
           (hello->service_flags & BVB_SERVICE_ANDROID_VULKAN_LOADER) != 0U
               ? "true"
               : "false",
           hello->pointer_bits,
           hello->page_size);
    if (caps != NULL) {
        printf(",\"vulkan_caps\":{\"loader_api_version\":%" PRIu32
               ",\"instance_extension_count\":%" PRIu32
               ",\"physical_device_count\":%" PRIu32
               ",\"physical_devices\":[",
               caps->loader_api_version,
               caps->instance_extension_count,
               caps->physical_device_count);
        for (uint32_t index = 0; index < caps->included_device_count; ++index) {
            const struct bvb_vulkan_device_caps *device = &caps->devices[index];
            if (index != 0U) {
                putchar(',');
            }
            printf("{\"index\":%" PRIu32 ",\"name\":", index);
            print_json_string(device->name);
            printf(",\"api_version\":%" PRIu32
                   ",\"driver_version\":%" PRIu32
                   ",\"vendor_id\":%" PRIu32
                   ",\"device_id\":%" PRIu32
                   ",\"device_type\":%" PRIu32
                   ",\"queue_family_count\":%" PRIu32
                   ",\"memory_heap_count\":%" PRIu32
                   ",\"device_local_bytes\":%" PRIu64 "}",
                   device->api_version,
                   device->driver_version,
                   device->vendor_id,
                   device->device_id,
                   device->device_type,
                   device->queue_family_count,
                   device->memory_heap_count,
                   device->device_local_bytes);
        }
        fputs("]}", stdout);
    }
    fputs("}\n", stdout);
}

int main(int argc, char **argv) {
    struct client_options options;
    int exit_code = parse_arguments(argc, argv, &options);
    if (exit_code != 0) {
        return exit_code;
    }

    int socket_fd = bvb_transport_connect(options.socket_path, geteuid());
    if (socket_fd < 0) {
        fprintf(stderr, "bvb: connect failed: %s\n", strerror(-socket_fd));
        return 3;
    }

    struct bvb_protocol_packet request;
    memset(&request, 0, sizeof(request));
    request.header.version = BVB_PROTOCOL_VERSION;
    request.header.kind = BVB_PROTOCOL_REQUEST;
    request.header.opcode = BVB_OPCODE_HELLO;
    request.header.request_id = 0x42564201U;
    request.header.payload_length = BVB_HELLO_REQUEST_SIZE;
    const struct bvb_hello_request hello_request = {
        .minimum_version = BVB_PROTOCOL_VERSION,
        .maximum_version = BVB_PROTOCOL_VERSION,
        .client_flags = 0,
    };
    int result = bvb_protocol_encode_hello_request(request.payload,
                                                   &hello_request);
    struct bvb_protocol_packet hello_packet;
    if (result == 0) {
        result = exchange(socket_fd, &request, &hello_packet);
    }
    if (result != 0) {
        (void)close(socket_fd);
        fputs("bvb: handshake I/O or response validation failed\n", stderr);
        return 4;
    }
    if (hello_packet.header.status != 0 ||
        hello_packet.header.payload_length != BVB_HELLO_RESPONSE_SIZE) {
        (void)close(socket_fd);
        fputs("bvb: service rejected handshake\n", stderr);
        return 5;
    }

    struct bvb_hello_response hello;
    result = bvb_protocol_decode_hello_response(hello_packet.payload, &hello);
    if (result != 0 || hello.negotiated_version != BVB_PROTOCOL_VERSION) {
        (void)close(socket_fd);
        fputs("bvb: invalid negotiated protocol\n", stderr);
        return 5;
    }

    struct bvb_vulkan_caps caps;
    struct bvb_vulkan_caps *caps_pointer = NULL;
    if (options.request_vulkan_caps) {
        memset(&request, 0, sizeof(request));
        request.header.version = BVB_PROTOCOL_VERSION;
        request.header.kind = BVB_PROTOCOL_REQUEST;
        request.header.opcode = BVB_OPCODE_VULKAN_CAPS;
        request.header.request_id = 0x42564202U;

        struct bvb_protocol_packet caps_packet;
        result = exchange(socket_fd, &request, &caps_packet);
        if (result != 0 || caps_packet.header.status != 0) {
            (void)close(socket_fd);
            fputs("bvb: Vulkan capability request failed\n", stderr);
            return 6;
        }
        result = bvb_protocol_decode_vulkan_caps(
            caps_packet.payload, caps_packet.header.payload_length, &caps);
        if (result != 0) {
            (void)close(socket_fd);
            fputs("bvb: invalid Vulkan capability response\n", stderr);
            return 6;
        }
        caps_pointer = &caps;
    }

    (void)close(socket_fd);
    print_document(&hello_packet, &hello, caps_pointer);
    return 0;
}

