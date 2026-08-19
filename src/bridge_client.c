#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <bvb/command_batch.h>
#include <bvb/protocol.h>
#include <bvb/transport.h>
#include <bvb/vulkan_caps.h>

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
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

struct client_options {
    const char *socket_path;
    bool request_vulkan_caps;
    bool request_vulkan_selftest;
    bool request_vulkan_batch_selftest;
    bool request_vulkan_shared_batch_selftest;
    bool request_vulkan_shared_batch_benchmark;
    uint32_t shared_batch_iterations;
    bool request_activity_status;
};

struct shared_batch_benchmark_result {
    uint32_t warmup_iterations;
    uint32_t measured_iterations;
    uint64_t control_total_ns;
    uint64_t control_min_ns;
    uint64_t control_max_ns;
    uint64_t submit_wait_total_ns;
    uint64_t submit_wait_min_ns;
    uint64_t submit_wait_max_ns;
    uint64_t mismatched_words;
};

static void usage(const char *program) {
    fprintf(stderr,
            "usage: %s --socket ABSOLUTE_PATH "
            "[--vulkan-caps] [--vulkan-selftest | "
            "--vulkan-batch-selftest | --vulkan-shared-batch-selftest | "
            "--vulkan-shared-batch-benchmark ITERATIONS] "
            "[--activity-status]\n",
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
        } else if (strcmp(argv[index], "--vulkan-selftest") == 0) {
            options->request_vulkan_selftest = true;
        } else if (strcmp(argv[index], "--vulkan-batch-selftest") == 0) {
            options->request_vulkan_batch_selftest = true;
        } else if (strcmp(argv[index], "--vulkan-shared-batch-selftest") == 0) {
            options->request_vulkan_shared_batch_selftest = true;
        } else if (strcmp(argv[index],
                          "--vulkan-shared-batch-benchmark") == 0 &&
                   index + 1 < argc) {
            char *end = NULL;
            errno = 0;
            unsigned long iterations = strtoul(argv[++index], &end, 10);
            if (errno != 0 || end == argv[index] || *end != '\0' ||
                iterations == 0U || iterations > 10000U) {
                usage(argv[0]);
                return 2;
            }
            options->request_vulkan_shared_batch_benchmark = true;
            options->shared_batch_iterations = (uint32_t)iterations;
        } else if (strcmp(argv[index], "--activity-status") == 0) {
            options->request_activity_status = true;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    unsigned int selftest_count =
        (options->request_vulkan_selftest ? 1U : 0U) +
        (options->request_vulkan_batch_selftest ? 1U : 0U) +
        (options->request_vulkan_shared_batch_selftest ? 1U : 0U) +
        (options->request_vulkan_shared_batch_benchmark ? 1U : 0U);
    if (options->socket_path == NULL || selftest_count > 1U) {
        usage(argv[0]);
        return 2;
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

static void print_extension_array(uint64_t flags, bool device) {
    struct extension_name {
        uint64_t flag;
        const char *name;
    };
    static const struct extension_name instance_extensions[] = {
        {BVB_INSTANCE_KHR_SURFACE, "VK_KHR_surface"},
        {BVB_INSTANCE_KHR_ANDROID_SURFACE, "VK_KHR_android_surface"},
        {BVB_INSTANCE_EXT_HEADLESS_SURFACE, "VK_EXT_headless_surface"},
        {BVB_INSTANCE_KHR_GET_PROPERTIES_2,
         "VK_KHR_get_physical_device_properties2"},
        {BVB_INSTANCE_KHR_EXTERNAL_MEMORY_CAPS,
         "VK_KHR_external_memory_capabilities"},
        {BVB_INSTANCE_KHR_EXTERNAL_SEMAPHORE_CAPS,
         "VK_KHR_external_semaphore_capabilities"},
    };
    static const struct extension_name device_extensions[] = {
        {BVB_DEVICE_KHR_SWAPCHAIN, "VK_KHR_swapchain"},
        {BVB_DEVICE_KHR_EXTERNAL_MEMORY, "VK_KHR_external_memory"},
        {BVB_DEVICE_KHR_EXTERNAL_MEMORY_FD, "VK_KHR_external_memory_fd"},
        {BVB_DEVICE_ANDROID_HARDWARE_BUFFER,
         "VK_ANDROID_external_memory_android_hardware_buffer"},
        {BVB_DEVICE_KHR_EXTERNAL_SEMAPHORE, "VK_KHR_external_semaphore"},
        {BVB_DEVICE_KHR_EXTERNAL_SEMAPHORE_FD,
         "VK_KHR_external_semaphore_fd"},
        {BVB_DEVICE_KHR_TIMELINE_SEMAPHORE, "VK_KHR_timeline_semaphore"},
        {BVB_DEVICE_KHR_EXTERNAL_FENCE_FD, "VK_KHR_external_fence_fd"},
    };
    const struct extension_name *extensions =
        device ? device_extensions : instance_extensions;
    size_t count = device
                       ? sizeof(device_extensions) / sizeof(device_extensions[0])
                       : sizeof(instance_extensions) /
                             sizeof(instance_extensions[0]);
    bool first = true;
    putchar('[');
    for (size_t index = 0; index < count; ++index) {
        if ((flags & extensions[index].flag) == 0U) {
            continue;
        }
        if (!first) {
            putchar(',');
        }
        printf("\"%s\"", extensions[index].name);
        first = false;
    }
    putchar(']');
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

static int exchange_fd(int socket_fd,
                       const struct bvb_protocol_packet *request,
                       struct bvb_protocol_packet *response, int passed_fd) {
    int result = bvb_transport_send_fd(socket_fd, request, passed_fd);
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
        return -EPROTO;
    }
    return 0;
}

static int build_transfer_batch(uint8_t *bytes, size_t capacity,
                                uint64_t sequence, size_t *batch_length) {
    if (bytes == NULL || batch_length == NULL) {
        return -EINVAL;
    }
    struct bvb_command_batch_builder builder;
    const uint64_t command_buffer_id =
        bvb_handle_id(BVB_OBJECT_COMMAND_BUFFER, 1U);
    const uint64_t buffer_id = bvb_handle_id(BVB_OBJECT_BUFFER, 1U);
    int result = bvb_command_batch_begin(&builder, bytes, capacity,
                                         command_buffer_id, sequence);
    if (result == 0) {
        result = bvb_command_batch_append_fill_buffer(
            &builder,
            &(const struct bvb_fill_buffer_command){
                .buffer_id = buffer_id,
                .offset = 0U,
                .size = 4096U,
                .data = UINT32_C(0xa5c3f00d),
            });
    }
    if (result == 0) {
        result = bvb_command_batch_append_buffer_host_read_barrier(
            &builder,
            &(const struct bvb_buffer_host_read_barrier_command){
                .buffer_id = buffer_id,
                .offset = 0U,
                .size = 4096U,
            });
    }
    if (result == 0) {
        result = bvb_command_batch_finish(&builder, batch_length);
    }
    return result;
}

static int request_shared_batch_selftest(
    int socket_fd, uint32_t measured_iterations,
    struct bvb_vulkan_selftest_result *selftest,
    struct shared_batch_benchmark_result *benchmark) {
    enum {
        REGION_BYTES = 4096,
        BATCH_OFFSET = 64,
    };
    int memory_fd = -1;
    void *mapping = MAP_FAILED;
    int result = 0;
    if (selftest == NULL ||
        (measured_iterations != 0U && benchmark == NULL)) {
        return -EINVAL;
    }
    if (benchmark != NULL) {
        memset(benchmark, 0, sizeof(*benchmark));
    }
    uint64_t generation = 0U;
    ssize_t random_bytes =
        syscall(SYS_getrandom, &generation, sizeof(generation), 0);
    if (random_bytes != (ssize_t)sizeof(generation) || generation == 0U) {
        return -EIO;
    }
    memory_fd = (int)syscall(SYS_memfd_create, "bvb-shared-batch",
                             MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (memory_fd < 0) {
        return -errno;
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
    size_t batch_length = 0U;
    result = build_transfer_batch((uint8_t *)mapping + BATCH_OFFSET,
                                  REGION_BYTES - BATCH_OFFSET, 1U,
                                  &batch_length);
    if (result != 0 || batch_length > UINT32_MAX) {
        result = result != 0 ? result : -EOVERFLOW;
        goto done;
    }
    if (fcntl(memory_fd, F_ADD_SEALS,
              F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0) {
        result = -errno;
        goto done;
    }
    atomic_thread_fence(memory_order_release);

    struct bvb_protocol_packet request;
    memset(&request, 0, sizeof(request));
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_SHARED_BATCH_SETUP,
        .request_id = 0x42564206U,
        .payload_length = BVB_SHARED_BATCH_SETUP_SIZE,
    };
    const struct bvb_shared_batch_setup setup = {
        .region_bytes = REGION_BYTES,
        .generation = generation,
    };
    result = bvb_protocol_encode_shared_batch_setup(request.payload, &setup);
    struct bvb_protocol_packet response;
    if (result == 0) {
        result = exchange_fd(socket_fd, &request, &response, memory_fd);
    }
    if (result != 0 || response.header.status != 0 ||
        response.header.payload_length != 0U) {
        result = result != 0 ? result : -EPROTO;
        goto done;
    }

    const uint32_t total_iterations =
        measured_iterations == 0U ? 1U : measured_iterations + 1U;
    for (uint32_t index = 0; index < total_iterations; ++index) {
        const uint64_t sequence = (uint64_t)index + 1U;
        result = build_transfer_batch(
            (uint8_t *)mapping + BATCH_OFFSET, REGION_BYTES - BATCH_OFFSET,
            sequence, &batch_length);
        if (result != 0 || batch_length > UINT32_MAX) {
            result = result != 0 ? result : -EOVERFLOW;
            goto done;
        }
        atomic_thread_fence(memory_order_release);

        memset(&request, 0, sizeof(request));
        request.header = (struct bvb_protocol_header){
            .version = BVB_PROTOCOL_VERSION,
            .kind = BVB_PROTOCOL_REQUEST,
            .opcode = BVB_OPCODE_SHARED_BATCH_EXECUTE,
            .request_id = UINT32_C(0x42564300) + index,
            .payload_length = BVB_SHARED_BATCH_EXECUTE_SIZE,
        };
        const struct bvb_shared_batch_execute execute = {
            .generation = generation,
            .offset = BATCH_OFFSET,
            .length = (uint32_t)batch_length,
            .sequence = sequence,
        };
        result = bvb_protocol_encode_shared_batch_execute(request.payload,
                                                          &execute);
        uint64_t control_start_ns = 0U;
        uint64_t control_end_ns = 0U;
        const bool measured = measured_iterations != 0U && index != 0U;
        if (result == 0 && measured) {
            result = monotonic_ns(&control_start_ns);
        }
        if (result == 0) {
            result = exchange(socket_fd, &request, &response);
        }
        if (result == 0 && measured) {
            result = monotonic_ns(&control_end_ns);
        }
        if (result != 0 || response.header.status != 0 ||
            response.header.payload_length != BVB_VULKAN_SELFTEST_SIZE) {
            result = result != 0 ? result : -EPROTO;
            goto done;
        }
        result = bvb_protocol_decode_vulkan_selftest(response.payload,
                                                     selftest);
        if (result != 0) {
            goto done;
        }
        if (measured) {
            const uint64_t control_ns = control_end_ns - control_start_ns;
            if (benchmark->measured_iterations == 0U ||
                control_ns < benchmark->control_min_ns) {
                benchmark->control_min_ns = control_ns;
            }
            if (control_ns > benchmark->control_max_ns) {
                benchmark->control_max_ns = control_ns;
            }
            if (benchmark->measured_iterations == 0U ||
                selftest->submit_wait_elapsed_ns <
                    benchmark->submit_wait_min_ns) {
                benchmark->submit_wait_min_ns =
                    selftest->submit_wait_elapsed_ns;
            }
            if (selftest->submit_wait_elapsed_ns >
                benchmark->submit_wait_max_ns) {
                benchmark->submit_wait_max_ns =
                    selftest->submit_wait_elapsed_ns;
            }
            benchmark->control_total_ns += control_ns;
            benchmark->submit_wait_total_ns +=
                selftest->submit_wait_elapsed_ns;
            benchmark->mismatched_words += selftest->mismatched_words;
            benchmark->measured_iterations += 1U;
        }
    }
    if (benchmark != NULL) {
        benchmark->warmup_iterations = measured_iterations == 0U ? 0U : 1U;
    }

done:
    if (mapping != MAP_FAILED) {
        (void)munmap(mapping, REGION_BYTES);
    }
    if (memory_fd >= 0) {
        (void)close(memory_fd);
    }
    return result;
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
                           const struct bvb_vulkan_caps *caps,
                           const struct bvb_vulkan_selftest_result *selftest,
                           const char *selftest_key,
                           const struct shared_batch_benchmark_result *benchmark,
                           const struct bvb_activity_status *activity) {
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
    if (selftest != NULL) {
        printf(",\"%s\":{\"instance_extension_count\":%" PRIu32
               ",\"instance_extension_flags\":%" PRIu64
               ",\"known_instance_extensions\":",
               selftest_key,
               selftest->instance_extension_count,
               selftest->instance_extension_flags);
        print_extension_array(selftest->instance_extension_flags, false);
        printf(",\"device_extension_count\":%" PRIu32
               ",\"device_extension_flags\":%" PRIu64
               ",\"known_device_extensions\":",
               selftest->device_extension_count,
               selftest->device_extension_flags);
        print_extension_array(selftest->device_extension_flags, true);
        printf(",\"queue_family_index\":%" PRIu32
               ",\"queue_flags\":%" PRIu32
               ",\"memory_type_index\":%" PRIu32
               ",\"memory_property_flags\":%" PRIu32
               ",\"buffer_bytes\":%" PRIu32
               ",\"fill_word\":%" PRIu32
               ",\"mismatched_words\":%" PRIu32
               ",\"submit_wait_elapsed_ns\":%" PRIu64 "}",
               selftest->queue_family_index,
               selftest->queue_flags,
               selftest->memory_type_index,
               selftest->memory_property_flags,
               selftest->buffer_bytes,
               selftest->fill_word,
               selftest->mismatched_words,
               selftest->submit_wait_elapsed_ns);
    }
    if (benchmark != NULL && benchmark->measured_iterations != 0U) {
        printf(",\"shared_batch_benchmark\":{\"warmup_iterations\":%" PRIu32
               ",\"measured_iterations\":%" PRIu32
               ",\"control_total_ns\":%" PRIu64
               ",\"control_min_ns\":%" PRIu64
               ",\"control_mean_ns\":%" PRIu64
               ",\"control_max_ns\":%" PRIu64
               ",\"submit_wait_total_ns\":%" PRIu64
               ",\"submit_wait_min_ns\":%" PRIu64
               ",\"submit_wait_mean_ns\":%" PRIu64
               ",\"submit_wait_max_ns\":%" PRIu64
               ",\"mismatched_words\":%" PRIu64 "}",
               benchmark->warmup_iterations,
               benchmark->measured_iterations,
               benchmark->control_total_ns,
               benchmark->control_min_ns,
               benchmark->control_total_ns /
                   benchmark->measured_iterations,
               benchmark->control_max_ns,
               benchmark->submit_wait_total_ns,
               benchmark->submit_wait_min_ns,
               benchmark->submit_wait_total_ns /
                   benchmark->measured_iterations,
               benchmark->submit_wait_max_ns,
               benchmark->mismatched_words);
    }
    if (activity != NULL) {
        printf(",\"activity_status\":{\"ingress_configured\":%s"
               ",\"authenticated_event_count\":%" PRIu32
               ",\"rejected_event_count\":%" PRIu32
               ",\"last_sequence\":%" PRIu32
               ",\"last_event\":%" PRIu32
               ",\"state_flags\":%" PRIu32
               ",\"created\":%s,\"started\":%s,\"resumed\":%s"
               ",\"window_present\":%s,\"renderer_ready\":%s"
               ",\"focused\":%s,\"destroyed\":%s"
               ",\"width\":%" PRIu32 ",\"height\":%" PRIu32
               ",\"activity_pid\":%" PRIu32
               ",\"last_event_monotonic_ns\":%" PRIu64
               ",\"last_event_received_ns\":%" PRIu64 "}",
               activity->ingress_configured != 0U ? "true" : "false",
               activity->authenticated_event_count,
               activity->rejected_event_count, activity->last_sequence,
               activity->last_event, activity->state_flags,
               (activity->state_flags & BVB_ACTIVITY_CREATED) != 0U ? "true"
                                                                    : "false",
               (activity->state_flags & BVB_ACTIVITY_STARTED) != 0U ? "true"
                                                                    : "false",
               (activity->state_flags & BVB_ACTIVITY_RESUMED) != 0U ? "true"
                                                                    : "false",
               (activity->state_flags & BVB_ACTIVITY_WINDOW_PRESENT) != 0U
                   ? "true"
                   : "false",
               (activity->state_flags & BVB_ACTIVITY_RENDERER_READY) != 0U
                   ? "true"
                   : "false",
               (activity->state_flags & BVB_ACTIVITY_FOCUSED) != 0U ? "true"
                                                                    : "false",
               (activity->state_flags & BVB_ACTIVITY_DESTROYED) != 0U ? "true"
                                                                      : "false",
               activity->width, activity->height, activity->activity_pid,
               activity->last_event_monotonic_ns,
               activity->last_event_received_ns);
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

    struct bvb_vulkan_selftest_result selftest;
    struct bvb_vulkan_selftest_result *selftest_pointer = NULL;
    struct shared_batch_benchmark_result benchmark;
    struct shared_batch_benchmark_result *benchmark_pointer = NULL;
    if (options.request_vulkan_selftest) {
        memset(&request, 0, sizeof(request));
        request.header.version = BVB_PROTOCOL_VERSION;
        request.header.kind = BVB_PROTOCOL_REQUEST;
        request.header.opcode = BVB_OPCODE_VULKAN_SELFTEST;
        request.header.request_id = 0x42564203U;

        struct bvb_protocol_packet selftest_packet;
        result = exchange(socket_fd, &request, &selftest_packet);
        if (result != 0 || selftest_packet.header.status != 0 ||
            selftest_packet.header.payload_length != BVB_VULKAN_SELFTEST_SIZE) {
            (void)close(socket_fd);
            fputs("bvb: Vulkan self-test request failed\n", stderr);
            return 7;
        }
        result = bvb_protocol_decode_vulkan_selftest(selftest_packet.payload,
                                                     &selftest);
        if (result != 0) {
            (void)close(socket_fd);
            fputs("bvb: invalid Vulkan self-test response\n", stderr);
            return 7;
        }
        selftest_pointer = &selftest;
    }

    if (options.request_vulkan_batch_selftest) {
        memset(&request, 0, sizeof(request));
        request.header.version = BVB_PROTOCOL_VERSION;
        request.header.kind = BVB_PROTOCOL_REQUEST;
        request.header.opcode = BVB_OPCODE_VULKAN_BATCH_SELFTEST;
        request.header.request_id = 0x42564205U;

        size_t batch_length = 0U;
        result = build_transfer_batch(request.payload, sizeof(request.payload),
                                      1U, &batch_length);
        if (result != 0 || batch_length > UINT32_MAX) {
            (void)close(socket_fd);
            fputs("bvb: could not construct Vulkan command batch\n", stderr);
            return 7;
        }
        request.header.payload_length = (uint32_t)batch_length;

        struct bvb_protocol_packet selftest_packet;
        result = exchange(socket_fd, &request, &selftest_packet);
        if (result != 0 || selftest_packet.header.status != 0 ||
            selftest_packet.header.payload_length != BVB_VULKAN_SELFTEST_SIZE) {
            (void)close(socket_fd);
            fputs("bvb: Vulkan batch self-test request failed\n", stderr);
            return 7;
        }
        result = bvb_protocol_decode_vulkan_selftest(selftest_packet.payload,
                                                     &selftest);
        if (result != 0) {
            (void)close(socket_fd);
            fputs("bvb: invalid Vulkan batch self-test response\n", stderr);
            return 7;
        }
        selftest_pointer = &selftest;
    }

    if (options.request_vulkan_shared_batch_selftest ||
        options.request_vulkan_shared_batch_benchmark) {
        if (options.request_vulkan_shared_batch_benchmark) {
            benchmark_pointer = &benchmark;
        }
        result = request_shared_batch_selftest(
            socket_fd, options.shared_batch_iterations, &selftest,
            benchmark_pointer);
        if (result != 0) {
            (void)close(socket_fd);
            fprintf(stderr, "bvb: Vulkan shared-batch self-test failed: %s\n",
                    strerror(-result));
            return 7;
        }
        selftest_pointer = &selftest;
    }

    struct bvb_activity_status activity_status;
    struct bvb_activity_status *activity_status_pointer = NULL;
    if (options.request_activity_status) {
        memset(&request, 0, sizeof(request));
        request.header.version = BVB_PROTOCOL_VERSION;
        request.header.kind = BVB_PROTOCOL_REQUEST;
        request.header.opcode = BVB_OPCODE_ACTIVITY_STATUS;
        request.header.request_id = 0x42564204U;

        struct bvb_protocol_packet activity_packet;
        result = exchange(socket_fd, &request, &activity_packet);
        if (result != 0 || activity_packet.header.status != 0 ||
            activity_packet.header.payload_length != BVB_ACTIVITY_STATUS_SIZE) {
            (void)close(socket_fd);
            fputs("bvb: Activity status request failed\n", stderr);
            return 8;
        }
        result = bvb_protocol_decode_activity_status(activity_packet.payload,
                                                     &activity_status);
        if (result != 0) {
            (void)close(socket_fd);
            fputs("bvb: invalid Activity status response\n", stderr);
            return 8;
        }
        activity_status_pointer = &activity_status;
    }

    (void)close(socket_fd);
    const char *selftest_key =
        (options.request_vulkan_shared_batch_selftest ||
         options.request_vulkan_shared_batch_benchmark)
                                   ? "vulkan_shared_batch_selftest"
                               : options.request_vulkan_batch_selftest
                                   ? "vulkan_batch_selftest"
                                   : "vulkan_selftest";
    print_document(&hello_packet, &hello, caps_pointer, selftest_pointer,
                   selftest_key, benchmark_pointer,
                   activity_status_pointer);
    return 0;
}
