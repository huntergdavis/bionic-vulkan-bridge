#include <bvb/protocol.h>
#include <bvb/transport.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *program) {
    fprintf(stderr, "usage: %s --socket ABSOLUTE_PATH\n", program);
}

static const char *parse_arguments(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--socket") == 0 && argv[2][0] == '/') {
        return argv[2];
    }
    usage(argv[0]);
    return NULL;
}

int main(int argc, char **argv) {
    const char *socket_path = parse_arguments(argc, argv);
    if (socket_path == NULL) {
        return 2;
    }

    int socket_fd = bvb_transport_connect(socket_path, geteuid());
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
    const struct bvb_hello_request hello = {
        .minimum_version = BVB_PROTOCOL_VERSION,
        .maximum_version = BVB_PROTOCOL_VERSION,
        .client_flags = 0,
    };
    int result = bvb_protocol_encode_hello_request(request.payload, &hello);
    if (result == 0) {
        result = bvb_transport_send(socket_fd, &request);
    }

    struct bvb_protocol_packet response;
    memset(&response, 0, sizeof(response));
    if (result == 0) {
        result = bvb_transport_receive(socket_fd, &response);
    }
    (void)close(socket_fd);
    if (result != 0) {
        fprintf(stderr, "bvb: handshake I/O failed: %s\n", strerror(-result));
        return 4;
    }
    if (response.header.kind != BVB_PROTOCOL_RESPONSE ||
        response.header.opcode != BVB_OPCODE_HELLO ||
        response.header.request_id != request.header.request_id) {
        fputs("bvb: invalid handshake response header\n", stderr);
        return 5;
    }
    if (response.header.status != 0) {
        fprintf(stderr, "bvb: service rejected handshake: %s\n",
                strerror(-response.header.status));
        return 6;
    }
    if (response.header.payload_length != BVB_HELLO_RESPONSE_SIZE) {
        fputs("bvb: invalid handshake response length\n", stderr);
        return 5;
    }

    struct bvb_hello_response negotiated;
    result = bvb_protocol_decode_hello_response(response.payload, &negotiated);
    if (result != 0 || negotiated.negotiated_version != BVB_PROTOCOL_VERSION) {
        fputs("bvb: invalid negotiated protocol\n", stderr);
        return 5;
    }

    printf("{\"schema_version\":1,\"protocol_version\":%u,"
           "\"request_id\":%" PRIu32 ",\"service_flags\":%" PRIu32
           ",\"bionic_service\":%s,\"android_vulkan_loader\":%s,"
           "\"pointer_bits\":%" PRIu32 ",\"page_size\":%" PRIu32 "}\n",
           (unsigned int)negotiated.negotiated_version,
           response.header.request_id,
           negotiated.service_flags,
           (negotiated.service_flags & BVB_SERVICE_BIONIC) != 0U ? "true"
                                                                  : "false",
           (negotiated.service_flags & BVB_SERVICE_ANDROID_VULKAN_LOADER) != 0U
               ? "true"
               : "false",
           negotiated.pointer_bits,
           negotiated.page_size);
    return 0;
}

