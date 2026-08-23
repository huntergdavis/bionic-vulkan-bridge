#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define VK_NO_PROTOTYPES

#include <bvb/command_batch.h>
#include <bvb/descriptor_transaction_ring.h>
#include <bvb/first_rejection.h>
#include <bvb/global_dispatch.h>
#include <bvb/handle.h>
#include <bvb/protocol.h>
#include <bvb/transport.h>
#include <bvb/vulkan_descriptor_wire.h>
#include <bvb/vulkan_discovery.h>
#include <bvb/vulkan_pipeline_wire.h>

#include <vulkan/vk_icd.h>
#include <vulkan/vk_layer.h>

#ifndef VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME
#define VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME     \
    "VK_ANDROID_external_memory_android_hardware_buffer"
#endif

#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static const uint64_t BVB_INSTANCE_PROXY_MAGIC =
    UINT64_C(0x425642494e535430);
static const uint64_t BVB_PHYSICAL_DEVICE_PROXY_MAGIC =
    UINT64_C(0x4256425048595330);
static const uint64_t BVB_DEVICE_PROXY_MAGIC =
    UINT64_C(0x4256424445563030);
static const uint64_t BVB_QUEUE_PROXY_MAGIC =
    UINT64_C(0x4256425155453030);
static const uint64_t BVB_COMMAND_BUFFER_PROXY_MAGIC =
    UINT64_C(0x425642434d443030);
enum { BVB_COMMAND_OWNERSHIP_CACHE_CAPACITY = 16U };

struct bvb_command_ownership_cache_entry {
    uint64_t wire_id;
    enum bvb_object_type type;
};

struct bvb_instance_proxy {
    const void *dispatch;
    uint64_t magic;
    uint64_t wire_id;
};

struct bvb_physical_device_proxy {
    const void *dispatch;
    uint64_t magic;
    uint64_t wire_id;
    uint64_t parent_id;
    VkExtensionProperties *device_extensions;
    uint32_t device_extension_count;
    bool device_extensions_valid;
    struct bvb_physical_device_proxy *next;
};

struct bvb_device_proxy {
    const void *dispatch;
    uint64_t magic;
    uint64_t wire_id;
    uint64_t parent_id;
    uint64_t instance_id;
    PFN_vkSetDeviceLoaderData set_loader_data;
    bool virtual_swapchain_enabled;
    struct bvb_device_proxy *next;
};

struct bvb_queue_proxy {
    const void *dispatch;
    uint64_t magic;
    uint64_t wire_id;
    uint64_t parent_id;
    bool loader_data_initialized;
    struct bvb_queue_proxy *next;
};

struct bvb_command_pool_proxy {
    uint64_t wire_id;
    uint64_t parent_id;
    struct bvb_command_pool_proxy *next;
};

struct bvb_command_buffer_proxy {
    const void *dispatch;
    uint64_t magic;
    uint64_t wire_id;
    uint64_t parent_pool_id;
    uint64_t device_id;
    pthread_mutex_t stream_mutex;
    struct bvb_command_batch_builder stream_builder;
    struct bvb_command_ownership_cache_entry
        ownership_cache[BVB_COMMAND_OWNERSHIP_CACHE_CAPACITY];
    uint64_t ownership_registry_reads;
    uint64_t stream_sequence;
    uint32_t stream_length;
    uint32_t stream_slot;
    const char *diagnostic_rejection_entry;
    const char *diagnostic_rejection_reason;
    const char *diagnostic_rejection_shape;
    int diagnostic_rejection_status;
    bool stream_recording;
    bool stream_sealed;
    bool stream_uploaded;
    bool stream_error;
    struct bvb_command_buffer_proxy *next;
};

struct bvb_descriptor_template_client_metadata {
    uint32_t entry_count;
    struct bvb_vulkan_descriptor_update_template_entry
        entries[BVB_VULKAN_MAX_DESCRIPTOR_UPDATE_TEMPLATE_ENTRIES];
};

struct bvb_resource_proxy {
    uint64_t wire_id;
    uint64_t parent_id;
    uint64_t allocation_size;
    uint64_t mapped_offset;
    uint64_t mapped_size;
    uint64_t mapped_generation;
    uint64_t bound_memory_id;
    uint8_t *mapped_bytes;
    uint32_t buffer_usage;
    uint32_t subtype;
    struct bvb_descriptor_template_client_metadata *descriptor_template;
    enum bvb_object_type type;
    bool memory_bound;
    bool mapped_shared;
    bool mapped_direct;
    struct bvb_resource_proxy *next;
};

struct bvb_surface_proxy {
    uint64_t wire_id;
    uint64_t parent_id;
    struct bvb_surface_proxy *next;
};

struct bvb_swapchain_proxy {
    uint64_t wire_id;
    uint64_t parent_id;
    uint64_t generation;
    uint32_t image_count;
    uint64_t image_ids[BVB_WSI_FRAME_RING_MAX_SLOTS];
    int control_fd;
    struct bvb_wsi_frame_ring *control;
    struct bvb_swapchain_proxy *next;
};

struct bvb_global_client_state {
    pthread_mutex_t mutex;
    pthread_mutex_t command_stream_slots_mutex;
    int socket_fd;
    uint32_t next_request_id;
    uint32_t service_flags;
    bool info_valid;
    struct bvb_vulkan_global_info info;
    struct bvb_physical_device_proxy *physical_devices;
    struct bvb_device_proxy *devices;
    struct bvb_queue_proxy *queues;
    struct bvb_command_pool_proxy *command_pools;
    struct bvb_command_buffer_proxy *command_buffers;
    struct bvb_resource_proxy *resources;
    struct bvb_surface_proxy *surfaces;
    struct bvb_swapchain_proxy *swapchains;
    uint64_t next_surface_serial;
    uint64_t next_swapchain_generation;
    uint8_t *command_stream_mapping;
    uint64_t command_stream_generation;
    uint64_t next_command_stream_sequence;
    uint64_t command_stream_slots[BVB_COMMAND_STREAM_SLOT_COUNT / 64U];
    uint8_t *descriptor_journal_mapping;
    uint64_t descriptor_journal_generation;
    uint64_t next_descriptor_journal_sequence;
    uint32_t descriptor_journal_length;
    uint32_t descriptor_journal_record_count;
    struct bvb_descriptor_transaction_ring *descriptor_transaction_ring;
    uint64_t descriptor_transaction_ring_generation;
    uint32_t next_descriptor_transaction_ring_sequence;
    atomic_bool command_stream_enabled;
    bool descriptor_journal_enabled;
    bool descriptor_journal_flushing;
    bool memory_mirror_enabled;
    bool memory_direct_enabled;
    bool frame_profile_enabled;
    bool connection_poisoned;
    uint64_t exchange_count;
    uint64_t descriptor_ring_call_count;
    uint64_t descriptor_lease_hit_count;
    uint64_t frame_profile_acquire_total_ns;
    uint64_t frame_profile_acquire_max_ns;
    uint64_t frame_profile_present_total_ns;
    uint64_t frame_profile_present_max_ns;
    uint32_t frame_profile_acquire_calls;
    uint32_t frame_profile_present_calls;
    bool frame_profile_rpc_window_started;
    uint32_t frame_profile_rpc_present_calls;
    uint64_t frame_profile_rpc_counts[BVB_OPCODE_LAST + 1U];
    uint64_t frame_profile_rpc_total_ns[BVB_OPCODE_LAST + 1U];
    uint64_t frame_profile_rpc_max_ns[BVB_OPCODE_LAST + 1U];
    uint64_t frame_profile_descriptor_ring_calls;
    uint64_t frame_profile_descriptor_ring_total_ns;
    uint64_t frame_profile_descriptor_ring_max_ns;
    uint64_t frame_profile_descriptor_lease_hits;
    uint64_t frame_profile_descriptor_lease_sets;
    uint64_t frame_profile_descriptor_lease_misses;
    uint16_t last_opcode;
};

static const uint64_t bvb_dispatch_anchor = UINT64_C(0x4256424449535030);
static atomic_bool bvb_icd_loader_active;
static struct bvb_global_client_state bvb_global_client = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .command_stream_slots_mutex = PTHREAD_MUTEX_INITIALIZER,
    .socket_fd = -1,
    .next_request_id = UINT32_C(0x42565000),
    .next_surface_serial = 1U,
    .next_swapchain_generation = 1U,
};
static pthread_rwlock_t bvb_object_registry_lock =
    PTHREAD_RWLOCK_INITIALIZER;

enum {
    BVB_CLIENT_LOW_MAP_START = UINT32_C(0x10000000),
    BVB_CLIENT_LOW_MAP_STRIDE = UINT32_C(0x01000000),
};

/*
 * Wine's 32-bit Vulkan thunk must represent the pointer returned by
 * vkMapMemory even though this ICD is a 64-bit AArch64 library. A normal
 * allocation on AArch64 usually lands above 4 GiB. Preserve that mapping as
 * a 64-bit fallback, but first search the low address space with
 * MAP_FIXED_NOREPLACE so no Wine or application mapping can be overwritten.
 */
static void *map_client_visible_memory_at_offset(
    int fd, size_t length, int protection, int flags, off_t offset) {
    if (length == 0U) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    void *normal = mmap(NULL, length, protection, flags, fd, offset);
    if (normal == MAP_FAILED) return MAP_FAILED;
    const uintptr_t normal_address = (uintptr_t)normal;
    if (normal_address <= UINT32_MAX &&
        length - 1U <= UINT32_MAX - normal_address) {
        return normal;
    }

    const uint64_t low_limit = UINT64_C(1) << 32;
    if ((uint64_t)length < low_limit) {
        const uint64_t last = low_limit - (uint64_t)length;
        for (uint64_t candidate = BVB_CLIENT_LOW_MAP_START;
             candidate <= last;
             candidate += BVB_CLIENT_LOW_MAP_STRIDE) {
            void *low = mmap(
                (void *)(uintptr_t)candidate, length, protection,
                flags | MAP_FIXED_NOREPLACE, fd, offset);
            if (low != MAP_FAILED) {
                (void)munmap(normal, length);
                return low;
            }
            if (errno != EEXIST && errno != EINVAL && errno != ENOMEM &&
                errno != EACCES) {
                break;
            }
        }
    }
    return normal;
}

static void *map_client_visible_memory(
    int fd, size_t length, int protection, int flags) {
    return map_client_visible_memory_at_offset(
        fd, length, protection, flags, 0);
}

static bool command_stream_is_enabled(void) {
    return atomic_load_explicit(&bvb_global_client.command_stream_enabled,
                                memory_order_acquire);
}

static bool memory_is_buffer_only_locked(uint64_t memory_id) {
    bool found = false;
    for (const struct bvb_resource_proxy *state = bvb_global_client.resources;
         state != NULL; state = state->next) {
        if (state->bound_memory_id != memory_id) continue;
        if (state->type != BVB_OBJECT_BUFFER) return false;
        found = true;
    }
    return found;
}

static const void *initial_dispatch_word(void) {
    return atomic_load(&bvb_icd_loader_active)
        ? (const void *)(uintptr_t)ICD_LOADER_MAGIC
        : &bvb_dispatch_anchor;
}

static bool valid_dispatch_word(const void *dispatch) {
    return dispatch != NULL;
}

BVB_GLOBAL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *name);

static PFN_vkVoidFunction erase_function(const void *bytes, size_t size) {
    PFN_vkVoidFunction result = NULL;
    if (bytes != NULL && size == sizeof(result)) {
        memcpy(&result, bytes, size);
    }
    return result;
}

#define BVB_ERASE_FUNCTION(function, type)                                     \
    erase_function(&(type){(function)}, sizeof(type))

static uint64_t frame_profile_monotonic_ns(void);
static void frame_profile_record_rpc_locked(
    uint16_t opcode, uint64_t started_ns, uint64_t finished_ns);
static int flush_descriptor_journal_locked(void);

static int flush_descriptor_journal_before_exchange_locked(uint16_t opcode) {
    if (bvb_global_client.descriptor_journal_flushing ||
        opcode == BVB_OPCODE_VULKAN_DESCRIPTOR_JOURNAL_SETUP ||
        opcode == BVB_OPCODE_VULKAN_DESCRIPTOR_JOURNAL_FLUSH ||
        opcode == BVB_OPCODE_VULKAN_DESCRIPTOR_TRANSACTION_ALLOCATE ||
        opcode == BVB_OPCODE_VULKAN_DESCRIPTOR_TRANSACTION_RING_SETUP) {
        return 0;
    }
    return flush_descriptor_journal_locked();
}

static int exchange_locked(const struct bvb_protocol_packet *request,
                           struct bvb_protocol_packet *response) {
    int result = flush_descriptor_journal_before_exchange_locked(
        request->header.opcode);
    if (result != 0) return result;
    ++bvb_global_client.exchange_count;
    bvb_global_client.last_opcode = request->header.opcode;
    const uint64_t profile_started_ns =
        bvb_global_client.frame_profile_enabled
        ? frame_profile_monotonic_ns() : 0U;
    result = bvb_transport_send(bvb_global_client.socket_fd, request);
    if (result == 0) {
        result = bvb_transport_receive(bvb_global_client.socket_fd, response);
    }
    const uint64_t profile_finished_ns =
        bvb_global_client.frame_profile_enabled
        ? frame_profile_monotonic_ns() : 0U;
    frame_profile_record_rpc_locked(
        request->header.opcode, profile_started_ns, profile_finished_ns);
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

static int exchange_fds_locked(
    const struct bvb_protocol_packet *request,
    struct bvb_protocol_packet *response, int *received_fds,
    size_t fd_capacity, size_t *received_fd_count) {
    int result = flush_descriptor_journal_before_exchange_locked(
        request->header.opcode);
    if (result != 0) return result;
    ++bvb_global_client.exchange_count;
    bvb_global_client.last_opcode = request->header.opcode;
    const uint64_t profile_started_ns =
        bvb_global_client.frame_profile_enabled
        ? frame_profile_monotonic_ns() : 0U;
    result = bvb_transport_send(bvb_global_client.socket_fd, request);
    if (result == 0)
        result = bvb_transport_receive_fds(
            bvb_global_client.socket_fd, response, received_fds,
            fd_capacity, received_fd_count);
    const uint64_t profile_finished_ns =
        bvb_global_client.frame_profile_enabled
        ? frame_profile_monotonic_ns() : 0U;
    frame_profile_record_rpc_locked(
        request->header.opcode, profile_started_ns, profile_finished_ns);
    if (result != 0) return result;
    if (response->header.kind != BVB_PROTOCOL_RESPONSE ||
        response->header.opcode != request->header.opcode ||
        response->header.request_id != request->header.request_id)
        return -EPROTO;
    return 0;
}

static int exchange_pass_fd_locked(
    const struct bvb_protocol_packet *request,
    struct bvb_protocol_packet *response, int passed_fd) {
    int result = flush_descriptor_journal_before_exchange_locked(
        request->header.opcode);
    if (result != 0) return result;
    ++bvb_global_client.exchange_count;
    bvb_global_client.last_opcode = request->header.opcode;
    const uint64_t profile_started_ns =
        bvb_global_client.frame_profile_enabled
        ? frame_profile_monotonic_ns() : 0U;
    result = bvb_transport_send_fd(
        bvb_global_client.socket_fd, request, passed_fd);
    if (result == 0) {
        result = bvb_transport_receive(bvb_global_client.socket_fd, response);
    }
    const uint64_t profile_finished_ns =
        bvb_global_client.frame_profile_enabled
        ? frame_profile_monotonic_ns() : 0U;
    frame_profile_record_rpc_locked(
        request->header.opcode, profile_started_ns, profile_finished_ns);
    if (result != 0) return result;
    if (response->header.kind != BVB_PROTOCOL_RESPONSE ||
        response->header.opcode != request->header.opcode ||
        response->header.request_id != request->header.request_id) {
        return -EPROTO;
    }
    return 0;
}

static uint32_t next_request_id_locked(void) {
    ++bvb_global_client.next_request_id;
    if (bvb_global_client.next_request_id == 0U) {
        ++bvb_global_client.next_request_id;
    }
    return bvb_global_client.next_request_id;
}

static bool command_stream_requested(void) {
    const char *mode = getenv("BVB_COMMAND_STREAM");
    return mode != NULL && strcmp(mode, "shared") == 0;
}

static int mapped_memory_mode_requested(bool *shared, bool *direct) {
    if (shared == NULL || direct == NULL) return -EINVAL;
    const char *mode = getenv("BVB_MAPPED_MEMORY");
    if (mode == NULL || strcmp(mode, "strict") == 0) {
        *shared = false;
        *direct = false;
        return 0;
    }
    if (strcmp(mode, "shared") == 0) {
        *shared = true;
        *direct = false;
        return 0;
    }
    if (strcmp(mode, "direct") == 0) {
        *shared = true;
        *direct = true;
        return 0;
    }
    return -EINVAL;
}

static int frame_profile_requested(bool *enabled) {
    if (enabled == NULL) return -EINVAL;
    const char *value = getenv("BVB_FRAME_PROFILE");
    if (value == NULL || strcmp(value, "0") == 0) {
        *enabled = false;
        return 0;
    }
    if (strcmp(value, "1") == 0) {
        *enabled = true;
        return 0;
    }
    return -EINVAL;
}

static int descriptor_journal_shared_requested(bool *shared) {
    if (shared == NULL) return -EINVAL;
    const char *mode = getenv("BVB_DESCRIPTOR_JOURNAL");
    if (mode == NULL || strcmp(mode, "strict") == 0) {
        *shared = false;
        return 0;
    }
    if (strcmp(mode, "shared") == 0) {
        *shared = true;
        return 0;
    }
    return -EINVAL;
}

static uint64_t frame_profile_monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0U;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static void frame_profile_reset_rpc_window_locked(void) {
    memset(bvb_global_client.frame_profile_rpc_counts, 0,
           sizeof(bvb_global_client.frame_profile_rpc_counts));
    memset(bvb_global_client.frame_profile_rpc_total_ns, 0,
           sizeof(bvb_global_client.frame_profile_rpc_total_ns));
    memset(bvb_global_client.frame_profile_rpc_max_ns, 0,
           sizeof(bvb_global_client.frame_profile_rpc_max_ns));
    bvb_global_client.frame_profile_rpc_present_calls = 0U;
    bvb_global_client.frame_profile_descriptor_ring_calls = 0U;
    bvb_global_client.frame_profile_descriptor_ring_total_ns = 0U;
    bvb_global_client.frame_profile_descriptor_ring_max_ns = 0U;
    bvb_global_client.frame_profile_descriptor_lease_hits = 0U;
    bvb_global_client.frame_profile_descriptor_lease_sets = 0U;
    bvb_global_client.frame_profile_descriptor_lease_misses = 0U;
}

static void frame_profile_emit_rpc_summary_locked(void) {
    enum { BVB_FRAME_PROFILE_RPC_TOP_COUNT = 8U,
           BVB_FRAME_PROFILE_RPC_LINE_BYTES = 1024U };
    uint64_t total_calls = 0U;
    uint64_t total_ns = 0U;
    for (uint16_t opcode = 1U; opcode <= BVB_OPCODE_LAST; ++opcode) {
        total_calls += bvb_global_client.frame_profile_rpc_counts[opcode];
        total_ns += bvb_global_client.frame_profile_rpc_total_ns[opcode];
    }
    char line[BVB_FRAME_PROFILE_RPC_LINE_BYTES];
    int written = snprintf(
        line, sizeof(line),
        "BVB_E117_RPC_PROFILE present_calls=%u total_calls=%llu "
        "total_ns=%llu top=",
        bvb_global_client.frame_profile_rpc_present_calls,
        (unsigned long long)total_calls, (unsigned long long)total_ns);
    if (written < 0) return;
    size_t used = (size_t)written;
    if (used >= sizeof(line)) used = sizeof(line) - 1U;
    bool selected[BVB_OPCODE_LAST + 1U] = {false};
    for (uint32_t rank = 0U; rank < BVB_FRAME_PROFILE_RPC_TOP_COUNT; ++rank) {
        uint16_t best_opcode = 0U;
        uint64_t best_total_ns = 0U;
        for (uint16_t opcode = 1U; opcode <= BVB_OPCODE_LAST; ++opcode) {
            if (!selected[opcode] &&
                bvb_global_client.frame_profile_rpc_counts[opcode] != 0U &&
                (best_opcode == 0U ||
                 bvb_global_client.frame_profile_rpc_total_ns[opcode] >
                     best_total_ns)) {
                best_opcode = opcode;
                best_total_ns =
                    bvb_global_client.frame_profile_rpc_total_ns[opcode];
            }
        }
        if (best_opcode == 0U || used >= sizeof(line) - 2U) break;
        selected[best_opcode] = true;
        written = snprintf(
            line + used, sizeof(line) - used,
            "%s%u/%llu/%llu/%llu", rank == 0U ? "" : ",",
            best_opcode,
            (unsigned long long)
                bvb_global_client.frame_profile_rpc_counts[best_opcode],
            (unsigned long long)best_total_ns,
            (unsigned long long)
                bvb_global_client.frame_profile_rpc_max_ns[best_opcode]);
        if (written < 0) break;
        if ((size_t)written >= sizeof(line) - used) {
            used = sizeof(line) - 1U;
            break;
        }
        used += (size_t)written;
    }
    if (used < sizeof(line) - 1U) line[used++] = '\n';
    const ssize_t ignored = write(STDERR_FILENO, line, used);
    (void)ignored;
    char ring_line[256];
    const int ring_written = snprintf(
        ring_line, sizeof(ring_line),
        "BVB_E128_DESCRIPTOR_RING_PROFILE present_calls=%u calls=%llu "
        "total_ns=%llu max_ns=%llu\n",
        bvb_global_client.frame_profile_rpc_present_calls,
        (unsigned long long)
            bvb_global_client.frame_profile_descriptor_ring_calls,
        (unsigned long long)
            bvb_global_client.frame_profile_descriptor_ring_total_ns,
        (unsigned long long)
            bvb_global_client.frame_profile_descriptor_ring_max_ns);
    if (ring_written > 0) {
        const size_t ring_length = (size_t)ring_written < sizeof(ring_line)
            ? (size_t)ring_written : sizeof(ring_line) - 1U;
        const ssize_t ring_ignored = write(
            STDERR_FILENO, ring_line, ring_length);
        (void)ring_ignored;
    }
    char lease_line[256];
    const int lease_written = snprintf(
        lease_line, sizeof(lease_line),
        "BVB_E131_DESCRIPTOR_LEASE_PROFILE present_calls=%u hits=%llu "
        "sets=%llu misses=%llu\n",
        bvb_global_client.frame_profile_rpc_present_calls,
        (unsigned long long)
            bvb_global_client.frame_profile_descriptor_lease_hits,
        (unsigned long long)
            bvb_global_client.frame_profile_descriptor_lease_sets,
        (unsigned long long)
            bvb_global_client.frame_profile_descriptor_lease_misses);
    if (lease_written > 0) {
        const size_t lease_length = (size_t)lease_written < sizeof(lease_line)
            ? (size_t)lease_written : sizeof(lease_line) - 1U;
        const ssize_t lease_ignored = write(
            STDERR_FILENO, lease_line, lease_length);
        (void)lease_ignored;
    }
}

static void frame_profile_record_descriptor_ring_locked(
    uint64_t started_ns, uint64_t finished_ns) {
    if (!bvb_global_client.frame_profile_enabled ||
        !bvb_global_client.frame_profile_rpc_window_started ||
        started_ns == 0U || finished_ns < started_ns) {
        return;
    }
    const uint64_t elapsed_ns = finished_ns - started_ns;
    ++bvb_global_client.frame_profile_descriptor_ring_calls;
    bvb_global_client.frame_profile_descriptor_ring_total_ns += elapsed_ns;
    if (elapsed_ns > bvb_global_client.frame_profile_descriptor_ring_max_ns) {
        bvb_global_client.frame_profile_descriptor_ring_max_ns = elapsed_ns;
    }
}

static void frame_profile_record_rpc_locked(
    uint16_t opcode, uint64_t started_ns, uint64_t finished_ns) {
    if (!bvb_global_client.frame_profile_enabled || opcode == 0U ||
        opcode > BVB_OPCODE_LAST || started_ns == 0U ||
        finished_ns < started_ns) return;
    if (!bvb_global_client.frame_profile_rpc_window_started) {
        if (opcode != BVB_OPCODE_VULKAN_SWAPCHAIN_PRESENT) return;
        frame_profile_reset_rpc_window_locked();
        bvb_global_client.frame_profile_rpc_window_started = true;
    }
    const uint64_t elapsed_ns = finished_ns - started_ns;
    ++bvb_global_client.frame_profile_rpc_counts[opcode];
    bvb_global_client.frame_profile_rpc_total_ns[opcode] += elapsed_ns;
    if (elapsed_ns > bvb_global_client.frame_profile_rpc_max_ns[opcode])
        bvb_global_client.frame_profile_rpc_max_ns[opcode] = elapsed_ns;
    if (opcode != BVB_OPCODE_VULKAN_SWAPCHAIN_PRESENT) return;
    ++bvb_global_client.frame_profile_rpc_present_calls;
    if (bvb_global_client.frame_profile_rpc_present_calls < 32U) return;
    frame_profile_emit_rpc_summary_locked();
    frame_profile_reset_rpc_window_locked();
}

static void frame_profile_record_client_exchange_locked(
    bool present, uint64_t started_ns, uint64_t finished_ns) {
    if (!bvb_global_client.frame_profile_enabled || started_ns == 0U ||
        finished_ns < started_ns) return;
    const uint64_t elapsed_ns = finished_ns - started_ns;
    if (present) {
        ++bvb_global_client.frame_profile_present_calls;
        bvb_global_client.frame_profile_present_total_ns += elapsed_ns;
        if (elapsed_ns > bvb_global_client.frame_profile_present_max_ns)
            bvb_global_client.frame_profile_present_max_ns = elapsed_ns;
    } else {
        ++bvb_global_client.frame_profile_acquire_calls;
        bvb_global_client.frame_profile_acquire_total_ns += elapsed_ns;
        if (elapsed_ns > bvb_global_client.frame_profile_acquire_max_ns)
            bvb_global_client.frame_profile_acquire_max_ns = elapsed_ns;
    }
    if (!present || bvb_global_client.frame_profile_present_calls < 32U)
        return;
    fprintf(stderr,
            "BVB_E116_WSI_CLIENT_PROFILE present_calls=%u "
            "present_total_ns=%llu present_max_ns=%llu acquire_calls=%u "
            "acquire_total_ns=%llu acquire_max_ns=%llu\n",
            bvb_global_client.frame_profile_present_calls,
            (unsigned long long)bvb_global_client.frame_profile_present_total_ns,
            (unsigned long long)bvb_global_client.frame_profile_present_max_ns,
            bvb_global_client.frame_profile_acquire_calls,
            (unsigned long long)bvb_global_client.frame_profile_acquire_total_ns,
            (unsigned long long)bvb_global_client.frame_profile_acquire_max_ns);
    bvb_global_client.frame_profile_present_calls = 0U;
    bvb_global_client.frame_profile_present_total_ns = 0U;
    bvb_global_client.frame_profile_present_max_ns = 0U;
    bvb_global_client.frame_profile_acquire_calls = 0U;
    bvb_global_client.frame_profile_acquire_total_ns = 0U;
    bvb_global_client.frame_profile_acquire_max_ns = 0U;
}

static int setup_command_stream_locked(void) {
    if (!command_stream_requested()) return 0;
    int memory_fd = -1;
    void *mapping = MAP_FAILED;
    uint64_t generation = 0U;
    const ssize_t random_bytes = syscall(
        SYS_getrandom, &generation, sizeof(generation), 0);
    if (random_bytes != (ssize_t)sizeof(generation) || generation == 0U) {
        return -EIO;
    }
    memory_fd = (int)syscall(
        SYS_memfd_create, "bvb-command-stream",
        MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (memory_fd < 0) return -errno;
    int result = 0;
    if (ftruncate(memory_fd, BVB_COMMAND_STREAM_REGION_BYTES) != 0) {
        result = -errno;
        goto done;
    }
    mapping = mmap(NULL, BVB_COMMAND_STREAM_REGION_BYTES,
                   PROT_READ | PROT_WRITE, MAP_SHARED, memory_fd, 0);
    if (mapping == MAP_FAILED) {
        result = -errno;
        goto done;
    }
    if (fcntl(memory_fd, F_ADD_SEALS,
              F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0) {
        result = -errno;
        goto done;
    }
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_COMMAND_STREAM_SETUP,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_SHARED_BATCH_SETUP_SIZE,
    };
    const struct bvb_shared_batch_setup setup = {
        .region_bytes = BVB_COMMAND_STREAM_REGION_BYTES,
        .generation = generation,
    };
    result = bvb_protocol_encode_vulkan_command_stream_setup(
        request.payload, &setup);
    struct bvb_protocol_packet response = {0};
    if (result == 0) {
        result = exchange_pass_fd_locked(&request, &response, memory_fd);
    }
    if (result == 0 &&
        (response.header.status != 0 || response.header.payload_length != 0U)) {
        result = response.header.status != 0 ? response.header.status
                                             : -EPROTO;
    }
    if (result == 0) {
        bvb_global_client.command_stream_mapping = mapping;
        bvb_global_client.command_stream_generation = generation;
        bvb_global_client.next_command_stream_sequence = 1U;
        atomic_store_explicit(&bvb_global_client.command_stream_enabled, true,
                              memory_order_release);
        mapping = MAP_FAILED;
    }
done:
    if (mapping != MAP_FAILED) {
        (void)munmap(mapping, BVB_COMMAND_STREAM_REGION_BYTES);
    }
    (void)close(memory_fd);
    return result;
}

static int setup_descriptor_journal_locked(void) {
    if (!bvb_global_client.descriptor_journal_enabled) return 0;
    int memory_fd = -1;
    void *mapping = MAP_FAILED;
    uint64_t generation = 0U;
    const ssize_t random_bytes = syscall(
        SYS_getrandom, &generation, sizeof(generation), 0);
    if (random_bytes != (ssize_t)sizeof(generation) || generation == 0U) {
        return -EIO;
    }
    memory_fd = (int)syscall(
        SYS_memfd_create, "bvb-descriptor-journal",
        MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (memory_fd < 0) return -errno;
    int result = 0;
    if (ftruncate(memory_fd, BVB_DESCRIPTOR_JOURNAL_REGION_BYTES) != 0) {
        result = -errno;
        goto done;
    }
    mapping = mmap(NULL, BVB_DESCRIPTOR_JOURNAL_REGION_BYTES,
                   PROT_READ | PROT_WRITE, MAP_SHARED, memory_fd, 0);
    if (mapping == MAP_FAILED) {
        result = -errno;
        goto done;
    }
    if (fcntl(memory_fd, F_ADD_SEALS,
              F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0) {
        result = -errno;
        goto done;
    }
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_DESCRIPTOR_JOURNAL_SETUP,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_SHARED_BATCH_SETUP_SIZE,
    };
    const struct bvb_shared_batch_setup setup = {
        .region_bytes = BVB_DESCRIPTOR_JOURNAL_REGION_BYTES,
        .generation = generation,
    };
    result = bvb_protocol_encode_vulkan_descriptor_journal_setup(
        request.payload, &setup);
    struct bvb_protocol_packet response = {0};
    if (result == 0) {
        result = exchange_pass_fd_locked(&request, &response, memory_fd);
    }
    if (result == 0 &&
        (response.header.status != 0 || response.header.payload_length != 0U)) {
        result = response.header.status != 0 ? response.header.status
                                             : -EPROTO;
    }
    if (result == 0) {
        bvb_global_client.descriptor_journal_mapping = mapping;
        bvb_global_client.descriptor_journal_generation = generation;
        bvb_global_client.next_descriptor_journal_sequence = 1U;
        bvb_global_client.descriptor_journal_length = 0U;
        bvb_global_client.descriptor_journal_record_count = 0U;
        mapping = MAP_FAILED;
    }
done:
    if (mapping != MAP_FAILED) {
        (void)munmap(mapping, BVB_DESCRIPTOR_JOURNAL_REGION_BYTES);
    }
    (void)close(memory_fd);
    return result;
}

static int setup_descriptor_transaction_ring_locked(void) {
    if (!bvb_global_client.descriptor_journal_enabled ||
        bvb_global_client.descriptor_transaction_ring != NULL) {
        return bvb_global_client.descriptor_journal_enabled ? 0 : -EINVAL;
    }
    int memory_fd = -1;
    void *mapping = MAP_FAILED;
    uint64_t generation = 0U;
    const ssize_t random_bytes = syscall(
        SYS_getrandom, &generation, sizeof(generation), 0);
    if (random_bytes != (ssize_t)sizeof(generation) || generation == 0U) {
        return -EIO;
    }
    memory_fd = (int)syscall(
        SYS_memfd_create, "bvb-descriptor-transaction-ring",
        MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (memory_fd < 0) return -errno;
    int result = 0;
    if (ftruncate(memory_fd,
                  BVB_DESCRIPTOR_TRANSACTION_RING_REGION_BYTES) != 0) {
        result = -errno;
        goto done;
    }
    mapping = mmap(NULL, BVB_DESCRIPTOR_TRANSACTION_RING_REGION_BYTES,
                   PROT_READ | PROT_WRITE, MAP_SHARED, memory_fd, 0);
    if (mapping == MAP_FAILED) {
        result = -errno;
        goto done;
    }
    result = bvb_descriptor_transaction_ring_initialize(
        mapping, BVB_DESCRIPTOR_TRANSACTION_RING_REGION_BYTES,
        BVB_DESCRIPTOR_TRANSACTION_RING_SLOT_COUNT, generation);
    if (result != 0) goto done;
    if (fcntl(memory_fd, F_ADD_SEALS,
              F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0) {
        result = -errno;
        goto done;
    }
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_DESCRIPTOR_TRANSACTION_RING_SETUP,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_SHARED_BATCH_SETUP_SIZE,
    };
    const struct bvb_shared_batch_setup setup = {
        .region_bytes = BVB_DESCRIPTOR_TRANSACTION_RING_REGION_BYTES,
        .generation = generation,
    };
    result = bvb_protocol_encode_shared_batch_setup(request.payload, &setup);
    struct bvb_protocol_packet response = {0};
    if (result == 0) {
        result = exchange_pass_fd_locked(&request, &response, memory_fd);
    }
    if (result == 0 &&
        (response.header.status != 0 || response.header.payload_length != 0U)) {
        result = response.header.status != 0 ? response.header.status
                                             : -EPROTO;
    }
    if (result == 0) {
        bvb_global_client.descriptor_transaction_ring = mapping;
        bvb_global_client.descriptor_transaction_ring_generation = generation;
        bvb_global_client.next_descriptor_transaction_ring_sequence = 1U;
        mapping = MAP_FAILED;
    }
done:
    if (mapping != MAP_FAILED) {
        (void)munmap(mapping,
                     BVB_DESCRIPTOR_TRANSACTION_RING_REGION_BYTES);
    }
    (void)close(memory_fd);
    return result;
}

static void poison_descriptor_journal_connection_locked(void) {
    bvb_global_client.connection_poisoned = true;
    if (bvb_global_client.socket_fd >= 0) {
        (void)close(bvb_global_client.socket_fd);
        bvb_global_client.socket_fd = -1;
    }
}

static int flush_descriptor_journal_locked(void) {
    if (!bvb_global_client.descriptor_journal_enabled ||
        bvb_global_client.descriptor_journal_length == 0U) {
        return 0;
    }
    if (bvb_global_client.socket_fd < 0 ||
        bvb_global_client.descriptor_journal_mapping == NULL ||
        bvb_global_client.next_descriptor_journal_sequence == 0U) {
        poison_descriptor_journal_connection_locked();
        return -EPIPE;
    }
    atomic_thread_fence(memory_order_release);
    const struct bvb_descriptor_journal_flush flush = {
        .generation = bvb_global_client.descriptor_journal_generation,
        .sequence = bvb_global_client.next_descriptor_journal_sequence,
        .length = bvb_global_client.descriptor_journal_length,
        .record_count = bvb_global_client.descriptor_journal_record_count,
    };
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_DESCRIPTOR_JOURNAL_FLUSH,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_DESCRIPTOR_JOURNAL_FLUSH_SIZE,
    };
    int result = bvb_protocol_encode_vulkan_descriptor_journal_flush(
        request.payload, &flush);
    struct bvb_protocol_packet response = {0};
    if (result == 0) {
        bvb_global_client.descriptor_journal_flushing = true;
        result = exchange_locked(&request, &response);
        bvb_global_client.descriptor_journal_flushing = false;
    }
    if (result == 0 &&
        (response.header.status != 0 || response.header.payload_length != 0U)) {
        result = response.header.status != 0 ? response.header.status
                                             : -EPROTO;
    }
    if (result != 0) {
        poison_descriptor_journal_connection_locked();
        return result;
    }
    bvb_global_client.descriptor_journal_length = 0U;
    bvb_global_client.descriptor_journal_record_count = 0U;
    ++bvb_global_client.next_descriptor_journal_sequence;
    if (bvb_global_client.next_descriptor_journal_sequence == 0U) {
        poison_descriptor_journal_connection_locked();
        return -EOVERFLOW;
    }
    return 0;
}

static int append_descriptor_journal_locked(
    const uint8_t *payload, uint32_t payload_length) {
    if (!bvb_global_client.descriptor_journal_enabled || payload == NULL ||
        payload_length == 0U || payload_length > BVB_PROTOCOL_MAX_PAYLOAD ||
        bvb_global_client.descriptor_journal_mapping == NULL) {
        return -EINVAL;
    }
    const uint32_t unaligned =
        BVB_DESCRIPTOR_JOURNAL_RECORD_HEADER_SIZE + payload_length;
    const uint32_t record_length = (unaligned + 7U) & ~UINT32_C(7);
    if (record_length > BVB_DESCRIPTOR_JOURNAL_REGION_BYTES) return -E2BIG;
    if (bvb_global_client.descriptor_journal_record_count ==
            BVB_DESCRIPTOR_JOURNAL_MAX_RECORDS ||
        record_length > BVB_DESCRIPTOR_JOURNAL_REGION_BYTES -
                            bvb_global_client.descriptor_journal_length) {
        int result = flush_descriptor_journal_locked();
        if (result != 0) return result;
    }
    uint8_t *record = bvb_global_client.descriptor_journal_mapping +
        bvb_global_client.descriptor_journal_length;
    bvb_wire_put_u32(record, payload_length);
    bvb_wire_put_u32(record + 4, 0U);
    memcpy(record + BVB_DESCRIPTOR_JOURNAL_RECORD_HEADER_SIZE,
           payload, payload_length);
    memset(record + unaligned, 0, record_length - unaligned);
    bvb_global_client.descriptor_journal_length += record_length;
    ++bvb_global_client.descriptor_journal_record_count;
    return 0;
}

static int connect_locked(void) {
    if (bvb_global_client.connection_poisoned) return -EPIPE;
    bool memory_mirror_enabled = false;
    bool memory_direct_enabled = false;
    bool frame_profile_enabled = false;
    bool descriptor_journal_enabled = false;
    int result = mapped_memory_mode_requested(
        &memory_mirror_enabled, &memory_direct_enabled);
    if (result == 0)
        result = frame_profile_requested(&frame_profile_enabled);
    if (result == 0)
        result = descriptor_journal_shared_requested(
            &descriptor_journal_enabled);
    if (result != 0) return result;
    if (bvb_global_client.socket_fd >= 0) {
        return bvb_global_client.memory_mirror_enabled ==
                       memory_mirror_enabled &&
                   bvb_global_client.memory_direct_enabled ==
                       memory_direct_enabled &&
                   bvb_global_client.frame_profile_enabled ==
                       frame_profile_enabled &&
                   bvb_global_client.descriptor_journal_enabled ==
                       descriptor_journal_enabled
                   ? 0
                   : -EPROTO;
    }
    const char *socket_path = getenv("BVB_BRIDGE_SOCKET");
    if (socket_path == NULL || socket_path[0] != '/') {
        if (getenv("BVB_ICD_DIAGNOSTICS") != NULL) {
            fprintf(stderr,
                    "BVB_ICD_CONNECT path=%s euid=%lu status=%d\n",
                    socket_path == NULL ? "(null)" : socket_path,
                    (unsigned long)geteuid(), -ENOENT);
        }
        return -ENOENT;
    }
    int socket_fd = bvb_transport_connect(socket_path, geteuid());
    if (socket_fd < 0) {
        if (getenv("BVB_ICD_DIAGNOSTICS") != NULL) {
            fprintf(stderr,
                    "BVB_ICD_CONNECT path=%s euid=%lu status=%d\n",
                    socket_path, (unsigned long)geteuid(), socket_fd);
        }
        return socket_fd;
    }
    if (getenv("BVB_ICD_DIAGNOSTICS") != NULL) {
        fprintf(stderr, "BVB_ICD_CONNECT path=%s euid=%lu status=0\n",
                socket_path, (unsigned long)geteuid());
    }
    bvb_global_client.socket_fd = socket_fd;
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_HELLO,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_HELLO_REQUEST_SIZE,
    };
    const struct bvb_hello_request hello = {
        .minimum_version = BVB_PROTOCOL_VERSION,
        .maximum_version = BVB_PROTOCOL_VERSION,
    };
    result = bvb_protocol_encode_hello_request(request.payload, &hello);
    struct bvb_protocol_packet response = {0};
    if (result == 0) {
        result = exchange_locked(&request, &response);
    }
    struct bvb_hello_response decoded;
    if (result == 0 &&
        (response.header.status != 0 ||
         response.header.payload_length != BVB_HELLO_RESPONSE_SIZE)) {
        result = -EPROTO;
    }
    if (result == 0) {
        result = bvb_protocol_decode_hello_response(response.payload, &decoded);
    }
    if (getenv("BVB_ICD_DIAGNOSTICS") != NULL) {
        fprintf(stderr,
                "BVB_ICD_HELLO status=%d wire_status=%d payload=%u "
                "negotiated=%u\n",
                result, response.header.status,
                response.header.payload_length,
                result == 0 ? decoded.negotiated_version : 0U);
    }
    if (result != 0 || decoded.negotiated_version != BVB_PROTOCOL_VERSION) {
        (void)close(bvb_global_client.socket_fd);
        bvb_global_client.socket_fd = -1;
        return result != 0 ? result : -EPROTONOSUPPORT;
    }
    bvb_global_client.service_flags = decoded.service_flags;
    bvb_global_client.memory_mirror_enabled = memory_mirror_enabled;
    bvb_global_client.memory_direct_enabled = memory_direct_enabled;
    bvb_global_client.frame_profile_enabled = frame_profile_enabled;
    bvb_global_client.descriptor_journal_enabled =
        descriptor_journal_enabled;
    result = setup_command_stream_locked();
    if (result == 0) result = setup_descriptor_journal_locked();
    if (result != 0) {
        (void)close(bvb_global_client.socket_fd);
        bvb_global_client.socket_fd = -1;
        return result;
    }
    return 0;
}

static int global_info_locked(struct bvb_vulkan_global_info *info) {
    if (bvb_global_client.info_valid) {
        *info = bvb_global_client.info;
        return 0;
    }
    int result = connect_locked();
    if (result != 0) {
        return result;
    }
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_GLOBAL_INFO,
        .request_id = next_request_id_locked(),
    };
    struct bvb_protocol_packet response = {0};
    result = exchange_locked(&request, &response);
    if (result == 0 &&
        (response.header.status != 0 ||
         response.header.payload_length != BVB_VULKAN_GLOBAL_INFO_SIZE)) {
        result = -EPROTO;
    }
    if (result == 0) {
        result = bvb_protocol_decode_vulkan_global_info(response.payload,
                                                        &bvb_global_client.info);
    }
    if (result == 0) {
        bvb_global_client.info_valid = true;
        *info = bvb_global_client.info;
    }
    return result;
}

static struct bvb_instance_proxy *instance_proxy(VkInstance instance) {
    struct bvb_instance_proxy *proxy = (struct bvb_instance_proxy *)instance;
    if (proxy == NULL || !valid_dispatch_word(proxy->dispatch) ||
        proxy->magic != BVB_INSTANCE_PROXY_MAGIC ||
        bvb_handle_expect(proxy->wire_id, BVB_OBJECT_INSTANCE) != 0) {
        return NULL;
    }
    return proxy;
}

BVB_GLOBAL_EXPORT uint64_t bvb_instance_proxy_id(VkInstance instance) {
    struct bvb_instance_proxy *proxy = instance_proxy(instance);
    return proxy == NULL ? 0U : proxy->wire_id;
}

static struct bvb_physical_device_proxy *physical_device_proxy(
    VkPhysicalDevice physical_device) {
    struct bvb_physical_device_proxy *proxy =
        (struct bvb_physical_device_proxy *)physical_device;
    if (proxy == NULL || !valid_dispatch_word(proxy->dispatch) ||
        proxy->magic != BVB_PHYSICAL_DEVICE_PROXY_MAGIC ||
        bvb_handle_expect(proxy->wire_id, BVB_OBJECT_PHYSICAL_DEVICE) != 0 ||
        bvb_handle_expect(proxy->parent_id, BVB_OBJECT_INSTANCE) != 0) {
        return NULL;
    }
    return proxy;
}

BVB_GLOBAL_EXPORT uint64_t bvb_physical_device_proxy_id(
    VkPhysicalDevice physical_device) {
    struct bvb_physical_device_proxy *proxy =
        physical_device_proxy(physical_device);
    return proxy == NULL ? 0U : proxy->wire_id;
}

static struct bvb_physical_device_proxy *physical_proxy_locked(
    uint64_t wire_id, uint64_t parent_id) {
    for (struct bvb_physical_device_proxy *proxy =
             bvb_global_client.physical_devices;
         proxy != NULL; proxy = proxy->next) {
        if (proxy->wire_id == wire_id) {
            return proxy->parent_id == parent_id ? proxy : NULL;
        }
    }
    struct bvb_physical_device_proxy *proxy = calloc(1, sizeof(*proxy));
    if (proxy == NULL) {
        return NULL;
    }
    proxy->dispatch = initial_dispatch_word();
    proxy->magic = BVB_PHYSICAL_DEVICE_PROXY_MAGIC;
    proxy->wire_id = wire_id;
    proxy->parent_id = parent_id;
    proxy->next = bvb_global_client.physical_devices;
    bvb_global_client.physical_devices = proxy;
    return proxy;
}

static struct bvb_device_proxy *device_proxy(VkDevice device) {
    struct bvb_device_proxy *proxy = (struct bvb_device_proxy *)device;
    if (proxy == NULL || !valid_dispatch_word(proxy->dispatch) ||
        proxy->magic != BVB_DEVICE_PROXY_MAGIC ||
        bvb_handle_expect(proxy->wire_id, BVB_OBJECT_DEVICE) != 0 ||
        bvb_handle_expect(proxy->parent_id, BVB_OBJECT_PHYSICAL_DEVICE) != 0 ||
        bvb_handle_expect(proxy->instance_id, BVB_OBJECT_INSTANCE) != 0) {
        return NULL;
    }
    return proxy;
}

BVB_GLOBAL_EXPORT uint64_t bvb_device_proxy_id(VkDevice device) {
    struct bvb_device_proxy *proxy = device_proxy(device);
    return proxy == NULL ? 0U : proxy->wire_id;
}

static struct bvb_queue_proxy *queue_proxy(VkQueue queue) {
    struct bvb_queue_proxy *proxy = (struct bvb_queue_proxy *)queue;
    if (proxy == NULL || !valid_dispatch_word(proxy->dispatch) ||
        proxy->magic != BVB_QUEUE_PROXY_MAGIC ||
        bvb_handle_expect(proxy->wire_id, BVB_OBJECT_QUEUE) != 0 ||
        bvb_handle_expect(proxy->parent_id, BVB_OBJECT_DEVICE) != 0) {
        return NULL;
    }
    return proxy;
}

BVB_GLOBAL_EXPORT uint64_t bvb_queue_proxy_id(VkQueue queue) {
    struct bvb_queue_proxy *proxy = queue_proxy(queue);
    return proxy == NULL ? 0U : proxy->wire_id;
}

static uint64_t command_pool_wire_id(VkCommandPool command_pool) {
    uint64_t wire_id = 0U;
    _Static_assert(sizeof(command_pool) <= sizeof(wire_id),
                   "VkCommandPool exceeds bridge handle width");
    memcpy(&wire_id, &command_pool, sizeof(command_pool));
    return wire_id;
}

static struct bvb_command_pool_proxy *command_pool_proxy_locked(
    VkCommandPool command_pool) {
    const uint64_t wire_id = command_pool_wire_id(command_pool);
    if (bvb_handle_expect(wire_id, BVB_OBJECT_COMMAND_POOL) != 0) {
        return NULL;
    }
    for (struct bvb_command_pool_proxy *proxy =
             bvb_global_client.command_pools;
         proxy != NULL; proxy = proxy->next) {
        if (proxy->wire_id == wire_id) {
            return proxy;
        }
    }
    return NULL;
}

BVB_GLOBAL_EXPORT uint64_t bvb_command_pool_proxy_id(
    VkCommandPool command_pool) {
    uint64_t result = 0U;
    if (pthread_mutex_lock(&bvb_global_client.mutex) == 0) {
        struct bvb_command_pool_proxy *proxy =
            command_pool_proxy_locked(command_pool);
        result = proxy == NULL ? 0U : proxy->wire_id;
        (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    }
    return result;
}

static struct bvb_command_buffer_proxy *command_buffer_proxy(
    VkCommandBuffer command_buffer) {
    struct bvb_command_buffer_proxy *proxy =
        (struct bvb_command_buffer_proxy *)command_buffer;
    if (proxy == NULL || !valid_dispatch_word(proxy->dispatch) ||
        proxy->magic != BVB_COMMAND_BUFFER_PROXY_MAGIC ||
        bvb_handle_expect(proxy->wire_id, BVB_OBJECT_COMMAND_BUFFER) != 0 ||
        bvb_handle_expect(proxy->parent_pool_id,
                          BVB_OBJECT_COMMAND_POOL) != 0 ||
        bvb_handle_expect(proxy->device_id, BVB_OBJECT_DEVICE) != 0) {
        return NULL;
    }
    return proxy;
}

BVB_GLOBAL_EXPORT uint64_t bvb_command_buffer_proxy_id(
    VkCommandBuffer command_buffer) {
    struct bvb_command_buffer_proxy *proxy =
        command_buffer_proxy(command_buffer);
    return proxy == NULL ? 0U : proxy->wire_id;
}

static void store_command_diagnostic_locked(
    struct bvb_command_buffer_proxy *proxy, const char *entry,
    const char *reason, const char *shape, int status) {
    if (proxy == NULL || !bvb_first_rejection_enabled() ||
        proxy->diagnostic_rejection_entry != NULL) return;
    proxy->diagnostic_rejection_entry = entry;
    proxy->diagnostic_rejection_reason = reason;
    proxy->diagnostic_rejection_shape = shape;
    proxy->diagnostic_rejection_status = status;
}

BVB_GLOBAL_EXPORT void bvb_global_diagnostic_poison_command(
    VkCommandBuffer command_buffer, const char *entry, const char *reason,
    const char *shape, int status) {
    if (!bvb_first_rejection_enabled()) return;
    struct bvb_command_buffer_proxy *proxy =
        command_buffer_proxy(command_buffer);
    if (proxy == NULL) return;
    if (command_stream_is_enabled()) {
        if (pthread_mutex_lock(&proxy->stream_mutex) != 0) return;
        store_command_diagnostic_locked(proxy, entry, reason, shape, status);
        proxy->stream_error = true;
        proxy->stream_sealed = false;
        (void)pthread_mutex_unlock(&proxy->stream_mutex);
        return;
    }
    if (pthread_mutex_lock(&bvb_global_client.mutex) != 0) return;
    store_command_diagnostic_locked(proxy, entry, reason, shape, status);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
}

static uint64_t non_dispatchable_wire_id(const void *handle, size_t size) {
    uint64_t wire_id = 0U;
    if (handle != NULL && size <= sizeof(wire_id))
        memcpy(&wire_id, handle, size);
    return wire_id;
}

static VkSurfaceKHR surface_from_wire_id(uint64_t wire_id) {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    _Static_assert(sizeof(surface) <= sizeof(wire_id),
                   "VkSurfaceKHR exceeds bridge handle width");
    memcpy(&surface, &wire_id, sizeof(surface));
    return surface;
}

static struct bvb_surface_proxy *surface_proxy_locked(VkSurfaceKHR surface) {
    const uint64_t wire_id = non_dispatchable_wire_id(&surface, sizeof(surface));
    if (bvb_handle_expect(wire_id, BVB_OBJECT_SURFACE) != 0) {
        return NULL;
    }
    for (struct bvb_surface_proxy *proxy = bvb_global_client.surfaces;
         proxy != NULL; proxy = proxy->next) {
        if (proxy->wire_id == wire_id) return proxy;
    }
    return NULL;
}

static void remove_surface_proxy_locked(struct bvb_surface_proxy *target) {
    struct bvb_surface_proxy **cursor = &bvb_global_client.surfaces;
    while (*cursor != NULL) {
        struct bvb_surface_proxy *proxy = *cursor;
        if (proxy == target) {
            *cursor = proxy->next;
            free(proxy);
            return;
        }
        cursor = &proxy->next;
    }
}

static void remove_surfaces_for_instance_locked(uint64_t parent_id) {
    struct bvb_surface_proxy **cursor = &bvb_global_client.surfaces;
    while (*cursor != NULL) {
        struct bvb_surface_proxy *proxy = *cursor;
        if (proxy->parent_id == parent_id) {
            *cursor = proxy->next;
            free(proxy);
        } else {
            cursor = &proxy->next;
        }
    }
}

static VkSwapchainKHR swapchain_from_wire_id(uint64_t wire_id) {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    _Static_assert(sizeof(swapchain) <= sizeof(wire_id),
                   "VkSwapchainKHR exceeds bridge handle width");
    memcpy(&swapchain, &wire_id, sizeof(swapchain));
    return swapchain;
}

static VkImage image_from_wire_id(uint64_t wire_id) {
    VkImage image = VK_NULL_HANDLE;
    _Static_assert(sizeof(image) <= sizeof(wire_id),
                   "VkImage exceeds bridge handle width");
    memcpy(&image, &wire_id, sizeof(image));
    return image;
}

static struct bvb_swapchain_proxy *swapchain_proxy_locked(
    VkSwapchainKHR swapchain) {
    const uint64_t wire_id = non_dispatchable_wire_id(
        &swapchain, sizeof(swapchain));
    if (bvb_handle_expect(wire_id, BVB_OBJECT_SWAPCHAIN) != 0) return NULL;
    for (struct bvb_swapchain_proxy *proxy = bvb_global_client.swapchains;
         proxy != NULL; proxy = proxy->next) {
        if (proxy->wire_id == wire_id) return proxy;
    }
    return NULL;
}

static void release_swapchain_proxy(struct bvb_swapchain_proxy *proxy) {
    if (proxy == NULL) return;
    if (proxy->control != NULL && proxy->control != MAP_FAILED)
        (void)munmap(proxy->control, BVB_WSI_FRAME_RING_REGION_BYTES);
    if (proxy->control_fd >= 0) (void)close(proxy->control_fd);
    free(proxy);
}

static void remove_swapchain_proxy_locked(
    struct bvb_swapchain_proxy *target) {
    struct bvb_swapchain_proxy **cursor = &bvb_global_client.swapchains;
    while (*cursor != NULL) {
        struct bvb_swapchain_proxy *proxy = *cursor;
        if (proxy == target) {
            *cursor = proxy->next;
            release_swapchain_proxy(proxy);
            return;
        }
        cursor = &proxy->next;
    }
}

static void remove_swapchains_for_device_locked(uint64_t parent_id) {
    if (pthread_rwlock_wrlock(&bvb_object_registry_lock) != 0) return;
    struct bvb_swapchain_proxy **cursor = &bvb_global_client.swapchains;
    while (*cursor != NULL) {
        struct bvb_swapchain_proxy *proxy = *cursor;
        if (proxy->parent_id == parent_id) {
            *cursor = proxy->next;
            release_swapchain_proxy(proxy);
        } else {
            cursor = &proxy->next;
        }
    }
    (void)pthread_rwlock_unlock(&bvb_object_registry_lock);
}

static struct bvb_resource_proxy *resource_proxy_locked(
    uint64_t wire_id, enum bvb_object_type type) {
    if (bvb_handle_expect(wire_id, type) != 0) return NULL;
    for (struct bvb_resource_proxy *proxy = bvb_global_client.resources;
         proxy != NULL; proxy = proxy->next)
        if (proxy->wire_id == wire_id)
            return proxy->type == type ? proxy : NULL;
    return NULL;
}

static bool image_owned_by_device_locked(uint64_t image_id,
                                         uint64_t device_id) {
    struct bvb_resource_proxy *resource =
        resource_proxy_locked(image_id, BVB_OBJECT_IMAGE);
    if (resource != NULL) return resource->parent_id == device_id;
    if (bvb_handle_expect(image_id, BVB_OBJECT_IMAGE) != 0) return false;
    for (struct bvb_swapchain_proxy *swapchain =
             bvb_global_client.swapchains;
         swapchain != NULL; swapchain = swapchain->next) {
        if (swapchain->parent_id != device_id) continue;
        for (uint32_t index = 0U; index < swapchain->image_count; ++index)
            if (swapchain->image_ids[index] == image_id) return true;
    }
    return false;
}

/* The caller owns command_state->stream_mutex on entry. A cache miss releases
 * it before the registry read, then reacquires it and returns with it held.
 * Positive entries remain valid for one recording under Vulkan's
 * object-lifetime/external-sync rules; BeginCommandBuffer clears every entry
 * before a rerecord. A negative return means reacquisition failed. */
static int shared_object_owned_by_device_cached_locked(
    struct bvb_command_buffer_proxy *command_state, uint64_t wire_id,
    enum bvb_object_type type) {
    if (command_state == NULL ||
        (type != BVB_OBJECT_BUFFER && type != BVB_OBJECT_IMAGE &&
         type != BVB_OBJECT_IMAGE_VIEW && type != BVB_OBJECT_PIPELINE &&
         type != BVB_OBJECT_PIPELINE_LAYOUT &&
         type != BVB_OBJECT_DESCRIPTOR_SET) ||
        bvb_handle_expect(wire_id, type) != 0) {
        return 0;
    }
    _Static_assert((BVB_COMMAND_OWNERSHIP_CACHE_CAPACITY &
                    (BVB_COMMAND_OWNERSHIP_CACHE_CAPACITY - 1U)) == 0U,
                   "command ownership cache capacity must be a power of two");
    const uint64_t hash = wire_id ^ (wire_id >> 32U) ^ (uint64_t)type;
    const uint32_t index = (uint32_t)hash &
        (BVB_COMMAND_OWNERSHIP_CACHE_CAPACITY - 1U);
    const struct bvb_command_ownership_cache_entry *cached =
        &command_state->ownership_cache[index];
    if (cached->wire_id == wire_id && cached->type == type) return 1;
    const uint64_t recording_sequence = command_state->stream_sequence;
    (void)pthread_mutex_unlock(&command_state->stream_mutex);
    const int registry_status =
        pthread_rwlock_rdlock(&bvb_object_registry_lock);
    bool owned = false;
    if (registry_status == 0) {
        if (type == BVB_OBJECT_IMAGE) {
            owned = image_owned_by_device_locked(
                wire_id, command_state->device_id);
        } else if (type == BVB_OBJECT_DESCRIPTOR_SET) {
            const struct bvb_resource_proxy *set =
                resource_proxy_locked(wire_id, type);
            const struct bvb_resource_proxy *pool = set == NULL ? NULL :
                resource_proxy_locked(set->parent_id,
                                      BVB_OBJECT_DESCRIPTOR_POOL);
            owned = pool != NULL &&
                pool->parent_id == command_state->device_id;
        } else {
            const struct bvb_resource_proxy *proxy =
                resource_proxy_locked(wire_id, type);
            owned = proxy != NULL &&
                proxy->parent_id == command_state->device_id;
        }
        (void)pthread_rwlock_unlock(&bvb_object_registry_lock);
    }
    if (pthread_mutex_lock(&command_state->stream_mutex) != 0)
        return -EDEADLK;
    if (registry_status != 0) return 0;
    ++command_state->ownership_registry_reads;
    if (command_state->stream_sequence != recording_sequence ||
        !command_state->stream_recording) {
        return 0;
    }
    if (owned) {
        command_state->ownership_cache[index] =
            (struct bvb_command_ownership_cache_entry){
                .wire_id = wire_id,
                .type = type,
            };
    }
    return owned ? 1 : 0;
}

BVB_GLOBAL_EXPORT uint64_t bvb_buffer_proxy_id(VkBuffer buffer) {
    const uint64_t wire_id = non_dispatchable_wire_id(&buffer, sizeof(buffer));
    uint64_t result = 0U;
    if (pthread_mutex_lock(&bvb_global_client.mutex) == 0) {
        result = resource_proxy_locked(wire_id, BVB_OBJECT_BUFFER) == NULL
                     ? 0U : wire_id;
        (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    }
    return result;
}

BVB_GLOBAL_EXPORT uint64_t bvb_image_proxy_id(VkImage image) {
    const uint64_t wire_id = non_dispatchable_wire_id(&image, sizeof(image));
    uint64_t result = 0U;
    if (pthread_mutex_lock(&bvb_global_client.mutex) == 0) {
        result = resource_proxy_locked(wire_id, BVB_OBJECT_IMAGE) == NULL
                     ? 0U : wire_id;
        (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    }
    return result;
}

BVB_GLOBAL_EXPORT uint64_t bvb_command_buffer_ownership_registry_reads(
    VkCommandBuffer command_buffer) {
    struct bvb_command_buffer_proxy *proxy =
        command_buffer_proxy(command_buffer);
    if (proxy == NULL || pthread_mutex_lock(&proxy->stream_mutex) != 0)
        return 0U;
    const uint64_t reads = proxy->ownership_registry_reads;
    (void)pthread_mutex_unlock(&proxy->stream_mutex);
    return reads;
}

BVB_GLOBAL_EXPORT uint64_t bvb_image_view_proxy_id(VkImageView image_view) {
    const uint64_t wire_id = non_dispatchable_wire_id(
        &image_view, sizeof(image_view));
    uint64_t result = 0U;
    if (pthread_mutex_lock(&bvb_global_client.mutex) == 0) {
        result = resource_proxy_locked(wire_id, BVB_OBJECT_IMAGE_VIEW) == NULL
                     ? 0U : wire_id;
        (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    }
    return result;
}

BVB_GLOBAL_EXPORT uint64_t bvb_descriptor_set_proxy_id(
    VkDescriptorSet descriptor_set) {
    const uint64_t wire_id = non_dispatchable_wire_id(
        &descriptor_set, sizeof(descriptor_set));
    uint64_t result = 0U;
    if (pthread_mutex_lock(&bvb_global_client.mutex) == 0) {
        result = resource_proxy_locked(wire_id, BVB_OBJECT_DESCRIPTOR_SET) ==
                NULL ? 0U : wire_id;
        (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    }
    return result;
}

BVB_GLOBAL_EXPORT uint64_t bvb_memory_proxy_id(VkDeviceMemory memory) {
    const uint64_t wire_id = non_dispatchable_wire_id(&memory, sizeof(memory));
    uint64_t result = 0U;
    if (pthread_mutex_lock(&bvb_global_client.mutex) == 0) {
        result = resource_proxy_locked(wire_id, BVB_OBJECT_DEVICE_MEMORY) == NULL
                     ? 0U : wire_id;
        (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    }
    return result;
}

BVB_GLOBAL_EXPORT uint64_t bvb_fence_proxy_id(VkFence fence) {
    const uint64_t wire_id = non_dispatchable_wire_id(&fence, sizeof(fence));
    uint64_t result = 0U;
    if (pthread_mutex_lock(&bvb_global_client.mutex) == 0) {
        result = resource_proxy_locked(wire_id, BVB_OBJECT_FENCE) == NULL
                     ? 0U : wire_id;
        (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    }
    return result;
}

BVB_GLOBAL_EXPORT uint64_t bvb_global_dispatch_exchange_count(void) {
    uint64_t count = UINT64_MAX;
    if (pthread_mutex_lock(&bvb_global_client.mutex) == 0) {
        count = bvb_global_client.exchange_count;
        (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    }
    return count;
}

BVB_GLOBAL_EXPORT uint64_t bvb_global_dispatch_descriptor_ring_call_count(
    void) {
    uint64_t count = UINT64_MAX;
    if (pthread_mutex_lock(&bvb_global_client.mutex) == 0) {
        count = bvb_global_client.descriptor_ring_call_count;
        (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    }
    return count;
}

BVB_GLOBAL_EXPORT uint64_t bvb_global_dispatch_descriptor_lease_hit_count(
    void) {
    uint64_t count = UINT64_MAX;
    if (pthread_mutex_lock(&bvb_global_client.mutex) == 0) {
        count = bvb_global_client.descriptor_lease_hit_count;
        (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    }
    return count;
}

BVB_GLOBAL_EXPORT int bvb_memory_proxy_is_mapped(VkDeviceMemory memory) {
    const uint64_t wire_id = non_dispatchable_wire_id(&memory, sizeof(memory));
    int mapped = -1;
    if (pthread_mutex_lock(&bvb_global_client.mutex) == 0) {
        const struct bvb_resource_proxy *state =
            resource_proxy_locked(wire_id, BVB_OBJECT_DEVICE_MEMORY);
        mapped = state != NULL && state->mapped_bytes != NULL ? 1 : 0;
        (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    }
    return mapped;
}

BVB_GLOBAL_EXPORT int bvb_global_dispatch_connection_is_open(void) {
    int connected = -1;
    if (pthread_mutex_lock(&bvb_global_client.mutex) == 0) {
        connected = bvb_global_client.socket_fd >= 0 ? 1 : 0;
        (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    }
    return connected;
}

BVB_GLOBAL_EXPORT uint16_t bvb_global_dispatch_last_opcode(void) {
    uint16_t opcode = 0U;
    if (pthread_mutex_lock(&bvb_global_client.mutex) == 0) {
        opcode = bvb_global_client.last_opcode;
        (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    }
    return opcode;
}

static struct bvb_queue_proxy *queue_proxy_locked(
    uint64_t wire_id, uint64_t parent_id) {
    for (struct bvb_queue_proxy *proxy = bvb_global_client.queues;
         proxy != NULL; proxy = proxy->next) {
        if (proxy->wire_id == wire_id) {
            return proxy->parent_id == parent_id ? proxy : NULL;
        }
    }
    struct bvb_queue_proxy *proxy = calloc(1, sizeof(*proxy));
    if (proxy == NULL) {
        return NULL;
    }
    proxy->dispatch = initial_dispatch_word();
    proxy->magic = BVB_QUEUE_PROXY_MAGIC;
    proxy->wire_id = wire_id;
    proxy->parent_id = parent_id;
    proxy->next = bvb_global_client.queues;
    bvb_global_client.queues = proxy;
    return proxy;
}

static void remove_queue_proxies_locked(uint64_t parent_id) {
    struct bvb_queue_proxy **cursor = &bvb_global_client.queues;
    while (*cursor != NULL) {
        struct bvb_queue_proxy *proxy = *cursor;
        if (proxy->parent_id == parent_id) {
            *cursor = proxy->next;
            proxy->magic = 0U;
            free(proxy);
        } else {
            cursor = &proxy->next;
        }
    }
}

static int lease_command_stream_slot(
    struct bvb_command_buffer_proxy *proxy) {
    if (proxy == NULL || !command_stream_is_enabled() ||
        pthread_mutex_lock(&bvb_global_client.command_stream_slots_mutex) !=
            0) {
        return -EINVAL;
    }
    int result = -ENOSPC;
    for (uint32_t slot = 0U; slot < BVB_COMMAND_STREAM_SLOT_COUNT; ++slot) {
        const uint32_t word = slot / 64U;
        const uint64_t bit = UINT64_C(1) << (slot % 64U);
        if ((bvb_global_client.command_stream_slots[word] & bit) == 0U) {
            if (bvb_global_client.next_command_stream_sequence == 0U) {
                result = -EOVERFLOW;
                break;
            }
            bvb_global_client.command_stream_slots[word] |= bit;
            proxy->stream_slot = slot;
            proxy->stream_sequence =
                bvb_global_client.next_command_stream_sequence++;
            result = 0;
            break;
        }
    }
    (void)pthread_mutex_unlock(
        &bvb_global_client.command_stream_slots_mutex);
    return result;
}

/* The caller owns proxy->stream_mutex. */
static void release_command_stream_slot(
    struct bvb_command_buffer_proxy *proxy) {
    if (proxy == NULL || proxy->stream_slot >= BVB_COMMAND_STREAM_SLOT_COUNT) {
        return;
    }
    if (pthread_mutex_lock(&bvb_global_client.command_stream_slots_mutex) !=
        0) {
        return;
    }
    const uint32_t word = proxy->stream_slot / 64U;
    bvb_global_client.command_stream_slots[word] &=
        ~(UINT64_C(1) << (proxy->stream_slot % 64U));
    proxy->stream_slot = UINT32_MAX;
    (void)pthread_mutex_unlock(
        &bvb_global_client.command_stream_slots_mutex);
}

/* The caller owns proxy->stream_mutex. */
static void reset_command_stream_state(
    struct bvb_command_buffer_proxy *proxy) {
    if (proxy == NULL) return;
    release_command_stream_slot(proxy);
    memset(&proxy->stream_builder, 0, sizeof(proxy->stream_builder));
    memset(proxy->ownership_cache, 0, sizeof(proxy->ownership_cache));
    proxy->ownership_registry_reads = 0U;
    proxy->stream_sequence = 0U;
    proxy->stream_length = 0U;
    proxy->stream_recording = false;
    proxy->stream_sealed = false;
    proxy->stream_uploaded = false;
    proxy->stream_error = false;
    proxy->diagnostic_rejection_entry = NULL;
    proxy->diagnostic_rejection_reason = NULL;
    proxy->diagnostic_rejection_shape = NULL;
    proxy->diagnostic_rejection_status = 0;
}

static void reset_command_streams_for_pool_locked(uint64_t parent_pool_id) {
    for (struct bvb_command_buffer_proxy *proxy =
             bvb_global_client.command_buffers;
         proxy != NULL; proxy = proxy->next) {
        if (proxy->parent_pool_id == parent_pool_id) {
            if (pthread_mutex_lock(&proxy->stream_mutex) == 0) {
                reset_command_stream_state(proxy);
                (void)pthread_mutex_unlock(&proxy->stream_mutex);
            }
        }
    }
}

static void remove_command_buffer_proxy_locked(
    struct bvb_command_buffer_proxy *target) {
    struct bvb_command_buffer_proxy **cursor =
        &bvb_global_client.command_buffers;
    while (*cursor != NULL) {
        struct bvb_command_buffer_proxy *proxy = *cursor;
        if (proxy == target) {
            *cursor = proxy->next;
            if (pthread_mutex_lock(&proxy->stream_mutex) == 0) {
                release_command_stream_slot(proxy);
                proxy->magic = 0U;
                (void)pthread_mutex_unlock(&proxy->stream_mutex);
            }
            (void)pthread_mutex_destroy(&proxy->stream_mutex);
            free(proxy);
            return;
        }
        cursor = &proxy->next;
    }
}

static void remove_command_buffers_for_pool_locked(uint64_t parent_pool_id) {
    struct bvb_command_buffer_proxy **cursor =
        &bvb_global_client.command_buffers;
    while (*cursor != NULL) {
        struct bvb_command_buffer_proxy *proxy = *cursor;
        if (proxy->parent_pool_id == parent_pool_id) {
            *cursor = proxy->next;
            if (pthread_mutex_lock(&proxy->stream_mutex) == 0) {
                release_command_stream_slot(proxy);
                proxy->magic = 0U;
                (void)pthread_mutex_unlock(&proxy->stream_mutex);
            }
            (void)pthread_mutex_destroy(&proxy->stream_mutex);
            free(proxy);
        } else {
            cursor = &proxy->next;
        }
    }
}

static void remove_command_pool_proxy_locked(
    struct bvb_command_pool_proxy *target) {
    struct bvb_command_pool_proxy **cursor =
        &bvb_global_client.command_pools;
    while (*cursor != NULL) {
        struct bvb_command_pool_proxy *proxy = *cursor;
        if (proxy == target) {
            *cursor = proxy->next;
            remove_command_buffers_for_pool_locked(proxy->wire_id);
            free(proxy);
            return;
        }
        cursor = &proxy->next;
    }
}

static void remove_command_pools_for_device_locked(uint64_t parent_id) {
    struct bvb_command_pool_proxy **cursor =
        &bvb_global_client.command_pools;
    while (*cursor != NULL) {
        struct bvb_command_pool_proxy *proxy = *cursor;
        if (proxy->parent_id == parent_id) {
            *cursor = proxy->next;
            remove_command_buffers_for_pool_locked(proxy->wire_id);
            free(proxy);
        } else {
            cursor = &proxy->next;
        }
    }
}

static void release_mapped_shadow_locked(struct bvb_resource_proxy *proxy) {
    if (proxy == NULL || proxy->mapped_bytes == NULL) return;
    (void)munmap(proxy->mapped_bytes, (size_t)proxy->mapped_size);
    proxy->mapped_bytes = NULL;
    proxy->mapped_offset = 0U;
    proxy->mapped_size = 0U;
    proxy->mapped_generation = 0U;
    proxy->mapped_shared = false;
    proxy->mapped_direct = false;
}

static void release_resource_metadata(struct bvb_resource_proxy *proxy) {
    if (proxy == NULL) return;
    free(proxy->descriptor_template);
    proxy->descriptor_template = NULL;
}

static void remove_resource_proxy_locked(struct bvb_resource_proxy *target) {
    struct bvb_resource_proxy **cursor = &bvb_global_client.resources;
    while (*cursor != NULL) {
        struct bvb_resource_proxy *proxy = *cursor;
        if (proxy == target) {
            *cursor = proxy->next;
            release_mapped_shadow_locked(proxy);
            release_resource_metadata(proxy);
            free(proxy);
            return;
        }
        cursor = &proxy->next;
    }
}

static void remove_descriptor_sets_for_pool_locked(uint64_t pool_id) {
    struct bvb_resource_proxy **cursor = &bvb_global_client.resources;
    while (*cursor != NULL) {
        struct bvb_resource_proxy *proxy = *cursor;
        if (proxy->type == BVB_OBJECT_DESCRIPTOR_SET &&
            proxy->parent_id == pool_id) {
            *cursor = proxy->next;
            release_resource_metadata(proxy);
            free(proxy);
        } else {
            cursor = &proxy->next;
        }
    }
}

static void remove_resources_for_device_locked(uint64_t parent_id) {
    if (pthread_rwlock_wrlock(&bvb_object_registry_lock) != 0) return;
    for (struct bvb_resource_proxy *proxy = bvb_global_client.resources;
         proxy != NULL; proxy = proxy->next) {
        if (proxy->type == BVB_OBJECT_DESCRIPTOR_POOL &&
            proxy->parent_id == parent_id) {
            remove_descriptor_sets_for_pool_locked(proxy->wire_id);
        }
    }
    struct bvb_resource_proxy **cursor = &bvb_global_client.resources;
    while (*cursor != NULL) {
        struct bvb_resource_proxy *proxy = *cursor;
        if (proxy->parent_id == parent_id) {
            *cursor = proxy->next;
            release_mapped_shadow_locked(proxy);
            release_resource_metadata(proxy);
            free(proxy);
        } else {
            cursor = &proxy->next;
        }
    }
    (void)pthread_rwlock_unlock(&bvb_object_registry_lock);
}

static void remove_device_proxy_locked(struct bvb_device_proxy *target) {
    struct bvb_device_proxy **cursor = &bvb_global_client.devices;
    while (*cursor != NULL) {
        struct bvb_device_proxy *proxy = *cursor;
        if (proxy == target) {
            *cursor = proxy->next;
            remove_command_pools_for_device_locked(proxy->wire_id);
            remove_resources_for_device_locked(proxy->wire_id);
            remove_queue_proxies_locked(proxy->wire_id);
            proxy->magic = 0U;
            free(proxy);
            return;
        }
        cursor = &proxy->next;
    }
}

static void remove_device_proxies_for_instance_locked(uint64_t instance_id) {
    struct bvb_device_proxy **cursor = &bvb_global_client.devices;
    while (*cursor != NULL) {
        struct bvb_device_proxy *proxy = *cursor;
        if (proxy->instance_id == instance_id) {
            *cursor = proxy->next;
            remove_command_pools_for_device_locked(proxy->wire_id);
            remove_resources_for_device_locked(proxy->wire_id);
            remove_queue_proxies_locked(proxy->wire_id);
            proxy->magic = 0U;
            free(proxy);
        } else {
            cursor = &proxy->next;
        }
    }
}

static void remove_physical_proxies_locked(uint64_t parent_id) {
    remove_device_proxies_for_instance_locked(parent_id);
    struct bvb_physical_device_proxy **cursor =
        &bvb_global_client.physical_devices;
    while (*cursor != NULL) {
        struct bvb_physical_device_proxy *proxy = *cursor;
        if (proxy->parent_id == parent_id) {
            *cursor = proxy->next;
            proxy->magic = 0U;
            free(proxy->device_extensions);
            free(proxy);
        } else {
            cursor = &proxy->next;
        }
    }
}

static VkResult VKAPI_CALL bvb_bridge_vkEnumerateInstanceVersion(
    uint32_t *api_version) {
    if (api_version == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    struct bvb_vulkan_global_info info;
    int lock_result = pthread_mutex_lock(&bvb_global_client.mutex);
    if (lock_result != 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    int result = global_info_locked(&info);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result != 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *api_version = info.loader_api_version;
    return VK_SUCCESS;
}

static bool is_virtual_wsi_extension(const char *name) {
    return strcmp(name, "VK_KHR_surface") == 0 ||
           strcmp(name, "VK_KHR_xlib_surface") == 0 ||
           strcmp(name, "VK_KHR_xcb_surface") == 0 ||
           strcmp(name, "VK_KHR_wayland_surface") == 0;
}

static VkResult VKAPI_CALL bvb_bridge_vkEnumerateInstanceExtensionProperties(
    const char *layer_name, uint32_t *property_count,
    VkExtensionProperties *properties) {
    static const VkExtensionProperties virtual_wsi_extensions[] = {
        {{"VK_KHR_surface"}, 25U},
        {{"VK_KHR_xlib_surface"}, 6U},
        {{"VK_KHR_xcb_surface"}, 6U},
        {{"VK_KHR_wayland_surface"}, 6U},
    };
    if (property_count == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (layer_name != NULL) {
        return VK_ERROR_LAYER_NOT_PRESENT;
    }
    int lock_result = pthread_mutex_lock(&bvb_global_client.mutex);
    if (lock_result != 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    int result = connect_locked();
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_INSTANCE_EXTENSIONS,
        .request_id = next_request_id_locked(),
    };
    struct bvb_protocol_packet response = {0};
    if (result == 0) {
        result = exchange_locked(&request, &response);
    }
    if (result == 0 && response.header.status != 0) {
        result = response.header.status;
    }
    struct bvb_vulkan_extension_page page = {0};
    if (result == 0) {
        result = bvb_vulkan_decode_extension_page(
            response.payload, response.header.payload_length, &page);
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result != 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (page.vulkan_result != VK_SUCCESS) {
        return (VkResult)page.vulkan_result;
    }
    if (page.first != 0U || page.count != page.total_count) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const uint32_t virtual_count =
        (uint32_t)(sizeof(virtual_wsi_extensions) /
                   sizeof(virtual_wsi_extensions[0]));
    const uint32_t total_count = page.total_count + virtual_count;
    if (getenv("BVB_ICD_DIAGNOSTICS") != NULL) {
        fprintf(stderr, "BVB_ICD_WSI_READY advertised=%u\n", virtual_count);
    }
    const uint32_t capacity = properties == NULL ? 0U : *property_count;
    if (properties == NULL) {
        *property_count = total_count;
        return VK_SUCCESS;
    }
    uint32_t written = capacity < page.count ? capacity : page.count;
    if (written > 0U) {
        memcpy(properties, page.properties,
               written * sizeof(*properties));
    }
    for (uint32_t index = 0U;
         index < virtual_count && written < capacity; ++index) {
        properties[written++] = virtual_wsi_extensions[index];
    }
    *property_count = written;
    return capacity < total_count ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult VKAPI_CALL bvb_bridge_vkEnumerateInstanceLayerProperties(
    uint32_t *property_count, VkLayerProperties *properties) {
    if (property_count == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    struct bvb_vulkan_global_info info;
    int lock_result = pthread_mutex_lock(&bvb_global_client.mutex);
    if (lock_result != 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    int result = global_info_locked(&info);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result != 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    uint32_t capacity = properties == NULL ? 0U : *property_count;
    *property_count = 0U;
    return capacity < info.exposed_layer_count ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult VKAPI_CALL bvb_bridge_vkCreateInstance(
    const VkInstanceCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkInstance *instance) {
    if (getenv("BVB_ICD_DIAGNOSTICS") != NULL) {
        fprintf(stderr,
                "BVB_ICD_CREATE_INSTANCE info=%p stype=%u pnext=%p "
                "flags=%u allocator=%p app=%p app_stype=%u app_pnext=%p "
                "layers=%u extensions=%u\n",
                (const void *)create_info,
                create_info == NULL ? 0U : (unsigned int)create_info->sType,
                create_info == NULL ? NULL : create_info->pNext,
                create_info == NULL ? 0U : (unsigned int)create_info->flags,
                (const void *)allocator,
                create_info == NULL
                    ? NULL : (const void *)create_info->pApplicationInfo,
                create_info == NULL || create_info->pApplicationInfo == NULL
                    ? 0U
                    : (unsigned int)create_info->pApplicationInfo->sType,
                create_info == NULL || create_info->pApplicationInfo == NULL
                    ? NULL : create_info->pApplicationInfo->pNext,
                create_info == NULL ? 0U : create_info->enabledLayerCount,
                create_info == NULL ? 0U : create_info->enabledExtensionCount);
    }
    (void)allocator;
    if (instance != NULL) {
        *instance = VK_NULL_HANDLE;
    }
    if (create_info == NULL || instance == NULL ||
        create_info->sType != VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO ||
        create_info->flags != 0U ||
        (create_info->pApplicationInfo != NULL &&
         (create_info->pApplicationInfo->sType !=
              VK_STRUCTURE_TYPE_APPLICATION_INFO ||
          create_info->pApplicationInfo->pNext != NULL))) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (create_info->enabledLayerCount != 0U) {
        return VK_ERROR_LAYER_NOT_PRESENT;
    }
    if (create_info->enabledExtensionCount >
            BVB_VULKAN_MAX_ENABLED_EXTENSIONS ||
        (create_info->enabledExtensionCount != 0U &&
         create_info->ppEnabledExtensionNames == NULL)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    for (uint32_t index = 0U;
         index < create_info->enabledExtensionCount; ++index) {
        const char *name = create_info->ppEnabledExtensionNames[index];
        if (name == NULL || name[0] == '\0' ||
            memchr(name, '\0', BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE) ==
                NULL) {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
    }
    const char *unique_extensions[BVB_VULKAN_MAX_ENABLED_EXTENSIONS] = {0};
    uint32_t unique_extension_count = 0U;
    for (uint32_t index = 0U;
         index < create_info->enabledExtensionCount; ++index) {
        const char *name = create_info->ppEnabledExtensionNames[index];
        if (is_virtual_wsi_extension(name)) {
            continue;
        }
        bool duplicate = false;
        for (uint32_t prior = 0U; prior < unique_extension_count; ++prior) {
            if (strcmp(name, unique_extensions[prior]) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            unique_extensions[unique_extension_count++] = name;
        }
    }
    if (getenv("BVB_ICD_DIAGNOSTICS") != NULL) {
        for (uint32_t index = 0U;
             index < create_info->enabledExtensionCount; ++index) {
            fprintf(stderr,
                    "BVB_ICD_CREATE_INSTANCE_EXTENSION index=%u name=%s\n",
                    index, create_info->ppEnabledExtensionNames[index]);
        }
        fprintf(stderr,
                "BVB_ICD_CREATE_INSTANCE_NORMALIZED original=%u unique=%u\n",
                create_info->enabledExtensionCount, unique_extension_count);
    }
    struct bvb_instance_proxy *proxy = calloc(1, sizeof(*proxy));
    if (proxy == NULL) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    const struct bvb_vulkan_instance_create_request create_request = {
        .api_version = create_info->pApplicationInfo == NULL
                           ? 0U
                           : create_info->pApplicationInfo->apiVersion,
        .flags = create_info->flags,
        .enabled_layer_count = create_info->enabledLayerCount,
        .enabled_extension_count = unique_extension_count,
    };
    int lock_result = pthread_mutex_lock(&bvb_global_client.mutex);
    if (lock_result != 0) {
        free(proxy);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    int result = connect_locked();
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = unique_extension_count == 0U
                      ? BVB_OPCODE_VULKAN_INSTANCE_CREATE
                      : BVB_OPCODE_VULKAN_INSTANCE_CREATE_EXTENDED,
        .request_id = next_request_id_locked(),
    };
    if (result == 0) {
        if (unique_extension_count == 0U) {
            request.header.payload_length =
                BVB_VULKAN_INSTANCE_CREATE_REQUEST_SIZE;
            result = bvb_protocol_encode_vulkan_instance_create_request(
                request.payload, &create_request);
        } else {
            struct bvb_vulkan_instance_create_extended_request extended = {
                .base = create_request,
            };
            for (uint32_t index = 0U; index < unique_extension_count;
                 ++index) {
                const char *name = unique_extensions[index];
                const char *terminator = memchr(
                    name, '\0', BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE);
                const size_t length = (size_t)(terminator - name) + 1U;
                memcpy(extended.enabled_extensions[index], name, length);
            }
            result =
                bvb_protocol_encode_vulkan_instance_create_extended_request(
                    request.payload, &extended,
                    &request.header.payload_length);
        }
    }
    struct bvb_protocol_packet response = {0};
    if (result == 0) {
        result = exchange_locked(&request, &response);
    }
    if (result == 0 &&
        (response.header.status != 0 ||
         response.header.payload_length !=
             BVB_VULKAN_INSTANCE_CREATE_RESPONSE_SIZE)) {
        result = -EPROTO;
    }
    struct bvb_vulkan_instance_create_response create_response = {0};
    if (result == 0) {
        result = bvb_protocol_decode_vulkan_instance_create_response(
            response.payload, &create_response);
    }
    if (result == 0 && create_response.vulkan_result == VK_SUCCESS &&
        bvb_handle_expect(create_response.instance_id,
                          BVB_OBJECT_INSTANCE) != 0) {
        result = -EPROTO;
    }
    if (getenv("BVB_ICD_DIAGNOSTICS") != NULL) {
        fprintf(stderr,
                "BVB_ICD_CREATE_RESPONSE status=%d wire_status=%d "
                "payload=%u vulkan=%d instance=%llu\n",
                result, response.header.status,
                response.header.payload_length,
                create_response.vulkan_result,
                (unsigned long long)create_response.instance_id);
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result != 0) {
        free(proxy);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (create_response.vulkan_result != VK_SUCCESS) {
        free(proxy);
        return (VkResult)create_response.vulkan_result;
    }
    proxy->dispatch = initial_dispatch_word();
    proxy->magic = BVB_INSTANCE_PROXY_MAGIC;
    proxy->wire_id = create_response.instance_id;
    *instance = (VkInstance)proxy;
    return VK_SUCCESS;
}

static void VKAPI_CALL bvb_bridge_vkDestroyInstance(
    VkInstance instance, const VkAllocationCallbacks *allocator) {
    (void)allocator;
    struct bvb_instance_proxy *proxy = instance_proxy(instance);
    if (proxy == NULL) {
        return;
    }
    int lock_result = pthread_mutex_lock(&bvb_global_client.mutex);
    if (lock_result != 0) {
        return;
    }
    int result = connect_locked();
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_INSTANCE_DESTROY,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_INSTANCE_ID_SIZE,
    };
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_instance_id(
            request.payload, proxy->wire_id);
    }
    struct bvb_protocol_packet response = {0};
    if (result == 0) {
        result = exchange_locked(&request, &response);
    }
    if (result == 0 &&
        (response.header.status != 0 || response.header.payload_length != 0U)) {
        result = -EPROTO;
    }
    if (result == 0) {
        remove_surfaces_for_instance_locked(proxy->wire_id);
        remove_physical_proxies_locked(proxy->wire_id);
        proxy->magic = 0U;
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result == 0) {
        free(proxy);
    }
}

static int activity_status_locked(struct bvb_activity_status *status) {
    if (status == NULL) return -EINVAL;
    int result = connect_locked();
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_ACTIVITY_STATUS,
        .request_id = next_request_id_locked(),
    };
    struct bvb_protocol_packet response = {0};
    if (result == 0) result = exchange_locked(&request, &response);
    if (result == 0 &&
        (response.header.status != 0 ||
         response.header.payload_length != BVB_ACTIVITY_STATUS_SIZE)) {
        result = response.header.status != 0 ? response.header.status : -EPROTO;
    }
    if (result == 0) {
        result = bvb_protocol_decode_activity_status(response.payload, status);
    }
    return result;
}

static int ready_activity_status_locked(struct bvb_activity_status *status) {
    int result = activity_status_locked(status);
    const uint32_t required = BVB_ACTIVITY_WINDOW_PRESENT |
                              BVB_ACTIVITY_RENDERER_READY;
    if (result == 0 &&
        (status->ingress_configured == 0U || status->width == 0U ||
         status->height == 0U ||
         (status->state_flags & required) != required)) {
        result = -ENODEV;
    }
    return result;
}

static VkResult create_virtual_surface(
    VkInstance instance, const void *create_info,
    const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface) {
    (void)allocator;
    if (surface != NULL) *surface = VK_NULL_HANDLE;
    struct bvb_instance_proxy *instance_state = instance_proxy(instance);
    if (instance_state == NULL || create_info == NULL || surface == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    struct bvb_surface_proxy *proxy = calloc(1, sizeof(*proxy));
    if (proxy == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        free(proxy);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkResult result = VK_SUCCESS;
    if (bvb_global_client.next_surface_serial == 0U ||
        bvb_global_client.next_surface_serial > BVB_HANDLE_SERIAL_MASK) {
        result = VK_ERROR_TOO_MANY_OBJECTS;
    } else {
        proxy->wire_id = bvb_handle_id(
            BVB_OBJECT_SURFACE, bvb_global_client.next_surface_serial++);
        proxy->parent_id = instance_state->wire_id;
        proxy->next = bvb_global_client.surfaces;
        bvb_global_client.surfaces = proxy;
        *surface = surface_from_wire_id(proxy->wire_id);
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result != VK_SUCCESS) free(proxy);
    return result;
}

static VkResult create_virtual_surface_diagnostic(
    const char *entry, const char *shape, VkInstance instance,
    const void *create_info, const VkAllocationCallbacks *allocator,
    VkSurfaceKHR *surface) {
    const VkResult result =
        create_virtual_surface(instance, create_info, allocator, surface);
    if (result < 0) {
        const uint64_t pointer_mask =
            (create_info != NULL ? UINT64_C(1) << 1 : 0U) |
            (allocator != NULL ? UINT64_C(1) << 2 : 0U) |
            (surface != NULL ? UINT64_C(1) << 3 : 0U);
        bvb_first_rejection_record(
            "implemented_rejection", entry, entry, "instance",
            "negative_vkresult", result, 4U, pointer_mask, shape,
            0U, 0U, false);
    }
    return result;
}

static VkResult VKAPI_CALL bvb_bridge_vkCreateXlibSurfaceKHR(
    VkInstance instance, const void *create_info,
    const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface) {
    return create_virtual_surface_diagnostic(
        "vkCreateXlibSurfaceKHR",
        "VkInstance_value,VkXlibSurfaceCreateInfoKHR_ptr,VkAllocationCallbacks_ptr,VkSurfaceKHR_ptr",
        instance, create_info, allocator, surface);
}

static VkResult VKAPI_CALL bvb_bridge_vkCreateXcbSurfaceKHR(
    VkInstance instance, const void *create_info,
    const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface) {
    return create_virtual_surface_diagnostic(
        "vkCreateXcbSurfaceKHR",
        "VkInstance_value,VkXcbSurfaceCreateInfoKHR_ptr,VkAllocationCallbacks_ptr,VkSurfaceKHR_ptr",
        instance, create_info, allocator, surface);
}

static VkResult VKAPI_CALL bvb_bridge_vkCreateWaylandSurfaceKHR(
    VkInstance instance, const void *create_info,
    const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface) {
    return create_virtual_surface_diagnostic(
        "vkCreateWaylandSurfaceKHR",
        "VkInstance_value,VkWaylandSurfaceCreateInfoKHR_ptr,VkAllocationCallbacks_ptr,VkSurfaceKHR_ptr",
        instance, create_info, allocator, surface);
}

static void VKAPI_CALL bvb_bridge_vkDestroySurfaceKHR(
    VkInstance instance, VkSurfaceKHR surface,
    const VkAllocationCallbacks *allocator) {
    (void)allocator;
    struct bvb_instance_proxy *instance_state = instance_proxy(instance);
    if (instance_state == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return;
    }
    struct bvb_surface_proxy *proxy = surface_proxy_locked(surface);
    if (proxy != NULL && proxy->parent_id == instance_state->wire_id) {
        remove_surface_proxy_locked(proxy);
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
}

static VkResult VKAPI_CALL bvb_bridge_vkGetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice physical_device, uint32_t queue_family_index,
    VkSurfaceKHR surface, VkBool32 *supported) {
    if (supported == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    *supported = VK_FALSE;
    struct bvb_physical_device_proxy *physical =
        physical_device_proxy(physical_device);
    if (physical == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    struct bvb_surface_proxy *surface_state = surface_proxy_locked(surface);
    struct bvb_activity_status activity = {0};
    int result = surface_state == NULL ||
                         surface_state->parent_id != physical->parent_id
                     ? -EINVAL
                     : ready_activity_status_locked(&activity);
    if (result == 0 && queue_family_index == 0U) *supported = VK_TRUE;
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return result == 0 ? VK_SUCCESS : VK_ERROR_SURFACE_LOST_KHR;
}

static VkBool32 virtual_presentation_support(
    VkPhysicalDevice physical_device, uint32_t queue_family_index) {
    struct bvb_physical_device_proxy *physical =
        physical_device_proxy(physical_device);
    if (physical == NULL || queue_family_index != 0U ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return VK_FALSE;
    }
    struct bvb_activity_status activity = {0};
    const int result = ready_activity_status_locked(&activity);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return result == 0 ? VK_TRUE : VK_FALSE;
}

static VkBool32 VKAPI_CALL
bvb_bridge_vkGetPhysicalDeviceXlibPresentationSupportKHR(
    VkPhysicalDevice physical_device, uint32_t queue_family_index,
    void *display, unsigned long visual_id) {
    (void)display;
    (void)visual_id;
    return virtual_presentation_support(physical_device, queue_family_index);
}

static VkBool32 VKAPI_CALL
bvb_bridge_vkGetPhysicalDeviceXcbPresentationSupportKHR(
    VkPhysicalDevice physical_device, uint32_t queue_family_index,
    void *connection, uint32_t visual_id) {
    (void)connection;
    (void)visual_id;
    return virtual_presentation_support(physical_device, queue_family_index);
}

static VkBool32 VKAPI_CALL
bvb_bridge_vkGetPhysicalDeviceWaylandPresentationSupportKHR(
    VkPhysicalDevice physical_device, uint32_t queue_family_index,
    void *display) {
    (void)display;
    return virtual_presentation_support(physical_device, queue_family_index);
}

static VkResult VKAPI_CALL
bvb_bridge_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physical_device, VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR *capabilities) {
    if (capabilities == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    memset(capabilities, 0, sizeof(*capabilities));
    struct bvb_physical_device_proxy *physical =
        physical_device_proxy(physical_device);
    if (physical == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    struct bvb_surface_proxy *surface_state = surface_proxy_locked(surface);
    struct bvb_activity_status activity = {0};
    int result = surface_state == NULL ||
                         surface_state->parent_id != physical->parent_id
                     ? -EINVAL
                     : ready_activity_status_locked(&activity);
    if (result == 0) {
        *capabilities = (VkSurfaceCapabilitiesKHR){
            .minImageCount = 2U,
            .maxImageCount = 3U,
            .currentExtent = {activity.width, activity.height},
            .minImageExtent = {activity.width, activity.height},
            .maxImageExtent = {activity.width, activity.height},
            .maxImageArrayLayers = 1U,
            .supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
            .currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
            .supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT,
        };
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return result == 0 ? VK_SUCCESS : VK_ERROR_SURFACE_LOST_KHR;
}

static VkResult VKAPI_CALL bvb_bridge_vkGetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice physical_device, VkSurfaceKHR surface,
    uint32_t *format_count, VkSurfaceFormatKHR *formats) {
    static const VkSurfaceFormatKHR virtual_formats[] = {
        {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    };
    if (format_count == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    struct bvb_physical_device_proxy *physical =
        physical_device_proxy(physical_device);
    if (physical == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    struct bvb_surface_proxy *surface_state = surface_proxy_locked(surface);
    struct bvb_activity_status activity = {0};
    int result = surface_state == NULL ||
                         surface_state->parent_id != physical->parent_id
                     ? -EINVAL
                     : ready_activity_status_locked(&activity);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result != 0) return VK_ERROR_SURFACE_LOST_KHR;
    const uint32_t available =
        (uint32_t)(sizeof(virtual_formats) / sizeof(virtual_formats[0]));
    if (formats == NULL) {
        *format_count = available;
        return VK_SUCCESS;
    }
    const uint32_t capacity = *format_count;
    const uint32_t written = capacity < available ? capacity : available;
    if (written != 0U) {
        memcpy(formats, virtual_formats, written * sizeof(*formats));
    }
    *format_count = written;
    return capacity < available ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult VKAPI_CALL
bvb_bridge_vkGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice physical_device, VkSurfaceKHR surface,
    uint32_t *present_mode_count, VkPresentModeKHR *present_modes) {
    if (present_mode_count == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    struct bvb_physical_device_proxy *physical =
        physical_device_proxy(physical_device);
    if (physical == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    struct bvb_surface_proxy *surface_state = surface_proxy_locked(surface);
    struct bvb_activity_status activity = {0};
    int result = surface_state == NULL ||
                         surface_state->parent_id != physical->parent_id
                     ? -EINVAL
                     : ready_activity_status_locked(&activity);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result != 0) return VK_ERROR_SURFACE_LOST_KHR;
    if (present_modes == NULL) {
        *present_mode_count = 1U;
        return VK_SUCCESS;
    }
    if (*present_mode_count == 0U) return VK_INCOMPLETE;
    present_modes[0] = VK_PRESENT_MODE_FIFO_KHR;
    *present_mode_count = 1U;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL bvb_bridge_vkEnumeratePhysicalDevices(
    VkInstance instance, uint32_t *physical_device_count,
    VkPhysicalDevice *physical_devices) {
    struct bvb_instance_proxy *instance_state = instance_proxy(instance);
    if (instance_state == NULL || physical_device_count == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const uint32_t capacity =
        physical_devices == NULL ? 0U : *physical_device_count;
    int lock_result = pthread_mutex_lock(&bvb_global_client.mutex);
    if (lock_result != 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    int result = connect_locked();
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_PHYSICAL_DEVICES,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_INSTANCE_ID_SIZE,
    };
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_instance_id(
            request.payload, instance_state->wire_id);
    }
    struct bvb_protocol_packet response = {0};
    if (result == 0) {
        result = exchange_locked(&request, &response);
    }
    if (result == 0 && response.header.status != 0) {
        result = -EPROTO;
    }
    struct bvb_vulkan_physical_devices decoded;
    if (result == 0) {
        result = bvb_protocol_decode_vulkan_physical_devices(
            response.payload, response.header.payload_length, &decoded);
    }
    VkResult vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    if (result == 0) {
        vulkan_result = (VkResult)decoded.vulkan_result;
        if (vulkan_result == VK_SUCCESS || vulkan_result == VK_INCOMPLETE) {
            if (physical_devices == NULL) {
                *physical_device_count = decoded.count;
            } else {
                const uint32_t written =
                    capacity < decoded.count ? capacity : decoded.count;
                for (uint32_t index = 0U; index < written; ++index) {
                    struct bvb_physical_device_proxy *proxy =
                        physical_proxy_locked(decoded.ids[index],
                                              instance_state->wire_id);
                    if (proxy == NULL) {
                        vulkan_result = VK_ERROR_OUT_OF_HOST_MEMORY;
                        break;
                    }
                    physical_devices[index] = (VkPhysicalDevice)proxy;
                }
                *physical_device_count = written;
                if (vulkan_result != VK_ERROR_OUT_OF_HOST_MEMORY &&
                    capacity < decoded.count) {
                    vulkan_result = VK_INCOMPLETE;
                }
            }
        }
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return result == 0 ? vulkan_result : VK_ERROR_INITIALIZATION_FAILED;
}

static int physical_query_locked(
    uint16_t opcode, const struct bvb_physical_device_proxy *proxy,
    struct bvb_protocol_packet *response) {
    if (proxy == NULL || response == NULL) {
        return -EINVAL;
    }
    int result = connect_locked();
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = opcode,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_PHYSICAL_DEVICE_ID_SIZE,
    };
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_physical_device_id(
            request.payload, proxy->wire_id);
    }
    if (result == 0) {
        result = exchange_locked(&request, response);
    }
    if (result == 0 && response->header.status != 0) {
        result = response->header.status;
    }
    return result;
}

static void VKAPI_CALL bvb_bridge_vkGetPhysicalDeviceProperties(
    VkPhysicalDevice physical_device, VkPhysicalDeviceProperties *properties) {
    if (properties == NULL) {
        return;
    }
    memset(properties, 0, sizeof(*properties));
    struct bvb_physical_device_proxy *proxy =
        physical_device_proxy(physical_device);
    if (proxy == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return;
    }
    struct bvb_protocol_packet response = {0};
    int result = physical_query_locked(
        BVB_OPCODE_VULKAN_PHYSICAL_DEVICE_PROPERTIES, proxy, &response);
    if (result == 0) {
        result = bvb_vulkan_decode_physical_device_properties(
            response.payload, response.header.payload_length, properties);
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result != 0) {
        memset(properties, 0, sizeof(*properties));
    }
    if (getenv("BVB_ICD_DIAGNOSTICS") != NULL) {
        fprintf(stderr,
                "BVB_ICD_PHYSICAL_PROPERTIES status=%d api=%u driver=%u "
                "vendor=%u device=%u name=%s\n",
                result, properties->apiVersion, properties->driverVersion,
                properties->vendorID, properties->deviceID,
                properties->deviceName);
    }
}

static void VKAPI_CALL bvb_bridge_vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice physical_device, uint32_t *property_count,
    VkQueueFamilyProperties *properties) {
    if (property_count == NULL) {
        return;
    }
    const uint32_t capacity = properties == NULL ? 0U : *property_count;
    struct bvb_physical_device_proxy *proxy =
        physical_device_proxy(physical_device);
    if (proxy == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        *property_count = 0U;
        return;
    }
    struct bvb_protocol_packet response = {0};
    int result = physical_query_locked(
        BVB_OPCODE_VULKAN_QUEUE_FAMILY_PROPERTIES, proxy, &response);
    VkQueueFamilyProperties decoded[BVB_VULKAN_MAX_QUEUE_FAMILIES];
    uint32_t available = 0U;
    if (result == 0) {
        result = bvb_vulkan_decode_queue_family_properties(
            response.payload, response.header.payload_length,
            decoded, &available);
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result != 0) {
        *property_count = 0U;
        return;
    }
    if (properties == NULL) {
        *property_count = available;
        return;
    }
    const uint32_t written = capacity < available ? capacity : available;
    if (written != 0U) {
        memcpy(properties, decoded, written * sizeof(*properties));
    }
    *property_count = written;
}

static void VKAPI_CALL bvb_bridge_vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physical_device,
    VkPhysicalDeviceMemoryProperties *properties) {
    if (properties == NULL) {
        return;
    }
    memset(properties, 0, sizeof(*properties));
    struct bvb_physical_device_proxy *proxy =
        physical_device_proxy(physical_device);
    if (proxy == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return;
    }
    struct bvb_protocol_packet response = {0};
    int result = physical_query_locked(
        BVB_OPCODE_VULKAN_MEMORY_PROPERTIES, proxy, &response);
    if (result == 0) {
        result = bvb_vulkan_decode_memory_properties(
            response.payload, response.header.payload_length, properties);
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result != 0) {
        memset(properties, 0, sizeof(*properties));
    }
}

static int extension_page_locked(
    const struct bvb_physical_device_proxy *proxy, uint32_t first,
    uint32_t max_count, struct bvb_vulkan_extension_page *page) {
    int result = connect_locked();
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_DEVICE_EXTENSIONS,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_DEVICE_EXTENSION_QUERY_SIZE,
    };
    const struct bvb_vulkan_device_extension_query query = {
        .physical_device_id = proxy->wire_id,
        .first = first,
        .max_count = max_count,
    };
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_device_extension_query(
            request.payload, &query);
    }
    struct bvb_protocol_packet response = {0};
    if (result == 0) {
        result = exchange_locked(&request, &response);
    }
    if (result == 0 && response.header.status != 0) {
        result = response.header.status;
    }
    if (result == 0) {
        result = bvb_vulkan_decode_extension_page(
            response.payload, response.header.payload_length, page);
    }
    return result;
}

static int device_extensions_locked(
    struct bvb_physical_device_proxy *proxy,
    const VkExtensionProperties **properties, uint32_t *count) {
    if (proxy == NULL || properties == NULL || count == NULL) return -EINVAL;
    if (!proxy->device_extensions_valid) {
        struct bvb_vulkan_extension_page page = {0};
        int result = extension_page_locked(proxy, 0U, 0U, &page);
        if (result != 0) return result;
        if (page.vulkan_result != VK_SUCCESS ||
            page.total_count > BVB_VULKAN_MAX_DEVICE_EXTENSIONS) {
            return -EPROTO;
        }
        const uint32_t raw_count = page.total_count;
        const bool virtual_wsi =
            (bvb_global_client.service_flags &
             BVB_SERVICE_ACTIVITY_INGRESS) != 0U;
        const uint32_t allocation_count =
            raw_count + (virtual_wsi ? 1U : 0U);
        VkExtensionProperties *loaded =
            allocation_count == 0U
                ? NULL
                : calloc(allocation_count, sizeof(*loaded));
        if (allocation_count != 0U && loaded == NULL) return -ENOMEM;

        uint32_t raw_offset = 0U;
        uint32_t exposed_count = 0U;
        bool swapchain_found = false;
        while (result == 0 && raw_offset < raw_count) {
            uint32_t requested = raw_count - raw_offset;
            if (requested > BVB_VULKAN_EXTENSION_PAGE_CAPACITY) {
                requested = BVB_VULKAN_EXTENSION_PAGE_CAPACITY;
            }
            result = extension_page_locked(
                proxy, raw_offset, requested, &page);
            if (result != 0 || page.vulkan_result != VK_SUCCESS ||
                page.total_count != raw_count ||
                page.first != raw_offset || page.count == 0U ||
                page.count > requested) {
                result = result != 0 ? result : -EPROTO;
                break;
            }
            for (uint32_t index = 0U; index < page.count; ++index) {
                const VkExtensionProperties *property =
                    &page.properties[index];
                if (strcmp(property->extensionName,
                           VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
                    swapchain_found = true;
                }
                loaded[exposed_count++] = *property;
            }
            raw_offset += page.count;
        }
        if (result == 0 && virtual_wsi && !swapchain_found) {
            VkExtensionProperties *property = &loaded[exposed_count++];
            (void)snprintf(property->extensionName,
                           sizeof(property->extensionName), "%s",
                           VK_KHR_SWAPCHAIN_EXTENSION_NAME);
            property->specVersion = VK_KHR_SWAPCHAIN_SPEC_VERSION;
        }
        if (result != 0) {
            free(loaded);
            return result;
        }
        proxy->device_extensions = loaded;
        proxy->device_extension_count = exposed_count;
        proxy->device_extensions_valid = true;
    }
    *properties = proxy->device_extensions;
    *count = proxy->device_extension_count;
    return 0;
}

static VkResult VKAPI_CALL bvb_bridge_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physical_device, const char *layer_name,
    uint32_t *property_count, VkExtensionProperties *properties) {
    if (property_count == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (layer_name != NULL) {
        return VK_ERROR_LAYER_NOT_PRESENT;
    }
    const uint32_t capacity = properties == NULL ? 0U : *property_count;
    struct bvb_physical_device_proxy *proxy =
        physical_device_proxy(physical_device);
    if (proxy == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkExtensionProperties *available_properties = NULL;
    uint32_t available = 0U;
    int result = device_extensions_locked(
        proxy, &available_properties, &available);
    VkResult vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    if (result == 0 && properties != NULL) {
        const uint32_t written = capacity < available ? capacity : available;
        if (written != 0U) {
            memcpy(properties, available_properties,
                   written * sizeof(*properties));
        }
        *property_count = written;
        vulkan_result = capacity < available ? VK_INCOMPLETE : VK_SUCCESS;
    } else if (result == 0) {
        *property_count = available;
        vulkan_result = VK_SUCCESS;
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return result == 0 ? vulkan_result : VK_ERROR_INITIALIZATION_FAILED;
}

static void VKAPI_CALL bvb_bridge_vkGetPhysicalDeviceFeatures(
    VkPhysicalDevice physical_device, VkPhysicalDeviceFeatures *features) {
    if (features == NULL) {
        return;
    }
    memset(features, 0, sizeof(*features));
    struct bvb_physical_device_proxy *proxy =
        physical_device_proxy(physical_device);
    if (proxy == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return;
    }
    struct bvb_protocol_packet response = {0};
    int result = physical_query_locked(
        BVB_OPCODE_VULKAN_PHYSICAL_DEVICE_FEATURES, proxy, &response);
    if (result == 0) {
        result = bvb_vulkan_decode_physical_device_features(
            response.payload, response.header.payload_length, features);
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result != 0) {
        memset(features, 0, sizeof(*features));
    }
}

static void VKAPI_CALL bvb_bridge_vkGetPhysicalDeviceFormatProperties(
    VkPhysicalDevice physical_device, VkFormat format,
    VkFormatProperties *properties) {
    if (properties == NULL) {
        return;
    }
    memset(properties, 0, sizeof(*properties));
    struct bvb_physical_device_proxy *proxy =
        physical_device_proxy(physical_device);
    if (proxy == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return;
    }
    int result = connect_locked();
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_FORMAT_PROPERTIES,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_FORMAT_QUERY_SIZE,
    };
    const struct bvb_vulkan_format_query query = {
        .physical_device_id = proxy->wire_id,
        .format = (uint32_t)format,
    };
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_format_query(
            request.payload, &query);
    }
    struct bvb_protocol_packet response = {0};
    if (result == 0) {
        result = exchange_locked(&request, &response);
    }
    if (result == 0 && response.header.status != 0) {
        result = response.header.status;
    }
    struct bvb_vulkan_format_properties decoded = {0};
    if (result == 0 && response.header.payload_length !=
                           BVB_VULKAN_FORMAT_PROPERTIES_SIZE) {
        result = -EPROTO;
    }
    if (result == 0) {
        result = bvb_protocol_decode_vulkan_format_properties(
            response.payload, &decoded);
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result == 0) {
        properties->linearTilingFeatures = decoded.linear_tiling_features;
        properties->optimalTilingFeatures = decoded.optimal_tiling_features;
        properties->bufferFeatures = decoded.buffer_features;
    }
}

static int get_format_properties_3(
    struct bvb_physical_device_proxy *proxy, VkFormat format,
    VkFormatProperties3 *properties) {
    if (proxy == NULL || properties == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return -EINVAL;
    }
    int result = connect_locked();
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_FORMAT_PROPERTIES_3,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_FORMAT_QUERY_SIZE,
    };
    const struct bvb_vulkan_format_query query = {
        .physical_device_id = proxy->wire_id,
        .format = (uint32_t)format,
    };
    if (result == 0)
        result = bvb_protocol_encode_vulkan_format_query(
            request.payload, &query);
    struct bvb_protocol_packet response = {0};
    if (result == 0) result = exchange_locked(&request, &response);
    if (result == 0 && response.header.status != 0)
        result = response.header.status;
    struct bvb_vulkan_format_properties_3 decoded = {0};
    if (result == 0 && response.header.payload_length !=
                           BVB_VULKAN_FORMAT_PROPERTIES_3_SIZE) {
        result = -EPROTO;
    }
    if (result == 0)
        result = bvb_protocol_decode_vulkan_format_properties_3(
            response.payload, &decoded);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result == 0) {
        properties->linearTilingFeatures = decoded.linear_tiling_features;
        properties->optimalTilingFeatures = decoded.optimal_tiling_features;
        properties->bufferFeatures = decoded.buffer_features;
    }
    return result;
}

static VkResult VKAPI_CALL
bvb_bridge_vkGetPhysicalDeviceImageFormatProperties(
    VkPhysicalDevice physical_device, VkFormat format, VkImageType type,
    VkImageTiling tiling, VkImageUsageFlags usage,
    VkImageCreateFlags flags, VkImageFormatProperties *properties) {
    if (properties == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    memset(properties, 0, sizeof(*properties));
    struct bvb_physical_device_proxy *proxy =
        physical_device_proxy(physical_device);
    if (proxy == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    int result = connect_locked();
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_IMAGE_FORMAT_PROPERTIES,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_IMAGE_FORMAT_QUERY_SIZE,
    };
    const struct bvb_vulkan_image_format_query query = {
        .physical_device_id = proxy->wire_id,
        .format = (uint32_t)format,
        .type = (uint32_t)type,
        .tiling = (uint32_t)tiling,
        .usage = (uint32_t)usage,
        .flags = (uint32_t)flags,
    };
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_image_format_query(
            request.payload, &query);
    }
    struct bvb_protocol_packet response = {0};
    if (result == 0) {
        result = exchange_locked(&request, &response);
    }
    if (result == 0 && response.header.status != 0) {
        result = response.header.status;
    }
    struct bvb_vulkan_image_format_properties decoded = {0};
    if (result == 0 && response.header.payload_length !=
                           BVB_VULKAN_IMAGE_FORMAT_PROPERTIES_SIZE) {
        result = -EPROTO;
    }
    if (result == 0) {
        result = bvb_protocol_decode_vulkan_image_format_properties(
            response.payload, &decoded);
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result != 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (decoded.vulkan_result == VK_SUCCESS) {
        properties->maxExtent = (VkExtent3D){
            decoded.max_extent_width,
            decoded.max_extent_height,
            decoded.max_extent_depth,
        };
        properties->maxMipLevels = decoded.max_mip_levels;
        properties->maxArrayLayers = decoded.max_array_layers;
        properties->sampleCounts = decoded.sample_counts;
        properties->maxResourceSize = decoded.max_resource_size;
    }
    return (VkResult)decoded.vulkan_result;
}

static void VKAPI_CALL
bvb_bridge_vkGetPhysicalDeviceExternalBufferProperties(
    VkPhysicalDevice physical_device,
    const VkPhysicalDeviceExternalBufferInfo *info,
    VkExternalBufferProperties *properties) {
    if (info == NULL || properties == NULL ||
        info->sType !=
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO ||
        properties->sType != VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES) {
        return;
    }
    properties->externalMemoryProperties = (VkExternalMemoryProperties){0};
    struct bvb_physical_device_proxy *proxy =
        physical_device_proxy(physical_device);
    if (proxy == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return;
    }
    int result = connect_locked();
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_EXTERNAL_BUFFER_PROPERTIES,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_EXTERNAL_BUFFER_QUERY_SIZE,
    };
    const struct bvb_vulkan_external_buffer_query query = {
        .physical_device_id = proxy->wire_id,
        .flags = (uint32_t)info->flags,
        .usage = (uint32_t)info->usage,
        .handle_type = (uint32_t)info->handleType,
    };
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_external_buffer_query(
            request.payload, &query);
    }
    struct bvb_protocol_packet response = {0};
    if (result == 0) {
        result = exchange_locked(&request, &response);
    }
    if (result == 0 && response.header.status != 0) {
        result = response.header.status;
    }
    struct bvb_vulkan_external_buffer_properties decoded = {0};
    if (result == 0 && response.header.payload_length !=
                           BVB_VULKAN_EXTERNAL_BUFFER_PROPERTIES_SIZE) {
        result = -EPROTO;
    }
    if (result == 0) {
        result = bvb_protocol_decode_vulkan_external_buffer_properties(
            response.payload, &decoded);
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result == 0) {
        properties->externalMemoryProperties = (VkExternalMemoryProperties){
            .externalMemoryFeatures =
                (VkExternalMemoryFeatureFlags)
                    decoded.external_memory_features,
            .exportFromImportedHandleTypes =
                (VkExternalMemoryHandleTypeFlags)
                    decoded.export_from_imported_handle_types,
            .compatibleHandleTypes =
                (VkExternalMemoryHandleTypeFlags)
                    decoded.compatible_handle_types,
        };
    }
}

static void VKAPI_CALL
bvb_bridge_vkGetPhysicalDeviceExternalSemaphoreProperties(
    VkPhysicalDevice physical_device,
    const VkPhysicalDeviceExternalSemaphoreInfo *info,
    VkExternalSemaphoreProperties *properties) {
    if (info == NULL || properties == NULL ||
        info->sType !=
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO ||
        properties->sType !=
            VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES) {
        return;
    }
    properties->exportFromImportedHandleTypes = 0U;
    properties->compatibleHandleTypes = 0U;
    properties->externalSemaphoreFeatures = 0U;
    struct bvb_physical_device_proxy *proxy =
        physical_device_proxy(physical_device);
    if (proxy == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return;
    }
    int result = connect_locked();
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_EXTERNAL_SEMAPHORE_PROPERTIES,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_EXTERNAL_SEMAPHORE_QUERY_SIZE,
    };
    const struct bvb_vulkan_external_semaphore_query query = {
        .physical_device_id = proxy->wire_id,
        .handle_type = (uint32_t)info->handleType,
    };
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_external_semaphore_query(
            request.payload, &query);
    }
    struct bvb_protocol_packet response = {0};
    if (result == 0) {
        result = exchange_locked(&request, &response);
    }
    if (result == 0 && response.header.status != 0) {
        result = response.header.status;
    }
    struct bvb_vulkan_external_semaphore_properties decoded = {0};
    if (result == 0 && response.header.payload_length !=
                           BVB_VULKAN_EXTERNAL_SEMAPHORE_PROPERTIES_SIZE) {
        result = -EPROTO;
    }
    if (result == 0) {
        result = bvb_protocol_decode_vulkan_external_semaphore_properties(
            response.payload, &decoded);
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result == 0) {
        properties->exportFromImportedHandleTypes =
            (VkExternalSemaphoreHandleTypeFlags)
                decoded.export_from_imported_handle_types;
        properties->compatibleHandleTypes =
            (VkExternalSemaphoreHandleTypeFlags)
                decoded.compatible_handle_types;
        properties->externalSemaphoreFeatures =
            (VkExternalSemaphoreFeatureFlags)
                decoded.external_semaphore_features;
    }
}

static void VKAPI_CALL
bvb_bridge_vkGetPhysicalDeviceSparseImageFormatProperties(
    VkPhysicalDevice physical_device, VkFormat format, VkImageType type,
    VkSampleCountFlagBits samples, VkImageUsageFlags usage,
    VkImageTiling tiling, uint32_t *property_count,
    VkSparseImageFormatProperties *properties) {
    (void)format;
    (void)type;
    (void)samples;
    (void)usage;
    (void)tiling;
    (void)properties;
    if (physical_device_proxy(physical_device) == NULL ||
        property_count == NULL) {
        return;
    }
    *property_count = 0U;
}

static int core_features_locked(
    const struct bvb_physical_device_proxy *proxy,
    struct bvb_vulkan_core_features *features) {
    struct bvb_protocol_packet response = {0};
    int result = physical_query_locked(
        BVB_OPCODE_VULKAN_CORE_FEATURES, proxy, &response);
    if (result == 0 &&
        response.header.payload_length !=
            BVB_VULKAN_CORE_FEATURES_SIZE) {
        result = -EPROTO;
    }
    if (result == 0) {
        result = bvb_protocol_decode_vulkan_core_features(
            response.payload, features);
    }
    return result;
}

static void VKAPI_CALL bvb_bridge_vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice physical_device, VkPhysicalDeviceFeatures2 *features) {
    if (features == NULL ||
        features->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
        return;
    }
    bvb_bridge_vkGetPhysicalDeviceFeatures(
        physical_device, &features->features);
    bool requested = false;
    VkBaseOutStructure *entry = (VkBaseOutStructure *)features->pNext;
    for (uint32_t index = 0U; entry != NULL && index < 64U; ++index) {
        if (getenv("BVB_ICD_DIAGNOSTICS") != NULL) {
            fprintf(stderr,
                    "BVB_ICD_FEATURE_CHAIN index=%u stype=%u\n",
                    index, (uint32_t)entry->sType);
        }
        requested |=
            entry->sType ==
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES ||
            entry->sType ==
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES ||
            entry->sType ==
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES ||
            entry->sType ==
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES ||
            entry->sType ==
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
        requested |=
            entry->sType ==
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT ||
            entry->sType ==
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT ||
            entry->sType ==
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR ||
            entry->sType ==
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR;
        entry = entry->pNext;
    }
    if (!requested) {
        return;
    }
    struct bvb_physical_device_proxy *proxy =
        physical_device_proxy(physical_device);
    if (proxy == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return;
    }
    struct bvb_vulkan_core_features bridged = {0};
    const int result = core_features_locked(proxy, &bridged);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result != 0) {
        return;
    }
    entry = (VkBaseOutStructure *)features->pNext;
    for (uint32_t index = 0U; entry != NULL && index < 64U; ++index) {
        if (entry->sType ==
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) {
            VkPhysicalDeviceVulkan11Features *vulkan11 =
                (VkPhysicalDeviceVulkan11Features *)entry;
            vulkan11->shaderDrawParameters =
                (VkBool32)bridged.shader_draw_parameters;
        } else if (entry->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES) {
            VkPhysicalDeviceShaderDrawParametersFeatures *shader_draw =
                (VkPhysicalDeviceShaderDrawParametersFeatures *)entry;
            shader_draw->shaderDrawParameters =
                (VkBool32)bridged.shader_draw_parameters;
        } else if (entry->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            VkPhysicalDeviceVulkan12Features *vulkan12 =
                (VkPhysicalDeviceVulkan12Features *)entry;
            vulkan12->bufferDeviceAddress =
                (VkBool32)bridged.buffer_device_address;
            vulkan12->descriptorIndexing =
                (VkBool32)bridged.descriptor_indexing;
            vulkan12->descriptorBindingSampledImageUpdateAfterBind =
                (VkBool32)bridged
                    .descriptor_binding_sampled_image_update_after_bind;
            vulkan12->descriptorBindingUpdateUnusedWhilePending =
                (VkBool32)bridged
                    .descriptor_binding_update_unused_while_pending;
            vulkan12->descriptorBindingPartiallyBound =
                (VkBool32)bridged.descriptor_binding_partially_bound;
            vulkan12->hostQueryReset =
                (VkBool32)bridged.host_query_reset;
            vulkan12->runtimeDescriptorArray =
                (VkBool32)bridged.runtime_descriptor_array;
            vulkan12->samplerMirrorClampToEdge =
                (VkBool32)bridged.sampler_mirror_clamp_to_edge;
            vulkan12->scalarBlockLayout =
                (VkBool32)bridged.scalar_block_layout;
            vulkan12->timelineSemaphore =
                (VkBool32)bridged.timeline_semaphore;
            vulkan12->uniformBufferStandardLayout =
                (VkBool32)bridged.uniform_buffer_standard_layout;
            vulkan12->vulkanMemoryModel =
                (VkBool32)bridged.vulkan_memory_model;
        } else if (entry->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
            VkPhysicalDeviceVulkan13Features *vulkan13 =
                (VkPhysicalDeviceVulkan13Features *)entry;
            vulkan13->computeFullSubgroups =
                (VkBool32)bridged.compute_full_subgroups;
            vulkan13->dynamicRendering =
                (VkBool32)bridged.dynamic_rendering;
            vulkan13->maintenance4 = (VkBool32)bridged.maintenance4;
            vulkan13->shaderDemoteToHelperInvocation =
                (VkBool32)bridged.shader_demote_to_helper_invocation;
            vulkan13->shaderZeroInitializeWorkgroupMemory =
                (VkBool32)bridged
                    .shader_zero_initialize_workgroup_memory;
            vulkan13->subgroupSizeControl =
                (VkBool32)bridged.subgroup_size_control;
            vulkan13->synchronization2 =
                (VkBool32)bridged.synchronization2;
        } else if (entry->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT) {
            VkPhysicalDeviceDepthClipEnableFeaturesEXT *depth_clip =
                (VkPhysicalDeviceDepthClipEnableFeaturesEXT *)entry;
            depth_clip->depthClipEnable =
                (VkBool32)bridged.depth_clip_enable;
        } else if (entry->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT) {
            VkPhysicalDeviceRobustness2FeaturesEXT *robustness2 =
                (VkPhysicalDeviceRobustness2FeaturesEXT *)entry;
            robustness2->robustBufferAccess2 =
                (VkBool32)bridged.robust_buffer_access2;
            robustness2->nullDescriptor =
                (VkBool32)bridged.null_descriptor;
        } else if (entry->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR) {
            VkPhysicalDeviceMaintenance5FeaturesKHR *maintenance5 =
                (VkPhysicalDeviceMaintenance5FeaturesKHR *)entry;
            maintenance5->maintenance5 =
                (VkBool32)bridged.maintenance5;
        } else if (entry->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR) {
            VkPhysicalDeviceMaintenance6FeaturesKHR *maintenance6 =
                (VkPhysicalDeviceMaintenance6FeaturesKHR *)entry;
            maintenance6->maintenance6 =
                (VkBool32)bridged.maintenance6;
        } else if (entry->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES) {
            VkPhysicalDeviceBufferDeviceAddressFeatures *buffer_address =
                (VkPhysicalDeviceBufferDeviceAddressFeatures *)entry;
            buffer_address->bufferDeviceAddress =
                (VkBool32)bridged.buffer_device_address;
        }
        entry = entry->pNext;
    }
}

static void VKAPI_CALL bvb_bridge_vkGetPhysicalDeviceProperties2(
    VkPhysicalDevice physical_device,
    VkPhysicalDeviceProperties2 *properties) {
    if (properties == NULL ||
        properties->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2) {
        return;
    }
    bvb_bridge_vkGetPhysicalDeviceProperties(
        physical_device, &properties->properties);
}

static void VKAPI_CALL bvb_bridge_vkGetPhysicalDeviceFormatProperties2(
    VkPhysicalDevice physical_device, VkFormat format,
    VkFormatProperties2 *properties) {
    if (properties == NULL ||
        properties->sType !=
            VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2) {
        return;
    }
    bvb_bridge_vkGetPhysicalDeviceFormatProperties(
        physical_device, format, &properties->formatProperties);
    VkBaseOutStructure *next = properties->pNext;
    for (uint32_t count = 0U; next != NULL && count < 8U; ++count) {
        if (next->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) {
            VkFormatProperties3 *properties_3 =
                (VkFormatProperties3 *)next;
            properties_3->linearTilingFeatures = 0U;
            properties_3->optimalTilingFeatures = 0U;
            properties_3->bufferFeatures = 0U;
            (void)get_format_properties_3(
                physical_device_proxy(physical_device), format, properties_3);
        }
        next = next->pNext;
    }
}

static VkResult VKAPI_CALL
bvb_bridge_vkGetPhysicalDeviceImageFormatProperties2(
    VkPhysicalDevice physical_device,
    const VkPhysicalDeviceImageFormatInfo2 *image_info,
    VkImageFormatProperties2 *properties) {
    if (image_info == NULL || properties == NULL ||
        image_info->sType !=
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2 ||
        properties->sType !=
            VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 ||
        image_info->pNext != NULL || properties->pNext != NULL) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    return bvb_bridge_vkGetPhysicalDeviceImageFormatProperties(
        physical_device, image_info->format, image_info->type,
        image_info->tiling, image_info->usage, image_info->flags,
        &properties->imageFormatProperties);
}

static void VKAPI_CALL
bvb_bridge_vkGetPhysicalDeviceQueueFamilyProperties2(
    VkPhysicalDevice physical_device, uint32_t *property_count,
    VkQueueFamilyProperties2 *properties) {
    if (property_count == NULL) {
        return;
    }
    if (properties == NULL) {
        bvb_bridge_vkGetPhysicalDeviceQueueFamilyProperties(
            physical_device, property_count, NULL);
        return;
    }
    const uint32_t capacity = *property_count;
    if (capacity > BVB_VULKAN_MAX_QUEUE_FAMILIES) {
        *property_count = 0U;
        return;
    }
    VkQueueFamilyProperties decoded[BVB_VULKAN_MAX_QUEUE_FAMILIES];
    uint32_t written = capacity;
    bvb_bridge_vkGetPhysicalDeviceQueueFamilyProperties(
        physical_device, &written, decoded);
    for (uint32_t index = 0U; index < written; ++index) {
        if (properties[index].sType ==
            VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2) {
            properties[index].queueFamilyProperties = decoded[index];
        }
    }
    *property_count = written;
}

static void VKAPI_CALL bvb_bridge_vkGetPhysicalDeviceMemoryProperties2(
    VkPhysicalDevice physical_device,
    VkPhysicalDeviceMemoryProperties2 *properties) {
    if (properties == NULL || properties->sType !=
                                  VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2) {
        return;
    }
    bvb_bridge_vkGetPhysicalDeviceMemoryProperties(
        physical_device, &properties->memoryProperties);
}

static void VKAPI_CALL
bvb_bridge_vkGetPhysicalDeviceSparseImageFormatProperties2(
    VkPhysicalDevice physical_device,
    const VkPhysicalDeviceSparseImageFormatInfo2 *format_info,
    uint32_t *property_count,
    VkSparseImageFormatProperties2 *properties) {
    (void)properties;
    if (physical_device_proxy(physical_device) == NULL ||
        format_info == NULL || property_count == NULL ||
        format_info->sType !=
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SPARSE_IMAGE_FORMAT_INFO_2) {
        return;
    }
    *property_count = 0U;
}

#define BVB_FEATURE_INDEX(type, first, field) \
    ((offsetof(type, field) - offsetof(type, first)) / sizeof(VkBool32))

_Static_assert(sizeof(VkPhysicalDeviceFeatures) ==
                   BVB_VULKAN_BASE_FEATURES_SIZE,
               "VkPhysicalDeviceFeatures wire size changed");

static bool requested_feature_bools_are_supported(
    const VkBool32 *values, size_t value_count,
    const size_t *supported_indices, size_t supported_count) {
    for (size_t index = 0U; index < value_count; ++index) {
        if (values[index] > VK_TRUE) {
            return false;
        }
        if (values[index] == VK_FALSE) {
            continue;
        }
        bool supported = false;
        for (size_t candidate = 0U; candidate < supported_count;
             ++candidate) {
            if (supported_indices[candidate] == index) {
                supported = true;
                break;
            }
        }
        if (!supported) {
            return false;
        }
    }
    return true;
}

static bool zero_only_extension_feature_struct(
    const VkBaseInStructure *entry) {
    const VkBool32 *values = NULL;
    size_t count = 0U;
#define BVB_ZERO_FEATURE_RANGE(type, first, last) \
    do { \
        const type *features = (const type *)entry; \
        values = &features->first; \
        count = BVB_FEATURE_INDEX(type, first, last) + 1U; \
    } while (0)
    switch (entry->sType) {
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_UNIFORM_CONTROL_FLOW_FEATURES_KHR:
        BVB_ZERO_FEATURE_RANGE(
            VkPhysicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR,
            shaderSubgroupUniformControlFlow,
            shaderSubgroupUniformControlFlow);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES:
        BVB_ZERO_FEATURE_RANGE(
            VkPhysicalDeviceVertexAttributeDivisorFeatures,
            vertexAttributeInstanceRateDivisor,
            vertexAttributeInstanceRateZeroDivisor);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT:
        BVB_ZERO_FEATURE_RANGE(
            VkPhysicalDeviceTransformFeedbackFeaturesEXT,
            transformFeedback, geometryStreams);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT:
        BVB_ZERO_FEATURE_RANGE(
            VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT,
            shaderModuleIdentifier, shaderModuleIdentifier);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT:
        BVB_ZERO_FEATURE_RANGE(
            VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT,
            nonSeamlessCubeMap, nonSeamlessCubeMap);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT:
        BVB_ZERO_FEATURE_RANGE(
            VkPhysicalDeviceMultiDrawFeaturesEXT, multiDraw, multiDraw);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES:
        BVB_ZERO_FEATURE_RANGE(
            VkPhysicalDeviceLineRasterizationFeatures, rectangularLines,
            stippledSmoothLines);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT:
        BVB_ZERO_FEATURE_RANGE(
            VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT,
            graphicsPipelineLibrary, graphicsPipelineLibrary);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT:
        BVB_ZERO_FEATURE_RANGE(
            VkPhysicalDeviceExtendedDynamicState3FeaturesEXT,
            extendedDynamicState3TessellationDomainOrigin,
            extendedDynamicState3ShadingRateImageEnable);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT:
        BVB_ZERO_FEATURE_RANGE(
            VkPhysicalDeviceDescriptorBufferFeaturesEXT, descriptorBuffer,
            descriptorBufferPushDescriptors);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT:
        BVB_ZERO_FEATURE_RANGE(
            VkPhysicalDeviceCustomBorderColorFeaturesEXT,
            customBorderColors, customBorderColorWithoutFormat);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BORDER_COLOR_SWIZZLE_FEATURES_EXT:
        BVB_ZERO_FEATURE_RANGE(
            VkPhysicalDeviceBorderColorSwizzleFeaturesEXT,
            borderColorSwizzle, borderColorSwizzleFromImage);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT:
        BVB_ZERO_FEATURE_RANGE(
            VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT,
            attachmentFeedbackLoopLayout, attachmentFeedbackLoopLayout);
        break;
    default:
        return false;
    }
#undef BVB_ZERO_FEATURE_RANGE
    return requested_feature_bools_are_supported(values, count, NULL, 0U);
}

static VkResult find_device_loader_data_callback(
    const void *chain, PFN_vkSetDeviceLoaderData *callback) {
    if (callback == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *callback = NULL;
    const VkBaseInStructure *entry = chain;
    for (uint32_t index = 0U; entry != NULL && index < 64U; ++index) {
        if ((uint32_t)entry->sType == UINT32_C(0x7ffffffe)) {
            return VK_SUCCESS;
        }
        if (entry->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            const VkLayerDeviceCreateInfo *loader_info =
                (const VkLayerDeviceCreateInfo *)entry;
            if (loader_info->function == VK_LOADER_DATA_CALLBACK) {
                if (loader_info->u.pfnSetDeviceLoaderData == NULL ||
                    (*callback != NULL &&
                     *callback != loader_info->u.pfnSetDeviceLoaderData)) {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                *callback = loader_info->u.pfnSetDeviceLoaderData;
            }
        }
        entry = entry->pNext;
    }
    return entry == NULL ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
}

static VkResult pack_device_feature_chain(
    const void *chain,
    struct bvb_vulkan_device_create_packed_request *packed) {
    const VkBaseInStructure *entry = chain;
    for (uint32_t index = 0U; entry != NULL && index < 64U; ++index) {
        /* The standard loader's private chain is client-local metadata. */
        if ((uint32_t)entry->sType == UINT32_C(0x7ffffffe)) {
            return VK_SUCCESS;
        }
        if (entry->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            entry = entry->pNext;
            continue;
        }
        uint32_t feature_bit = 0U;
        if (entry->sType ==
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) {
            const VkPhysicalDeviceVulkan11Features *features =
                (const VkPhysicalDeviceVulkan11Features *)entry;
            const size_t supported[] = {
                BVB_FEATURE_INDEX(VkPhysicalDeviceVulkan11Features,
                                  storageBuffer16BitAccess,
                                  shaderDrawParameters),
            };
            const size_t count =
                BVB_FEATURE_INDEX(VkPhysicalDeviceVulkan11Features,
                                  storageBuffer16BitAccess,
                                  shaderDrawParameters) + 1U;
            if (!requested_feature_bools_are_supported(
                    &features->storageBuffer16BitAccess, count,
                    supported, sizeof(supported) / sizeof(supported[0]))) {
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            feature_bit = BVB_VULKAN_DEVICE_FEATURE_VULKAN_11;
            packed->enabled_features.shader_draw_parameters =
                (uint32_t)features->shaderDrawParameters;
        } else if (entry->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            const VkPhysicalDeviceVulkan12Features *features =
                (const VkPhysicalDeviceVulkan12Features *)entry;
            const size_t supported[] = {
                BVB_FEATURE_INDEX(VkPhysicalDeviceVulkan12Features,
                                  samplerMirrorClampToEdge,
                                  samplerMirrorClampToEdge),
                BVB_FEATURE_INDEX(VkPhysicalDeviceVulkan12Features,
                                  samplerMirrorClampToEdge,
                                  descriptorIndexing),
                BVB_FEATURE_INDEX(VkPhysicalDeviceVulkan12Features,
                                  samplerMirrorClampToEdge,
                                  descriptorBindingSampledImageUpdateAfterBind),
                BVB_FEATURE_INDEX(VkPhysicalDeviceVulkan12Features,
                                  samplerMirrorClampToEdge,
                                  descriptorBindingUpdateUnusedWhilePending),
                BVB_FEATURE_INDEX(VkPhysicalDeviceVulkan12Features,
                                  samplerMirrorClampToEdge,
                                  descriptorBindingPartiallyBound),
                BVB_FEATURE_INDEX(VkPhysicalDeviceVulkan12Features,
                                  samplerMirrorClampToEdge,
                                  runtimeDescriptorArray),
                BVB_FEATURE_INDEX(VkPhysicalDeviceVulkan12Features,
                                  samplerMirrorClampToEdge,
                                  scalarBlockLayout),
                BVB_FEATURE_INDEX(VkPhysicalDeviceVulkan12Features,
                                  samplerMirrorClampToEdge,
                                  uniformBufferStandardLayout),
                BVB_FEATURE_INDEX(VkPhysicalDeviceVulkan12Features,
                                  samplerMirrorClampToEdge,
                                  hostQueryReset),
                BVB_FEATURE_INDEX(VkPhysicalDeviceVulkan12Features,
                                  samplerMirrorClampToEdge,
                                  timelineSemaphore),
                BVB_FEATURE_INDEX(VkPhysicalDeviceVulkan12Features,
                                  samplerMirrorClampToEdge,
                                  bufferDeviceAddress),
                BVB_FEATURE_INDEX(VkPhysicalDeviceVulkan12Features,
                                  samplerMirrorClampToEdge,
                                  vulkanMemoryModel),
            };
            const size_t count =
                BVB_FEATURE_INDEX(VkPhysicalDeviceVulkan12Features,
                                  samplerMirrorClampToEdge,
                                  subgroupBroadcastDynamicId) + 1U;
            if (!requested_feature_bools_are_supported(
                    &features->samplerMirrorClampToEdge, count,
                    supported, sizeof(supported) / sizeof(supported[0]))) {
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            feature_bit = BVB_VULKAN_DEVICE_FEATURE_VULKAN_12;
            packed->enabled_features.buffer_device_address =
                (uint32_t)features->bufferDeviceAddress;
            packed->enabled_features.descriptor_indexing =
                (uint32_t)features->descriptorIndexing;
            packed->enabled_features
                    .descriptor_binding_sampled_image_update_after_bind =
                (uint32_t)features
                    ->descriptorBindingSampledImageUpdateAfterBind;
            packed->enabled_features
                    .descriptor_binding_update_unused_while_pending =
                (uint32_t)features
                    ->descriptorBindingUpdateUnusedWhilePending;
            packed->enabled_features.descriptor_binding_partially_bound =
                (uint32_t)features->descriptorBindingPartiallyBound;
            packed->enabled_features.host_query_reset =
                (uint32_t)features->hostQueryReset;
            packed->enabled_features.runtime_descriptor_array =
                (uint32_t)features->runtimeDescriptorArray;
            packed->enabled_features.sampler_mirror_clamp_to_edge =
                (uint32_t)features->samplerMirrorClampToEdge;
            packed->enabled_features.scalar_block_layout =
                (uint32_t)features->scalarBlockLayout;
            packed->enabled_features.timeline_semaphore =
                (uint32_t)features->timelineSemaphore;
            packed->enabled_features.uniform_buffer_standard_layout =
                (uint32_t)features->uniformBufferStandardLayout;
            packed->enabled_features.vulkan_memory_model =
                (uint32_t)features->vulkanMemoryModel;
        } else if (entry->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
            const VkPhysicalDeviceVulkan13Features *features =
                (const VkPhysicalDeviceVulkan13Features *)entry;
            const size_t supported[] = {
                BVB_FEATURE_INDEX(VkPhysicalDeviceVulkan13Features,
                                  robustImageAccess,
                                  shaderDemoteToHelperInvocation),
                BVB_FEATURE_INDEX(VkPhysicalDeviceVulkan13Features,
                                  robustImageAccess,
                                  subgroupSizeControl),
                BVB_FEATURE_INDEX(VkPhysicalDeviceVulkan13Features,
                                  robustImageAccess,
                                  computeFullSubgroups),
                BVB_FEATURE_INDEX(VkPhysicalDeviceVulkan13Features,
                                  robustImageAccess,
                                  synchronization2),
                BVB_FEATURE_INDEX(VkPhysicalDeviceVulkan13Features,
                                  robustImageAccess,
                                  shaderZeroInitializeWorkgroupMemory),
                BVB_FEATURE_INDEX(VkPhysicalDeviceVulkan13Features,
                                  robustImageAccess,
                                  dynamicRendering),
                BVB_FEATURE_INDEX(VkPhysicalDeviceVulkan13Features,
                                  robustImageAccess,
                                  maintenance4),
            };
            const size_t count =
                BVB_FEATURE_INDEX(VkPhysicalDeviceVulkan13Features,
                                  robustImageAccess, maintenance4) + 1U;
            if (!requested_feature_bools_are_supported(
                    &features->robustImageAccess, count,
                    supported, sizeof(supported) / sizeof(supported[0]))) {
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            feature_bit = BVB_VULKAN_DEVICE_FEATURE_VULKAN_13;
            packed->enabled_features.compute_full_subgroups =
                (uint32_t)features->computeFullSubgroups;
            packed->enabled_features.dynamic_rendering =
                (uint32_t)features->dynamicRendering;
            packed->enabled_features.maintenance4 =
                (uint32_t)features->maintenance4;
            packed->enabled_features.shader_demote_to_helper_invocation =
                (uint32_t)features->shaderDemoteToHelperInvocation;
            packed->enabled_features
                    .shader_zero_initialize_workgroup_memory =
                (uint32_t)features->shaderZeroInitializeWorkgroupMemory;
            packed->enabled_features.subgroup_size_control =
                (uint32_t)features->subgroupSizeControl;
            packed->enabled_features.synchronization2 =
                (uint32_t)features->synchronization2;
        } else if (entry->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT) {
            const VkPhysicalDeviceDepthClipEnableFeaturesEXT *features =
                (const VkPhysicalDeviceDepthClipEnableFeaturesEXT *)entry;
            if (features->depthClipEnable > VK_TRUE) {
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            feature_bit = BVB_VULKAN_DEVICE_FEATURE_DEPTH_CLIP_ENABLE;
            packed->enabled_features.depth_clip_enable =
                (uint32_t)features->depthClipEnable;
        } else if (entry->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT) {
            const VkPhysicalDeviceRobustness2FeaturesEXT *features =
                (const VkPhysicalDeviceRobustness2FeaturesEXT *)entry;
            const size_t supported[] = {0U, 2U};
            if (!requested_feature_bools_are_supported(
                    &features->robustBufferAccess2, 3U, supported,
                    sizeof(supported) / sizeof(supported[0]))) {
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            feature_bit = BVB_VULKAN_DEVICE_FEATURE_ROBUSTNESS_2;
            packed->enabled_features.robust_buffer_access2 =
                (uint32_t)features->robustBufferAccess2;
            packed->enabled_features.null_descriptor =
                (uint32_t)features->nullDescriptor;
        } else if (entry->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR) {
            const VkPhysicalDeviceMaintenance5FeaturesKHR *features =
                (const VkPhysicalDeviceMaintenance5FeaturesKHR *)entry;
            if (features->maintenance5 > VK_TRUE) {
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            feature_bit = BVB_VULKAN_DEVICE_FEATURE_MAINTENANCE_5;
            packed->enabled_features.maintenance5 =
                (uint32_t)features->maintenance5;
        } else if (entry->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR) {
            const VkPhysicalDeviceMaintenance6FeaturesKHR *features =
                (const VkPhysicalDeviceMaintenance6FeaturesKHR *)entry;
            if (features->maintenance6 > VK_TRUE) {
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            feature_bit = BVB_VULKAN_DEVICE_FEATURE_MAINTENANCE_6;
            packed->enabled_features.maintenance6 =
                (uint32_t)features->maintenance6;
        } else if (zero_only_extension_feature_struct(entry)) {
            entry = entry->pNext;
            continue;
        } else {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        if ((packed->enabled_feature_structs & feature_bit) != 0U) {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        packed->enabled_feature_structs |= feature_bit;
        entry = entry->pNext;
    }
    return entry == NULL ? VK_SUCCESS : VK_ERROR_FEATURE_NOT_PRESENT;
}

#undef BVB_FEATURE_INDEX

static VkResult VKAPI_CALL bvb_bridge_vkCreateDevice(
    VkPhysicalDevice physical_device, const VkDeviceCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkDevice *device) {
    if (getenv("BVB_ICD_DIAGNOSTICS") != NULL) {
        fprintf(stderr,
                "BVB_ICD_CREATE_DEVICE physical=%p info=%p stype=%u "
                "pnext=%p flags=%u allocator=%p queues=%u layers=%u "
                "extensions=%u features=%p\n",
                (void *)physical_device, (const void *)create_info,
                create_info == NULL ? 0U : (unsigned int)create_info->sType,
                create_info == NULL ? NULL : create_info->pNext,
                create_info == NULL ? 0U : (unsigned int)create_info->flags,
                (const void *)allocator,
                create_info == NULL ? 0U : create_info->queueCreateInfoCount,
                create_info == NULL ? 0U : create_info->enabledLayerCount,
                create_info == NULL ? 0U : create_info->enabledExtensionCount,
                create_info == NULL
                    ? NULL : (const void *)create_info->pEnabledFeatures);
    }
    if (device != NULL) {
        *device = VK_NULL_HANDLE;
    }
    struct bvb_physical_device_proxy *physical =
        physical_device_proxy(physical_device);
    if (physical == NULL || create_info == NULL || device == NULL ||
        allocator != NULL ||
        create_info->sType != VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO ||
        create_info->flags != 0U ||
        create_info->queueCreateInfoCount == 0U ||
        create_info->queueCreateInfoCount >
            BVB_VULKAN_MAX_DEVICE_QUEUE_CREATE_INFOS ||
        create_info->pQueueCreateInfos == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (create_info->enabledLayerCount != 0U) {
        return VK_ERROR_LAYER_NOT_PRESENT;
    }
    if (create_info->enabledExtensionCount >
            BVB_VULKAN_MAX_DEVICE_CREATE_EXTENSIONS ||
        (create_info->enabledExtensionCount != 0U &&
         create_info->ppEnabledExtensionNames == NULL)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    for (uint32_t index = 0U;
         index < create_info->enabledExtensionCount; ++index) {
        const char *name = create_info->ppEnabledExtensionNames[index];
        if (name == NULL || name[0] == '\0' ||
            memchr(name, '\0', BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE) ==
                NULL) {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
    }
    const char *native_extensions[
        BVB_VULKAN_MAX_DEVICE_CREATE_EXTENSIONS] = {0};
    uint32_t native_extension_count = 0U;
    bool virtual_swapchain_requested = false;
    const bool virtual_wsi_available =
        (bvb_global_client.service_flags &
         BVB_SERVICE_ACTIVITY_INGRESS) != 0U;
    for (uint32_t index = 0U;
         index < create_info->enabledExtensionCount; ++index) {
        const char *name = create_info->ppEnabledExtensionNames[index];
        if (virtual_wsi_available &&
            strcmp(name, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
            virtual_swapchain_requested = true;
        } else {
            bool duplicate = false;
            for (uint32_t prior = 0U;
                 prior < native_extension_count; ++prior) {
                if (strcmp(name, native_extensions[prior]) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                native_extensions[native_extension_count++] = name;
            }
        }
    }
    bool external_memory_fd_injected = false;
    bool external_memory_dma_buf_injected = false;
    bool ahardwarebuffer_injected = false;
    const bool direct_memory_requested =
        bvb_global_client.memory_direct_enabled;
    if (virtual_swapchain_requested || direct_memory_requested) {
        bool native_external_memory_fd_supported = false;
        bool native_external_memory_dma_buf_supported = false;
        bool native_ahardwarebuffer_supported = false;
        if (pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const VkExtensionProperties *native_properties = NULL;
        uint32_t native_property_count = 0U;
        const int extension_result = device_extensions_locked(
            physical, &native_properties, &native_property_count);
        if (extension_result == 0) {
            for (uint32_t index = 0U;
                 index < native_property_count; ++index) {
                const char *extension_name =
                    native_properties[index].extensionName;
                native_external_memory_fd_supported |=
                    strcmp(extension_name,
                           VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) == 0;
                native_external_memory_dma_buf_supported |=
                    strcmp(extension_name,
                           VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) == 0;
                native_ahardwarebuffer_supported |=
                    strcmp(
                        extension_name,
                        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME) ==
                    0;
            }
        }
        (void)pthread_mutex_unlock(&bvb_global_client.mutex);
        if (extension_result != 0) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (!native_external_memory_fd_supported ||
            (virtual_swapchain_requested &&
             (!native_external_memory_dma_buf_supported ||
              !native_ahardwarebuffer_supported))) {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
        bool external_memory_fd_enabled = false;
        bool external_memory_dma_buf_enabled = false;
        bool ahardwarebuffer_enabled = false;
        for (uint32_t index = 0U;
             index < native_extension_count; ++index) {
            if (strcmp(native_extensions[index],
                       VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) == 0) {
                external_memory_fd_enabled = true;
            }
            if (strcmp(native_extensions[index],
                       VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) == 0) {
                external_memory_dma_buf_enabled = true;
            }
            if (strcmp(
                    native_extensions[index],
                    VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME) ==
                0) {
                ahardwarebuffer_enabled = true;
            }
        }
        if (!external_memory_fd_enabled) {
            if (native_extension_count >=
                BVB_VULKAN_MAX_DEVICE_CREATE_EXTENSIONS) {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            native_extensions[native_extension_count++] =
                VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
            external_memory_fd_injected = true;
        }
        if (virtual_swapchain_requested &&
            !external_memory_dma_buf_enabled) {
            if (native_extension_count >=
                BVB_VULKAN_MAX_DEVICE_CREATE_EXTENSIONS) {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            native_extensions[native_extension_count++] =
                VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
            external_memory_dma_buf_injected = true;
        }
        if (virtual_swapchain_requested && !ahardwarebuffer_enabled) {
            if (native_extension_count >=
                BVB_VULKAN_MAX_DEVICE_CREATE_EXTENSIONS) {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            native_extensions[native_extension_count++] =
                VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME;
            ahardwarebuffer_injected = true;
        }
    }
    if (getenv("BVB_ICD_DIAGNOSTICS") != NULL) {
        for (uint32_t index = 0U;
             index < create_info->enabledExtensionCount; ++index) {
            fprintf(stderr,
                    "BVB_ICD_CREATE_DEVICE_EXTENSION index=%u name=%s\n",
                    index, create_info->ppEnabledExtensionNames[index]);
        }
        fprintf(stderr,
                "BVB_ICD_CREATE_DEVICE_NORMALIZED original=%u native=%u "
                "virtual_swapchain=%u external_memory_fd_injected=%u "
                "external_memory_dma_buf_injected=%u "
                "ahardwarebuffer_injected=%u direct_memory=%u\n",
                create_info->enabledExtensionCount, native_extension_count,
                virtual_swapchain_requested ? 1U : 0U,
                external_memory_fd_injected ? 1U : 0U,
                external_memory_dma_buf_injected ? 1U : 0U,
                ahardwarebuffer_injected ? 1U : 0U,
                direct_memory_requested ? 1U : 0U);
    }
    struct bvb_vulkan_device_create_packed_request packed = {
        .physical_device_id = physical->wire_id,
        .flags = create_info->flags,
        .queue_create_info_count = create_info->queueCreateInfoCount,
        .enabled_layer_count = create_info->enabledLayerCount,
        .enabled_extension_count = native_extension_count,
    };
    PFN_vkSetDeviceLoaderData set_loader_data = NULL;
    const VkResult loader_data_result = find_device_loader_data_callback(
        create_info->pNext, &set_loader_data);
    if (loader_data_result != VK_SUCCESS) {
        return loader_data_result;
    }
    if (create_info->pEnabledFeatures != NULL) {
        packed.enabled_feature_structs |=
            BVB_VULKAN_DEVICE_FEATURE_BASE;
        memcpy(packed.enabled_base_features.values,
               create_info->pEnabledFeatures,
               BVB_VULKAN_BASE_FEATURES_SIZE);
    }
    const VkResult feature_result =
        pack_device_feature_chain(create_info->pNext, &packed);
    if (feature_result != VK_SUCCESS) {
        return feature_result;
    }
    for (uint32_t index = 0U;
         index < create_info->queueCreateInfoCount; ++index) {
        const VkDeviceQueueCreateInfo *queue_info =
            &create_info->pQueueCreateInfos[index];
        if (queue_info->sType !=
                VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO ||
            queue_info->pNext != NULL || queue_info->flags != 0U ||
            queue_info->queueCount == 0U ||
            queue_info->pQueuePriorities == NULL ||
            queue_info->queueCount >
                BVB_VULKAN_MAX_DEVICE_QUEUE_PRIORITIES -
                    packed.queue_priority_count) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        for (uint32_t prior = 0U; prior < index; ++prior) {
            if (create_info->pQueueCreateInfos[prior].queueFamilyIndex ==
                queue_info->queueFamilyIndex) {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }
        packed.queue_create_infos[index] =
            (struct bvb_vulkan_device_queue_create_info){
                .flags = queue_info->flags,
                .queue_family_index = queue_info->queueFamilyIndex,
                .queue_count = queue_info->queueCount,
                .first_priority = packed.queue_priority_count,
            };
        for (uint32_t priority = 0U;
             priority < queue_info->queueCount; ++priority) {
            const float value = queue_info->pQueuePriorities[priority];
            if (!(value >= 0.0F && value <= 1.0F)) {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            memcpy(&packed.queue_priority_bits[packed.queue_priority_count],
                   &value, sizeof(value));
            ++packed.queue_priority_count;
        }
        if (getenv("BVB_ICD_DIAGNOSTICS") != NULL) {
            fprintf(stderr,
                    "BVB_ICD_CREATE_DEVICE_QUEUE index=%u family=%u "
                    "count=%u flags=%u\n",
                    index, queue_info->queueFamilyIndex,
                    queue_info->queueCount, (unsigned int)queue_info->flags);
        }
    }
    for (uint32_t index = 0U;
         index < native_extension_count; ++index) {
        const char *name = native_extensions[index];
        const char *terminator = memchr(
            name, '\0', BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE);
        const size_t length = (size_t)(terminator - name) + 1U;
        memcpy(packed.enabled_extensions[index], name, length);
    }
    const bool use_packed = create_info->queueCreateInfoCount != 1U ||
        create_info->pQueueCreateInfos[0].queueCount != 1U ||
        native_extension_count > BVB_VULKAN_MAX_ENABLED_EXTENSIONS ||
        packed.enabled_feature_structs != 0U;
    struct bvb_device_proxy *proxy = calloc(1, sizeof(*proxy));
    if (proxy == NULL) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    const VkDeviceQueueCreateInfo *queue_info = create_info->pQueueCreateInfos;
    const struct bvb_vulkan_device_create_request create_request = {
        .physical_device_id = physical->wire_id,
        .flags = create_info->flags,
        .queue_family_index = queue_info->queueFamilyIndex,
        .queue_count = queue_info->queueCount,
        .queue_priority_bits = packed.queue_priority_bits[0],
        .enabled_layer_count = create_info->enabledLayerCount,
        .enabled_extension_count = native_extension_count,
    };
    if (pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        free(proxy);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    int result = connect_locked();
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = use_packed
                      ? BVB_OPCODE_VULKAN_DEVICE_CREATE_PACKED
                      : (native_extension_count == 0U
                             ? BVB_OPCODE_VULKAN_DEVICE_CREATE
                             : BVB_OPCODE_VULKAN_DEVICE_CREATE_EXTENDED),
        .request_id = next_request_id_locked(),
    };
    if (result == 0) {
        if (use_packed) {
            result = bvb_protocol_encode_vulkan_device_create_packed_request(
                request.payload, &packed, &request.header.payload_length);
        } else if (native_extension_count == 0U) {
            request.header.payload_length =
                BVB_VULKAN_DEVICE_CREATE_REQUEST_SIZE;
            result = bvb_protocol_encode_vulkan_device_create_request(
                request.payload, &create_request);
        } else {
            struct bvb_vulkan_device_create_extended_request extended = {
                .base = create_request,
            };
            for (uint32_t index = 0U;
                 index < native_extension_count; ++index) {
                const char *name = native_extensions[index];
                const char *terminator = memchr(
                    name, '\0', BVB_VULKAN_ENABLED_EXTENSION_NAME_SIZE);
                const size_t length = (size_t)(terminator - name) + 1U;
                memcpy(extended.enabled_extensions[index], name, length);
            }
            result = bvb_protocol_encode_vulkan_device_create_extended_request(
                request.payload, &extended,
                &request.header.payload_length);
        }
    }
    struct bvb_protocol_packet response = {0};
    if (result == 0) {
        result = exchange_locked(&request, &response);
    }
    if (result == 0 &&
        (response.header.status != 0 ||
         response.header.payload_length !=
             BVB_VULKAN_DEVICE_CREATE_RESPONSE_SIZE)) {
        result = -EPROTO;
    }
    struct bvb_vulkan_device_create_response decoded = {0};
    if (result == 0) {
        result = bvb_protocol_decode_vulkan_device_create_response(
            response.payload, &decoded);
    }
    if (result == 0 && decoded.vulkan_result == VK_SUCCESS &&
        bvb_handle_expect(decoded.device_id, BVB_OBJECT_DEVICE) != 0) {
        result = -EPROTO;
    }
    if (result == 0 && decoded.vulkan_result == VK_SUCCESS) {
        proxy->dispatch = initial_dispatch_word();
        proxy->magic = BVB_DEVICE_PROXY_MAGIC;
        proxy->wire_id = decoded.device_id;
        proxy->parent_id = physical->wire_id;
        proxy->instance_id = physical->parent_id;
        proxy->set_loader_data = set_loader_data;
        proxy->virtual_swapchain_enabled = virtual_swapchain_requested;
        proxy->next = bvb_global_client.devices;
        bvb_global_client.devices = proxy;
    }
    if (getenv("BVB_ICD_DIAGNOSTICS") != NULL) {
        fprintf(stderr,
                "BVB_ICD_CREATE_DEVICE_RESPONSE status=%d wire_status=%d "
                "payload=%u vulkan=%d device=%llu\n",
                result, response.header.status,
                response.header.payload_length, decoded.vulkan_result,
                (unsigned long long)decoded.device_id);
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result != 0) {
        free(proxy);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (decoded.vulkan_result != VK_SUCCESS) {
        free(proxy);
        return (VkResult)decoded.vulkan_result;
    }
    *device = (VkDevice)proxy;
    return VK_SUCCESS;
}

static void VKAPI_CALL bvb_bridge_vkDestroyDevice(
    VkDevice device, const VkAllocationCallbacks *allocator) {
    struct bvb_device_proxy *proxy = device_proxy(device);
    if (proxy == NULL || allocator != NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return;
    }
    int result = connect_locked();
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_DEVICE_DESTROY,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_DEVICE_ID_SIZE,
    };
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_device_id(
            request.payload, proxy->wire_id);
    }
    struct bvb_protocol_packet response = {0};
    if (result == 0) {
        result = exchange_locked(&request, &response);
    }
    if (result == 0 &&
        (response.header.status != 0 || response.header.payload_length != 0U)) {
        result = -EPROTO;
    }
    if (result == 0) {
        remove_swapchains_for_device_locked(proxy->wire_id);
        remove_device_proxy_locked(proxy);
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
}

static void VKAPI_CALL bvb_bridge_vkGetDeviceQueue(
    VkDevice device, uint32_t queue_family_index, uint32_t queue_index,
    VkQueue *queue) {
    if (queue == NULL) {
        return;
    }
    *queue = VK_NULL_HANDLE;
    struct bvb_device_proxy *proxy = device_proxy(device);
    if (proxy == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return;
    }
    int result = connect_locked();
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_DEVICE_QUEUE,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_DEVICE_QUEUE_REQUEST_SIZE,
    };
    const struct bvb_vulkan_device_queue_request queue_request = {
        .device_id = proxy->wire_id,
        .queue_family_index = queue_family_index,
        .queue_index = queue_index,
    };
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_device_queue_request(
            request.payload, &queue_request);
    }
    struct bvb_protocol_packet response = {0};
    if (result == 0) {
        result = exchange_locked(&request, &response);
    }
    if (result == 0 &&
        (response.header.status != 0 ||
         response.header.payload_length != BVB_VULKAN_QUEUE_ID_SIZE)) {
        result = -EPROTO;
    }
    uint64_t queue_id = 0U;
    if (result == 0) {
        result = bvb_protocol_decode_vulkan_queue_id(
            response.payload, &queue_id);
    }
    struct bvb_queue_proxy *queue_state = NULL;
    bool initialize_loader_data = false;
    if (result == 0) {
        queue_state = queue_proxy_locked(queue_id, proxy->wire_id);
        if (queue_state == NULL) {
            result = -ENOMEM;
        } else if (proxy->set_loader_data != NULL &&
                   !queue_state->loader_data_initialized) {
            initialize_loader_data = true;
        }
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result == 0 && initialize_loader_data) {
        const VkResult loader_result =
            proxy->set_loader_data(device, queue_state);
        if (loader_result != VK_SUCCESS) {
            return;
        }
        queue_state->loader_data_initialized = true;
    }
    if (result == 0) {
        *queue = (VkQueue)queue_state;
    }
}

static int result_exchange_locked(
    uint16_t opcode, const uint8_t *payload, uint32_t payload_length,
    VkResult *vulkan_result) {
    if (vulkan_result == NULL) return -EINVAL;
    *vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    int result = connect_locked();
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = opcode,
        .request_id = next_request_id_locked(),
        .payload_length = payload_length,
    };
    if (result == 0 && payload_length != 0U) {
        if (payload == NULL || payload_length > sizeof(request.payload)) {
            result = -EINVAL;
        } else {
            memcpy(request.payload, payload, payload_length);
        }
    }
    struct bvb_protocol_packet response = {0};
    if (result == 0) {
        result = exchange_locked(&request, &response);
    }
    if (result == 0 &&
        (response.header.status != 0 ||
         response.header.payload_length != BVB_VULKAN_RESULT_SIZE)) {
        result = -EPROTO;
    }
    int32_t decoded_result = VK_ERROR_INITIALIZATION_FAILED;
    if (result == 0) {
        result = bvb_protocol_decode_vulkan_result(
            response.payload, &decoded_result);
    }
    if (result == 0) *vulkan_result = (VkResult)decoded_result;
    return result;
}

static VkResult result_request_locked(
    uint16_t opcode, const uint8_t *payload, uint32_t payload_length) {
    VkResult vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    return result_exchange_locked(
               opcode, payload, payload_length, &vulkan_result) == 0
               ? vulkan_result
               : VK_ERROR_INITIALIZATION_FAILED;
}

static VkResult VKAPI_CALL bvb_bridge_vkCreateCommandPool(
    VkDevice device, const VkCommandPoolCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkCommandPool *command_pool) {
    if (command_pool != NULL) {
        *command_pool = VK_NULL_HANDLE;
    }
    struct bvb_device_proxy *device_state = device_proxy(device);
    const VkCommandPoolCreateFlags supported_flags =
        VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (device_state == NULL || create_info == NULL || command_pool == NULL ||
        allocator != NULL ||
        create_info->sType != VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO ||
        create_info->pNext != NULL ||
        (create_info->flags & ~supported_flags) != 0U) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    struct bvb_command_pool_proxy *pool_state =
        calloc(1, sizeof(*pool_state));
    if (pool_state == NULL) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    const struct bvb_vulkan_command_pool_create_request create_request = {
        .device_id = device_state->wire_id,
        .flags = create_info->flags,
        .queue_family_index = create_info->queueFamilyIndex,
    };
    if (pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        free(pool_state);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    int result = connect_locked();
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_COMMAND_POOL_CREATE,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_COMMAND_POOL_CREATE_REQUEST_SIZE,
    };
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_command_pool_create_request(
            request.payload, &create_request);
    }
    struct bvb_protocol_packet response = {0};
    if (result == 0) {
        result = exchange_locked(&request, &response);
    }
    if (result == 0 &&
        (response.header.status != 0 ||
         response.header.payload_length !=
             BVB_VULKAN_COMMAND_POOL_CREATE_RESPONSE_SIZE)) {
        result = -EPROTO;
    }
    struct bvb_vulkan_command_pool_create_response decoded = {0};
    if (result == 0) {
        result = bvb_protocol_decode_vulkan_command_pool_create_response(
            response.payload, &decoded);
    }
    if (result == 0 && decoded.vulkan_result == VK_SUCCESS) {
        pool_state->wire_id = decoded.command_pool_id;
        pool_state->parent_id = device_state->wire_id;
        pool_state->next = bvb_global_client.command_pools;
        bvb_global_client.command_pools = pool_state;
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result != 0) {
        free(pool_state);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (decoded.vulkan_result != VK_SUCCESS) {
        free(pool_state);
        return (VkResult)decoded.vulkan_result;
    }
    memcpy(command_pool, &decoded.command_pool_id, sizeof(*command_pool));
    return VK_SUCCESS;
}

static void VKAPI_CALL bvb_bridge_vkDestroyCommandPool(
    VkDevice device, VkCommandPool command_pool,
    const VkAllocationCallbacks *allocator) {
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || allocator != NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return;
    }
    struct bvb_command_pool_proxy *pool_state =
        command_pool_proxy_locked(command_pool);
    int result = pool_state != NULL &&
                         pool_state->parent_id == device_state->wire_id
                     ? connect_locked()
                     : -EINVAL;
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_COMMAND_POOL_DESTROY,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_COMMAND_POOL_ID_SIZE,
    };
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_command_pool_id(
            request.payload, pool_state->wire_id);
    }
    struct bvb_protocol_packet response = {0};
    if (result == 0) {
        result = exchange_locked(&request, &response);
    }
    if (result == 0 &&
        (response.header.status != 0 || response.header.payload_length != 0U)) {
        result = -EPROTO;
    }
    if (result == 0) {
        remove_command_pool_proxy_locked(pool_state);
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
}

static VkResult VKAPI_CALL bvb_bridge_vkResetCommandPool(
    VkDevice device, VkCommandPool command_pool,
    VkCommandPoolResetFlags flags) {
    struct bvb_device_proxy *device_state = device_proxy(device);
    if ((flags & ~VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT) != 0U ||
        device_state == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    struct bvb_command_pool_proxy *pool_state =
        command_pool_proxy_locked(command_pool);
    VkResult vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    if (pool_state != NULL && pool_state->parent_id == device_state->wire_id) {
        const struct bvb_vulkan_command_pool_reset_request reset_request = {
            .command_pool_id = pool_state->wire_id,
            .flags = flags,
        };
        uint8_t payload[BVB_VULKAN_COMMAND_POOL_RESET_REQUEST_SIZE];
        int result = bvb_protocol_encode_vulkan_command_pool_reset_request(
            payload, &reset_request);
        if (result == 0) {
            vulkan_result = result_request_locked(
                BVB_OPCODE_VULKAN_COMMAND_POOL_RESET, payload,
                sizeof(payload));
            if (vulkan_result == VK_SUCCESS) {
                reset_command_streams_for_pool_locked(pool_state->wire_id);
            }
        }
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return vulkan_result;
}

static VkResult VKAPI_CALL bvb_bridge_vkAllocateCommandBuffers(
    VkDevice device, const VkCommandBufferAllocateInfo *allocate_info,
    VkCommandBuffer *command_buffers) {
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || allocate_info == NULL ||
        command_buffers == NULL ||
        allocate_info->sType !=
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO ||
        allocate_info->pNext != NULL ||
        allocate_info->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY ||
        allocate_info->commandBufferCount != 1U) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    command_buffers[0] = VK_NULL_HANDLE;
    struct bvb_command_buffer_proxy *command_state =
        calloc(1, sizeof(*command_state));
    if (command_state == NULL) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (pthread_mutex_init(&command_state->stream_mutex, NULL) != 0) {
        free(command_state);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    command_state->stream_slot = UINT32_MAX;
    if (pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        (void)pthread_mutex_destroy(&command_state->stream_mutex);
        free(command_state);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    struct bvb_command_pool_proxy *pool_state =
        command_pool_proxy_locked(allocate_info->commandPool);
    int result = pool_state != NULL &&
                         pool_state->parent_id == device_state->wire_id
                     ? connect_locked()
                     : -EINVAL;
    const struct bvb_vulkan_command_buffer_allocate_request allocate_request = {
        .command_pool_id = pool_state == NULL ? 0U : pool_state->wire_id,
        .level = allocate_info->level,
        .count = allocate_info->commandBufferCount,
    };
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_COMMAND_BUFFER_ALLOCATE,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_COMMAND_BUFFER_ALLOCATE_REQUEST_SIZE,
    };
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_command_buffer_allocate_request(
            request.payload, &allocate_request);
    }
    struct bvb_protocol_packet response = {0};
    if (result == 0) {
        result = exchange_locked(&request, &response);
    }
    if (result == 0 &&
        (response.header.status != 0 ||
         response.header.payload_length !=
             BVB_VULKAN_COMMAND_BUFFER_ALLOCATE_RESPONSE_SIZE)) {
        result = -EPROTO;
    }
    struct bvb_vulkan_command_buffer_allocate_response decoded = {0};
    if (result == 0) {
        result = bvb_protocol_decode_vulkan_command_buffer_allocate_response(
            response.payload, &decoded);
    }
    if (result == 0 && decoded.vulkan_result == VK_SUCCESS) {
        command_state->dispatch = initial_dispatch_word();
        command_state->magic = BVB_COMMAND_BUFFER_PROXY_MAGIC;
        command_state->wire_id = decoded.command_buffer_id;
        command_state->parent_pool_id = pool_state->wire_id;
        command_state->device_id = device_state->wire_id;
        command_state->next = bvb_global_client.command_buffers;
        bvb_global_client.command_buffers = command_state;
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result != 0) {
        (void)pthread_mutex_destroy(&command_state->stream_mutex);
        free(command_state);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (decoded.vulkan_result != VK_SUCCESS) {
        (void)pthread_mutex_destroy(&command_state->stream_mutex);
        free(command_state);
        return (VkResult)decoded.vulkan_result;
    }
    if (device_state->set_loader_data != NULL) {
        const VkResult loader_result =
            device_state->set_loader_data(device, command_state);
        if (loader_result != VK_SUCCESS) {
            return loader_result;
        }
    }
    *command_buffers = (VkCommandBuffer)command_state;
    return VK_SUCCESS;
}

static void VKAPI_CALL bvb_bridge_vkFreeCommandBuffers(
    VkDevice device, VkCommandPool command_pool,
    uint32_t command_buffer_count, const VkCommandBuffer *command_buffers) {
    if (command_buffer_count == 0U) {
        return;
    }
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || command_buffer_count != 1U ||
        command_buffers == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return;
    }
    struct bvb_command_pool_proxy *pool_state =
        command_pool_proxy_locked(command_pool);
    struct bvb_command_buffer_proxy *command_state =
        command_buffer_proxy(command_buffers[0]);
    int result = pool_state != NULL && command_state != NULL &&
                         pool_state->parent_id == device_state->wire_id &&
                         command_state->parent_pool_id == pool_state->wire_id
                     ? connect_locked()
                     : -EINVAL;
    const struct bvb_vulkan_command_buffer_free_request free_request = {
        .command_pool_id = pool_state == NULL ? 0U : pool_state->wire_id,
        .command_buffer_id =
            command_state == NULL ? 0U : command_state->wire_id,
    };
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_COMMAND_BUFFER_FREE,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_COMMAND_BUFFER_FREE_REQUEST_SIZE,
    };
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_command_buffer_free_request(
            request.payload, &free_request);
    }
    struct bvb_protocol_packet response = {0};
    if (result == 0) {
        result = exchange_locked(&request, &response);
    }
    if (result == 0 &&
        (response.header.status != 0 || response.header.payload_length != 0U)) {
        result = -EPROTO;
    }
    if (result == 0) {
        remove_command_buffer_proxy_locked(command_state);
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
}

static VkResult VKAPI_CALL bvb_bridge_vkBeginCommandBuffer(
    VkCommandBuffer command_buffer,
    const VkCommandBufferBeginInfo *begin_info) {
    struct bvb_command_buffer_proxy *command_state =
        command_buffer_proxy(command_buffer);
    if (command_state == NULL || begin_info == NULL ||
        begin_info->sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO ||
        begin_info->pNext != NULL || begin_info->pInheritanceInfo != NULL ||
        (begin_info->flags & ~VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) !=
            0U) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (command_stream_is_enabled()) {
        if (pthread_mutex_lock(&command_state->stream_mutex) != 0)
            return VK_ERROR_INITIALIZATION_FAILED;
        command_state->diagnostic_rejection_entry = NULL;
        command_state->diagnostic_rejection_reason = NULL;
        command_state->diagnostic_rejection_shape = NULL;
        command_state->diagnostic_rejection_status = 0;
        int result = 0;
        if (command_state->stream_recording ||
            bvb_global_client.command_stream_mapping == NULL) {
            result = -EINVAL;
        }
        if (result == 0) {
            reset_command_stream_state(command_state);
            result = lease_command_stream_slot(command_state);
        }
        if (result == 0) {
            uint8_t *slot = bvb_global_client.command_stream_mapping +
                (size_t)command_state->stream_slot *
                    BVB_COMMAND_STREAM_SLOT_BYTES;
            result = bvb_command_batch_begin(
                &command_state->stream_builder, slot,
                BVB_COMMAND_STREAM_SLOT_BYTES, command_state->wire_id,
                command_state->stream_sequence);
        }
        if (result == 0) {
            result = bvb_command_batch_append_vulkan_begin(
                &command_state->stream_builder,
                &(const struct bvb_vulkan_begin_command){
                    .flags = begin_info->flags,
                });
        }
        command_state->stream_recording = result == 0;
        command_state->stream_error = result != 0;
        if (result != 0) release_command_stream_slot(command_state);
        (void)pthread_mutex_unlock(&command_state->stream_mutex);
        return result == 0 ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
    }
    if (pthread_mutex_lock(&bvb_global_client.mutex) != 0)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    command_state->diagnostic_rejection_entry = NULL;
    command_state->diagnostic_rejection_reason = NULL;
    command_state->diagnostic_rejection_shape = NULL;
    command_state->diagnostic_rejection_status = 0;
    const struct bvb_vulkan_command_buffer_begin_request begin_request = {
        .command_buffer_id = command_state->wire_id,
        .flags = begin_info->flags,
    };
    uint8_t payload[BVB_VULKAN_COMMAND_BUFFER_BEGIN_REQUEST_SIZE];
    int result = bvb_protocol_encode_vulkan_command_buffer_begin_request(
        payload, &begin_request);
    VkResult vulkan_result = result == 0
                                 ? result_request_locked(
                                       BVB_OPCODE_VULKAN_COMMAND_BUFFER_BEGIN,
                                       payload, sizeof(payload))
                                 : VK_ERROR_INITIALIZATION_FAILED;
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return vulkan_result;
}

static VkResult VKAPI_CALL bvb_bridge_vkEndCommandBuffer(
    VkCommandBuffer command_buffer) {
    struct bvb_command_buffer_proxy *command_state =
        command_buffer_proxy(command_buffer);
    if (command_state == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (command_stream_is_enabled()) {
        if (pthread_mutex_lock(&command_state->stream_mutex) != 0)
            return VK_ERROR_INITIALIZATION_FAILED;
        int result = command_state->stream_recording &&
                             !command_state->stream_error
                         ? bvb_command_batch_append_vulkan_end(
                               &command_state->stream_builder)
                         : -EINVAL;
        size_t stream_length = 0U;
        if (result == 0) {
            result = bvb_command_batch_finish(
                &command_state->stream_builder, &stream_length);
        }
        if (result == 0 && stream_length <= UINT32_MAX) {
            atomic_thread_fence(memory_order_release);
            command_state->stream_length = (uint32_t)stream_length;
            command_state->stream_sealed = true;
        } else {
            command_state->stream_length = 0U;
            command_state->stream_sealed = false;
            command_state->stream_error = true;
            release_command_stream_slot(command_state);
        }
        command_state->stream_recording = false;
        const char *diagnostic_entry =
            command_state->diagnostic_rejection_entry;
        const char *diagnostic_reason =
            command_state->diagnostic_rejection_reason;
        const char *diagnostic_shape =
            command_state->diagnostic_rejection_shape;
        const int diagnostic_status =
            command_state->diagnostic_rejection_status;
        const uint64_t command_buffer_id = command_state->wire_id;
        const uint64_t command_sequence = command_state->stream_sequence;
        (void)pthread_mutex_unlock(&command_state->stream_mutex);
        if (result != 0 && diagnostic_entry != NULL)
            bvb_first_rejection_record_command_poison(
                diagnostic_entry, diagnostic_reason, diagnostic_shape,
                diagnostic_status, command_buffer_id, command_sequence);
        return result == 0 ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
    }
    if (pthread_mutex_lock(&bvb_global_client.mutex) != 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (command_state->diagnostic_rejection_entry != NULL) {
        const char *diagnostic_entry =
            command_state->diagnostic_rejection_entry;
        const char *diagnostic_reason =
            command_state->diagnostic_rejection_reason;
        const char *diagnostic_shape =
            command_state->diagnostic_rejection_shape;
        const int diagnostic_status =
            command_state->diagnostic_rejection_status;
        const uint64_t command_buffer_id = command_state->wire_id;
        (void)pthread_mutex_unlock(&bvb_global_client.mutex);
        bvb_first_rejection_record_command_poison(
            diagnostic_entry, diagnostic_reason, diagnostic_shape,
            diagnostic_status, command_buffer_id, 0U);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    uint8_t payload[BVB_VULKAN_COMMAND_BUFFER_ID_SIZE];
    int result = bvb_protocol_encode_vulkan_command_buffer_id(
        payload, command_state->wire_id);
    VkResult vulkan_result = result == 0
                                 ? result_request_locked(
                                       BVB_OPCODE_VULKAN_COMMAND_BUFFER_END,
                                       payload, sizeof(payload))
                                 : VK_ERROR_INITIALIZATION_FAILED;
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return vulkan_result;
}

static void destroy_resource(
    VkDevice device, uint64_t wire_id, enum bvb_object_type type,
    uint16_t opcode, const VkAllocationCallbacks *allocator);

static VkResult create_resource_locked(
    uint16_t opcode, const uint8_t *payload, uint32_t payload_length,
    enum bvb_object_type type, uint64_t parent_id,
    struct bvb_resource_proxy *state, uint64_t *wire_id) {
    int result = connect_locked();
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = opcode,
        .request_id = next_request_id_locked(),
        .payload_length = payload_length,
    };
    if (result == 0) memcpy(request.payload, payload, payload_length);
    struct bvb_protocol_packet response = {0};
    if (result == 0) result = exchange_locked(&request, &response);
    if (result == 0 &&
        (response.header.status != 0 ||
         response.header.payload_length != BVB_VULKAN_OBJECT_CREATE_RESPONSE_SIZE))
        result = -EPROTO;
    struct bvb_vulkan_object_create_response decoded = {0};
    if (result == 0)
        result = bvb_protocol_decode_vulkan_object_create_response(
            response.payload, &decoded, (uint8_t)type);
    if (result != 0) return VK_ERROR_INITIALIZATION_FAILED;
    if (decoded.vulkan_result != VK_SUCCESS)
        return (VkResult)decoded.vulkan_result;
    state->wire_id = decoded.object_id;
    state->parent_id = parent_id;
    state->type = type;
    if (pthread_rwlock_wrlock(&bvb_object_registry_lock) != 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    state->next = bvb_global_client.resources;
    bvb_global_client.resources = state;
    (void)pthread_rwlock_unlock(&bvb_object_registry_lock);
    *wire_id = decoded.object_id;
    return VK_SUCCESS;
}

static VkResult create_resource_fd_locked(
    uint16_t opcode, const uint8_t *payload, uint32_t payload_length,
    int passed_fd, enum bvb_object_type type, uint64_t parent_id,
    struct bvb_resource_proxy *state, uint64_t *wire_id) {
    int result = passed_fd >= 0 ? connect_locked() : -EINVAL;
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = opcode,
        .request_id = next_request_id_locked(),
        .payload_length = payload_length,
    };
    if (result == 0 && (payload == NULL ||
                        payload_length > sizeof(request.payload))) {
        result = -EINVAL;
    }
    if (result == 0) memcpy(request.payload, payload, payload_length);
    struct bvb_protocol_packet response = {0};
    if (result == 0)
        result = exchange_pass_fd_locked(&request, &response, passed_fd);
    if (result == 0 &&
        (response.header.status != 0 ||
         response.header.payload_length !=
             BVB_VULKAN_OBJECT_CREATE_RESPONSE_SIZE)) {
        result = -EPROTO;
    }
    struct bvb_vulkan_object_create_response decoded = {0};
    if (result == 0)
        result = bvb_protocol_decode_vulkan_object_create_response(
            response.payload, &decoded, (uint8_t)type);
    if (result != 0) return VK_ERROR_INITIALIZATION_FAILED;
    if (decoded.vulkan_result != VK_SUCCESS)
        return (VkResult)decoded.vulkan_result;
    state->wire_id = decoded.object_id;
    state->parent_id = parent_id;
    state->type = type;
    if (pthread_rwlock_wrlock(&bvb_object_registry_lock) != 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    state->next = bvb_global_client.resources;
    bvb_global_client.resources = state;
    (void)pthread_rwlock_unlock(&bvb_object_registry_lock);
    *wire_id = decoded.object_id;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL bvb_bridge_vkCreateDescriptorSetLayout(
    VkDevice device, const VkDescriptorSetLayoutCreateInfo *create_info,
    const VkAllocationCallbacks *allocator,
    VkDescriptorSetLayout *set_layout) {
    if (set_layout != NULL) *set_layout = VK_NULL_HANDLE;
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || create_info == NULL || set_layout == NULL ||
        allocator != NULL ||
        create_info->sType !=
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO ||
        create_info->bindingCount >
            BVB_VULKAN_MAX_DESCRIPTOR_LAYOUT_BINDINGS ||
        (create_info->bindingCount != 0U && create_info->pBindings == NULL)) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    const VkDescriptorSetLayoutBindingFlagsCreateInfo *flags_info = NULL;
    if (create_info->pNext != NULL) {
        const VkBaseInStructure *next = create_info->pNext;
        if (next->sType !=
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO ||
            next->pNext != NULL) {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        flags_info =
            (const VkDescriptorSetLayoutBindingFlagsCreateInfo *)next;
        if (flags_info->bindingCount != create_info->bindingCount ||
            (flags_info->bindingCount != 0U &&
             flags_info->pBindingFlags == NULL)) {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    if (getenv("BVB_ICD_DIAGNOSTICS") != NULL) {
        fprintf(stderr,
                "BVB_ICD_DESCRIPTOR_LAYOUT flags=%u bindings=%u "
                "binding_flags=%u\n",
                (unsigned int)create_info->flags,
                (unsigned int)create_info->bindingCount,
                flags_info != NULL ? 1U : 0U);
        for (uint32_t index = 0U; index < create_info->bindingCount; ++index) {
            const VkDescriptorSetLayoutBinding *binding =
                &create_info->pBindings[index];
            fprintf(stderr,
                    "BVB_ICD_DESCRIPTOR_BINDING index=%u binding=%u type=%u "
                    "count=%u stages=%u flags=%u immutable=%u\n",
                    (unsigned int)index, (unsigned int)binding->binding,
                    (unsigned int)binding->descriptorType,
                    (unsigned int)binding->descriptorCount,
                    (unsigned int)binding->stageFlags,
                    flags_info == NULL
                        ? 0U
                        : (unsigned int)flags_info->pBindingFlags[index],
                    binding->pImmutableSamplers != NULL ? 1U : 0U);
        }
    }
    struct bvb_vulkan_descriptor_set_layout_create_request decoded = {
        .device_id = device_state->wire_id,
        .flags = create_info->flags,
        .binding_count = create_info->bindingCount,
        .has_binding_flags = flags_info != NULL ? 1U : 0U,
    };
    for (uint32_t index = 0U; index < create_info->bindingCount; ++index) {
        const VkDescriptorSetLayoutBinding *binding =
            &create_info->pBindings[index];
        if (binding->pImmutableSamplers != NULL) {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        decoded.bindings[index] =
            (struct bvb_vulkan_descriptor_layout_binding){
                .binding = binding->binding,
                .descriptor_type = binding->descriptorType,
                .descriptor_count = binding->descriptorCount,
                .stage_flags = binding->stageFlags,
                .binding_flags = flags_info == NULL
                    ? 0U : flags_info->pBindingFlags[index],
            };
    }
    uint8_t payload[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t payload_length = 0U;
    int result =
        bvb_protocol_encode_vulkan_descriptor_set_layout_create_request(
            payload, &decoded, &payload_length);
    struct bvb_resource_proxy *state = calloc(1, sizeof(*state));
    if (state == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (result != 0 || pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        free(state);
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    uint64_t wire_id = 0U;
    const VkResult vulkan_result = create_resource_locked(
        BVB_OPCODE_VULKAN_DESCRIPTOR_SET_LAYOUT_CREATE,
        payload, payload_length, BVB_OBJECT_DESCRIPTOR_SET_LAYOUT,
        device_state->wire_id, state, &wire_id);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (getenv("BVB_ICD_DIAGNOSTICS") != NULL) {
        fprintf(stderr, "BVB_ICD_DESCRIPTOR_LAYOUT_RESULT result=%d\n",
                (int)vulkan_result);
    }
    if (vulkan_result != VK_SUCCESS) {
        free(state);
        return vulkan_result;
    }
    memcpy(set_layout, &wire_id, sizeof(*set_layout));
    return VK_SUCCESS;
}

static void VKAPI_CALL bvb_bridge_vkDestroyDescriptorSetLayout(
    VkDevice device, VkDescriptorSetLayout set_layout,
    const VkAllocationCallbacks *allocator) {
    destroy_resource(
        device, non_dispatchable_wire_id(&set_layout, sizeof(set_layout)),
        BVB_OBJECT_DESCRIPTOR_SET_LAYOUT,
        BVB_OPCODE_VULKAN_DESCRIPTOR_OBJECT_DESTROY, allocator);
}

static VkResult VKAPI_CALL bvb_bridge_vkCreateDescriptorPool(
    VkDevice device, const VkDescriptorPoolCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkDescriptorPool *pool) {
    if (pool != NULL) *pool = VK_NULL_HANDLE;
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || create_info == NULL || pool == NULL ||
        allocator != NULL ||
        create_info->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO ||
        create_info->pNext != NULL || create_info->maxSets == 0U ||
        create_info->poolSizeCount == 0U ||
        create_info->poolSizeCount > BVB_VULKAN_MAX_DESCRIPTOR_POOL_SIZES ||
        create_info->pPoolSizes == NULL) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    struct bvb_vulkan_descriptor_pool_create_request decoded = {
        .device_id = device_state->wire_id,
        .flags = create_info->flags,
        .max_sets = create_info->maxSets,
        .pool_size_count = create_info->poolSizeCount,
    };
    for (uint32_t index = 0U; index < create_info->poolSizeCount; ++index) {
        decoded.pool_sizes[index] = (struct bvb_vulkan_descriptor_pool_size){
            .descriptor_type = create_info->pPoolSizes[index].type,
            .descriptor_count =
                create_info->pPoolSizes[index].descriptorCount,
        };
    }
    uint8_t payload[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t payload_length = 0U;
    int result = bvb_protocol_encode_vulkan_descriptor_pool_create_request(
        payload, &decoded, &payload_length);
    struct bvb_resource_proxy *state = calloc(1, sizeof(*state));
    if (state == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (result != 0 || pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        free(state);
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    uint64_t wire_id = 0U;
    const VkResult vulkan_result = create_resource_locked(
        BVB_OPCODE_VULKAN_DESCRIPTOR_POOL_CREATE, payload, payload_length,
        BVB_OBJECT_DESCRIPTOR_POOL, device_state->wire_id, state, &wire_id);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (vulkan_result != VK_SUCCESS) {
        free(state);
        return vulkan_result;
    }
    memcpy(pool, &wire_id, sizeof(*pool));
    return VK_SUCCESS;
}

static void VKAPI_CALL bvb_bridge_vkDestroyDescriptorPool(
    VkDevice device, VkDescriptorPool pool,
    const VkAllocationCallbacks *allocator) {
    destroy_resource(
        device, non_dispatchable_wire_id(&pool, sizeof(pool)),
        BVB_OBJECT_DESCRIPTOR_POOL,
        BVB_OPCODE_VULKAN_DESCRIPTOR_OBJECT_DESTROY, allocator);
}

static VkResult VKAPI_CALL bvb_bridge_vkResetDescriptorPool(
    VkDevice device, VkDescriptorPool descriptor_pool,
    VkDescriptorPoolResetFlags flags) {
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || flags != 0U ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const uint64_t pool_id = non_dispatchable_wire_id(
        &descriptor_pool, sizeof(descriptor_pool));
    struct bvb_resource_proxy *pool_state =
        resource_proxy_locked(pool_id, BVB_OBJECT_DESCRIPTOR_POOL);
    VkResult vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    if (pool_state != NULL && pool_state->parent_id == device_state->wire_id) {
        const struct bvb_vulkan_descriptor_pool_reset_request reset_request = {
            .descriptor_pool_id = pool_id,
            .flags = flags,
        };
        uint8_t payload[BVB_VULKAN_DESCRIPTOR_POOL_RESET_REQUEST_SIZE];
        int result = bvb_protocol_encode_vulkan_descriptor_pool_reset_request(
            payload, &reset_request);
        if (result == 0) {
            vulkan_result = result_request_locked(
                BVB_OPCODE_VULKAN_DESCRIPTOR_POOL_RESET, payload,
                sizeof(payload));
            if (vulkan_result == VK_SUCCESS) {
                if (pthread_rwlock_wrlock(&bvb_object_registry_lock) != 0) {
                    vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
                } else {
                    remove_descriptor_sets_for_pool_locked(pool_id);
                    (void)pthread_rwlock_unlock(&bvb_object_registry_lock);
                }
            }
        }
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return vulkan_result;
}

static VkResult VKAPI_CALL bvb_bridge_vkCreateQueryPool(
    VkDevice device, const VkQueryPoolCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkQueryPool *query_pool) {
    if (query_pool != NULL) *query_pool = VK_NULL_HANDLE;
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || create_info == NULL || query_pool == NULL ||
        allocator != NULL ||
        create_info->sType != VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO ||
        create_info->pNext != NULL)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const struct bvb_vulkan_query_pool_create_request decoded = {
        .device_id = device_state->wire_id,
        .flags = create_info->flags,
        .query_type = create_info->queryType,
        .query_count = create_info->queryCount,
        .pipeline_statistics = create_info->pipelineStatistics,
    };
    uint8_t payload[BVB_VULKAN_QUERY_POOL_CREATE_REQUEST_SIZE];
    int result = bvb_protocol_encode_vulkan_query_pool_create_request(
        payload, &decoded);
    struct bvb_resource_proxy *state = calloc(1, sizeof(*state));
    if (state == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
    state->allocation_size = create_info->queryCount;
    state->subtype = create_info->queryType;
    if (result != 0 || pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        free(state);
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    uint64_t wire_id = 0U;
    const VkResult vulkan_result = create_resource_locked(
        BVB_OPCODE_VULKAN_QUERY_POOL_CREATE, payload, sizeof(payload),
        BVB_OBJECT_QUERY_POOL, device_state->wire_id, state, &wire_id);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (vulkan_result != VK_SUCCESS) {
        free(state);
        return vulkan_result;
    }
    memcpy(query_pool, &wire_id, sizeof(*query_pool));
    return VK_SUCCESS;
}

static void VKAPI_CALL bvb_bridge_vkDestroyQueryPool(
    VkDevice device, VkQueryPool query_pool,
    const VkAllocationCallbacks *allocator) {
    destroy_resource(
        device, non_dispatchable_wire_id(&query_pool, sizeof(query_pool)),
        BVB_OBJECT_QUERY_POOL, BVB_OPCODE_VULKAN_QUERY_POOL_DESTROY,
        allocator);
}

static VkResult VKAPI_CALL bvb_bridge_vkGetQueryPoolResults(
    VkDevice device, VkQueryPool query_pool, uint32_t first_query,
    uint32_t query_count, size_t data_size, void *data,
    VkDeviceSize stride, VkQueryResultFlags flags) {
    struct bvb_device_proxy *device_state = device_proxy(device);
    const uint64_t pool_id = non_dispatchable_wire_id(
        &query_pool, sizeof(query_pool));
    if (device_state == NULL || data == NULL || data_size == 0U ||
        data_size > BVB_VULKAN_QUERY_POOL_RESULTS_MAX_BYTES ||
        stride > SIZE_MAX ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    struct bvb_resource_proxy *state =
        resource_proxy_locked(pool_id, BVB_OBJECT_QUERY_POOL);
    int result = state != NULL && state->parent_id == device_state->wire_id &&
                         first_query <= state->allocation_size &&
                         query_count <= state->allocation_size - first_query
                     ? connect_locked() : -EINVAL;
    const struct bvb_vulkan_query_pool_results_request decoded = {
        .query_pool_id = pool_id,
        .data_size = data_size,
        .stride = stride,
        .first_query = first_query,
        .query_count = query_count,
        .flags = flags,
    };
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_QUERY_POOL_RESULTS,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_QUERY_POOL_RESULTS_REQUEST_SIZE,
    };
    if (result == 0)
        result = bvb_protocol_encode_vulkan_query_pool_results_request(
            request.payload, &decoded);
    struct bvb_protocol_packet response = {0};
    if (result == 0) result = exchange_locked(&request, &response);
    if (result == 0 && response.header.status != 0)
        result = response.header.status;
    struct bvb_vulkan_query_pool_results_response results = {
        .vulkan_result = VK_ERROR_INITIALIZATION_FAILED,
    };
    if (result == 0)
        result = bvb_protocol_decode_vulkan_query_pool_results_response(
            response.payload, response.header.payload_length, &results);
    if (result == 0 && results.data_size != data_size) result = -EPROTO;
    if (result == 0) memcpy(data, results.data, data_size);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return result == 0 ? (VkResult)results.vulkan_result
                       : VK_ERROR_INITIALIZATION_FAILED;
}

static void VKAPI_CALL bvb_bridge_vkResetQueryPool(
    VkDevice device, VkQueryPool query_pool, uint32_t first_query,
    uint32_t query_count) {
    struct bvb_device_proxy *device_state = device_proxy(device);
    const uint64_t pool_id = non_dispatchable_wire_id(
        &query_pool, sizeof(query_pool));
    if (device_state == NULL || query_count == 0U ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0)
        return;
    struct bvb_resource_proxy *state =
        resource_proxy_locked(pool_id, BVB_OBJECT_QUERY_POOL);
    int result = state != NULL && state->parent_id == device_state->wire_id &&
                         first_query <= state->allocation_size &&
                         query_count <= state->allocation_size - first_query
                     ? connect_locked() : -EINVAL;
    const struct bvb_vulkan_query_pool_reset_request decoded = {
        .query_pool_id = pool_id,
        .first_query = first_query,
        .query_count = query_count,
    };
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_QUERY_POOL_RESET,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_QUERY_POOL_RESET_REQUEST_SIZE,
    };
    if (result == 0)
        result = bvb_protocol_encode_vulkan_query_pool_reset_request(
            request.payload, &decoded);
    struct bvb_protocol_packet response = {0};
    if (result == 0) result = exchange_locked(&request, &response);
    if (result == 0 &&
        (response.header.status != 0 || response.header.payload_length != 0U))
        result = response.header.status != 0
            ? response.header.status : -EPROTO;
    (void)result;
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
}

static VkResult VKAPI_CALL bvb_bridge_vkAllocateDescriptorSets(
    VkDevice device, const VkDescriptorSetAllocateInfo *allocate_info,
    VkDescriptorSet *descriptor_sets) {
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || allocate_info == NULL ||
        descriptor_sets == NULL ||
        allocate_info->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO ||
        allocate_info->pNext != NULL || allocate_info->descriptorSetCount == 0U ||
        allocate_info->descriptorSetCount >
            BVB_VULKAN_MAX_DESCRIPTOR_SETS_PER_ALLOCATE ||
        allocate_info->pSetLayouts == NULL) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    for (uint32_t index = 0U; index < allocate_info->descriptorSetCount;
         ++index) {
        descriptor_sets[index] = VK_NULL_HANDLE;
    }
    struct bvb_resource_proxy *states[
        BVB_VULKAN_MAX_DESCRIPTOR_SETS_PER_ALLOCATE] = {0};
    for (uint32_t index = 0U; index < allocate_info->descriptorSetCount;
         ++index) {
        states[index] = calloc(1, sizeof(*states[index]));
        if (states[index] == NULL) {
            for (uint32_t prior = 0U; prior < index; ++prior) {
                free(states[prior]);
            }
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }
    const uint64_t pool_id = non_dispatchable_wire_id(
        &allocate_info->descriptorPool, sizeof(allocate_info->descriptorPool));
    struct bvb_vulkan_descriptor_set_allocate_request decoded = {
        .descriptor_pool_id = pool_id,
        .descriptor_set_count = allocate_info->descriptorSetCount,
    };
    for (uint32_t index = 0U; index < allocate_info->descriptorSetCount;
         ++index) {
        decoded.set_layout_ids[index] = non_dispatchable_wire_id(
            &allocate_info->pSetLayouts[index],
            sizeof(allocate_info->pSetLayouts[index]));
    }
    uint8_t payload[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t payload_length = 0U;
    int result = bvb_protocol_encode_vulkan_descriptor_set_allocate_request(
        payload, &decoded, &payload_length);
    if (result != 0 || pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        for (uint32_t index = 0U; index < allocate_info->descriptorSetCount;
             ++index) free(states[index]);
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    struct bvb_resource_proxy *pool_state =
        resource_proxy_locked(pool_id, BVB_OBJECT_DESCRIPTOR_POOL);
    if (pool_state == NULL || pool_state->parent_id != device_state->wire_id) {
        result = -EINVAL;
    }
    for (uint32_t index = 0U;
         result == 0 && index < allocate_info->descriptorSetCount; ++index) {
        struct bvb_resource_proxy *layout_state = resource_proxy_locked(
            decoded.set_layout_ids[index], BVB_OBJECT_DESCRIPTOR_SET_LAYOUT);
        if (layout_state == NULL ||
            layout_state->parent_id != device_state->wire_id) {
            result = -EINVAL;
        }
    }
    if (result == 0) result = connect_locked();
    const bool descriptor_transaction =
        result == 0 && bvb_global_client.descriptor_journal_enabled;
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = descriptor_transaction
            ? BVB_OPCODE_VULKAN_DESCRIPTOR_TRANSACTION_ALLOCATE
            : BVB_OPCODE_VULKAN_DESCRIPTOR_SET_ALLOCATE,
        .request_id = next_request_id_locked(),
        .payload_length = payload_length,
    };
    if (result == 0 && descriptor_transaction) {
        const struct bvb_vulkan_descriptor_transaction_allocate_request
            transaction = {
                .journal_generation =
                    bvb_global_client.descriptor_journal_generation,
                .journal_sequence =
                    bvb_global_client.next_descriptor_journal_sequence,
                .journal_length =
                    bvb_global_client.descriptor_journal_length,
                .journal_record_count =
                    bvb_global_client.descriptor_journal_record_count,
                .allocation = decoded,
            };
        result =
            bvb_protocol_encode_vulkan_descriptor_transaction_allocate_request(
                request.payload, &transaction,
                &request.header.payload_length);
    } else if (result == 0) {
        memcpy(request.payload, payload, payload_length);
    }
    struct bvb_protocol_packet response = {0};
    struct bvb_vulkan_descriptor_set_allocate_response allocated = {0};
    uint8_t ring_response[BVB_DESCRIPTOR_TRANSACTION_RING_RESPONSE_BYTES];
    uint32_t ring_response_length = 0U;
    bool descriptor_lease_claimed = false;
    if (result == 0 && descriptor_transaction) {
        result = setup_descriptor_transaction_ring_locked();
    }
    if (result == 0 && descriptor_transaction &&
        bvb_global_client.descriptor_journal_length == 0U &&
        bvb_global_client.descriptor_journal_record_count == 0U) {
        uint64_t lease_epoch = 0U;
        const int lease_result = bvb_descriptor_lease_claim(
            bvb_global_client.descriptor_transaction_ring,
            BVB_DESCRIPTOR_TRANSACTION_RING_REGION_BYTES, pool_id,
            decoded.set_layout_ids, decoded.descriptor_set_count,
            allocated.descriptor_set_ids, &lease_epoch);
        if (lease_result == 0) {
            allocated.vulkan_result = VK_SUCCESS;
            allocated.descriptor_set_count = decoded.descriptor_set_count;
            descriptor_lease_claimed = true;
            ++bvb_global_client.descriptor_lease_hit_count;
            if (bvb_global_client.frame_profile_enabled &&
                bvb_global_client.frame_profile_rpc_window_started) {
                ++bvb_global_client.frame_profile_descriptor_lease_hits;
                bvb_global_client.frame_profile_descriptor_lease_sets +=
                    decoded.descriptor_set_count;
            }
        } else if (lease_result == -ENOENT) {
            if (bvb_global_client.frame_profile_enabled &&
                bvb_global_client.frame_profile_rpc_window_started) {
                ++bvb_global_client.frame_profile_descriptor_lease_misses;
            }
        } else {
            result = lease_result;
        }
    }
    if (result == 0 && descriptor_transaction && !descriptor_lease_claimed) {
        ++bvb_global_client.descriptor_ring_call_count;
        const uint64_t started_ns = bvb_global_client.frame_profile_enabled
            ? frame_profile_monotonic_ns() : 0U;
        result = bvb_descriptor_transaction_ring_call(
            bvb_global_client.descriptor_transaction_ring,
            BVB_DESCRIPTOR_TRANSACTION_RING_REGION_BYTES,
            bvb_global_client.descriptor_transaction_ring_generation,
            bvb_global_client.next_descriptor_transaction_ring_sequence,
            request.payload, request.header.payload_length,
            ring_response, sizeof(ring_response), &ring_response_length,
            30000U);
        const uint64_t finished_ns = bvb_global_client.frame_profile_enabled
            ? frame_profile_monotonic_ns() : 0U;
        frame_profile_record_descriptor_ring_locked(started_ns, finished_ns);
        if (result == 0) {
            memcpy(response.payload, ring_response, ring_response_length);
            response.header.payload_length = ring_response_length;
        }
    } else if (result == 0 && !descriptor_transaction) {
        result = exchange_locked(&request, &response);
        if (result == 0 && response.header.status != 0)
            result = response.header.status;
    }
    if (result == 0 && !descriptor_lease_claimed) {
        result = bvb_protocol_decode_vulkan_descriptor_set_allocate_response(
            response.payload, response.header.payload_length, &allocated);
    }
    if (descriptor_transaction && !descriptor_lease_claimed) {
        if (result == 0) {
            bvb_global_client.descriptor_journal_length = 0U;
            bvb_global_client.descriptor_journal_record_count = 0U;
            ++bvb_global_client.next_descriptor_journal_sequence;
            if (bvb_global_client.next_descriptor_journal_sequence == 0U) {
                result = -EOVERFLOW;
            }
            if (result == 0) {
                ++bvb_global_client
                      .next_descriptor_transaction_ring_sequence;
                if (bvb_global_client
                        .next_descriptor_transaction_ring_sequence == 0U) {
                    result = -EOVERFLOW;
                }
            }
        }
        if (result != 0) {
            if (bvb_global_client.descriptor_transaction_ring != NULL) {
                (void)bvb_descriptor_transaction_ring_fail_client(
                    bvb_global_client.descriptor_transaction_ring, result);
            }
            poison_descriptor_journal_connection_locked();
        }
    }
    VkResult vulkan_result = result == 0
        ? (VkResult)allocated.vulkan_result : VK_ERROR_INITIALIZATION_FAILED;
    if (vulkan_result == VK_SUCCESS &&
        allocated.descriptor_set_count != allocate_info->descriptorSetCount) {
        vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    }
    if (vulkan_result == VK_SUCCESS) {
        if (pthread_rwlock_wrlock(&bvb_object_registry_lock) != 0) {
            vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    if (vulkan_result == VK_SUCCESS) {
        for (uint32_t index = 0U; index < allocated.descriptor_set_count;
             ++index) {
            states[index]->wire_id = allocated.descriptor_set_ids[index];
            states[index]->parent_id = pool_id;
            states[index]->type = BVB_OBJECT_DESCRIPTOR_SET;
            states[index]->next = bvb_global_client.resources;
            bvb_global_client.resources = states[index];
            memcpy(&descriptor_sets[index],
                   &allocated.descriptor_set_ids[index],
                   sizeof(descriptor_sets[index]));
        }
        (void)pthread_rwlock_unlock(&bvb_object_registry_lock);
    } else {
        for (uint32_t index = 0U; index < allocate_info->descriptorSetCount;
             ++index) free(states[index]);
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return vulkan_result;
}

static uint32_t float_wire_bits(float value) {
    uint32_t bits = 0U;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static VkResult VKAPI_CALL bvb_bridge_vkCreateSampler(
    VkDevice device, const VkSamplerCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkSampler *sampler) {
    if (sampler != NULL) *sampler = VK_NULL_HANDLE;
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || create_info == NULL || sampler == NULL ||
        allocator != NULL ||
        create_info->sType != VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO ||
        create_info->pNext != NULL) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    const struct bvb_vulkan_sampler_create_request decoded = {
        .device_id = device_state->wire_id,
        .flags = create_info->flags,
        .mag_filter = create_info->magFilter,
        .min_filter = create_info->minFilter,
        .mipmap_mode = create_info->mipmapMode,
        .address_mode_u = create_info->addressModeU,
        .address_mode_v = create_info->addressModeV,
        .address_mode_w = create_info->addressModeW,
        .mip_lod_bias_bits = float_wire_bits(create_info->mipLodBias),
        .anisotropy_enable = create_info->anisotropyEnable,
        .max_anisotropy_bits = float_wire_bits(create_info->maxAnisotropy),
        .compare_enable = create_info->compareEnable,
        .compare_op = create_info->compareOp,
        .min_lod_bits = float_wire_bits(create_info->minLod),
        .max_lod_bits = float_wire_bits(create_info->maxLod),
        .border_color = create_info->borderColor,
        .unnormalized_coordinates = create_info->unnormalizedCoordinates,
    };
    uint8_t payload[BVB_VULKAN_SAMPLER_CREATE_REQUEST_SIZE];
    int result = bvb_protocol_encode_vulkan_sampler_create_request(
        payload, &decoded);
    struct bvb_resource_proxy *state = calloc(1, sizeof(*state));
    if (state == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (result != 0 || pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        free(state);
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    uint64_t wire_id = 0U;
    const VkResult vulkan_result = create_resource_locked(
        BVB_OPCODE_VULKAN_SAMPLER_CREATE, payload, sizeof(payload),
        BVB_OBJECT_SAMPLER, device_state->wire_id, state, &wire_id);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (vulkan_result != VK_SUCCESS) {
        free(state);
        return vulkan_result;
    }
    memcpy(sampler, &wire_id, sizeof(*sampler));
    return VK_SUCCESS;
}

static void VKAPI_CALL bvb_bridge_vkDestroySampler(
    VkDevice device, VkSampler sampler,
    const VkAllocationCallbacks *allocator) {
    destroy_resource(
        device, non_dispatchable_wire_id(&sampler, sizeof(sampler)),
        BVB_OBJECT_SAMPLER, BVB_OPCODE_VULKAN_DESCRIPTOR_OBJECT_DESTROY,
        allocator);
}

static void VKAPI_CALL bvb_bridge_vkUpdateDescriptorSets(
    VkDevice device, uint32_t descriptor_write_count,
    const VkWriteDescriptorSet *descriptor_writes,
    uint32_t descriptor_copy_count,
    const VkCopyDescriptorSet *descriptor_copies) {
    (void)descriptor_copies;
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (descriptor_write_count == 0U && descriptor_copy_count == 0U) return;
    if (device_state == NULL || descriptor_copy_count != 0U ||
        descriptor_write_count == 0U ||
        descriptor_write_count > BVB_VULKAN_MAX_DESCRIPTOR_WRITES ||
        descriptor_writes == NULL) return;
    struct bvb_vulkan_descriptor_update_request decoded = {
        .device_id = device_state->wire_id,
        .write_count = descriptor_write_count,
    };
    for (uint32_t index = 0U; index < descriptor_write_count; ++index) {
        const VkWriteDescriptorSet *write = &descriptor_writes[index];
        if (write->sType != VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET ||
            write->pNext != NULL || write->descriptorCount == 0U ||
            write->descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER ||
            write->pImageInfo == NULL || write->pBufferInfo != NULL ||
            write->pTexelBufferView != NULL ||
            write->descriptorCount >
                BVB_VULKAN_MAX_DESCRIPTOR_SAMPLERS - decoded.sampler_count) {
            return;
        }
        decoded.writes[index] = (struct bvb_vulkan_descriptor_write){
            .descriptor_set_id = non_dispatchable_wire_id(
                &write->dstSet, sizeof(write->dstSet)),
            .dst_binding = write->dstBinding,
            .dst_array_element = write->dstArrayElement,
            .descriptor_count = write->descriptorCount,
            .descriptor_type = write->descriptorType,
            .first_sampler = decoded.sampler_count,
        };
        for (uint32_t descriptor = 0U;
             descriptor < write->descriptorCount; ++descriptor) {
            const VkDescriptorImageInfo *image =
                &write->pImageInfo[descriptor];
            if (image->sampler == VK_NULL_HANDLE ||
                image->imageView != VK_NULL_HANDLE ||
                image->imageLayout != VK_IMAGE_LAYOUT_UNDEFINED) return;
            decoded.sampler_ids[decoded.sampler_count++] =
                non_dispatchable_wire_id(
                    &image->sampler, sizeof(image->sampler));
        }
    }
    uint8_t payload[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t payload_length = 0U;
    int result = bvb_protocol_encode_vulkan_descriptor_update_request(
        payload, &decoded, &payload_length);
    if (result != 0 || pthread_mutex_lock(&bvb_global_client.mutex) != 0) return;
    for (uint32_t index = 0U; result == 0 && index < decoded.write_count;
         ++index) {
        struct bvb_resource_proxy *set_state = resource_proxy_locked(
            decoded.writes[index].descriptor_set_id,
            BVB_OBJECT_DESCRIPTOR_SET);
        struct bvb_resource_proxy *pool_state = set_state == NULL
            ? NULL : resource_proxy_locked(
                set_state->parent_id, BVB_OBJECT_DESCRIPTOR_POOL);
        if (pool_state == NULL ||
            pool_state->parent_id != device_state->wire_id) result = -EINVAL;
    }
    for (uint32_t index = 0U; result == 0 && index < decoded.sampler_count;
         ++index) {
        struct bvb_resource_proxy *sampler_state = resource_proxy_locked(
            decoded.sampler_ids[index], BVB_OBJECT_SAMPLER);
        if (sampler_state == NULL ||
            sampler_state->parent_id != device_state->wire_id) result = -EINVAL;
    }
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_DESCRIPTOR_UPDATE,
        .request_id = next_request_id_locked(),
        .payload_length = payload_length,
    };
    if (result == 0) memcpy(request.payload, payload, payload_length);
    struct bvb_protocol_packet response = {0};
    if (result == 0) result = connect_locked();
    if (result == 0) result = exchange_locked(&request, &response);
    if (result == 0 &&
        (response.header.status != 0 || response.header.payload_length != 0U)) {
        result = -EPROTO;
    }
    (void)result;
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
}

static VkResult VKAPI_CALL bvb_bridge_vkCreateDescriptorUpdateTemplate(
    VkDevice device,
    const VkDescriptorUpdateTemplateCreateInfo *create_info,
    const VkAllocationCallbacks *allocator,
    VkDescriptorUpdateTemplate *descriptor_update_template) {
    if (descriptor_update_template != NULL)
        *descriptor_update_template = VK_NULL_HANDLE;
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || create_info == NULL ||
        descriptor_update_template == NULL || allocator != NULL ||
        create_info->sType !=
            VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO ||
        create_info->pNext != NULL || create_info->flags != 0U ||
        create_info->descriptorUpdateEntryCount == 0U ||
        create_info->descriptorUpdateEntryCount >
            BVB_VULKAN_MAX_DESCRIPTOR_UPDATE_TEMPLATE_ENTRIES ||
        create_info->pDescriptorUpdateEntries == NULL ||
        create_info->templateType !=
            VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET ||
        create_info->descriptorSetLayout == VK_NULL_HANDLE ||
        create_info->pipelineLayout != VK_NULL_HANDLE ||
        create_info->pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS ||
        create_info->set != 0U) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    struct bvb_vulkan_descriptor_update_template_create_request decoded = {
        .device_id = device_state->wire_id,
        .flags = create_info->flags,
        .entry_count = create_info->descriptorUpdateEntryCount,
        .template_type = create_info->templateType,
        .set = create_info->set,
        .descriptor_set_layout_id = non_dispatchable_wire_id(
            &create_info->descriptorSetLayout,
            sizeof(create_info->descriptorSetLayout)),
        .pipeline_layout_id = 0U,
        .pipeline_bind_point = create_info->pipelineBindPoint,
    };
    for (uint32_t index = 0U;
         index < create_info->descriptorUpdateEntryCount; ++index) {
        const VkDescriptorUpdateTemplateEntry *entry =
            &create_info->pDescriptorUpdateEntries[index];
        decoded.entries[index] =
            (struct bvb_vulkan_descriptor_update_template_entry){
                .dst_binding = entry->dstBinding,
                .dst_array_element = entry->dstArrayElement,
                .descriptor_count = entry->descriptorCount,
                .descriptor_type = entry->descriptorType,
                .offset = entry->offset,
                .stride = entry->stride,
            };
    }
    uint8_t payload[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t payload_length = 0U;
    int result =
        bvb_protocol_encode_vulkan_descriptor_update_template_create_request(
            payload, &decoded, &payload_length);
    struct bvb_resource_proxy *state = calloc(1, sizeof(*state));
    if (state == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
    state->descriptor_template = calloc(
        1U, sizeof(*state->descriptor_template));
    if (state->descriptor_template == NULL) {
        free(state);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    state->descriptor_template->entry_count = decoded.entry_count;
    memcpy(state->descriptor_template->entries, decoded.entries,
           decoded.entry_count * sizeof(decoded.entries[0]));
    if (result != 0 || pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        release_resource_metadata(state);
        free(state);
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    struct bvb_resource_proxy *layout_state = resource_proxy_locked(
        decoded.descriptor_set_layout_id,
        BVB_OBJECT_DESCRIPTOR_SET_LAYOUT);
    if (layout_state == NULL ||
        layout_state->parent_id != device_state->wire_id) {
        result = -EINVAL;
    }
    uint64_t wire_id = 0U;
    VkResult vulkan_result = result == 0
        ? create_resource_locked(
              BVB_OPCODE_VULKAN_DESCRIPTOR_UPDATE_TEMPLATE_CREATE,
              payload, payload_length,
              BVB_OBJECT_DESCRIPTOR_UPDATE_TEMPLATE,
              device_state->wire_id, state, &wire_id)
        : VK_ERROR_INITIALIZATION_FAILED;
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (getenv("BVB_ICD_DIAGNOSTICS") != NULL) {
        fprintf(stderr,
                "BVB_ICD_DESCRIPTOR_TEMPLATE entries=%u layout=%#llx "
                "result=%d\n",
                (unsigned int)decoded.entry_count,
                (unsigned long long)decoded.descriptor_set_layout_id,
                (int)vulkan_result);
    }
    if (vulkan_result != VK_SUCCESS) {
        release_resource_metadata(state);
        free(state);
        return vulkan_result;
    }
    memcpy(descriptor_update_template, &wire_id,
           sizeof(*descriptor_update_template));
    return VK_SUCCESS;
}

static void VKAPI_CALL bvb_bridge_vkDestroyDescriptorUpdateTemplate(
    VkDevice device, VkDescriptorUpdateTemplate descriptor_update_template,
    const VkAllocationCallbacks *allocator) {
    destroy_resource(
        device,
        non_dispatchable_wire_id(
            &descriptor_update_template, sizeof(descriptor_update_template)),
        BVB_OBJECT_DESCRIPTOR_UPDATE_TEMPLATE,
        BVB_OPCODE_VULKAN_DESCRIPTOR_OBJECT_DESTROY, allocator);
}

static void VKAPI_CALL bvb_bridge_vkUpdateDescriptorSetWithTemplate(
    VkDevice device, VkDescriptorSet descriptor_set,
    VkDescriptorUpdateTemplate descriptor_update_template,
    const void *data) {
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || data == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) return;
    const uint64_t set_id = non_dispatchable_wire_id(
        &descriptor_set, sizeof(descriptor_set));
    const uint64_t template_id = non_dispatchable_wire_id(
        &descriptor_update_template, sizeof(descriptor_update_template));
    struct bvb_resource_proxy *set_state = resource_proxy_locked(
        set_id, BVB_OBJECT_DESCRIPTOR_SET);
    struct bvb_resource_proxy *pool_state = set_state == NULL ? NULL :
        resource_proxy_locked(set_state->parent_id,
                              BVB_OBJECT_DESCRIPTOR_POOL);
    struct bvb_resource_proxy *template_state = resource_proxy_locked(
        template_id, BVB_OBJECT_DESCRIPTOR_UPDATE_TEMPLATE);
    struct bvb_descriptor_template_client_metadata *metadata =
        template_state == NULL ? NULL : template_state->descriptor_template;
    int result = pool_state != NULL && template_state != NULL &&
            pool_state->parent_id == device_state->wire_id &&
            template_state->parent_id == device_state->wire_id &&
            metadata != NULL
        ? 0 : -EINVAL;
    struct bvb_vulkan_descriptor_template_update_request decoded = {
        .device_id = device_state->wire_id,
        .descriptor_set_id = set_id,
        .descriptor_update_template_id = template_id,
    };
    const uint8_t *input = data;
    for (uint32_t entry_index = 0U; result == 0 &&
         entry_index < metadata->entry_count; ++entry_index) {
        const struct bvb_vulkan_descriptor_update_template_entry *entry =
            &metadata->entries[entry_index];
        if (entry->descriptor_count >
            BVB_VULKAN_MAX_DESCRIPTOR_TEMPLATE_VALUES - decoded.value_count) {
            result = -E2BIG;
            break;
        }
        for (uint32_t descriptor = 0U;
             result == 0 && descriptor < entry->descriptor_count;
             ++descriptor) {
            const uint8_t *source = input + entry->offset +
                (uint64_t)descriptor * entry->stride;
            struct bvb_vulkan_descriptor_template_value *value =
                &decoded.values[decoded.value_count++];
            value->descriptor_type = entry->descriptor_type;
            switch ((VkDescriptorType)entry->descriptor_type) {
            case VK_DESCRIPTOR_TYPE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
                VkDescriptorImageInfo info;
                memcpy(&info, source, sizeof(info));
                value->image_layout = info.imageLayout;
                if (entry->descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLER) {
                    value->object_id = non_dispatchable_wire_id(
                        &info.sampler, sizeof(info.sampler));
                } else {
                    value->object_id = non_dispatchable_wire_id(
                        &info.imageView, sizeof(info.imageView));
                    if (entry->descriptor_type ==
                        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                        value->auxiliary_object_id =
                            non_dispatchable_wire_id(
                                &info.sampler, sizeof(info.sampler));
                    } else if (info.sampler != VK_NULL_HANDLE) {
                        result = -EPROTO;
                    }
                }
                break;
            }
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
                VkBufferView view = VK_NULL_HANDLE;
                memcpy(&view, source, sizeof(view));
                if (view != VK_NULL_HANDLE) result = -ENOTSUP;
                break;
            }
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
                VkDescriptorBufferInfo info;
                memcpy(&info, source, sizeof(info));
                value->object_id = non_dispatchable_wire_id(
                    &info.buffer, sizeof(info.buffer));
                value->offset = info.offset;
                value->range = info.range;
                break;
            }
            default:
                result = -ENOTSUP;
                break;
            }
        }
    }
    for (uint32_t index = 0U; result == 0 &&
         index < decoded.value_count; ++index) {
        const struct bvb_vulkan_descriptor_template_value *value =
            &decoded.values[index];
        if (value->object_id != 0U) {
            enum bvb_object_type type = BVB_OBJECT_INVALID;
            if (value->descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLER)
                type = BVB_OBJECT_SAMPLER;
            else if (value->descriptor_type ==
                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                     value->descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                     value->descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
                     value->descriptor_type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)
                type = BVB_OBJECT_IMAGE_VIEW;
            else if (value->descriptor_type >= VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
                     value->descriptor_type <=
                         VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
                type = BVB_OBJECT_BUFFER;
            struct bvb_resource_proxy *object =
                resource_proxy_locked(value->object_id, type);
            if (object == NULL) {
                result = -EINVAL;
            } else if (type == BVB_OBJECT_IMAGE_VIEW) {
                if (object->parent_id != device_state->wire_id)
                    result = -EINVAL;
            } else if (object->parent_id != device_state->wire_id) {
                result = -EINVAL;
            }
        }
        if (result == 0 && value->auxiliary_object_id != 0U) {
            struct bvb_resource_proxy *sampler = resource_proxy_locked(
                value->auxiliary_object_id, BVB_OBJECT_SAMPLER);
            if (sampler == NULL ||
                sampler->parent_id != device_state->wire_id) result = -EINVAL;
        }
    }
    uint8_t payload[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t payload_length = 0U;
    if (result == 0)
        result = bvb_protocol_encode_vulkan_descriptor_template_update_request(
            payload, &decoded, &payload_length);
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_DESCRIPTOR_UPDATE_TEMPLATE,
        .request_id = next_request_id_locked(),
        .payload_length = payload_length,
    };
    if (result == 0) memcpy(request.payload, payload, payload_length);
    struct bvb_protocol_packet response = {0};
    if (result == 0) result = connect_locked();
    if (result == 0 && bvb_global_client.descriptor_journal_enabled) {
        result = append_descriptor_journal_locked(payload, payload_length);
    } else if (result == 0) {
        result = exchange_locked(&request, &response);
        if (result == 0 &&
            (response.header.status != 0 ||
             response.header.payload_length != 0U)) {
            result = -EPROTO;
        }
    }
    if (result != 0 && bvb_global_client.descriptor_journal_enabled)
        poison_descriptor_journal_connection_locked();
    if (result != 0 && getenv("BVB_ICD_DIAGNOSTICS") != NULL)
        fprintf(stderr, "BVB_ICD_DESCRIPTOR_TEMPLATE_UPDATE result=%d\n",
                result);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
}

static VkResult VKAPI_CALL bvb_bridge_vkCreatePipelineLayout(
    VkDevice device, const VkPipelineLayoutCreateInfo *create_info,
    const VkAllocationCallbacks *allocator,
    VkPipelineLayout *pipeline_layout) {
    if (pipeline_layout != NULL) *pipeline_layout = VK_NULL_HANDLE;
    struct bvb_device_proxy *device_state = device_proxy(device);
    const VkPipelineLayoutCreateFlags allowed_flags =
        VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT;
    const VkShaderStageFlags allowed_stages =
        VK_SHADER_STAGE_VERTEX_BIT |
        VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
        VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT |
        VK_SHADER_STAGE_GEOMETRY_BIT |
        VK_SHADER_STAGE_FRAGMENT_BIT |
        VK_SHADER_STAGE_COMPUTE_BIT;
    if (device_state == NULL || create_info == NULL ||
        pipeline_layout == NULL || allocator != NULL ||
        create_info->sType != VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO ||
        create_info->pNext != NULL ||
        (create_info->flags & ~allowed_flags) != 0U ||
        create_info->setLayoutCount > BVB_VULKAN_MAX_PIPELINE_SET_LAYOUTS ||
        create_info->pushConstantRangeCount >
            BVB_VULKAN_MAX_PIPELINE_PUSH_CONSTANT_RANGES ||
        (create_info->setLayoutCount != 0U &&
         create_info->pSetLayouts == NULL) ||
        (create_info->pushConstantRangeCount != 0U &&
         create_info->pPushConstantRanges == NULL)) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    struct bvb_vulkan_pipeline_layout_create_request decoded = {
        .device_id = device_state->wire_id,
        .flags = create_info->flags,
        .set_layout_count = create_info->setLayoutCount,
        .push_constant_range_count = create_info->pushConstantRangeCount,
    };
    for (uint32_t index = 0U; index < create_info->setLayoutCount; ++index) {
        decoded.set_layout_ids[index] = non_dispatchable_wire_id(
            &create_info->pSetLayouts[index],
            sizeof(create_info->pSetLayouts[index]));
        if (decoded.set_layout_ids[index] == 0U &&
            (create_info->flags &
             VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT) == 0U) {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    VkShaderStageFlags used_stages = 0U;
    for (uint32_t index = 0U;
         index < create_info->pushConstantRangeCount; ++index) {
        const VkPushConstantRange *range =
            &create_info->pPushConstantRanges[index];
        if (range->stageFlags == 0U ||
            (range->stageFlags & ~allowed_stages) != 0U ||
            (range->stageFlags & used_stages) != 0U || range->size == 0U ||
            (range->offset & 3U) != 0U || (range->size & 3U) != 0U ||
            range->offset > 256U || range->size > 256U - range->offset) {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        used_stages |= range->stageFlags;
        decoded.push_constant_ranges[index] =
            (struct bvb_vulkan_pipeline_push_constant_range){
                .stage_flags = range->stageFlags,
                .offset = range->offset,
                .size = range->size,
            };
    }
    uint8_t payload[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t payload_length = 0U;
    int result = bvb_protocol_encode_vulkan_pipeline_layout_create_request(
        payload, &decoded, &payload_length);
    struct bvb_resource_proxy *state = calloc(1, sizeof(*state));
    if (state == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (result != 0 || pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        free(state);
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    for (uint32_t index = 0U;
         result == 0 && index < decoded.set_layout_count; ++index) {
        if (decoded.set_layout_ids[index] == 0U) continue;
        struct bvb_resource_proxy *layout_state = resource_proxy_locked(
            decoded.set_layout_ids[index], BVB_OBJECT_DESCRIPTOR_SET_LAYOUT);
        if (layout_state == NULL ||
            layout_state->parent_id != device_state->wire_id) result = -EINVAL;
    }
    uint64_t wire_id = 0U;
    VkResult vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
    if (result == 0) {
        vulkan_result = create_resource_locked(
            BVB_OPCODE_VULKAN_PIPELINE_LAYOUT_CREATE, payload, payload_length,
            BVB_OBJECT_PIPELINE_LAYOUT, device_state->wire_id, state,
            &wire_id);
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (vulkan_result != VK_SUCCESS) {
        free(state);
        return vulkan_result;
    }
    memcpy(pipeline_layout, &wire_id, sizeof(*pipeline_layout));
    return VK_SUCCESS;
}

static void VKAPI_CALL bvb_bridge_vkDestroyPipelineLayout(
    VkDevice device, VkPipelineLayout pipeline_layout,
    const VkAllocationCallbacks *allocator) {
    destroy_resource(
        device,
        non_dispatchable_wire_id(&pipeline_layout, sizeof(pipeline_layout)),
        BVB_OBJECT_PIPELINE_LAYOUT,
        BVB_OPCODE_VULKAN_PIPELINE_LAYOUT_DESTROY, allocator);
}

static bool stencil_op_state_is_zero(const VkStencilOpState *state) {
    return state->failOp == VK_STENCIL_OP_KEEP &&
        state->passOp == VK_STENCIL_OP_KEEP &&
        state->depthFailOp == VK_STENCIL_OP_KEEP &&
        state->compareOp == VK_COMPARE_OP_NEVER && state->compareMask == 0U &&
        state->writeMask == 0U && state->reference == 0U;
}

static bool depth_stencil_state_is_dxvk_null_fragment(
    const VkPipelineDepthStencilStateCreateInfo *state) {
    return state != NULL &&
        state->sType ==
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO &&
        state->pNext == NULL && state->flags == 0U &&
        state->depthTestEnable == VK_FALSE &&
        state->depthWriteEnable == VK_FALSE &&
        state->depthCompareOp == VK_COMPARE_OP_NEVER &&
        state->depthBoundsTestEnable == VK_FALSE &&
        state->stencilTestEnable == VK_FALSE &&
        stencil_op_state_is_zero(&state->front) &&
        stencil_op_state_is_zero(&state->back) &&
        state->minDepthBounds == 0.0F && state->maxDepthBounds == 0.0F;
}

static bool dynamic_state_is_dxvk_null_fragment(
    const VkPipelineDynamicStateCreateInfo *state) {
    static const VkDynamicState expected[] = {
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP,
    };
    return state != NULL &&
        state->sType == VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO &&
        state->pNext == NULL && state->flags == 0U &&
        state->dynamicStateCount ==
            (uint32_t)(sizeof(expected) / sizeof(expected[0])) &&
        state->pDynamicStates != NULL &&
        memcmp(state->pDynamicStates, expected, sizeof(expected)) == 0;
}

static void diagnose_graphics_pipeline_call(
    VkPipelineCache pipeline_cache, uint32_t create_info_count,
    const VkGraphicsPipelineCreateInfo *create_infos,
    const VkAllocationCallbacks *allocator) {
    if (getenv("BVB_ICD_DIAGNOSTICS") == NULL) return;
    fprintf(stderr,
            "BVB_ICD_GRAPHICS_PIPELINE count=%u cache=%#llx allocator=%u "
            "infos=%u\n",
            create_info_count, (unsigned long long)(uintptr_t)pipeline_cache,
            allocator != NULL, create_infos != NULL);
    if (create_infos == NULL || create_info_count == 0U) return;
    const VkGraphicsPipelineCreateInfo *info = &create_infos[0];
    fprintf(stderr,
            "BVB_ICD_GRAPHICS_ROOT stype=%u flags=%u stages=%u layout=%#llx "
            "render_pass=%#llx subpass=%u base=%#llx base_index=%d "
            "vi=%u ia=%u tess=%u viewport=%u raster=%u ms=%u depth=%u "
            "blend=%u dynamic=%u\n",
            (unsigned)info->sType, (unsigned)info->flags, info->stageCount,
            (unsigned long long)(uintptr_t)info->layout,
            (unsigned long long)(uintptr_t)info->renderPass, info->subpass,
            (unsigned long long)(uintptr_t)info->basePipelineHandle,
            info->basePipelineIndex, info->pVertexInputState != NULL,
            info->pInputAssemblyState != NULL,
            info->pTessellationState != NULL,
            info->pViewportState != NULL,
            info->pRasterizationState != NULL,
            info->pMultisampleState != NULL,
            info->pDepthStencilState != NULL,
            info->pColorBlendState != NULL,
            info->pDynamicState != NULL);
    const VkBaseInStructure *next = info->pNext;
    for (uint32_t index = 0U; next != NULL && index < 8U; ++index) {
        fprintf(stderr, "BVB_ICD_GRAPHICS_PNEXT index=%u stype=%u\n",
                index, (unsigned)next->sType);
        if (next->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
            const VkPipelineRenderingCreateInfo *rendering =
                (const VkPipelineRenderingCreateInfo *)next;
            fprintf(stderr,
                    "BVB_ICD_GRAPHICS_RENDERING view_mask=%u colors=%u "
                    "color0=%u depth=%u stencil=%u\n",
                    rendering->viewMask, rendering->colorAttachmentCount,
                    rendering->colorAttachmentCount != 0U &&
                            rendering->pColorAttachmentFormats != NULL
                        ? (unsigned)rendering->pColorAttachmentFormats[0]
                        : 0U,
                    (unsigned)rendering->depthAttachmentFormat,
                    (unsigned)rendering->stencilAttachmentFormat);
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO) {
            const VkPipelineCreateFlags2CreateInfo *flags =
                (const VkPipelineCreateFlags2CreateInfo *)next;
            fprintf(stderr, "BVB_ICD_GRAPHICS_FLAGS2 flags=%#llx\n",
                    (unsigned long long)flags->flags);
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT) {
            const VkGraphicsPipelineLibraryCreateInfoEXT *library =
                (const VkGraphicsPipelineLibraryCreateInfoEXT *)next;
            fprintf(stderr, "BVB_ICD_GRAPHICS_LIBRARY flags=%u\n",
                    (unsigned)library->flags);
        }
        next = next->pNext;
    }
    if (info->pStages != NULL) {
        const uint32_t count = info->stageCount > 8U ? 8U : info->stageCount;
        for (uint32_t index = 0U; index < count; ++index) {
            const VkPipelineShaderStageCreateInfo *stage =
                &info->pStages[index];
            const VkBaseInStructure *stage_next = stage->pNext;
            size_t code_size = 0U;
            if (stage_next != NULL && stage_next->sType ==
                    VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
                const VkShaderModuleCreateInfo *module =
                    (const VkShaderModuleCreateInfo *)stage_next;
                code_size = module->codeSize;
            }
            fprintf(stderr,
                    "BVB_ICD_GRAPHICS_STAGE index=%u stype=%u flags=%u "
                    "stage=%u module=%#llx name=%s pnext=%u code_bytes=%zu "
                    "spec_entries=%u spec_bytes=%zu\n",
                    index, (unsigned)stage->sType, (unsigned)stage->flags,
                    (unsigned)stage->stage,
                    (unsigned long long)(uintptr_t)stage->module,
                    stage->pName != NULL ? stage->pName : "(null)",
                    stage_next != NULL ? (unsigned)stage_next->sType : 0U,
                    code_size,
                    stage->pSpecializationInfo != NULL
                        ? stage->pSpecializationInfo->mapEntryCount : 0U,
                    stage->pSpecializationInfo != NULL
                        ? stage->pSpecializationInfo->dataSize : 0U);
        }
    }
    if (info->pVertexInputState != NULL)
        fprintf(stderr, "BVB_ICD_GRAPHICS_VI bindings=%u attributes=%u\n",
                info->pVertexInputState->vertexBindingDescriptionCount,
                info->pVertexInputState->vertexAttributeDescriptionCount);
    if (info->pInputAssemblyState != NULL)
        fprintf(stderr, "BVB_ICD_GRAPHICS_IA topology=%u restart=%u\n",
                (unsigned)info->pInputAssemblyState->topology,
                info->pInputAssemblyState->primitiveRestartEnable);
    if (info->pViewportState != NULL)
        fprintf(stderr, "BVB_ICD_GRAPHICS_VIEWPORT viewports=%u scissors=%u\n",
                info->pViewportState->viewportCount,
                info->pViewportState->scissorCount);
    if (info->pRasterizationState != NULL)
        fprintf(stderr,
                "BVB_ICD_GRAPHICS_RASTER depth_clamp=%u discard=%u "
                "polygon=%u cull=%u front=%u depth_bias=%u line_width=%a\n",
                info->pRasterizationState->depthClampEnable,
                info->pRasterizationState->rasterizerDiscardEnable,
                (unsigned)info->pRasterizationState->polygonMode,
                (unsigned)info->pRasterizationState->cullMode,
                (unsigned)info->pRasterizationState->frontFace,
                info->pRasterizationState->depthBiasEnable,
                (double)info->pRasterizationState->lineWidth);
    if (info->pRasterizationState != NULL &&
        info->pRasterizationState->pNext != NULL) {
        const VkBaseInStructure *raster_next =
            info->pRasterizationState->pNext;
        fprintf(stderr,
                "BVB_ICD_GRAPHICS_RASTER_PNEXT stype=%u next=%u\n",
                (unsigned)raster_next->sType, raster_next->pNext != NULL);
    }
    if (info->pMultisampleState != NULL)
        fprintf(stderr,
                "BVB_ICD_GRAPHICS_MS samples=%u shading=%u min=%a mask0=%#x "
                "alpha_coverage=%u alpha_one=%u\n",
                (unsigned)info->pMultisampleState->rasterizationSamples,
                info->pMultisampleState->sampleShadingEnable,
                (double)info->pMultisampleState->minSampleShading,
                info->pMultisampleState->pSampleMask != NULL
                    ? info->pMultisampleState->pSampleMask[0] : 0U,
                info->pMultisampleState->alphaToCoverageEnable,
                info->pMultisampleState->alphaToOneEnable);
    if (info->pColorBlendState != NULL) {
        const VkPipelineColorBlendStateCreateInfo *blend =
            info->pColorBlendState;
        fprintf(stderr,
                "BVB_ICD_GRAPHICS_BLEND logic=%u op=%u attachments=%u\n",
                blend->logicOpEnable, (unsigned)blend->logicOp,
                blend->attachmentCount);
        if (blend->attachmentCount != 0U && blend->pAttachments != NULL) {
            const VkPipelineColorBlendAttachmentState *attachment =
                &blend->pAttachments[0];
            fprintf(stderr,
                    "BVB_ICD_GRAPHICS_BLEND0 enable=%u src_color=%u "
                    "dst_color=%u color_op=%u src_alpha=%u dst_alpha=%u "
                    "alpha_op=%u mask=%u\n",
                    attachment->blendEnable,
                    (unsigned)attachment->srcColorBlendFactor,
                    (unsigned)attachment->dstColorBlendFactor,
                    (unsigned)attachment->colorBlendOp,
                    (unsigned)attachment->srcAlphaBlendFactor,
                    (unsigned)attachment->dstAlphaBlendFactor,
                    (unsigned)attachment->alphaBlendOp,
                    (unsigned)attachment->colorWriteMask);
        }
    }
    if (info->pDynamicState != NULL) {
        const VkPipelineDynamicStateCreateInfo *dynamic =
            info->pDynamicState;
        fprintf(stderr, "BVB_ICD_GRAPHICS_DYNAMIC count=%u\n",
                dynamic->dynamicStateCount);
        if (dynamic->pDynamicStates != NULL) {
            const uint32_t count = dynamic->dynamicStateCount > 32U
                ? 32U : dynamic->dynamicStateCount;
            for (uint32_t index = 0U; index < count; ++index)
                fprintf(stderr,
                        "BVB_ICD_GRAPHICS_DYNAMIC_STATE index=%u state=%u\n",
                        index, (unsigned)dynamic->pDynamicStates[index]);
        }
    }
}

static bool graphics_pipeline_is_dxvk_builtin_blit(
    const VkGraphicsPipelineCreateInfo *info) {
    if (info == NULL ||
        info->sType != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO ||
        info->flags != 0U || info->pNext == NULL ||
        ((const VkBaseInStructure *)info->pNext)->sType !=
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO ||
        ((const VkBaseInStructure *)info->pNext)->pNext != NULL ||
        info->stageCount != 2U || info->pStages == NULL ||
        info->pVertexInputState == NULL ||
        info->pInputAssemblyState == NULL ||
        info->pTessellationState != NULL || info->pViewportState == NULL ||
        info->pRasterizationState == NULL ||
        info->pMultisampleState == NULL ||
        info->pDepthStencilState != NULL || info->pColorBlendState == NULL ||
        info->pDynamicState == NULL || info->layout == VK_NULL_HANDLE ||
        info->renderPass != VK_NULL_HANDLE || info->subpass != 0U ||
        info->basePipelineHandle != VK_NULL_HANDLE ||
        info->basePipelineIndex != -1) {
        return false;
    }
    const VkPipelineRenderingCreateInfo *rendering = info->pNext;
    if (rendering->viewMask != 0U || rendering->colorAttachmentCount != 1U ||
        rendering->pColorAttachmentFormats == NULL ||
        rendering->pColorAttachmentFormats[0] !=
            VK_FORMAT_R8G8B8A8_UNORM ||
        rendering->depthAttachmentFormat != VK_FORMAT_UNDEFINED ||
        rendering->stencilAttachmentFormat != VK_FORMAT_UNDEFINED) {
        return false;
    }
    const VkPipelineShaderStageCreateInfo *vertex = &info->pStages[0];
    const VkPipelineShaderStageCreateInfo *fragment = &info->pStages[1];
    const VkShaderModuleCreateInfo *vertex_module = vertex->pNext;
    const VkShaderModuleCreateInfo *fragment_module = fragment->pNext;
    if (vertex->sType !=
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO ||
        vertex->flags != 0U || vertex->stage != VK_SHADER_STAGE_VERTEX_BIT ||
        vertex->module != VK_NULL_HANDLE || vertex->pName == NULL ||
        strcmp(vertex->pName, "main") != 0 ||
        vertex->pSpecializationInfo != NULL || vertex_module == NULL ||
        vertex_module->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO ||
        vertex_module->pNext != NULL || vertex_module->flags != 0U ||
        vertex_module->codeSize !=
            BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_VERTEX_CODE_SIZE ||
        vertex_module->pCode == NULL ||
        vertex_module->pCode[0] != UINT32_C(0x07230203) ||
        fragment->sType !=
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO ||
        fragment->flags != 0U ||
        fragment->stage != VK_SHADER_STAGE_FRAGMENT_BIT ||
        fragment->module != VK_NULL_HANDLE || fragment->pName == NULL ||
        strcmp(fragment->pName, "main") != 0 || fragment_module == NULL ||
        fragment_module->sType !=
            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO ||
        fragment_module->pNext != NULL || fragment_module->flags != 0U ||
        fragment_module->codeSize !=
            BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_FRAGMENT_CODE_SIZE ||
        fragment_module->pCode == NULL ||
        fragment_module->pCode[0] != UINT32_C(0x07230203) ||
        fragment->pSpecializationInfo == NULL ||
        fragment->pSpecializationInfo->mapEntryCount !=
            BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_ENTRY_COUNT ||
        fragment->pSpecializationInfo->pMapEntries == NULL ||
        fragment->pSpecializationInfo->dataSize !=
            BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_DATA_SIZE ||
        fragment->pSpecializationInfo->pData == NULL) {
        return false;
    }
    const VkSpecializationInfo *specialization =
        fragment->pSpecializationInfo;
    for (uint32_t index = 0U;
         index < BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_ENTRY_COUNT;
         ++index) {
        const VkSpecializationMapEntry *entry =
            &specialization->pMapEntries[index];
        if (entry->size == 0U ||
            entry->offset >
                BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_DATA_SIZE ||
            entry->size >
                BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_DATA_SIZE -
                    entry->offset) {
            return false;
        }
    }
    const VkPipelineVertexInputStateCreateInfo *vertex_input =
        info->pVertexInputState;
    const VkPipelineInputAssemblyStateCreateInfo *input_assembly =
        info->pInputAssemblyState;
    const VkPipelineViewportStateCreateInfo *viewport = info->pViewportState;
    const VkPipelineRasterizationStateCreateInfo *rasterization =
        info->pRasterizationState;
    const VkPipelineMultisampleStateCreateInfo *multisample =
        info->pMultisampleState;
    const VkPipelineColorBlendStateCreateInfo *blend =
        info->pColorBlendState;
    const VkPipelineDynamicStateCreateInfo *dynamic = info->pDynamicState;
    if (vertex_input->sType !=
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO ||
        vertex_input->pNext != NULL || vertex_input->flags != 0U ||
        vertex_input->vertexBindingDescriptionCount != 0U ||
        vertex_input->pVertexBindingDescriptions != NULL ||
        vertex_input->vertexAttributeDescriptionCount != 0U ||
        vertex_input->pVertexAttributeDescriptions != NULL ||
        input_assembly->sType !=
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO ||
        input_assembly->pNext != NULL || input_assembly->flags != 0U ||
        input_assembly->topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST ||
        input_assembly->primitiveRestartEnable != VK_FALSE ||
        viewport->sType != VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO ||
        viewport->pNext != NULL || viewport->flags != 0U ||
        viewport->viewportCount != 0U || viewport->pViewports != NULL ||
        viewport->scissorCount != 0U || viewport->pScissors != NULL ||
        rasterization->sType !=
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO ||
        rasterization->pNext != NULL || rasterization->flags != 0U ||
        rasterization->depthClampEnable != VK_FALSE ||
        rasterization->rasterizerDiscardEnable != VK_FALSE ||
        rasterization->polygonMode != VK_POLYGON_MODE_FILL ||
        rasterization->cullMode != VK_CULL_MODE_NONE ||
        rasterization->frontFace != VK_FRONT_FACE_COUNTER_CLOCKWISE ||
        rasterization->depthBiasEnable != VK_FALSE ||
        rasterization->depthBiasConstantFactor != 0.0F ||
        rasterization->depthBiasClamp != 0.0F ||
        rasterization->depthBiasSlopeFactor != 0.0F ||
        rasterization->lineWidth != 1.0F ||
        multisample->sType !=
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO ||
        multisample->pNext != NULL || multisample->flags != 0U ||
        multisample->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT ||
        multisample->sampleShadingEnable != VK_FALSE ||
        multisample->minSampleShading != 1.0F ||
        multisample->pSampleMask == NULL ||
        multisample->pSampleMask[0] != 1U ||
        multisample->alphaToCoverageEnable != VK_FALSE ||
        multisample->alphaToOneEnable != VK_FALSE ||
        blend->sType != VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO ||
        blend->pNext != NULL || blend->flags != 0U ||
        blend->logicOpEnable != VK_FALSE || blend->logicOp != VK_LOGIC_OP_CLEAR ||
        blend->attachmentCount != 1U || blend->pAttachments == NULL ||
        blend->blendConstants[0] != 0.0F || blend->blendConstants[1] != 0.0F ||
        blend->blendConstants[2] != 0.0F || blend->blendConstants[3] != 0.0F) {
        return false;
    }
    const VkPipelineColorBlendAttachmentState *attachment =
        &blend->pAttachments[0];
    const VkDynamicState expected_dynamic[2] = {
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,
        VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
    };
    return attachment->blendEnable == VK_FALSE &&
        attachment->srcColorBlendFactor == VK_BLEND_FACTOR_ZERO &&
        attachment->dstColorBlendFactor == VK_BLEND_FACTOR_ZERO &&
        attachment->colorBlendOp == VK_BLEND_OP_ADD &&
        attachment->srcAlphaBlendFactor == VK_BLEND_FACTOR_ZERO &&
        attachment->dstAlphaBlendFactor == VK_BLEND_FACTOR_ZERO &&
        attachment->alphaBlendOp == VK_BLEND_OP_ADD &&
        attachment->colorWriteMask ==
            (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT) &&
        dynamic->sType == VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO &&
        dynamic->pNext == NULL && dynamic->flags == 0U &&
        dynamic->dynamicStateCount == 2U &&
        dynamic->pDynamicStates != NULL &&
        memcmp(dynamic->pDynamicStates, expected_dynamic,
               sizeof(expected_dynamic)) == 0;
}

static VkResult create_dxvk_builtin_blit_pipeline(
    const struct bvb_device_proxy *device_state,
    const VkGraphicsPipelineCreateInfo *create_info, VkPipeline *pipeline) {
    const VkPipelineShaderStageCreateInfo *stages = create_info->pStages;
    const VkShaderModuleCreateInfo *vertex_module = stages[0].pNext;
    const VkShaderModuleCreateInfo *fragment_module = stages[1].pNext;
    const VkSpecializationInfo *specialization =
        stages[1].pSpecializationInfo;
    uint8_t specialization_entries[
        BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_ENTRY_COUNT *
        BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_ENTRY_SIZE];
    memset(specialization_entries, 0, sizeof(specialization_entries));
    for (uint32_t index = 0U;
         index < BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_ENTRY_COUNT;
         ++index) {
        const VkSpecializationMapEntry *source =
            &specialization->pMapEntries[index];
        uint8_t *destination = specialization_entries +
            index * BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_ENTRY_SIZE;
        bvb_wire_put_u32(destination, source->constantID);
        bvb_wire_put_u32(destination + 4U, source->offset);
        bvb_wire_put_u64(destination + 8U, source->size);
    }
    uint8_t *blob = malloc(BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_BLOB_SIZE);
    if (blob == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
    int result = bvb_protocol_encode_vulkan_builtin_graphics_pipeline_blob(
        blob, vertex_module->pCode, fragment_module->pCode,
        specialization_entries, specialization->pData);
    int blob_fd = -1;
    if (result == 0) {
        blob_fd = (int)syscall(
            SYS_memfd_create, "bvb-dxvk-builtin-pipeline",
            MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (blob_fd < 0) result = -errno;
    }
    if (result == 0 &&
        ftruncate(blob_fd,
                  BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_BLOB_SIZE) != 0) {
        result = -errno;
    }
    size_t written = 0U;
    while (result == 0 &&
           written < BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_BLOB_SIZE) {
        const ssize_t count = pwrite(
            blob_fd, blob + written,
            BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_BLOB_SIZE - written,
            (off_t)written);
        if (count <= 0) {
            result = count < 0 ? -errno : -EIO;
        } else {
            written += (size_t)count;
        }
    }
    if (result == 0 &&
        fcntl(blob_fd, F_ADD_SEALS,
              F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_WRITE | F_SEAL_SEAL) !=
            0) {
        result = -errno;
    }
    free(blob);
    const struct bvb_vulkan_builtin_graphics_pipeline_create_request decoded = {
        .device_id = device_state->wire_id,
        .pipeline_layout_id = non_dispatchable_wire_id(
            &create_info->layout, sizeof(create_info->layout)),
        .blob_bytes = BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_BLOB_SIZE,
        .schema = BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_BLOB_VERSION,
    };
    uint8_t payload[BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_REQUEST_SIZE];
    if (result == 0)
        result =
            bvb_protocol_encode_vulkan_builtin_graphics_pipeline_create_request(
                payload, &decoded);
    struct bvb_resource_proxy *state = calloc(1, sizeof(*state));
    if (state == NULL) {
        if (blob_fd >= 0) (void)close(blob_fd);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (result != 0 || pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        if (blob_fd >= 0) (void)close(blob_fd);
        free(state);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    struct bvb_resource_proxy *layout_state = resource_proxy_locked(
        decoded.pipeline_layout_id, BVB_OBJECT_PIPELINE_LAYOUT);
    if (layout_state == NULL ||
        layout_state->parent_id != device_state->wire_id) result = -EINVAL;
    uint64_t wire_id = 0U;
    VkResult vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    if (result == 0)
        vulkan_result = create_resource_fd_locked(
            BVB_OPCODE_VULKAN_BUILTIN_GRAPHICS_PIPELINE_CREATE, payload,
            sizeof(payload), blob_fd, BVB_OBJECT_PIPELINE,
            device_state->wire_id, state, &wire_id);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    (void)close(blob_fd);
    if (vulkan_result != VK_SUCCESS) {
        free(state);
        return vulkan_result;
    }
    memcpy(pipeline, &wire_id, sizeof(*pipeline));
    return VK_SUCCESS;
}

struct general_graphics_blob_builder {
    uint8_t *data;
    uint32_t size;
    uint32_t capacity;
};

static uint32_t general_graphics_blob_append(
    struct general_graphics_blob_builder *builder, const void *source,
    uint32_t size, uint32_t alignment) {
    if (builder == NULL || builder->data == NULL || alignment == 0U ||
        (alignment & (alignment - 1U)) != 0U) {
        return 0U;
    }
    const uint32_t aligned =
        (builder->size + alignment - 1U) & ~(alignment - 1U);
    if (aligned < builder->size || size > builder->capacity - aligned)
        return 0U;
    if (source != NULL) memcpy(builder->data + aligned, source, size);
    builder->size = aligned + size;
    return aligned;
}

static const void *general_graphics_blob_offset(uint32_t offset) {
    return (const void *)(uintptr_t)offset;
}

static int create_sealed_graphics_blob_fd(
    const uint8_t *blob, uint32_t blob_bytes) {
    const int fd = (int)syscall(
        SYS_memfd_create, "bvb-general-graphics-pipeline",
        MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -errno;
    int result = 0;
    if (ftruncate(fd, (off_t)blob_bytes) != 0) result = -errno;
    size_t written = 0U;
    while (result == 0 && written < blob_bytes) {
        const ssize_t count = pwrite(
            fd, blob + written, blob_bytes - written, (off_t)written);
        if (count <= 0)
            result = count < 0 ? -errno : -EIO;
        else
            written += (size_t)count;
    }
    if (result == 0 &&
        fcntl(fd, F_ADD_SEALS,
              F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_WRITE | F_SEAL_SEAL) != 0)
        result = -errno;
    if (result != 0) {
        (void)close(fd);
        return result;
    }
    return fd;
}

static VkResult create_general_graphics_pipeline(
    const struct bvb_device_proxy *device_state,
    const VkGraphicsPipelineCreateInfo *info, VkPipeline *pipeline) {
    const VkPipelineRenderingCreateInfo *rendering = info == NULL
        ? NULL : (const VkPipelineRenderingCreateInfo *)info->pNext;
    if (info == NULL || rendering == NULL ||
        info->sType != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO ||
        info->flags != 0U ||
        rendering->sType != VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO ||
        rendering->pNext != NULL ||
        rendering->colorAttachmentCount >
            BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_COLOR_ATTACHMENTS ||
        (rendering->colorAttachmentCount != 0U &&
         rendering->pColorAttachmentFormats == NULL) ||
        info->stageCount == 0U ||
        info->stageCount > BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_STAGES ||
        info->pStages == NULL || info->pVertexInputState == NULL ||
        info->pInputAssemblyState == NULL || info->pViewportState == NULL ||
        info->pRasterizationState == NULL || info->pMultisampleState == NULL ||
        info->pColorBlendState == NULL || info->pDynamicState == NULL ||
        info->layout == VK_NULL_HANDLE || info->renderPass != VK_NULL_HANDLE ||
        info->subpass != 0U || info->basePipelineHandle != VK_NULL_HANDLE ||
        info->basePipelineIndex != -1) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    const VkPipelineVertexInputStateCreateInfo *vertex =
        info->pVertexInputState;
    const VkPipelineInputAssemblyStateCreateInfo *assembly =
        info->pInputAssemblyState;
    const VkPipelineViewportStateCreateInfo *viewport = info->pViewportState;
    const VkPipelineRasterizationStateCreateInfo *raster =
        info->pRasterizationState;
    const VkPipelineRasterizationDepthClipStateCreateInfoEXT *depth_clip =
        raster == NULL ? NULL : raster->pNext;
    const VkPipelineMultisampleStateCreateInfo *multisample =
        info->pMultisampleState;
    const VkPipelineDepthStencilStateCreateInfo *depth =
        info->pDepthStencilState;
    const VkPipelineColorBlendStateCreateInfo *blend = info->pColorBlendState;
    const VkPipelineDynamicStateCreateInfo *dynamic = info->pDynamicState;
    const VkPipelineTessellationStateCreateInfo *tessellation =
        info->pTessellationState;
    if (vertex->sType !=
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO ||
        vertex->pNext != NULL || vertex->flags != 0U ||
        vertex->vertexBindingDescriptionCount >
            BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_VERTEX_BINDINGS ||
        vertex->vertexAttributeDescriptionCount >
            BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_VERTEX_ATTRIBUTES ||
        (vertex->vertexBindingDescriptionCount != 0U &&
         vertex->pVertexBindingDescriptions == NULL) ||
        (vertex->vertexAttributeDescriptionCount != 0U &&
         vertex->pVertexAttributeDescriptions == NULL) ||
        assembly->sType !=
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO ||
        assembly->pNext != NULL || assembly->flags != 0U ||
        viewport->sType != VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO ||
        viewport->pNext != NULL || viewport->flags != 0U ||
        viewport->viewportCount >
            BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_VIEWPORTS ||
        viewport->scissorCount >
            BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_VIEWPORTS ||
        (viewport->viewportCount != 0U && viewport->pViewports == NULL) ||
        (viewport->scissorCount != 0U && viewport->pScissors == NULL) ||
        raster->sType !=
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO ||
        raster->flags != 0U ||
        (depth_clip != NULL &&
         (depth_clip->sType !=
              VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT ||
          depth_clip->pNext != NULL || depth_clip->flags != 0U ||
          depth_clip->depthClipEnable > VK_TRUE)) ||
        multisample->sType !=
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO ||
        multisample->pNext != NULL || multisample->flags != 0U ||
        (depth != NULL &&
         (depth->sType !=
              VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO ||
          depth->pNext != NULL || depth->flags != 0U)) ||
        blend->sType !=
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO ||
        blend->pNext != NULL || blend->flags != 0U ||
        blend->attachmentCount >
            BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_COLOR_ATTACHMENTS ||
        (blend->attachmentCount != 0U && blend->pAttachments == NULL) ||
        dynamic->sType !=
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO ||
        dynamic->pNext != NULL || dynamic->flags != 0U ||
        dynamic->dynamicStateCount == 0U ||
        dynamic->dynamicStateCount >
            BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_DYNAMIC_STATES ||
        dynamic->pDynamicStates == NULL ||
        (tessellation != NULL &&
         (tessellation->sType !=
              VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO ||
          tessellation->pNext != NULL || tessellation->flags != 0U))) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    for (uint32_t index = 0U; index < info->stageCount; ++index) {
        const VkPipelineShaderStageCreateInfo *stage = &info->pStages[index];
        const VkShaderModuleCreateInfo *module = stage->pNext;
        const VkSpecializationInfo *specialization = stage->pSpecializationInfo;
        if (stage->sType !=
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO ||
            stage->flags != 0U || stage->module != VK_NULL_HANDLE ||
            stage->pName == NULL || strcmp(stage->pName, "main") != 0 ||
            module == NULL ||
            module->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO ||
            module->pNext != NULL || module->flags != 0U ||
            module->codeSize < 5U * sizeof(uint32_t) ||
            (module->codeSize & 3U) != 0U || module->pCode == NULL ||
            module->pCode[0] != UINT32_C(0x07230203) ||
            module->codeSize >
                BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_BLOB_MAX_SIZE ||
            (specialization != NULL &&
             (specialization->mapEntryCount >
                  BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_SPEC_ENTRIES ||
              specialization->dataSize >
                  BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_SPEC_BYTES ||
              (specialization->mapEntryCount != 0U &&
               specialization->pMapEntries == NULL) ||
              (specialization->dataSize != 0U &&
               specialization->pData == NULL)))) {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    uint8_t *blob = calloc(
        1U, BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_BLOB_MAX_SIZE);
    if (blob == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct general_graphics_blob_builder builder = {
        .data = blob,
        .size = BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_BLOB_HEADER_SIZE,
        .capacity = BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_BLOB_MAX_SIZE,
    };
    const uint32_t root_offset = general_graphics_blob_append(
        &builder, info, sizeof(*info), 8U);
    VkGraphicsPipelineCreateInfo *wire_root =
        root_offset == 0U ? NULL : (VkGraphicsPipelineCreateInfo *)(blob + root_offset);
    int result = wire_root == NULL ? -E2BIG : 0;
#define BVB_APPEND_OBJECT(source, type) \
    general_graphics_blob_append(&builder, (source), sizeof(type), 8U)
#define BVB_APPEND_ARRAY(source, count, type) \
    ((count) == 0U ? 0U : general_graphics_blob_append( \
        &builder, (source), (uint32_t)((count) * sizeof(type)), 8U))
    uint32_t rendering_offset = 0U, stages_offset = 0U;
    if (result == 0) {
        rendering_offset = BVB_APPEND_OBJECT(rendering, VkPipelineRenderingCreateInfo);
        stages_offset = BVB_APPEND_ARRAY(
            info->pStages, info->stageCount, VkPipelineShaderStageCreateInfo);
        if (rendering_offset == 0U || stages_offset == 0U) result = -E2BIG;
    }
    if (result == 0) {
        VkPipelineRenderingCreateInfo *wire_rendering =
            (VkPipelineRenderingCreateInfo *)(blob + rendering_offset);
        wire_rendering->pNext = NULL;
        const uint32_t formats_offset = BVB_APPEND_ARRAY(
            rendering->pColorAttachmentFormats,
            rendering->colorAttachmentCount, VkFormat);
        if (rendering->colorAttachmentCount != 0U && formats_offset == 0U)
            result = -E2BIG;
        wire_rendering->pColorAttachmentFormats =
            general_graphics_blob_offset(formats_offset);
        wire_root->pNext = general_graphics_blob_offset(rendering_offset);
        wire_root->pStages = general_graphics_blob_offset(stages_offset);
    }
    if (result == 0) {
        VkPipelineShaderStageCreateInfo *wire_stages =
            (VkPipelineShaderStageCreateInfo *)(blob + stages_offset);
        for (uint32_t index = 0U; index < info->stageCount && result == 0;
             ++index) {
            const VkPipelineShaderStageCreateInfo *stage = &info->pStages[index];
            const VkShaderModuleCreateInfo *module = stage->pNext;
            const VkSpecializationInfo *specialization = stage->pSpecializationInfo;
            const uint32_t module_offset = BVB_APPEND_OBJECT(
                module, VkShaderModuleCreateInfo);
            const uint32_t code_offset = general_graphics_blob_append(
                &builder, module->pCode, (uint32_t)module->codeSize, 8U);
            const uint32_t name_offset = general_graphics_blob_append(
                &builder, "main", 5U, 1U);
            if (module_offset == 0U || code_offset == 0U || name_offset == 0U) {
                result = -E2BIG;
                break;
            }
            VkShaderModuleCreateInfo *wire_module =
                (VkShaderModuleCreateInfo *)(blob + module_offset);
            wire_module->pNext = NULL;
            wire_module->pCode = general_graphics_blob_offset(code_offset);
            wire_stages[index].pNext = general_graphics_blob_offset(module_offset);
            wire_stages[index].pName = general_graphics_blob_offset(name_offset);
            wire_stages[index].module = VK_NULL_HANDLE;
            wire_stages[index].pSpecializationInfo = NULL;
            if (specialization != NULL) {
                const uint32_t spec_offset = BVB_APPEND_OBJECT(
                    specialization, VkSpecializationInfo);
                const uint32_t entries_offset = BVB_APPEND_ARRAY(
                    specialization->pMapEntries,
                    specialization->mapEntryCount, VkSpecializationMapEntry);
                const uint32_t data_offset = specialization->dataSize == 0U
                    ? 0U : general_graphics_blob_append(
                        &builder, specialization->pData,
                        (uint32_t)specialization->dataSize, 8U);
                if (spec_offset == 0U ||
                    (specialization->mapEntryCount != 0U &&
                     entries_offset == 0U) ||
                    (specialization->dataSize != 0U && data_offset == 0U)) {
                    result = -E2BIG;
                    break;
                }
                VkSpecializationInfo *wire_spec =
                    (VkSpecializationInfo *)(blob + spec_offset);
                wire_spec->pMapEntries =
                    general_graphics_blob_offset(entries_offset);
                wire_spec->pData = general_graphics_blob_offset(data_offset);
                wire_stages[index].pSpecializationInfo =
                    general_graphics_blob_offset(spec_offset);
            }
        }
    }
#define BVB_COPY_STATE(root_field, source, type) do { \
    const uint32_t state_offset = BVB_APPEND_OBJECT((source), type); \
    if (state_offset == 0U) result = -E2BIG; \
    else wire_root->root_field = general_graphics_blob_offset(state_offset); \
} while (0)
    if (result == 0) {
        BVB_COPY_STATE(pVertexInputState, vertex,
                       VkPipelineVertexInputStateCreateInfo);
        if (result == 0) {
            VkPipelineVertexInputStateCreateInfo *wire =
                (VkPipelineVertexInputStateCreateInfo *)(blob +
                    (uintptr_t)wire_root->pVertexInputState);
            const uint32_t bindings = BVB_APPEND_ARRAY(
                vertex->pVertexBindingDescriptions,
                vertex->vertexBindingDescriptionCount,
                VkVertexInputBindingDescription);
            const uint32_t attributes = BVB_APPEND_ARRAY(
                vertex->pVertexAttributeDescriptions,
                vertex->vertexAttributeDescriptionCount,
                VkVertexInputAttributeDescription);
            if ((vertex->vertexBindingDescriptionCount != 0U && bindings == 0U) ||
                (vertex->vertexAttributeDescriptionCount != 0U && attributes == 0U))
                result = -E2BIG;
            wire->pNext = NULL;
            wire->pVertexBindingDescriptions =
                general_graphics_blob_offset(bindings);
            wire->pVertexAttributeDescriptions =
                general_graphics_blob_offset(attributes);
        }
    }
    if (result == 0) BVB_COPY_STATE(
        pInputAssemblyState, assembly,
        VkPipelineInputAssemblyStateCreateInfo);
    if (result == 0 && tessellation != NULL)
        BVB_COPY_STATE(pTessellationState, tessellation,
                       VkPipelineTessellationStateCreateInfo);
    else if (wire_root != NULL)
        wire_root->pTessellationState = NULL;
    if (result == 0) {
        BVB_COPY_STATE(pViewportState, viewport,
                       VkPipelineViewportStateCreateInfo);
        if (result == 0) {
            VkPipelineViewportStateCreateInfo *wire =
                (VkPipelineViewportStateCreateInfo *)(blob +
                    (uintptr_t)wire_root->pViewportState);
            const uint32_t viewports = BVB_APPEND_ARRAY(
                viewport->pViewports, viewport->viewportCount, VkViewport);
            const uint32_t scissors = BVB_APPEND_ARRAY(
                viewport->pScissors, viewport->scissorCount, VkRect2D);
            if ((viewport->viewportCount != 0U && viewports == 0U) ||
                (viewport->scissorCount != 0U && scissors == 0U))
                result = -E2BIG;
            wire->pNext = NULL;
            wire->pViewports = general_graphics_blob_offset(viewports);
            wire->pScissors = general_graphics_blob_offset(scissors);
        }
    }
    if (result == 0) {
        BVB_COPY_STATE(pRasterizationState, raster,
                       VkPipelineRasterizationStateCreateInfo);
        if (result == 0) {
            VkPipelineRasterizationStateCreateInfo *wire =
                (VkPipelineRasterizationStateCreateInfo *)(blob +
                    (uintptr_t)wire_root->pRasterizationState);
            wire->pNext = NULL;
            if (depth_clip != NULL) {
                const uint32_t depth_clip_offset = BVB_APPEND_OBJECT(
                    depth_clip,
                    VkPipelineRasterizationDepthClipStateCreateInfoEXT);
                if (depth_clip_offset == 0U) result = -E2BIG;
                else {
                    VkPipelineRasterizationDepthClipStateCreateInfoEXT
                        *wire_depth_clip =
                            (VkPipelineRasterizationDepthClipStateCreateInfoEXT *)(
                                blob + depth_clip_offset);
                    wire_depth_clip->pNext = NULL;
                    wire->pNext = general_graphics_blob_offset(
                        depth_clip_offset);
                }
            }
        }
    }
    if (result == 0) {
        BVB_COPY_STATE(pMultisampleState, multisample,
                       VkPipelineMultisampleStateCreateInfo);
        if (result == 0) {
            VkPipelineMultisampleStateCreateInfo *wire =
                (VkPipelineMultisampleStateCreateInfo *)(blob +
                    (uintptr_t)wire_root->pMultisampleState);
            const uint32_t mask_count =
                ((uint32_t)multisample->rasterizationSamples + 31U) / 32U;
            const uint32_t masks = multisample->pSampleMask == NULL ? 0U :
                BVB_APPEND_ARRAY(multisample->pSampleMask, mask_count,
                                 VkSampleMask);
            if (multisample->pSampleMask != NULL && masks == 0U)
                result = -E2BIG;
            wire->pNext = NULL;
            wire->pSampleMask = general_graphics_blob_offset(masks);
        }
    }
    if (result == 0 && depth != NULL)
        BVB_COPY_STATE(pDepthStencilState, depth,
                       VkPipelineDepthStencilStateCreateInfo);
    else if (wire_root != NULL)
        wire_root->pDepthStencilState = NULL;
    if (result == 0) {
        BVB_COPY_STATE(pColorBlendState, blend,
                       VkPipelineColorBlendStateCreateInfo);
        if (result == 0) {
            VkPipelineColorBlendStateCreateInfo *wire =
                (VkPipelineColorBlendStateCreateInfo *)(blob +
                    (uintptr_t)wire_root->pColorBlendState);
            const uint32_t attachments = BVB_APPEND_ARRAY(
                blend->pAttachments, blend->attachmentCount,
                VkPipelineColorBlendAttachmentState);
            if (blend->attachmentCount != 0U && attachments == 0U)
                result = -E2BIG;
            wire->pNext = NULL;
            wire->pAttachments = general_graphics_blob_offset(attachments);
        }
    }
    if (result == 0) {
        BVB_COPY_STATE(pDynamicState, dynamic,
                       VkPipelineDynamicStateCreateInfo);
        if (result == 0) {
            VkPipelineDynamicStateCreateInfo *wire =
                (VkPipelineDynamicStateCreateInfo *)(blob +
                    (uintptr_t)wire_root->pDynamicState);
            const uint32_t states = BVB_APPEND_ARRAY(
                dynamic->pDynamicStates, dynamic->dynamicStateCount,
                VkDynamicState);
            if (states == 0U) result = -E2BIG;
            wire->pNext = NULL;
            wire->pDynamicStates = general_graphics_blob_offset(states);
        }
    }
#undef BVB_COPY_STATE
#undef BVB_APPEND_ARRAY
#undef BVB_APPEND_OBJECT
    if (result == 0) {
        const uint32_t aligned_size = (builder.size + 7U) & ~7U;
        if (aligned_size > builder.capacity) result = -E2BIG;
        builder.size = aligned_size;
        bvb_wire_put_u32(blob, BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_BLOB_MAGIC);
        bvb_wire_put_u32(blob + 4U,
                         BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_BLOB_VERSION);
        bvb_wire_put_u32(blob + 8U, builder.size);
        bvb_wire_put_u32(blob + 12U, root_offset);
        bvb_wire_put_u32(blob + 16U, sizeof(VkGraphicsPipelineCreateInfo));
        bvb_wire_put_u32(blob + 20U, sizeof(VkPipelineShaderStageCreateInfo));
        bvb_wire_put_u32(blob + 24U, sizeof(VkShaderModuleCreateInfo));
        bvb_wire_put_u32(blob + 28U, sizeof(VkSpecializationInfo));
        bvb_wire_put_u32(blob + 32U, sizeof(VkPipelineVertexInputStateCreateInfo));
        bvb_wire_put_u32(blob + 36U, sizeof(VkPipelineInputAssemblyStateCreateInfo));
        bvb_wire_put_u32(blob + 40U, sizeof(VkPipelineViewportStateCreateInfo));
        bvb_wire_put_u32(blob + 44U, sizeof(VkPipelineRasterizationStateCreateInfo));
        bvb_wire_put_u32(blob + 48U, sizeof(VkPipelineMultisampleStateCreateInfo));
        bvb_wire_put_u32(blob + 52U, sizeof(VkPipelineDepthStencilStateCreateInfo));
        bvb_wire_put_u32(blob + 56U, sizeof(VkPipelineColorBlendStateCreateInfo));
        bvb_wire_put_u32(blob + 60U, sizeof(VkPipelineDynamicStateCreateInfo));
        bvb_wire_put_u32(blob + 64U, sizeof(VkPipelineRenderingCreateInfo));
        bvb_wire_put_u32(blob + 68U, sizeof(VkPipelineTessellationStateCreateInfo));
        bvb_wire_put_u32(blob + 72U, sizeof(VkViewport));
        bvb_wire_put_u32(blob + 76U, sizeof(VkRect2D));
        bvb_wire_put_u32(
            blob + 80U,
            sizeof(VkPipelineRasterizationDepthClipStateCreateInfoEXT));
        bvb_wire_put_u32(blob + 84U, 0U);
    }
    int blob_fd = result == 0
        ? create_sealed_graphics_blob_fd(blob, builder.size) : result;
    free(blob);
    if (blob_fd < 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
    const struct bvb_vulkan_builtin_graphics_pipeline_create_request request = {
        .device_id = device_state->wire_id,
        .pipeline_layout_id = non_dispatchable_wire_id(
            &info->layout, sizeof(info->layout)),
        .blob_bytes = builder.size,
        .schema = BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_BLOB_VERSION,
    };
    uint8_t payload[BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_REQUEST_SIZE];
    result = bvb_protocol_encode_vulkan_builtin_graphics_pipeline_create_request(
        payload, &request);
    struct bvb_resource_proxy *state = calloc(1, sizeof(*state));
    if (state == NULL) result = -ENOMEM;
    if (result != 0 || pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        (void)close(blob_fd);
        free(state);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    struct bvb_resource_proxy *layout_state = resource_proxy_locked(
        request.pipeline_layout_id, BVB_OBJECT_PIPELINE_LAYOUT);
    if (layout_state == NULL || layout_state->parent_id != device_state->wire_id)
        result = -EINVAL;
    uint64_t wire_id = 0U;
    VkResult vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
    if (result == 0)
        vulkan_result = create_resource_fd_locked(
            BVB_OPCODE_VULKAN_BUILTIN_GRAPHICS_PIPELINE_CREATE, payload,
            sizeof(payload), blob_fd, BVB_OBJECT_PIPELINE,
            device_state->wire_id, state, &wire_id);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    (void)close(blob_fd);
    if (vulkan_result != VK_SUCCESS) {
        free(state);
        return vulkan_result;
    }
    memcpy(pipeline, &wire_id, sizeof(*pipeline));
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL bvb_bridge_vkCreateGraphicsPipelines(
    VkDevice device, VkPipelineCache pipeline_cache,
    uint32_t create_info_count,
    const VkGraphicsPipelineCreateInfo *create_infos,
    const VkAllocationCallbacks *allocator, VkPipeline *pipelines) {
    if (pipelines != NULL && create_info_count != 0U)
        pipelines[0] = VK_NULL_HANDLE;
    diagnose_graphics_pipeline_call(
        pipeline_cache, create_info_count, create_infos, allocator);
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || pipeline_cache != VK_NULL_HANDLE ||
        create_info_count != 1U || create_infos == NULL ||
        allocator != NULL || pipelines == NULL) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    const VkGraphicsPipelineCreateInfo *create_info = &create_infos[0];
    if (graphics_pipeline_is_dxvk_builtin_blit(create_info)) {
        return create_dxvk_builtin_blit_pipeline(
            device_state, create_info, &pipelines[0]);
    }
    if (create_info->pNext != NULL &&
        ((const VkBaseInStructure *)create_info->pNext)->sType ==
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
        return create_general_graphics_pipeline(
            device_state, create_info, &pipelines[0]);
    }
    const VkGraphicsPipelineLibraryCreateInfoEXT *library_info =
        create_info->pNext;
    const VkPipelineCreateFlags2CreateInfo *flags_info = library_info == NULL
        ? NULL : library_info->pNext;
    const VkPipelineRenderingCreateInfo *rendering_info = flags_info == NULL
        ? NULL : flags_info->pNext;
    if (create_info->sType != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO ||
        create_info->flags != 0U || library_info == NULL ||
        library_info->sType !=
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT ||
        library_info->flags !=
            VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT ||
        flags_info == NULL ||
        flags_info->sType !=
            VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO ||
        flags_info->flags != VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR ||
        rendering_info == NULL ||
        rendering_info->sType !=
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO ||
        rendering_info->pNext != NULL || rendering_info->viewMask != 0U ||
        rendering_info->colorAttachmentCount != 0U ||
        rendering_info->pColorAttachmentFormats != NULL ||
        rendering_info->depthAttachmentFormat != VK_FORMAT_UNDEFINED ||
        rendering_info->stencilAttachmentFormat != VK_FORMAT_UNDEFINED ||
        create_info->stageCount != 1U || create_info->pStages == NULL ||
        create_info->pVertexInputState != NULL ||
        create_info->pInputAssemblyState != NULL ||
        create_info->pTessellationState != NULL ||
        create_info->pViewportState != NULL ||
        create_info->pRasterizationState != NULL ||
        create_info->pMultisampleState != NULL ||
        !depth_stencil_state_is_dxvk_null_fragment(
            create_info->pDepthStencilState) ||
        create_info->pColorBlendState != NULL ||
        !dynamic_state_is_dxvk_null_fragment(create_info->pDynamicState) ||
        create_info->layout == VK_NULL_HANDLE ||
        create_info->renderPass != VK_NULL_HANDLE ||
        create_info->subpass != 0U ||
        create_info->basePipelineHandle != VK_NULL_HANDLE ||
        create_info->basePipelineIndex != -1) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    const VkPipelineShaderStageCreateInfo *stage_info = &create_info->pStages[0];
    const VkShaderModuleCreateInfo *module_info = stage_info->pNext;
    if (stage_info->sType !=
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO ||
        stage_info->flags != 0U ||
        stage_info->stage != VK_SHADER_STAGE_FRAGMENT_BIT ||
        stage_info->module != VK_NULL_HANDLE || stage_info->pName == NULL ||
        strcmp(stage_info->pName, "main") != 0 ||
        stage_info->pSpecializationInfo != NULL || module_info == NULL ||
        module_info->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO ||
        module_info->pNext != NULL || module_info->flags != 0U ||
        module_info->codeSize < 5U * sizeof(uint32_t) ||
        module_info->codeSize % sizeof(uint32_t) != 0U ||
        module_info->codeSize >
            BVB_VULKAN_MAX_GRAPHICS_PIPELINE_SHADER_WORDS * sizeof(uint32_t) ||
        module_info->pCode == NULL ||
        module_info->pCode[0] != UINT32_C(0x07230203)) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    struct bvb_vulkan_graphics_pipeline_create_request decoded = {
        .device_id = device_state->wire_id,
        .pipeline_layout_id = non_dispatchable_wire_id(
            &create_info->layout, sizeof(create_info->layout)),
        .flags_2 = flags_info->flags,
        .library_flags = library_info->flags,
        .shader_stage = stage_info->stage,
        .dynamic_state_count = create_info->pDynamicState->dynamicStateCount,
        .shader_word_count =
            (uint32_t)(module_info->codeSize / sizeof(uint32_t)),
    };
    for (uint32_t index = 0U; index < decoded.dynamic_state_count; ++index) {
        decoded.dynamic_states[index] =
            create_info->pDynamicState->pDynamicStates[index];
    }
    memcpy(decoded.shader_words, module_info->pCode, module_info->codeSize);
    uint8_t payload[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t payload_length = 0U;
    int result = bvb_protocol_encode_vulkan_graphics_pipeline_create_request(
        payload, &decoded, &payload_length);
    struct bvb_resource_proxy *state = calloc(1, sizeof(*state));
    if (state == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (result != 0 || pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        free(state);
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    struct bvb_resource_proxy *layout_state = resource_proxy_locked(
        decoded.pipeline_layout_id, BVB_OBJECT_PIPELINE_LAYOUT);
    if (layout_state == NULL ||
        layout_state->parent_id != device_state->wire_id) result = -EINVAL;
    uint64_t wire_id = 0U;
    VkResult vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
    if (result == 0) {
        vulkan_result = create_resource_locked(
            BVB_OPCODE_VULKAN_GRAPHICS_PIPELINE_CREATE, payload,
            payload_length, BVB_OBJECT_PIPELINE, device_state->wire_id,
            state, &wire_id);
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (vulkan_result != VK_SUCCESS) {
        free(state);
        return vulkan_result;
    }
    memcpy(&pipelines[0], &wire_id, sizeof(pipelines[0]));
    return VK_SUCCESS;
}

static void VKAPI_CALL bvb_bridge_vkDestroyPipeline(
    VkDevice device, VkPipeline pipeline,
    const VkAllocationCallbacks *allocator) {
    destroy_resource(
        device, non_dispatchable_wire_id(&pipeline, sizeof(pipeline)),
        BVB_OBJECT_PIPELINE, BVB_OPCODE_VULKAN_PIPELINE_DESTROY, allocator);
}

static VkResult VKAPI_CALL bvb_bridge_vkCreateBuffer(
    VkDevice device, const VkBufferCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkBuffer *buffer) {
    if (buffer != NULL) *buffer = VK_NULL_HANDLE;
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || create_info == NULL || buffer == NULL ||
        allocator != NULL || create_info->sType != VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO ||
        create_info->pNext != NULL || create_info->flags != 0U ||
        create_info->size == 0U ||
        create_info->size > BVB_VULKAN_MAX_MEMORY_ALLOCATION_SIZE ||
        (create_info->usage != VK_BUFFER_USAGE_TRANSFER_DST_BIT &&
         create_info->usage != VK_BUFFER_USAGE_TRANSFER_SRC_BIT &&
         (((create_info->usage &
            (VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
             VK_BUFFER_USAGE_TRANSFER_DST_BIT |
             VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) !=
           (VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) ||
          (create_info->usage &
           ~(VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
             VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
             VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
             VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
             VK_BUFFER_USAGE_TRANSFER_DST_BIT |
             VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
             VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT |
             VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT)) !=
              0U)) ||
        create_info->sharingMode != VK_SHARING_MODE_EXCLUSIVE ||
        create_info->queueFamilyIndexCount != 0U) return VK_ERROR_FEATURE_NOT_PRESENT;
    struct bvb_resource_proxy *state = calloc(1, sizeof(*state));
    if (state == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
    const struct bvb_vulkan_buffer_create_request decoded = {
        .device_id = device_state->wire_id,
        .size = create_info->size,
        .usage = create_info->usage,
        .flags = create_info->flags |
            (bvb_global_client.memory_direct_enabled
                 ? BVB_VULKAN_BUFFER_CREATE_EXPORT_OPAQUE_FD : 0U),
    };
    state->buffer_usage = create_info->usage;
    uint8_t payload[BVB_VULKAN_BUFFER_CREATE_REQUEST_SIZE];
    int result = bvb_protocol_encode_vulkan_buffer_create_request(
        payload, &decoded);
    if (result != 0 || pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        free(state);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    uint64_t wire_id = 0U;
    VkResult vulkan_result = create_resource_locked(
        BVB_OPCODE_VULKAN_BUFFER_CREATE, payload, sizeof(payload),
        BVB_OBJECT_BUFFER, device_state->wire_id, state, &wire_id);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (vulkan_result != VK_SUCCESS) {
        free(state);
        return vulkan_result;
    }
    memcpy(buffer, &wire_id, sizeof(*buffer));
    return VK_SUCCESS;
}

static void destroy_resource(
    VkDevice device, uint64_t wire_id, enum bvb_object_type type,
    uint16_t opcode, const VkAllocationCallbacks *allocator) {
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || allocator != NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) return;
    struct bvb_resource_proxy *state = resource_proxy_locked(wire_id, type);
    int result = state != NULL && state->parent_id == device_state->wire_id
                     ? connect_locked() : -EINVAL;
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = opcode,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_OBJECT_ID_SIZE,
    };
    if (result == 0)
        result = bvb_protocol_encode_vulkan_object_id(
            request.payload, wire_id, (uint8_t)type);
    struct bvb_protocol_packet response = {0};
    if (result == 0) result = exchange_locked(&request, &response);
    if (result == 0 &&
        (response.header.status != 0 || response.header.payload_length != 0U))
        result = -EPROTO;
    if (result == 0) {
        if (pthread_rwlock_wrlock(&bvb_object_registry_lock) != 0) {
            result = -EDEADLK;
        }
    }
    if (result == 0) {
        if (type == BVB_OBJECT_DESCRIPTOR_POOL) {
            remove_descriptor_sets_for_pool_locked(wire_id);
        }
        remove_resource_proxy_locked(state);
        (void)pthread_rwlock_unlock(&bvb_object_registry_lock);
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
}

static void VKAPI_CALL bvb_bridge_vkDestroyBuffer(
    VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *allocator) {
    destroy_resource(
        device, non_dispatchable_wire_id(&buffer, sizeof(buffer)),
        BVB_OBJECT_BUFFER, BVB_OPCODE_VULKAN_BUFFER_DESTROY, allocator);
}

static void VKAPI_CALL bvb_bridge_vkGetBufferMemoryRequirements(
    VkDevice device, VkBuffer buffer, VkMemoryRequirements *requirements) {
    if (requirements == NULL) return;
    *requirements = (VkMemoryRequirements){0};
    struct bvb_device_proxy *device_state = device_proxy(device);
    const uint64_t wire_id = non_dispatchable_wire_id(&buffer, sizeof(buffer));
    if (device_state == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) return;
    struct bvb_resource_proxy *state =
        resource_proxy_locked(wire_id, BVB_OBJECT_BUFFER);
    int result = state != NULL && state->parent_id == device_state->wire_id
                     ? connect_locked() : -EINVAL;
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_BUFFER_REQUIREMENTS,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_OBJECT_ID_SIZE,
    };
    if (result == 0)
        result = bvb_protocol_encode_vulkan_object_id(
            request.payload, wire_id, BVB_OBJECT_BUFFER);
    struct bvb_protocol_packet response = {0};
    if (result == 0) result = exchange_locked(&request, &response);
    if (result == 0 &&
        (response.header.status != 0 ||
         response.header.payload_length != BVB_VULKAN_BUFFER_REQUIREMENTS_SIZE))
        result = -EPROTO;
    struct bvb_vulkan_buffer_requirements decoded = {0};
    if (result == 0)
        result = bvb_protocol_decode_vulkan_buffer_requirements(
            response.payload, &decoded);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result == 0) {
        requirements->size = decoded.size;
        requirements->alignment = decoded.alignment;
        requirements->memoryTypeBits = decoded.memory_type_bits;
    }
}

static VkResult encode_memory_allocate_pnext(
    const void *pnext,
    struct bvb_vulkan_memory_allocate_extended_request *request) {
    const VkBaseInStructure *next = pnext;
    uint32_t count = 0U;
    while (next != NULL) {
        if (++count > 2U) return VK_ERROR_FEATURE_NOT_PRESENT;
        if (next->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) {
            const VkMemoryDedicatedAllocateInfo *dedicated =
                (const VkMemoryDedicatedAllocateInfo *)next;
            if ((request->pnext_flags &
                 BVB_VULKAN_MEMORY_ALLOCATE_PNEXT_DEDICATED_IMAGE) != 0U ||
                dedicated->buffer != VK_NULL_HANDLE ||
                dedicated->image == VK_NULL_HANDLE) {
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            request->pnext_flags |=
                BVB_VULKAN_MEMORY_ALLOCATE_PNEXT_DEDICATED_IMAGE;
            request->dedicated_image_id = non_dispatchable_wire_id(
                &dedicated->image, sizeof(dedicated->image));
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO) {
            const VkMemoryAllocateFlagsInfo *flags =
                (const VkMemoryAllocateFlagsInfo *)next;
            if ((request->pnext_flags &
                 BVB_VULKAN_MEMORY_ALLOCATE_PNEXT_FLAGS) != 0U ||
                flags->flags != VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT ||
                flags->deviceMask != 0U) {
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            request->pnext_flags |=
                BVB_VULKAN_MEMORY_ALLOCATE_PNEXT_FLAGS;
            request->allocation_flags = flags->flags;
            request->device_mask = flags->deviceMask;
        } else {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        next = next->pNext;
    }
    return VK_SUCCESS;
}

static int device_buffer_create_info_supported(
    const VkBufferCreateInfo *create_info) {
    if (create_info == NULL ||
        create_info->sType != VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO ||
        create_info->pNext != NULL || create_info->size == 0U ||
        create_info->usage == 0U)
        return 0;
    if (create_info->sharingMode == VK_SHARING_MODE_EXCLUSIVE)
        return create_info->queueFamilyIndexCount == 0U;
    if (create_info->sharingMode != VK_SHARING_MODE_CONCURRENT ||
        create_info->queueFamilyIndexCount < 2U ||
        create_info->queueFamilyIndexCount >
            BVB_VULKAN_DEVICE_BUFFER_MAX_QUEUE_FAMILIES ||
        create_info->pQueueFamilyIndices == NULL)
        return 0;
    for (uint32_t index = 0U; index < create_info->queueFamilyIndexCount;
         ++index)
        for (uint32_t earlier = 0U; earlier < index; ++earlier)
            if (create_info->pQueueFamilyIndices[earlier] ==
                create_info->pQueueFamilyIndices[index])
                return 0;
    return 1;
}

static int device_buffer_dedicated_output(
    VkMemoryRequirements2 *requirements,
    VkMemoryDedicatedRequirements **dedicated) {
    *dedicated = NULL;
    if (requirements->pNext == NULL) return 0;
    VkMemoryDedicatedRequirements *candidate =
        (VkMemoryDedicatedRequirements *)requirements->pNext;
    if (candidate->sType !=
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS ||
        candidate->pNext != NULL)
        return -1;
    *dedicated = candidate;
    return 0;
}

static void VKAPI_CALL bvb_bridge_vkGetDeviceBufferMemoryRequirements(
    VkDevice device, const VkDeviceBufferMemoryRequirements *info,
    VkMemoryRequirements2 *requirements) {
    if (requirements == NULL) return;
    requirements->memoryRequirements = (VkMemoryRequirements){0};
    if (requirements->sType != VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2)
        return;
    VkMemoryDedicatedRequirements *dedicated = NULL;
    if (device_buffer_dedicated_output(requirements, &dedicated) != 0)
        return;
    if (dedicated != NULL) {
        dedicated->prefersDedicatedAllocation = VK_FALSE;
        dedicated->requiresDedicatedAllocation = VK_FALSE;
    }
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || info == NULL ||
        info->sType != VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS ||
        info->pNext != NULL ||
        !device_buffer_create_info_supported(info->pCreateInfo))
        return;
    const VkBufferCreateInfo *create_info = info->pCreateInfo;
    struct bvb_vulkan_device_buffer_requirements_request decoded = {
        .device_id = device_state->wire_id,
        .size = create_info->size,
        .flags = create_info->flags,
        .usage = create_info->usage,
        .sharing_mode = create_info->sharingMode,
        .queue_family_index_count = create_info->queueFamilyIndexCount,
    };
    for (uint32_t index = 0U; index < create_info->queueFamilyIndexCount;
         ++index)
        decoded.queue_family_indices[index] =
            create_info->pQueueFamilyIndices[index];
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_DEVICE_BUFFER_REQUIREMENTS,
        .payload_length =
            BVB_VULKAN_DEVICE_BUFFER_REQUIREMENTS_REQUEST_SIZE,
    };
    int result =
        bvb_protocol_encode_vulkan_device_buffer_requirements_request(
            request.payload, &decoded);
    if (result != 0 || pthread_mutex_lock(&bvb_global_client.mutex) != 0)
        return;
    request.header.request_id = next_request_id_locked();
    result = connect_locked();
    struct bvb_protocol_packet response = {0};
    if (result == 0) result = exchange_locked(&request, &response);
    if (result == 0 &&
        (response.header.status != 0 ||
         response.header.payload_length !=
             BVB_VULKAN_DEVICE_BUFFER_REQUIREMENTS_RESPONSE_SIZE))
        result = -EPROTO;
    struct bvb_vulkan_device_buffer_requirements_response native = {0};
    if (result == 0)
        result =
            bvb_protocol_decode_vulkan_device_buffer_requirements_response(
                response.payload, &native);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result != 0) return;
    requirements->memoryRequirements = (VkMemoryRequirements){
        .size = native.memory.size,
        .alignment = native.memory.alignment,
        .memoryTypeBits = native.memory.memory_type_bits,
    };
    if (dedicated != NULL) {
        dedicated->prefersDedicatedAllocation =
            native.prefers_dedicated_allocation != 0U ? VK_TRUE : VK_FALSE;
        dedicated->requiresDedicatedAllocation =
            native.requires_dedicated_allocation != 0U ? VK_TRUE : VK_FALSE;
    }
}

static void VKAPI_CALL bvb_bridge_vkGetBufferMemoryRequirements2(
    VkDevice device, const VkBufferMemoryRequirementsInfo2 *info,
    VkMemoryRequirements2 *requirements) {
    if (requirements == NULL) return;
    requirements->memoryRequirements = (VkMemoryRequirements){0};
    VkMemoryDedicatedRequirements *dedicated = NULL;
    if (requirements->pNext != NULL) {
        VkBaseOutStructure *next = requirements->pNext;
        if (next->sType != VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS)
            return;
        dedicated = (VkMemoryDedicatedRequirements *)next;
        dedicated->prefersDedicatedAllocation = VK_FALSE;
        dedicated->requiresDedicatedAllocation = VK_FALSE;
        if (next->pNext != NULL) return;
    }
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || info == NULL ||
        info->sType != VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2 ||
        info->pNext != NULL ||
        requirements->sType != VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2) {
        return;
    }
    const uint64_t buffer_id = non_dispatchable_wire_id(
        &info->buffer, sizeof(info->buffer));
    const struct bvb_vulkan_buffer_requirements_2_request decoded = {
        .buffer_id = buffer_id,
        .pnext_flags = dedicated != NULL
            ? BVB_VULKAN_BUFFER_REQUIREMENTS_2_PNEXT_DEDICATED : 0U,
    };
    uint8_t payload[BVB_VULKAN_BUFFER_REQUIREMENTS_2_REQUEST_SIZE];
    int result = bvb_protocol_encode_vulkan_buffer_requirements_2_request(
        payload, &decoded);
    if (result != 0 || pthread_mutex_lock(&bvb_global_client.mutex) != 0)
        return;
    struct bvb_resource_proxy *buffer_state = resource_proxy_locked(
        buffer_id, BVB_OBJECT_BUFFER);
    if (buffer_state == NULL ||
        buffer_state->parent_id != device_state->wire_id) {
        result = -EINVAL;
    } else {
        result = connect_locked();
    }
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_BUFFER_REQUIREMENTS_2,
        .request_id = next_request_id_locked(),
        .payload_length = sizeof(payload),
    };
    if (result == 0) memcpy(request.payload, payload, sizeof(payload));
    struct bvb_protocol_packet response = {0};
    if (result == 0) result = exchange_locked(&request, &response);
    if (result == 0 &&
        (response.header.status != 0 ||
         response.header.payload_length !=
             BVB_VULKAN_BUFFER_REQUIREMENTS_2_RESPONSE_SIZE)) {
        result = -EPROTO;
    }
    struct bvb_vulkan_buffer_requirements_2_response wire = {0};
    if (result == 0)
        result = bvb_protocol_decode_vulkan_buffer_requirements_2_response(
            response.payload, &wire);
    if (result == 0 && wire.pnext_flags != decoded.pnext_flags)
        result = -EPROTO;
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result != 0) return;
    requirements->memoryRequirements = (VkMemoryRequirements){
        .size = wire.size,
        .alignment = wire.alignment,
        .memoryTypeBits = wire.memory_type_bits,
    };
    if (dedicated != NULL) {
        dedicated->prefersDedicatedAllocation =
            (VkBool32)wire.prefers_dedicated;
        dedicated->requiresDedicatedAllocation =
            (VkBool32)wire.requires_dedicated;
    }
}

static VkDeviceAddress VKAPI_CALL bvb_bridge_vkGetBufferDeviceAddress(
    VkDevice device, const VkBufferDeviceAddressInfo *info) {
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || info == NULL ||
        info->sType != VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO ||
        info->pNext != NULL) {
        return 0U;
    }
    const uint64_t buffer_id = non_dispatchable_wire_id(
        &info->buffer, sizeof(info->buffer));
    const struct bvb_vulkan_buffer_device_address_request decoded = {
        .buffer_id = buffer_id,
    };
    uint8_t payload[BVB_VULKAN_BUFFER_DEVICE_ADDRESS_REQUEST_SIZE];
    int result = bvb_protocol_encode_vulkan_buffer_device_address_request(
        payload, &decoded);
    if (result != 0 || pthread_mutex_lock(&bvb_global_client.mutex) != 0)
        return 0U;
    struct bvb_resource_proxy *buffer_state = resource_proxy_locked(
        buffer_id, BVB_OBJECT_BUFFER);
    if (buffer_state == NULL ||
        buffer_state->parent_id != device_state->wire_id ||
        !buffer_state->memory_bound ||
        (buffer_state->buffer_usage &
         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) == 0U) {
        result = -EINVAL;
    } else {
        result = connect_locked();
    }
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_BUFFER_DEVICE_ADDRESS,
        .request_id = next_request_id_locked(),
        .payload_length = sizeof(payload),
    };
    if (result == 0) memcpy(request.payload, payload, sizeof(payload));
    struct bvb_protocol_packet response = {0};
    if (result == 0) result = exchange_locked(&request, &response);
    if (result == 0 &&
        (response.header.status != 0 ||
         response.header.payload_length !=
             BVB_VULKAN_BUFFER_DEVICE_ADDRESS_RESPONSE_SIZE)) {
        result = -EPROTO;
    }
    struct bvb_vulkan_buffer_device_address_response wire = {0};
    if (result == 0)
        result = bvb_protocol_decode_vulkan_buffer_device_address_response(
            response.payload, &wire);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return result == 0 ? (VkDeviceAddress)wire.device_address : 0U;
}

static VkResult VKAPI_CALL bvb_bridge_vkAllocateMemory(
    VkDevice device, const VkMemoryAllocateInfo *allocate_info,
    const VkAllocationCallbacks *allocator, VkDeviceMemory *memory) {
    if (memory != NULL) *memory = VK_NULL_HANDLE;
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || allocate_info == NULL || memory == NULL ||
        allocator != NULL ||
        allocate_info->sType != VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO ||
        allocate_info->allocationSize == 0U ||
        allocate_info->allocationSize > BVB_VULKAN_MAX_MEMORY_ALLOCATION_SIZE)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    struct bvb_resource_proxy *state = calloc(1, sizeof(*state));
    if (state == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct bvb_vulkan_memory_allocate_extended_request decoded = {
        .device_id = device_state->wire_id,
        .allocation_size = allocate_info->allocationSize,
        .memory_type_index = allocate_info->memoryTypeIndex,
    };
    VkResult vulkan_result = encode_memory_allocate_pnext(
        allocate_info->pNext, &decoded);
    bool memory_shared = false;
    bool memory_direct = false;
    if (vulkan_result == VK_SUCCESS &&
        mapped_memory_mode_requested(&memory_shared, &memory_direct) != 0)
        vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
    if (vulkan_result == VK_SUCCESS && memory_direct)
        decoded.pnext_flags |=
            BVB_VULKAN_MEMORY_ALLOCATE_PNEXT_EXPORT_OPAQUE_FD;
    uint8_t payload[BVB_VULKAN_MEMORY_ALLOCATE_EXTENDED_REQUEST_SIZE];
    int result = vulkan_result == VK_SUCCESS
        ? bvb_protocol_encode_vulkan_memory_allocate_extended_request(
              payload, &decoded)
        : -EINVAL;
    if (result != 0 || pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        free(state);
        return vulkan_result != VK_SUCCESS
            ? vulkan_result : VK_ERROR_INITIALIZATION_FAILED;
    }
    if ((decoded.pnext_flags &
         BVB_VULKAN_MEMORY_ALLOCATE_PNEXT_DEDICATED_IMAGE) != 0U) {
        struct bvb_resource_proxy *image_state = resource_proxy_locked(
            decoded.dedicated_image_id, BVB_OBJECT_IMAGE);
        if (image_state == NULL ||
            image_state->parent_id != device_state->wire_id) {
            (void)pthread_mutex_unlock(&bvb_global_client.mutex);
            free(state);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    state->allocation_size = allocate_info->allocationSize;
    uint64_t wire_id = 0U;
    vulkan_result = create_resource_locked(
        BVB_OPCODE_VULKAN_MEMORY_ALLOCATE_EXTENDED, payload, sizeof(payload),
        BVB_OBJECT_DEVICE_MEMORY, device_state->wire_id, state, &wire_id);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (vulkan_result != VK_SUCCESS) {
        free(state);
        return vulkan_result;
    }
    memcpy(memory, &wire_id, sizeof(*memory));
    return VK_SUCCESS;
}

static void VKAPI_CALL bvb_bridge_vkUnmapMemory(
    VkDevice device, VkDeviceMemory memory);

static void VKAPI_CALL bvb_bridge_vkFreeMemory(
    VkDevice device, VkDeviceMemory memory,
    const VkAllocationCallbacks *allocator) {
    bvb_bridge_vkUnmapMemory(device, memory);
    destroy_resource(
        device, non_dispatchable_wire_id(&memory, sizeof(memory)),
        BVB_OBJECT_DEVICE_MEMORY, BVB_OPCODE_VULKAN_MEMORY_FREE, allocator);
}

static VkResult VKAPI_CALL bvb_bridge_vkBindBufferMemory(
    VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
    VkDeviceSize memory_offset) {
    struct bvb_device_proxy *device_state = device_proxy(device);
    const uint64_t buffer_id = non_dispatchable_wire_id(&buffer, sizeof(buffer));
    const uint64_t memory_id = non_dispatchable_wire_id(&memory, sizeof(memory));
    if (device_state == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct bvb_resource_proxy *buffer_state =
        resource_proxy_locked(buffer_id, BVB_OBJECT_BUFFER);
    struct bvb_resource_proxy *memory_state =
        resource_proxy_locked(memory_id, BVB_OBJECT_DEVICE_MEMORY);
    VkResult vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    if (buffer_state != NULL && memory_state != NULL &&
        buffer_state->parent_id == device_state->wire_id &&
        memory_state->parent_id == device_state->wire_id) {
        const struct bvb_vulkan_buffer_bind_request decoded = {
            .buffer_id = buffer_id,
            .memory_id = memory_id,
            .offset = memory_offset,
        };
        uint8_t payload[BVB_VULKAN_BUFFER_BIND_REQUEST_SIZE];
        int result = bvb_protocol_encode_vulkan_buffer_bind_request(
            payload, &decoded);
        if (result == 0) {
            vulkan_result = result_request_locked(
                BVB_OPCODE_VULKAN_BUFFER_BIND, payload, sizeof(payload));
            if (vulkan_result == VK_SUCCESS) {
                buffer_state->memory_bound = true;
                buffer_state->bound_memory_id = memory_id;
            }
        }
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return vulkan_result;
}

static VkResult encode_image_create_pnext(
    const void *pnext, struct bvb_vulkan_image_create_request *request) {
    const VkBaseInStructure *next = pnext;
    uint32_t count = 0U;
    while (next != NULL) {
        if (++count > 2U) return VK_ERROR_FEATURE_NOT_PRESENT;
        if (next->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
            const VkImageFormatListCreateInfo *format_list =
                (const VkImageFormatListCreateInfo *)next;
            if ((request->pnext_flags &
                 BVB_VULKAN_IMAGE_CREATE_PNEXT_FORMAT_LIST) != 0U ||
                format_list->viewFormatCount >
                    BVB_VULKAN_IMAGE_MAX_VIEW_FORMATS ||
                (format_list->viewFormatCount != 0U &&
                 format_list->pViewFormats == NULL)) {
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            request->pnext_flags |=
                BVB_VULKAN_IMAGE_CREATE_PNEXT_FORMAT_LIST;
            request->view_format_count = format_list->viewFormatCount;
            for (uint32_t index = 0U;
                 index < format_list->viewFormatCount; ++index) {
                request->view_formats[index] =
                    (uint32_t)format_list->pViewFormats[index];
            }
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO) {
            const VkImageStencilUsageCreateInfo *stencil_usage =
                (const VkImageStencilUsageCreateInfo *)next;
            if ((request->pnext_flags &
                 BVB_VULKAN_IMAGE_CREATE_PNEXT_STENCIL_USAGE) != 0U) {
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            request->pnext_flags |=
                BVB_VULKAN_IMAGE_CREATE_PNEXT_STENCIL_USAGE;
            request->stencil_usage = stencil_usage->stencilUsage;
        } else {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        next = next->pNext;
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL bvb_bridge_vkCreateImage(
    VkDevice device, const VkImageCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkImage *image) {
    if (image != NULL) *image = VK_NULL_HANDLE;
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || create_info == NULL || image == NULL ||
        allocator != NULL ||
        create_info->sType != VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO ||
        create_info->queueFamilyIndexCount >
            BVB_VULKAN_IMAGE_MAX_QUEUE_FAMILIES ||
        (create_info->queueFamilyIndexCount != 0U &&
         create_info->pQueueFamilyIndices == NULL)) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    struct bvb_vulkan_image_create_request decoded = {
        .device_id = device_state->wire_id,
        .flags = create_info->flags,
        .image_type = create_info->imageType,
        .format = create_info->format,
        .extent_width = create_info->extent.width,
        .extent_height = create_info->extent.height,
        .extent_depth = create_info->extent.depth,
        .mip_levels = create_info->mipLevels,
        .array_layers = create_info->arrayLayers,
        .samples = create_info->samples,
        .tiling = create_info->tiling,
        .usage = create_info->usage,
        .sharing_mode = create_info->sharingMode,
        .queue_family_index_count = create_info->queueFamilyIndexCount,
        .initial_layout = create_info->initialLayout,
    };
    for (uint32_t index = 0U;
         index < create_info->queueFamilyIndexCount; ++index) {
        decoded.queue_family_indices[index] =
            create_info->pQueueFamilyIndices[index];
    }
    VkResult vulkan_result = encode_image_create_pnext(
        create_info->pNext, &decoded);
    uint8_t payload[BVB_VULKAN_IMAGE_CREATE_REQUEST_SIZE];
    if (vulkan_result == VK_SUCCESS &&
        bvb_protocol_encode_vulkan_image_create_request(
            payload, &decoded) != 0) {
        vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (vulkan_result != VK_SUCCESS) return vulkan_result;
    struct bvb_resource_proxy *state = calloc(1, sizeof(*state));
    if (state == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        free(state);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    uint64_t wire_id = 0U;
    vulkan_result = create_resource_locked(
        BVB_OPCODE_VULKAN_IMAGE_CREATE, payload, sizeof(payload),
        BVB_OBJECT_IMAGE, device_state->wire_id, state, &wire_id);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (vulkan_result != VK_SUCCESS) {
        free(state);
        return vulkan_result;
    }
    memcpy(image, &wire_id, sizeof(*image));
    return VK_SUCCESS;
}

static void VKAPI_CALL bvb_bridge_vkDestroyImage(
    VkDevice device, VkImage image, const VkAllocationCallbacks *allocator) {
    destroy_resource(
        device, non_dispatchable_wire_id(&image, sizeof(image)),
        BVB_OBJECT_IMAGE, BVB_OPCODE_VULKAN_IMAGE_DESTROY, allocator);
}

static void VKAPI_CALL bvb_bridge_vkGetImageMemoryRequirements(
    VkDevice device, VkImage image, VkMemoryRequirements *requirements) {
    if (requirements == NULL) return;
    *requirements = (VkMemoryRequirements){0};
    struct bvb_device_proxy *device_state = device_proxy(device);
    const uint64_t wire_id = non_dispatchable_wire_id(&image, sizeof(image));
    if (device_state == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) return;
    struct bvb_resource_proxy *state =
        resource_proxy_locked(wire_id, BVB_OBJECT_IMAGE);
    int result = state != NULL && state->parent_id == device_state->wire_id
                     ? connect_locked() : -EINVAL;
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_IMAGE_REQUIREMENTS,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_OBJECT_ID_SIZE,
    };
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_object_id(
            request.payload, wire_id, BVB_OBJECT_IMAGE);
    }
    struct bvb_protocol_packet response = {0};
    if (result == 0) result = exchange_locked(&request, &response);
    if (result == 0 &&
        (response.header.status != 0 ||
         response.header.payload_length != BVB_VULKAN_IMAGE_REQUIREMENTS_SIZE)) {
        result = -EPROTO;
    }
    struct bvb_vulkan_image_requirements decoded = {0};
    if (result == 0) {
        result = bvb_protocol_decode_vulkan_image_requirements(
            response.payload, &decoded);
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result == 0) {
        requirements->size = decoded.size;
        requirements->alignment = decoded.alignment;
        requirements->memoryTypeBits = decoded.memory_type_bits;
    }
}

static void VKAPI_CALL bvb_bridge_vkGetImageMemoryRequirements2(
    VkDevice device, const VkImageMemoryRequirementsInfo2 *info,
    VkMemoryRequirements2 *requirements) {
    if (requirements == NULL) return;
    requirements->memoryRequirements = (VkMemoryRequirements){0};
    VkMemoryDedicatedRequirements *dedicated = NULL;
    if (requirements->pNext != NULL) {
        VkBaseOutStructure *next = requirements->pNext;
        if (next->sType != VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
            return;
        }
        dedicated = (VkMemoryDedicatedRequirements *)next;
        dedicated->prefersDedicatedAllocation = VK_FALSE;
        dedicated->requiresDedicatedAllocation = VK_FALSE;
        if (next->pNext != NULL) return;
    }
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || info == NULL ||
        info->sType != VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2 ||
        info->pNext != NULL ||
        requirements->sType != VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2) {
        return;
    }
    const uint64_t image_id = non_dispatchable_wire_id(
        &info->image, sizeof(info->image));
    const struct bvb_vulkan_image_requirements_2_request decoded = {
        .image_id = image_id,
        .pnext_flags = dedicated != NULL
            ? BVB_VULKAN_IMAGE_REQUIREMENTS_2_PNEXT_DEDICATED : 0U,
    };
    uint8_t payload[BVB_VULKAN_IMAGE_REQUIREMENTS_2_REQUEST_SIZE];
    int result = bvb_protocol_encode_vulkan_image_requirements_2_request(
        payload, &decoded);
    if (result != 0 || pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return;
    }
    struct bvb_resource_proxy *image_state = resource_proxy_locked(
        image_id, BVB_OBJECT_IMAGE);
    if (image_state == NULL ||
        image_state->parent_id != device_state->wire_id) {
        result = -EINVAL;
    } else {
        result = connect_locked();
    }
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_IMAGE_REQUIREMENTS_2,
        .request_id = next_request_id_locked(),
        .payload_length = sizeof(payload),
    };
    if (result == 0) memcpy(request.payload, payload, sizeof(payload));
    struct bvb_protocol_packet response = {0};
    if (result == 0) result = exchange_locked(&request, &response);
    if (result == 0 &&
        (response.header.status != 0 ||
         response.header.payload_length !=
             BVB_VULKAN_IMAGE_REQUIREMENTS_2_RESPONSE_SIZE)) {
        result = -EPROTO;
    }
    struct bvb_vulkan_image_requirements_2_response wire = {0};
    if (result == 0) {
        result = bvb_protocol_decode_vulkan_image_requirements_2_response(
            response.payload, &wire);
    }
    if (result == 0 && wire.pnext_flags != decoded.pnext_flags) {
        result = -EPROTO;
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result != 0) return;
    requirements->memoryRequirements = (VkMemoryRequirements){
        .size = wire.size,
        .alignment = wire.alignment,
        .memoryTypeBits = wire.memory_type_bits,
    };
    if (dedicated != NULL) {
        dedicated->prefersDedicatedAllocation =
            (VkBool32)wire.prefers_dedicated;
        dedicated->requiresDedicatedAllocation =
            (VkBool32)wire.requires_dedicated;
    }
}

static VkResult VKAPI_CALL bvb_bridge_vkBindImageMemory(
    VkDevice device, VkImage image, VkDeviceMemory memory,
    VkDeviceSize memory_offset) {
    struct bvb_device_proxy *device_state = device_proxy(device);
    const uint64_t image_id = non_dispatchable_wire_id(&image, sizeof(image));
    const uint64_t memory_id = non_dispatchable_wire_id(&memory, sizeof(memory));
    if (device_state == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    struct bvb_resource_proxy *image_state =
        resource_proxy_locked(image_id, BVB_OBJECT_IMAGE);
    struct bvb_resource_proxy *memory_state =
        resource_proxy_locked(memory_id, BVB_OBJECT_DEVICE_MEMORY);
    VkResult vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    if (image_state != NULL && memory_state != NULL &&
        image_state->parent_id == device_state->wire_id &&
        memory_state->parent_id == device_state->wire_id &&
        !memory_state->mapped_shared) {
        const struct bvb_vulkan_image_bind_request decoded = {
            .image_id = image_id,
            .memory_id = memory_id,
            .offset = memory_offset,
        };
        uint8_t payload[BVB_VULKAN_IMAGE_BIND_REQUEST_SIZE];
        const int result = bvb_protocol_encode_vulkan_image_bind_request(
            payload, &decoded);
        if (result == 0) {
            vulkan_result = result_request_locked(
                BVB_OPCODE_VULKAN_IMAGE_BIND, payload, sizeof(payload));
            if (vulkan_result == VK_SUCCESS)
                image_state->bound_memory_id = memory_id;
        }
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return vulkan_result;
}

static VkResult encode_image_view_create_pnext(
    const void *pnext,
    struct bvb_vulkan_image_view_create_request *request) {
    if (pnext == NULL) return VK_SUCCESS;
    const VkBaseInStructure *base = pnext;
    if (base->sType != VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO ||
        base->pNext != NULL) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    const VkImageViewUsageCreateInfo *usage =
        (const VkImageViewUsageCreateInfo *)base;
    request->pnext_flags = BVB_VULKAN_IMAGE_VIEW_CREATE_PNEXT_USAGE;
    request->usage = usage->usage;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL bvb_bridge_vkCreateImageView(
    VkDevice device, const VkImageViewCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkImageView *image_view) {
    if (image_view != NULL) *image_view = VK_NULL_HANDLE;
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || create_info == NULL || image_view == NULL ||
        allocator != NULL ||
        create_info->sType != VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    const uint64_t image_id = non_dispatchable_wire_id(
        &create_info->image, sizeof(create_info->image));
    struct bvb_vulkan_image_view_create_request decoded = {
        .device_id = device_state->wire_id,
        .image_id = image_id,
        .flags = create_info->flags,
        .view_type = create_info->viewType,
        .format = create_info->format,
        .component_r = create_info->components.r,
        .component_g = create_info->components.g,
        .component_b = create_info->components.b,
        .component_a = create_info->components.a,
        .aspect_mask = create_info->subresourceRange.aspectMask,
        .base_mip_level = create_info->subresourceRange.baseMipLevel,
        .level_count = create_info->subresourceRange.levelCount,
        .base_array_layer = create_info->subresourceRange.baseArrayLayer,
        .layer_count = create_info->subresourceRange.layerCount,
    };
    VkResult vulkan_result = encode_image_view_create_pnext(
        create_info->pNext, &decoded);
    uint8_t payload[BVB_VULKAN_IMAGE_VIEW_CREATE_REQUEST_SIZE];
    if (vulkan_result == VK_SUCCESS &&
        bvb_protocol_encode_vulkan_image_view_create_request(
            payload, &decoded) != 0) {
        vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (vulkan_result != VK_SUCCESS) return vulkan_result;
    struct bvb_resource_proxy *state = calloc(1, sizeof(*state));
    if (state == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        free(state);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (!image_owned_by_device_locked(image_id, device_state->wire_id)) {
        (void)pthread_mutex_unlock(&bvb_global_client.mutex);
        free(state);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    uint64_t wire_id = 0U;
    vulkan_result = create_resource_locked(
        BVB_OPCODE_VULKAN_IMAGE_VIEW_CREATE, payload, sizeof(payload),
        BVB_OBJECT_IMAGE_VIEW, device_state->wire_id, state, &wire_id);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (vulkan_result != VK_SUCCESS) {
        free(state);
        return vulkan_result;
    }
    memcpy(image_view, &wire_id, sizeof(*image_view));
    return VK_SUCCESS;
}

static void VKAPI_CALL bvb_bridge_vkDestroyImageView(
    VkDevice device, VkImageView image_view,
    const VkAllocationCallbacks *allocator) {
    destroy_resource(
        device, non_dispatchable_wire_id(&image_view, sizeof(image_view)),
        BVB_OBJECT_IMAGE_VIEW, BVB_OPCODE_VULKAN_IMAGE_VIEW_DESTROY,
        allocator);
}

static VkResult memory_write_locked(
    const struct bvb_resource_proxy *state, uint64_t offset,
    const uint8_t *bytes, uint64_t length) {
    if (state == NULL || bytes == NULL || length == 0U ||
        offset > state->allocation_size ||
        length > state->allocation_size - offset || connect_locked() != 0) {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    uint64_t completed = 0U;
    while (completed < length) {
        const uint32_t chunk =
            length - completed > BVB_VULKAN_MEMORY_IO_MAX_BYTES
                ? BVB_VULKAN_MEMORY_IO_MAX_BYTES
                : (uint32_t)(length - completed);
        const struct bvb_vulkan_memory_io_request decoded = {
            .memory_id = state->wire_id,
            .offset = offset + completed,
            .length = chunk,
        };
        struct bvb_protocol_packet request = {0};
        request.header = (struct bvb_protocol_header){
            .version = BVB_PROTOCOL_VERSION,
            .kind = BVB_PROTOCOL_REQUEST,
            .opcode = BVB_OPCODE_VULKAN_MEMORY_WRITE,
            .request_id = next_request_id_locked(),
        };
        int result = bvb_protocol_encode_vulkan_memory_write_request(
            request.payload, &decoded, bytes + completed,
            &request.header.payload_length);
        struct bvb_protocol_packet response = {0};
        if (result == 0) result = exchange_locked(&request, &response);
        struct bvb_vulkan_memory_io_response written = {0};
        const uint8_t *response_data = NULL;
        if (result == 0 && response.header.status == 0) {
            result = bvb_protocol_decode_vulkan_memory_io_response(
                response.payload, response.header.payload_length, &written,
                &response_data);
        }
        if (result != 0 || response.header.status != 0 ||
            written.length != 0U) {
            return VK_ERROR_MEMORY_MAP_FAILED;
        }
        if (written.vulkan_result != VK_SUCCESS) {
            return (VkResult)written.vulkan_result;
        }
        completed += chunk;
    }
    return VK_SUCCESS;
}

static VkResult memory_read_locked(
    const struct bvb_resource_proxy *state, uint64_t offset,
    uint8_t *bytes, uint64_t length) {
    if (state == NULL || bytes == NULL || length == 0U ||
        offset > state->allocation_size ||
        length > state->allocation_size - offset || connect_locked() != 0) {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    uint64_t completed = 0U;
    while (completed < length) {
        const uint32_t chunk =
            length - completed > BVB_VULKAN_MEMORY_IO_MAX_BYTES
                ? BVB_VULKAN_MEMORY_IO_MAX_BYTES
                : (uint32_t)(length - completed);
        const struct bvb_vulkan_memory_io_request decoded = {
            .memory_id = state->wire_id,
            .offset = offset + completed,
            .length = chunk,
        };
        struct bvb_protocol_packet request = {0};
        request.header = (struct bvb_protocol_header){
            .version = BVB_PROTOCOL_VERSION,
            .kind = BVB_PROTOCOL_REQUEST,
            .opcode = BVB_OPCODE_VULKAN_MEMORY_READ,
            .request_id = next_request_id_locked(),
            .payload_length = BVB_VULKAN_MEMORY_IO_PREFIX_SIZE,
        };
        int result = bvb_protocol_encode_vulkan_memory_read_request(
            request.payload, &decoded);
        struct bvb_protocol_packet response = {0};
        if (result == 0) result = exchange_locked(&request, &response);
        struct bvb_vulkan_memory_io_response read = {0};
        const uint8_t *response_data = NULL;
        if (result == 0 && response.header.status == 0) {
            result = bvb_protocol_decode_vulkan_memory_io_response(
                response.payload, response.header.payload_length, &read,
                &response_data);
        }
        if (result != 0 || response.header.status != 0) {
            return VK_ERROR_MEMORY_MAP_FAILED;
        }
        if (read.vulkan_result != VK_SUCCESS) {
            return (VkResult)read.vulkan_result;
        }
        if (read.length != chunk) return VK_ERROR_MEMORY_MAP_FAILED;
        memcpy(bytes + completed, response_data, chunk);
        completed += chunk;
    }
    return VK_SUCCESS;
}

static VkResult setup_memory_mirror_locked(
    const struct bvb_device_proxy *device_state,
    struct bvb_resource_proxy *state, uint64_t offset, uint64_t length,
    uint8_t **mapping, uint64_t *generation) {
    if (device_state == NULL || state == NULL || mapping == NULL ||
        generation == NULL || length == 0U || length > SIZE_MAX ||
        connect_locked() != 0 || !bvb_global_client.memory_mirror_enabled)
        return VK_ERROR_MEMORY_MAP_FAILED;
    *mapping = NULL;
    *generation = 0U;
    uint64_t selected_generation = 0U;
    const ssize_t random_bytes = syscall(
        SYS_getrandom, &selected_generation, sizeof(selected_generation), 0);
    if (random_bytes != (ssize_t)sizeof(selected_generation) ||
        selected_generation == 0U)
        return VK_ERROR_MEMORY_MAP_FAILED;
    int memory_fd = (int)syscall(
        SYS_memfd_create, "bvb-memory-mirror",
        MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (memory_fd < 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint8_t *shared = MAP_FAILED;
    int result = 0;
    int32_t vulkan_result = VK_ERROR_MEMORY_MAP_FAILED;
    if (ftruncate(memory_fd, (off_t)length) != 0) {
        result = -errno;
        goto done;
    }
    shared = map_client_visible_memory(
        memory_fd, (size_t)length, PROT_READ | PROT_WRITE, MAP_SHARED);
    if (shared == MAP_FAILED) {
        result = -errno;
        goto done;
    }
    if (fcntl(memory_fd, F_ADD_SEALS,
              F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0) {
        result = -errno;
        goto done;
    }
    const struct bvb_vulkan_memory_mirror_setup_request decoded = {
        .device_id = device_state->wire_id,
        .memory_id = state->wire_id,
        .generation = selected_generation,
        .offset = offset,
        .length = length,
    };
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_MEMORY_MIRROR_SETUP,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_MEMORY_MIRROR_SETUP_SIZE,
    };
    result = bvb_protocol_encode_vulkan_memory_mirror_setup_request(
        request.payload, &decoded);
    struct bvb_protocol_packet response = {0};
    if (result == 0)
        result = exchange_pass_fd_locked(&request, &response, memory_fd);
    if (result == 0 &&
        (response.header.status != 0 ||
         response.header.payload_length != BVB_VULKAN_RESULT_SIZE))
        result = response.header.status != 0 ? response.header.status
                                             : -EPROTO;
    if (result == 0)
        result = bvb_protocol_decode_vulkan_result(
            response.payload, &vulkan_result);
    if (result == 0 && vulkan_result == VK_SUCCESS) {
        atomic_thread_fence(memory_order_acquire);
        *mapping = shared;
        *generation = selected_generation;
        shared = MAP_FAILED;
    }
done:
    if (shared != MAP_FAILED) (void)munmap(shared, (size_t)length);
    (void)close(memory_fd);
    if (result != 0) {
        if (bvb_global_client.socket_fd >= 0)
            (void)close(bvb_global_client.socket_fd);
        bvb_global_client.socket_fd = -1;
        bvb_global_client.connection_poisoned = true;
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    return (VkResult)vulkan_result;
}

static VkResult memory_mirror_range_locked(
    const struct bvb_resource_proxy *state, uint64_t offset,
    uint64_t size, bool invalidate) {
    if (state == NULL || !state->mapped_shared ||
        state->mapped_generation == 0U) return VK_ERROR_MEMORY_MAP_FAILED;
    const struct bvb_vulkan_memory_mirror_range_request decoded = {
        .device_id = state->parent_id,
        .memory_id = state->wire_id,
        .generation = state->mapped_generation,
        .offset = offset,
        .size = size,
    };
    uint8_t payload[BVB_VULKAN_MEMORY_MIRROR_RANGE_SIZE];
    int result = bvb_protocol_encode_vulkan_memory_mirror_range_request(
        payload, &decoded);
    if (result == 0 && !invalidate)
        atomic_thread_fence(memory_order_release);
    VkResult vulkan_result = result == 0
        ? result_request_locked(
              invalidate ? BVB_OPCODE_VULKAN_MEMORY_MIRROR_INVALIDATE
                         : BVB_OPCODE_VULKAN_MEMORY_MIRROR_FLUSH,
              payload, sizeof(payload))
        : VK_ERROR_MEMORY_MAP_FAILED;
    if (vulkan_result == VK_SUCCESS && invalidate)
        atomic_thread_fence(memory_order_acquire);
    return vulkan_result;
}

static void unmap_memory_mirror_locked(
    const struct bvb_resource_proxy *state) {
    if (state == NULL || !state->mapped_shared ||
        state->mapped_generation == 0U) return;
    const struct bvb_vulkan_memory_mirror_unmap_request decoded = {
        .device_id = state->parent_id,
        .memory_id = state->wire_id,
        .generation = state->mapped_generation,
    };
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_MEMORY_MIRROR_UNMAP,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_MEMORY_MIRROR_UNMAP_SIZE,
    };
    int result = bvb_protocol_encode_vulkan_memory_mirror_unmap_request(
        request.payload, &decoded);
    struct bvb_protocol_packet response = {0};
    const uint64_t profile_started_ns =
        result == 0 && bvb_global_client.frame_profile_enabled
        ? frame_profile_monotonic_ns() : 0U;
    if (result == 0) {
        ++bvb_global_client.exchange_count;
        bvb_global_client.last_opcode = request.header.opcode;
        result = bvb_transport_send(bvb_global_client.socket_fd, &request);
        if (result == 0 &&
            getenv("BVB_TEST_DROP_MEMORY_UNMAP_ACK") != NULL) {
            (void)shutdown(bvb_global_client.socket_fd, SHUT_RDWR);
            result = -ECONNRESET;
        }
        if (result == 0)
            result = bvb_transport_receive(
                bvb_global_client.socket_fd, &response);
    }
    const uint64_t profile_finished_ns =
        bvb_global_client.frame_profile_enabled
        ? frame_profile_monotonic_ns() : 0U;
    frame_profile_record_rpc_locked(
        request.header.opcode, profile_started_ns, profile_finished_ns);
    if (result == 0 &&
        (response.header.status != 0 || response.header.payload_length != 0U))
        result = response.header.status != 0 ? response.header.status
                                             : -EPROTO;
    if (result != 0) {
        if (bvb_global_client.socket_fd >= 0)
            (void)close(bvb_global_client.socket_fd);
        bvb_global_client.socket_fd = -1;
        bvb_global_client.connection_poisoned = true;
    }
}

static VkResult setup_direct_memory_locked(
    const struct bvb_device_proxy *device_state,
    struct bvb_resource_proxy *state, uint64_t offset, uint64_t length,
    uint8_t **mapping, uint64_t *generation) {
    if (device_state == NULL || state == NULL || mapping == NULL ||
        generation == NULL || length == 0U || length > SIZE_MAX ||
        offset > INT64_MAX || connect_locked() != 0 ||
        !bvb_global_client.memory_direct_enabled)
        return VK_ERROR_MEMORY_MAP_FAILED;
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || offset % (uint64_t)page_size != 0U)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    *mapping = NULL;
    *generation = 0U;
    uint64_t selected_generation = 0U;
    const ssize_t random_bytes = syscall(
        SYS_getrandom, &selected_generation, sizeof(selected_generation), 0);
    if (random_bytes != (ssize_t)sizeof(selected_generation) ||
        selected_generation == 0U)
        return VK_ERROR_MEMORY_MAP_FAILED;
    const struct bvb_vulkan_memory_mirror_setup_request decoded = {
        .device_id = device_state->wire_id,
        .memory_id = state->wire_id,
        .generation = selected_generation,
        .offset = offset,
        .length = length,
    };
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_MEMORY_DIRECT_MAP_SETUP,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_MEMORY_MIRROR_SETUP_SIZE,
    };
    int result = bvb_protocol_encode_vulkan_memory_mirror_setup_request(
        request.payload, &decoded);
    struct bvb_protocol_packet response = {0};
    int received_fd = -1;
    size_t received_fd_count = 0U;
    if (result == 0)
        result = exchange_fds_locked(
            &request, &response, &received_fd, 1U, &received_fd_count);
    if (result == 0 &&
        (response.header.status != 0 ||
         response.header.payload_length != BVB_VULKAN_RESULT_SIZE))
        result = response.header.status != 0 ? response.header.status
                                             : -EPROTO;
    int32_t vulkan_result = VK_ERROR_MEMORY_MAP_FAILED;
    if (result == 0)
        result = bvb_protocol_decode_vulkan_result(
            response.payload, &vulkan_result);
    if (result == 0 &&
        ((vulkan_result == VK_SUCCESS &&
          (received_fd_count != 1U || received_fd < 0)) ||
         (vulkan_result != VK_SUCCESS && received_fd_count != 0U)))
        result = -EPROTO;
    uint8_t *shared = MAP_FAILED;
    if (result == 0 && vulkan_result == VK_SUCCESS) {
        struct stat fd_metadata;
        if (fstat(received_fd, &fd_metadata) != 0 ||
            (fd_metadata.st_size > 0 &&
             ((uint64_t)fd_metadata.st_size < offset ||
              length > (uint64_t)fd_metadata.st_size - offset))) {
            result = -EPROTO;
        } else {
            shared = map_client_visible_memory_at_offset(
                received_fd, (size_t)length, PROT_READ | PROT_WRITE,
                MAP_SHARED, (off_t)offset);
            if (shared == MAP_FAILED) result = -errno;
        }
    }
    if (received_fd >= 0) (void)close(received_fd);
    if (result == 0 && vulkan_result == VK_SUCCESS) {
        atomic_thread_fence(memory_order_acquire);
        *mapping = shared;
        *generation = selected_generation;
        return VK_SUCCESS;
    }
    if (shared != MAP_FAILED) (void)munmap(shared, (size_t)length);
    if (result != 0) {
        if (vulkan_result == VK_SUCCESS) {
            const struct bvb_resource_proxy cleanup = {
                .wire_id = state->wire_id,
                .parent_id = device_state->wire_id,
                .mapped_shared = true,
                .mapped_generation = selected_generation,
            };
            unmap_memory_mirror_locked(&cleanup);
        }
        if (!bvb_global_client.connection_poisoned) {
            if (bvb_global_client.socket_fd >= 0)
                (void)close(bvb_global_client.socket_fd);
            bvb_global_client.socket_fd = -1;
            bvb_global_client.connection_poisoned = true;
        }
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    return (VkResult)vulkan_result;
}

static VkResult mapped_range_locked(
    struct bvb_resource_proxy *state, uint64_t offset, uint64_t size,
    bool read_from_native) {
    if (state == NULL || state->mapped_bytes == NULL ||
        offset < state->mapped_offset || offset > state->allocation_size) {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    const uint64_t effective_size = size == VK_WHOLE_SIZE
                                        ? state->allocation_size - offset
                                        : size;
    if (effective_size == 0U ||
        effective_size > state->allocation_size - offset ||
        offset - state->mapped_offset > state->mapped_size ||
        effective_size >
            state->mapped_size - (offset - state->mapped_offset)) {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    uint8_t *shadow =
        state->mapped_bytes + (size_t)(offset - state->mapped_offset);
    if (state->mapped_direct) {
        if (read_from_native)
            atomic_thread_fence(memory_order_acquire);
        else
            atomic_thread_fence(memory_order_release);
        return VK_SUCCESS;
    }
    if (state->mapped_shared)
        return memory_mirror_range_locked(
            state, offset, effective_size, read_from_native);
    return read_from_native
               ? memory_read_locked(state, offset, shadow, effective_size)
               : memory_write_locked(state, offset, shadow, effective_size);
}

static VkResult VKAPI_CALL bvb_bridge_vkMapMemory(
    VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
    VkDeviceSize size, VkMemoryMapFlags flags, void **data) {
    if (data != NULL) *data = NULL;
    struct bvb_device_proxy *device_state = device_proxy(device);
    const uint64_t memory_id = non_dispatchable_wire_id(&memory, sizeof(memory));
    if (device_state == NULL || data == NULL || flags != 0U ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    struct bvb_resource_proxy *state =
        resource_proxy_locked(memory_id, BVB_OBJECT_DEVICE_MEMORY);
    const uint64_t effective_size =
        state != NULL && size == VK_WHOLE_SIZE && offset <= state->allocation_size
            ? state->allocation_size - offset
            : size;
    VkResult result = VK_ERROR_MEMORY_MAP_FAILED;
    bool used_shared_mirror = false;
    bool used_direct_mapping = false;
    uint8_t *returned_shadow = NULL;
    if (state != NULL && state->parent_id == device_state->wire_id &&
        state->mapped_bytes == NULL && effective_size != 0U &&
        offset <= state->allocation_size &&
        effective_size <= state->allocation_size - offset &&
        effective_size <= SIZE_MAX && connect_locked() == 0) {
        uint8_t *shadow = NULL;
        uint64_t generation = 0U;
        const bool use_shared_mirror =
            bvb_global_client.memory_mirror_enabled &&
            memory_is_buffer_only_locked(state->wire_id);
        used_shared_mirror = use_shared_mirror;
        if (use_shared_mirror) {
            if (bvb_global_client.memory_direct_enabled) {
                result = setup_direct_memory_locked(
                    device_state, state, offset, effective_size,
                    &shadow, &generation);
                used_direct_mapping = result == VK_SUCCESS;
            }
            if (!bvb_global_client.memory_direct_enabled ||
                (!used_direct_mapping &&
                 !bvb_global_client.connection_poisoned)) {
                result = setup_memory_mirror_locked(
                    device_state, state, offset, effective_size,
                    &shadow, &generation);
            }
        } else {
            shadow = map_client_visible_memory(
                -1, (size_t)effective_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS);
            result = shadow == MAP_FAILED
                ? VK_ERROR_OUT_OF_HOST_MEMORY
                : memory_read_locked(state, offset, shadow, effective_size);
            if (shadow == MAP_FAILED) shadow = NULL;
        }
        if (result == VK_SUCCESS) {
            state->mapped_offset = offset;
            state->mapped_size = effective_size;
            state->mapped_generation = generation;
            state->mapped_bytes = shadow;
            state->mapped_shared =
                use_shared_mirror;
            state->mapped_direct = used_direct_mapping;
            *data = shadow;
            returned_shadow = shadow;
        } else if (shadow != NULL) {
            (void)munmap(shadow, (size_t)effective_size);
        }
    }
    if (getenv("BVB_ICD_DIAGNOSTICS") != NULL) {
        const bool low32 = returned_shadow != NULL &&
            (uintptr_t)returned_shadow <= UINT32_MAX &&
            effective_size - 1U <=
                UINT32_MAX - (uintptr_t)returned_shadow;
        fprintf(stderr,
                "BVB_ICD_MEMORY_MAP memory=%#llx offset=%llu size=%llu "
                "shared=%u direct=%u result=%d address=%p low32=%u\n",
                (unsigned long long)memory_id,
                (unsigned long long)offset,
                (unsigned long long)effective_size,
                used_shared_mirror, used_direct_mapping, (int)result,
                (void *)returned_shadow,
                low32);
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return result;
}

static VkResult VKAPI_CALL bvb_bridge_vkMapMemory2(
    VkDevice device, const VkMemoryMapInfo *map_info, void **data) {
    if (data != NULL) *data = NULL;
    if (map_info == NULL || data == NULL ||
        map_info->sType != VK_STRUCTURE_TYPE_MEMORY_MAP_INFO ||
        map_info->pNext != NULL) {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    return bvb_bridge_vkMapMemory(
        device, map_info->memory, map_info->offset, map_info->size,
        map_info->flags, data);
}

static void VKAPI_CALL bvb_bridge_vkUnmapMemory(
    VkDevice device, VkDeviceMemory memory) {
    struct bvb_device_proxy *device_state = device_proxy(device);
    const uint64_t memory_id = non_dispatchable_wire_id(&memory, sizeof(memory));
    if (device_state == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return;
    }
    struct bvb_resource_proxy *state =
        resource_proxy_locked(memory_id, BVB_OBJECT_DEVICE_MEMORY);
    if (state != NULL && state->parent_id == device_state->wire_id &&
        state->mapped_bytes != NULL) {
        if (state->mapped_shared) {
            unmap_memory_mirror_locked(state);
            release_mapped_shadow_locked(state);
        } else {
            (void)mapped_range_locked(state, state->mapped_offset,
                                      state->mapped_size, false);
            release_mapped_shadow_locked(state);
        }
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
}

static VkResult mapped_ranges_operation(
    VkDevice device, uint32_t range_count, const VkMappedMemoryRange *ranges,
    bool read_from_native) {
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || range_count == 0U || ranges == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    VkResult result = VK_SUCCESS;
    for (uint32_t index = 0U; index < range_count && result == VK_SUCCESS;
         ++index) {
        const uint64_t memory_id = non_dispatchable_wire_id(
            &ranges[index].memory, sizeof(ranges[index].memory));
        struct bvb_resource_proxy *state =
            resource_proxy_locked(memory_id, BVB_OBJECT_DEVICE_MEMORY);
        if (ranges[index].sType != VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE ||
            ranges[index].pNext != NULL || state == NULL ||
            state->parent_id != device_state->wire_id) {
            result = VK_ERROR_MEMORY_MAP_FAILED;
        } else {
            result = mapped_range_locked(
                state, ranges[index].offset, ranges[index].size,
                read_from_native);
        }
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return result;
}

static VkResult VKAPI_CALL bvb_bridge_vkFlushMappedMemoryRanges(
    VkDevice device, uint32_t range_count,
    const VkMappedMemoryRange *ranges) {
    return mapped_ranges_operation(device, range_count, ranges, false);
}

static VkResult VKAPI_CALL bvb_bridge_vkInvalidateMappedMemoryRanges(
    VkDevice device, uint32_t range_count,
    const VkMappedMemoryRange *ranges) {
    return mapped_ranges_operation(device, range_count, ranges, true);
}

static VkResult flush_mapped_resources_locked(uint64_t device_id) {
    bool shared_mapping = false;
    for (struct bvb_resource_proxy *state = bvb_global_client.resources;
         state != NULL; state = state->next) {
        if (state->type == BVB_OBJECT_DEVICE_MEMORY &&
            state->parent_id == device_id && state->mapped_bytes != NULL &&
            !state->mapped_shared) {
            VkResult result = mapped_range_locked(
                state, state->mapped_offset, state->mapped_size, false);
            if (result != VK_SUCCESS) return result;
        } else if (state->type == BVB_OBJECT_DEVICE_MEMORY &&
                   state->parent_id == device_id &&
                   state->mapped_bytes != NULL && state->mapped_shared) {
            shared_mapping = true;
        }
    }
    if (shared_mapping) atomic_thread_fence(memory_order_release);
    return VK_SUCCESS;
}

static void poison_shared_command_stream(
    struct bvb_command_buffer_proxy *command_state, const char *entry,
    const char *reason, const char *shape, int status) {
    if (command_state == NULL) return;
    if (!command_stream_is_enabled()) {
        bvb_global_diagnostic_poison_command(
            (VkCommandBuffer)command_state, entry, reason, shape, status);
        return;
    }
    if (pthread_mutex_lock(&command_state->stream_mutex) != 0) return;
    store_command_diagnostic_locked(
        command_state, entry, reason, shape, status);
    command_state->stream_error = true;
    command_state->stream_sealed = false;
    (void)pthread_mutex_unlock(&command_state->stream_mutex);
}

static int init_image_subresource_range_supported(
    const VkImageSubresourceRange *range) {
    return range != NULL && range->aspectMask == VK_IMAGE_ASPECT_COLOR_BIT &&
           range->baseMipLevel == 0U && range->levelCount == 1U &&
           range->baseArrayLayer == 0U && range->layerCount == 1U;
}

static int command_image_barrier_range_supported(
    const VkImageSubresourceRange *range) {
    const VkImageAspectFlags supported_aspects =
        VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT |
        VK_IMAGE_ASPECT_STENCIL_BIT | VK_IMAGE_ASPECT_METADATA_BIT |
        VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT |
        VK_IMAGE_ASPECT_PLANE_2_BIT |
        VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT |
        VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT |
        VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT |
        VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT;
    return range != NULL && range->aspectMask != 0U &&
           (range->aspectMask & ~supported_aspects) == 0U &&
           range->levelCount != 0U && range->layerCount != 0U;
}

static int command_clear_color_range_supported(
    const VkImageSubresourceRange *range) {
    return command_image_barrier_range_supported(range) &&
           range->aspectMask == VK_IMAGE_ASPECT_COLOR_BIT;
}

static int command_image_layout_supported(VkImageLayout layout) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
        case VK_IMAGE_LAYOUT_GENERAL:
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        case VK_IMAGE_LAYOUT_PREINITIALIZED:
        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
        case VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR:
        case VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ:
        case VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT:
        case VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR:
        case VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT:
        case VK_IMAGE_LAYOUT_ZERO_INITIALIZED_EXT:
            return 1;
        default:
            return 0;
    }
}

static struct bvb_vulkan_image_subresource_range command_image_range(
    const VkImageSubresourceRange *range) {
    return (struct bvb_vulkan_image_subresource_range){
        .aspect_mask = range->aspectMask,
        .base_mip_level = range->baseMipLevel,
        .level_count = range->levelCount,
        .base_array_layer = range->baseArrayLayer,
        .layer_count = range->layerCount,
    };
}

static int submit_single_render_record(
    struct bvb_command_buffer_proxy *command_state,
    const uint8_t *batch, size_t batch_length,
    const char *entry, const char *shape) {
    if (command_state == NULL || batch == NULL ||
        batch_length < BVB_COMMAND_BATCH_HEADER_SIZE ||
        batch_length > BVB_PROTOCOL_MAX_PAYLOAD) return -EINVAL;
    if (command_stream_is_enabled()) {
        if (pthread_mutex_lock(&command_state->stream_mutex) != 0)
            return -EDEADLK;
        struct bvb_command_batch_iterator iterator;
        struct bvb_command_record record;
        int result = command_state->stream_recording &&
                             !command_state->stream_error
                         ? bvb_command_batch_iterator_init(
                               &iterator, batch, batch_length)
                         : -EINVAL;
        if (result == 0) result = bvb_command_batch_next(&iterator, &record);
        if (result == 0)
            result = bvb_command_batch_append_record(
                &command_state->stream_builder, &record);
        if (result == 0 &&
            bvb_command_batch_next(
                &iterator, &(struct bvb_command_record){0}) != 1)
            result = -EPROTO;
        if (result != 0) {
            store_command_diagnostic_locked(
                command_state, entry,
                "shared_render_record_append_rejected", shape, result);
            command_state->stream_error = true;
            command_state->stream_sealed = false;
        }
        (void)pthread_mutex_unlock(&command_state->stream_mutex);
        return result;
    }
    if (pthread_mutex_lock(&bvb_global_client.mutex) != 0) return -EDEADLK;
    int result = connect_locked();
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_COMMAND_BUFFER_IMMEDIATE_RECORD,
        .request_id = next_request_id_locked(),
        .payload_length = (uint32_t)batch_length,
    };
    if (result == 0) memcpy(request.payload, batch, batch_length);
    struct bvb_protocol_packet response = {0};
    if (result == 0) result = exchange_locked(&request, &response);
    if (result == 0 &&
        (response.header.status != 0 || response.header.payload_length != 0U))
        result = response.header.status != 0
                     ? response.header.status : -EPROTO;
    if (result != 0)
        store_command_diagnostic_locked(
            command_state, entry, "strict_render_record_rejected",
            shape, result);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return result;
}

static int begin_single_render_record(
    struct bvb_command_buffer_proxy *command_state,
    uint8_t bytes[BVB_PROTOCOL_MAX_PAYLOAD],
    struct bvb_command_batch_builder *builder) {
    return command_state == NULL ? -EINVAL : bvb_command_batch_begin(
        builder, bytes, BVB_PROTOCOL_MAX_PAYLOAD,
        command_state->wire_id, 1U);
}

static int finish_single_render_record(
    struct bvb_command_buffer_proxy *command_state,
    uint8_t bytes[BVB_PROTOCOL_MAX_PAYLOAD],
    struct bvb_command_batch_builder *builder,
    const char *entry, const char *shape) {
    size_t length = 0U;
    int result = bvb_command_batch_finish(builder, &length);
    return result != 0 ? result : submit_single_render_record(
        command_state, bytes, length, entry, shape);
}

static void record_query_command(
    VkCommandBuffer command_buffer, VkQueryPool query_pool,
    uint32_t first_query, uint32_t query_count, uint32_t flags,
    uint32_t index, uint64_t stage_mask, uint32_t kind,
    const char *entry) {
    struct bvb_command_buffer_proxy *command_state =
        command_buffer_proxy(command_buffer);
    const char *shape =
        "VkQueryPool_value,uint32_t_value,uint32_t_value,uint64_t_value";
    if (command_state == NULL) return;
    uint8_t bytes[BVB_PROTOCOL_MAX_PAYLOAD];
    struct bvb_command_batch_builder builder;
    const struct bvb_vulkan_query_command command = {
        .query_pool_id = non_dispatchable_wire_id(
            &query_pool, sizeof(query_pool)),
        .stage_mask = stage_mask,
        .first_query = first_query,
        .query_count = query_count,
        .flags = flags,
        .index = index,
        .kind = kind,
    };
    int result = begin_single_render_record(
        command_state, bytes, &builder);
    if (result == 0)
        result = bvb_command_batch_append_vulkan_query(&builder, &command);
    if (result == 0)
        result = finish_single_render_record(
            command_state, bytes, &builder, entry, shape);
    if (result != 0)
        poison_shared_command_stream(
            command_state, entry, "query_record_rejected", shape, result);
}

static void VKAPI_CALL bvb_bridge_vkCmdResetQueryPool(
    VkCommandBuffer command_buffer, VkQueryPool query_pool,
    uint32_t first_query, uint32_t query_count) {
    record_query_command(
        command_buffer, query_pool, first_query, query_count, 0U, 0U, 0U,
        BVB_VULKAN_QUERY_COMMAND_RESET, "vkCmdResetQueryPool");
}

static void VKAPI_CALL bvb_bridge_vkCmdBeginQuery(
    VkCommandBuffer command_buffer, VkQueryPool query_pool,
    uint32_t query, VkQueryControlFlags flags) {
    record_query_command(
        command_buffer, query_pool, query, 1U, flags, 0U, 0U,
        BVB_VULKAN_QUERY_COMMAND_BEGIN, "vkCmdBeginQuery");
}

static void VKAPI_CALL bvb_bridge_vkCmdEndQuery(
    VkCommandBuffer command_buffer, VkQueryPool query_pool,
    uint32_t query) {
    record_query_command(
        command_buffer, query_pool, query, 1U, 0U, 0U, 0U,
        BVB_VULKAN_QUERY_COMMAND_END, "vkCmdEndQuery");
}

static void VKAPI_CALL bvb_bridge_vkCmdWriteTimestamp(
    VkCommandBuffer command_buffer, VkPipelineStageFlagBits stage,
    VkQueryPool query_pool, uint32_t query) {
    record_query_command(
        command_buffer, query_pool, query, 1U, 0U, 0U, stage,
        BVB_VULKAN_QUERY_COMMAND_WRITE_TIMESTAMP, "vkCmdWriteTimestamp");
}

static void VKAPI_CALL bvb_bridge_vkCmdWriteTimestamp2(
    VkCommandBuffer command_buffer, VkPipelineStageFlags2 stage,
    VkQueryPool query_pool, uint32_t query) {
    record_query_command(
        command_buffer, query_pool, query, 1U, 0U, 0U, stage,
        BVB_VULKAN_QUERY_COMMAND_WRITE_TIMESTAMP_2, "vkCmdWriteTimestamp2");
}

static void VKAPI_CALL bvb_bridge_vkCmdBeginQueryIndexedEXT(
    VkCommandBuffer command_buffer, VkQueryPool query_pool,
    uint32_t query, VkQueryControlFlags flags, uint32_t index) {
    record_query_command(
        command_buffer, query_pool, query, 1U, flags, index, 0U,
        BVB_VULKAN_QUERY_COMMAND_BEGIN_INDEXED,
        "vkCmdBeginQueryIndexedEXT");
}

static void VKAPI_CALL bvb_bridge_vkCmdEndQueryIndexedEXT(
    VkCommandBuffer command_buffer, VkQueryPool query_pool,
    uint32_t query, uint32_t index) {
    record_query_command(
        command_buffer, query_pool, query, 1U, 0U, index, 0U,
        BVB_VULKAN_QUERY_COMMAND_END_INDEXED,
        "vkCmdEndQueryIndexedEXT");
}

static int rendering_attachment_to_command(
    const VkRenderingAttachmentInfo *attachment, bool required,
    struct bvb_vulkan_rendering_attachment *command) {
    if (command == NULL || (required && attachment == NULL)) return -EINVAL;
    memset(command, 0, sizeof(*command));
    if (attachment == NULL) return 0;
    if (attachment->sType != VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO)
        return -ENOTSUP;
    if (attachment->pNext != NULL) {
        const VkAttachmentFeedbackLoopInfoEXT *feedback =
            (const VkAttachmentFeedbackLoopInfoEXT *)attachment->pNext;
        if (feedback->sType !=
                VK_STRUCTURE_TYPE_ATTACHMENT_FEEDBACK_LOOP_INFO_EXT ||
            feedback->pNext != NULL || feedback->feedbackLoopEnable > 1U)
            return -ENOTSUP;
        command->feedback_loop_enable = feedback->feedbackLoopEnable;
    }
    if (attachment->imageView == VK_NULL_HANDLE) {
        return !required && attachment->resolveMode == VK_RESOLVE_MODE_NONE &&
                       attachment->resolveImageView == VK_NULL_HANDLE
                   ? 0
                   : -EINVAL;
    }
    if (!command_image_layout_supported(attachment->imageLayout))
        return -ENOTSUP;
    command->image_view_id = non_dispatchable_wire_id(
        &attachment->imageView, sizeof(attachment->imageView));
    command->image_layout = (uint32_t)attachment->imageLayout;
    command->resolve_mode = (uint32_t)attachment->resolveMode;
    command->load_op = (uint32_t)attachment->loadOp;
    command->store_op = (uint32_t)attachment->storeOp;
    if (attachment->resolveMode == VK_RESOLVE_MODE_NONE) {
        if (attachment->resolveImageView != VK_NULL_HANDLE) return -EINVAL;
    } else {
        if (attachment->resolveImageView == VK_NULL_HANDLE ||
            !command_image_layout_supported(attachment->resolveImageLayout))
            return -EINVAL;
        command->resolve_image_view_id = non_dispatchable_wire_id(
            &attachment->resolveImageView,
            sizeof(attachment->resolveImageView));
        command->resolve_image_layout =
            (uint32_t)attachment->resolveImageLayout;
    }
    memcpy(command->clear_words, &attachment->clearValue,
           sizeof(command->clear_words));
    return 0;
}

static int rendering_command_ownership_locked(
    struct bvb_command_buffer_proxy *state,
    const struct bvb_begin_rendering_command *command) {
    const uint32_t attachment_count = command->color_attachment_count +
        command->has_depth_attachment + command->has_stencil_attachment;
    for (uint32_t index = 0U; index < attachment_count; ++index) {
        const struct bvb_vulkan_rendering_attachment *attachment =
            index < command->color_attachment_count
                ? &command->color_attachments[index]
                : index == command->color_attachment_count
                          ? (command->has_depth_attachment != 0U
                                 ? &command->depth_attachment
                                 : &command->stencil_attachment)
                          : &command->stencil_attachment;
        if (attachment->image_view_id != 0U) {
            const int owned = shared_object_owned_by_device_cached_locked(
                state, attachment->image_view_id, BVB_OBJECT_IMAGE_VIEW);
            if (owned <= 0) return owned;
        }
        if (attachment->resolve_image_view_id != 0U) {
            const int owned = shared_object_owned_by_device_cached_locked(
                state, attachment->resolve_image_view_id,
                BVB_OBJECT_IMAGE_VIEW);
            if (owned <= 0) return owned;
        }
    }
    return 1;
}

static void VKAPI_CALL bvb_bridge_vkCmdBeginRendering(
    VkCommandBuffer command_buffer, const VkRenderingInfo *rendering_info) {
    struct bvb_command_buffer_proxy *state =
        command_buffer_proxy(command_buffer);
    const char *shape = "VkRenderingInfo_ptr";
    if (state == NULL || rendering_info == NULL ||
        rendering_info->sType != VK_STRUCTURE_TYPE_RENDERING_INFO ||
        rendering_info->pNext != NULL ||
        rendering_info->renderArea.extent.width == 0U ||
        rendering_info->renderArea.extent.height == 0U ||
        rendering_info->layerCount == 0U ||
        rendering_info->colorAttachmentCount >
            BVB_COMMAND_VULKAN_MAX_COLOR_ATTACHMENTS ||
        (rendering_info->colorAttachmentCount != 0U &&
         rendering_info->pColorAttachments == NULL) ||
        (rendering_info->colorAttachmentCount == 0U &&
         rendering_info->pDepthAttachment == NULL &&
         rendering_info->pStencilAttachment == NULL)) {
        poison_shared_command_stream(state, "vkCmdBeginRendering",
                                     "unsupported_rendering_shape",
                                     shape, -ENOTSUP);
        return;
    }
    struct bvb_begin_rendering_command command = {
        .flags = rendering_info->flags,
        .render_offset_x = rendering_info->renderArea.offset.x,
        .render_offset_y = rendering_info->renderArea.offset.y,
        .width = rendering_info->renderArea.extent.width,
        .height = rendering_info->renderArea.extent.height,
        .layer_count = rendering_info->layerCount,
        .view_mask = rendering_info->viewMask,
        .color_attachment_count = rendering_info->colorAttachmentCount,
        .has_depth_attachment =
            rendering_info->pDepthAttachment == NULL ? 0U : 1U,
        .has_stencil_attachment =
            rendering_info->pStencilAttachment == NULL ? 0U : 1U,
    };
    int result = 0;
    for (uint32_t index = 0U;
         result == 0 && index < command.color_attachment_count; ++index)
        result = rendering_attachment_to_command(
            &rendering_info->pColorAttachments[index], false,
            &command.color_attachments[index]);
    if (result == 0 && command.has_depth_attachment != 0U)
        result = rendering_attachment_to_command(
            rendering_info->pDepthAttachment, true,
            &command.depth_attachment);
    if (result == 0 && command.has_stencil_attachment != 0U)
        result = rendering_attachment_to_command(
            rendering_info->pStencilAttachment, true,
            &command.stencil_attachment);
    if (getenv("BVB_ICD_DIAGNOSTICS") != NULL) {
        fprintf(stderr,
                "BVB_ICD_BEGIN_RENDERING flags=%u offset=%d,%d "
                "extent=%ux%u layers=%u view_mask=%u colors=%u "
                "depth=%u stencil=%u shape_status=%d\n",
                command.flags, command.render_offset_x,
                command.render_offset_y, command.width, command.height,
                command.layer_count, command.view_mask,
                command.color_attachment_count,
                command.has_depth_attachment,
                command.has_stencil_attachment, result);
    }
    if (result != 0) {
        poison_shared_command_stream(state, "vkCmdBeginRendering",
                                     "unsupported_rendering_attachment",
                                     shape, result);
        return;
    }
    if (command_stream_is_enabled()) {
        if (pthread_mutex_lock(&state->stream_mutex) != 0) return;
        const int owned = rendering_command_ownership_locked(state, &command);
        if (owned < 0) return;
        (void)pthread_mutex_unlock(&state->stream_mutex);
        if (owned <= 0) {
            poison_shared_command_stream(state, "vkCmdBeginRendering",
                                         "image_view_ownership_rejected",
                                         shape, -EINVAL);
            return;
        }
    }
    uint8_t bytes[BVB_PROTOCOL_MAX_PAYLOAD];
    struct bvb_command_batch_builder builder;
    result = begin_single_render_record(state, bytes, &builder);
    if (result == 0)
        result = bvb_command_batch_append_begin_rendering(&builder, &command);
    if (result == 0)
        result = finish_single_render_record(
            state, bytes, &builder, "vkCmdBeginRendering", shape);
    if (result != 0)
        poison_shared_command_stream(state, "vkCmdBeginRendering",
                                     "render_record_rejected", shape, result);
}

static void VKAPI_CALL bvb_bridge_vkCmdEndRendering(
    VkCommandBuffer command_buffer) {
    struct bvb_command_buffer_proxy *state =
        command_buffer_proxy(command_buffer);
    uint8_t bytes[BVB_PROTOCOL_MAX_PAYLOAD];
    struct bvb_command_batch_builder builder;
    int result = begin_single_render_record(state, bytes, &builder);
    if (result == 0)
        result = bvb_command_batch_append_end_rendering(&builder);
    if (result == 0)
        result = finish_single_render_record(
            state, bytes, &builder, "vkCmdEndRendering", "");
    if (result != 0)
        poison_shared_command_stream(state, "vkCmdEndRendering",
                                     "render_record_rejected", "", result);
}

static void VKAPI_CALL bvb_bridge_vkCmdBindPipeline(
    VkCommandBuffer command_buffer, VkPipelineBindPoint bind_point,
    VkPipeline pipeline) {
    struct bvb_command_buffer_proxy *state =
        command_buffer_proxy(command_buffer);
    const char *shape = "VkPipelineBindPoint_value,VkPipeline_value";
    if (state == NULL || bind_point != VK_PIPELINE_BIND_POINT_GRAPHICS ||
        pipeline == VK_NULL_HANDLE) {
        poison_shared_command_stream(state, "vkCmdBindPipeline",
                                     "unsupported_pipeline_bind", shape,
                                     -ENOTSUP);
        return;
    }
    const uint64_t pipeline_id = non_dispatchable_wire_id(
        &pipeline, sizeof(pipeline));
    if (command_stream_is_enabled()) {
        if (pthread_mutex_lock(&state->stream_mutex) != 0) return;
        const int owned = shared_object_owned_by_device_cached_locked(
            state, pipeline_id, BVB_OBJECT_PIPELINE);
        if (owned < 0) return;
        (void)pthread_mutex_unlock(&state->stream_mutex);
        if (owned <= 0) {
            poison_shared_command_stream(state, "vkCmdBindPipeline",
                                         "pipeline_ownership_rejected",
                                         shape, -EINVAL);
            return;
        }
    }
    uint8_t bytes[BVB_PROTOCOL_MAX_PAYLOAD];
    struct bvb_command_batch_builder builder;
    int result = begin_single_render_record(state, bytes, &builder);
    if (result == 0)
        result = bvb_command_batch_append_bind_graphics_pipeline(
            &builder, &(const struct bvb_bind_graphics_pipeline_command){
                .pipeline_id = pipeline_id});
    if (result == 0)
        result = finish_single_render_record(
            state, bytes, &builder, "vkCmdBindPipeline", shape);
    if (result != 0)
        poison_shared_command_stream(state, "vkCmdBindPipeline",
                                     "render_record_rejected", shape, result);
}

static void VKAPI_CALL bvb_bridge_vkCmdPushConstants(
    VkCommandBuffer command_buffer, VkPipelineLayout layout,
    VkShaderStageFlags stage_flags, uint32_t offset, uint32_t size,
    const void *values) {
    struct bvb_command_buffer_proxy *state =
        command_buffer_proxy(command_buffer);
    const char *shape =
        "VkPipelineLayout_value,VkShaderStageFlags_value,uint32_t_value,uint32_t_value,void_ptr";
    if (state == NULL || layout == VK_NULL_HANDLE || stage_flags == 0U ||
        values == NULL || size == 0U ||
        size > BVB_COMMAND_VULKAN_MAX_PUSH_CONSTANT_BYTES ||
        (offset & 3U) != 0U || (size & 3U) != 0U ||
        offset > BVB_COMMAND_VULKAN_MAX_PUSH_CONSTANT_BYTES - size) {
        poison_shared_command_stream(state, "vkCmdPushConstants",
                                     "unsupported_push_constants",
                                     shape, -ENOTSUP);
        return;
    }
    struct bvb_vulkan_push_constants_command command = {
        .pipeline_layout_id = non_dispatchable_wire_id(&layout, sizeof(layout)),
        .stage_flags = stage_flags, .offset = offset, .size = size,
    };
    memcpy(command.data, values, size);
    if (command_stream_is_enabled()) {
        if (pthread_mutex_lock(&state->stream_mutex) != 0) return;
        const int owned = shared_object_owned_by_device_cached_locked(
            state, command.pipeline_layout_id,
            BVB_OBJECT_PIPELINE_LAYOUT);
        if (owned < 0) return;
        (void)pthread_mutex_unlock(&state->stream_mutex);
        if (owned <= 0) {
            poison_shared_command_stream(state, "vkCmdPushConstants",
                                         "layout_ownership_rejected",
                                         shape, -EINVAL);
            return;
        }
    }
    uint8_t bytes[BVB_PROTOCOL_MAX_PAYLOAD];
    struct bvb_command_batch_builder builder;
    int result = begin_single_render_record(state, bytes, &builder);
    if (result == 0)
        result = bvb_command_batch_append_vulkan_push_constants(
            &builder, &command);
    if (result == 0)
        result = finish_single_render_record(
            state, bytes, &builder, "vkCmdPushConstants", shape);
    if (result != 0)
        poison_shared_command_stream(state, "vkCmdPushConstants",
                                     "render_record_rejected", shape, result);
}

static void VKAPI_CALL bvb_bridge_vkCmdSetViewportWithCount(
    VkCommandBuffer command_buffer, uint32_t viewport_count,
    const VkViewport *viewports) {
    struct bvb_command_buffer_proxy *state =
        command_buffer_proxy(command_buffer);
    const char *shape = "uint32_t_value,VkViewport_ptr";
    if (state == NULL || viewport_count != 1U || viewports == NULL) {
        poison_shared_command_stream(state, "vkCmdSetViewportWithCount",
                                     "unsupported_viewport_count",
                                     shape, -ENOTSUP);
        return;
    }
    const struct bvb_set_viewport_command command = {
        .x = viewports[0].x, .y = viewports[0].y,
        .width = viewports[0].width, .height = viewports[0].height,
        .minimum_depth = viewports[0].minDepth,
        .maximum_depth = viewports[0].maxDepth,
    };
    uint8_t bytes[BVB_PROTOCOL_MAX_PAYLOAD];
    struct bvb_command_batch_builder builder;
    int result = begin_single_render_record(state, bytes, &builder);
    if (result == 0)
        result = bvb_command_batch_append_set_viewport(&builder, &command);
    if (result == 0)
        result = finish_single_render_record(
            state, bytes, &builder, "vkCmdSetViewportWithCount", shape);
    if (result != 0)
        poison_shared_command_stream(state, "vkCmdSetViewportWithCount",
                                     "render_record_rejected", shape, result);
}

static void VKAPI_CALL bvb_bridge_vkCmdSetScissorWithCount(
    VkCommandBuffer command_buffer, uint32_t scissor_count,
    const VkRect2D *scissors) {
    struct bvb_command_buffer_proxy *state =
        command_buffer_proxy(command_buffer);
    const char *shape = "uint32_t_value,VkRect2D_ptr";
    if (state == NULL || scissor_count != 1U || scissors == NULL) {
        poison_shared_command_stream(state, "vkCmdSetScissorWithCount",
                                     "unsupported_scissor_count",
                                     shape, -ENOTSUP);
        return;
    }
    const struct bvb_set_scissor_command command = {
        .x = scissors[0].offset.x, .y = scissors[0].offset.y,
        .width = scissors[0].extent.width,
        .height = scissors[0].extent.height,
    };
    uint8_t bytes[BVB_PROTOCOL_MAX_PAYLOAD];
    struct bvb_command_batch_builder builder;
    int result = begin_single_render_record(state, bytes, &builder);
    if (result == 0)
        result = bvb_command_batch_append_set_scissor(&builder, &command);
    if (result == 0)
        result = finish_single_render_record(
            state, bytes, &builder, "vkCmdSetScissorWithCount", shape);
    if (result != 0)
        poison_shared_command_stream(state, "vkCmdSetScissorWithCount",
                                     "render_record_rejected", shape, result);
}

static void VKAPI_CALL bvb_bridge_vkCmdDraw(
    VkCommandBuffer command_buffer, uint32_t vertex_count,
    uint32_t instance_count, uint32_t first_vertex,
    uint32_t first_instance) {
    struct bvb_command_buffer_proxy *state =
        command_buffer_proxy(command_buffer);
    const char *shape =
        "uint32_t_value,uint32_t_value,uint32_t_value,uint32_t_value";
    const struct bvb_draw_command command = {
        .vertex_count = vertex_count, .instance_count = instance_count,
        .first_vertex = first_vertex, .first_instance = first_instance,
    };
    uint8_t bytes[BVB_PROTOCOL_MAX_PAYLOAD];
    struct bvb_command_batch_builder builder;
    int result = begin_single_render_record(state, bytes, &builder);
    if (result == 0)
        result = bvb_command_batch_append_draw(&builder, &command);
    if (result == 0)
        result = finish_single_render_record(
            state, bytes, &builder, "vkCmdDraw", shape);
    if (result != 0)
        poison_shared_command_stream(state, "vkCmdDraw",
                                     "render_record_rejected", shape, result);
}

static int shared_buffers_owned_by_command_device(
    struct bvb_command_buffer_proxy *state, const uint64_t *buffer_ids,
    uint32_t buffer_count) {
    if (!command_stream_is_enabled()) return 1;
    if (state == NULL || buffer_ids == NULL || buffer_count == 0U)
        return -EINVAL;
    if (pthread_mutex_lock(&state->stream_mutex) != 0) return -EDEADLK;
    int result = 1;
    for (uint32_t index = 0U; result > 0 && index < buffer_count; ++index) {
        if (buffer_ids[index] == 0U) continue;
        result = shared_object_owned_by_device_cached_locked(
            state, buffer_ids[index], BVB_OBJECT_BUFFER);
    }
    if (result >= 0) (void)pthread_mutex_unlock(&state->stream_mutex);
    return result;
}

static void submit_vertex_buffers_command(
    struct bvb_command_buffer_proxy *state, uint16_t opcode,
    const struct bvb_vulkan_bind_vertex_buffers_command *command,
    const char *entry, const char *shape) {
    const int owned = shared_buffers_owned_by_command_device(
        state, command->buffer_ids, command->binding_count);
    if (owned <= 0) {
        poison_shared_command_stream(
            state, entry, "vertex_buffer_ownership_rejected", shape,
            owned < 0 ? owned : -EINVAL);
        return;
    }
    uint8_t bytes[BVB_PROTOCOL_MAX_PAYLOAD];
    struct bvb_command_batch_builder builder;
    int result = begin_single_render_record(state, bytes, &builder);
    if (result == 0)
        result = bvb_command_batch_append_vulkan_bind_vertex_buffers(
            &builder, opcode, command);
    if (result == 0)
        result = finish_single_render_record(
            state, bytes, &builder, entry, shape);
    if (result != 0)
        poison_shared_command_stream(
            state, entry, "vertex_buffer_record_rejected", shape, result);
}

static void VKAPI_CALL bvb_bridge_vkCmdBindVertexBuffers(
    VkCommandBuffer command_buffer, uint32_t first_binding,
    uint32_t binding_count, const VkBuffer *buffers,
    const VkDeviceSize *offsets) {
    struct bvb_command_buffer_proxy *state =
        command_buffer_proxy(command_buffer);
    const char *shape =
        "uint32_t_value,uint32_t_value,VkBuffer_ptr,VkDeviceSize_ptr";
    if (state == NULL || binding_count == 0U ||
        binding_count > BVB_COMMAND_VULKAN_MAX_VERTEX_BINDINGS ||
        first_binding > UINT32_MAX - binding_count || buffers == NULL ||
        offsets == NULL) {
        poison_shared_command_stream(
            state, "vkCmdBindVertexBuffers", "unsupported_vertex_shape",
            shape, -ENOTSUP);
        return;
    }
    struct bvb_vulkan_bind_vertex_buffers_command command = {
        .first_binding = first_binding, .binding_count = binding_count,
    };
    for (uint32_t index = 0U; index < binding_count; ++index) {
        command.buffer_ids[index] = non_dispatchable_wire_id(
            &buffers[index], sizeof(buffers[index]));
        command.offsets[index] = offsets[index];
    }
    submit_vertex_buffers_command(
        state, BVB_COMMAND_VULKAN_BIND_VERTEX_BUFFERS, &command,
        "vkCmdBindVertexBuffers", shape);
}

static void VKAPI_CALL bvb_bridge_vkCmdBindVertexBuffers2(
    VkCommandBuffer command_buffer, uint32_t first_binding,
    uint32_t binding_count, const VkBuffer *buffers,
    const VkDeviceSize *offsets, const VkDeviceSize *sizes,
    const VkDeviceSize *strides) {
    struct bvb_command_buffer_proxy *state =
        command_buffer_proxy(command_buffer);
    const char *shape =
        "uint32_t_value,uint32_t_value,VkBuffer_ptr,VkDeviceSize_ptr,VkDeviceSize_ptr,VkDeviceSize_ptr";
    if (state == NULL || binding_count == 0U ||
        binding_count > BVB_COMMAND_VULKAN_MAX_VERTEX_BINDINGS ||
        first_binding > UINT32_MAX - binding_count || buffers == NULL ||
        offsets == NULL) {
        poison_shared_command_stream(
            state, "vkCmdBindVertexBuffers2", "unsupported_vertex_shape",
            shape, -ENOTSUP);
        return;
    }
    struct bvb_vulkan_bind_vertex_buffers_command command = {
        .first_binding = first_binding, .binding_count = binding_count,
        .has_sizes = sizes != NULL ? 1U : 0U,
        .has_strides = strides != NULL ? 1U : 0U,
    };
    for (uint32_t index = 0U; index < binding_count; ++index) {
        command.buffer_ids[index] = non_dispatchable_wire_id(
            &buffers[index], sizeof(buffers[index]));
        command.offsets[index] = offsets[index];
        if (sizes != NULL) command.sizes[index] = sizes[index];
        if (strides != NULL) command.strides[index] = strides[index];
    }
    submit_vertex_buffers_command(
        state, BVB_COMMAND_VULKAN_BIND_VERTEX_BUFFERS_2, &command,
        "vkCmdBindVertexBuffers2", shape);
}

static void submit_index_buffer_command(
    struct bvb_command_buffer_proxy *state, uint16_t opcode,
    const struct bvb_vulkan_bind_index_buffer_command *command,
    const char *entry, const char *shape) {
    const int owned = shared_buffers_owned_by_command_device(
        state, &command->buffer_id, 1U);
    if (owned <= 0) {
        poison_shared_command_stream(
            state, entry, "index_buffer_ownership_rejected", shape,
            owned < 0 ? owned : -EINVAL);
        return;
    }
    uint8_t bytes[BVB_PROTOCOL_MAX_PAYLOAD];
    struct bvb_command_batch_builder builder;
    int result = begin_single_render_record(state, bytes, &builder);
    if (result == 0)
        result = bvb_command_batch_append_vulkan_bind_index_buffer(
            &builder, opcode, command);
    if (result == 0)
        result = finish_single_render_record(
            state, bytes, &builder, entry, shape);
    if (result != 0)
        poison_shared_command_stream(
            state, entry, "index_buffer_record_rejected", shape, result);
}

static void VKAPI_CALL bvb_bridge_vkCmdBindIndexBuffer(
    VkCommandBuffer command_buffer, VkBuffer buffer, VkDeviceSize offset,
    VkIndexType index_type) {
    struct bvb_command_buffer_proxy *state =
        command_buffer_proxy(command_buffer);
    const char *shape = "VkBuffer_value,VkDeviceSize_value,VkIndexType_value";
    const struct bvb_vulkan_bind_index_buffer_command command = {
        .buffer_id = non_dispatchable_wire_id(&buffer, sizeof(buffer)),
        .offset = offset, .size = UINT64_MAX,
        .index_type = (uint32_t)index_type,
    };
    submit_index_buffer_command(
        state, BVB_COMMAND_VULKAN_BIND_INDEX_BUFFER, &command,
        "vkCmdBindIndexBuffer", shape);
}

static void VKAPI_CALL bvb_bridge_vkCmdBindIndexBuffer2(
    VkCommandBuffer command_buffer, VkBuffer buffer, VkDeviceSize offset,
    VkDeviceSize size, VkIndexType index_type) {
    struct bvb_command_buffer_proxy *state =
        command_buffer_proxy(command_buffer);
    const char *shape =
        "VkBuffer_value,VkDeviceSize_value,VkDeviceSize_value,VkIndexType_value";
    const struct bvb_vulkan_bind_index_buffer_command command = {
        .buffer_id = non_dispatchable_wire_id(&buffer, sizeof(buffer)),
        .offset = offset, .size = size,
        .index_type = (uint32_t)index_type,
    };
    submit_index_buffer_command(
        state, BVB_COMMAND_VULKAN_BIND_INDEX_BUFFER_2, &command,
        "vkCmdBindIndexBuffer2", shape);
}

static void VKAPI_CALL bvb_bridge_vkCmdDrawIndexed(
    VkCommandBuffer command_buffer, uint32_t index_count,
    uint32_t instance_count, uint32_t first_index, int32_t vertex_offset,
    uint32_t first_instance) {
    struct bvb_command_buffer_proxy *state =
        command_buffer_proxy(command_buffer);
    const char *shape =
        "uint32_t_value,uint32_t_value,uint32_t_value,int32_t_value,uint32_t_value";
    const struct bvb_vulkan_draw_indexed_command command = {
        .index_count = index_count, .instance_count = instance_count,
        .first_index = first_index, .vertex_offset = vertex_offset,
        .first_instance = first_instance,
    };
    uint8_t bytes[BVB_PROTOCOL_MAX_PAYLOAD];
    struct bvb_command_batch_builder builder;
    int result = begin_single_render_record(state, bytes, &builder);
    if (result == 0)
        result = bvb_command_batch_append_vulkan_draw_indexed(
            &builder, &command);
    if (result == 0)
        result = finish_single_render_record(
            state, bytes, &builder, "vkCmdDrawIndexed", shape);
    if (result != 0)
        poison_shared_command_stream(
            state, "vkCmdDrawIndexed", "draw_record_rejected", shape,
            result);
}

static void submit_indirect_draw_command(
    struct bvb_command_buffer_proxy *state, uint16_t opcode,
    const struct bvb_vulkan_draw_indirect_command *command,
    const char *entry, const char *shape) {
    const int owned = shared_buffers_owned_by_command_device(
        state, &command->buffer_id, 1U);
    if (owned <= 0) {
        poison_shared_command_stream(
            state, entry, "indirect_buffer_ownership_rejected", shape,
            owned < 0 ? owned : -EINVAL);
        return;
    }
    uint8_t bytes[BVB_PROTOCOL_MAX_PAYLOAD];
    struct bvb_command_batch_builder builder;
    int result = begin_single_render_record(state, bytes, &builder);
    if (result == 0)
        result = bvb_command_batch_append_vulkan_draw_indirect(
            &builder, opcode, command);
    if (result == 0)
        result = finish_single_render_record(
            state, bytes, &builder, entry, shape);
    if (result != 0)
        poison_shared_command_stream(
            state, entry, "indirect_draw_record_rejected", shape, result);
}

static void bvb_bridge_vkCmdDrawIndirectCommon(
    VkCommandBuffer command_buffer, VkBuffer buffer, VkDeviceSize offset,
    uint32_t draw_count, uint32_t stride, uint16_t opcode,
    const char *entry) {
    struct bvb_command_buffer_proxy *state =
        command_buffer_proxy(command_buffer);
    const char *shape =
        "VkBuffer_value,VkDeviceSize_value,uint32_t_value,uint32_t_value";
    const struct bvb_vulkan_draw_indirect_command command = {
        .buffer_id = non_dispatchable_wire_id(&buffer, sizeof(buffer)),
        .offset = offset, .draw_count = draw_count, .stride = stride,
    };
    submit_indirect_draw_command(
        state, opcode, &command, entry, shape);
}

static void VKAPI_CALL bvb_bridge_vkCmdDrawIndirect(
    VkCommandBuffer command_buffer, VkBuffer buffer, VkDeviceSize offset,
    uint32_t draw_count, uint32_t stride) {
    bvb_bridge_vkCmdDrawIndirectCommon(
        command_buffer, buffer, offset, draw_count, stride,
        BVB_COMMAND_VULKAN_DRAW_INDIRECT, "vkCmdDrawIndirect");
}

static void VKAPI_CALL bvb_bridge_vkCmdDrawIndexedIndirect(
    VkCommandBuffer command_buffer, VkBuffer buffer, VkDeviceSize offset,
    uint32_t draw_count, uint32_t stride) {
    bvb_bridge_vkCmdDrawIndirectCommon(
        command_buffer, buffer, offset, draw_count, stride,
        BVB_COMMAND_VULKAN_DRAW_INDEXED_INDIRECT,
        "vkCmdDrawIndexedIndirect");
}

static void submit_indirect_count_draw_command(
    struct bvb_command_buffer_proxy *state, uint16_t opcode,
    const struct bvb_vulkan_draw_indirect_count_command *command,
    const char *entry, const char *shape) {
    const uint64_t buffers[2] = {
        command->buffer_id, command->count_buffer_id};
    const int owned = shared_buffers_owned_by_command_device(
        state, buffers, 2U);
    if (owned <= 0) {
        poison_shared_command_stream(
            state, entry, "indirect_count_ownership_rejected", shape,
            owned < 0 ? owned : -EINVAL);
        return;
    }
    uint8_t bytes[BVB_PROTOCOL_MAX_PAYLOAD];
    struct bvb_command_batch_builder builder;
    int result = begin_single_render_record(state, bytes, &builder);
    if (result == 0)
        result = bvb_command_batch_append_vulkan_draw_indirect_count(
            &builder, opcode, command);
    if (result == 0)
        result = finish_single_render_record(
            state, bytes, &builder, entry, shape);
    if (result != 0)
        poison_shared_command_stream(
            state, entry, "indirect_count_record_rejected", shape, result);
}

static void bvb_bridge_vkCmdDrawIndirectCountCommon(
    VkCommandBuffer command_buffer, VkBuffer buffer, VkDeviceSize offset,
    VkBuffer count_buffer, VkDeviceSize count_buffer_offset,
    uint32_t maximum_draw_count, uint32_t stride, uint16_t opcode,
    const char *entry) {
    struct bvb_command_buffer_proxy *state =
        command_buffer_proxy(command_buffer);
    const char *shape =
        "VkBuffer_value,VkDeviceSize_value,VkBuffer_value,VkDeviceSize_value,uint32_t_value,uint32_t_value";
    const struct bvb_vulkan_draw_indirect_count_command command = {
        .buffer_id = non_dispatchable_wire_id(&buffer, sizeof(buffer)),
        .offset = offset,
        .count_buffer_id = non_dispatchable_wire_id(
            &count_buffer, sizeof(count_buffer)),
        .count_buffer_offset = count_buffer_offset,
        .maximum_draw_count = maximum_draw_count, .stride = stride,
    };
    submit_indirect_count_draw_command(
        state, opcode, &command, entry, shape);
}

static void VKAPI_CALL bvb_bridge_vkCmdDrawIndirectCount(
    VkCommandBuffer command_buffer, VkBuffer buffer, VkDeviceSize offset,
    VkBuffer count_buffer, VkDeviceSize count_buffer_offset,
    uint32_t maximum_draw_count, uint32_t stride) {
    bvb_bridge_vkCmdDrawIndirectCountCommon(
        command_buffer, buffer, offset, count_buffer, count_buffer_offset,
        maximum_draw_count, stride, BVB_COMMAND_VULKAN_DRAW_INDIRECT_COUNT,
        "vkCmdDrawIndirectCount");
}

static void VKAPI_CALL bvb_bridge_vkCmdDrawIndexedIndirectCount(
    VkCommandBuffer command_buffer, VkBuffer buffer, VkDeviceSize offset,
    VkBuffer count_buffer, VkDeviceSize count_buffer_offset,
    uint32_t maximum_draw_count, uint32_t stride) {
    bvb_bridge_vkCmdDrawIndirectCountCommon(
        command_buffer, buffer, offset, count_buffer, count_buffer_offset,
        maximum_draw_count, stride,
        BVB_COMMAND_VULKAN_DRAW_INDEXED_INDIRECT_COUNT,
        "vkCmdDrawIndexedIndirectCount");
}

static uint32_t dynamic_float_word(float value) {
    uint32_t word = 0U;
    memcpy(&word, &value, sizeof(word));
    return word;
}

static void submit_dynamic_state_command(
    VkCommandBuffer command_buffer, uint32_t kind, uint32_t value_count,
    const uint32_t *values, const char *entry, const char *shape) {
    struct bvb_command_buffer_proxy *state =
        command_buffer_proxy(command_buffer);
    if (state == NULL || value_count == 0U ||
        value_count > BVB_COMMAND_VULKAN_MAX_DYNAMIC_STATE_VALUES ||
        values == NULL) {
        poison_shared_command_stream(
            state, entry, "unsupported_dynamic_state_shape", shape,
            -ENOTSUP);
        return;
    }
    struct bvb_vulkan_dynamic_state_command command = {
        .kind = kind, .value_count = value_count,
    };
    memcpy(command.values, values, value_count * sizeof(values[0]));
    uint8_t bytes[BVB_PROTOCOL_MAX_PAYLOAD];
    struct bvb_command_batch_builder builder;
    int result = begin_single_render_record(state, bytes, &builder);
    if (result == 0)
        result = bvb_command_batch_append_vulkan_dynamic_state(
            &builder, &command);
    if (result == 0)
        result = finish_single_render_record(
            state, bytes, &builder, entry, shape);
    if (result != 0)
        poison_shared_command_stream(
            state, entry, "dynamic_state_record_rejected", shape, result);
}

static void VKAPI_CALL bvb_bridge_vkCmdSetCullMode(
    VkCommandBuffer command_buffer, VkCullModeFlags cull_mode) {
    const uint32_t value = cull_mode;
    submit_dynamic_state_command(
        command_buffer, BVB_VULKAN_DYNAMIC_STATE_CULL_MODE, 1U, &value,
        "vkCmdSetCullMode", "VkCullModeFlags_value");
}

static void VKAPI_CALL bvb_bridge_vkCmdSetFrontFace(
    VkCommandBuffer command_buffer, VkFrontFace front_face) {
    const uint32_t value = (uint32_t)front_face;
    submit_dynamic_state_command(
        command_buffer, BVB_VULKAN_DYNAMIC_STATE_FRONT_FACE, 1U, &value,
        "vkCmdSetFrontFace", "VkFrontFace_value");
}

static void VKAPI_CALL bvb_bridge_vkCmdSetPrimitiveTopology(
    VkCommandBuffer command_buffer, VkPrimitiveTopology topology) {
    const uint32_t value = (uint32_t)topology;
    submit_dynamic_state_command(
        command_buffer, BVB_VULKAN_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, 1U,
        &value, "vkCmdSetPrimitiveTopology", "VkPrimitiveTopology_value");
}

static void VKAPI_CALL bvb_bridge_vkCmdSetDepthTestEnable(
    VkCommandBuffer command_buffer, VkBool32 enable) {
    submit_dynamic_state_command(
        command_buffer, BVB_VULKAN_DYNAMIC_STATE_DEPTH_TEST_ENABLE, 1U,
        &enable, "vkCmdSetDepthTestEnable", "VkBool32_value");
}

static void VKAPI_CALL bvb_bridge_vkCmdSetDepthWriteEnable(
    VkCommandBuffer command_buffer, VkBool32 enable) {
    submit_dynamic_state_command(
        command_buffer, BVB_VULKAN_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, 1U,
        &enable, "vkCmdSetDepthWriteEnable", "VkBool32_value");
}

static void VKAPI_CALL bvb_bridge_vkCmdSetDepthCompareOp(
    VkCommandBuffer command_buffer, VkCompareOp compare_op) {
    const uint32_t value = (uint32_t)compare_op;
    submit_dynamic_state_command(
        command_buffer, BVB_VULKAN_DYNAMIC_STATE_DEPTH_COMPARE_OP, 1U,
        &value, "vkCmdSetDepthCompareOp", "VkCompareOp_value");
}

static void VKAPI_CALL bvb_bridge_vkCmdSetDepthBoundsTestEnable(
    VkCommandBuffer command_buffer, VkBool32 enable) {
    submit_dynamic_state_command(
        command_buffer,
        BVB_VULKAN_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE, 1U, &enable,
        "vkCmdSetDepthBoundsTestEnable", "VkBool32_value");
}

static void VKAPI_CALL bvb_bridge_vkCmdSetStencilTestEnable(
    VkCommandBuffer command_buffer, VkBool32 enable) {
    submit_dynamic_state_command(
        command_buffer, BVB_VULKAN_DYNAMIC_STATE_STENCIL_TEST_ENABLE, 1U,
        &enable, "vkCmdSetStencilTestEnable", "VkBool32_value");
}

static void VKAPI_CALL bvb_bridge_vkCmdSetStencilOp(
    VkCommandBuffer command_buffer, VkStencilFaceFlags face_mask,
    VkStencilOp fail_op, VkStencilOp pass_op,
    VkStencilOp depth_fail_op, VkCompareOp compare_op) {
    const uint32_t values[5] = {
        face_mask, (uint32_t)fail_op, (uint32_t)pass_op,
        (uint32_t)depth_fail_op, (uint32_t)compare_op,
    };
    submit_dynamic_state_command(
        command_buffer, BVB_VULKAN_DYNAMIC_STATE_STENCIL_OP, 5U, values,
        "vkCmdSetStencilOp",
        "VkStencilFaceFlags_value,VkStencilOp_value,VkStencilOp_value,VkStencilOp_value,VkCompareOp_value");
}

static void VKAPI_CALL bvb_bridge_vkCmdSetRasterizerDiscardEnable(
    VkCommandBuffer command_buffer, VkBool32 enable) {
    submit_dynamic_state_command(
        command_buffer,
        BVB_VULKAN_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, 1U, &enable,
        "vkCmdSetRasterizerDiscardEnable", "VkBool32_value");
}

static void VKAPI_CALL bvb_bridge_vkCmdSetDepthBiasEnable(
    VkCommandBuffer command_buffer, VkBool32 enable) {
    submit_dynamic_state_command(
        command_buffer, BVB_VULKAN_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, 1U,
        &enable, "vkCmdSetDepthBiasEnable", "VkBool32_value");
}

static void VKAPI_CALL bvb_bridge_vkCmdSetPrimitiveRestartEnable(
    VkCommandBuffer command_buffer, VkBool32 enable) {
    submit_dynamic_state_command(
        command_buffer,
        BVB_VULKAN_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, 1U, &enable,
        "vkCmdSetPrimitiveRestartEnable", "VkBool32_value");
}

static void VKAPI_CALL bvb_bridge_vkCmdSetDepthBias(
    VkCommandBuffer command_buffer, float constant_factor,
    float clamp, float slope_factor) {
    const uint32_t values[3] = {
        dynamic_float_word(constant_factor), dynamic_float_word(clamp),
        dynamic_float_word(slope_factor),
    };
    submit_dynamic_state_command(
        command_buffer, BVB_VULKAN_DYNAMIC_STATE_DEPTH_BIAS, 3U, values,
        "vkCmdSetDepthBias", "float_value,float_value,float_value");
}

static void VKAPI_CALL bvb_bridge_vkCmdSetDepthBounds(
    VkCommandBuffer command_buffer, float minimum, float maximum) {
    const uint32_t values[2] = {
        dynamic_float_word(minimum), dynamic_float_word(maximum),
    };
    submit_dynamic_state_command(
        command_buffer, BVB_VULKAN_DYNAMIC_STATE_DEPTH_BOUNDS, 2U, values,
        "vkCmdSetDepthBounds", "float_value,float_value");
}

static void VKAPI_CALL bvb_bridge_vkCmdSetStencilCompareMask(
    VkCommandBuffer command_buffer, VkStencilFaceFlags face_mask,
    uint32_t compare_mask) {
    const uint32_t values[2] = {face_mask, compare_mask};
    submit_dynamic_state_command(
        command_buffer, BVB_VULKAN_DYNAMIC_STATE_STENCIL_COMPARE_MASK, 2U,
        values, "vkCmdSetStencilCompareMask",
        "VkStencilFaceFlags_value,uint32_t_value");
}

static void VKAPI_CALL bvb_bridge_vkCmdSetStencilWriteMask(
    VkCommandBuffer command_buffer, VkStencilFaceFlags face_mask,
    uint32_t write_mask) {
    const uint32_t values[2] = {face_mask, write_mask};
    submit_dynamic_state_command(
        command_buffer, BVB_VULKAN_DYNAMIC_STATE_STENCIL_WRITE_MASK, 2U,
        values, "vkCmdSetStencilWriteMask",
        "VkStencilFaceFlags_value,uint32_t_value");
}

static void VKAPI_CALL bvb_bridge_vkCmdSetStencilReference(
    VkCommandBuffer command_buffer, VkStencilFaceFlags face_mask,
    uint32_t reference) {
    const uint32_t values[2] = {face_mask, reference};
    submit_dynamic_state_command(
        command_buffer, BVB_VULKAN_DYNAMIC_STATE_STENCIL_REFERENCE, 2U,
        values, "vkCmdSetStencilReference",
        "VkStencilFaceFlags_value,uint32_t_value");
}

static void VKAPI_CALL bvb_bridge_vkCmdSetLineWidth(
    VkCommandBuffer command_buffer, float line_width) {
    const uint32_t value = dynamic_float_word(line_width);
    submit_dynamic_state_command(
        command_buffer, BVB_VULKAN_DYNAMIC_STATE_LINE_WIDTH, 1U, &value,
        "vkCmdSetLineWidth", "float_value");
}

static void VKAPI_CALL bvb_bridge_vkCmdSetBlendConstants(
    VkCommandBuffer command_buffer, const float blend_constants[4]) {
    struct bvb_command_buffer_proxy *state =
        command_buffer_proxy(command_buffer);
    if (blend_constants == NULL) {
        poison_shared_command_stream(
            state, "vkCmdSetBlendConstants",
            "unsupported_dynamic_state_shape", "float_ptr", -ENOTSUP);
        return;
    }
    const uint32_t values[4] = {
        dynamic_float_word(blend_constants[0]),
        dynamic_float_word(blend_constants[1]),
        dynamic_float_word(blend_constants[2]),
        dynamic_float_word(blend_constants[3]),
    };
    submit_dynamic_state_command(
        command_buffer, BVB_VULKAN_DYNAMIC_STATE_BLEND_CONSTANTS, 4U,
        values, "vkCmdSetBlendConstants", "float_ptr");
}

static void VKAPI_CALL bvb_bridge_vkCmdClearDepthStencilImage(
    VkCommandBuffer command_buffer, VkImage image,
    VkImageLayout image_layout, const VkClearDepthStencilValue *value,
    uint32_t range_count, const VkImageSubresourceRange *ranges) {
    struct bvb_command_buffer_proxy *state =
        command_buffer_proxy(command_buffer);
    const char *shape =
        "VkImage_value,VkImageLayout_value,VkClearDepthStencilValue_ptr,uint32_t_value,VkImageSubresourceRange_ptr";
    if (state == NULL || value == NULL || ranges == NULL ||
        (image_layout != VK_IMAGE_LAYOUT_GENERAL &&
         image_layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) ||
        range_count == 0U ||
        range_count > BVB_COMMAND_VULKAN_MAX_CLEAR_RANGES) {
        poison_shared_command_stream(
            state, "vkCmdClearDepthStencilImage",
            "unsupported_clear_shape", shape, -ENOTSUP);
        return;
    }
    struct bvb_vulkan_clear_depth_stencil_image_command command = {
        .image_id = non_dispatchable_wire_id(&image, sizeof(image)),
        .image_layout = (uint32_t)image_layout,
        .range_count = range_count,
        .stencil = value->stencil,
    };
    command.depth_word = dynamic_float_word(value->depth);
    for (uint32_t index = 0U; index < range_count; ++index) {
        if ((ranges[index].aspectMask &
             ~(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) !=
                0U ||
            (ranges[index].aspectMask &
             (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) ==
                0U ||
            ranges[index].levelCount == 0U ||
            ranges[index].layerCount == 0U) {
            poison_shared_command_stream(
                state, "vkCmdClearDepthStencilImage",
                "unsupported_clear_range", shape, -ENOTSUP);
            return;
        }
        command.ranges[index] = command_image_range(&ranges[index]);
    }
    uint8_t bytes[BVB_PROTOCOL_MAX_PAYLOAD];
    struct bvb_command_batch_builder builder;
    int result = begin_single_render_record(state, bytes, &builder);
    if (result == 0)
        result = bvb_command_batch_append_vulkan_clear_depth_stencil_image(
            &builder, &command);
    if (result == 0)
        result = finish_single_render_record(
            state, bytes, &builder, "vkCmdClearDepthStencilImage", shape);
    if (result != 0)
        poison_shared_command_stream(
            state, "vkCmdClearDepthStencilImage",
            "clear_record_rejected", shape, result);
}

static void VKAPI_CALL bvb_bridge_vkCmdClearAttachments(
    VkCommandBuffer command_buffer, uint32_t attachment_count,
    const VkClearAttachment *attachments, uint32_t rect_count,
    const VkClearRect *rects) {
    struct bvb_command_buffer_proxy *state =
        command_buffer_proxy(command_buffer);
    const char *shape =
        "uint32_t_value,VkClearAttachment_ptr,uint32_t_value,VkClearRect_ptr";
    if (state == NULL || attachments == NULL || rects == NULL ||
        attachment_count == 0U ||
        attachment_count > BVB_COMMAND_VULKAN_MAX_CLEAR_ATTACHMENTS ||
        rect_count == 0U || rect_count > BVB_COMMAND_VULKAN_MAX_CLEAR_RECTS) {
        poison_shared_command_stream(
            state, "vkCmdClearAttachments",
            "unsupported_clear_shape", shape, -ENOTSUP);
        return;
    }
    struct bvb_vulkan_clear_attachments_command command = {
        .attachment_count = attachment_count,
        .rect_count = rect_count,
    };
    for (uint32_t index = 0U; index < attachment_count; ++index) {
        if (attachments[index].aspectMask == 0U ||
            (attachments[index].aspectMask &
             ~(VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT |
               VK_IMAGE_ASPECT_STENCIL_BIT)) != 0U) {
            poison_shared_command_stream(
                state, "vkCmdClearAttachments",
                "unsupported_clear_attachment", shape, -ENOTSUP);
            return;
        }
        command.attachments[index].aspect_mask =
            attachments[index].aspectMask;
        command.attachments[index].color_attachment =
            attachments[index].colorAttachment;
        memcpy(command.attachments[index].clear_words,
               &attachments[index].clearValue,
               sizeof(command.attachments[index].clear_words));
    }
    for (uint32_t index = 0U; index < rect_count; ++index) {
        if (rects[index].rect.extent.width == 0U ||
            rects[index].rect.extent.height == 0U ||
            rects[index].layerCount == 0U) {
            poison_shared_command_stream(
                state, "vkCmdClearAttachments",
                "unsupported_clear_rect", shape, -ENOTSUP);
            return;
        }
        command.rects[index] = (struct bvb_vulkan_clear_rect){
            .offset_x = rects[index].rect.offset.x,
            .offset_y = rects[index].rect.offset.y,
            .width = rects[index].rect.extent.width,
            .height = rects[index].rect.extent.height,
            .base_array_layer = rects[index].baseArrayLayer,
            .layer_count = rects[index].layerCount,
        };
    }
    uint8_t bytes[BVB_PROTOCOL_MAX_PAYLOAD];
    struct bvb_command_batch_builder builder;
    int result = begin_single_render_record(state, bytes, &builder);
    if (result == 0)
        result = bvb_command_batch_append_vulkan_clear_attachments(
            &builder, &command);
    if (result == 0)
        result = finish_single_render_record(
            state, bytes, &builder, "vkCmdClearAttachments", shape);
    if (result != 0)
        poison_shared_command_stream(
            state, "vkCmdClearAttachments",
            "clear_record_rejected", shape, result);
}

static struct bvb_vulkan_image_subresource_layers
transfer_image_layers(const VkImageSubresourceLayers *layers) {
    return (struct bvb_vulkan_image_subresource_layers){
        .aspect_mask = layers->aspectMask,
        .mip_level = layers->mipLevel,
        .base_array_layer = layers->baseArrayLayer,
        .layer_count = layers->layerCount,
    };
}

static struct bvb_vulkan_offset_3d transfer_offset_3d(
    const VkOffset3D *offset) {
    return (struct bvb_vulkan_offset_3d){
        .x = offset->x, .y = offset->y, .z = offset->z};
}

static struct bvb_vulkan_extent_3d transfer_extent_3d(
    const VkExtent3D *extent) {
    return (struct bvb_vulkan_extent_3d){
        .width = extent->width, .height = extent->height,
        .depth = extent->depth};
}

static enum bvb_object_type transfer_source_type(uint16_t opcode) {
    return opcode == BVB_COMMAND_VULKAN_COPY_BUFFER_2 ||
                   opcode == BVB_COMMAND_VULKAN_COPY_BUFFER_TO_IMAGE_2
               ? BVB_OBJECT_BUFFER : BVB_OBJECT_IMAGE;
}

static enum bvb_object_type transfer_destination_type(uint16_t opcode) {
    return opcode == BVB_COMMAND_VULKAN_COPY_BUFFER_2 ||
                   opcode == BVB_COMMAND_VULKAN_COPY_IMAGE_TO_BUFFER_2
               ? BVB_OBJECT_BUFFER : BVB_OBJECT_IMAGE;
}

static void submit_transfer_command(
    struct bvb_command_buffer_proxy *state, uint16_t opcode,
    const struct bvb_vulkan_transfer_command *command,
    const char *entry, const char *shape) {
    if (state == NULL || command == NULL) return;
    if (command_stream_is_enabled()) {
        if (pthread_mutex_lock(&state->stream_mutex) != 0) return;
        int result = 0;
        int owned = shared_object_owned_by_device_cached_locked(
            state, command->source_id, transfer_source_type(opcode));
        if (owned <= 0) result = owned < 0 ? owned : -EINVAL;
        if (result == 0) {
            owned = shared_object_owned_by_device_cached_locked(
                state, command->destination_id,
                transfer_destination_type(opcode));
            if (owned <= 0) result = owned < 0 ? owned : -EINVAL;
        }
        if (result == 0 && state->stream_recording &&
            !state->stream_error) {
            result = bvb_command_batch_append_vulkan_transfer(
                &state->stream_builder, opcode, command);
        } else if (result == 0) {
            result = -EINVAL;
        }
        if (result != 0) {
            const char *reason = result == -ENOSPC
                ? "command_stream_slot_exhausted"
                : "ownership_or_transfer_append_rejected";
            store_command_diagnostic_locked(
                state, entry, reason, shape, result);
            state->stream_error = true;
            state->stream_sealed = false;
        }
        (void)pthread_mutex_unlock(&state->stream_mutex);
        return;
    }
    uint8_t bytes[BVB_PROTOCOL_MAX_PAYLOAD];
    struct bvb_command_batch_builder builder;
    int result = begin_single_render_record(state, bytes, &builder);
    if (result == 0)
        result = bvb_command_batch_append_vulkan_transfer(
            &builder, opcode, command);
    if (result == 0)
        result = finish_single_render_record(
            state, bytes, &builder, entry, shape);
    if (result != 0)
        poison_shared_command_stream(
            state, entry, "transfer_record_rejected", shape, result);
}

static void VKAPI_CALL bvb_bridge_vkCmdCopyBuffer2(
    VkCommandBuffer command_buffer, const VkCopyBufferInfo2 *info) {
    struct bvb_command_buffer_proxy *state =
        command_buffer_proxy(command_buffer);
    const char *shape = "VkCopyBufferInfo2_ptr";
    if (state == NULL || info == NULL ||
        info->sType != VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2 ||
        info->pNext != NULL || info->srcBuffer == VK_NULL_HANDLE ||
        info->dstBuffer == VK_NULL_HANDLE || info->regionCount == 0U ||
        info->regionCount > BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS ||
        info->pRegions == NULL) {
        poison_shared_command_stream(state, "vkCmdCopyBuffer2",
                                     "unsupported_transfer_shape", shape,
                                     -ENOTSUP);
        return;
    }
    struct bvb_vulkan_transfer_command command = {
        .source_id = non_dispatchable_wire_id(
            &info->srcBuffer, sizeof(info->srcBuffer)),
        .destination_id = non_dispatchable_wire_id(
            &info->dstBuffer, sizeof(info->dstBuffer)),
        .region_count = info->regionCount,
    };
    for (uint32_t index = 0U; index < info->regionCount; ++index) {
        const VkBufferCopy2 *region = &info->pRegions[index];
        if (region->sType != VK_STRUCTURE_TYPE_BUFFER_COPY_2 ||
            region->pNext != NULL || region->size == 0U) {
            poison_shared_command_stream(state, "vkCmdCopyBuffer2",
                                         "unsupported_transfer_region",
                                         shape, -ENOTSUP);
            return;
        }
        command.regions[index].source_buffer_offset = region->srcOffset;
        command.regions[index].destination_buffer_offset = region->dstOffset;
        command.regions[index].size = region->size;
    }
    submit_transfer_command(state, BVB_COMMAND_VULKAN_COPY_BUFFER_2,
                            &command, "vkCmdCopyBuffer2", shape);
}

static void VKAPI_CALL bvb_bridge_vkCmdCopyBufferToImage2(
    VkCommandBuffer command_buffer,
    const VkCopyBufferToImageInfo2 *info) {
    struct bvb_command_buffer_proxy *state =
        command_buffer_proxy(command_buffer);
    const char *shape = "VkCopyBufferToImageInfo2_ptr";
    if (state == NULL || info == NULL ||
        info->sType != VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2 ||
        info->pNext != NULL || info->srcBuffer == VK_NULL_HANDLE ||
        info->dstImage == VK_NULL_HANDLE ||
        info->dstImageLayout == VK_IMAGE_LAYOUT_UNDEFINED ||
        info->regionCount == 0U ||
        info->regionCount > BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS ||
        info->pRegions == NULL) {
        poison_shared_command_stream(state, "vkCmdCopyBufferToImage2",
                                     "unsupported_transfer_shape", shape,
                                     -ENOTSUP);
        return;
    }
    struct bvb_vulkan_transfer_command command = {
        .source_id = non_dispatchable_wire_id(
            &info->srcBuffer, sizeof(info->srcBuffer)),
        .destination_id = non_dispatchable_wire_id(
            &info->dstImage, sizeof(info->dstImage)),
        .destination_layout = info->dstImageLayout,
        .region_count = info->regionCount,
    };
    for (uint32_t index = 0U; index < info->regionCount; ++index) {
        const VkBufferImageCopy2 *region = &info->pRegions[index];
        if (region->sType != VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2 ||
            region->pNext != NULL || region->imageSubresource.aspectMask == 0U ||
            region->imageSubresource.layerCount == 0U ||
            region->imageExtent.width == 0U ||
            region->imageExtent.height == 0U ||
            region->imageExtent.depth == 0U) {
            poison_shared_command_stream(
                state, "vkCmdCopyBufferToImage2",
                "unsupported_transfer_region", shape, -ENOTSUP);
            return;
        }
        struct bvb_vulkan_transfer_region *wire = &command.regions[index];
        wire->source_buffer_offset = region->bufferOffset;
        wire->buffer_row_length = region->bufferRowLength;
        wire->buffer_image_height = region->bufferImageHeight;
        wire->destination_layers =
            transfer_image_layers(&region->imageSubresource);
        wire->destination_offsets[0] =
            transfer_offset_3d(&region->imageOffset);
        wire->extent = transfer_extent_3d(&region->imageExtent);
    }
    submit_transfer_command(
        state, BVB_COMMAND_VULKAN_COPY_BUFFER_TO_IMAGE_2, &command,
        "vkCmdCopyBufferToImage2", shape);
}

static void VKAPI_CALL bvb_bridge_vkCmdCopyImageToBuffer2(
    VkCommandBuffer command_buffer,
    const VkCopyImageToBufferInfo2 *info) {
    struct bvb_command_buffer_proxy *state =
        command_buffer_proxy(command_buffer);
    const char *shape = "VkCopyImageToBufferInfo2_ptr";
    if (state == NULL || info == NULL ||
        info->sType != VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2 ||
        info->pNext != NULL || info->srcImage == VK_NULL_HANDLE ||
        info->dstBuffer == VK_NULL_HANDLE ||
        info->srcImageLayout == VK_IMAGE_LAYOUT_UNDEFINED ||
        info->regionCount == 0U ||
        info->regionCount > BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS ||
        info->pRegions == NULL) {
        poison_shared_command_stream(state, "vkCmdCopyImageToBuffer2",
                                     "unsupported_transfer_shape", shape,
                                     -ENOTSUP);
        return;
    }
    struct bvb_vulkan_transfer_command command = {
        .source_id = non_dispatchable_wire_id(
            &info->srcImage, sizeof(info->srcImage)),
        .destination_id = non_dispatchable_wire_id(
            &info->dstBuffer, sizeof(info->dstBuffer)),
        .source_layout = info->srcImageLayout,
        .region_count = info->regionCount,
    };
    for (uint32_t index = 0U; index < info->regionCount; ++index) {
        const VkBufferImageCopy2 *region = &info->pRegions[index];
        if (region->sType != VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2 ||
            region->pNext != NULL || region->imageSubresource.aspectMask == 0U ||
            region->imageSubresource.layerCount == 0U ||
            region->imageExtent.width == 0U ||
            region->imageExtent.height == 0U ||
            region->imageExtent.depth == 0U) {
            poison_shared_command_stream(
                state, "vkCmdCopyImageToBuffer2",
                "unsupported_transfer_region", shape, -ENOTSUP);
            return;
        }
        struct bvb_vulkan_transfer_region *wire = &command.regions[index];
        wire->destination_buffer_offset = region->bufferOffset;
        wire->buffer_row_length = region->bufferRowLength;
        wire->buffer_image_height = region->bufferImageHeight;
        wire->source_layers = transfer_image_layers(&region->imageSubresource);
        wire->source_offsets[0] = transfer_offset_3d(&region->imageOffset);
        wire->extent = transfer_extent_3d(&region->imageExtent);
    }
    submit_transfer_command(
        state, BVB_COMMAND_VULKAN_COPY_IMAGE_TO_BUFFER_2, &command,
        "vkCmdCopyImageToBuffer2", shape);
}

static int transfer_image_regions(
    struct bvb_command_buffer_proxy *state, const char *entry,
    const char *shape, uint32_t region_count,
    const VkImageCopy2 *regions,
    struct bvb_vulkan_transfer_command *command) {
    for (uint32_t index = 0U; index < region_count; ++index) {
        const VkImageCopy2 *region = &regions[index];
        if (region->sType != VK_STRUCTURE_TYPE_IMAGE_COPY_2 ||
            region->pNext != NULL || region->srcSubresource.aspectMask == 0U ||
            region->srcSubresource.layerCount == 0U ||
            region->dstSubresource.aspectMask == 0U ||
            region->dstSubresource.layerCount == 0U ||
            region->extent.width == 0U || region->extent.height == 0U ||
            region->extent.depth == 0U) {
            poison_shared_command_stream(state, entry,
                                         "unsupported_transfer_region",
                                         shape, -ENOTSUP);
            return -ENOTSUP;
        }
        struct bvb_vulkan_transfer_region *wire = &command->regions[index];
        wire->source_layers = transfer_image_layers(&region->srcSubresource);
        wire->destination_layers =
            transfer_image_layers(&region->dstSubresource);
        wire->source_offsets[0] = transfer_offset_3d(&region->srcOffset);
        wire->destination_offsets[0] = transfer_offset_3d(&region->dstOffset);
        wire->extent = transfer_extent_3d(&region->extent);
    }
    return 0;
}

static void VKAPI_CALL bvb_bridge_vkCmdCopyImage2(
    VkCommandBuffer command_buffer, const VkCopyImageInfo2 *info) {
    struct bvb_command_buffer_proxy *state =
        command_buffer_proxy(command_buffer);
    const char *shape = "VkCopyImageInfo2_ptr";
    if (state == NULL || info == NULL ||
        info->sType != VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2 ||
        info->pNext != NULL || info->srcImage == VK_NULL_HANDLE ||
        info->dstImage == VK_NULL_HANDLE ||
        info->srcImageLayout == VK_IMAGE_LAYOUT_UNDEFINED ||
        info->dstImageLayout == VK_IMAGE_LAYOUT_UNDEFINED ||
        info->regionCount == 0U ||
        info->regionCount > BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS ||
        info->pRegions == NULL) {
        poison_shared_command_stream(state, "vkCmdCopyImage2",
                                     "unsupported_transfer_shape", shape,
                                     -ENOTSUP);
        return;
    }
    struct bvb_vulkan_transfer_command command = {
        .source_id = non_dispatchable_wire_id(
            &info->srcImage, sizeof(info->srcImage)),
        .destination_id = non_dispatchable_wire_id(
            &info->dstImage, sizeof(info->dstImage)),
        .source_layout = info->srcImageLayout,
        .destination_layout = info->dstImageLayout,
        .region_count = info->regionCount,
    };
    if (transfer_image_regions(state, "vkCmdCopyImage2", shape,
                               info->regionCount, info->pRegions,
                               &command) != 0) return;
    submit_transfer_command(state, BVB_COMMAND_VULKAN_COPY_IMAGE_2,
                            &command, "vkCmdCopyImage2", shape);
}

static void VKAPI_CALL bvb_bridge_vkCmdBlitImage2(
    VkCommandBuffer command_buffer, const VkBlitImageInfo2 *info) {
    struct bvb_command_buffer_proxy *state =
        command_buffer_proxy(command_buffer);
    const char *shape = "VkBlitImageInfo2_ptr";
    if (state == NULL || info == NULL ||
        info->sType != VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2 ||
        info->pNext != NULL || info->srcImage == VK_NULL_HANDLE ||
        info->dstImage == VK_NULL_HANDLE ||
        info->srcImageLayout == VK_IMAGE_LAYOUT_UNDEFINED ||
        info->dstImageLayout == VK_IMAGE_LAYOUT_UNDEFINED ||
        (info->filter != VK_FILTER_NEAREST &&
         info->filter != VK_FILTER_LINEAR) || info->regionCount == 0U ||
        info->regionCount > BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS ||
        info->pRegions == NULL) {
        poison_shared_command_stream(state, "vkCmdBlitImage2",
                                     "unsupported_transfer_shape", shape,
                                     -ENOTSUP);
        return;
    }
    struct bvb_vulkan_transfer_command command = {
        .source_id = non_dispatchable_wire_id(
            &info->srcImage, sizeof(info->srcImage)),
        .destination_id = non_dispatchable_wire_id(
            &info->dstImage, sizeof(info->dstImage)),
        .source_layout = info->srcImageLayout,
        .destination_layout = info->dstImageLayout,
        .filter = info->filter,
        .region_count = info->regionCount,
    };
    for (uint32_t index = 0U; index < info->regionCount; ++index) {
        const VkImageBlit2 *region = &info->pRegions[index];
        if (region->sType != VK_STRUCTURE_TYPE_IMAGE_BLIT_2 ||
            region->pNext != NULL || region->srcSubresource.aspectMask == 0U ||
            region->srcSubresource.layerCount == 0U ||
            region->dstSubresource.aspectMask == 0U ||
            region->dstSubresource.layerCount == 0U) {
            poison_shared_command_stream(state, "vkCmdBlitImage2",
                                         "unsupported_transfer_region",
                                         shape, -ENOTSUP);
            return;
        }
        struct bvb_vulkan_transfer_region *wire = &command.regions[index];
        wire->source_layers = transfer_image_layers(&region->srcSubresource);
        wire->destination_layers =
            transfer_image_layers(&region->dstSubresource);
        for (uint32_t offset = 0U; offset < 2U; ++offset) {
            wire->source_offsets[offset] =
                transfer_offset_3d(&region->srcOffsets[offset]);
            wire->destination_offsets[offset] =
                transfer_offset_3d(&region->dstOffsets[offset]);
        }
        wire->extent = (struct bvb_vulkan_extent_3d){1U, 1U, 1U};
    }
    submit_transfer_command(state, BVB_COMMAND_VULKAN_BLIT_IMAGE_2,
                            &command, "vkCmdBlitImage2", shape);
}

static void VKAPI_CALL bvb_bridge_vkCmdResolveImage2(
    VkCommandBuffer command_buffer, const VkResolveImageInfo2 *info) {
    struct bvb_command_buffer_proxy *state =
        command_buffer_proxy(command_buffer);
    const char *shape = "VkResolveImageInfo2_ptr";
    if (state == NULL || info == NULL ||
        info->sType != VK_STRUCTURE_TYPE_RESOLVE_IMAGE_INFO_2 ||
        info->pNext != NULL || info->srcImage == VK_NULL_HANDLE ||
        info->dstImage == VK_NULL_HANDLE ||
        info->srcImageLayout == VK_IMAGE_LAYOUT_UNDEFINED ||
        info->dstImageLayout == VK_IMAGE_LAYOUT_UNDEFINED ||
        info->regionCount == 0U ||
        info->regionCount > BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS ||
        info->pRegions == NULL) {
        poison_shared_command_stream(state, "vkCmdResolveImage2",
                                     "unsupported_transfer_shape", shape,
                                     -ENOTSUP);
        return;
    }
    struct bvb_vulkan_transfer_command command = {
        .source_id = non_dispatchable_wire_id(
            &info->srcImage, sizeof(info->srcImage)),
        .destination_id = non_dispatchable_wire_id(
            &info->dstImage, sizeof(info->dstImage)),
        .source_layout = info->srcImageLayout,
        .destination_layout = info->dstImageLayout,
        .region_count = info->regionCount,
    };
    for (uint32_t index = 0U; index < info->regionCount; ++index) {
        const VkImageResolve2 *region = &info->pRegions[index];
        if (region->sType != VK_STRUCTURE_TYPE_IMAGE_RESOLVE_2 ||
            region->pNext != NULL || region->srcSubresource.aspectMask == 0U ||
            region->srcSubresource.layerCount == 0U ||
            region->dstSubresource.aspectMask == 0U ||
            region->dstSubresource.layerCount == 0U ||
            region->extent.width == 0U || region->extent.height == 0U ||
            region->extent.depth == 0U) {
            poison_shared_command_stream(state, "vkCmdResolveImage2",
                                         "unsupported_transfer_region",
                                         shape, -ENOTSUP);
            return;
        }
        struct bvb_vulkan_transfer_region *wire = &command.regions[index];
        wire->source_layers = transfer_image_layers(&region->srcSubresource);
        wire->destination_layers =
            transfer_image_layers(&region->dstSubresource);
        wire->source_offsets[0] = transfer_offset_3d(&region->srcOffset);
        wire->destination_offsets[0] = transfer_offset_3d(&region->dstOffset);
        wire->extent = transfer_extent_3d(&region->extent);
    }
    submit_transfer_command(state, BVB_COMMAND_VULKAN_RESOLVE_IMAGE_2,
                            &command, "vkCmdResolveImage2", shape);
}

static void VKAPI_CALL bvb_bridge_vkCmdUpdateBuffer(
    VkCommandBuffer command_buffer, VkBuffer destination_buffer,
    VkDeviceSize destination_offset, VkDeviceSize data_size,
    const void *data) {
    struct bvb_command_buffer_proxy *state =
        command_buffer_proxy(command_buffer);
    const char *shape =
        "VkBuffer_value,VkDeviceSize_value,VkDeviceSize_value,void_ptr";
    if (state == NULL || destination_buffer == VK_NULL_HANDLE || data == NULL ||
        data_size == 0U ||
        data_size > BVB_COMMAND_VULKAN_MAX_UPDATE_BUFFER_BYTES ||
        (destination_offset & 3U) != 0U || (data_size & 3U) != 0U) {
        poison_shared_command_stream(state, "vkCmdUpdateBuffer",
                                     "unsupported_update_shape", shape,
                                     -ENOTSUP);
        return;
    }
    struct bvb_vulkan_update_buffer_command command = {
        .buffer_id = non_dispatchable_wire_id(
            &destination_buffer, sizeof(destination_buffer)),
        .offset = destination_offset,
        .data_size = (uint32_t)data_size,
    };
    memcpy(command.data, data, (size_t)data_size);
    if (command_stream_is_enabled()) {
        if (pthread_mutex_lock(&state->stream_mutex) != 0) return;
        int result = state->stream_recording && !state->stream_error
                         ? 0 : -EINVAL;
        if (result == 0) {
            const int owned = shared_object_owned_by_device_cached_locked(
                state, command.buffer_id, BVB_OBJECT_BUFFER);
            result = owned > 0 ? 0 : owned < 0 ? owned : -EINVAL;
        }
        if (result == 0)
            result = bvb_command_batch_append_vulkan_update_buffer(
                &state->stream_builder, &command);
        if (result != 0) {
            store_command_diagnostic_locked(
                state, "vkCmdUpdateBuffer",
                "ownership_or_update_append_rejected", shape, result);
            state->stream_error = true;
            state->stream_sealed = false;
        }
        (void)pthread_mutex_unlock(&state->stream_mutex);
        return;
    }
    uint8_t bytes[BVB_PROTOCOL_MAX_PAYLOAD];
    struct bvb_command_batch_builder builder;
    int result = begin_single_render_record(state, bytes, &builder);
    if (result == 0)
        result = bvb_command_batch_append_vulkan_update_buffer(
            &builder, &command);
    if (result == 0)
        result = finish_single_render_record(
            state, bytes, &builder, "vkCmdUpdateBuffer", shape);
    if (result != 0)
        poison_shared_command_stream(
            state, "vkCmdUpdateBuffer", "update_record_rejected", shape,
            result);
}

static void VKAPI_CALL bvb_bridge_vkCmdCopyBuffer(
    VkCommandBuffer command_buffer, VkBuffer source, VkBuffer destination,
    uint32_t region_count, const VkBufferCopy *regions) {
    VkBufferCopy2 converted[BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS];
    if (region_count == 0U ||
        region_count > BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS ||
        regions == NULL) {
        poison_shared_command_stream(command_buffer_proxy(command_buffer),
                                     "vkCmdCopyBuffer",
                                     "unsupported_transfer_shape",
                                     "VkBuffer_value,VkBuffer_value,uint32_t_value,VkBufferCopy_ptr",
                                     -ENOTSUP);
        return;
    }
    for (uint32_t index = 0U; index < region_count; ++index)
        converted[index] = (VkBufferCopy2){
            .sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
            .srcOffset = regions[index].srcOffset,
            .dstOffset = regions[index].dstOffset,
            .size = regions[index].size,
        };
    bvb_bridge_vkCmdCopyBuffer2(command_buffer, &(const VkCopyBufferInfo2){
        .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
        .srcBuffer = source, .dstBuffer = destination,
        .regionCount = region_count, .pRegions = converted});
}

static void VKAPI_CALL bvb_bridge_vkCmdCopyBufferToImage(
    VkCommandBuffer command_buffer, VkBuffer source, VkImage destination,
    VkImageLayout layout, uint32_t region_count,
    const VkBufferImageCopy *regions) {
    VkBufferImageCopy2 converted[BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS];
    if (region_count == 0U ||
        region_count > BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS ||
        regions == NULL) {
        poison_shared_command_stream(command_buffer_proxy(command_buffer),
                                     "vkCmdCopyBufferToImage",
                                     "unsupported_transfer_shape",
                                     "VkBuffer_value,VkImage_value,VkImageLayout_value,uint32_t_value,VkBufferImageCopy_ptr",
                                     -ENOTSUP);
        return;
    }
    for (uint32_t index = 0U; index < region_count; ++index)
        converted[index] = (VkBufferImageCopy2){
            .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
            .bufferOffset = regions[index].bufferOffset,
            .bufferRowLength = regions[index].bufferRowLength,
            .bufferImageHeight = regions[index].bufferImageHeight,
            .imageSubresource = regions[index].imageSubresource,
            .imageOffset = regions[index].imageOffset,
            .imageExtent = regions[index].imageExtent,
        };
    bvb_bridge_vkCmdCopyBufferToImage2(
        command_buffer, &(const VkCopyBufferToImageInfo2){
            .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
            .srcBuffer = source, .dstImage = destination,
            .dstImageLayout = layout, .regionCount = region_count,
            .pRegions = converted});
}

static void VKAPI_CALL bvb_bridge_vkCmdCopyImageToBuffer(
    VkCommandBuffer command_buffer, VkImage source, VkImageLayout layout,
    VkBuffer destination, uint32_t region_count,
    const VkBufferImageCopy *regions) {
    VkBufferImageCopy2 converted[BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS];
    if (region_count == 0U ||
        region_count > BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS ||
        regions == NULL) {
        poison_shared_command_stream(command_buffer_proxy(command_buffer),
                                     "vkCmdCopyImageToBuffer",
                                     "unsupported_transfer_shape",
                                     "VkImage_value,VkImageLayout_value,VkBuffer_value,uint32_t_value,VkBufferImageCopy_ptr",
                                     -ENOTSUP);
        return;
    }
    for (uint32_t index = 0U; index < region_count; ++index)
        converted[index] = (VkBufferImageCopy2){
            .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
            .bufferOffset = regions[index].bufferOffset,
            .bufferRowLength = regions[index].bufferRowLength,
            .bufferImageHeight = regions[index].bufferImageHeight,
            .imageSubresource = regions[index].imageSubresource,
            .imageOffset = regions[index].imageOffset,
            .imageExtent = regions[index].imageExtent,
        };
    bvb_bridge_vkCmdCopyImageToBuffer2(
        command_buffer, &(const VkCopyImageToBufferInfo2){
            .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
            .srcImage = source, .srcImageLayout = layout,
            .dstBuffer = destination, .regionCount = region_count,
            .pRegions = converted});
}

static void VKAPI_CALL bvb_bridge_vkCmdCopyImage(
    VkCommandBuffer command_buffer, VkImage source,
    VkImageLayout source_layout, VkImage destination,
    VkImageLayout destination_layout, uint32_t region_count,
    const VkImageCopy *regions) {
    VkImageCopy2 converted[BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS];
    if (region_count == 0U ||
        region_count > BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS ||
        regions == NULL) {
        poison_shared_command_stream(command_buffer_proxy(command_buffer),
                                     "vkCmdCopyImage",
                                     "unsupported_transfer_shape",
                                     "VkImage_value,VkImageLayout_value,VkImage_value,VkImageLayout_value,uint32_t_value,VkImageCopy_ptr",
                                     -ENOTSUP);
        return;
    }
    for (uint32_t index = 0U; index < region_count; ++index)
        converted[index] = (VkImageCopy2){
            .sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
            .srcSubresource = regions[index].srcSubresource,
            .srcOffset = regions[index].srcOffset,
            .dstSubresource = regions[index].dstSubresource,
            .dstOffset = regions[index].dstOffset,
            .extent = regions[index].extent,
        };
    bvb_bridge_vkCmdCopyImage2(command_buffer, &(const VkCopyImageInfo2){
        .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2,
        .srcImage = source, .srcImageLayout = source_layout,
        .dstImage = destination, .dstImageLayout = destination_layout,
        .regionCount = region_count, .pRegions = converted});
}

static void VKAPI_CALL bvb_bridge_vkCmdBlitImage(
    VkCommandBuffer command_buffer, VkImage source,
    VkImageLayout source_layout, VkImage destination,
    VkImageLayout destination_layout, uint32_t region_count,
    const VkImageBlit *regions, VkFilter filter) {
    VkImageBlit2 converted[BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS];
    if (region_count == 0U ||
        region_count > BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS ||
        regions == NULL) {
        poison_shared_command_stream(command_buffer_proxy(command_buffer),
                                     "vkCmdBlitImage",
                                     "unsupported_transfer_shape",
                                     "VkImage_value,VkImageLayout_value,VkImage_value,VkImageLayout_value,uint32_t_value,VkImageBlit_ptr,VkFilter_value",
                                     -ENOTSUP);
        return;
    }
    for (uint32_t index = 0U; index < region_count; ++index) {
        converted[index] = (VkImageBlit2){
            .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
            .srcSubresource = regions[index].srcSubresource,
            .dstSubresource = regions[index].dstSubresource,
        };
        memcpy(converted[index].srcOffsets, regions[index].srcOffsets,
               sizeof(converted[index].srcOffsets));
        memcpy(converted[index].dstOffsets, regions[index].dstOffsets,
               sizeof(converted[index].dstOffsets));
    }
    bvb_bridge_vkCmdBlitImage2(command_buffer, &(const VkBlitImageInfo2){
        .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
        .srcImage = source, .srcImageLayout = source_layout,
        .dstImage = destination, .dstImageLayout = destination_layout,
        .regionCount = region_count, .pRegions = converted, .filter = filter});
}

static void VKAPI_CALL bvb_bridge_vkCmdResolveImage(
    VkCommandBuffer command_buffer, VkImage source,
    VkImageLayout source_layout, VkImage destination,
    VkImageLayout destination_layout, uint32_t region_count,
    const VkImageResolve *regions) {
    VkImageResolve2 converted[BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS];
    if (region_count == 0U ||
        region_count > BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS ||
        regions == NULL) {
        poison_shared_command_stream(command_buffer_proxy(command_buffer),
                                     "vkCmdResolveImage",
                                     "unsupported_transfer_shape",
                                     "VkImage_value,VkImageLayout_value,VkImage_value,VkImageLayout_value,uint32_t_value,VkImageResolve_ptr",
                                     -ENOTSUP);
        return;
    }
    for (uint32_t index = 0U; index < region_count; ++index)
        converted[index] = (VkImageResolve2){
            .sType = VK_STRUCTURE_TYPE_IMAGE_RESOLVE_2,
            .srcSubresource = regions[index].srcSubresource,
            .srcOffset = regions[index].srcOffset,
            .dstSubresource = regions[index].dstSubresource,
            .dstOffset = regions[index].dstOffset,
            .extent = regions[index].extent,
        };
    bvb_bridge_vkCmdResolveImage2(command_buffer,
        &(const VkResolveImageInfo2){
            .sType = VK_STRUCTURE_TYPE_RESOLVE_IMAGE_INFO_2,
            .srcImage = source, .srcImageLayout = source_layout,
            .dstImage = destination, .dstImageLayout = destination_layout,
            .regionCount = region_count, .pRegions = converted});
}

static void VKAPI_CALL bvb_bridge_vkCmdBindDescriptorSets(
    VkCommandBuffer command_buffer, VkPipelineBindPoint pipeline_bind_point,
    VkPipelineLayout layout, uint32_t first_set,
    uint32_t descriptor_set_count, const VkDescriptorSet *descriptor_sets,
    uint32_t dynamic_offset_count, const uint32_t *dynamic_offsets) {
    struct bvb_command_buffer_proxy *command_state =
        command_buffer_proxy(command_buffer);
    if (command_state == NULL ||
        (pipeline_bind_point != VK_PIPELINE_BIND_POINT_GRAPHICS &&
         pipeline_bind_point != VK_PIPELINE_BIND_POINT_COMPUTE) ||
        layout == VK_NULL_HANDLE || descriptor_set_count == 0U ||
        descriptor_set_count > BVB_VULKAN_MAX_BOUND_DESCRIPTOR_SETS ||
        descriptor_sets == NULL ||
        dynamic_offset_count > BVB_VULKAN_MAX_DYNAMIC_OFFSETS ||
        (dynamic_offset_count != 0U && dynamic_offsets == NULL)) {
        poison_shared_command_stream(
            command_state, "vkCmdBindDescriptorSets",
            "unsupported_descriptor_bind_shape",
            "VkPipelineBindPoint_value,VkPipelineLayout_value,uint32_t_value,uint32_t_value,VkDescriptorSet_ptr,uint32_t_value,uint32_t_ptr",
            -ENOTSUP);
        return;
    }
    struct bvb_vulkan_bind_descriptor_sets_request decoded = {
        .command_buffer_id = command_state->wire_id,
        .pipeline_layout_id = non_dispatchable_wire_id(
            &layout, sizeof(layout)),
        .pipeline_bind_point = pipeline_bind_point,
        .first_set = first_set,
        .descriptor_set_count = descriptor_set_count,
        .dynamic_offset_count = dynamic_offset_count,
    };
    for (uint32_t index = 0U; index < descriptor_set_count; ++index)
        decoded.descriptor_set_ids[index] = non_dispatchable_wire_id(
            &descriptor_sets[index], sizeof(descriptor_sets[index]));
    if (dynamic_offset_count != 0U)
        memcpy(decoded.dynamic_offsets, dynamic_offsets,
               dynamic_offset_count * sizeof(dynamic_offsets[0]));
    if (command_stream_is_enabled()) {
        if (pthread_mutex_lock(&command_state->stream_mutex) != 0) return;
        int owned = shared_object_owned_by_device_cached_locked(
            command_state, decoded.pipeline_layout_id,
            BVB_OBJECT_PIPELINE_LAYOUT);
        if (owned < 0) return;
        int result = owned > 0 ? 0 : -EINVAL;
        for (uint32_t index = 0U; result == 0 &&
             index < descriptor_set_count; ++index) {
            owned = shared_object_owned_by_device_cached_locked(
                command_state, decoded.descriptor_set_ids[index],
                BVB_OBJECT_DESCRIPTOR_SET);
            if (owned < 0) return;
            if (owned <= 0) result = -EINVAL;
        }
        if (result == 0 && command_state->stream_recording &&
            !command_state->stream_error) {
            struct bvb_vulkan_bind_descriptor_sets_command command = {
                .pipeline_layout_id = decoded.pipeline_layout_id,
                .pipeline_bind_point = decoded.pipeline_bind_point,
                .first_set = decoded.first_set,
                .descriptor_set_count = decoded.descriptor_set_count,
                .dynamic_offset_count = decoded.dynamic_offset_count,
            };
            memcpy(command.descriptor_set_ids, decoded.descriptor_set_ids,
                   descriptor_set_count * sizeof(decoded.descriptor_set_ids[0]));
            memcpy(command.dynamic_offsets, decoded.dynamic_offsets,
                   dynamic_offset_count * sizeof(decoded.dynamic_offsets[0]));
            result = bvb_command_batch_append_vulkan_bind_descriptor_sets(
                &command_state->stream_builder, &command);
        } else if (result == 0) {
            result = -EINVAL;
        }
        if (result != 0) {
            store_command_diagnostic_locked(
                command_state, "vkCmdBindDescriptorSets",
                "ownership_or_stream_append_rejected",
                "VkPipelineBindPoint_value,VkPipelineLayout_value,uint32_t_value,uint32_t_value,VkDescriptorSet_ptr,uint32_t_value,uint32_t_ptr",
                result);
            command_state->stream_error = true;
            command_state->stream_sealed = false;
        }
        (void)pthread_mutex_unlock(&command_state->stream_mutex);
        return;
    }
    uint8_t payload[BVB_PROTOCOL_MAX_PAYLOAD];
    uint32_t payload_length = 0U;
    int result =
        bvb_protocol_encode_vulkan_bind_descriptor_sets_request(
            payload, &decoded, &payload_length);
    if (result != 0 ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) return;
    struct bvb_resource_proxy *layout_state = resource_proxy_locked(
        decoded.pipeline_layout_id, BVB_OBJECT_PIPELINE_LAYOUT);
    if (layout_state == NULL ||
        layout_state->parent_id != command_state->device_id) result = -EINVAL;
    for (uint32_t index = 0U; result == 0 &&
         index < descriptor_set_count; ++index) {
        struct bvb_resource_proxy *set_state = resource_proxy_locked(
            decoded.descriptor_set_ids[index], BVB_OBJECT_DESCRIPTOR_SET);
        struct bvb_resource_proxy *pool_state = set_state == NULL ? NULL :
            resource_proxy_locked(set_state->parent_id,
                                  BVB_OBJECT_DESCRIPTOR_POOL);
        if (pool_state == NULL ||
            pool_state->parent_id != command_state->device_id) result = -EINVAL;
    }
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_COMMAND_BUFFER_BIND_DESCRIPTOR_SETS,
        .request_id = next_request_id_locked(),
        .payload_length = payload_length,
    };
    if (result == 0) memcpy(request.payload, payload, payload_length);
    struct bvb_protocol_packet response = {0};
    if (result == 0) result = connect_locked();
    if (result == 0) result = exchange_locked(&request, &response);
    if (result == 0 &&
        (response.header.status != 0 || response.header.payload_length != 0U))
        result = -EPROTO;
    if (result != 0)
        store_command_diagnostic_locked(
            command_state, "vkCmdBindDescriptorSets",
            "strict_transport_rejected",
            "VkPipelineBindPoint_value,VkPipelineLayout_value,uint32_t_value,uint32_t_value,VkDescriptorSet_ptr,uint32_t_value,uint32_t_ptr",
            result);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
}

static void VKAPI_CALL bvb_bridge_vkCmdPipelineBarrier2(
    VkCommandBuffer command_buffer, const VkDependencyInfo *dependency_info) {
    struct bvb_command_buffer_proxy *command_state =
        command_buffer_proxy(command_buffer);
    char dependency_shape[160] = "VkDependencyInfo_ptr";
    if (dependency_info != NULL) {
        (void)snprintf(
            dependency_shape, sizeof(dependency_shape),
            "VkDependencyInfo_ptr;flags=0x%x;memory=%u;buffer=%u;image=%u;pnext=%u",
            (unsigned)dependency_info->dependencyFlags,
            dependency_info->memoryBarrierCount,
            dependency_info->bufferMemoryBarrierCount,
            dependency_info->imageMemoryBarrierCount,
            dependency_info->pNext != NULL ? 1U : 0U);
    }
    if (command_state == NULL || dependency_info == NULL ||
        dependency_info->sType != VK_STRUCTURE_TYPE_DEPENDENCY_INFO ||
        dependency_info->pNext != NULL ||
        (dependency_info->dependencyFlags & ~UINT32_C(0x6f)) != 0U ||
        dependency_info->memoryBarrierCount >
            BVB_COMMAND_VULKAN_MAX_MEMORY_BARRIERS ||
        dependency_info->bufferMemoryBarrierCount >
            BVB_COMMAND_VULKAN_MAX_BUFFER_BARRIERS ||
        dependency_info->imageMemoryBarrierCount >
            BVB_COMMAND_VULKAN_MAX_IMAGE_BARRIERS ||
        (dependency_info->memoryBarrierCount != 0U &&
         dependency_info->pMemoryBarriers == NULL) ||
        (dependency_info->bufferMemoryBarrierCount != 0U &&
         dependency_info->pBufferMemoryBarriers == NULL) ||
        (dependency_info->imageMemoryBarrierCount != 0U &&
         dependency_info->pImageMemoryBarriers == NULL)) {
        poison_shared_command_stream(
            command_state, "vkCmdPipelineBarrier2",
            "unsupported_dependency_shape", dependency_shape,
            -ENOTSUP);
        return;
    }
    struct bvb_vulkan_image_barrier_2_command general = {
        .dependency_flags = dependency_info->dependencyFlags,
        .memory_count = dependency_info->memoryBarrierCount,
        .buffer_count = dependency_info->bufferMemoryBarrierCount,
        .image_count = dependency_info->imageMemoryBarrierCount,
    };
    for (uint32_t index = 0U; index < general.memory_count; ++index) {
        const VkMemoryBarrier2 *barrier =
            &dependency_info->pMemoryBarriers[index];
        if (barrier->sType != VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 ||
            barrier->pNext != NULL) {
            poison_shared_command_stream(
                command_state, "vkCmdPipelineBarrier2",
                "unsupported_memory_barrier_shape",
                "VkMemoryBarrier2_ptr", -ENOTSUP);
            return;
        }
        general.memory[index] = (struct bvb_vulkan_memory_barrier_2){
            .source_stage_mask = barrier->srcStageMask,
            .source_access_mask = barrier->srcAccessMask,
            .destination_stage_mask = barrier->dstStageMask,
            .destination_access_mask = barrier->dstAccessMask,
        };
    }
    for (uint32_t index = 0U; index < general.buffer_count; ++index) {
        const VkBufferMemoryBarrier2 *barrier =
            &dependency_info->pBufferMemoryBarriers[index];
        if (barrier->sType != VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 ||
            barrier->pNext != NULL || barrier->buffer == VK_NULL_HANDLE ||
            barrier->size == 0U) {
            poison_shared_command_stream(
                command_state, "vkCmdPipelineBarrier2",
                "unsupported_buffer_barrier_shape",
                "VkBufferMemoryBarrier2_ptr", -ENOTSUP);
            return;
        }
        const uint64_t buffer_id = non_dispatchable_wire_id(
            &barrier->buffer, sizeof(barrier->buffer));
        general.buffers[index] = (struct bvb_vulkan_buffer_barrier_2){
            .source_stage_mask = barrier->srcStageMask,
            .source_access_mask = barrier->srcAccessMask,
            .destination_stage_mask = barrier->dstStageMask,
            .destination_access_mask = barrier->dstAccessMask,
            .source_queue_family_index = barrier->srcQueueFamilyIndex,
            .destination_queue_family_index = barrier->dstQueueFamilyIndex,
            .buffer_id = buffer_id,
            .offset = barrier->offset,
            .size = barrier->size,
        };
    }
    for (uint32_t index = 0U; index < general.image_count; ++index) {
        const VkImageMemoryBarrier2 *barrier =
            &dependency_info->pImageMemoryBarriers[index];
        if (barrier->sType != VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 ||
            barrier->pNext != NULL ||
            !command_image_layout_supported(barrier->oldLayout) ||
            !command_image_layout_supported(barrier->newLayout) ||
            barrier->newLayout == VK_IMAGE_LAYOUT_UNDEFINED ||
            barrier->newLayout == VK_IMAGE_LAYOUT_PREINITIALIZED ||
            !command_image_barrier_range_supported(
                &barrier->subresourceRange)) {
            poison_shared_command_stream(
                command_state, "vkCmdPipelineBarrier2",
                "unsupported_image_barrier_shape",
                "VkImageMemoryBarrier2_ptr", -ENOTSUP);
            return;
        }
        const uint64_t image_id =
            non_dispatchable_wire_id(&barrier->image, sizeof(barrier->image));
        general.images[index] = (struct bvb_vulkan_image_barrier_2){
            .source_stage_mask = barrier->srcStageMask,
            .source_access_mask = barrier->srcAccessMask,
            .destination_stage_mask = barrier->dstStageMask,
            .destination_access_mask = barrier->dstAccessMask,
            .old_layout = barrier->oldLayout,
            .new_layout = barrier->newLayout,
            .source_queue_family_index = barrier->srcQueueFamilyIndex,
            .destination_queue_family_index = barrier->dstQueueFamilyIndex,
            .image_id = image_id,
            .range = command_image_range(&barrier->subresourceRange),
        };
    }
    if (command_stream_is_enabled()) {
        if (pthread_mutex_lock(&command_state->stream_mutex) != 0) return;
        int result = command_state->stream_recording &&
                             !command_state->stream_error
                         ? 0 : -EINVAL;
        for (uint32_t index = 0U;
             result == 0 && index < general.buffer_count; ++index) {
            const int owned = shared_object_owned_by_device_cached_locked(
                command_state, general.buffers[index].buffer_id,
                BVB_OBJECT_BUFFER);
            result = owned > 0 ? 0 : owned < 0 ? owned : -EINVAL;
        }
        for (uint32_t index = 0U;
             result == 0 && index < general.image_count; ++index) {
            const int owned = shared_object_owned_by_device_cached_locked(
                command_state, general.images[index].image_id,
                BVB_OBJECT_IMAGE);
            result = owned > 0 ? 0 : owned < 0 ? owned : -EINVAL;
        }
        if (result == 0)
            result = bvb_command_batch_append_vulkan_image_barrier_2(
                &command_state->stream_builder, &general);
        if (result != 0) {
            store_command_diagnostic_locked(
                command_state, "vkCmdPipelineBarrier2",
                "ownership_or_stream_append_rejected",
                "VkDependencyInfo_ptr", result);
            command_state->stream_error = true;
            command_state->stream_sealed = false;
        }
        (void)pthread_mutex_unlock(&command_state->stream_mutex);
        return;
    }
    uint8_t bytes[BVB_PROTOCOL_MAX_PAYLOAD];
    struct bvb_command_batch_builder builder;
    int result = begin_single_render_record(command_state, bytes, &builder);
    if (result == 0)
        result = bvb_command_batch_append_vulkan_image_barrier_2(
            &builder, &general);
    if (result == 0)
        result = finish_single_render_record(
            command_state, bytes, &builder, "vkCmdPipelineBarrier2",
            "VkDependencyInfo_ptr");
    if (result != 0)
        poison_shared_command_stream(
            command_state, "vkCmdPipelineBarrier2",
            "dependency_record_rejected", "VkDependencyInfo_ptr", result);
}

static void VKAPI_CALL bvb_bridge_vkCmdClearColorImage(
    VkCommandBuffer command_buffer, VkImage image, VkImageLayout image_layout,
    const VkClearColorValue *color, uint32_t range_count,
    const VkImageSubresourceRange *ranges) {
    struct bvb_command_buffer_proxy *command_state =
        command_buffer_proxy(command_buffer);
    uint32_t color_words[4] = {0};
    if (color != NULL) memcpy(color_words, color, sizeof(color_words));
    if (command_state == NULL || color == NULL ||
        (image_layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
         image_layout != VK_IMAGE_LAYOUT_GENERAL) ||
        range_count == 0U ||
        range_count > BVB_COMMAND_VULKAN_MAX_CLEAR_RANGES || ranges == NULL) {
        poison_shared_command_stream(
            command_state, "vkCmdClearColorImage",
            "unsupported_clear_shape",
            "VkImage_value,VkImageLayout_value,VkClearColorValue_ptr,uint32_t_value,VkImageSubresourceRange_ptr",
            -ENOTSUP);
        return;
    }
    bool ranges_supported = true;
    for (uint32_t index = 0U; index < range_count; ++index) {
        ranges_supported &=
            command_clear_color_range_supported(&ranges[index]) != 0;
    }
    if (!ranges_supported) {
        poison_shared_command_stream(
            command_state, "vkCmdClearColorImage",
            "unsupported_clear_range",
            "VkImage_value,VkImageLayout_value,VkClearColorValue_ptr,uint32_t_value,VkImageSubresourceRange_ptr",
            -ENOTSUP);
        return;
    }
    const bool fixed_clear_shape =
        image_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
        color_words[0] == 0U && color_words[1] == 0U &&
        color_words[2] == 0U && color_words[3] == 0U && range_count == 1U &&
        init_image_subresource_range_supported(ranges);
    const bool shared_stream = command_stream_is_enabled();
    if (!fixed_clear_shape && !shared_stream) {
        bvb_global_diagnostic_poison_command(
            command_buffer, "vkCmdClearColorImage",
            "strict_transport_shape_unimplemented",
            "VkImage_value,VkImageLayout_value,VkClearColorValue_ptr,uint32_t_value,VkImageSubresourceRange_ptr",
            -ENOTSUP);
        return;
    }
    const uint64_t image_id =
        non_dispatchable_wire_id(&image, sizeof(image));
    if (shared_stream) {
        if (pthread_mutex_lock(&command_state->stream_mutex) != 0) return;
        const int owned = shared_object_owned_by_device_cached_locked(
            command_state, image_id, BVB_OBJECT_IMAGE);
        if (owned < 0) return;
        int result = owned != 0 ? 0 : -EINVAL;
        if (result == 0 && command_state->stream_recording &&
            !command_state->stream_error) {
            if (fixed_clear_shape) {
                result = bvb_command_batch_append_vulkan_clear_color_image(
                    &command_state->stream_builder,
                    &(const struct bvb_vulkan_clear_color_image_command){
                        .image_id = image_id,
                    });
            } else {
                struct bvb_vulkan_clear_color_image_general_command command = {
                    .image_id = image_id,
                    .image_layout = image_layout,
                    .range_count = range_count,
                };
                memcpy(command.color_words, color_words,
                       sizeof(command.color_words));
                for (uint32_t index = 0U; index < range_count; ++index) {
                    command.ranges[index] = command_image_range(&ranges[index]);
                }
                result =
                    bvb_command_batch_append_vulkan_clear_color_image_general(
                        &command_state->stream_builder, &command);
            }
        } else {
            result = -EINVAL;
        }
        if (result != 0) {
            store_command_diagnostic_locked(
                command_state, "vkCmdClearColorImage",
                "ownership_or_stream_append_rejected",
                "VkImage_value,VkImageLayout_value,VkClearColorValue_ptr,uint32_t_value,VkImageSubresourceRange_ptr",
                result);
            command_state->stream_error = true;
            command_state->stream_sealed = false;
        }
        (void)pthread_mutex_unlock(&command_state->stream_mutex);
        return;
    }
    if (pthread_mutex_lock(&bvb_global_client.mutex) != 0) return;
    int result = image_owned_by_device_locked(image_id,
                                              command_state->device_id)
                     ? 0 : -EINVAL;
    if (result == 0) result = connect_locked();
    const struct bvb_vulkan_command_buffer_clear_color_image_request decoded = {
        .command_buffer_id = command_state->wire_id,
        .image_id = image_id,
    };
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_COMMAND_BUFFER_CLEAR_COLOR_IMAGE,
        .request_id = next_request_id_locked(),
        .payload_length =
            BVB_VULKAN_COMMAND_BUFFER_CLEAR_COLOR_IMAGE_REQUEST_SIZE,
    };
    if (result == 0)
        result =
            bvb_protocol_encode_vulkan_command_buffer_clear_color_image_request(
                request.payload, &decoded);
    struct bvb_protocol_packet response = {0};
    if (result == 0) result = exchange_locked(&request, &response);
    if (result == 0 &&
        (response.header.status != 0 || response.header.payload_length != 0U))
        result = -EPROTO;
    if (result != 0)
        store_command_diagnostic_locked(
            command_state, "vkCmdClearColorImage", "strict_transport_rejected",
            "VkImage_value,VkImageLayout_value,VkClearColorValue_ptr,uint32_t_value,VkImageSubresourceRange_ptr",
            result);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
}

static void VKAPI_CALL bvb_bridge_vkCmdFillBuffer(
    VkCommandBuffer command_buffer, VkBuffer destination_buffer,
    VkDeviceSize destination_offset, VkDeviceSize size, uint32_t data) {
    struct bvb_command_buffer_proxy *command_state =
        command_buffer_proxy(command_buffer);
    const uint64_t buffer_id = non_dispatchable_wire_id(
        &destination_buffer, sizeof(destination_buffer));
    if (command_state == NULL || size == 0U || (destination_offset & 3U) != 0U ||
        (size & 3U) != 0U) {
        poison_shared_command_stream(
            command_state, "vkCmdFillBuffer", "unsupported_fill_shape",
            "VkBuffer_value,VkDeviceSize_value,VkDeviceSize_value,uint32_t_value",
            -ENOTSUP);
        return;
    }
    if (command_stream_is_enabled()) {
        if (pthread_mutex_lock(&command_state->stream_mutex) != 0) return;
        const int owned = shared_object_owned_by_device_cached_locked(
            command_state, buffer_id, BVB_OBJECT_BUFFER);
        if (owned < 0) return;
        int result = owned != 0 ? 0 : -EINVAL;
        if (result == 0 && command_state->stream_recording &&
            !command_state->stream_error) {
            result = bvb_command_batch_append_fill_buffer(
                &command_state->stream_builder,
                &(const struct bvb_fill_buffer_command){
                    .buffer_id = buffer_id,
                    .offset = destination_offset,
                    .size = size,
                    .data = data,
                });
        } else {
            result = -EINVAL;
        }
        if (result != 0) {
            store_command_diagnostic_locked(
                command_state, "vkCmdFillBuffer",
                "ownership_or_stream_append_rejected",
                "VkBuffer_value,VkDeviceSize_value,VkDeviceSize_value,uint32_t_value",
                result);
            command_state->stream_error = true;
            command_state->stream_sealed = false;
        }
        (void)pthread_mutex_unlock(&command_state->stream_mutex);
        return;
    }
    if (pthread_mutex_lock(&bvb_global_client.mutex) != 0) return;
    struct bvb_resource_proxy *buffer_state =
        resource_proxy_locked(buffer_id, BVB_OBJECT_BUFFER);
    int result = buffer_state != NULL &&
                         buffer_state->parent_id == command_state->device_id
                     ? 0 : -EINVAL;
    if (result == 0) result = connect_locked();
    const struct bvb_vulkan_command_buffer_fill_request decoded = {
        .command_buffer_id = command_state->wire_id,
        .buffer_id = buffer_id,
        .offset = destination_offset,
        .size = size,
        .data = data,
    };
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_COMMAND_BUFFER_FILL,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_COMMAND_BUFFER_FILL_REQUEST_SIZE,
    };
    if (result == 0)
        result = bvb_protocol_encode_vulkan_command_buffer_fill_request(
            request.payload, &decoded);
    struct bvb_protocol_packet response = {0};
    if (result == 0) result = exchange_locked(&request, &response);
    if (result == 0 &&
        (response.header.status != 0 || response.header.payload_length != 0U))
        result = -EPROTO;
    if (result != 0)
        store_command_diagnostic_locked(
            command_state, "vkCmdFillBuffer", "strict_transport_rejected",
            "VkBuffer_value,VkDeviceSize_value,VkDeviceSize_value,uint32_t_value",
            result);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
}

BVB_GLOBAL_EXPORT int bvb_verify_memory_fill(
    VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
    uint32_t expected_word, uint32_t *mismatched_words) {
    if (mismatched_words == NULL) return -EINVAL;
    *mismatched_words = UINT32_MAX;
    const uint64_t memory_id = non_dispatchable_wire_id(&memory, sizeof(memory));
    if (pthread_mutex_lock(&bvb_global_client.mutex) != 0) return -EDEADLK;
    struct bvb_resource_proxy *state =
        resource_proxy_locked(memory_id, BVB_OBJECT_DEVICE_MEMORY);
    int result = state == NULL ? -EINVAL : connect_locked();
    const struct bvb_vulkan_memory_verify_fill_request decoded = {
        .memory_id = memory_id,
        .offset = offset,
        .size = size,
        .expected_word = expected_word,
    };
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_MEMORY_VERIFY_FILL,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_MEMORY_VERIFY_FILL_REQUEST_SIZE,
    };
    if (result == 0)
        result = bvb_protocol_encode_vulkan_memory_verify_fill_request(
            request.payload, &decoded);
    struct bvb_protocol_packet response = {0};
    if (result == 0) result = exchange_locked(&request, &response);
    if (result == 0 &&
        (response.header.status != 0 || response.header.payload_length !=
             BVB_VULKAN_MEMORY_VERIFY_FILL_RESPONSE_SIZE)) result = -EPROTO;
    struct bvb_vulkan_memory_verify_fill_response verified = {0};
    if (result == 0)
        result = bvb_protocol_decode_vulkan_memory_verify_fill_response(
            response.payload, &verified);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result == 0 && verified.vulkan_result != VK_SUCCESS) result = -EIO;
    if (result == 0) *mismatched_words = verified.mismatched_words;
    return result;
}

static VkResult VKAPI_CALL bvb_bridge_vkCreateFence(
    VkDevice device, const VkFenceCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkFence *fence) {
    if (fence != NULL) *fence = VK_NULL_HANDLE;
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || create_info == NULL || fence == NULL ||
        allocator != NULL ||
        create_info->sType != VK_STRUCTURE_TYPE_FENCE_CREATE_INFO ||
        create_info->pNext != NULL ||
        (create_info->flags & ~VK_FENCE_CREATE_SIGNALED_BIT) != 0U)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    struct bvb_resource_proxy *state = calloc(1, sizeof(*state));
    if (state == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
    const struct bvb_vulkan_fence_create_request decoded = {
        .device_id = device_state->wire_id,
        .flags = create_info->flags,
    };
    uint8_t payload[BVB_VULKAN_FENCE_CREATE_REQUEST_SIZE];
    int result = bvb_protocol_encode_vulkan_fence_create_request(
        payload, &decoded);
    if (result != 0 || pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        free(state);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    uint64_t wire_id = 0U;
    VkResult vulkan_result = create_resource_locked(
        BVB_OPCODE_VULKAN_FENCE_CREATE, payload, sizeof(payload),
        BVB_OBJECT_FENCE, device_state->wire_id, state, &wire_id);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (vulkan_result != VK_SUCCESS) {
        free(state);
        return vulkan_result;
    }
    memcpy(fence, &wire_id, sizeof(*fence));
    return VK_SUCCESS;
}

static void VKAPI_CALL bvb_bridge_vkDestroyFence(
    VkDevice device, VkFence fence, const VkAllocationCallbacks *allocator) {
    destroy_resource(
        device, non_dispatchable_wire_id(&fence, sizeof(fence)),
        BVB_OBJECT_FENCE, BVB_OPCODE_VULKAN_FENCE_DESTROY, allocator);
}

static VkResult VKAPI_CALL bvb_bridge_vkCreateSemaphore(
    VkDevice device, const VkSemaphoreCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkSemaphore *semaphore) {
    if (semaphore != NULL) *semaphore = VK_NULL_HANDLE;
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || create_info == NULL || semaphore == NULL ||
        allocator != NULL ||
        create_info->sType != VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO ||
        create_info->flags != 0U)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkSemaphoreType semaphore_type = VK_SEMAPHORE_TYPE_BINARY;
    uint64_t initial_value = 0U;
    if (create_info->pNext != NULL) {
        const VkSemaphoreTypeCreateInfo *type_info = create_info->pNext;
        if (type_info->sType !=
                VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO ||
            type_info->pNext != NULL ||
            (type_info->semaphoreType != VK_SEMAPHORE_TYPE_BINARY &&
             type_info->semaphoreType != VK_SEMAPHORE_TYPE_TIMELINE) ||
            (type_info->semaphoreType == VK_SEMAPHORE_TYPE_BINARY &&
             type_info->initialValue != 0U))
            return VK_ERROR_FEATURE_NOT_PRESENT;
        semaphore_type = type_info->semaphoreType;
        initial_value = type_info->initialValue;
    }
    struct bvb_resource_proxy *state = calloc(1, sizeof(*state));
    if (state == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
    state->subtype = (uint32_t)semaphore_type;
    const struct bvb_vulkan_semaphore_create_request decoded = {
        .device_id = device_state->wire_id,
        .initial_value = initial_value,
        .semaphore_type = semaphore_type,
        .flags = create_info->flags,
    };
    uint8_t payload[BVB_VULKAN_SEMAPHORE_CREATE_REQUEST_SIZE];
    int result = bvb_protocol_encode_vulkan_semaphore_create_request(
        payload, &decoded);
    if (result != 0 || pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        free(state);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    uint64_t wire_id = 0U;
    VkResult vulkan_result = create_resource_locked(
        BVB_OPCODE_VULKAN_SEMAPHORE_CREATE, payload, sizeof(payload),
        BVB_OBJECT_SEMAPHORE, device_state->wire_id, state, &wire_id);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (vulkan_result != VK_SUCCESS) {
        free(state);
        return vulkan_result;
    }
    memcpy(semaphore, &wire_id, sizeof(*semaphore));
    return VK_SUCCESS;
}

static void VKAPI_CALL bvb_bridge_vkDestroySemaphore(
    VkDevice device, VkSemaphore semaphore,
    const VkAllocationCallbacks *allocator) {
    destroy_resource(
        device, non_dispatchable_wire_id(&semaphore, sizeof(semaphore)),
        BVB_OBJECT_SEMAPHORE, BVB_OPCODE_VULKAN_SEMAPHORE_DESTROY, allocator);
}

static VkResult VKAPI_CALL bvb_bridge_vkGetSemaphoreCounterValue(
    VkDevice device, VkSemaphore semaphore, uint64_t *value) {
    if (value != NULL) *value = 0U;
    struct bvb_device_proxy *device_state = device_proxy(device);
    const uint64_t wire_id = non_dispatchable_wire_id(
        &semaphore, sizeof(semaphore));
    if (device_state == NULL || value == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct bvb_resource_proxy *state =
        resource_proxy_locked(wire_id, BVB_OBJECT_SEMAPHORE);
    int result = state != NULL && state->parent_id == device_state->wire_id
                     ? connect_locked() : -EINVAL;
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_SEMAPHORE_COUNTER,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_OBJECT_ID_SIZE,
    };
    if (result == 0)
        result = bvb_protocol_encode_vulkan_object_id(
            request.payload, wire_id, BVB_OBJECT_SEMAPHORE);
    struct bvb_protocol_packet response = {0};
    if (result == 0) result = exchange_locked(&request, &response);
    if (result == 0 &&
        (response.header.status != 0 || response.header.payload_length !=
            BVB_VULKAN_SEMAPHORE_COUNTER_RESPONSE_SIZE)) result = -EPROTO;
    struct bvb_vulkan_semaphore_counter_response decoded = {
        .vulkan_result = VK_ERROR_INITIALIZATION_FAILED,
    };
    if (result == 0)
        result = bvb_protocol_decode_vulkan_semaphore_counter_response(
            response.payload, &decoded);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result == 0 && decoded.vulkan_result == VK_SUCCESS)
        *value = decoded.value;
    return result == 0 ? (VkResult)decoded.vulkan_result
                       : VK_ERROR_INITIALIZATION_FAILED;
}

static VkResult VKAPI_CALL bvb_bridge_vkSignalSemaphore(
    VkDevice device, const VkSemaphoreSignalInfo *signal_info) {
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || signal_info == NULL ||
        signal_info->sType != VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO ||
        signal_info->pNext != NULL)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const uint64_t wire_id = non_dispatchable_wire_id(
        &signal_info->semaphore, sizeof(signal_info->semaphore));
    if (pthread_mutex_lock(&bvb_global_client.mutex) != 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct bvb_resource_proxy *state =
        resource_proxy_locked(wire_id, BVB_OBJECT_SEMAPHORE);
    int result = state != NULL && state->parent_id == device_state->wire_id
                     ? 0 : -EINVAL;
    const struct bvb_vulkan_semaphore_signal_request decoded = {
        .device_id = device_state->wire_id,
        .semaphore_id = wire_id,
        .value = signal_info->value,
    };
    uint8_t payload[BVB_VULKAN_SEMAPHORE_SIGNAL_REQUEST_SIZE];
    if (result == 0)
        result = bvb_protocol_encode_vulkan_semaphore_signal_request(
            payload, &decoded);
    VkResult vulkan_result = result == 0
        ? result_request_locked(BVB_OPCODE_VULKAN_SEMAPHORE_SIGNAL,
                                payload, sizeof(payload))
        : VK_ERROR_INITIALIZATION_FAILED;
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return vulkan_result;
}

static VkResult VKAPI_CALL bvb_bridge_vkWaitSemaphores(
    VkDevice device, const VkSemaphoreWaitInfo *wait_info, uint64_t timeout) {
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || wait_info == NULL ||
        wait_info->sType != VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO ||
        wait_info->pNext != NULL || wait_info->semaphoreCount == 0U ||
        wait_info->semaphoreCount > BVB_VULKAN_MAX_SEMAPHORES_PER_WAIT ||
        wait_info->pSemaphores == NULL || wait_info->pValues == NULL ||
        (wait_info->flags & ~VK_SEMAPHORE_WAIT_ANY_BIT) != 0U)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (pthread_mutex_lock(&bvb_global_client.mutex) != 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct bvb_vulkan_semaphore_wait_request decoded = {
        .device_id = device_state->wire_id,
        .timeout = timeout,
        .flags = wait_info->flags,
        .semaphore_count = wait_info->semaphoreCount,
    };
    int result = 0;
    for (uint32_t index = 0U; index < wait_info->semaphoreCount; ++index) {
        const uint64_t wire_id = non_dispatchable_wire_id(
            &wait_info->pSemaphores[index], sizeof(VkSemaphore));
        struct bvb_resource_proxy *state =
            resource_proxy_locked(wire_id, BVB_OBJECT_SEMAPHORE);
        if (state == NULL || state->parent_id != device_state->wire_id) {
            result = -EINVAL;
            break;
        }
        decoded.semaphores[index] =
            (struct bvb_vulkan_semaphore_wait_record){
                .semaphore_id = wire_id,
                .value = wait_info->pValues[index],
            };
    }
    uint8_t payload[BVB_VULKAN_SEMAPHORE_WAIT_MAX_SIZE];
    uint32_t payload_length = 0U;
    if (result == 0)
        result = bvb_protocol_encode_vulkan_semaphore_wait_request(
            payload, &decoded, &payload_length);
    VkResult vulkan_result = result == 0
        ? result_request_locked(BVB_OPCODE_VULKAN_SEMAPHORE_WAIT,
                                payload, payload_length)
        : VK_ERROR_INITIALIZATION_FAILED;
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return vulkan_result;
}

static VkResult fence_result_operation(
    VkDevice device, VkFence fence, uint16_t opcode) {
    struct bvb_device_proxy *device_state = device_proxy(device);
    const uint64_t wire_id = non_dispatchable_wire_id(&fence, sizeof(fence));
    if (device_state == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct bvb_resource_proxy *state =
        resource_proxy_locked(wire_id, BVB_OBJECT_FENCE);
    int result = state != NULL && state->parent_id == device_state->wire_id
                     ? connect_locked() : -EINVAL;
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = opcode,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_OBJECT_ID_SIZE,
    };
    if (result == 0)
        result = bvb_protocol_encode_vulkan_object_id(
            request.payload, wire_id, BVB_OBJECT_FENCE);
    struct bvb_protocol_packet response = {0};
    if (result == 0) result = exchange_locked(&request, &response);
    if (result == 0 &&
        (response.header.status != 0 ||
         response.header.payload_length != BVB_VULKAN_RESULT_SIZE))
        result = -EPROTO;
    int32_t vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    if (result == 0)
        result = bvb_protocol_decode_vulkan_result(
            response.payload, &vulkan_result);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return result == 0 ? (VkResult)vulkan_result
                       : VK_ERROR_INITIALIZATION_FAILED;
}

static VkResult VKAPI_CALL bvb_bridge_vkGetFenceStatus(
    VkDevice device, VkFence fence) {
    return fence_result_operation(device, fence,
                                  BVB_OPCODE_VULKAN_FENCE_STATUS);
}

static VkResult VKAPI_CALL bvb_bridge_vkResetFences(
    VkDevice device, uint32_t fence_count, const VkFence *fences) {
    if (fence_count != 1U || fences == NULL)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    return fence_result_operation(device, fences[0],
                                  BVB_OPCODE_VULKAN_FENCE_RESET);
}

static VkResult VKAPI_CALL bvb_bridge_vkWaitForFences(
    VkDevice device, uint32_t fence_count, const VkFence *fences,
    VkBool32 wait_all, uint64_t timeout) {
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || fence_count != 1U || fences == NULL ||
        (wait_all != VK_FALSE && wait_all != VK_TRUE))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const uint64_t wire_id = non_dispatchable_wire_id(
        &fences[0], sizeof(fences[0]));
    if (pthread_mutex_lock(&bvb_global_client.mutex) != 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct bvb_resource_proxy *state =
        resource_proxy_locked(wire_id, BVB_OBJECT_FENCE);
    int result = state != NULL && state->parent_id == device_state->wire_id
                     ? connect_locked() : -EINVAL;
    const struct bvb_vulkan_fence_wait_request decoded = {
        .fence_id = wire_id,
        .timeout = timeout,
        .wait_all = wait_all != VK_FALSE ? 1U : 0U,
    };
    uint8_t payload[BVB_VULKAN_FENCE_WAIT_REQUEST_SIZE];
    if (result == 0)
        result = bvb_protocol_encode_vulkan_fence_wait_request(
            payload, &decoded);
    VkResult vulkan_result = result == 0
                                 ? result_request_locked(
                                       BVB_OPCODE_VULKAN_FENCE_WAIT,
                                       payload, sizeof(payload))
                                 : VK_ERROR_INITIALIZATION_FAILED;
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return vulkan_result;
}

static VkResult queue_result_operation(
    uint16_t opcode, VkQueue queue) {
    struct bvb_queue_proxy *proxy = queue_proxy(queue);
    if (proxy == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    int result = connect_locked();
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = opcode,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_QUEUE_ID_SIZE,
    };
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_queue_id(
            request.payload, proxy->wire_id);
    }
    struct bvb_protocol_packet response = {0};
    if (result == 0) {
        result = exchange_locked(&request, &response);
    }
    if (result == 0 &&
        (response.header.status != 0 ||
         response.header.payload_length != BVB_VULKAN_RESULT_SIZE)) {
        result = -EPROTO;
    }
    int32_t vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    if (result == 0) {
        result = bvb_protocol_decode_vulkan_result(
            response.payload, &vulkan_result);
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return result == 0 ? (VkResult)vulkan_result
                       : VK_ERROR_INITIALIZATION_FAILED;
}

static VkResult VKAPI_CALL bvb_bridge_vkQueueSubmit(
    VkQueue queue, uint32_t submit_count, const VkSubmitInfo *submits,
    VkFence fence) {
    struct bvb_queue_proxy *queue_state = queue_proxy(queue);
    if (queue_state == NULL) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (submit_count == 0U) {
        if (fence != VK_NULL_HANDLE) return VK_ERROR_FEATURE_NOT_PRESENT;
        return queue_result_operation(
            BVB_OPCODE_VULKAN_QUEUE_SUBMIT_EMPTY, queue);
    }
    if (submit_count != 1U || submits == NULL ||
        submits[0].sType != VK_STRUCTURE_TYPE_SUBMIT_INFO ||
        submits[0].pNext != NULL || submits[0].waitSemaphoreCount != 0U ||
        submits[0].commandBufferCount != 1U ||
        submits[0].pCommandBuffers == NULL ||
        submits[0].signalSemaphoreCount != 0U) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    struct bvb_command_buffer_proxy *command_state =
        command_buffer_proxy(submits[0].pCommandBuffers[0]);
    if (command_state == NULL ||
        command_state->device_id != queue_state->parent_id ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const bool shared_stream = command_stream_is_enabled();
    if (shared_stream &&
        pthread_mutex_lock(&command_state->stream_mutex) != 0) {
        (void)pthread_mutex_unlock(&bvb_global_client.mutex);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (shared_stream &&
        command_state->stream_sealed && !command_state->stream_uploaded) {
        (void)pthread_mutex_unlock(&command_state->stream_mutex);
        (void)pthread_mutex_unlock(&bvb_global_client.mutex);
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    const uint64_t fence_id = non_dispatchable_wire_id(&fence, sizeof(fence));
    struct bvb_resource_proxy *fence_state = fence == VK_NULL_HANDLE
        ? NULL : resource_proxy_locked(fence_id, BVB_OBJECT_FENCE);
    if (fence != VK_NULL_HANDLE &&
        (fence_state == NULL ||
         fence_state->parent_id != queue_state->parent_id)) {
        if (shared_stream)
            (void)pthread_mutex_unlock(&command_state->stream_mutex);
        (void)pthread_mutex_unlock(&bvb_global_client.mutex);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkResult upload_result =
        flush_mapped_resources_locked(queue_state->parent_id);
    if (upload_result != VK_SUCCESS) {
        if (shared_stream)
            (void)pthread_mutex_unlock(&command_state->stream_mutex);
        (void)pthread_mutex_unlock(&bvb_global_client.mutex);
        return upload_result;
    }
    if (fence != VK_NULL_HANDLE) {
        const struct bvb_vulkan_queue_submit_command_fence_request
            submit_request = {
                .queue_id = queue_state->wire_id,
                .command_buffer_id = command_state->wire_id,
                .fence_id = fence_id,
            };
        uint8_t payload[
            BVB_VULKAN_QUEUE_SUBMIT_COMMAND_FENCE_REQUEST_SIZE];
        int result =
            bvb_protocol_encode_vulkan_queue_submit_command_fence_request(
                payload, &submit_request);
        VkResult vulkan_result = result == 0
            ? result_request_locked(
                  BVB_OPCODE_VULKAN_QUEUE_SUBMIT_COMMAND_FENCE,
                  payload, sizeof(payload))
            : VK_ERROR_INITIALIZATION_FAILED;
        if (shared_stream)
            (void)pthread_mutex_unlock(&command_state->stream_mutex);
        (void)pthread_mutex_unlock(&bvb_global_client.mutex);
        return vulkan_result;
    }
    const struct bvb_vulkan_queue_submit_command_request submit_request = {
        .queue_id = queue_state->wire_id,
        .command_buffer_id = command_state->wire_id,
    };
    uint8_t payload[BVB_VULKAN_QUEUE_SUBMIT_COMMAND_REQUEST_SIZE];
    int result = bvb_protocol_encode_vulkan_queue_submit_command_request(
        payload, &submit_request);
    VkResult vulkan_result = result == 0
                                 ? result_request_locked(
                                       BVB_OPCODE_VULKAN_QUEUE_SUBMIT_COMMAND,
                                       payload, sizeof(payload))
                                 : VK_ERROR_INITIALIZATION_FAILED;
    if (shared_stream)
        (void)pthread_mutex_unlock(&command_state->stream_mutex);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return vulkan_result;
}

static VkResult VKAPI_CALL bvb_bridge_vkQueueSubmit2(
    VkQueue queue, uint32_t submit_count, const VkSubmitInfo2 *submits,
    VkFence fence) {
    struct bvb_queue_proxy *queue_state = queue_proxy(queue);
    if (queue_state == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    if (submit_count != 1U || submits == NULL ||
        submits[0].sType != VK_STRUCTURE_TYPE_SUBMIT_INFO_2 ||
        submits[0].pNext != NULL || submits[0].flags != 0U ||
        submits[0].waitSemaphoreInfoCount >
            BVB_VULKAN_MAX_SEMAPHORES_PER_WAIT ||
        submits[0].commandBufferInfoCount >
            BVB_VULKAN_MAX_COMMAND_BUFFERS_PER_SUBMIT ||
        submits[0].signalSemaphoreInfoCount >
            BVB_VULKAN_MAX_SEMAPHORES_PER_WAIT ||
        (submits[0].waitSemaphoreInfoCount != 0U &&
         submits[0].pWaitSemaphoreInfos == NULL) ||
        (submits[0].commandBufferInfoCount != 0U &&
         submits[0].pCommandBufferInfos == NULL) ||
        (submits[0].signalSemaphoreInfoCount != 0U &&
         submits[0].pSignalSemaphoreInfos == NULL))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (pthread_mutex_lock(&bvb_global_client.mutex) != 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    const uint64_t fence_id = non_dispatchable_wire_id(&fence, sizeof(fence));
    struct bvb_resource_proxy *fence_state = fence == VK_NULL_HANDLE
        ? NULL : resource_proxy_locked(fence_id, BVB_OBJECT_FENCE);
    int result = fence == VK_NULL_HANDLE ||
                         (fence_state != NULL &&
                          fence_state->parent_id == queue_state->parent_id)
                     ? 0 : -EINVAL;
    struct bvb_vulkan_queue_submit_2_request decoded = {
        .queue_id = queue_state->wire_id,
        .fence_id = fence_id,
        .flags = submits[0].flags,
        .wait_count = submits[0].waitSemaphoreInfoCount,
        .command_count = submits[0].commandBufferInfoCount,
        .signal_count = submits[0].signalSemaphoreInfoCount,
    };
    const bool shared_stream = command_stream_is_enabled();
    struct bvb_command_buffer_proxy *locked_commands[
        BVB_VULKAN_MAX_COMMAND_BUFFERS_PER_SUBMIT] = {0};
    uint32_t locked_command_count = 0U;
    bool stream_submit = false;
    for (uint32_t index = 0U; result == 0 && index < decoded.wait_count;
         ++index) {
        const VkSemaphoreSubmitInfo *info =
            &submits[0].pWaitSemaphoreInfos[index];
        const uint64_t wire_id = non_dispatchable_wire_id(
            &info->semaphore, sizeof(info->semaphore));
        struct bvb_resource_proxy *state =
            resource_proxy_locked(wire_id, BVB_OBJECT_SEMAPHORE);
        if (info->sType != VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO ||
            info->pNext != NULL || info->deviceIndex != 0U || state == NULL ||
            state->parent_id != queue_state->parent_id) {
            result = -EINVAL;
            break;
        }
        decoded.waits[index] =
            (struct bvb_vulkan_submit_2_semaphore_record){
                .semaphore_id = wire_id,
                .value = info->value,
                .stage_mask = info->stageMask,
                .device_index = info->deviceIndex,
            };
    }
    for (uint32_t index = 0U; result == 0 && index < decoded.command_count;
         ++index) {
        const VkCommandBufferSubmitInfo *info =
            &submits[0].pCommandBufferInfos[index];
        struct bvb_command_buffer_proxy *state =
            command_buffer_proxy(info->commandBuffer);
        if (info->sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO ||
            info->pNext != NULL || info->deviceMask != 0U || state == NULL ||
            state->device_id != queue_state->parent_id) {
            result = -EINVAL;
            break;
        }
        if (shared_stream) {
            bool already_locked = false;
            for (uint32_t locked = 0U; locked < locked_command_count;
                 ++locked) {
                already_locked |= locked_commands[locked] == state;
            }
            if (!already_locked) {
                if (pthread_mutex_lock(&state->stream_mutex) != 0) {
                    result = -EDEADLK;
                    break;
                }
                locked_commands[locked_command_count++] = state;
            }
        }
        decoded.commands[index] = (struct bvb_vulkan_submit_2_command_record){
            .command_buffer_id = state->wire_id,
            .device_mask = info->deviceMask,
        };
        if (shared_stream && !state->stream_uploaded) {
            if (state->stream_recording || state->stream_error ||
                !state->stream_sealed || state->stream_length == 0U ||
                state->stream_slot >= BVB_COMMAND_STREAM_SLOT_COUNT) {
                result = -EINVAL;
                break;
            }
            decoded.commands[index].stream_generation =
                bvb_global_client.command_stream_generation;
            decoded.commands[index].stream_sequence = state->stream_sequence;
            decoded.commands[index].stream_offset =
                state->stream_slot * BVB_COMMAND_STREAM_SLOT_BYTES;
            decoded.commands[index].stream_length = state->stream_length;
            decoded.commands[index].stream_flags =
                BVB_VULKAN_SUBMIT_2_COMMAND_SHARED_STREAM;
            stream_submit = true;
        }
    }
    for (uint32_t index = 0U; result == 0 && index < decoded.signal_count;
         ++index) {
        const VkSemaphoreSubmitInfo *info =
            &submits[0].pSignalSemaphoreInfos[index];
        const uint64_t wire_id = non_dispatchable_wire_id(
            &info->semaphore, sizeof(info->semaphore));
        struct bvb_resource_proxy *state =
            resource_proxy_locked(wire_id, BVB_OBJECT_SEMAPHORE);
        if (info->sType != VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO ||
            info->pNext != NULL || info->deviceIndex != 0U || state == NULL ||
            state->parent_id != queue_state->parent_id) {
            result = -EINVAL;
            break;
        }
        decoded.signals[index] =
            (struct bvb_vulkan_submit_2_semaphore_record){
                .semaphore_id = wire_id,
                .value = info->value,
                .stage_mask = info->stageMask,
                .device_index = info->deviceIndex,
            };
    }
    VkResult upload_result = result == 0
        ? flush_mapped_resources_locked(queue_state->parent_id)
        : VK_ERROR_INITIALIZATION_FAILED;
    uint8_t payload[BVB_VULKAN_SUBMIT_2_STREAM_MAX_SIZE];
    uint32_t payload_length = 0U;
    if (result == 0 && upload_result == VK_SUCCESS) {
        result = stream_submit
            ? bvb_protocol_encode_vulkan_queue_submit_2_stream_request(
                  payload, &decoded, &payload_length)
            : bvb_protocol_encode_vulkan_queue_submit_2_request(
                  payload, &decoded, &payload_length);
    }
    VkResult vulkan_result = result == 0
                                 ? upload_result
                                 : VK_ERROR_INITIALIZATION_FAILED;
    bool submit_acknowledged = false;
    if (result == 0 && upload_result == VK_SUCCESS) {
        submit_acknowledged = result_exchange_locked(
                                  stream_submit
                                      ? BVB_OPCODE_VULKAN_QUEUE_SUBMIT_2_STREAM
                                      : BVB_OPCODE_VULKAN_QUEUE_SUBMIT_2,
                                  payload, payload_length,
                                  &vulkan_result) == 0;
        if (!submit_acknowledged)
            vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    }
    if (submit_acknowledged) {
        for (uint32_t index = 0U; index < decoded.command_count; ++index) {
            if (decoded.commands[index].stream_flags ==
                BVB_VULKAN_SUBMIT_2_COMMAND_SHARED_STREAM) {
                struct bvb_command_buffer_proxy *state =
                    command_buffer_proxy(
                        submits[0].pCommandBufferInfos[index].commandBuffer);
                if (state != NULL &&
                    state->stream_sequence ==
                        decoded.commands[index].stream_sequence) {
                    state->stream_uploaded = true;
                    release_command_stream_slot(state);
                }
            }
        }
    }
    while (locked_command_count != 0U) {
        --locked_command_count;
        (void)pthread_mutex_unlock(
            &locked_commands[locked_command_count]->stream_mutex);
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return vulkan_result;
}

static VkResult VKAPI_CALL bvb_bridge_vkQueueWaitIdle(VkQueue queue) {
    return queue_result_operation(BVB_OPCODE_VULKAN_QUEUE_WAIT_IDLE, queue);
}

static VkResult VKAPI_CALL bvb_bridge_vkDeviceWaitIdle(VkDevice device) {
    struct bvb_device_proxy *proxy = device_proxy(device);
    if (proxy == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    int result = connect_locked();
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_DEVICE_WAIT_IDLE,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_DEVICE_ID_SIZE,
    };
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_device_id(
            request.payload, proxy->wire_id);
    }
    struct bvb_protocol_packet response = {0};
    if (result == 0) {
        result = exchange_locked(&request, &response);
    }
    if (result == 0 &&
        (response.header.status != 0 ||
         response.header.payload_length != BVB_VULKAN_RESULT_SIZE)) {
        result = -EPROTO;
    }
    int32_t vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    if (result == 0) {
        result = bvb_protocol_decode_vulkan_result(
            response.payload, &vulkan_result);
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return result == 0 ? (VkResult)vulkan_result
                       : VK_ERROR_INITIALIZATION_FAILED;
}

static VkResult VKAPI_CALL bvb_bridge_vkCreateSwapchainKHR(
    VkDevice device, const VkSwapchainCreateInfoKHR *create_info,
    const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain) {
    if (swapchain != NULL) *swapchain = VK_NULL_HANDLE;
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || !device_state->virtual_swapchain_enabled ||
        create_info == NULL || swapchain == NULL || allocator != NULL ||
        create_info->sType != VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR ||
        create_info->pNext != NULL || create_info->flags != 0U ||
        create_info->minImageCount < 2U ||
        create_info->minImageCount > 3U ||
        (create_info->imageFormat != VK_FORMAT_R8G8B8A8_UNORM &&
         create_info->imageFormat != VK_FORMAT_B8G8R8A8_UNORM) ||
        create_info->imageColorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR ||
        create_info->imageExtent.width == 0U ||
        create_info->imageExtent.height == 0U ||
        create_info->imageArrayLayers != 1U ||
        create_info->imageUsage == 0U ||
        create_info->imageSharingMode != VK_SHARING_MODE_EXCLUSIVE ||
        create_info->queueFamilyIndexCount != 0U ||
        create_info->preTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ||
        create_info->compositeAlpha != VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR ||
        create_info->presentMode != VK_PRESENT_MODE_FIFO_KHR ||
        create_info->oldSwapchain != VK_NULL_HANDLE) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    struct bvb_swapchain_proxy *proxy = calloc(1, sizeof(*proxy));
    if (proxy == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
    proxy->control_fd = -1;
    proxy->control = MAP_FAILED;
    if (pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        free(proxy);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    struct bvb_surface_proxy *surface_state =
        surface_proxy_locked(create_info->surface);
    struct bvb_activity_status activity = {0};
    int result = surface_state == NULL ||
                         surface_state->parent_id != device_state->instance_id
                     ? -EINVAL
                     : ready_activity_status_locked(&activity);
    if (result == 0 &&
        (create_info->imageExtent.width != activity.width ||
         create_info->imageExtent.height != activity.height)) {
        result = -ERANGE;
    }
    uint64_t generation = 0U;
    if (result == 0) {
        if (bvb_global_client.next_swapchain_generation == 0U ||
            bvb_global_client.next_swapchain_generation >
                UINT64_C(0x0000ffffffffffff)) {
            result = -EOVERFLOW;
        } else {
            generation = UINT64_C(0xe060000000000000) |
                bvb_global_client.next_swapchain_generation++;
        }
    }
    const struct bvb_vulkan_swapchain_prepare_request decoded_request = {
        .device_id = device_state->wire_id,
        .width = create_info->imageExtent.width,
        .height = create_info->imageExtent.height,
        .format = create_info->imageFormat,
        .image_usage = create_info->imageUsage,
        .min_image_count = 3U,
        .generation = generation,
    };
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_SWAPCHAIN_PREPARE,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_SWAPCHAIN_PREPARE_REQUEST_SIZE,
    };
    if (result == 0)
        result = bvb_protocol_encode_vulkan_swapchain_prepare_request(
            request.payload, &decoded_request);
    int descriptors[BVB_WSI_FRAME_RING_MAX_SLOTS + 1U];
    for (size_t index = 0U;
         index < BVB_WSI_FRAME_RING_MAX_SLOTS + 1U; ++index)
        descriptors[index] = -1;
    size_t descriptor_count = 0U;
    struct bvb_protocol_packet response = {0};
    if (result == 0)
        result = exchange_fds_locked(
            &request, &response, descriptors,
            BVB_WSI_FRAME_RING_MAX_SLOTS + 1U, &descriptor_count);
    if (result == 0 && response.header.status != 0)
        result = response.header.status;
    if (result == 0 && response.header.payload_length !=
                           BVB_VULKAN_SWAPCHAIN_PREPARE_RESPONSE_SIZE)
        result = -EPROTO;
    struct bvb_vulkan_swapchain_prepare_response prepared = {0};
    if (result == 0)
        result = bvb_protocol_decode_vulkan_swapchain_prepare_response(
            response.payload, &prepared);
    if (result == 0 && prepared.vulkan_result == VK_SUCCESS &&
        descriptor_count !=
            ((prepared.flags &
              BVB_VULKAN_SWAPCHAIN_PREPARE_FLAG_AHARDWAREBUFFER) != 0U
                 ? 1U
                 : (size_t)prepared.image_count + 1U))
        result = -EPROTO;
    if (result == 0 && prepared.vulkan_result != VK_SUCCESS &&
        descriptor_count != 0U)
        result = -EPROTO;
    if (result == 0 && prepared.vulkan_result == VK_SUCCESS &&
        (prepared.flags &
         BVB_VULKAN_SWAPCHAIN_PREPARE_FLAG_AHARDWAREBUFFER) == 0U) {
        for (uint32_t index = 0U; index < prepared.image_count; ++index) {
            struct stat descriptor_status;
            if (fstat(descriptors[index], &descriptor_status) != 0 ||
                descriptor_status.st_size <= 0 ||
                (uint64_t)descriptor_status.st_size !=
                    prepared.images[index].allocation_size) {
                result = -EPROTO;
                break;
            }
            (void)close(descriptors[index]);
            descriptors[index] = -1;
        }
    }
    if (result == 0 && prepared.vulkan_result == VK_SUCCESS) {
        const uint32_t control_index =
            (prepared.flags &
             BVB_VULKAN_SWAPCHAIN_PREPARE_FLAG_AHARDWAREBUFFER) != 0U
                ? 0U
                : prepared.image_count;
        struct stat control_status;
        if (fstat(descriptors[control_index], &control_status) != 0 ||
            control_status.st_size != BVB_WSI_FRAME_RING_REGION_BYTES) {
            result = -EPROTO;
        } else {
            proxy->control = mmap(
                NULL, BVB_WSI_FRAME_RING_REGION_BYTES,
                PROT_READ | PROT_WRITE, MAP_SHARED,
                descriptors[control_index], 0U);
            if (proxy->control == MAP_FAILED) {
                result = -errno;
            } else {
                proxy->control_fd = descriptors[control_index];
                descriptors[control_index] = -1;
                result = bvb_wsi_frame_ring_validate(
                    proxy->control, prepared.generation);
            }
        }
    }
    if (result == 0 && prepared.vulkan_result == VK_SUCCESS) {
        proxy->wire_id = prepared.swapchain_id;
        proxy->parent_id = device_state->wire_id;
        proxy->generation = prepared.generation;
        proxy->image_count = prepared.image_count;
        for (uint32_t index = 0U; index < prepared.image_count; ++index)
            proxy->image_ids[index] = prepared.images[index].image_id;
        if (pthread_rwlock_wrlock(&bvb_object_registry_lock) != 0) {
            result = -EDEADLK;
        } else {
            proxy->next = bvb_global_client.swapchains;
            bvb_global_client.swapchains = proxy;
            (void)pthread_rwlock_unlock(&bvb_object_registry_lock);
            *swapchain = swapchain_from_wire_id(proxy->wire_id);
        }
    }
    if (result != 0 && prepared.vulkan_result == VK_SUCCESS &&
        bvb_handle_expect(prepared.swapchain_id,
                          BVB_OBJECT_SWAPCHAIN) == 0) {
        struct bvb_protocol_packet destroy_request = {0};
        destroy_request.header = (struct bvb_protocol_header){
            .version = BVB_PROTOCOL_VERSION,
            .kind = BVB_PROTOCOL_REQUEST,
            .opcode = BVB_OPCODE_VULKAN_SWAPCHAIN_DESTROY,
            .request_id = next_request_id_locked(),
            .payload_length = BVB_VULKAN_OBJECT_ID_SIZE,
        };
        if (bvb_protocol_encode_vulkan_object_id(
                destroy_request.payload, prepared.swapchain_id,
                BVB_OBJECT_SWAPCHAIN) == 0) {
            struct bvb_protocol_packet destroy_response = {0};
            (void)exchange_locked(&destroy_request, &destroy_response);
        }
    }
    if (getenv("BVB_ICD_DIAGNOSTICS") != NULL) {
        fprintf(stderr,
                "BVB_ICD_VIRTUAL_SWAPCHAIN_CREATE surface=%llu "
                "extent=%ux%u activity=%ux%u backing=activity "
                "frame_transport=e060 status=%d vulkan=%d images=%u\n",
                (unsigned long long)non_dispatchable_wire_id(
                    &create_info->surface, sizeof(create_info->surface)),
                create_info->imageExtent.width,
                create_info->imageExtent.height, activity.width,
                activity.height, result, prepared.vulkan_result,
                prepared.image_count);
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    for (size_t index = 0U;
         index < BVB_WSI_FRAME_RING_MAX_SLOTS + 1U; ++index)
        if (descriptors[index] >= 0) (void)close(descriptors[index]);
    if (result != 0) {
        release_swapchain_proxy(proxy);
        return result == -ERANGE || result == -ENODEV || result == -EAGAIN ||
                       result == -ENOTCONN
                   ? VK_ERROR_SURFACE_LOST_KHR
                   : VK_ERROR_INITIALIZATION_FAILED;
    }
    if (prepared.vulkan_result != VK_SUCCESS) {
        release_swapchain_proxy(proxy);
        return (VkResult)prepared.vulkan_result;
    }
    return VK_SUCCESS;
}

static void VKAPI_CALL bvb_bridge_vkDestroySwapchainKHR(
    VkDevice device, VkSwapchainKHR swapchain,
    const VkAllocationCallbacks *allocator) {
    if (swapchain == VK_NULL_HANDLE) return;
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || allocator != NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) return;
    struct bvb_swapchain_proxy *proxy = swapchain_proxy_locked(swapchain);
    int result = proxy != NULL && proxy->parent_id == device_state->wire_id
        ? connect_locked() : -EINVAL;
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_SWAPCHAIN_DESTROY,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_OBJECT_ID_SIZE,
    };
    if (result == 0)
        result = bvb_protocol_encode_vulkan_object_id(
            request.payload, proxy->wire_id, BVB_OBJECT_SWAPCHAIN);
    struct bvb_protocol_packet response = {0};
    if (result == 0) result = exchange_locked(&request, &response);
    if (result == 0 &&
        (response.header.status != 0 || response.header.payload_length != 0U))
        result = -EPROTO;
    if (result == 0) {
        if (pthread_rwlock_wrlock(&bvb_object_registry_lock) != 0) {
            result = -EDEADLK;
        } else {
            remove_swapchain_proxy_locked(proxy);
            (void)pthread_rwlock_unlock(&bvb_object_registry_lock);
        }
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
}

static VkResult VKAPI_CALL bvb_bridge_vkGetSwapchainImagesKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint32_t *image_count,
    VkImage *images) {
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || !device_state->virtual_swapchain_enabled ||
        image_count == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct bvb_swapchain_proxy *proxy = swapchain_proxy_locked(swapchain);
    if (proxy == NULL || proxy->parent_id != device_state->wire_id) {
        *image_count = 0U;
        (void)pthread_mutex_unlock(&bvb_global_client.mutex);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (images == NULL) {
        *image_count = proxy->image_count;
        (void)pthread_mutex_unlock(&bvb_global_client.mutex);
        return VK_SUCCESS;
    }
    const uint32_t capacity = *image_count;
    const uint32_t returned = capacity < proxy->image_count
        ? capacity : proxy->image_count;
    for (uint32_t index = 0U; index < returned; ++index)
        images[index] = image_from_wire_id(proxy->image_ids[index]);
    *image_count = returned;
    const VkResult result = returned < proxy->image_count
        ? VK_INCOMPLETE : VK_SUCCESS;
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return result;
}

static VkResult VKAPI_CALL bvb_bridge_vkAcquireNextImageKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, uint32_t *image_index) {
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (image_index != NULL) *image_index = 0U;
    if (device_state == NULL || !device_state->virtual_swapchain_enabled ||
        image_index == NULL ||
        (semaphore == VK_NULL_HANDLE && fence == VK_NULL_HANDLE) ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct bvb_swapchain_proxy *proxy = swapchain_proxy_locked(swapchain);
    const uint64_t semaphore_id = non_dispatchable_wire_id(
        &semaphore, sizeof(semaphore));
    const uint64_t fence_id = non_dispatchable_wire_id(&fence, sizeof(fence));
    struct bvb_resource_proxy *semaphore_state = semaphore == VK_NULL_HANDLE
        ? NULL : resource_proxy_locked(semaphore_id, BVB_OBJECT_SEMAPHORE);
    struct bvb_resource_proxy *fence_state = fence == VK_NULL_HANDLE
        ? NULL : resource_proxy_locked(fence_id, BVB_OBJECT_FENCE);
    int result = proxy != NULL && proxy->parent_id == device_state->wire_id &&
                         bvb_wsi_frame_ring_validate(
                             proxy->control, proxy->generation) == 0 &&
                         (semaphore == VK_NULL_HANDLE ||
                          (semaphore_state != NULL &&
                           semaphore_state->parent_id == device_state->wire_id &&
                           semaphore_state->subtype ==
                               VK_SEMAPHORE_TYPE_BINARY)) &&
                         (fence == VK_NULL_HANDLE ||
                          (fence_state != NULL &&
                           fence_state->parent_id == device_state->wire_id))
                     ? connect_locked() : -EINVAL;
    const struct bvb_vulkan_swapchain_acquire_request decoded = {
        .device_id = device_state->wire_id,
        .swapchain_id = proxy == NULL ? 0U : proxy->wire_id,
        .timeout_ns = timeout,
        .semaphore_id = semaphore_id,
        .fence_id = fence_id,
    };
    uint8_t payload[BVB_VULKAN_SWAPCHAIN_ACQUIRE_REQUEST_SIZE];
    if (result == 0)
        result = bvb_protocol_encode_vulkan_swapchain_acquire_request(
            payload, &decoded);
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_SWAPCHAIN_ACQUIRE,
        .request_id = next_request_id_locked(),
        .payload_length = sizeof(payload),
    };
    if (result == 0) memcpy(request.payload, payload, sizeof(payload));
    struct bvb_protocol_packet response = {0};
    const uint64_t profile_started_ns =
        result == 0 && bvb_global_client.frame_profile_enabled
        ? frame_profile_monotonic_ns() : 0U;
    if (result == 0) result = exchange_locked(&request, &response);
    const uint64_t profile_finished_ns =
        bvb_global_client.frame_profile_enabled
        ? frame_profile_monotonic_ns() : 0U;
    frame_profile_record_client_exchange_locked(
        false, profile_started_ns, profile_finished_ns);
    if (result == 0 &&
        (response.header.status != 0 ||
         response.header.payload_length !=
             BVB_VULKAN_SWAPCHAIN_ACQUIRE_RESPONSE_SIZE))
        result = response.header.status != 0 ? response.header.status : -EPROTO;
    struct bvb_vulkan_swapchain_acquire_response acquired = {0};
    if (result == 0)
        result = bvb_protocol_decode_vulkan_swapchain_acquire_response(
            response.payload, &acquired);
    if (result == 0 && acquired.vulkan_result == VK_SUCCESS &&
        acquired.image_index >= proxy->image_count) result = -EPROTO;
    if (result == 0) *image_index = acquired.image_index;
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return result == 0 ? (VkResult)acquired.vulkan_result
                       : VK_ERROR_INITIALIZATION_FAILED;
}

static VkResult VKAPI_CALL bvb_bridge_vkAcquireNextImage2KHR(
    VkDevice device, const VkAcquireNextImageInfoKHR *acquire_info,
    uint32_t *image_index) {
    if (acquire_info == NULL ||
        acquire_info->sType != VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR ||
        acquire_info->pNext != NULL || acquire_info->deviceMask != 1U) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return bvb_bridge_vkAcquireNextImageKHR(
        device, acquire_info->swapchain, acquire_info->timeout,
        acquire_info->semaphore, acquire_info->fence, image_index);
}

static VkResult VKAPI_CALL bvb_bridge_vkQueuePresentKHR(
    VkQueue queue, const VkPresentInfoKHR *present_info) {
    struct bvb_queue_proxy *queue_state = queue_proxy(queue);
    if (queue_state == NULL || present_info == NULL ||
        present_info->sType != VK_STRUCTURE_TYPE_PRESENT_INFO_KHR ||
        present_info->pNext != NULL || present_info->swapchainCount != 1U ||
        present_info->pSwapchains == NULL ||
        present_info->pImageIndices == NULL ||
        present_info->waitSemaphoreCount >
            BVB_VULKAN_MAX_PRESENT_WAIT_SEMAPHORES ||
        (present_info->waitSemaphoreCount != 0U &&
         present_info->pWaitSemaphores == NULL)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    struct bvb_device_proxy *device_state = NULL;
    for (struct bvb_device_proxy *device = bvb_global_client.devices;
         device != NULL; device = device->next) {
        if (device->wire_id == queue_state->parent_id) {
            device_state = device;
            break;
        }
    }
    struct bvb_swapchain_proxy *proxy = swapchain_proxy_locked(
        present_info->pSwapchains[0]);
    int result = device_state != NULL &&
                         device_state->virtual_swapchain_enabled &&
                         proxy != NULL &&
                         proxy->parent_id == queue_state->parent_id &&
                         present_info->pImageIndices[0] < proxy->image_count &&
                         bvb_wsi_frame_ring_validate(
                             proxy->control, proxy->generation) == 0
                     ? connect_locked() : -EINVAL;
    struct bvb_vulkan_swapchain_present_request decoded = {
        .queue_id = queue_state->wire_id,
        .swapchain_id = proxy == NULL ? 0U : proxy->wire_id,
        .image_index = present_info->pImageIndices[0],
        .wait_semaphore_count = present_info->waitSemaphoreCount,
    };
    for (uint32_t index = 0U; result == 0 &&
         index < present_info->waitSemaphoreCount; ++index) {
        const uint64_t semaphore_id = non_dispatchable_wire_id(
            &present_info->pWaitSemaphores[index], sizeof(VkSemaphore));
        struct bvb_resource_proxy *state = resource_proxy_locked(
            semaphore_id, BVB_OBJECT_SEMAPHORE);
        if (state == NULL || state->parent_id != queue_state->parent_id ||
            state->subtype != VK_SEMAPHORE_TYPE_BINARY) {
            result = -EINVAL;
            break;
        }
        decoded.wait_semaphore_ids[index] = semaphore_id;
    }
    uint8_t payload[BVB_VULKAN_SWAPCHAIN_PRESENT_MAX_SIZE];
    uint32_t payload_length = 0U;
    if (result == 0)
        result = bvb_protocol_encode_vulkan_swapchain_present_request(
            payload, &decoded, &payload_length);
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_SWAPCHAIN_PRESENT,
        .request_id = next_request_id_locked(),
        .payload_length = payload_length,
    };
    if (result == 0) memcpy(request.payload, payload, payload_length);
    struct bvb_protocol_packet response = {0};
    const uint64_t profile_started_ns =
        result == 0 && bvb_global_client.frame_profile_enabled
        ? frame_profile_monotonic_ns() : 0U;
    if (result == 0) result = exchange_locked(&request, &response);
    const uint64_t profile_finished_ns =
        bvb_global_client.frame_profile_enabled
        ? frame_profile_monotonic_ns() : 0U;
    frame_profile_record_client_exchange_locked(
        true, profile_started_ns, profile_finished_ns);
    if (result == 0 &&
        (response.header.status != 0 ||
         response.header.payload_length !=
             BVB_VULKAN_SWAPCHAIN_PRESENT_RESPONSE_SIZE))
        result = response.header.status != 0 ? response.header.status : -EPROTO;
    struct bvb_vulkan_swapchain_present_response presented = {0};
    if (result == 0)
        result = bvb_protocol_decode_vulkan_swapchain_present_response(
            response.payload, &presented);
    const VkResult vulkan_result = result == 0
        ? (VkResult)presented.vulkan_result : VK_ERROR_INITIALIZATION_FAILED;
    if (present_info->pResults != NULL) present_info->pResults[0] = vulkan_result;
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return vulkan_result;
}

static VkResult VKAPI_CALL
bvb_bridge_vkGetDeviceGroupPresentCapabilitiesKHR(
    VkDevice device, VkDeviceGroupPresentCapabilitiesKHR *capabilities) {
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (capabilities == NULL ||
        capabilities->sType !=
            VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_CAPABILITIES_KHR ||
        device_state == NULL || !device_state->virtual_swapchain_enabled) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    memset(capabilities->presentMask, 0, sizeof(capabilities->presentMask));
    capabilities->modes = 0U;
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

static VkResult VKAPI_CALL
bvb_bridge_vkGetDeviceGroupSurfacePresentModesKHR(
    VkDevice device, VkSurfaceKHR surface,
    VkDeviceGroupPresentModeFlagsKHR *modes) {
    (void)surface;
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (modes != NULL) *modes = 0U;
    return modes != NULL && device_state != NULL &&
                   device_state->virtual_swapchain_enabled
               ? VK_ERROR_FEATURE_NOT_PRESENT
               : VK_ERROR_INITIALIZATION_FAILED;
}

PFN_vkVoidFunction bvb_global_device_proc_addr(
    VkDevice device, const char *name) {
    if (device_proxy(device) == NULL || name == NULL) {
        return NULL;
    }
#define BVB_DEVICE_MATCH(entry_name, function)                                \
    if (strcmp(name, (entry_name)) == 0) {                                    \
        PFN_vkVoidFunction raw =                                               \
            BVB_ERASE_FUNCTION((function), __typeof__(&(function)));          \
        return bvb_first_rejection_wrap(                                       \
            (entry_name), BVB_DXVK_SCOPE_DEVICE, raw);                        \
    }
    BVB_DEVICE_MATCH("vkGetDeviceProcAddr", vkGetDeviceProcAddr)
    BVB_DEVICE_MATCH("vkDestroyDevice", bvb_bridge_vkDestroyDevice)
    BVB_DEVICE_MATCH("vkGetDeviceQueue", bvb_bridge_vkGetDeviceQueue)
    BVB_DEVICE_MATCH("vkQueueSubmit", bvb_bridge_vkQueueSubmit)
    BVB_DEVICE_MATCH("vkQueueSubmit2", bvb_bridge_vkQueueSubmit2)
    BVB_DEVICE_MATCH("vkQueueSubmit2KHR", bvb_bridge_vkQueueSubmit2)
    BVB_DEVICE_MATCH("vkQueueWaitIdle", bvb_bridge_vkQueueWaitIdle)
    BVB_DEVICE_MATCH("vkDeviceWaitIdle", bvb_bridge_vkDeviceWaitIdle)
    if (device_proxy(device)->virtual_swapchain_enabled) {
        BVB_DEVICE_MATCH("vkCreateSwapchainKHR",
                         bvb_bridge_vkCreateSwapchainKHR)
        BVB_DEVICE_MATCH("vkDestroySwapchainKHR",
                         bvb_bridge_vkDestroySwapchainKHR)
        BVB_DEVICE_MATCH("vkGetSwapchainImagesKHR",
                         bvb_bridge_vkGetSwapchainImagesKHR)
        BVB_DEVICE_MATCH("vkAcquireNextImageKHR",
                         bvb_bridge_vkAcquireNextImageKHR)
        BVB_DEVICE_MATCH("vkAcquireNextImage2KHR",
                         bvb_bridge_vkAcquireNextImage2KHR)
        BVB_DEVICE_MATCH("vkQueuePresentKHR", bvb_bridge_vkQueuePresentKHR)
        BVB_DEVICE_MATCH("vkGetDeviceGroupPresentCapabilitiesKHR",
                         bvb_bridge_vkGetDeviceGroupPresentCapabilitiesKHR)
        BVB_DEVICE_MATCH("vkGetDeviceGroupSurfacePresentModesKHR",
                         bvb_bridge_vkGetDeviceGroupSurfacePresentModesKHR)
    }
    BVB_DEVICE_MATCH("vkCreateCommandPool", bvb_bridge_vkCreateCommandPool)
    BVB_DEVICE_MATCH("vkDestroyCommandPool", bvb_bridge_vkDestroyCommandPool)
    BVB_DEVICE_MATCH("vkResetCommandPool", bvb_bridge_vkResetCommandPool)
    BVB_DEVICE_MATCH("vkAllocateCommandBuffers",
                     bvb_bridge_vkAllocateCommandBuffers)
    BVB_DEVICE_MATCH("vkFreeCommandBuffers", bvb_bridge_vkFreeCommandBuffers)
    BVB_DEVICE_MATCH("vkBeginCommandBuffer", bvb_bridge_vkBeginCommandBuffer)
    BVB_DEVICE_MATCH("vkEndCommandBuffer", bvb_bridge_vkEndCommandBuffer)
    BVB_DEVICE_MATCH("vkCreateDescriptorSetLayout",
                     bvb_bridge_vkCreateDescriptorSetLayout)
    BVB_DEVICE_MATCH("vkDestroyDescriptorSetLayout",
                     bvb_bridge_vkDestroyDescriptorSetLayout)
    BVB_DEVICE_MATCH("vkCreateDescriptorPool",
                     bvb_bridge_vkCreateDescriptorPool)
    BVB_DEVICE_MATCH("vkDestroyDescriptorPool",
                     bvb_bridge_vkDestroyDescriptorPool)
    BVB_DEVICE_MATCH("vkResetDescriptorPool",
                     bvb_bridge_vkResetDescriptorPool)
    BVB_DEVICE_MATCH("vkCreateQueryPool", bvb_bridge_vkCreateQueryPool)
    BVB_DEVICE_MATCH("vkDestroyQueryPool", bvb_bridge_vkDestroyQueryPool)
    BVB_DEVICE_MATCH("vkGetQueryPoolResults",
                     bvb_bridge_vkGetQueryPoolResults)
    BVB_DEVICE_MATCH("vkResetQueryPool", bvb_bridge_vkResetQueryPool)
    BVB_DEVICE_MATCH("vkAllocateDescriptorSets",
                     bvb_bridge_vkAllocateDescriptorSets)
    BVB_DEVICE_MATCH("vkCreateSampler", bvb_bridge_vkCreateSampler)
    BVB_DEVICE_MATCH("vkDestroySampler", bvb_bridge_vkDestroySampler)
    BVB_DEVICE_MATCH("vkUpdateDescriptorSets",
                     bvb_bridge_vkUpdateDescriptorSets)
    BVB_DEVICE_MATCH("vkCreateDescriptorUpdateTemplate",
                     bvb_bridge_vkCreateDescriptorUpdateTemplate)
    BVB_DEVICE_MATCH("vkDestroyDescriptorUpdateTemplate",
                     bvb_bridge_vkDestroyDescriptorUpdateTemplate)
    BVB_DEVICE_MATCH("vkUpdateDescriptorSetWithTemplate",
                     bvb_bridge_vkUpdateDescriptorSetWithTemplate)
    BVB_DEVICE_MATCH("vkUpdateDescriptorSetWithTemplateKHR",
                     bvb_bridge_vkUpdateDescriptorSetWithTemplate)
    BVB_DEVICE_MATCH("vkCreatePipelineLayout",
                     bvb_bridge_vkCreatePipelineLayout)
    BVB_DEVICE_MATCH("vkDestroyPipelineLayout",
                     bvb_bridge_vkDestroyPipelineLayout)
    BVB_DEVICE_MATCH("vkCreateGraphicsPipelines",
                     bvb_bridge_vkCreateGraphicsPipelines)
    BVB_DEVICE_MATCH("vkDestroyPipeline", bvb_bridge_vkDestroyPipeline)
    BVB_DEVICE_MATCH("vkCreateBuffer", bvb_bridge_vkCreateBuffer)
    BVB_DEVICE_MATCH("vkDestroyBuffer", bvb_bridge_vkDestroyBuffer)
    BVB_DEVICE_MATCH("vkGetBufferMemoryRequirements",
                     bvb_bridge_vkGetBufferMemoryRequirements)
    BVB_DEVICE_MATCH("vkGetDeviceBufferMemoryRequirements",
                     bvb_bridge_vkGetDeviceBufferMemoryRequirements)
    BVB_DEVICE_MATCH("vkGetBufferMemoryRequirements2",
                     bvb_bridge_vkGetBufferMemoryRequirements2)
    BVB_DEVICE_MATCH("vkGetBufferDeviceAddress",
                     bvb_bridge_vkGetBufferDeviceAddress)
    BVB_DEVICE_MATCH("vkAllocateMemory", bvb_bridge_vkAllocateMemory)
    BVB_DEVICE_MATCH("vkFreeMemory", bvb_bridge_vkFreeMemory)
    BVB_DEVICE_MATCH("vkBindBufferMemory", bvb_bridge_vkBindBufferMemory)
    BVB_DEVICE_MATCH("vkCreateImage", bvb_bridge_vkCreateImage)
    BVB_DEVICE_MATCH("vkDestroyImage", bvb_bridge_vkDestroyImage)
    BVB_DEVICE_MATCH("vkGetImageMemoryRequirements",
                     bvb_bridge_vkGetImageMemoryRequirements)
    BVB_DEVICE_MATCH("vkGetImageMemoryRequirements2",
                     bvb_bridge_vkGetImageMemoryRequirements2)
    BVB_DEVICE_MATCH("vkBindImageMemory", bvb_bridge_vkBindImageMemory)
    BVB_DEVICE_MATCH("vkCreateImageView", bvb_bridge_vkCreateImageView)
    BVB_DEVICE_MATCH("vkDestroyImageView", bvb_bridge_vkDestroyImageView)
    BVB_DEVICE_MATCH("vkMapMemory", bvb_bridge_vkMapMemory)
    BVB_DEVICE_MATCH("vkMapMemory2", bvb_bridge_vkMapMemory2)
    BVB_DEVICE_MATCH("vkMapMemory2KHR", bvb_bridge_vkMapMemory2)
    BVB_DEVICE_MATCH("vkUnmapMemory", bvb_bridge_vkUnmapMemory)
    BVB_DEVICE_MATCH("vkFlushMappedMemoryRanges",
                     bvb_bridge_vkFlushMappedMemoryRanges)
    BVB_DEVICE_MATCH("vkInvalidateMappedMemoryRanges",
                     bvb_bridge_vkInvalidateMappedMemoryRanges)
    BVB_DEVICE_MATCH("vkCmdPipelineBarrier2",
                     bvb_bridge_vkCmdPipelineBarrier2)
    BVB_DEVICE_MATCH("vkCmdClearColorImage",
                     bvb_bridge_vkCmdClearColorImage)
    BVB_DEVICE_MATCH("vkCmdFillBuffer", bvb_bridge_vkCmdFillBuffer)
    BVB_DEVICE_MATCH("vkCmdBindDescriptorSets",
                     bvb_bridge_vkCmdBindDescriptorSets)
    BVB_DEVICE_MATCH("vkCmdBeginRendering", bvb_bridge_vkCmdBeginRendering)
    BVB_DEVICE_MATCH("vkCmdBeginRenderingKHR", bvb_bridge_vkCmdBeginRendering)
    BVB_DEVICE_MATCH("vkCmdEndRendering", bvb_bridge_vkCmdEndRendering)
    BVB_DEVICE_MATCH("vkCmdEndRenderingKHR", bvb_bridge_vkCmdEndRendering)
    BVB_DEVICE_MATCH("vkCmdBindPipeline", bvb_bridge_vkCmdBindPipeline)
    BVB_DEVICE_MATCH("vkCmdPushConstants", bvb_bridge_vkCmdPushConstants)
    BVB_DEVICE_MATCH("vkCmdSetViewportWithCount",
                     bvb_bridge_vkCmdSetViewportWithCount)
    BVB_DEVICE_MATCH("vkCmdSetViewportWithCountEXT",
                     bvb_bridge_vkCmdSetViewportWithCount)
    BVB_DEVICE_MATCH("vkCmdSetScissorWithCount",
                     bvb_bridge_vkCmdSetScissorWithCount)
    BVB_DEVICE_MATCH("vkCmdSetScissorWithCountEXT",
                     bvb_bridge_vkCmdSetScissorWithCount)
    BVB_DEVICE_MATCH("vkCmdDraw", bvb_bridge_vkCmdDraw)
    BVB_DEVICE_MATCH("vkCmdBindVertexBuffers",
                     bvb_bridge_vkCmdBindVertexBuffers)
    BVB_DEVICE_MATCH("vkCmdBindVertexBuffers2",
                     bvb_bridge_vkCmdBindVertexBuffers2)
    BVB_DEVICE_MATCH("vkCmdBindVertexBuffers2EXT",
                     bvb_bridge_vkCmdBindVertexBuffers2)
    BVB_DEVICE_MATCH("vkCmdBindIndexBuffer",
                     bvb_bridge_vkCmdBindIndexBuffer)
    BVB_DEVICE_MATCH("vkCmdBindIndexBuffer2",
                     bvb_bridge_vkCmdBindIndexBuffer2)
    BVB_DEVICE_MATCH("vkCmdBindIndexBuffer2KHR",
                     bvb_bridge_vkCmdBindIndexBuffer2)
    BVB_DEVICE_MATCH("vkCmdDrawIndexed", bvb_bridge_vkCmdDrawIndexed)
    BVB_DEVICE_MATCH("vkCmdDrawIndirect", bvb_bridge_vkCmdDrawIndirect)
    BVB_DEVICE_MATCH("vkCmdDrawIndexedIndirect",
                     bvb_bridge_vkCmdDrawIndexedIndirect)
    BVB_DEVICE_MATCH("vkCmdDrawIndirectCount",
                     bvb_bridge_vkCmdDrawIndirectCount)
    BVB_DEVICE_MATCH("vkCmdDrawIndirectCountKHR",
                     bvb_bridge_vkCmdDrawIndirectCount)
    BVB_DEVICE_MATCH("vkCmdDrawIndexedIndirectCount",
                     bvb_bridge_vkCmdDrawIndexedIndirectCount)
    BVB_DEVICE_MATCH("vkCmdDrawIndexedIndirectCountKHR",
                     bvb_bridge_vkCmdDrawIndexedIndirectCount)
    BVB_DEVICE_MATCH("vkCmdSetCullMode", bvb_bridge_vkCmdSetCullMode)
    BVB_DEVICE_MATCH("vkCmdSetCullModeEXT", bvb_bridge_vkCmdSetCullMode)
    BVB_DEVICE_MATCH("vkCmdSetFrontFace", bvb_bridge_vkCmdSetFrontFace)
    BVB_DEVICE_MATCH("vkCmdSetFrontFaceEXT", bvb_bridge_vkCmdSetFrontFace)
    BVB_DEVICE_MATCH("vkCmdSetPrimitiveTopology",
                     bvb_bridge_vkCmdSetPrimitiveTopology)
    BVB_DEVICE_MATCH("vkCmdSetPrimitiveTopologyEXT",
                     bvb_bridge_vkCmdSetPrimitiveTopology)
    BVB_DEVICE_MATCH("vkCmdSetDepthTestEnable",
                     bvb_bridge_vkCmdSetDepthTestEnable)
    BVB_DEVICE_MATCH("vkCmdSetDepthTestEnableEXT",
                     bvb_bridge_vkCmdSetDepthTestEnable)
    BVB_DEVICE_MATCH("vkCmdSetDepthWriteEnable",
                     bvb_bridge_vkCmdSetDepthWriteEnable)
    BVB_DEVICE_MATCH("vkCmdSetDepthWriteEnableEXT",
                     bvb_bridge_vkCmdSetDepthWriteEnable)
    BVB_DEVICE_MATCH("vkCmdSetDepthCompareOp",
                     bvb_bridge_vkCmdSetDepthCompareOp)
    BVB_DEVICE_MATCH("vkCmdSetDepthCompareOpEXT",
                     bvb_bridge_vkCmdSetDepthCompareOp)
    BVB_DEVICE_MATCH("vkCmdSetDepthBoundsTestEnable",
                     bvb_bridge_vkCmdSetDepthBoundsTestEnable)
    BVB_DEVICE_MATCH("vkCmdSetDepthBoundsTestEnableEXT",
                     bvb_bridge_vkCmdSetDepthBoundsTestEnable)
    BVB_DEVICE_MATCH("vkCmdSetStencilTestEnable",
                     bvb_bridge_vkCmdSetStencilTestEnable)
    BVB_DEVICE_MATCH("vkCmdSetStencilTestEnableEXT",
                     bvb_bridge_vkCmdSetStencilTestEnable)
    BVB_DEVICE_MATCH("vkCmdSetStencilOp", bvb_bridge_vkCmdSetStencilOp)
    BVB_DEVICE_MATCH("vkCmdSetStencilOpEXT", bvb_bridge_vkCmdSetStencilOp)
    BVB_DEVICE_MATCH("vkCmdSetRasterizerDiscardEnable",
                     bvb_bridge_vkCmdSetRasterizerDiscardEnable)
    BVB_DEVICE_MATCH("vkCmdSetRasterizerDiscardEnableEXT",
                     bvb_bridge_vkCmdSetRasterizerDiscardEnable)
    BVB_DEVICE_MATCH("vkCmdSetDepthBiasEnable",
                     bvb_bridge_vkCmdSetDepthBiasEnable)
    BVB_DEVICE_MATCH("vkCmdSetDepthBiasEnableEXT",
                     bvb_bridge_vkCmdSetDepthBiasEnable)
    BVB_DEVICE_MATCH("vkCmdSetPrimitiveRestartEnable",
                     bvb_bridge_vkCmdSetPrimitiveRestartEnable)
    BVB_DEVICE_MATCH("vkCmdSetPrimitiveRestartEnableEXT",
                     bvb_bridge_vkCmdSetPrimitiveRestartEnable)
    BVB_DEVICE_MATCH("vkCmdSetDepthBias", bvb_bridge_vkCmdSetDepthBias)
    BVB_DEVICE_MATCH("vkCmdSetDepthBounds", bvb_bridge_vkCmdSetDepthBounds)
    BVB_DEVICE_MATCH("vkCmdSetStencilCompareMask",
                     bvb_bridge_vkCmdSetStencilCompareMask)
    BVB_DEVICE_MATCH("vkCmdSetStencilWriteMask",
                     bvb_bridge_vkCmdSetStencilWriteMask)
    BVB_DEVICE_MATCH("vkCmdSetStencilReference",
                     bvb_bridge_vkCmdSetStencilReference)
    BVB_DEVICE_MATCH("vkCmdSetLineWidth", bvb_bridge_vkCmdSetLineWidth)
    BVB_DEVICE_MATCH("vkCmdSetBlendConstants",
                     bvb_bridge_vkCmdSetBlendConstants)
    BVB_DEVICE_MATCH("vkCmdClearDepthStencilImage",
                     bvb_bridge_vkCmdClearDepthStencilImage)
    BVB_DEVICE_MATCH("vkCmdClearAttachments",
                     bvb_bridge_vkCmdClearAttachments)
    BVB_DEVICE_MATCH("vkCmdUpdateBuffer", bvb_bridge_vkCmdUpdateBuffer)
    BVB_DEVICE_MATCH("vkCmdResetQueryPool",
                     bvb_bridge_vkCmdResetQueryPool)
    BVB_DEVICE_MATCH("vkCmdBeginQuery", bvb_bridge_vkCmdBeginQuery)
    BVB_DEVICE_MATCH("vkCmdEndQuery", bvb_bridge_vkCmdEndQuery)
    BVB_DEVICE_MATCH("vkCmdWriteTimestamp",
                     bvb_bridge_vkCmdWriteTimestamp)
    BVB_DEVICE_MATCH("vkCmdWriteTimestamp2",
                     bvb_bridge_vkCmdWriteTimestamp2)
    BVB_DEVICE_MATCH("vkCmdWriteTimestamp2KHR",
                     bvb_bridge_vkCmdWriteTimestamp2)
    BVB_DEVICE_MATCH("vkCmdBeginQueryIndexedEXT",
                     bvb_bridge_vkCmdBeginQueryIndexedEXT)
    BVB_DEVICE_MATCH("vkCmdEndQueryIndexedEXT",
                     bvb_bridge_vkCmdEndQueryIndexedEXT)
    BVB_DEVICE_MATCH("vkCmdCopyBuffer", bvb_bridge_vkCmdCopyBuffer)
    BVB_DEVICE_MATCH("vkCmdCopyBufferToImage",
                     bvb_bridge_vkCmdCopyBufferToImage)
    BVB_DEVICE_MATCH("vkCmdCopyImageToBuffer",
                     bvb_bridge_vkCmdCopyImageToBuffer)
    BVB_DEVICE_MATCH("vkCmdCopyImage", bvb_bridge_vkCmdCopyImage)
    BVB_DEVICE_MATCH("vkCmdBlitImage", bvb_bridge_vkCmdBlitImage)
    BVB_DEVICE_MATCH("vkCmdResolveImage", bvb_bridge_vkCmdResolveImage)
    BVB_DEVICE_MATCH("vkCmdCopyBuffer2", bvb_bridge_vkCmdCopyBuffer2)
    BVB_DEVICE_MATCH("vkCmdCopyBufferToImage2",
                     bvb_bridge_vkCmdCopyBufferToImage2)
    BVB_DEVICE_MATCH("vkCmdCopyImageToBuffer2",
                     bvb_bridge_vkCmdCopyImageToBuffer2)
    BVB_DEVICE_MATCH("vkCmdCopyImage2", bvb_bridge_vkCmdCopyImage2)
    BVB_DEVICE_MATCH("vkCmdBlitImage2", bvb_bridge_vkCmdBlitImage2)
    BVB_DEVICE_MATCH("vkCmdResolveImage2", bvb_bridge_vkCmdResolveImage2)
    BVB_DEVICE_MATCH("vkCreateFence", bvb_bridge_vkCreateFence)
    BVB_DEVICE_MATCH("vkDestroyFence", bvb_bridge_vkDestroyFence)
    BVB_DEVICE_MATCH("vkGetFenceStatus", bvb_bridge_vkGetFenceStatus)
    BVB_DEVICE_MATCH("vkWaitForFences", bvb_bridge_vkWaitForFences)
    BVB_DEVICE_MATCH("vkResetFences", bvb_bridge_vkResetFences)
    BVB_DEVICE_MATCH("vkCreateSemaphore", bvb_bridge_vkCreateSemaphore)
    BVB_DEVICE_MATCH("vkDestroySemaphore", bvb_bridge_vkDestroySemaphore)
    BVB_DEVICE_MATCH("vkGetSemaphoreCounterValue",
                     bvb_bridge_vkGetSemaphoreCounterValue)
    BVB_DEVICE_MATCH("vkGetSemaphoreCounterValueKHR",
                     bvb_bridge_vkGetSemaphoreCounterValue)
    BVB_DEVICE_MATCH("vkWaitSemaphores", bvb_bridge_vkWaitSemaphores)
    BVB_DEVICE_MATCH("vkWaitSemaphoresKHR", bvb_bridge_vkWaitSemaphores)
    BVB_DEVICE_MATCH("vkSignalSemaphore", bvb_bridge_vkSignalSemaphore)
    BVB_DEVICE_MATCH("vkSignalSemaphoreKHR", bvb_bridge_vkSignalSemaphore)
#undef BVB_DEVICE_MATCH
    return NULL;
}

BVB_GLOBAL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *name) {
    if (name != NULL && getenv("BVB_ICD_DIAGNOSTICS") != NULL) {
        fprintf(stderr, "BVB_ICD_GIPA_QUERY instance=%p name=%s\n",
                (void *)instance, name);
    }
    if (name == NULL ||
        (instance != VK_NULL_HANDLE && instance_proxy(instance) == NULL)) {
        return NULL;
    }
#define BVB_GLOBAL_MATCH(entry_name, function)                                 \
    if (strcmp(name, (entry_name)) == 0) {                                     \
        PFN_vkVoidFunction raw =                                                \
            BVB_ERASE_FUNCTION((function), __typeof__(&(function)));           \
        return bvb_first_rejection_wrap(                                        \
            (entry_name), BVB_DXVK_SCOPE_GLOBAL, raw);                         \
    }
    BVB_GLOBAL_MATCH("vkGetInstanceProcAddr", vkGetInstanceProcAddr)
    BVB_GLOBAL_MATCH("vkCreateInstance", bvb_bridge_vkCreateInstance)
    BVB_GLOBAL_MATCH("vkEnumerateInstanceExtensionProperties",
                     bvb_bridge_vkEnumerateInstanceExtensionProperties)
    BVB_GLOBAL_MATCH("vkEnumerateInstanceLayerProperties",
                     bvb_bridge_vkEnumerateInstanceLayerProperties)
    BVB_GLOBAL_MATCH("vkEnumerateInstanceVersion",
                     bvb_bridge_vkEnumerateInstanceVersion)
#undef BVB_GLOBAL_MATCH
    if (instance != VK_NULL_HANDLE) {
#define BVB_INSTANCE_MATCH(entry_name, function)                               \
        if (strcmp(name, (entry_name)) == 0) {                                 \
            PFN_vkVoidFunction raw =                                            \
                BVB_ERASE_FUNCTION((function), __typeof__(&(function)));       \
            return bvb_first_rejection_wrap(                                    \
                (entry_name), BVB_DXVK_SCOPE_INSTANCE, raw);                   \
        }
        BVB_INSTANCE_MATCH("vkDestroyInstance",
                           bvb_bridge_vkDestroyInstance)
        BVB_INSTANCE_MATCH("vkEnumeratePhysicalDevices",
                           bvb_bridge_vkEnumeratePhysicalDevices)
        BVB_INSTANCE_MATCH("vkGetPhysicalDeviceProperties",
                           bvb_bridge_vkGetPhysicalDeviceProperties)
        BVB_INSTANCE_MATCH("vkGetPhysicalDeviceQueueFamilyProperties",
                           bvb_bridge_vkGetPhysicalDeviceQueueFamilyProperties)
        BVB_INSTANCE_MATCH("vkGetPhysicalDeviceMemoryProperties",
                           bvb_bridge_vkGetPhysicalDeviceMemoryProperties)
        BVB_INSTANCE_MATCH("vkEnumerateDeviceExtensionProperties",
                           bvb_bridge_vkEnumerateDeviceExtensionProperties)
        BVB_INSTANCE_MATCH("vkGetPhysicalDeviceFeatures",
                           bvb_bridge_vkGetPhysicalDeviceFeatures)
        BVB_INSTANCE_MATCH("vkGetPhysicalDeviceFormatProperties",
                           bvb_bridge_vkGetPhysicalDeviceFormatProperties)
        BVB_INSTANCE_MATCH(
            "vkGetPhysicalDeviceImageFormatProperties",
            bvb_bridge_vkGetPhysicalDeviceImageFormatProperties)
        BVB_INSTANCE_MATCH(
            "vkGetPhysicalDeviceSparseImageFormatProperties",
            bvb_bridge_vkGetPhysicalDeviceSparseImageFormatProperties)
        BVB_INSTANCE_MATCH("vkGetPhysicalDeviceFeatures2",
                           bvb_bridge_vkGetPhysicalDeviceFeatures2)
        BVB_INSTANCE_MATCH("vkGetPhysicalDeviceFeatures2KHR",
                           bvb_bridge_vkGetPhysicalDeviceFeatures2)
        BVB_INSTANCE_MATCH("vkGetPhysicalDeviceProperties2",
                           bvb_bridge_vkGetPhysicalDeviceProperties2)
        BVB_INSTANCE_MATCH("vkGetPhysicalDeviceProperties2KHR",
                           bvb_bridge_vkGetPhysicalDeviceProperties2)
        BVB_INSTANCE_MATCH("vkGetPhysicalDeviceFormatProperties2",
                           bvb_bridge_vkGetPhysicalDeviceFormatProperties2)
        BVB_INSTANCE_MATCH("vkGetPhysicalDeviceFormatProperties2KHR",
                           bvb_bridge_vkGetPhysicalDeviceFormatProperties2)
        BVB_INSTANCE_MATCH(
            "vkGetPhysicalDeviceImageFormatProperties2",
            bvb_bridge_vkGetPhysicalDeviceImageFormatProperties2)
        BVB_INSTANCE_MATCH(
            "vkGetPhysicalDeviceImageFormatProperties2KHR",
            bvb_bridge_vkGetPhysicalDeviceImageFormatProperties2)
        BVB_INSTANCE_MATCH(
            "vkGetPhysicalDeviceQueueFamilyProperties2",
            bvb_bridge_vkGetPhysicalDeviceQueueFamilyProperties2)
        BVB_INSTANCE_MATCH(
            "vkGetPhysicalDeviceQueueFamilyProperties2KHR",
            bvb_bridge_vkGetPhysicalDeviceQueueFamilyProperties2)
        BVB_INSTANCE_MATCH("vkGetPhysicalDeviceMemoryProperties2",
                           bvb_bridge_vkGetPhysicalDeviceMemoryProperties2)
        BVB_INSTANCE_MATCH("vkGetPhysicalDeviceMemoryProperties2KHR",
                           bvb_bridge_vkGetPhysicalDeviceMemoryProperties2)
        BVB_INSTANCE_MATCH(
            "vkGetPhysicalDeviceSparseImageFormatProperties2",
            bvb_bridge_vkGetPhysicalDeviceSparseImageFormatProperties2)
        BVB_INSTANCE_MATCH(
            "vkGetPhysicalDeviceSparseImageFormatProperties2KHR",
            bvb_bridge_vkGetPhysicalDeviceSparseImageFormatProperties2)
        BVB_INSTANCE_MATCH(
            "vkGetPhysicalDeviceExternalBufferProperties",
            bvb_bridge_vkGetPhysicalDeviceExternalBufferProperties)
        BVB_INSTANCE_MATCH(
            "vkGetPhysicalDeviceExternalBufferPropertiesKHR",
            bvb_bridge_vkGetPhysicalDeviceExternalBufferProperties)
        BVB_INSTANCE_MATCH(
            "vkGetPhysicalDeviceExternalSemaphoreProperties",
            bvb_bridge_vkGetPhysicalDeviceExternalSemaphoreProperties)
        BVB_INSTANCE_MATCH(
            "vkGetPhysicalDeviceExternalSemaphorePropertiesKHR",
            bvb_bridge_vkGetPhysicalDeviceExternalSemaphoreProperties)
        BVB_INSTANCE_MATCH("vkCreateXlibSurfaceKHR",
                           bvb_bridge_vkCreateXlibSurfaceKHR)
        BVB_INSTANCE_MATCH("vkGetPhysicalDeviceXlibPresentationSupportKHR",
                           bvb_bridge_vkGetPhysicalDeviceXlibPresentationSupportKHR)
        BVB_INSTANCE_MATCH("vkCreateXcbSurfaceKHR",
                           bvb_bridge_vkCreateXcbSurfaceKHR)
        BVB_INSTANCE_MATCH("vkGetPhysicalDeviceXcbPresentationSupportKHR",
                           bvb_bridge_vkGetPhysicalDeviceXcbPresentationSupportKHR)
        BVB_INSTANCE_MATCH("vkCreateWaylandSurfaceKHR",
                           bvb_bridge_vkCreateWaylandSurfaceKHR)
        BVB_INSTANCE_MATCH(
            "vkGetPhysicalDeviceWaylandPresentationSupportKHR",
            bvb_bridge_vkGetPhysicalDeviceWaylandPresentationSupportKHR)
        BVB_INSTANCE_MATCH("vkDestroySurfaceKHR",
                           bvb_bridge_vkDestroySurfaceKHR)
        BVB_INSTANCE_MATCH("vkGetPhysicalDeviceSurfaceSupportKHR",
                           bvb_bridge_vkGetPhysicalDeviceSurfaceSupportKHR)
        BVB_INSTANCE_MATCH("vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
                           bvb_bridge_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
        BVB_INSTANCE_MATCH("vkGetPhysicalDeviceSurfaceFormatsKHR",
                           bvb_bridge_vkGetPhysicalDeviceSurfaceFormatsKHR)
        BVB_INSTANCE_MATCH("vkGetPhysicalDeviceSurfacePresentModesKHR",
                           bvb_bridge_vkGetPhysicalDeviceSurfacePresentModesKHR)
        BVB_INSTANCE_MATCH("vkCreateDevice",
                           bvb_bridge_vkCreateDevice)
#undef BVB_INSTANCE_MATCH
        if (strcmp(name, "vkGetDeviceProcAddr") == 0) {
            PFN_vkVoidFunction raw = BVB_ERASE_FUNCTION(
                vkGetDeviceProcAddr, PFN_vkGetDeviceProcAddr);
            return bvb_first_rejection_wrap(
                name, BVB_DXVK_SCOPE_DEVICE, raw);
        }
        PFN_vkVoidFunction stub = bvb_first_rejection_required_stub(
            name, BVB_DXVK_SCOPE_INSTANCE);
        if (stub != NULL) return stub;
        return vkGetDeviceProcAddr(VK_NULL_HANDLE, name);
    }
    return bvb_first_rejection_required_stub(
        name, BVB_DXVK_SCOPE_GLOBAL);
}

BVB_GLOBAL_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *version) {
    if (version == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    enum { BVB_ICD_INTERFACE_VERSION = 5 };
    if (*version > BVB_ICD_INTERFACE_VERSION) {
        *version = BVB_ICD_INTERFACE_VERSION;
    }
    atomic_store(&bvb_icd_loader_active, true);
    return VK_SUCCESS;
}

BVB_GLOBAL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *name) {
    if (instance == VK_NULL_HANDLE || name == NULL) return NULL;
    return vkGetInstanceProcAddr(instance, name);
}

BVB_GLOBAL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *name) {
    if (name == NULL) return NULL;
    if (strcmp(name, "vk_icdNegotiateLoaderICDInterfaceVersion") == 0) {
        return BVB_ERASE_FUNCTION(vk_icdNegotiateLoaderICDInterfaceVersion,
                                  PFN_vk_icdNegotiateLoaderICDInterfaceVersion);
    }
    if (strcmp(name, "vk_icdGetPhysicalDeviceProcAddr") == 0) {
        return BVB_ERASE_FUNCTION(vk_icdGetPhysicalDeviceProcAddr,
                                  PFN_vk_icdGetPhysicalDeviceProcAddr);
    }
    return vkGetInstanceProcAddr(instance, name);
}
