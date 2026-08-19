#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <bvb/lifecycle.h>
#include <bvb/protocol.h>
#include <bvb/transport.h>
#include <bvb/triangle_batch_builder.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/memfd.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

enum {
    REGION_BYTES = 4096,
    BATCH_OFFSET = 64,
    CONNECT_ATTEMPTS = 500,
    CONNECT_RETRY_NS = 10000000,
    EXCHANGE_TIMEOUT_SECONDS = 5,
};

struct client_options {
    const char *socket_name;
    uint16_t tcp_port;
    uint8_t token[BVB_LIFECYCLE_TOKEN_SIZE];
    uint32_t width;
    uint32_t height;
};

static void usage(const char *program) {
    fprintf(stderr,
            "usage: %s (--socket-name NAME | --tcp-port PORT) "
            "--token 64_HEX_DIGITS "
            "--width PIXELS --height PIXELS\n",
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
                           struct client_options *options) {
    memset(options, 0, sizeof(*options));
    bool token_present = false;
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--socket-name") == 0 && index + 1 < argc &&
            argv[index + 1][0] != '\0') {
            options->socket_name = argv[++index];
        } else if (strcmp(argv[index], "--tcp-port") == 0 &&
                   index + 1 < argc) {
            uint32_t port = 0U;
            if (parse_u32(argv[index + 1], &port) != 0 ||
                port > UINT16_MAX) {
                usage(argv[0]);
                return -EINVAL;
            }
            options->tcp_port = (uint16_t)port;
            ++index;
        } else if (strcmp(argv[index], "--token") == 0 &&
                   index + 1 < argc &&
                   bvb_lifecycle_token_from_hex(argv[index + 1],
                                                options->token) == 0) {
            ++index;
            token_present = true;
        } else if (strcmp(argv[index], "--width") == 0 &&
                   index + 1 < argc &&
                   parse_u32(argv[index + 1], &options->width) == 0) {
            ++index;
        } else if (strcmp(argv[index], "--height") == 0 &&
                   index + 1 < argc &&
                   parse_u32(argv[index + 1], &options->height) == 0) {
            ++index;
        } else {
            usage(argv[0]);
            return -EINVAL;
        }
    }
    if ((options->socket_name == NULL) == (options->tcp_port == 0U) ||
        !token_present ||
        options->width == 0U || options->height == 0U) {
        usage(argv[0]);
        return -EINVAL;
    }
    return 0;
}

static int monotonic_ns(uint64_t *output) {
    struct timespec timestamp;
    if (output == NULL) {
        return -EINVAL;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
        return -errno;
    }
    *output = (uint64_t)timestamp.tv_sec * UINT64_C(1000000000) +
              (uint64_t)timestamp.tv_nsec;
    return 0;
}

static int random_nonzero_u64(uint64_t *output) {
    if (output == NULL) {
        return -EINVAL;
    }
    ssize_t received;
    do {
        received = syscall(SYS_getrandom, output, sizeof(*output), 0);
    } while (received < 0 && errno == EINTR);
    if (received != (ssize_t)sizeof(*output) || *output == 0U) {
        return received < 0 ? -errno : -EIO;
    }
    return 0;
}

static int connect_abstract_with_retry(const char *name) {
    const struct timespec delay = {.tv_nsec = CONNECT_RETRY_NS};
    for (unsigned int attempt = 0U; attempt < CONNECT_ATTEMPTS; ++attempt) {
        int socket_fd = bvb_transport_connect_abstract(
            (const uint8_t *)name, strlen(name));
        if (socket_fd >= 0) {
            return socket_fd;
        }
        if (socket_fd != -ENOENT && socket_fd != -ECONNREFUSED) {
            return socket_fd;
        }
        if (attempt + 1U < CONNECT_ATTEMPTS) {
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

static int connect_loopback_with_retry(uint16_t port) {
    const struct timespec delay = {.tv_nsec = CONNECT_RETRY_NS};
    for (unsigned int attempt = 0U; attempt < CONNECT_ATTEMPTS; ++attempt) {
        int socket_fd = bvb_transport_connect_loopback(port);
        if (socket_fd >= 0) {
            return socket_fd;
        }
        if (socket_fd != -ECONNREFUSED) {
            return socket_fd;
        }
        if (attempt + 1U < CONNECT_ATTEMPTS) {
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

static int exchange(int socket_fd,
                    const struct bvb_protocol_packet *request,
                    struct bvb_protocol_packet *response, int passed_fd) {
    int result = passed_fd >= 0
                     ? bvb_transport_send_fd(socket_fd, request, passed_fd)
                     : bvb_transport_send(socket_fd, request);
    if (result != 0) {
        return result < 0 ? result : -EPROTO;
    }
    memset(response, 0, sizeof(*response));
    result = bvb_transport_receive(socket_fd, response);
    if (result != 0) {
        return result;
    }
    if (response->header.version != BVB_PROTOCOL_VERSION ||
        response->header.kind != BVB_PROTOCOL_RESPONSE ||
        response->header.opcode != request->header.opcode ||
        response->header.request_id != request->header.request_id ||
        response->header.payload_length != 0U) {
        return -EPROTO;
    }
    return response->header.status <= 0 ? response->header.status : -EPROTO;
}

static int set_exchange_timeout(int socket_fd) {
    const struct timeval timeout = {
        .tv_sec = EXCHANGE_TIMEOUT_SECONDS,
    };
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) != 0 ||
        setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                   sizeof(timeout)) != 0) {
        return -errno;
    }
    return 0;
}

static int run(const struct client_options *options,
               const char **failure_stage) {
    int result = 0;
    int memory_fd = -1;
    int socket_fd = -1;
    uint8_t *mapping = MAP_FAILED;
    uint8_t inline_storage[REGION_BYTES];
    uint64_t generation = 0U;
    size_t batch_length = 0U;
    const bool inline_mode = options->tcp_port != 0U;
    uint8_t *batch_bytes = NULL;

    *failure_stage = inline_mode ? "inline_batch" : "shared_region";
    if (inline_mode) {
        batch_bytes = inline_storage;
    } else {
        result = random_nonzero_u64(&generation);
        if (result != 0) {
            goto done;
        }
        memory_fd = (int)syscall(SYS_memfd_create, "bvb-visible-triangle",
                                 MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (memory_fd < 0) {
            result = -errno;
            goto done;
        }
        if (ftruncate(memory_fd, REGION_BYTES) != 0) {
            result = -errno;
            goto done;
        }
        mapping = mmap(NULL, REGION_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED,
                       memory_fd, 0);
        if (mapping == MAP_FAILED) {
            result = -errno;
            goto done;
        }
        batch_bytes = mapping + BATCH_OFFSET;
    }
    result = bvb_triangle_batch_build(
        batch_bytes,
        inline_mode ? sizeof(inline_storage) : REGION_BYTES - BATCH_OFFSET,
        options->width, options->height, &batch_length);
    if (result != 0 || batch_length > UINT32_MAX) {
        result = result != 0 ? result : -EOVERFLOW;
        goto done;
    }
    if (!inline_mode) {
        if (fcntl(memory_fd, F_ADD_SEALS,
                  F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0) {
            result = -errno;
            goto done;
        }
        atomic_thread_fence(memory_order_release);
    }

    *failure_stage = inline_mode ? "connect_loopback" : "connect_abstract";
    socket_fd = inline_mode
                    ? connect_loopback_with_retry(options->tcp_port)
                    : connect_abstract_with_retry(options->socket_name);
    if (socket_fd < 0) {
        result = socket_fd;
        socket_fd = -1;
        goto done;
    }
    result = set_exchange_timeout(socket_fd);
    if (result != 0) {
        goto done;
    }

    struct bvb_protocol_packet request;
    struct bvb_protocol_packet response;
    uint64_t start_ns = 0U;
    uint64_t end_ns = 0U;
    if (inline_mode) {
        *failure_stage = "visible_inline";
        if (batch_length > BVB_VISIBLE_BATCH_INLINE_MAX_BYTES) {
            result = -EMSGSIZE;
            goto done;
        }
        memset(&request, 0, sizeof(request));
        request.header = (struct bvb_protocol_header){
            .version = BVB_PROTOCOL_VERSION,
            .kind = BVB_PROTOCOL_REQUEST,
            .opcode = BVB_OPCODE_VISIBLE_BATCH_INLINE,
            .request_id = UINT32_C(0x4256420a),
            .payload_length =
                (uint32_t)(BVB_VISIBLE_BATCH_INLINE_PREFIX_SIZE +
                           batch_length),
        };
        memcpy(request.payload, options->token,
               BVB_VISIBLE_BATCH_INLINE_PREFIX_SIZE);
        memcpy(request.payload + BVB_VISIBLE_BATCH_INLINE_PREFIX_SIZE,
               batch_bytes, batch_length);
        result = monotonic_ns(&start_ns);
        if (result == 0) {
            result = exchange(socket_fd, &request, &response, -1);
        }
        if (result == 0) {
            result = monotonic_ns(&end_ns);
        }
        if (result != 0) {
            goto done;
        }
        *failure_stage = "complete";
        printf("{\"transport\":\"loopback_tcp_inline\",\"tcp_port\":%u,"
               "\"width\":%" PRIu32 ",\"height\":%" PRIu32
               ",\"batch_bytes\":%zu,\"commands\":6,\"sequence\":1,"
               "\"packet_bytes\":%zu,\"round_trip_ns\":%" PRIu64 "}\n",
               (unsigned int)options->tcp_port, options->width,
               options->height, batch_length,
               (size_t)BVB_PROTOCOL_HEADER_SIZE +
                   BVB_VISIBLE_BATCH_INLINE_PREFIX_SIZE + batch_length,
               end_ns - start_ns);
        goto done;
    }

    *failure_stage = "visible_setup";
    memset(&request, 0, sizeof(request));
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VISIBLE_BATCH_SETUP,
        .request_id = UINT32_C(0x42564208),
        .payload_length = BVB_VISIBLE_BATCH_SETUP_SIZE,
    };
    struct bvb_visible_batch_setup setup = {
        .shared = {
            .region_bytes = REGION_BYTES,
            .generation = generation,
        },
    };
    memcpy(setup.token, options->token, sizeof(setup.token));
    result = bvb_protocol_encode_visible_batch_setup(request.payload, &setup);
    if (result == 0) {
        result = exchange(socket_fd, &request, &response, memory_fd);
    }
    if (result != 0) {
        goto done;
    }

    *failure_stage = "visible_execute";
    memset(&request, 0, sizeof(request));
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VISIBLE_BATCH_EXECUTE,
        .request_id = UINT32_C(0x42564209),
        .payload_length = BVB_VISIBLE_BATCH_EXECUTE_SIZE,
    };
    struct bvb_visible_batch_execute execute = {
        .shared = {
            .generation = generation,
            .offset = BATCH_OFFSET,
            .length = (uint32_t)batch_length,
            .sequence = 1U,
        },
    };
    memcpy(execute.token, options->token, sizeof(execute.token));
    result =
        bvb_protocol_encode_visible_batch_execute(request.payload, &execute);
    if (result == 0) {
        result = monotonic_ns(&start_ns);
    }
    if (result == 0) {
        result = exchange(socket_fd, &request, &response, -1);
    }
    if (result == 0) {
        result = monotonic_ns(&end_ns);
    }
    if (result != 0) {
        goto done;
    }

    *failure_stage = "complete";
    printf("{\"socket_name\":\"%s\",\"width\":%" PRIu32
           ",\"height\":%" PRIu32 ",\"region_bytes\":%u"
           ",\"batch_offset\":%u,\"batch_bytes\":%zu,\"commands\":6"
           ",\"sequence\":1,\"setup_packet_bytes\":%u"
           ",\"execute_packet_bytes\":%u,\"execute_round_trip_ns\":%" PRIu64
           "}\n",
           options->socket_name, options->width, options->height, REGION_BYTES,
           BATCH_OFFSET, batch_length,
           BVB_PROTOCOL_HEADER_SIZE + BVB_VISIBLE_BATCH_SETUP_SIZE,
           BVB_PROTOCOL_HEADER_SIZE + BVB_VISIBLE_BATCH_EXECUTE_SIZE,
           end_ns - start_ns);

done:
    if (socket_fd >= 0 && close(socket_fd) != 0 && result == 0) {
        result = -errno;
    }
    if (mapping != MAP_FAILED && munmap(mapping, REGION_BYTES) != 0 &&
        result == 0) {
        result = -errno;
    }
    if (memory_fd >= 0 && close(memory_fd) != 0 && result == 0) {
        result = -errno;
    }
    return result;
}

int main(int argc, char **argv) {
    struct client_options options;
    const char *failure_stage = "arguments";
    int result = parse_arguments(argc, argv, &options);
    if (result == 0) {
        result = run(&options, &failure_stage);
    }
    if (result != 0) {
        fprintf(stderr, "visible triangle client failed: stage=%s: %s (%d)\n",
                failure_stage, strerror(-result), result);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
