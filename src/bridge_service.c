#include <bvb/protocol.h>
#include <bvb/transport.h>
#include <bvb/vulkan_caps.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BVB_SYSTEM_VULKAN_LOADER "/system/lib64/libvulkan.so"

struct service_options {
    const char *socket_path;
    const char *loader_path;
    bool once;
};

static void usage(const char *program) {
    fprintf(stderr,
            "usage: %s --socket ABSOLUTE_PATH "
            "[--loader ABSOLUTE_PATH] [--once]\n",
            program);
}

static int parse_arguments(int argc, char **argv,
                           struct service_options *options) {
    *options = (struct service_options){
        .loader_path = BVB_SYSTEM_VULKAN_LOADER,
    };
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--socket") == 0 && index + 1 < argc &&
            argv[index + 1][0] == '/') {
            options->socket_path = argv[++index];
        } else if (strcmp(argv[index], "--loader") == 0 && index + 1 < argc &&
                   argv[index + 1][0] == '/') {
            options->loader_path = argv[++index];
        } else if (strcmp(argv[index], "--once") == 0) {
            options->once = true;
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

static uint32_t service_flags(const char *loader_path) {
    uint32_t flags = 0;
#if defined(__BIONIC__)
    flags |= BVB_SERVICE_BIONIC;
#endif
    if (strcmp(loader_path, BVB_SYSTEM_VULKAN_LOADER) == 0 &&
        access(loader_path, R_OK) == 0) {
        flags |= BVB_SERVICE_ANDROID_VULKAN_LOADER;
    }
    return flags;
}

static void prepare_response(struct bvb_protocol_packet *response,
                             const struct bvb_protocol_packet *request) {
    memset(response, 0, sizeof(*response));
    response->header.version = BVB_PROTOCOL_VERSION;
    response->header.kind = BVB_PROTOCOL_RESPONSE;
    response->header.opcode = request->header.opcode;
    response->header.request_id = request->header.request_id;
}

static int answer_hello(int client_fd,
                        const struct bvb_protocol_packet *request,
                        const char *loader_path,
                        bool *negotiated) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    *negotiated = false;
    if (request->header.payload_length != BVB_HELLO_REQUEST_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }

    struct bvb_hello_request hello;
    int result = bvb_protocol_decode_hello_request(request->payload, &hello);
    if (result != 0) {
        response.header.status = result;
        return bvb_transport_send(client_fd, &response);
    }
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
        .service_flags = service_flags(loader_path),
        .pointer_bits = (uint32_t)(sizeof(void *) * 8U),
        .page_size = (uint32_t)native_page_size,
    };
    result = bvb_protocol_encode_hello_response(response.payload,
                                                &hello_response);
    if (result != 0) {
        return result;
    }
    response.header.payload_length = BVB_HELLO_RESPONSE_SIZE;
    result = bvb_transport_send(client_fd, &response);
    if (result == 0) {
        *negotiated = true;
    }
    return result;
}

static int answer_vulkan_caps(int client_fd,
                              const struct bvb_protocol_packet *request,
                              const char *loader_path,
                              bool negotiated) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || request->header.payload_length != 0U) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }

    struct bvb_vulkan_caps caps;
    char diagnostic[512];
    int result = bvb_vulkan_collect(loader_path, &caps, diagnostic,
                                    sizeof(diagnostic));
    if (result != 0) {
        fprintf(stderr, "bvb: Vulkan capability query failed: %s\n",
                diagnostic);
        response.header.status = result;
        return bvb_transport_send(client_fd, &response);
    }
    result = bvb_protocol_encode_vulkan_caps(response.payload, &caps,
                                             &response.header.payload_length);
    if (result != 0) {
        return result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int serve_connection(int client_fd, const char *loader_path) {
    bool negotiated = false;
    while (true) {
        struct bvb_protocol_packet request;
        memset(&request, 0, sizeof(request));
        int result = bvb_transport_receive(client_fd, &request);
        if (result == BVB_TRANSPORT_EOF) {
            return 0;
        }
        if (result != 0) {
            return result;
        }
        if (request.header.kind != BVB_PROTOCOL_REQUEST) {
            return -EPROTO;
        }
        if (request.header.opcode == BVB_OPCODE_HELLO) {
            result = answer_hello(client_fd, &request, loader_path,
                                  &negotiated);
        } else if (request.header.opcode == BVB_OPCODE_VULKAN_CAPS) {
            result = answer_vulkan_caps(client_fd, &request, loader_path,
                                        negotiated);
        } else {
            result = -EPROTO;
        }
        if (result != 0) {
            return result;
        }
    }
}

int main(int argc, char **argv) {
    struct service_options options;
    int exit_code = parse_arguments(argc, argv, &options);
    if (exit_code != 0) {
        return exit_code;
    }

    int listener = bvb_transport_listen(options.socket_path, geteuid());
    if (listener < 0) {
        fprintf(stderr, "bvb: listen failed: %s\n", strerror(-listener));
        return 3;
    }
    printf("bvb-bridge-service: ready socket=%s loader=%s\n",
           options.socket_path, options.loader_path);
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
            result = serve_connection(client_fd, options.loader_path);
        }
        (void)close(client_fd);
        if (result != 0) {
            fprintf(stderr, "bvb: connection from pid %ld failed: %s\n",
                    (long)peer_pid, strerror(-result));
            exit_code = 5;
            if (options.once) {
                break;
            }
        }
        if (options.once) {
            break;
        }
    } while (true);

    (void)close(listener);
    if (unlink(options.socket_path) != 0 && errno != ENOENT) {
        fprintf(stderr, "bvb: could not remove socket: %s\n", strerror(errno));
        return 6;
    }
    return exit_code;
}

