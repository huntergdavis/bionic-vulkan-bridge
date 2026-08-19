#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <bvb/protocol.h>
#include <bvb/transport.h>
#include <bvb/triangle_batch_builder.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum {
    BVB_E021_REGION_BYTES = 4096,
    BVB_E021_REQUEST_ID = 0xE021,
    BVB_E022_REQUEST_ID = 0xE022,
    BVB_E022_GENERATION = 1,
    BVB_E022_BATCH_OFFSET = 64,
    BVB_E022_CONNECT_ATTEMPTS = 500,
    BVB_E022_CONNECT_RETRY_NS = 10000000,
    BVB_E022_TIMEOUT_SECONDS = 10,
};

struct relay_options {
    const char *socket_name;
    bool visible;
    uint16_t visible_port;
    uint8_t token[BVB_LIFECYCLE_TOKEN_SIZE];
    uint32_t width;
    uint32_t height;
};

static void usage(const char *program) {
    fprintf(stderr,
            "usage: %s --socket ABSTRACT_NAME [--visible-port PORT "
            "--token 64_HEX_DIGITS --width PIXELS --height PIXELS]\n",
            program);
}

static int parse_u32(const char *input, uint32_t *output) {
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(input, &end, 10);
    if (errno != 0 || end == input || *end != '\0' || value == 0U ||
        value > UINT32_MAX) {
        return -EINVAL;
    }
    *output = (uint32_t)value;
    return 0;
}

static int parse_arguments(int argc, char **argv,
                           struct relay_options *options) {
    memset(options, 0, sizeof(*options));
    bool token_present = false;
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--socket") == 0 && index + 1 < argc &&
            argv[index + 1][0] != '\0') {
            options->socket_name = argv[++index];
        } else if (strcmp(argv[index], "--visible-port") == 0 &&
                   index + 1 < argc) {
            uint32_t port = 0U;
            if (parse_u32(argv[++index], &port) != 0 || port > UINT16_MAX) {
                return -EINVAL;
            }
            options->visible_port = (uint16_t)port;
        } else if (strcmp(argv[index], "--token") == 0 &&
                   index + 1 < argc &&
                   bvb_lifecycle_token_from_hex(argv[index + 1],
                                                options->token) == 0) {
            ++index;
            token_present = true;
        } else if (strcmp(argv[index], "--width") == 0 &&
                   index + 1 < argc &&
                   parse_u32(argv[++index], &options->width) == 0) {
        } else if (strcmp(argv[index], "--height") == 0 &&
                   index + 1 < argc &&
                   parse_u32(argv[++index], &options->height) == 0) {
        } else {
            return -EINVAL;
        }
    }
    if (options->socket_name == NULL) {
        return -EINVAL;
    }
    const unsigned int visible_arguments =
        (options->visible_port != 0U ? 1U : 0U) +
        (token_present ? 1U : 0U) + (options->width != 0U ? 1U : 0U) +
        (options->height != 0U ? 1U : 0U);
    if (visible_arguments != 0U && visible_arguments != 4U) {
        return -EINVAL;
    }
    options->visible = visible_arguments == 4U;
    return 0;
}

static int64_t elapsed_ns(const struct timespec *started,
                          const struct timespec *finished) {
    return (int64_t)(finished->tv_sec - started->tv_sec) * 1000000000LL +
           (int64_t)finished->tv_nsec - (int64_t)started->tv_nsec;
}

static int validate_request(const struct bvb_protocol_packet *request) {
    if (request->header.version != BVB_PROTOCOL_VERSION ||
        request->header.kind != BVB_PROTOCOL_REQUEST ||
        request->header.opcode != BVB_OPCODE_HELLO ||
        request->header.request_id != BVB_E021_REQUEST_ID ||
        request->header.payload_length != BVB_HELLO_REQUEST_SIZE ||
        request->header.status != 0) {
        return -EPROTO;
    }
    struct bvb_hello_request hello;
    int result = bvb_protocol_decode_hello_request(request->payload, &hello);
    if (result != 0 || hello.minimum_version != BVB_PROTOCOL_VERSION ||
        hello.maximum_version != BVB_PROTOCOL_VERSION ||
        hello.client_flags != 0U) {
        return -EPROTO;
    }
    return 0;
}

static int validate_region(int descriptor, int *seals_out,
                           uint8_t **mapping_out) {
    struct stat metadata;
    if (fstat(descriptor, &metadata) != 0) {
        return -errno;
    }
    if (!S_ISREG(metadata.st_mode) ||
        metadata.st_size != BVB_E021_REGION_BYTES) {
        return -EPROTO;
    }
    int seals = fcntl(descriptor, F_GET_SEALS);
    if (seals < 0) {
        return -errno;
    }
    const int required_seals = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    if ((seals & required_seals) != required_seals ||
        (seals & F_SEAL_WRITE) != 0) {
        return -EPROTO;
    }
    int descriptor_flags = fcntl(descriptor, F_GETFL);
    if (descriptor_flags < 0) {
        return -errno;
    }
    if ((descriptor_flags & O_ACCMODE) != O_RDWR) {
        return -EACCES;
    }
    uint8_t *mapping = mmap(NULL, BVB_E021_REGION_BYTES,
                            PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
    if (mapping == MAP_FAILED) {
        return -errno;
    }
    static const char marker[] =
        "BVB_E020_SHARED_REGION binder_parcel_fd=PASS\n";
    int result = memcmp(mapping, marker, sizeof(marker) - 1U) == 0
                     ? 0
                     : -EBADMSG;
    if (result == 0) {
        mapping[BVB_E021_REGION_BYTES - 1U] = 0x21U;
        if (mapping[BVB_E021_REGION_BYTES - 1U] != 0x21U) {
            result = -EIO;
        }
    }
    if (result == 0) {
        *seals_out = seals;
        *mapping_out = mapping;
    } else {
        (void)munmap(mapping, BVB_E021_REGION_BYTES);
    }
    return result;
}

static int configure_socket_timeout(int socket_fd) {
    const struct timeval timeout = {
        .tv_sec = BVB_E022_TIMEOUT_SECONDS,
    };
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) != 0 ||
        setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                   sizeof(timeout)) != 0) {
        return -errno;
    }
    return 0;
}

static int connect_visible_with_retry(uint16_t port) {
    const struct timespec delay = {.tv_nsec = BVB_E022_CONNECT_RETRY_NS};
    for (unsigned int attempt = 0U; attempt < BVB_E022_CONNECT_ATTEMPTS;
         ++attempt) {
        int socket_fd = bvb_transport_connect_loopback(port);
        if (socket_fd >= 0) {
            return socket_fd;
        }
        if (socket_fd != -ECONNREFUSED) {
            return socket_fd;
        }
        if (attempt + 1U < BVB_E022_CONNECT_ATTEMPTS) {
            struct timespec remaining = delay;
            while (nanosleep(&remaining, &remaining) != 0) {
                if (errno != EINTR) {
                    return -errno;
                }
            }
        }
    }
    return -ETIMEDOUT;
}

static int execute_visible(const struct relay_options *options,
                           uint8_t *mapping, size_t *batch_length,
                           int64_t *round_trip_ns) {
    int result = bvb_triangle_batch_build(
        mapping + BVB_E022_BATCH_OFFSET,
        BVB_E021_REGION_BYTES - BVB_E022_BATCH_OFFSET, options->width,
        options->height, batch_length);
    if (result != 0 || *batch_length > UINT32_MAX) {
        return result != 0 ? result : -EOVERFLOW;
    }
    atomic_thread_fence(memory_order_release);
    int socket_fd = connect_visible_with_retry(options->visible_port);
    if (socket_fd < 0) {
        return socket_fd;
    }
    result = configure_socket_timeout(socket_fd);
    struct bvb_protocol_packet request;
    memset(&request, 0, sizeof(request));
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VISIBLE_BATCH_EXECUTE,
        .request_id = BVB_E022_REQUEST_ID,
        .payload_length = BVB_VISIBLE_BATCH_EXECUTE_SIZE,
    };
    struct bvb_visible_batch_execute execute = {
        .shared = {
            .generation = BVB_E022_GENERATION,
            .offset = BVB_E022_BATCH_OFFSET,
            .length = (uint32_t)*batch_length,
            .sequence = 1U,
        },
    };
    memcpy(execute.token, options->token, sizeof(execute.token));
    if (result == 0) {
        result = bvb_protocol_encode_visible_batch_execute(request.payload,
                                                           &execute);
    }
    struct timespec started;
    struct timespec finished;
    if (result == 0 && clock_gettime(CLOCK_MONOTONIC, &started) != 0) {
        result = -errno;
    }
    if (result == 0) {
        result = bvb_transport_send(socket_fd, &request);
    }
    struct bvb_protocol_packet response;
    memset(&response, 0, sizeof(response));
    if (result == 0) {
        result = bvb_transport_receive(socket_fd, &response);
    }
    if (result == 0 && clock_gettime(CLOCK_MONOTONIC, &finished) != 0) {
        result = -errno;
    }
    if (result == 0 &&
        (response.header.version != BVB_PROTOCOL_VERSION ||
         response.header.kind != BVB_PROTOCOL_RESPONSE ||
         response.header.opcode != BVB_OPCODE_VISIBLE_BATCH_EXECUTE ||
         response.header.request_id != BVB_E022_REQUEST_ID ||
         response.header.payload_length != 0U || response.header.status > 0)) {
        result = -EPROTO;
    }
    if (result == 0 && response.header.status != 0) {
        result = response.header.status;
    }
    if (result == 0) {
        *round_trip_ns = elapsed_ns(&started, &finished);
    }
    if (close(socket_fd) != 0 && result == 0) {
        result = -errno;
    }
    return result;
}

static int send_response(int socket_fd,
                         const struct bvb_protocol_packet *request,
                         int status) {
    struct bvb_protocol_packet response;
    memset(&response, 0, sizeof(response));
    response.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_RESPONSE,
        .opcode = request->header.opcode,
        .request_id = request->header.request_id,
        .status = status,
    };
    return bvb_transport_send(socket_fd, &response);
}

int main(int argc, char **argv) {
    struct relay_options options;
    if (parse_arguments(argc, argv, &options) != 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    const uint8_t *socket_name = (const uint8_t *)options.socket_name;
    size_t socket_name_length = strlen(options.socket_name);
    int listener =
        bvb_transport_listen_abstract(socket_name, socket_name_length);
    if (listener < 0) {
        fprintf(stderr, "listen failed: %s (%d)\n", strerror(-listener),
                listener);
        return EXIT_FAILURE;
    }
    printf("bvb-shared-region-relay: ready socket=%s mode=%s\n",
           options.socket_name, options.visible ? "visible" : "transport");

    int connection = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    if (connection < 0) {
        fprintf(stderr, "accept failed: %s\n", strerror(errno));
        (void)close(listener);
        return EXIT_FAILURE;
    }
    uid_t peer_uid = (uid_t)-1;
    pid_t peer_pid = 0;
    int result = bvb_transport_peer_credentials(connection, &peer_uid,
                                                &peer_pid);
    if (result == 0 && peer_uid != getuid()) {
        result = -EACCES;
    }
    struct timespec started;
    struct timespec finished;
    (void)clock_gettime(CLOCK_MONOTONIC, &started);
    struct bvb_protocol_packet request;
    memset(&request, 0, sizeof(request));
    int region_fd = -1;
    if (result == 0) {
        result = bvb_transport_receive_fd(connection, &request, &region_fd);
    }
    if (result == 0) {
        result = validate_request(&request);
    }
    int seals = 0;
    uint8_t *mapping = MAP_FAILED;
    if (result == 0) {
        result = validate_region(region_fd, &seals, &mapping);
    }
    struct timespec validated;
    (void)clock_gettime(CLOCK_MONOTONIC, &validated);
    size_t batch_length = 0U;
    int64_t execute_round_trip_ns = 0;
    if (result == 0 && options.visible) {
        result = execute_visible(&options, mapping, &batch_length,
                                 &execute_round_trip_ns);
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &finished);
    int response_result = region_fd >= 0
                              ? send_response(connection, &request, result)
                              : 0;
    if (region_fd >= 0) {
        (void)close(region_fd);
    }
    if (mapping != MAP_FAILED) {
        (void)munmap(mapping, BVB_E021_REGION_BYTES);
    }
    (void)close(connection);
    (void)close(listener);
    if (result != 0 || response_result != 0) {
        int failure = result != 0 ? result : response_result;
        fprintf(stderr, "relay validation failed: %s (%d)\n",
                strerror(-failure), failure);
        return EXIT_FAILURE;
    }
    if (options.visible) {
        printf("{\"result\":\"pass\","
               "\"transport\":\"binder_scm_rights_then_loopback_metadata\","
               "\"peer_uid\":%lu,\"peer_pid\":%ld,"
               "\"region_bytes\":%u,\"seals\":%d,"
               "\"writable_mapping\":true,\"width\":%" PRIu32
               ",\"height\":%" PRIu32 ",\"batch_offset\":%u,"
               "\"batch_bytes\":%zu,\"commands\":6,\"sequence\":1,"
               "\"receive_validate_ns\":%" PRId64
               ",\"execute_round_trip_ns\":%" PRId64
               ",\"receive_to_present_ns\":%" PRId64 "}\n",
               (unsigned long)peer_uid, (long)peer_pid,
               BVB_E021_REGION_BYTES, seals, options.width, options.height,
               BVB_E022_BATCH_OFFSET, batch_length,
               elapsed_ns(&started, &validated), execute_round_trip_ns,
               elapsed_ns(&started, &finished));
    } else {
        printf("{\"result\":\"pass\","
               "\"transport\":\"binder_then_same_uid_scm_rights\","
               "\"peer_uid\":%lu,\"peer_pid\":%ld,"
               "\"region_bytes\":%u,\"seals\":%d,"
               "\"writable_mapping\":true,"
               "\"receive_validate_ns\":%" PRId64 "}\n",
               (unsigned long)peer_uid, (long)peer_pid,
               BVB_E021_REGION_BYTES, seals,
               elapsed_ns(&started, &validated));
    }
    return EXIT_SUCCESS;
}
