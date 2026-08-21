#define _POSIX_C_SOURCE 200809L

#include <bvb/command_batch.h>
#include <bvb/activity_frame_transport.h>
#include <bvb/handle.h>
#include <bvb/protocol.h>
#include <bvb/transport.h>
#include <bvb/vulkan_caps.h>
#include <bvb/vulkan_global.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BVB_SYSTEM_VULKAN_LOADER "/system/lib64/libvulkan.so"

struct service_options {
    const char *socket_path;
    const char *loader_path;
    const char *activity_frame_socket;
    bool once;
    bool activity_ingress;
    uint16_t activity_port;
    uint8_t activity_token[BVB_LIFECYCLE_TOKEN_SIZE];
};

struct shared_batch_region {
    const uint8_t *address;
    size_t length;
    uint64_t generation;
    uint64_t last_sequence;
};

struct connection_worker {
    int client_fd;
    const char *loader_path;
    const char *activity_frame_socket;
    bool activity_ingress;
    struct bvb_activity_status activity_status;
    pid_t peer_pid;
};

static void usage(const char *program) {
    fprintf(stderr,
            "usage: %s --socket ABSOLUTE_PATH "
            "[--loader ABSOLUTE_PATH] [--once] "
            "[--activity-port 0..65535 --activity-token 64_HEX] "
            "[--activity-frame-socket ABSTRACT_NAME]\n",
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
        } else if (strcmp(argv[index], "--activity-port") == 0 &&
                   index + 1 < argc) {
            char *end = NULL;
            errno = 0;
            unsigned long port = strtoul(argv[++index], &end, 10);
            if (errno != 0 || end == argv[index] || *end != '\0' ||
                port > UINT16_MAX) {
                usage(argv[0]);
                return 2;
            }
            options->activity_port = (uint16_t)port;
            options->activity_ingress = true;
        } else if (strcmp(argv[index], "--activity-token") == 0 &&
                   index + 1 < argc) {
            if (bvb_lifecycle_token_from_hex(argv[++index],
                                             options->activity_token) != 0) {
                usage(argv[0]);
                return 2;
            }
            options->activity_ingress = true;
        } else if (strcmp(argv[index], "--activity-frame-socket") == 0 &&
                   index + 1 < argc && argv[index + 1][0] != '\0' &&
                   strlen(argv[index + 1]) <= 106U) {
            options->activity_frame_socket = argv[++index];
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (options->socket_path == NULL) {
        usage(argv[0]);
        return 2;
    }
    bool token_present = false;
    for (size_t index = 0; index < BVB_LIFECYCLE_TOKEN_SIZE; ++index) {
        token_present |= options->activity_token[index] != 0U;
    }
    if (options->activity_ingress && !token_present) {
        fputs("bvb: activity ingress requires a nonzero 256-bit token\n", stderr);
        return 2;
    }
    return 0;
}

static uint32_t service_flags(const char *loader_path, bool activity_ingress) {
    uint32_t flags = 0;
#if defined(__BIONIC__)
    flags |= BVB_SERVICE_BIONIC;
#endif
    if (strcmp(loader_path, BVB_SYSTEM_VULKAN_LOADER) == 0 &&
        access(loader_path, R_OK) == 0) {
        flags |= BVB_SERVICE_ANDROID_VULKAN_LOADER;
    }
    if (activity_ingress) {
        flags |= BVB_SERVICE_ACTIVITY_INGRESS;
    }
    return flags;
}

static uint64_t monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static int activity_listen(uint16_t requested_port, uint16_t *actual_port) {
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    if (listener < 0) {
        return -errno;
    }
    int enabled = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled,
                   sizeof(enabled)) != 0) {
        int result = -errno;
        (void)close(listener);
        return result;
    }
    const struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(requested_port),
        .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)},
    };
    if (bind(listener, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, 4) != 0) {
        int result = -errno;
        (void)close(listener);
        return result;
    }
    struct sockaddr_in bound;
    socklen_t bound_size = sizeof(bound);
    if (getsockname(listener, (struct sockaddr *)&bound, &bound_size) != 0 ||
        bound_size != sizeof(bound)) {
        int result = errno == 0 ? -EIO : -errno;
        (void)close(listener);
        return result;
    }
    *actual_port = ntohs(bound.sin_port);
    return listener;
}

static int receive_exact_timeout(int socket_fd, uint8_t *output,
                                 size_t length) {
    size_t offset = 0;
    while (offset < length) {
        struct pollfd descriptor = {.fd = socket_fd, .events = POLLIN};
        int ready = poll(&descriptor, 1, 1000);
        if (ready == 0) {
            return -ETIMEDOUT;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        ssize_t received = recv(socket_fd, output + offset, length - offset, 0);
        if (received == 0) {
            return -ECONNRESET;
        }
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        offset += (size_t)received;
    }
    return 0;
}

static int send_exact(int socket_fd, const uint8_t *input, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        ssize_t sent = send(socket_fd, input + offset, length - offset,
                            MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        offset += (size_t)sent;
    }
    return 0;
}

static int accept_cloexec(int listener) {
    int connection = accept(listener, NULL, NULL);
    if (connection < 0) {
        return -errno;
    }
    int flags = fcntl(connection, F_GETFD);
    if (flags < 0 || fcntl(connection, F_SETFD, flags | FD_CLOEXEC) != 0) {
        int result = -errno;
        (void)close(connection);
        return result;
    }
    return connection;
}

static bool token_matches(const uint8_t *left, const uint8_t *right) {
    uint8_t difference = 0;
    for (size_t index = 0; index < BVB_LIFECYCLE_TOKEN_SIZE; ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0U;
}

static int apply_lifecycle_event(struct bvb_activity_status *status,
                                 const struct bvb_lifecycle_record *record) {
    if (record->event == BVB_LIFECYCLE_EVENT_CREATED) {
        if (record->sequence != 1U) {
            return -EPROTO;
        }
        status->state_flags = BVB_ACTIVITY_CREATED;
        status->width = 0;
        status->height = 0;
        status->activity_pid = record->activity_pid;
    } else if (status->authenticated_event_count == 0U ||
               status->activity_pid != record->activity_pid ||
               record->sequence <= status->last_sequence) {
        return -EPROTO;
    }

    switch (record->event) {
    case BVB_LIFECYCLE_EVENT_CREATED:
        break;
    case BVB_LIFECYCLE_EVENT_STARTED:
        status->state_flags |= BVB_ACTIVITY_STARTED;
        break;
    case BVB_LIFECYCLE_EVENT_RESUMED:
        status->state_flags |= BVB_ACTIVITY_RESUMED;
        break;
    case BVB_LIFECYCLE_EVENT_PAUSED:
        status->state_flags &= ~(BVB_ACTIVITY_RESUMED | BVB_ACTIVITY_FOCUSED);
        break;
    case BVB_LIFECYCLE_EVENT_STOPPED:
        status->state_flags &=
            ~(BVB_ACTIVITY_STARTED | BVB_ACTIVITY_RESUMED | BVB_ACTIVITY_FOCUSED);
        break;
    case BVB_LIFECYCLE_EVENT_DESTROYED:
        status->state_flags = BVB_ACTIVITY_DESTROYED;
        status->width = 0;
        status->height = 0;
        break;
    case BVB_LIFECYCLE_EVENT_WINDOW_CREATED:
        status->state_flags |= BVB_ACTIVITY_WINDOW_PRESENT;
        status->width = record->width;
        status->height = record->height;
        break;
    case BVB_LIFECYCLE_EVENT_WINDOW_DESTROYED:
        status->state_flags &=
            ~(BVB_ACTIVITY_WINDOW_PRESENT | BVB_ACTIVITY_RENDERER_READY);
        status->width = 0;
        status->height = 0;
        break;
    case BVB_LIFECYCLE_EVENT_FOCUS_GAINED:
        status->state_flags |= BVB_ACTIVITY_FOCUSED;
        break;
    case BVB_LIFECYCLE_EVENT_FOCUS_LOST:
        status->state_flags &= ~BVB_ACTIVITY_FOCUSED;
        break;
    case BVB_LIFECYCLE_EVENT_RENDERER_READY:
        status->state_flags |=
            BVB_ACTIVITY_WINDOW_PRESENT | BVB_ACTIVITY_RENDERER_READY;
        status->width = record->width;
        status->height = record->height;
        break;
    case BVB_LIFECYCLE_EVENT_RENDERER_FAILED:
        status->state_flags &= ~BVB_ACTIVITY_RENDERER_READY;
        status->state_flags |= BVB_ACTIVITY_WINDOW_PRESENT;
        status->width = record->width;
        status->height = record->height;
        break;
    case BVB_LIFECYCLE_EVENT_NATIVE_BINDER_STATUS:
        break;
    default:
        return -EPROTO;
    }
    status->authenticated_event_count += 1U;
    status->last_sequence = record->sequence;
    status->last_event = record->event;
    status->last_event_monotonic_ns = record->monotonic_ns;
    status->last_event_received_ns = monotonic_ns();
    return 0;
}

static int handle_activity_connection(
    int connection, const uint8_t token[BVB_LIFECYCLE_TOKEN_SIZE],
    struct bvb_activity_status *status) {
    uint8_t wire[BVB_LIFECYCLE_RECORD_SIZE];
    int result = receive_exact_timeout(connection, wire, sizeof(wire));
    if (result != 0) {
        status->rejected_event_count += 1U;
        return result;
    }
    struct bvb_lifecycle_record record;
    result = bvb_lifecycle_decode_record(wire, &record);
    uint32_t sequence = bvb_wire_get_u32(wire + 8);
    if (result == 0 && !token_matches(record.token, token)) {
        result = -EACCES;
    }
    if (result == 0) {
        result = apply_lifecycle_event(status, &record);
    }
    if (result != 0) {
        status->rejected_event_count += 1U;
    }
    if (sequence == 0U) {
        return result == 0 ? -EPROTO : result;
    }
    const struct bvb_lifecycle_ack ack = {
        .sequence = sequence,
        .status = result,
    };
    uint8_t ack_wire[BVB_LIFECYCLE_ACK_SIZE];
    int ack_result = bvb_lifecycle_encode_ack(ack_wire, &ack);
    if (ack_result == 0) {
        ack_result = send_exact(connection, ack_wire, sizeof(ack_wire));
    }
    if (result == 0) {
        printf("bvb-bridge-service: activity_event=%u sequence=%u pid=%u "
               "width=%u height=%u\n",
               (unsigned int)record.event, record.sequence,
               record.activity_pid, record.width, record.height);
        fflush(stdout);
    }
    return ack_result != 0 ? ack_result : result;
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
                        const char *loader_path, bool activity_ingress,
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
        .service_flags = service_flags(loader_path, activity_ingress),
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

static int answer_activity_status(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    const struct bvb_activity_status *activity_status) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || request->header.payload_length != 0U) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    int result = bvb_protocol_encode_activity_status(response.payload,
                                                     activity_status);
    if (result != 0) {
        return result;
    }
    response.header.payload_length = BVB_ACTIVITY_STATUS_SIZE;
    return bvb_transport_send(client_fd, &response);
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

static int ensure_vulkan_global_context(
    const char *loader_path, struct bvb_vulkan_global_context **context) {
    if (*context != NULL) {
        return 0;
    }
    char diagnostic[512];
    int result = bvb_vulkan_global_context_create(
        loader_path, context, diagnostic, sizeof(diagnostic));
    if (result != 0) {
        fprintf(stderr, "bvb: global Vulkan context failed: %s\n",
                diagnostic);
    }
    return result;
}

static int answer_vulkan_global_info(
    int client_fd, const struct bvb_protocol_packet *request,
    const char *loader_path, bool negotiated,
    struct bvb_vulkan_global_context **context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length != 0U) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    int result = ensure_vulkan_global_context(loader_path, context);
    struct bvb_vulkan_global_info info;
    if (result == 0) {
        result = bvb_vulkan_global_context_info(*context, &info);
    }
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_global_info(response.payload,
                                                        &info);
    }
    if (result == 0) {
        response.header.payload_length = BVB_VULKAN_GLOBAL_INFO_SIZE;
    } else {
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_instance_create(
    int client_fd, const struct bvb_protocol_packet *request,
    const char *loader_path, bool negotiated,
    struct bvb_vulkan_global_context **context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        (request->header.opcode == BVB_OPCODE_VULKAN_INSTANCE_CREATE &&
         request->header.payload_length !=
             BVB_VULKAN_INSTANCE_CREATE_REQUEST_SIZE) ||
        (request->header.opcode != BVB_OPCODE_VULKAN_INSTANCE_CREATE &&
         request->header.opcode !=
             BVB_OPCODE_VULKAN_INSTANCE_CREATE_EXTENDED)) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_instance_create_request create_request = {0};
    struct bvb_vulkan_instance_create_extended_request extended = {0};
    const char *enabled_extensions[BVB_VULKAN_MAX_ENABLED_EXTENSIONS] = {0};
    int result = 0;
    if (request->header.opcode == BVB_OPCODE_VULKAN_INSTANCE_CREATE) {
        result = bvb_protocol_decode_vulkan_instance_create_request(
            request->payload, &create_request);
    } else {
        result = bvb_protocol_decode_vulkan_instance_create_extended_request(
            request->payload, request->header.payload_length, &extended);
        if (result == 0) {
            create_request = extended.base;
            for (uint32_t index = 0U;
                 index < create_request.enabled_extension_count; ++index) {
                enabled_extensions[index] = extended.enabled_extensions[index];
            }
        }
    }
    if (result == 0) {
        result = ensure_vulkan_global_context(loader_path, context);
    }
    struct bvb_vulkan_instance_create_response create_response;
    char diagnostic[512];
    if (result == 0) {
        result = bvb_vulkan_global_context_create_instance(
            *context, &create_request,
            create_request.enabled_extension_count == 0U
                ? NULL : enabled_extensions,
            &create_response,
            diagnostic, sizeof(diagnostic));
        if (result != 0) {
            fprintf(stderr, "bvb: Vulkan instance create failed: %s\n",
                    diagnostic);
        }
    }
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_instance_create_response(
            response.payload, &create_response);
    }
    if (result == 0) {
        response.header.payload_length =
            BVB_VULKAN_INSTANCE_CREATE_RESPONSE_SIZE;
    } else {
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_instance_extensions(
    int client_fd, const struct bvb_protocol_packet *request,
    const char *loader_path, bool negotiated,
    struct bvb_vulkan_global_context **context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length != 0U) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    int result = ensure_vulkan_global_context(loader_path, context);
    struct bvb_vulkan_extension_page page;
    if (result == 0) {
        result = bvb_vulkan_global_context_enumerate_instance_extensions(
            *context, &page);
    }
    if (result == 0) {
        result = bvb_vulkan_encode_extension_page(
            response.payload, &page, &response.header.payload_length);
    }
    if (result != 0) {
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_instance_destroy(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length != BVB_VULKAN_INSTANCE_ID_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    uint64_t instance_id = 0U;
    int result = bvb_protocol_decode_vulkan_instance_id(
        request->payload, &instance_id);
    if (result == 0) {
        result = bvb_vulkan_global_context_destroy_instance(
            context, instance_id);
    }
    response.header.status = result;
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_physical_devices(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length != BVB_VULKAN_INSTANCE_ID_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    uint64_t instance_id = 0U;
    int result = bvb_protocol_decode_vulkan_instance_id(
        request->payload, &instance_id);
    struct bvb_vulkan_physical_devices devices;
    char diagnostic[512];
    if (result == 0) {
        result = bvb_vulkan_global_context_enumerate_physical_devices(
            context, instance_id, &devices, diagnostic, sizeof(diagnostic));
        if (result != 0) {
            fprintf(stderr, "bvb: physical-device enumeration failed: %s\n",
                    diagnostic);
        }
    }
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_physical_devices(
            response.payload, &devices, &response.header.payload_length);
    }
    if (result != 0) {
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_physical_device_properties(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length !=
            BVB_VULKAN_PHYSICAL_DEVICE_ID_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    uint64_t physical_device_id = 0U;
    int result = bvb_protocol_decode_vulkan_physical_device_id(
        request->payload, &physical_device_id);
    VkPhysicalDeviceProperties properties;
    char diagnostic[512];
    if (result == 0) {
        result = bvb_vulkan_global_context_get_physical_device_properties(
            context, physical_device_id, &properties,
            diagnostic, sizeof(diagnostic));
        if (result != 0) {
            fprintf(stderr, "bvb: physical-device properties failed: %s\n",
                    diagnostic);
        }
    }
    if (result == 0) {
        result = bvb_vulkan_encode_physical_device_properties(
            response.payload, &properties, &response.header.payload_length);
    }
    if (result != 0) {
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_queue_family_properties(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length !=
            BVB_VULKAN_PHYSICAL_DEVICE_ID_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    uint64_t physical_device_id = 0U;
    int result = bvb_protocol_decode_vulkan_physical_device_id(
        request->payload, &physical_device_id);
    VkQueueFamilyProperties properties[BVB_VULKAN_MAX_QUEUE_FAMILIES];
    uint32_t count = 0U;
    char diagnostic[512];
    if (result == 0) {
        result = bvb_vulkan_global_context_get_queue_family_properties(
            context, physical_device_id, properties, &count,
            diagnostic, sizeof(diagnostic));
        if (result != 0) {
            fprintf(stderr, "bvb: queue-family properties failed: %s\n",
                    diagnostic);
        }
    }
    if (result == 0) {
        result = bvb_vulkan_encode_queue_family_properties(
            response.payload, properties, count,
            &response.header.payload_length);
    }
    if (result != 0) {
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_memory_properties(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length !=
            BVB_VULKAN_PHYSICAL_DEVICE_ID_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    uint64_t physical_device_id = 0U;
    int result = bvb_protocol_decode_vulkan_physical_device_id(
        request->payload, &physical_device_id);
    VkPhysicalDeviceMemoryProperties properties;
    char diagnostic[512];
    if (result == 0) {
        result = bvb_vulkan_global_context_get_memory_properties(
            context, physical_device_id, &properties,
            diagnostic, sizeof(diagnostic));
        if (result != 0) {
            fprintf(stderr, "bvb: memory properties failed: %s\n",
                    diagnostic);
        }
    }
    if (result == 0) {
        result = bvb_vulkan_encode_memory_properties(
            response.payload, &properties, &response.header.payload_length);
    }
    if (result != 0) {
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_device_extensions(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length !=
            BVB_VULKAN_DEVICE_EXTENSION_QUERY_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_device_extension_query query;
    int result = bvb_protocol_decode_vulkan_device_extension_query(
        request->payload, &query);
    if (result == 0 &&
        query.max_count > BVB_VULKAN_EXTENSION_PAGE_CAPACITY) {
        result = -ERANGE;
    }
    struct bvb_vulkan_extension_page page;
    char diagnostic[512];
    if (result == 0) {
        result = bvb_vulkan_global_context_enumerate_device_extensions(
            context, &query, &page, diagnostic, sizeof(diagnostic));
        if (result != 0) {
            fprintf(stderr, "bvb: device-extension enumeration failed: %s\n",
                    diagnostic);
        }
    }
    if (result == 0) {
        result = bvb_vulkan_encode_extension_page(
            response.payload, &page, &response.header.payload_length);
    }
    if (result != 0) {
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_physical_device_features(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length !=
            BVB_VULKAN_PHYSICAL_DEVICE_ID_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    uint64_t physical_device_id = 0U;
    int result = bvb_protocol_decode_vulkan_physical_device_id(
        request->payload, &physical_device_id);
    VkPhysicalDeviceFeatures features;
    char diagnostic[512];
    if (result == 0) {
        result = bvb_vulkan_global_context_get_physical_device_features(
            context, physical_device_id, &features,
            diagnostic, sizeof(diagnostic));
        if (result != 0) {
            fprintf(stderr, "bvb: physical-device features failed: %s\n",
                    diagnostic);
        }
    }
    if (result == 0) {
        result = bvb_vulkan_encode_physical_device_features(
            response.payload, &features, &response.header.payload_length);
    }
    if (result != 0) {
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_core_features(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length !=
            BVB_VULKAN_PHYSICAL_DEVICE_ID_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    uint64_t physical_device_id = 0U;
    int result = bvb_protocol_decode_vulkan_physical_device_id(
        request->payload, &physical_device_id);
    struct bvb_vulkan_core_features features;
    char diagnostic[512];
    if (result == 0) {
        result = bvb_vulkan_global_context_get_core_features(
            context, physical_device_id, &features,
            diagnostic, sizeof(diagnostic));
        if (result != 0) {
            fprintf(stderr,
                    "bvb: core-feature query failed: %s\n",
                    diagnostic);
        }
    }
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_core_features(
            response.payload, &features);
        response.header.payload_length =
            BVB_VULKAN_CORE_FEATURES_SIZE;
    }
    if (result != 0) {
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_format_properties(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length != BVB_VULKAN_FORMAT_QUERY_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_format_query query;
    int result = bvb_protocol_decode_vulkan_format_query(
        request->payload, &query);
    struct bvb_vulkan_format_properties properties;
    char diagnostic[512];
    if (result == 0) {
        result = bvb_vulkan_global_context_get_format_properties(
            context, &query, &properties, diagnostic, sizeof(diagnostic));
        if (result != 0) {
            fprintf(stderr, "bvb: format properties failed: %s\n",
                    diagnostic);
        }
    }
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_format_properties(
            response.payload, &properties);
    }
    if (result == 0) {
        response.header.payload_length = BVB_VULKAN_FORMAT_PROPERTIES_SIZE;
    } else {
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_image_format_properties(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL || request->header.payload_length !=
        BVB_VULKAN_IMAGE_FORMAT_QUERY_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_image_format_query query;
    int result = bvb_protocol_decode_vulkan_image_format_query(
        request->payload, &query);
    struct bvb_vulkan_image_format_properties properties;
    char diagnostic[512];
    if (result == 0) {
        result = bvb_vulkan_global_context_get_image_format_properties(
            context, &query, &properties, diagnostic, sizeof(diagnostic));
        if (result != 0) {
            fprintf(stderr, "bvb: image-format properties failed: %s\n",
                    diagnostic);
        }
    }
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_image_format_properties(
            response.payload, &properties);
    }
    if (result == 0) {
        response.header.payload_length =
            BVB_VULKAN_IMAGE_FORMAT_PROPERTIES_SIZE;
    } else {
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_external_buffer_properties(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length !=
            BVB_VULKAN_EXTERNAL_BUFFER_QUERY_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_external_buffer_query query;
    int result = bvb_protocol_decode_vulkan_external_buffer_query(
        request->payload, &query);
    struct bvb_vulkan_external_buffer_properties properties;
    char diagnostic[512];
    if (result == 0) {
        result = bvb_vulkan_global_context_get_external_buffer_properties(
            context, &query, &properties, diagnostic, sizeof(diagnostic));
        if (result != 0) {
            fprintf(stderr, "bvb: external-buffer properties failed: %s\n",
                    diagnostic);
        }
    }
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_external_buffer_properties(
            response.payload, &properties);
    }
    if (result == 0) {
        response.header.payload_length =
            BVB_VULKAN_EXTERNAL_BUFFER_PROPERTIES_SIZE;
    } else {
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_external_semaphore_properties(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length !=
            BVB_VULKAN_EXTERNAL_SEMAPHORE_QUERY_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_external_semaphore_query query;
    int result = bvb_protocol_decode_vulkan_external_semaphore_query(
        request->payload, &query);
    struct bvb_vulkan_external_semaphore_properties properties;
    char diagnostic[512];
    if (result == 0) {
        result = bvb_vulkan_global_context_get_external_semaphore_properties(
            context, &query, &properties, diagnostic, sizeof(diagnostic));
        if (result != 0) {
            fprintf(
                stderr,
                "bvb: external-semaphore properties failed: %s\n",
                diagnostic);
        }
    }
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_external_semaphore_properties(
            response.payload, &properties);
    }
    if (result == 0) {
        response.header.payload_length =
            BVB_VULKAN_EXTERNAL_SEMAPHORE_PROPERTIES_SIZE;
    } else {
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_device_create(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        (request->header.opcode == BVB_OPCODE_VULKAN_DEVICE_CREATE &&
         request->header.payload_length !=
             BVB_VULKAN_DEVICE_CREATE_REQUEST_SIZE) ||
        (request->header.opcode != BVB_OPCODE_VULKAN_DEVICE_CREATE &&
         request->header.opcode !=
             BVB_OPCODE_VULKAN_DEVICE_CREATE_EXTENDED &&
         request->header.opcode !=
             BVB_OPCODE_VULKAN_DEVICE_CREATE_PACKED)) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_device_create_request create_request = {0};
    struct bvb_vulkan_device_create_extended_request extended = {0};
    struct bvb_vulkan_device_create_packed_request packed = {0};
    const char *enabled_extensions[
        BVB_VULKAN_MAX_DEVICE_CREATE_EXTENSIONS] = {0};
    int result = 0;
    if (request->header.opcode == BVB_OPCODE_VULKAN_DEVICE_CREATE) {
        result = bvb_protocol_decode_vulkan_device_create_request(
            request->payload, &create_request);
    } else if (request->header.opcode ==
               BVB_OPCODE_VULKAN_DEVICE_CREATE_EXTENDED) {
        result = bvb_protocol_decode_vulkan_device_create_extended_request(
            request->payload, request->header.payload_length, &extended);
        if (result == 0) {
            create_request = extended.base;
            for (uint32_t index = 0U;
                 index < create_request.enabled_extension_count; ++index) {
                enabled_extensions[index] = extended.enabled_extensions[index];
            }
        }
    } else {
        result = bvb_protocol_decode_vulkan_device_create_packed_request(
            request->payload, request->header.payload_length, &packed);
        if (result == 0) {
            for (uint32_t index = 0U;
                 index < packed.enabled_extension_count; ++index) {
                enabled_extensions[index] = packed.enabled_extensions[index];
            }
        }
    }
    struct bvb_vulkan_device_create_response create_response;
    char diagnostic[512];
    if (result == 0) {
        if (request->header.opcode ==
            BVB_OPCODE_VULKAN_DEVICE_CREATE_PACKED) {
            result = bvb_vulkan_global_context_create_device_packed(
                context, &packed,
                packed.enabled_extension_count == 0U
                    ? NULL : enabled_extensions,
                &create_response, diagnostic, sizeof(diagnostic));
        } else {
            result = bvb_vulkan_global_context_create_device(
                context, &create_request,
                create_request.enabled_extension_count == 0U
                    ? NULL : enabled_extensions,
                &create_response, diagnostic, sizeof(diagnostic));
        }
        if (result != 0) {
            fprintf(stderr, "bvb: Vulkan device create failed: %s\n",
                    diagnostic);
        }
    }
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_device_create_response(
            response.payload, &create_response);
    }
    if (result == 0) {
        response.header.payload_length =
            BVB_VULKAN_DEVICE_CREATE_RESPONSE_SIZE;
    } else {
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_device_destroy(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length != BVB_VULKAN_DEVICE_ID_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    uint64_t device_id = 0U;
    int result = bvb_protocol_decode_vulkan_device_id(
        request->payload, &device_id);
    if (result == 0) {
        result = bvb_vulkan_global_context_destroy_device(context, device_id);
    }
    response.header.status = result;
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_device_queue(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length !=
            BVB_VULKAN_DEVICE_QUEUE_REQUEST_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_device_queue_request queue_request;
    int result = bvb_protocol_decode_vulkan_device_queue_request(
        request->payload, &queue_request);
    uint64_t queue_id = 0U;
    char diagnostic[512];
    if (result == 0) {
        result = bvb_vulkan_global_context_get_device_queue(
            context, &queue_request, &queue_id,
            diagnostic, sizeof(diagnostic));
        if (result != 0) {
            fprintf(stderr, "bvb: device queue lookup failed: %s\n",
                    diagnostic);
        }
    }
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_queue_id(
            response.payload, queue_id);
    }
    if (result == 0) {
        response.header.payload_length = BVB_VULKAN_QUEUE_ID_SIZE;
    } else {
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_queue_operation(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length != BVB_VULKAN_QUEUE_ID_SIZE ||
        (request->header.opcode != BVB_OPCODE_VULKAN_QUEUE_SUBMIT_EMPTY &&
         request->header.opcode != BVB_OPCODE_VULKAN_QUEUE_WAIT_IDLE)) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    uint64_t queue_id = 0U;
    int result = bvb_protocol_decode_vulkan_queue_id(
        request->payload, &queue_id);
    int32_t vulkan_result = 0;
    char diagnostic[512] = {0};
    if (result == 0 && request->header.opcode ==
                           BVB_OPCODE_VULKAN_QUEUE_SUBMIT_EMPTY) {
        result = bvb_vulkan_global_context_queue_submit_empty(
            context, queue_id, &vulkan_result, diagnostic, sizeof(diagnostic));
    } else if (result == 0) {
        result = bvb_vulkan_global_context_queue_wait_idle(
            context, queue_id, &vulkan_result, diagnostic, sizeof(diagnostic));
    }
    if (result != 0) {
        fprintf(stderr, "bvb: queue operation failed: %s\n", diagnostic);
    } else {
        result = bvb_protocol_encode_vulkan_result(
            response.payload, vulkan_result);
    }
    if (result == 0) {
        response.header.payload_length = BVB_VULKAN_RESULT_SIZE;
    } else {
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_device_wait_idle(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length != BVB_VULKAN_DEVICE_ID_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    uint64_t device_id = 0U;
    int result = bvb_protocol_decode_vulkan_device_id(
        request->payload, &device_id);
    int32_t vulkan_result = 0;
    char diagnostic[512] = {0};
    if (result == 0) {
        result = bvb_vulkan_global_context_device_wait_idle(
            context, device_id, &vulkan_result,
            diagnostic, sizeof(diagnostic));
    }
    if (result != 0) {
        fprintf(stderr, "bvb: device wait idle failed: %s\n", diagnostic);
    } else {
        result = bvb_protocol_encode_vulkan_result(
            response.payload, vulkan_result);
    }
    if (result == 0) {
        response.header.payload_length = BVB_VULKAN_RESULT_SIZE;
    } else {
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_command_pool_create(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length !=
            BVB_VULKAN_COMMAND_POOL_CREATE_REQUEST_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_command_pool_create_request decoded;
    int result = bvb_protocol_decode_vulkan_command_pool_create_request(
        request->payload, &decoded);
    struct bvb_vulkan_command_pool_create_response created = {0};
    char diagnostic[512] = {0};
    if (result == 0) {
        result = bvb_vulkan_global_context_create_command_pool(
            context, &decoded, &created, diagnostic, sizeof(diagnostic));
    }
    if (result != 0) {
        fprintf(stderr, "bvb: command-pool create failed: %s\n", diagnostic);
    } else {
        result = bvb_protocol_encode_vulkan_command_pool_create_response(
            response.payload, &created);
    }
    if (result == 0) {
        response.header.payload_length =
            BVB_VULKAN_COMMAND_POOL_CREATE_RESPONSE_SIZE;
    } else {
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_command_pool_destroy(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length != BVB_VULKAN_COMMAND_POOL_ID_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    uint64_t command_pool_id = 0U;
    int result = bvb_protocol_decode_vulkan_command_pool_id(
        request->payload, &command_pool_id);
    char diagnostic[512] = {0};
    if (result == 0) {
        result = bvb_vulkan_global_context_destroy_command_pool(
            context, command_pool_id, diagnostic, sizeof(diagnostic));
    }
    if (result != 0) {
        fprintf(stderr, "bvb: command-pool destroy failed: %s\n", diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_command_pool_reset(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length !=
            BVB_VULKAN_COMMAND_POOL_RESET_REQUEST_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_command_pool_reset_request decoded;
    int result = bvb_protocol_decode_vulkan_command_pool_reset_request(
        request->payload, &decoded);
    int32_t vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    char diagnostic[512] = {0};
    if (result == 0) {
        result = bvb_vulkan_global_context_reset_command_pool(
            context, &decoded, &vulkan_result,
            diagnostic, sizeof(diagnostic));
    }
    if (result != 0) {
        fprintf(stderr, "bvb: command-pool reset failed: %s\n", diagnostic);
    } else {
        result = bvb_protocol_encode_vulkan_result(
            response.payload, vulkan_result);
    }
    if (result == 0) {
        response.header.payload_length = BVB_VULKAN_RESULT_SIZE;
    } else {
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_command_buffer_allocate(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length !=
            BVB_VULKAN_COMMAND_BUFFER_ALLOCATE_REQUEST_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_command_buffer_allocate_request decoded;
    int result = bvb_protocol_decode_vulkan_command_buffer_allocate_request(
        request->payload, &decoded);
    struct bvb_vulkan_command_buffer_allocate_response allocated = {0};
    char diagnostic[512] = {0};
    if (result == 0) {
        result = bvb_vulkan_global_context_allocate_command_buffer(
            context, &decoded, &allocated, diagnostic, sizeof(diagnostic));
    }
    if (result != 0) {
        fprintf(stderr, "bvb: command-buffer allocate failed: %s\n",
                diagnostic);
    } else {
        result = bvb_protocol_encode_vulkan_command_buffer_allocate_response(
            response.payload, &allocated);
    }
    if (result == 0) {
        response.header.payload_length =
            BVB_VULKAN_COMMAND_BUFFER_ALLOCATE_RESPONSE_SIZE;
    } else {
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_command_buffer_free(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length !=
            BVB_VULKAN_COMMAND_BUFFER_FREE_REQUEST_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_command_buffer_free_request decoded;
    int result = bvb_protocol_decode_vulkan_command_buffer_free_request(
        request->payload, &decoded);
    char diagnostic[512] = {0};
    if (result == 0) {
        result = bvb_vulkan_global_context_free_command_buffer(
            context, &decoded, diagnostic, sizeof(diagnostic));
    }
    if (result != 0) {
        fprintf(stderr, "bvb: command-buffer free failed: %s\n", diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_command_buffer_begin(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length !=
            BVB_VULKAN_COMMAND_BUFFER_BEGIN_REQUEST_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_command_buffer_begin_request decoded;
    int result = bvb_protocol_decode_vulkan_command_buffer_begin_request(
        request->payload, &decoded);
    int32_t vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    char diagnostic[512] = {0};
    if (result == 0) {
        result = bvb_vulkan_global_context_begin_command_buffer(
            context, &decoded, &vulkan_result,
            diagnostic, sizeof(diagnostic));
    }
    if (result != 0) {
        fprintf(stderr, "bvb: command-buffer begin failed: %s\n", diagnostic);
    } else {
        result = bvb_protocol_encode_vulkan_result(
            response.payload, vulkan_result);
    }
    if (result == 0) {
        response.header.payload_length = BVB_VULKAN_RESULT_SIZE;
    } else {
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_command_buffer_end(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length != BVB_VULKAN_COMMAND_BUFFER_ID_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    uint64_t command_buffer_id = 0U;
    int result = bvb_protocol_decode_vulkan_command_buffer_id(
        request->payload, &command_buffer_id);
    int32_t vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    char diagnostic[512] = {0};
    if (result == 0) {
        result = bvb_vulkan_global_context_end_command_buffer(
            context, command_buffer_id, &vulkan_result,
            diagnostic, sizeof(diagnostic));
    }
    if (result != 0) {
        fprintf(stderr, "bvb: command-buffer end failed: %s\n", diagnostic);
    } else {
        result = bvb_protocol_encode_vulkan_result(
            response.payload, vulkan_result);
    }
    if (result == 0) {
        response.header.payload_length = BVB_VULKAN_RESULT_SIZE;
    } else {
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_queue_submit_command(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length !=
            BVB_VULKAN_QUEUE_SUBMIT_COMMAND_REQUEST_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_queue_submit_command_request decoded;
    int result = bvb_protocol_decode_vulkan_queue_submit_command_request(
        request->payload, &decoded);
    int32_t vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    char diagnostic[512] = {0};
    if (result == 0) {
        result = bvb_vulkan_global_context_queue_submit_command(
            context, &decoded, &vulkan_result,
            diagnostic, sizeof(diagnostic));
    }
    if (result != 0) {
        fprintf(stderr, "bvb: queue command submit failed: %s\n", diagnostic);
    } else {
        result = bvb_protocol_encode_vulkan_result(
            response.payload, vulkan_result);
    }
    if (result == 0) {
        response.header.payload_length = BVB_VULKAN_RESULT_SIZE;
    } else {
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_resource_create(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_object_create_response created = {0};
    char diagnostic[512] = {0};
    int result = -EPROTO;
    uint8_t expected_type = 0U;
    if (request->header.opcode == BVB_OPCODE_VULKAN_BUFFER_CREATE &&
        request->header.payload_length == BVB_VULKAN_BUFFER_CREATE_REQUEST_SIZE) {
        struct bvb_vulkan_buffer_create_request decoded;
        result = bvb_protocol_decode_vulkan_buffer_create_request(
            request->payload, &decoded);
        if (result == 0)
            result = bvb_vulkan_global_context_create_buffer(
                context, &decoded, &created, diagnostic, sizeof(diagnostic));
        expected_type = BVB_OBJECT_BUFFER;
    } else if (request->header.opcode == BVB_OPCODE_VULKAN_MEMORY_ALLOCATE &&
               request->header.payload_length ==
                   BVB_VULKAN_MEMORY_ALLOCATE_REQUEST_SIZE) {
        struct bvb_vulkan_memory_allocate_request decoded;
        result = bvb_protocol_decode_vulkan_memory_allocate_request(
            request->payload, &decoded);
        if (result == 0)
            result = bvb_vulkan_global_context_allocate_memory(
                context, &decoded, &created, diagnostic, sizeof(diagnostic));
        expected_type = BVB_OBJECT_DEVICE_MEMORY;
    } else if (request->header.opcode ==
                   BVB_OPCODE_VULKAN_MEMORY_ALLOCATE_EXTENDED &&
               request->header.payload_length ==
                   BVB_VULKAN_MEMORY_ALLOCATE_EXTENDED_REQUEST_SIZE) {
        struct bvb_vulkan_memory_allocate_extended_request decoded;
        result = bvb_protocol_decode_vulkan_memory_allocate_extended_request(
            request->payload, &decoded);
        if (result == 0)
            result = bvb_vulkan_global_context_allocate_memory_extended(
                context, &decoded, &created, diagnostic, sizeof(diagnostic));
        expected_type = BVB_OBJECT_DEVICE_MEMORY;
    } else if (request->header.opcode == BVB_OPCODE_VULKAN_FENCE_CREATE &&
               request->header.payload_length ==
                   BVB_VULKAN_FENCE_CREATE_REQUEST_SIZE) {
        struct bvb_vulkan_fence_create_request decoded;
        result = bvb_protocol_decode_vulkan_fence_create_request(
            request->payload, &decoded);
        if (result == 0)
            result = bvb_vulkan_global_context_create_fence(
                context, &decoded, &created, diagnostic, sizeof(diagnostic));
        expected_type = BVB_OBJECT_FENCE;
    } else if (request->header.opcode ==
                   BVB_OPCODE_VULKAN_SEMAPHORE_CREATE &&
               request->header.payload_length ==
                   BVB_VULKAN_SEMAPHORE_CREATE_REQUEST_SIZE) {
        struct bvb_vulkan_semaphore_create_request decoded;
        result = bvb_protocol_decode_vulkan_semaphore_create_request(
            request->payload, &decoded);
        if (result == 0)
            result = bvb_vulkan_global_context_create_semaphore(
                context, &decoded, &created, diagnostic, sizeof(diagnostic));
        expected_type = BVB_OBJECT_SEMAPHORE;
    } else if (request->header.opcode ==
               BVB_OPCODE_VULKAN_DESCRIPTOR_SET_LAYOUT_CREATE) {
        struct bvb_vulkan_descriptor_set_layout_create_request decoded;
        result =
            bvb_protocol_decode_vulkan_descriptor_set_layout_create_request(
                request->payload, request->header.payload_length, &decoded);
        if (result == 0) {
            result = bvb_vulkan_global_context_create_descriptor_set_layout(
                context, &decoded, &created, diagnostic, sizeof(diagnostic));
        }
        expected_type = BVB_OBJECT_DESCRIPTOR_SET_LAYOUT;
    } else if (request->header.opcode ==
               BVB_OPCODE_VULKAN_DESCRIPTOR_POOL_CREATE) {
        struct bvb_vulkan_descriptor_pool_create_request decoded;
        result = bvb_protocol_decode_vulkan_descriptor_pool_create_request(
            request->payload, request->header.payload_length, &decoded);
        if (result == 0) {
            result = bvb_vulkan_global_context_create_descriptor_pool(
                context, &decoded, &created, diagnostic, sizeof(diagnostic));
        }
        expected_type = BVB_OBJECT_DESCRIPTOR_POOL;
    } else if (request->header.opcode == BVB_OPCODE_VULKAN_SAMPLER_CREATE &&
               request->header.payload_length ==
                   BVB_VULKAN_SAMPLER_CREATE_REQUEST_SIZE) {
        struct bvb_vulkan_sampler_create_request decoded;
        result = bvb_protocol_decode_vulkan_sampler_create_request(
            request->payload, &decoded);
        if (result == 0) {
            result = bvb_vulkan_global_context_create_sampler(
                context, &decoded, &created, diagnostic, sizeof(diagnostic));
        }
        expected_type = BVB_OBJECT_SAMPLER;
    } else if (request->header.opcode ==
               BVB_OPCODE_VULKAN_PIPELINE_LAYOUT_CREATE) {
        struct bvb_vulkan_pipeline_layout_create_request decoded;
        result = bvb_protocol_decode_vulkan_pipeline_layout_create_request(
            request->payload, request->header.payload_length, &decoded);
        if (result == 0) {
            result = bvb_vulkan_global_context_create_pipeline_layout(
                context, &decoded, &created, diagnostic, sizeof(diagnostic));
        }
        expected_type = BVB_OBJECT_PIPELINE_LAYOUT;
    } else if (request->header.opcode ==
               BVB_OPCODE_VULKAN_GRAPHICS_PIPELINE_CREATE) {
        struct bvb_vulkan_graphics_pipeline_create_request decoded;
        result = bvb_protocol_decode_vulkan_graphics_pipeline_create_request(
            request->payload, request->header.payload_length, &decoded);
        if (result == 0) {
            result = bvb_vulkan_global_context_create_graphics_pipeline(
                context, &decoded, &created, diagnostic, sizeof(diagnostic));
        }
        expected_type = BVB_OBJECT_PIPELINE;
    } else if (request->header.opcode == BVB_OPCODE_VULKAN_IMAGE_CREATE &&
               request->header.payload_length ==
                   BVB_VULKAN_IMAGE_CREATE_REQUEST_SIZE) {
        struct bvb_vulkan_image_create_request decoded;
        result = bvb_protocol_decode_vulkan_image_create_request(
            request->payload, &decoded);
        if (result == 0)
            result = bvb_vulkan_global_context_create_image(
                context, &decoded, &created, diagnostic, sizeof(diagnostic));
        expected_type = BVB_OBJECT_IMAGE;
    } else if (request->header.opcode ==
                   BVB_OPCODE_VULKAN_IMAGE_VIEW_CREATE &&
               request->header.payload_length ==
                   BVB_VULKAN_IMAGE_VIEW_CREATE_REQUEST_SIZE) {
        struct bvb_vulkan_image_view_create_request decoded;
        result = bvb_protocol_decode_vulkan_image_view_create_request(
            request->payload, &decoded);
        if (result == 0)
            result = bvb_vulkan_global_context_create_image_view(
                context, &decoded, &created, diagnostic, sizeof(diagnostic));
        expected_type = BVB_OBJECT_IMAGE_VIEW;
    }
    if (result != 0) {
        fprintf(stderr, "bvb: resource create failed: %s\n", diagnostic);
    } else {
        result = bvb_protocol_encode_vulkan_object_create_response(
            response.payload, &created, expected_type);
    }
    if (result == 0)
        response.header.payload_length = BVB_VULKAN_OBJECT_CREATE_RESPONSE_SIZE;
    else
        response.header.status = result;
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_resource_destroy(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length != BVB_VULKAN_OBJECT_ID_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    const bool buffer =
        request->header.opcode == BVB_OPCODE_VULKAN_BUFFER_DESTROY;
    const bool memory = request->header.opcode == BVB_OPCODE_VULKAN_MEMORY_FREE;
    const bool fence = request->header.opcode == BVB_OPCODE_VULKAN_FENCE_DESTROY;
    const bool semaphore =
        request->header.opcode == BVB_OPCODE_VULKAN_SEMAPHORE_DESTROY;
    const bool pipeline_layout = request->header.opcode ==
        BVB_OPCODE_VULKAN_PIPELINE_LAYOUT_DESTROY;
    const bool pipeline = request->header.opcode ==
        BVB_OPCODE_VULKAN_PIPELINE_DESTROY;
    const bool descriptor_object = request->header.opcode ==
        BVB_OPCODE_VULKAN_DESCRIPTOR_OBJECT_DESTROY;
    const enum bvb_object_type descriptor_type = descriptor_object
        ? bvb_handle_type(bvb_wire_get_u64(request->payload))
        : BVB_OBJECT_INVALID;
    const bool descriptor_layout =
        descriptor_type == BVB_OBJECT_DESCRIPTOR_SET_LAYOUT;
    const bool descriptor_pool =
        descriptor_type == BVB_OBJECT_DESCRIPTOR_POOL;
    const bool sampler = descriptor_type == BVB_OBJECT_SAMPLER;
    const bool image =
        request->header.opcode == BVB_OPCODE_VULKAN_IMAGE_DESTROY;
    const bool image_view =
        request->header.opcode == BVB_OPCODE_VULKAN_IMAGE_VIEW_DESTROY;
    uint64_t object_id = 0U;
    int result = buffer || memory || fence || semaphore || pipeline_layout ||
                         pipeline || image || image_view ||
                         descriptor_layout || descriptor_pool || sampler
                     ? bvb_protocol_decode_vulkan_object_id(
                           request->payload, &object_id,
                           buffer ? BVB_OBJECT_BUFFER :
                           memory ? BVB_OBJECT_DEVICE_MEMORY :
                           fence ? BVB_OBJECT_FENCE :
                           semaphore ? BVB_OBJECT_SEMAPHORE :
                           pipeline_layout ? BVB_OBJECT_PIPELINE_LAYOUT :
                           pipeline ? BVB_OBJECT_PIPELINE :
                           image ? BVB_OBJECT_IMAGE :
                           image_view ? BVB_OBJECT_IMAGE_VIEW :
                           descriptor_layout
                               ? BVB_OBJECT_DESCRIPTOR_SET_LAYOUT
                           : descriptor_pool
                               ? BVB_OBJECT_DESCRIPTOR_POOL
                               : BVB_OBJECT_SAMPLER)
                     : -EPROTO;
    char diagnostic[512] = {0};
    if (result == 0) {
        if (buffer) {
            result = bvb_vulkan_global_context_destroy_buffer(
                context, object_id, diagnostic, sizeof(diagnostic));
        } else if (memory) {
            result = bvb_vulkan_global_context_free_memory(
                context, object_id, diagnostic, sizeof(diagnostic));
        } else if (fence) {
            result = bvb_vulkan_global_context_destroy_fence(
                context, object_id, diagnostic, sizeof(diagnostic));
        } else if (semaphore) {
            result = bvb_vulkan_global_context_destroy_semaphore(
                context, object_id, diagnostic, sizeof(diagnostic));
        } else if (pipeline_layout) {
            result = bvb_vulkan_global_context_destroy_pipeline_layout(
                context, object_id, diagnostic, sizeof(diagnostic));
        } else if (pipeline) {
            result = bvb_vulkan_global_context_destroy_pipeline(
                context, object_id, diagnostic, sizeof(diagnostic));
        } else if (image) {
            result = bvb_vulkan_global_context_destroy_image(
                context, object_id, diagnostic, sizeof(diagnostic));
        } else if (image_view) {
            result = bvb_vulkan_global_context_destroy_image_view(
                context, object_id, diagnostic, sizeof(diagnostic));
        } else if (descriptor_layout) {
            result = bvb_vulkan_global_context_destroy_descriptor_set_layout(
                context, object_id, diagnostic, sizeof(diagnostic));
        } else if (descriptor_pool) {
            result = bvb_vulkan_global_context_destroy_descriptor_pool(
                context, object_id, diagnostic, sizeof(diagnostic));
        } else {
            result = bvb_vulkan_global_context_destroy_sampler(
                context, object_id, diagnostic, sizeof(diagnostic));
        }
    }
    if (result != 0) {
        fprintf(stderr, "bvb: resource destroy failed: %s\n", diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_descriptor_set_allocate(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_descriptor_set_allocate_request decoded;
    int result = bvb_protocol_decode_vulkan_descriptor_set_allocate_request(
        request->payload, request->header.payload_length, &decoded);
    struct bvb_vulkan_descriptor_set_allocate_response allocated = {0};
    char diagnostic[512] = {0};
    if (result == 0) {
        result = bvb_vulkan_global_context_allocate_descriptor_sets(
            context, &decoded, &allocated, diagnostic, sizeof(diagnostic));
    }
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_descriptor_set_allocate_response(
            response.payload, &allocated, &response.header.payload_length);
    }
    if (result != 0) {
        fprintf(stderr, "bvb: descriptor-set allocate failed: %s\n",
                diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_descriptor_update(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_descriptor_update_request decoded;
    int result = bvb_protocol_decode_vulkan_descriptor_update_request(
        request->payload, request->header.payload_length, &decoded);
    const bool wire_decoded = result == 0;
    char diagnostic[512] = {0};
    if (result == 0) {
        result = bvb_vulkan_global_context_update_descriptors(
            context, &decoded, diagnostic, sizeof(diagnostic));
    }
    if (result != 0) {
        fprintf(stderr,
                "bvb: descriptor update failed: status=%d phase=%s %s\n",
                result, wire_decoded ? "native" : "wire", diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_buffer_requirements(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length != BVB_VULKAN_OBJECT_ID_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    uint64_t buffer_id = 0U;
    int result = bvb_protocol_decode_vulkan_object_id(
        request->payload, &buffer_id, BVB_OBJECT_BUFFER);
    struct bvb_vulkan_buffer_requirements requirements = {0};
    char diagnostic[512] = {0};
    if (result == 0)
        result = bvb_vulkan_global_context_get_buffer_requirements(
            context, buffer_id, &requirements, diagnostic, sizeof(diagnostic));
    if (result == 0)
        result = bvb_protocol_encode_vulkan_buffer_requirements(
            response.payload, &requirements);
    if (result == 0)
        response.header.payload_length = BVB_VULKAN_BUFFER_REQUIREMENTS_SIZE;
    else {
        fprintf(stderr, "bvb: buffer requirements failed: %s\n", diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_device_buffer_requirements(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length !=
            BVB_VULKAN_DEVICE_BUFFER_REQUIREMENTS_REQUEST_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_device_buffer_requirements_request decoded;
    int result = bvb_protocol_decode_vulkan_device_buffer_requirements_request(
        request->payload, &decoded);
    const bool wire_decoded = result == 0;
    struct bvb_vulkan_device_buffer_requirements_response requirements = {0};
    char diagnostic[512] = {0};
    if (result == 0)
        result = bvb_vulkan_global_context_get_device_buffer_requirements(
            context, &decoded, &requirements, diagnostic,
            sizeof(diagnostic));
    if (result == 0)
        result =
            bvb_protocol_encode_vulkan_device_buffer_requirements_response(
                response.payload, &requirements);
    if (result == 0) {
        response.header.payload_length =
            BVB_VULKAN_DEVICE_BUFFER_REQUIREMENTS_RESPONSE_SIZE;
    } else {
        fprintf(stderr,
                "bvb: device buffer requirements failed: status=%d phase=%s %s\n",
                result, wire_decoded ? "native" : "wire", diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_buffer_requirements_2(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length !=
            BVB_VULKAN_BUFFER_REQUIREMENTS_2_REQUEST_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_buffer_requirements_2_request decoded;
    int result = bvb_protocol_decode_vulkan_buffer_requirements_2_request(
        request->payload, &decoded);
    const bool wire_decoded = result == 0;
    struct bvb_vulkan_buffer_requirements_2_response requirements = {0};
    char diagnostic[512] = {0};
    if (result == 0)
        result = bvb_vulkan_global_context_get_buffer_requirements_2(
            context, &decoded, &requirements, diagnostic,
            sizeof(diagnostic));
    if (result == 0)
        result = bvb_protocol_encode_vulkan_buffer_requirements_2_response(
            response.payload, &requirements);
    if (result == 0) {
        response.header.payload_length =
            BVB_VULKAN_BUFFER_REQUIREMENTS_2_RESPONSE_SIZE;
    } else {
        fprintf(stderr,
                "bvb: buffer requirements2 failed: status=%d phase=%s %s\n",
                result, wire_decoded ? "native" : "wire", diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_buffer_device_address(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length !=
            BVB_VULKAN_BUFFER_DEVICE_ADDRESS_REQUEST_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_buffer_device_address_request decoded;
    int result = bvb_protocol_decode_vulkan_buffer_device_address_request(
        request->payload, &decoded);
    const bool wire_decoded = result == 0;
    struct bvb_vulkan_buffer_device_address_response address = {0};
    char diagnostic[512] = {0};
    if (result == 0)
        result = bvb_vulkan_global_context_get_buffer_device_address(
            context, &decoded, &address, diagnostic, sizeof(diagnostic));
    if (result == 0)
        result = bvb_protocol_encode_vulkan_buffer_device_address_response(
            response.payload, &address);
    if (result == 0) {
        response.header.payload_length =
            BVB_VULKAN_BUFFER_DEVICE_ADDRESS_RESPONSE_SIZE;
    } else {
        fprintf(stderr,
                "bvb: buffer device address failed: status=%d phase=%s %s\n",
                result, wire_decoded ? "native" : "wire", diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_buffer_bind(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length != BVB_VULKAN_BUFFER_BIND_REQUEST_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_buffer_bind_request decoded;
    int result = bvb_protocol_decode_vulkan_buffer_bind_request(
        request->payload, &decoded);
    int32_t vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    char diagnostic[512] = {0};
    if (result == 0)
        result = bvb_vulkan_global_context_bind_buffer_memory(
            context, &decoded, &vulkan_result, diagnostic, sizeof(diagnostic));
    if (result == 0)
        result = bvb_protocol_encode_vulkan_result(
            response.payload, vulkan_result);
    if (result == 0)
        response.header.payload_length = BVB_VULKAN_RESULT_SIZE;
    else {
        fprintf(stderr, "bvb: buffer bind failed: %s\n", diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_image_requirements(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length != BVB_VULKAN_OBJECT_ID_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    uint64_t image_id = 0U;
    int result = bvb_protocol_decode_vulkan_object_id(
        request->payload, &image_id, BVB_OBJECT_IMAGE);
    struct bvb_vulkan_image_requirements requirements = {0};
    char diagnostic[512] = {0};
    if (result == 0)
        result = bvb_vulkan_global_context_get_image_requirements(
            context, image_id, &requirements, diagnostic, sizeof(diagnostic));
    if (result == 0)
        result = bvb_protocol_encode_vulkan_image_requirements(
            response.payload, &requirements);
    if (result == 0)
        response.header.payload_length = BVB_VULKAN_IMAGE_REQUIREMENTS_SIZE;
    else {
        fprintf(stderr, "bvb: image requirements failed: %s\n", diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_image_requirements_2(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length !=
            BVB_VULKAN_IMAGE_REQUIREMENTS_2_REQUEST_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_image_requirements_2_request decoded;
    int result = bvb_protocol_decode_vulkan_image_requirements_2_request(
        request->payload, &decoded);
    struct bvb_vulkan_image_requirements_2_response requirements = {0};
    char diagnostic[512] = {0};
    if (result == 0)
        result = bvb_vulkan_global_context_get_image_requirements_2(
            context, &decoded, &requirements, diagnostic, sizeof(diagnostic));
    if (result == 0)
        result = bvb_protocol_encode_vulkan_image_requirements_2_response(
            response.payload, &requirements);
    if (result == 0)
        response.header.payload_length =
            BVB_VULKAN_IMAGE_REQUIREMENTS_2_RESPONSE_SIZE;
    else {
        fprintf(stderr, "bvb: image requirements2 failed: %s\n", diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_image_bind(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length != BVB_VULKAN_IMAGE_BIND_REQUEST_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_image_bind_request decoded;
    int result = bvb_protocol_decode_vulkan_image_bind_request(
        request->payload, &decoded);
    int32_t vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    char diagnostic[512] = {0};
    if (result == 0)
        result = bvb_vulkan_global_context_bind_image_memory(
            context, &decoded, &vulkan_result, diagnostic, sizeof(diagnostic));
    if (result == 0)
        result = bvb_protocol_encode_vulkan_result(
            response.payload, vulkan_result);
    if (result == 0)
        response.header.payload_length = BVB_VULKAN_RESULT_SIZE;
    else {
        fprintf(stderr, "bvb: image bind failed: %s\n", diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_command_buffer_fill(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length !=
            BVB_VULKAN_COMMAND_BUFFER_FILL_REQUEST_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_command_buffer_fill_request decoded;
    int result = bvb_protocol_decode_vulkan_command_buffer_fill_request(
        request->payload, &decoded);
    char diagnostic[512] = {0};
    if (result == 0)
        result = bvb_vulkan_global_context_command_buffer_fill(
            context, &decoded, diagnostic, sizeof(diagnostic));
    if (result != 0) {
        fprintf(stderr, "bvb: command-buffer fill failed: %s\n", diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_command_buffer_image_barrier(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length !=
            BVB_VULKAN_COMMAND_BUFFER_IMAGE_BARRIER_REQUEST_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_command_buffer_image_barrier_request decoded;
    int result = bvb_protocol_decode_vulkan_command_buffer_image_barrier_request(
        request->payload, &decoded);
    char diagnostic[512] = {0};
    if (result == 0)
        result = bvb_vulkan_global_context_command_buffer_image_barrier(
            context, &decoded, diagnostic, sizeof(diagnostic));
    if (result != 0) {
        fprintf(stderr, "bvb: command-buffer image barrier failed: %s\n",
                diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_command_buffer_clear_color_image(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length !=
            BVB_VULKAN_COMMAND_BUFFER_CLEAR_COLOR_IMAGE_REQUEST_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_command_buffer_clear_color_image_request decoded;
    int result =
        bvb_protocol_decode_vulkan_command_buffer_clear_color_image_request(
            request->payload, &decoded);
    char diagnostic[512] = {0};
    if (result == 0)
        result = bvb_vulkan_global_context_command_buffer_clear_color_image(
            context, &decoded, diagnostic, sizeof(diagnostic));
    if (result != 0) {
        fprintf(stderr, "bvb: command-buffer clear color image failed: %s\n",
                diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_memory_verify_fill(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length !=
            BVB_VULKAN_MEMORY_VERIFY_FILL_REQUEST_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_memory_verify_fill_request decoded;
    int result = bvb_protocol_decode_vulkan_memory_verify_fill_request(
        request->payload, &decoded);
    struct bvb_vulkan_memory_verify_fill_response verified = {0};
    char diagnostic[512] = {0};
    if (result == 0)
        result = bvb_vulkan_global_context_verify_memory_fill(
            context, &decoded, &verified, diagnostic, sizeof(diagnostic));
    if (result == 0)
        result = bvb_protocol_encode_vulkan_memory_verify_fill_response(
            response.payload, &verified);
    if (result == 0)
        response.header.payload_length =
            BVB_VULKAN_MEMORY_VERIFY_FILL_RESPONSE_SIZE;
    else {
        fprintf(stderr, "bvb: memory verification failed: %s\n", diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_memory_write(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_memory_io_request decoded;
    const uint8_t *data = NULL;
    int result = bvb_protocol_decode_vulkan_memory_write_request(
        request->payload, request->header.payload_length, &decoded, &data);
    int32_t vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    char diagnostic[512] = {0};
    if (result == 0) {
        result = bvb_vulkan_global_context_write_memory(
            context, &decoded, data, &vulkan_result, diagnostic,
            sizeof(diagnostic));
    }
    const struct bvb_vulkan_memory_io_response written = {
        .vulkan_result = vulkan_result,
    };
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_memory_io_response(
            response.payload, &written, NULL,
            &response.header.payload_length);
    }
    if (result != 0) {
        fprintf(stderr, "bvb: memory write failed: %s\n", diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_memory_read(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length != BVB_VULKAN_MEMORY_IO_PREFIX_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_memory_io_request decoded;
    int result = bvb_protocol_decode_vulkan_memory_read_request(
        request->payload, &decoded);
    uint8_t data[BVB_VULKAN_MEMORY_IO_MAX_BYTES];
    uint32_t length = 0U;
    int32_t vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    char diagnostic[512] = {0};
    if (result == 0) {
        result = bvb_vulkan_global_context_read_memory(
            context, &decoded, data, sizeof(data), &length, &vulkan_result,
            diagnostic, sizeof(diagnostic));
    }
    const struct bvb_vulkan_memory_io_response read = {
        .vulkan_result = vulkan_result,
        .length = vulkan_result == VK_SUCCESS ? length : 0U,
    };
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_memory_io_response(
            response.payload, &read, length == 0U ? NULL : data,
            &response.header.payload_length);
    }
    if (result != 0) {
        fprintf(stderr, "bvb: memory read failed: %s\n", diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_fence_operation(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length != BVB_VULKAN_OBJECT_ID_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    uint64_t fence_id = 0U;
    int result = bvb_protocol_decode_vulkan_object_id(
        request->payload, &fence_id, BVB_OBJECT_FENCE);
    int32_t vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    char diagnostic[512] = {0};
    if (result == 0 && request->header.opcode == BVB_OPCODE_VULKAN_FENCE_STATUS)
        result = bvb_vulkan_global_context_get_fence_status(
            context, fence_id, &vulkan_result, diagnostic, sizeof(diagnostic));
    else if (result == 0 &&
             request->header.opcode == BVB_OPCODE_VULKAN_FENCE_RESET)
        result = bvb_vulkan_global_context_reset_fence(
            context, fence_id, &vulkan_result, diagnostic, sizeof(diagnostic));
    else if (result == 0)
        result = -EPROTO;
    if (result == 0)
        result = bvb_protocol_encode_vulkan_result(
            response.payload, vulkan_result);
    if (result == 0)
        response.header.payload_length = BVB_VULKAN_RESULT_SIZE;
    else {
        fprintf(stderr, "bvb: fence operation failed: %s\n", diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_fence_wait(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length != BVB_VULKAN_FENCE_WAIT_REQUEST_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_fence_wait_request decoded;
    int result = bvb_protocol_decode_vulkan_fence_wait_request(
        request->payload, &decoded);
    int32_t vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    char diagnostic[512] = {0};
    if (result == 0)
        result = bvb_vulkan_global_context_wait_fence(
            context, &decoded, &vulkan_result, diagnostic, sizeof(diagnostic));
    if (result == 0)
        result = bvb_protocol_encode_vulkan_result(
            response.payload, vulkan_result);
    if (result == 0)
        response.header.payload_length = BVB_VULKAN_RESULT_SIZE;
    else {
        fprintf(stderr, "bvb: fence wait failed: %s\n", diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_semaphore_counter(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length != BVB_VULKAN_OBJECT_ID_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    uint64_t semaphore_id = 0U;
    int result = bvb_protocol_decode_vulkan_object_id(
        request->payload, &semaphore_id, BVB_OBJECT_SEMAPHORE);
    struct bvb_vulkan_semaphore_counter_response counter = {
        .vulkan_result = VK_ERROR_INITIALIZATION_FAILED,
    };
    char diagnostic[512] = {0};
    if (result == 0)
        result = bvb_vulkan_global_context_get_semaphore_counter(
            context, semaphore_id, &counter, diagnostic, sizeof(diagnostic));
    if (result == 0)
        result = bvb_protocol_encode_vulkan_semaphore_counter_response(
            response.payload, &counter);
    if (result == 0)
        response.header.payload_length =
            BVB_VULKAN_SEMAPHORE_COUNTER_RESPONSE_SIZE;
    else {
        fprintf(stderr, "bvb: semaphore counter failed: %s\n", diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_semaphore_wait(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_semaphore_wait_request decoded;
    int result = bvb_protocol_decode_vulkan_semaphore_wait_request(
        request->payload, request->header.payload_length, &decoded);
    int32_t vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    char diagnostic[512] = {0};
    if (result == 0)
        result = bvb_vulkan_global_context_wait_semaphores(
            context, &decoded, &vulkan_result, diagnostic, sizeof(diagnostic));
    if (result == 0)
        result = bvb_protocol_encode_vulkan_result(
            response.payload, vulkan_result);
    if (result == 0)
        response.header.payload_length = BVB_VULKAN_RESULT_SIZE;
    else {
        fprintf(stderr, "bvb: semaphore wait failed: %s\n", diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_semaphore_signal(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL || request->header.payload_length !=
            BVB_VULKAN_SEMAPHORE_SIGNAL_REQUEST_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_semaphore_signal_request decoded;
    int result = bvb_protocol_decode_vulkan_semaphore_signal_request(
        request->payload, &decoded);
    int32_t vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    char diagnostic[512] = {0};
    if (result == 0)
        result = bvb_vulkan_global_context_signal_semaphore(
            context, &decoded, &vulkan_result, diagnostic, sizeof(diagnostic));
    if (result == 0)
        result = bvb_protocol_encode_vulkan_result(
            response.payload, vulkan_result);
    if (result == 0)
        response.header.payload_length = BVB_VULKAN_RESULT_SIZE;
    else {
        fprintf(stderr, "bvb: semaphore signal failed: %s\n", diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_queue_submit_command_fence(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length !=
            BVB_VULKAN_QUEUE_SUBMIT_COMMAND_FENCE_REQUEST_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_queue_submit_command_fence_request decoded;
    int result = bvb_protocol_decode_vulkan_queue_submit_command_fence_request(
        request->payload, &decoded);
    int32_t vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    char diagnostic[512] = {0};
    if (result == 0)
        result = bvb_vulkan_global_context_queue_submit_command_fence(
            context, &decoded, &vulkan_result, diagnostic, sizeof(diagnostic));
    if (result == 0)
        result = bvb_protocol_encode_vulkan_result(
            response.payload, vulkan_result);
    if (result == 0)
        response.header.payload_length = BVB_VULKAN_RESULT_SIZE;
    else {
        fprintf(stderr, "bvb: fenced queue submit failed: %s\n", diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_queue_submit_2(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_queue_submit_2_request decoded;
    int result = bvb_protocol_decode_vulkan_queue_submit_2_request(
        request->payload, request->header.payload_length, &decoded);
    int32_t vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    char diagnostic[512] = {0};
    if (result == 0)
        result = bvb_vulkan_global_context_queue_submit_2(
            context, &decoded, &vulkan_result, diagnostic, sizeof(diagnostic));
    if (result == 0)
        result = bvb_protocol_encode_vulkan_result(
            response.payload, vulkan_result);
    if (result == 0)
        response.header.payload_length = BVB_VULKAN_RESULT_SIZE;
    else {
        fprintf(stderr, "bvb: queue submit2 failed: %s\n", diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_swapchain_prepare(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context,
    const struct bvb_activity_status *activity_status,
    const char *activity_frame_socket) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    const uint32_t required_activity =
        BVB_ACTIVITY_RESUMED | BVB_ACTIVITY_WINDOW_PRESENT |
        BVB_ACTIVITY_RENDERER_READY;
    if (!negotiated || context == NULL ||
        request->header.payload_length !=
            BVB_VULKAN_SWAPCHAIN_PREPARE_REQUEST_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    if (activity_frame_socket == NULL) {
        response.header.status = -ENOTCONN;
        return bvb_transport_send(client_fd, &response);
    }
    if (activity_status == NULL ||
        activity_status->ingress_configured == 0U ||
        (activity_status->state_flags & required_activity) !=
            required_activity ||
        (activity_status->state_flags & BVB_ACTIVITY_DESTROYED) != 0U) {
        response.header.status = -EAGAIN;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_swapchain_prepare_request decoded;
    int result = bvb_protocol_decode_vulkan_swapchain_prepare_request(
        request->payload, &decoded);
    if (result == 0 &&
        (decoded.width != activity_status->width ||
         decoded.height != activity_status->height)) {
        result = -ERANGE;
    }
    struct bvb_vulkan_swapchain_prepare_response prepared = {0};
    int descriptors[BVB_WSI_FRAME_RING_MAX_SLOTS + 1U];
    size_t descriptor_count = 0U;
    char diagnostic[512] = {0};
    if (result == 0) {
        result = bvb_vulkan_global_context_prepare_swapchain(
            context, &decoded, &prepared, descriptors, &descriptor_count,
            diagnostic, sizeof(diagnostic));
    }
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_swapchain_prepare_response(
            response.payload, &prepared);
    }
    if (result == 0) {
        response.header.payload_length =
            BVB_VULKAN_SWAPCHAIN_PREPARE_RESPONSE_SIZE;
    } else {
        fprintf(stderr, "bvb: swapchain preparation failed: %s\n",
                diagnostic);
        response.header.status = result;
    }
    if (result == 0 && prepared.vulkan_result == VK_SUCCESS) {
        struct bvb_activity_frame_setup setup = {
            .magic = BVB_ACTIVITY_FRAME_SETUP_MAGIC,
            .version = BVB_ACTIVITY_FRAME_SETUP_VERSION,
            .header_bytes = BVB_ACTIVITY_FRAME_SETUP_BYTES,
            .image_count = prepared.image_count,
            .width = decoded.width,
            .height = decoded.height,
            .format = decoded.format,
            .image_usage = decoded.image_usage,
            .generation = prepared.generation,
        };
        for (uint32_t index = 0U; index < prepared.image_count; ++index) {
            setup.allocation_sizes[index] =
                prepared.images[index].allocation_size;
            setup.memory_type_indices[index] =
                prepared.images[index].memory_type_index;
        }
        result = bvb_activity_frame_setup_send(
            activity_frame_socket, &setup, descriptors, descriptor_count);
        if (result != 0) {
            char destroy_diagnostic[256] = {0};
            (void)bvb_vulkan_global_context_destroy_swapchain(
                context, prepared.swapchain_id, destroy_diagnostic,
                sizeof(destroy_diagnostic));
            response.header.status = result;
            response.header.payload_length = 0U;
            fprintf(stderr,
                    "bvb: Activity frame setup relay failed: %s\n",
                    strerror(-result));
        }
    }
    int send_result = 0;
    if (result == 0 && prepared.vulkan_result == VK_SUCCESS) {
        if (descriptor_count != (size_t)prepared.image_count + 1U) {
            send_result = -EPROTO;
        } else {
            send_result = bvb_transport_send_fds(
                client_fd, &response, descriptors, descriptor_count);
        }
    } else {
        send_result = bvb_transport_send(client_fd, &response);
    }
    for (size_t index = 0U; index < descriptor_count; ++index) {
        if (descriptors[index] >= 0) (void)close(descriptors[index]);
    }
    return send_result;
}

static int answer_vulkan_swapchain_destroy(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length != BVB_VULKAN_OBJECT_ID_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    uint64_t swapchain_id = 0U;
    int result = bvb_protocol_decode_vulkan_object_id(
        request->payload, &swapchain_id, BVB_OBJECT_SWAPCHAIN);
    char diagnostic[512] = {0};
    if (result == 0) {
        result = bvb_vulkan_global_context_destroy_swapchain(
            context, swapchain_id, diagnostic, sizeof(diagnostic));
    }
    if (result != 0) {
        fprintf(stderr, "bvb: swapchain destroy failed: %s\n", diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_swapchain_acquire(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length !=
            BVB_VULKAN_SWAPCHAIN_ACQUIRE_REQUEST_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_swapchain_acquire_request decoded;
    int result = bvb_protocol_decode_vulkan_swapchain_acquire_request(
        request->payload, &decoded);
    struct bvb_vulkan_swapchain_acquire_response acquired = {0};
    char diagnostic[512] = {0};
    if (result == 0)
        result = bvb_vulkan_global_context_acquire_swapchain_image(
            context, &decoded, &acquired, diagnostic, sizeof(diagnostic));
    if (result == 0)
        result = bvb_protocol_encode_vulkan_swapchain_acquire_response(
            response.payload, &acquired);
    if (result == 0)
        response.header.payload_length =
            BVB_VULKAN_SWAPCHAIN_ACQUIRE_RESPONSE_SIZE;
    else {
        fprintf(stderr, "bvb: swapchain acquire failed: %s\n", diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_swapchain_present(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    struct bvb_vulkan_global_context *context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || context == NULL ||
        request->header.payload_length <
            BVB_VULKAN_SWAPCHAIN_PRESENT_PREFIX_SIZE ||
        request->header.payload_length >
            BVB_VULKAN_SWAPCHAIN_PRESENT_MAX_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_vulkan_swapchain_present_request decoded;
    int result = bvb_protocol_decode_vulkan_swapchain_present_request(
        request->payload, request->header.payload_length, &decoded);
    struct bvb_vulkan_swapchain_present_response presented = {0};
    char diagnostic[512] = {0};
    if (result == 0)
        result = bvb_vulkan_global_context_present_swapchain_image(
            context, &decoded, &presented, diagnostic, sizeof(diagnostic));
    if (result == 0)
        result = bvb_protocol_encode_vulkan_swapchain_present_response(
            response.payload, &presented);
    if (result == 0)
        response.header.payload_length =
            BVB_VULKAN_SWAPCHAIN_PRESENT_RESPONSE_SIZE;
    else {
        fprintf(stderr, "bvb: swapchain present failed: %s\n", diagnostic);
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_selftest(int client_fd,
                                  const struct bvb_protocol_packet *request,
                                  const char *loader_path,
                                  bool negotiated) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || request->header.payload_length != 0U) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }

    struct bvb_vulkan_selftest_result selftest;
    char diagnostic[512];
    int result = bvb_vulkan_run_selftest(loader_path, &selftest, diagnostic,
                                         sizeof(diagnostic));
    if (result != 0) {
        fprintf(stderr, "bvb: Vulkan self-test failed: %s\n", diagnostic);
        response.header.status = result;
        return bvb_transport_send(client_fd, &response);
    }
    result = bvb_protocol_encode_vulkan_selftest(response.payload, &selftest);
    if (result != 0) {
        return result;
    }
    response.header.payload_length = BVB_VULKAN_SELFTEST_SIZE;
    return bvb_transport_send(client_fd, &response);
}

static int answer_vulkan_batch_selftest(
    int client_fd, const struct bvb_protocol_packet *request,
    const char *loader_path, bool negotiated) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || request->header.payload_length == 0U) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }

    struct bvb_vulkan_selftest_result selftest;
    char diagnostic[512];
    int result = bvb_vulkan_run_batched_selftest(
        loader_path, request->payload, request->header.payload_length,
        &selftest, diagnostic, sizeof(diagnostic));
    if (result != 0) {
        fprintf(stderr, "bvb: Vulkan batch self-test failed: %s\n",
                diagnostic);
        response.header.status = result;
        return bvb_transport_send(client_fd, &response);
    }
    result = bvb_protocol_encode_vulkan_selftest(response.payload, &selftest);
    if (result != 0) {
        return result;
    }
    response.header.payload_length = BVB_VULKAN_SELFTEST_SIZE;
    return bvb_transport_send(client_fd, &response);
}

static int answer_shared_batch_setup(
    int client_fd, const struct bvb_protocol_packet *request, bool negotiated,
    int received_fd, struct shared_batch_region *region) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    int status = 0;
    struct bvb_shared_batch_setup setup;
    if (!negotiated || received_fd < 0 || region == NULL ||
        region->address != NULL ||
        request->header.payload_length != BVB_SHARED_BATCH_SETUP_SIZE) {
        status = -EPROTO;
    } else {
        status = bvb_protocol_decode_shared_batch_setup(request->payload,
                                                        &setup);
    }
    struct stat metadata;
    if (status == 0 &&
        (fstat(received_fd, &metadata) != 0 ||
         !S_ISREG(metadata.st_mode) || metadata.st_size < 0 ||
         (uint64_t)metadata.st_size != setup.region_bytes)) {
        status = -EINVAL;
    }
    long page_size = sysconf(_SC_PAGESIZE);
    if (status == 0 &&
        (page_size <= 0 || setup.region_bytes % (uint32_t)page_size != 0U)) {
        status = -EINVAL;
    }
    void *mapping = MAP_FAILED;
    if (status == 0) {
        mapping = mmap(NULL, setup.region_bytes, PROT_READ, MAP_SHARED,
                       received_fd, 0);
        if (mapping == MAP_FAILED) {
            status = -errno;
        }
    }
    if (received_fd >= 0) {
        (void)close(received_fd);
    }
    if (status == 0) {
        *region = (struct shared_batch_region){
            .address = mapping,
            .length = setup.region_bytes,
            .generation = setup.generation,
        };
    }
    response.header.status = status;
    return bvb_transport_send(client_fd, &response);
}

static int answer_shared_batch_execute(
    int client_fd, const struct bvb_protocol_packet *request,
    const char *loader_path, bool negotiated, struct shared_batch_region *region,
    struct bvb_vulkan_batch_context **context) {
    struct bvb_protocol_packet response;
    prepare_response(&response, request);
    if (!negotiated || region == NULL || region->address == NULL ||
        context == NULL ||
        request->header.payload_length != BVB_SHARED_BATCH_EXECUTE_SIZE) {
        response.header.status = -EPROTO;
        return bvb_transport_send(client_fd, &response);
    }
    struct bvb_shared_batch_execute execute;
    int result = bvb_protocol_decode_shared_batch_execute(request->payload,
                                                           &execute);
    if (result == 0 && execute.generation != region->generation) {
        result = -ESTALE;
    }
    if (result == 0 && execute.sequence <= region->last_sequence) {
        result = -ESTALE;
    }
    if (result == 0 &&
        (execute.offset > region->length ||
         execute.length > region->length - execute.offset)) {
        result = -ERANGE;
    }
    const uint8_t *batch = NULL;
    struct bvb_command_batch_info batch_info;
    if (result == 0) {
        atomic_thread_fence(memory_order_acquire);
        batch = region->address + execute.offset;
        result = bvb_command_batch_validate(batch, execute.length, &batch_info);
    }
    if (result == 0 && batch_info.sequence != execute.sequence) {
        result = -ESTALE;
    }
    struct bvb_vulkan_selftest_result selftest;
    char diagnostic[512];
    if (result == 0 && *context == NULL) {
        result = bvb_vulkan_batch_context_create(
            loader_path, context, diagnostic, sizeof(diagnostic));
        if (result != 0) {
            fprintf(stderr, "bvb: persistent Vulkan context failed: %s\n",
                    diagnostic);
        }
    }
    if (result == 0) {
        result = bvb_vulkan_batch_context_execute(
            *context, batch, execute.length, &selftest, diagnostic,
            sizeof(diagnostic));
        if (result != 0) {
            fprintf(stderr, "bvb: shared Vulkan batch self-test failed: %s\n",
                    diagnostic);
        }
    }
    if (result == 0) {
        region->last_sequence = execute.sequence;
        result = bvb_protocol_encode_vulkan_selftest(response.payload,
                                                     &selftest);
        if (result == 0) {
            response.header.payload_length = BVB_VULKAN_SELFTEST_SIZE;
        }
    }
    if (result != 0) {
        response.header.status = result;
    }
    return bvb_transport_send(client_fd, &response);
}

static int serve_connection(int client_fd, const char *loader_path,
                            bool activity_ingress,
                            const struct bvb_activity_status *activity_status,
                            const char *activity_frame_socket) {
    bool negotiated = false;
    struct shared_batch_region shared_region = {0};
    struct bvb_vulkan_batch_context *vulkan_context = NULL;
    struct bvb_vulkan_global_context *global_context = NULL;
    int connection_status = 0;
    while (true) {
        struct bvb_protocol_packet request;
        memset(&request, 0, sizeof(request));
        int received_fd = -1;
        int result = bvb_transport_receive_fd(client_fd, &request,
                                              &received_fd);
        if (result == BVB_TRANSPORT_EOF) {
            break;
        }
        if (result != 0) {
            connection_status = result;
            break;
        }
        if (request.header.kind != BVB_PROTOCOL_REQUEST ||
            (received_fd >= 0 && request.header.opcode !=
                                     BVB_OPCODE_SHARED_BATCH_SETUP)) {
            if (received_fd >= 0) {
                (void)close(received_fd);
            }
            connection_status = -EPROTO;
            break;
        }
        if (request.header.opcode == BVB_OPCODE_HELLO) {
            result = answer_hello(client_fd, &request, loader_path,
                                  activity_ingress,
                                  &negotiated);
        } else if (request.header.opcode == BVB_OPCODE_VULKAN_CAPS) {
            result = answer_vulkan_caps(client_fd, &request, loader_path,
                                        negotiated);
        } else if (request.header.opcode == BVB_OPCODE_VULKAN_SELFTEST) {
            result = answer_vulkan_selftest(client_fd, &request, loader_path,
                                            negotiated);
        } else if (request.header.opcode == BVB_OPCODE_ACTIVITY_STATUS) {
            result = answer_activity_status(client_fd, &request, negotiated,
                                            activity_status);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_BATCH_SELFTEST) {
            result = answer_vulkan_batch_selftest(
                client_fd, &request, loader_path, negotiated);
        } else if (request.header.opcode == BVB_OPCODE_SHARED_BATCH_SETUP) {
            result = answer_shared_batch_setup(client_fd, &request, negotiated,
                                               received_fd, &shared_region);
            received_fd = -1;
        } else if (request.header.opcode == BVB_OPCODE_SHARED_BATCH_EXECUTE) {
            result = answer_shared_batch_execute(
                client_fd, &request, loader_path, negotiated, &shared_region,
                &vulkan_context);
        } else if (request.header.opcode == BVB_OPCODE_VULKAN_GLOBAL_INFO) {
            result = answer_vulkan_global_info(
                client_fd, &request, loader_path, negotiated, &global_context);
        } else if (request.header.opcode ==
                       BVB_OPCODE_VULKAN_INSTANCE_CREATE ||
                   request.header.opcode ==
                       BVB_OPCODE_VULKAN_INSTANCE_CREATE_EXTENDED) {
            result = answer_vulkan_instance_create(
                client_fd, &request, loader_path, negotiated, &global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_INSTANCE_EXTENSIONS) {
            result = answer_vulkan_instance_extensions(
                client_fd, &request, loader_path, negotiated, &global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_INSTANCE_DESTROY) {
            result = answer_vulkan_instance_destroy(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_PHYSICAL_DEVICES) {
            result = answer_vulkan_physical_devices(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_PHYSICAL_DEVICE_PROPERTIES) {
            result = answer_vulkan_physical_device_properties(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_QUEUE_FAMILY_PROPERTIES) {
            result = answer_vulkan_queue_family_properties(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_MEMORY_PROPERTIES) {
            result = answer_vulkan_memory_properties(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_DEVICE_EXTENSIONS) {
            result = answer_vulkan_device_extensions(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_PHYSICAL_DEVICE_FEATURES) {
            result = answer_vulkan_physical_device_features(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_CORE_FEATURES) {
            result = answer_vulkan_core_features(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_FORMAT_PROPERTIES) {
            result = answer_vulkan_format_properties(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_IMAGE_FORMAT_PROPERTIES) {
            result = answer_vulkan_image_format_properties(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_EXTERNAL_BUFFER_PROPERTIES) {
            result = answer_vulkan_external_buffer_properties(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_EXTERNAL_SEMAPHORE_PROPERTIES) {
            result = answer_vulkan_external_semaphore_properties(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                       BVB_OPCODE_VULKAN_DEVICE_CREATE ||
                   request.header.opcode ==
                       BVB_OPCODE_VULKAN_DEVICE_CREATE_EXTENDED ||
                   request.header.opcode ==
                       BVB_OPCODE_VULKAN_DEVICE_CREATE_PACKED) {
            result = answer_vulkan_device_create(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_DEVICE_DESTROY) {
            result = answer_vulkan_device_destroy(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_DEVICE_QUEUE) {
            result = answer_vulkan_device_queue(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                       BVB_OPCODE_VULKAN_QUEUE_SUBMIT_EMPTY ||
                   request.header.opcode ==
                       BVB_OPCODE_VULKAN_QUEUE_WAIT_IDLE) {
            result = answer_vulkan_queue_operation(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_DEVICE_WAIT_IDLE) {
            result = answer_vulkan_device_wait_idle(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_COMMAND_POOL_CREATE) {
            result = answer_vulkan_command_pool_create(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_COMMAND_POOL_DESTROY) {
            result = answer_vulkan_command_pool_destroy(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_COMMAND_POOL_RESET) {
            result = answer_vulkan_command_pool_reset(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_COMMAND_BUFFER_ALLOCATE) {
            result = answer_vulkan_command_buffer_allocate(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_COMMAND_BUFFER_FREE) {
            result = answer_vulkan_command_buffer_free(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_COMMAND_BUFFER_BEGIN) {
            result = answer_vulkan_command_buffer_begin(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_COMMAND_BUFFER_END) {
            result = answer_vulkan_command_buffer_end(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_QUEUE_SUBMIT_COMMAND) {
            result = answer_vulkan_queue_submit_command(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode == BVB_OPCODE_VULKAN_BUFFER_CREATE ||
                   request.header.opcode == BVB_OPCODE_VULKAN_MEMORY_ALLOCATE ||
                   request.header.opcode ==
                       BVB_OPCODE_VULKAN_MEMORY_ALLOCATE_EXTENDED ||
                   request.header.opcode == BVB_OPCODE_VULKAN_FENCE_CREATE ||
                   request.header.opcode ==
                       BVB_OPCODE_VULKAN_SEMAPHORE_CREATE ||
                   request.header.opcode ==
                       BVB_OPCODE_VULKAN_DESCRIPTOR_SET_LAYOUT_CREATE ||
                   request.header.opcode ==
                       BVB_OPCODE_VULKAN_DESCRIPTOR_POOL_CREATE ||
                   request.header.opcode ==
                       BVB_OPCODE_VULKAN_SAMPLER_CREATE ||
                   request.header.opcode ==
                       BVB_OPCODE_VULKAN_PIPELINE_LAYOUT_CREATE ||
                   request.header.opcode ==
                       BVB_OPCODE_VULKAN_GRAPHICS_PIPELINE_CREATE ||
                   request.header.opcode == BVB_OPCODE_VULKAN_IMAGE_CREATE ||
                   request.header.opcode ==
                       BVB_OPCODE_VULKAN_IMAGE_VIEW_CREATE) {
            result = answer_vulkan_resource_create(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode == BVB_OPCODE_VULKAN_BUFFER_DESTROY ||
                   request.header.opcode == BVB_OPCODE_VULKAN_MEMORY_FREE ||
                   request.header.opcode == BVB_OPCODE_VULKAN_FENCE_DESTROY ||
                   request.header.opcode ==
                       BVB_OPCODE_VULKAN_SEMAPHORE_DESTROY ||
                   request.header.opcode ==
                       BVB_OPCODE_VULKAN_DESCRIPTOR_OBJECT_DESTROY ||
                   request.header.opcode ==
                       BVB_OPCODE_VULKAN_PIPELINE_LAYOUT_DESTROY ||
                   request.header.opcode ==
                       BVB_OPCODE_VULKAN_PIPELINE_DESTROY ||
                   request.header.opcode == BVB_OPCODE_VULKAN_IMAGE_DESTROY ||
                   request.header.opcode ==
                       BVB_OPCODE_VULKAN_IMAGE_VIEW_DESTROY) {
            result = answer_vulkan_resource_destroy(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_DESCRIPTOR_SET_ALLOCATE) {
            result = answer_vulkan_descriptor_set_allocate(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_DESCRIPTOR_UPDATE) {
            result = answer_vulkan_descriptor_update(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_BUFFER_REQUIREMENTS) {
            result = answer_vulkan_buffer_requirements(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_DEVICE_BUFFER_REQUIREMENTS) {
            result = answer_vulkan_device_buffer_requirements(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_BUFFER_REQUIREMENTS_2) {
            result = answer_vulkan_buffer_requirements_2(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_BUFFER_DEVICE_ADDRESS) {
            result = answer_vulkan_buffer_device_address(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode == BVB_OPCODE_VULKAN_BUFFER_BIND) {
            result = answer_vulkan_buffer_bind(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_IMAGE_REQUIREMENTS) {
            result = answer_vulkan_image_requirements(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_IMAGE_REQUIREMENTS_2) {
            result = answer_vulkan_image_requirements_2(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode == BVB_OPCODE_VULKAN_IMAGE_BIND) {
            result = answer_vulkan_image_bind(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_COMMAND_BUFFER_FILL) {
            result = answer_vulkan_command_buffer_fill(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_COMMAND_BUFFER_IMAGE_BARRIER) {
            result = answer_vulkan_command_buffer_image_barrier(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_COMMAND_BUFFER_CLEAR_COLOR_IMAGE) {
            result = answer_vulkan_command_buffer_clear_color_image(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_MEMORY_VERIFY_FILL) {
            result = answer_vulkan_memory_verify_fill(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_MEMORY_WRITE) {
            result = answer_vulkan_memory_write(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_MEMORY_READ) {
            result = answer_vulkan_memory_read(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode == BVB_OPCODE_VULKAN_FENCE_STATUS ||
                   request.header.opcode == BVB_OPCODE_VULKAN_FENCE_RESET) {
            result = answer_vulkan_fence_operation(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode == BVB_OPCODE_VULKAN_FENCE_WAIT) {
            result = answer_vulkan_fence_wait(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_SEMAPHORE_COUNTER) {
            result = answer_vulkan_semaphore_counter(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_SEMAPHORE_WAIT) {
            result = answer_vulkan_semaphore_wait(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_SEMAPHORE_SIGNAL) {
            result = answer_vulkan_semaphore_signal(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_QUEUE_SUBMIT_COMMAND_FENCE) {
            result = answer_vulkan_queue_submit_command_fence(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_QUEUE_SUBMIT_2) {
            result = answer_vulkan_queue_submit_2(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_SWAPCHAIN_PREPARE) {
            result = answer_vulkan_swapchain_prepare(
                client_fd, &request, negotiated, global_context,
                activity_status, activity_frame_socket);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_SWAPCHAIN_DESTROY) {
            result = answer_vulkan_swapchain_destroy(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_SWAPCHAIN_ACQUIRE) {
            result = answer_vulkan_swapchain_acquire(
                client_fd, &request, negotiated, global_context);
        } else if (request.header.opcode ==
                   BVB_OPCODE_VULKAN_SWAPCHAIN_PRESENT) {
            result = answer_vulkan_swapchain_present(
                client_fd, &request, negotiated, global_context);
        } else {
            result = -EPROTO;
        }
        if (received_fd >= 0) {
            (void)close(received_fd);
        }
        if (result != 0) {
            connection_status = result;
            break;
        }
    }
    bvb_vulkan_batch_context_destroy(vulkan_context);
    bvb_vulkan_global_context_destroy(global_context);
    if (shared_region.address != NULL) {
        if (munmap((void *)shared_region.address, shared_region.length) != 0 &&
            connection_status == 0) {
            connection_status = -errno;
        }
    }
    return connection_status;
}

static void *serve_connection_worker(void *opaque) {
    struct connection_worker *worker = opaque;
    int result = serve_connection(worker->client_fd, worker->loader_path,
                                  worker->activity_ingress,
                                  &worker->activity_status,
                                  worker->activity_frame_socket);
    (void)close(worker->client_fd);
    if (result != 0) {
        fprintf(stderr, "bvb: connection from pid %ld failed: %s\n",
                (long)worker->peer_pid, strerror(-result));
    }
    free(worker);
    return NULL;
}

static int start_connection_worker(
    int client_fd, pid_t peer_pid, const struct service_options *options,
    const struct bvb_activity_status *activity_status) {
    struct connection_worker *worker = calloc(1, sizeof(*worker));
    if (worker == NULL) {
        return -ENOMEM;
    }
    *worker = (struct connection_worker){
        .client_fd = client_fd,
        .loader_path = options->loader_path,
        .activity_frame_socket = options->activity_frame_socket,
        .activity_ingress = options->activity_ingress,
        .activity_status = *activity_status,
        .peer_pid = peer_pid,
    };
    pthread_t thread;
    int result = pthread_create(&thread, NULL, serve_connection_worker,
                                worker);
    if (result != 0) {
        free(worker);
        return -result;
    }
    result = pthread_detach(thread);
    if (result != 0) {
        fprintf(stderr, "bvb: could not detach client worker: %s\n",
                strerror(result));
    }
    return 0;
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
    int activity_listener = -1;
    uint16_t activity_port = 0;
    struct bvb_activity_status activity_status = {
        .ingress_configured = options.activity_ingress ? 1U : 0U,
    };
    if (options.activity_ingress) {
        activity_listener = activity_listen(options.activity_port, &activity_port);
        if (activity_listener < 0) {
            fprintf(stderr, "bvb: activity listen failed: %s\n",
                    strerror(-activity_listener));
            (void)close(listener);
            (void)unlink(options.socket_path);
            return 3;
        }
    }
    printf("bvb-bridge-service: ready socket=%s loader=%s activity_port=%u\n",
           options.socket_path, options.loader_path, (unsigned int)activity_port);
    fflush(stdout);

    bool finished = false;
    do {
        struct pollfd descriptors[2] = {
            {.fd = listener, .events = POLLIN},
            {.fd = activity_listener, .events = POLLIN},
        };
        nfds_t descriptor_count = activity_listener < 0 ? 1U : 2U;
        int ready = poll(descriptors, descriptor_count, -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "bvb: poll failed: %s\n", strerror(errno));
            exit_code = 4;
            break;
        }
        if (activity_listener >= 0 && (descriptors[1].revents & POLLIN) != 0) {
            int activity_fd = accept_cloexec(activity_listener);
            if (activity_fd < 0) {
                fprintf(stderr, "bvb: activity accept failed: %s\n",
                        strerror(-activity_fd));
            } else {
                int result = handle_activity_connection(
                    activity_fd, options.activity_token, &activity_status);
                if (result != 0 && result != -EACCES && result != -EPROTO) {
                    fprintf(stderr, "bvb: activity event failed: %s\n",
                            strerror(-result));
                }
                (void)close(activity_fd);
            }
        }
        if ((descriptors[0].revents & POLLIN) == 0) {
            continue;
        }
        int client_fd = accept_cloexec(listener);
        if (client_fd < 0) {
            if (client_fd == -EINTR) {
                continue;
            }
            fprintf(stderr, "bvb: accept failed: %s\n", strerror(-client_fd));
            exit_code = 4;
            break;
        }
        pid_t peer_pid = 0;
        int result = bvb_transport_authenticate(client_fd, geteuid(), &peer_pid);
        if (result == 0 && options.once) {
            result = serve_connection(client_fd, options.loader_path,
                                      options.activity_ingress, &activity_status,
                                      options.activity_frame_socket);
        } else if (result == 0) {
            result = start_connection_worker(
                client_fd, peer_pid, &options, &activity_status);
            if (result == 0) {
                client_fd = -1;
            }
        }
        if (client_fd >= 0) {
            (void)close(client_fd);
        }
        if (result != 0) {
            fprintf(stderr, "bvb: connection from pid %ld failed: %s\n",
                    (long)peer_pid, strerror(-result));
            if (options.once) {
                exit_code = 5;
                break;
            }
        }
        if (options.once) {
            finished = true;
        }
    } while (!finished);

    if (activity_listener >= 0) {
        (void)close(activity_listener);
    }
    (void)close(listener);
    if (unlink(options.socket_path) != 0 && errno != ENOENT) {
        fprintf(stderr, "bvb: could not remove socket: %s\n", strerror(errno));
        return 6;
    }
    return exit_code;
}
