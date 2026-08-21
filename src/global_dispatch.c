#define VK_NO_PROTOTYPES

#include <bvb/global_dispatch.h>
#include <bvb/handle.h>
#include <bvb/protocol.h>
#include <bvb/transport.h>
#include <bvb/vulkan_descriptor_wire.h>
#include <bvb/vulkan_discovery.h>
#include <bvb/vulkan_pipeline_wire.h>

#include <vulkan/vk_icd.h>
#include <vulkan/vk_layer.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    struct bvb_command_buffer_proxy *next;
};

struct bvb_resource_proxy {
    uint64_t wire_id;
    uint64_t parent_id;
    uint64_t allocation_size;
    uint64_t mapped_offset;
    uint64_t mapped_size;
    uint8_t *mapped_bytes;
    enum bvb_object_type type;
    struct bvb_resource_proxy *next;
};

struct bvb_surface_proxy {
    uint64_t wire_id;
    uint64_t parent_id;
    struct bvb_surface_proxy *next;
};

struct bvb_global_client_state {
    pthread_mutex_t mutex;
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
    uint64_t next_surface_serial;
};

static const uint64_t bvb_dispatch_anchor = UINT64_C(0x4256424449535030);
static atomic_bool bvb_icd_loader_active;
static struct bvb_global_client_state bvb_global_client = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .socket_fd = -1,
    .next_request_id = UINT32_C(0x42565000),
    .next_surface_serial = 1U,
};

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

static int exchange_locked(const struct bvb_protocol_packet *request,
                           struct bvb_protocol_packet *response) {
    int result = bvb_transport_send(bvb_global_client.socket_fd, request);
    if (result == 0) {
        result = bvb_transport_receive(bvb_global_client.socket_fd, response);
    }
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

static uint32_t next_request_id_locked(void) {
    ++bvb_global_client.next_request_id;
    if (bvb_global_client.next_request_id == 0U) {
        ++bvb_global_client.next_request_id;
    }
    return bvb_global_client.next_request_id;
}

static int connect_locked(void) {
    if (bvb_global_client.socket_fd >= 0) {
        return 0;
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
    int result = bvb_protocol_encode_hello_request(request.payload, &hello);
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

static struct bvb_resource_proxy *resource_proxy_locked(
    uint64_t wire_id, enum bvb_object_type type) {
    if (bvb_handle_expect(wire_id, type) != 0) return NULL;
    for (struct bvb_resource_proxy *proxy = bvb_global_client.resources;
         proxy != NULL; proxy = proxy->next)
        if (proxy->wire_id == wire_id)
            return proxy->type == type ? proxy : NULL;
    return NULL;
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

static void remove_command_buffer_proxy_locked(
    struct bvb_command_buffer_proxy *target) {
    struct bvb_command_buffer_proxy **cursor =
        &bvb_global_client.command_buffers;
    while (*cursor != NULL) {
        struct bvb_command_buffer_proxy *proxy = *cursor;
        if (proxy == target) {
            *cursor = proxy->next;
            proxy->magic = 0U;
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
            proxy->magic = 0U;
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

static void remove_resource_proxy_locked(struct bvb_resource_proxy *target) {
    struct bvb_resource_proxy **cursor = &bvb_global_client.resources;
    while (*cursor != NULL) {
        struct bvb_resource_proxy *proxy = *cursor;
        if (proxy == target) {
            *cursor = proxy->next;
            free(proxy->mapped_bytes);
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
            free(proxy);
        } else {
            cursor = &proxy->next;
        }
    }
}

static void remove_resources_for_device_locked(uint64_t parent_id) {
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
            free(proxy->mapped_bytes);
            free(proxy);
        } else {
            cursor = &proxy->next;
        }
    }
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

static VkResult VKAPI_CALL bvb_bridge_vkCreateXlibSurfaceKHR(
    VkInstance instance, const void *create_info,
    const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface) {
    return create_virtual_surface(instance, create_info, allocator, surface);
}

static VkResult VKAPI_CALL bvb_bridge_vkCreateXcbSurfaceKHR(
    VkInstance instance, const void *create_info,
    const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface) {
    return create_virtual_surface(instance, create_info, allocator, surface);
}

static VkResult VKAPI_CALL bvb_bridge_vkCreateWaylandSurfaceKHR(
    VkInstance instance, const void *create_info,
    const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface) {
    return create_virtual_surface(instance, create_info, allocator, surface);
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
        {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
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
            native_extensions[native_extension_count++] = name;
        }
    }
    if (getenv("BVB_ICD_DIAGNOSTICS") != NULL) {
        for (uint32_t index = 0U;
             index < create_info->enabledExtensionCount; ++index) {
            fprintf(stderr,
                    "BVB_ICD_CREATE_DEVICE_EXTENSION index=%u name=%s\n",
                    index, create_info->ppEnabledExtensionNames[index]);
        }
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

static VkResult result_request_locked(
    uint16_t opcode, const uint8_t *payload, uint32_t payload_length) {
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
    int32_t vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    if (result == 0) {
        result = bvb_protocol_decode_vulkan_result(
            response.payload, &vulkan_result);
    }
    return result == 0 ? (VkResult)vulkan_result
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
    if (pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
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
        free(command_state);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (decoded.vulkan_result != VK_SUCCESS) {
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
            0U ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
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
    if (command_state == NULL ||
        pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
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
    state->next = bvb_global_client.resources;
    bvb_global_client.resources = state;
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
    struct bvb_protocol_packet request = {0};
    request.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_VULKAN_DESCRIPTOR_SET_ALLOCATE,
        .request_id = next_request_id_locked(),
        .payload_length = payload_length,
    };
    if (result == 0) memcpy(request.payload, payload, payload_length);
    struct bvb_protocol_packet response = {0};
    if (result == 0) result = connect_locked();
    if (result == 0) result = exchange_locked(&request, &response);
    if (result == 0 && response.header.status != 0) result = -EPROTO;
    struct bvb_vulkan_descriptor_set_allocate_response allocated = {0};
    if (result == 0) {
        result = bvb_protocol_decode_vulkan_descriptor_set_allocate_response(
            response.payload, response.header.payload_length, &allocated);
    }
    VkResult vulkan_result = result == 0
        ? (VkResult)allocated.vulkan_result : VK_ERROR_INITIALIZATION_FAILED;
    if (vulkan_result == VK_SUCCESS &&
        allocated.descriptor_set_count != allocate_info->descriptorSetCount) {
        vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
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

static VkResult VKAPI_CALL bvb_bridge_vkCreateBuffer(
    VkDevice device, const VkBufferCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkBuffer *buffer) {
    if (buffer != NULL) *buffer = VK_NULL_HANDLE;
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || create_info == NULL || buffer == NULL ||
        allocator != NULL || create_info->sType != VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO ||
        create_info->pNext != NULL || create_info->flags != 0U ||
        create_info->size == 0U || create_info->size > 16U * 1024U * 1024U ||
        create_info->usage != VK_BUFFER_USAGE_TRANSFER_DST_BIT ||
        create_info->sharingMode != VK_SHARING_MODE_EXCLUSIVE ||
        create_info->queueFamilyIndexCount != 0U) return VK_ERROR_FEATURE_NOT_PRESENT;
    struct bvb_resource_proxy *state = calloc(1, sizeof(*state));
    if (state == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
    const struct bvb_vulkan_buffer_create_request decoded = {
        .device_id = device_state->wire_id,
        .size = create_info->size,
        .usage = create_info->usage,
        .flags = create_info->flags,
    };
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
        if (type == BVB_OBJECT_DESCRIPTOR_POOL) {
            remove_descriptor_sets_for_pool_locked(wire_id);
        }
        remove_resource_proxy_locked(state);
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

static VkResult VKAPI_CALL bvb_bridge_vkAllocateMemory(
    VkDevice device, const VkMemoryAllocateInfo *allocate_info,
    const VkAllocationCallbacks *allocator, VkDeviceMemory *memory) {
    if (memory != NULL) *memory = VK_NULL_HANDLE;
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (device_state == NULL || allocate_info == NULL || memory == NULL ||
        allocator != NULL ||
        allocate_info->sType != VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO ||
        allocate_info->pNext != NULL || allocate_info->allocationSize == 0U ||
        allocate_info->allocationSize > 16U * 1024U * 1024U)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    struct bvb_resource_proxy *state = calloc(1, sizeof(*state));
    if (state == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
    const struct bvb_vulkan_memory_allocate_request decoded = {
        .device_id = device_state->wire_id,
        .allocation_size = allocate_info->allocationSize,
        .memory_type_index = allocate_info->memoryTypeIndex,
    };
    uint8_t payload[BVB_VULKAN_MEMORY_ALLOCATE_REQUEST_SIZE];
    int result = bvb_protocol_encode_vulkan_memory_allocate_request(
        payload, &decoded);
    if (result != 0 || pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        free(state);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    state->allocation_size = allocate_info->allocationSize;
    uint64_t wire_id = 0U;
    VkResult vulkan_result = create_resource_locked(
        BVB_OPCODE_VULKAN_MEMORY_ALLOCATE, payload, sizeof(payload),
        BVB_OBJECT_DEVICE_MEMORY, device_state->wire_id, state, &wire_id);
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (vulkan_result != VK_SUCCESS) {
        free(state);
        return vulkan_result;
    }
    memcpy(memory, &wire_id, sizeof(*memory));
    return VK_SUCCESS;
}

static void VKAPI_CALL bvb_bridge_vkFreeMemory(
    VkDevice device, VkDeviceMemory memory,
    const VkAllocationCallbacks *allocator) {
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
        if (result == 0)
            vulkan_result = result_request_locked(
                BVB_OPCODE_VULKAN_BUFFER_BIND, payload, sizeof(payload));
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return vulkan_result;
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
    if (state != NULL && state->parent_id == device_state->wire_id &&
        state->mapped_bytes == NULL && effective_size != 0U &&
        offset <= state->allocation_size &&
        effective_size <= state->allocation_size - offset &&
        effective_size <= SIZE_MAX) {
        uint8_t *shadow = malloc((size_t)effective_size);
        if (shadow == NULL) {
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
        } else {
            result = memory_read_locked(state, offset, shadow, effective_size);
            if (result == VK_SUCCESS) {
                state->mapped_offset = offset;
                state->mapped_size = effective_size;
                state->mapped_bytes = shadow;
                *data = shadow;
            } else {
                free(shadow);
            }
        }
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return result;
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
        (void)mapped_range_locked(state, state->mapped_offset,
                                  state->mapped_size, false);
        free(state->mapped_bytes);
        state->mapped_bytes = NULL;
        state->mapped_offset = 0U;
        state->mapped_size = 0U;
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
    for (struct bvb_resource_proxy *state = bvb_global_client.resources;
         state != NULL; state = state->next) {
        if (state->type == BVB_OBJECT_DEVICE_MEMORY &&
            state->parent_id == device_id && state->mapped_bytes != NULL) {
            VkResult result = mapped_range_locked(
                state, state->mapped_offset, state->mapped_size, false);
            if (result != VK_SUCCESS) return result;
        }
    }
    return VK_SUCCESS;
}

static void VKAPI_CALL bvb_bridge_vkCmdFillBuffer(
    VkCommandBuffer command_buffer, VkBuffer destination_buffer,
    VkDeviceSize destination_offset, VkDeviceSize size, uint32_t data) {
    struct bvb_command_buffer_proxy *command_state =
        command_buffer_proxy(command_buffer);
    const uint64_t buffer_id = non_dispatchable_wire_id(
        &destination_buffer, sizeof(destination_buffer));
    if (command_state == NULL || size == 0U || (destination_offset & 3U) != 0U ||
        (size & 3U) != 0U || pthread_mutex_lock(&bvb_global_client.mutex) != 0)
        return;
    struct bvb_resource_proxy *buffer_state =
        resource_proxy_locked(buffer_id, BVB_OBJECT_BUFFER);
    int result = buffer_state != NULL &&
                         buffer_state->parent_id == command_state->device_id
                     ? connect_locked() : -EINVAL;
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
    const uint64_t fence_id = non_dispatchable_wire_id(&fence, sizeof(fence));
    struct bvb_resource_proxy *fence_state = fence == VK_NULL_HANDLE
        ? NULL : resource_proxy_locked(fence_id, BVB_OBJECT_FENCE);
    if (fence != VK_NULL_HANDLE &&
        (fence_state == NULL ||
         fence_state->parent_id != queue_state->parent_id)) {
        (void)pthread_mutex_unlock(&bvb_global_client.mutex);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkResult upload_result =
        flush_mapped_resources_locked(queue_state->parent_id);
    if (upload_result != VK_SUCCESS) {
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
        decoded.commands[index] =
            (struct bvb_vulkan_submit_2_command_record){
                .command_buffer_id = state->wire_id,
                .device_mask = info->deviceMask,
            };
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
    uint8_t payload[BVB_VULKAN_SUBMIT_2_MAX_SIZE];
    uint32_t payload_length = 0U;
    if (result == 0 && upload_result == VK_SUCCESS)
        result = bvb_protocol_encode_vulkan_queue_submit_2_request(
            payload, &decoded, &payload_length);
    VkResult vulkan_result = result == 0 && upload_result == VK_SUCCESS
        ? result_request_locked(BVB_OPCODE_VULKAN_QUEUE_SUBMIT_2,
                                payload, payload_length)
        : upload_result;
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
        create_info->sType != VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
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
    if (getenv("BVB_ICD_DIAGNOSTICS") != NULL) {
        fprintf(stderr,
                "BVB_ICD_VIRTUAL_SWAPCHAIN_CREATE surface=%llu "
                "extent=%ux%u activity=%ux%u backing=activity "
                "frame_transport=unavailable status=%d\n",
                (unsigned long long)non_dispatchable_wire_id(
                    &create_info->surface, sizeof(create_info->surface)),
                create_info->imageExtent.width,
                create_info->imageExtent.height, activity.width,
                activity.height, result);
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result != 0) return VK_ERROR_SURFACE_LOST_KHR;

    /*
     * The Activity owns the real Android swapchain. Its external-image frame
     * transport is not connected to game-owned images yet, so creating a
     * usable proxy here would advertise pixels that cannot be presented.
     */
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

static void VKAPI_CALL bvb_bridge_vkDestroySwapchainKHR(
    VkDevice device, VkSwapchainKHR swapchain,
    const VkAllocationCallbacks *allocator) {
    (void)swapchain;
    (void)allocator;
    (void)device_proxy(device);
}

static VkResult VKAPI_CALL bvb_bridge_vkGetSwapchainImagesKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint32_t *image_count,
    VkImage *images) {
    (void)swapchain;
    (void)images;
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (image_count != NULL) *image_count = 0U;
    return device_state != NULL && device_state->virtual_swapchain_enabled &&
                   image_count != NULL
               ? VK_ERROR_FEATURE_NOT_PRESENT
               : VK_ERROR_INITIALIZATION_FAILED;
}

static VkResult VKAPI_CALL bvb_bridge_vkAcquireNextImageKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, uint32_t *image_index) {
    (void)swapchain;
    (void)timeout;
    (void)semaphore;
    (void)fence;
    struct bvb_device_proxy *device_state = device_proxy(device);
    if (image_index != NULL) *image_index = 0U;
    return device_state != NULL && device_state->virtual_swapchain_enabled &&
                   image_index != NULL
               ? VK_ERROR_FEATURE_NOT_PRESENT
               : VK_ERROR_INITIALIZATION_FAILED;
}

static VkResult VKAPI_CALL bvb_bridge_vkAcquireNextImage2KHR(
    VkDevice device, const VkAcquireNextImageInfoKHR *acquire_info,
    uint32_t *image_index) {
    if (acquire_info == NULL ||
        acquire_info->sType != VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR) {
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
        present_info->sType != VK_STRUCTURE_TYPE_PRESENT_INFO_KHR) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (pthread_mutex_lock(&bvb_global_client.mutex) != 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    bool enabled = false;
    for (struct bvb_device_proxy *device = bvb_global_client.devices;
         device != NULL; device = device->next) {
        if (device->wire_id == queue_state->parent_id) {
            enabled = device->virtual_swapchain_enabled;
            break;
        }
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    return enabled ? VK_ERROR_FEATURE_NOT_PRESENT
                   : VK_ERROR_INITIALIZATION_FAILED;
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
        return BVB_ERASE_FUNCTION((function), __typeof__(&(function)));       \
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
    BVB_DEVICE_MATCH("vkAllocateDescriptorSets",
                     bvb_bridge_vkAllocateDescriptorSets)
    BVB_DEVICE_MATCH("vkCreateSampler", bvb_bridge_vkCreateSampler)
    BVB_DEVICE_MATCH("vkDestroySampler", bvb_bridge_vkDestroySampler)
    BVB_DEVICE_MATCH("vkUpdateDescriptorSets",
                     bvb_bridge_vkUpdateDescriptorSets)
    BVB_DEVICE_MATCH("vkCreatePipelineLayout",
                     bvb_bridge_vkCreatePipelineLayout)
    BVB_DEVICE_MATCH("vkDestroyPipelineLayout",
                     bvb_bridge_vkDestroyPipelineLayout)
    BVB_DEVICE_MATCH("vkCreateBuffer", bvb_bridge_vkCreateBuffer)
    BVB_DEVICE_MATCH("vkDestroyBuffer", bvb_bridge_vkDestroyBuffer)
    BVB_DEVICE_MATCH("vkGetBufferMemoryRequirements",
                     bvb_bridge_vkGetBufferMemoryRequirements)
    BVB_DEVICE_MATCH("vkAllocateMemory", bvb_bridge_vkAllocateMemory)
    BVB_DEVICE_MATCH("vkFreeMemory", bvb_bridge_vkFreeMemory)
    BVB_DEVICE_MATCH("vkBindBufferMemory", bvb_bridge_vkBindBufferMemory)
    BVB_DEVICE_MATCH("vkMapMemory", bvb_bridge_vkMapMemory)
    BVB_DEVICE_MATCH("vkUnmapMemory", bvb_bridge_vkUnmapMemory)
    BVB_DEVICE_MATCH("vkFlushMappedMemoryRanges",
                     bvb_bridge_vkFlushMappedMemoryRanges)
    BVB_DEVICE_MATCH("vkInvalidateMappedMemoryRanges",
                     bvb_bridge_vkInvalidateMappedMemoryRanges)
    BVB_DEVICE_MATCH("vkCmdFillBuffer", bvb_bridge_vkCmdFillBuffer)
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
        return BVB_ERASE_FUNCTION((function), __typeof__(&(function)));        \
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
            return BVB_ERASE_FUNCTION((function), __typeof__(&(function)));    \
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
            return BVB_ERASE_FUNCTION(vkGetDeviceProcAddr,
                                      PFN_vkGetDeviceProcAddr);
        }
        return vkGetDeviceProcAddr(VK_NULL_HANDLE, name);
    }
    return NULL;
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
