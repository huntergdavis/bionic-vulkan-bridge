#define _POSIX_C_SOURCE 200809L

#include <bvb/command_batch.h>
#include <bvb/protocol.h>
#include <bvb/transport.h>
#include <bvb/vulkan_caps.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
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

static void usage(const char *program) {
    fprintf(stderr,
            "usage: %s --socket ABSOLUTE_PATH "
            "[--loader ABSOLUTE_PATH] [--once] "
            "[--activity-port 0..65535 --activity-token 64_HEX]\n",
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
                            const struct bvb_activity_status *activity_status) {
    bool negotiated = false;
    struct shared_batch_region shared_region = {0};
    struct bvb_vulkan_batch_context *vulkan_context = NULL;
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
    if (shared_region.address != NULL) {
        if (munmap((void *)shared_region.address, shared_region.length) != 0 &&
            connection_status == 0) {
            connection_status = -errno;
        }
    }
    return connection_status;
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
        if (result == 0) {
            result = serve_connection(client_fd, options.loader_path,
                                      options.activity_ingress, &activity_status);
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
