#include <bvb/protocol.h>
#include <bvb/transport.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void usage(const char *program) {
    fprintf(stderr, "usage: %s --socket ABSOLUTE_PATH [--once]\n", program);
}

static int parse_arguments(int argc, char **argv, const char **socket_path,
                           bool *once) {
    *socket_path = NULL;
    *once = false;
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--socket") == 0 && index + 1 < argc &&
            argv[index + 1][0] == '/') {
            *socket_path = argv[++index];
        } else if (strcmp(argv[index], "--once") == 0) {
            *once = true;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (*socket_path == NULL) {
        usage(argv[0]);
        return 2;
    }
    return 0;
}

static uint32_t service_flags(void) {
    uint32_t flags = 0;
#if defined(__BIONIC__)
    flags |= BVB_SERVICE_BIONIC;
#endif
    if (access("/system/lib64/libvulkan.so", R_OK) == 0) {
        flags |= BVB_SERVICE_ANDROID_VULKAN_LOADER;
    }
    return flags;
}

static int answer_hello(int client_fd) {
    struct bvb_protocol_packet request;
    memset(&request, 0, sizeof(request));
    int result = bvb_transport_receive(client_fd, &request);
    if (result != 0) {
        return result;
    }
    if (request.header.kind != BVB_PROTOCOL_REQUEST ||
        request.header.opcode != BVB_OPCODE_HELLO ||
        request.header.payload_length != BVB_HELLO_REQUEST_SIZE) {
        return -EPROTO;
    }

    struct bvb_hello_request hello;
    result = bvb_protocol_decode_hello_request(request.payload, &hello);
    if (result != 0) {
        return result;
    }

    struct bvb_protocol_packet response;
    memset(&response, 0, sizeof(response));
    response.header.version = BVB_PROTOCOL_VERSION;
    response.header.kind = BVB_PROTOCOL_RESPONSE;
    response.header.opcode = BVB_OPCODE_HELLO;
    response.header.request_id = request.header.request_id;

    if (hello.minimum_version > BVB_PROTOCOL_VERSION ||
        hello.maximum_version < BVB_PROTOCOL_VERSION) {
        response.header.status = -EPROTONOSUPPORT;
        return bvb_transport_send(client_fd, &response);
    }

    long native_page_size = sysconf(_SC_PAGESIZE);
    if (native_page_size <= 0 || (unsigned long)native_page_size > UINT32_MAX) {
        response.header.status = -EINVAL;
        return bvb_transport_send(client_fd, &response);
    }
    const struct bvb_hello_response hello_response = {
        .negotiated_version = BVB_PROTOCOL_VERSION,
        .service_flags = service_flags(),
        .pointer_bits = (uint32_t)(sizeof(void *) * 8U),
        .page_size = (uint32_t)native_page_size,
    };
    result = bvb_protocol_encode_hello_response(response.payload,
                                                &hello_response);
    if (result != 0) {
        return result;
    }
    response.header.payload_length = BVB_HELLO_RESPONSE_SIZE;
    return bvb_transport_send(client_fd, &response);
}

int main(int argc, char **argv) {
    const char *socket_path = NULL;
    bool once = false;
    int exit_code = parse_arguments(argc, argv, &socket_path, &once);
    if (exit_code != 0) {
        return exit_code;
    }

    int listener = bvb_transport_listen(socket_path, geteuid());
    if (listener < 0) {
        fprintf(stderr, "bvb: listen failed: %s\n", strerror(-listener));
        return 3;
    }
    printf("bvb-bridge-service: ready socket=%s\n", socket_path);
    fflush(stdout);

    do {
        int client_fd = accept(listener, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "bvb: accept failed: %s\n", strerror(errno));
            exit_code = 4;
            break;
        }
        pid_t peer_pid = 0;
        int result = bvb_transport_authenticate(client_fd, geteuid(), &peer_pid);
        if (result == 0) {
            result = answer_hello(client_fd);
        }
        (void)close(client_fd);
        if (result != 0) {
            fprintf(stderr, "bvb: request from pid %ld failed: %s\n",
                    (long)peer_pid, strerror(-result));
            exit_code = 5;
            if (once) {
                break;
            }
        }
        if (once) {
            break;
        }
    } while (true);

    (void)close(listener);
    if (unlink(socket_path) != 0 && errno != ENOENT) {
        fprintf(stderr, "bvb: could not remove socket: %s\n", strerror(errno));
        return 6;
    }
    return exit_code;
}

