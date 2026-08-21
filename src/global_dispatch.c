#define VK_NO_PROTOTYPES

#include <bvb/global_dispatch.h>
#include <bvb/handle.h>
#include <bvb/protocol.h>
#include <bvb/transport.h>
#include <bvb/vulkan_discovery.h>

#include <vulkan/vk_icd.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
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
    struct bvb_physical_device_proxy *next;
};

struct bvb_device_proxy {
    const void *dispatch;
    uint64_t magic;
    uint64_t wire_id;
    uint64_t parent_id;
    uint64_t instance_id;
    struct bvb_device_proxy *next;
};

struct bvb_queue_proxy {
    const void *dispatch;
    uint64_t magic;
    uint64_t wire_id;
    uint64_t parent_id;
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

static void remove_resources_for_device_locked(uint64_t parent_id) {
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
    struct bvb_vulkan_extension_page page;
    int result = extension_page_locked(proxy, 0U, 0U, &page);
    VkResult vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    uint32_t available = 0U;
    if (result == 0) {
        vulkan_result = (VkResult)page.vulkan_result;
        available = page.total_count;
    }
    if (result == 0 && vulkan_result == VK_SUCCESS && properties != NULL) {
        const uint32_t target = capacity < available ? capacity : available;
        uint32_t written = 0U;
        while (written < target) {
            uint32_t requested = target - written;
            if (requested > BVB_VULKAN_EXTENSION_PAGE_CAPACITY) {
                requested = BVB_VULKAN_EXTENSION_PAGE_CAPACITY;
            }
            result = extension_page_locked(
                proxy, written, requested, &page);
            if (result != 0 || page.vulkan_result != VK_SUCCESS ||
                page.total_count != available || page.first != written ||
                page.count == 0U || page.count > requested) {
                result = result != 0 ? result : -EPROTO;
                break;
            }
            memcpy(properties + written, page.properties,
                   page.count * sizeof(*properties));
            written += page.count;
        }
        *property_count = written;
        if (result == 0) {
            vulkan_result = capacity < available ? VK_INCOMPLETE : VK_SUCCESS;
        }
    } else if (result == 0 && vulkan_result == VK_SUCCESS) {
        *property_count = available;
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

static void VKAPI_CALL bvb_bridge_vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice physical_device, VkPhysicalDeviceFeatures2 *features) {
    if (features == NULL ||
        features->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
        return;
    }
    bvb_bridge_vkGetPhysicalDeviceFeatures(
        physical_device, &features->features);
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
        create_info->queueCreateInfoCount != 1U ||
        create_info->pQueueCreateInfos == NULL) {
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
    if (getenv("BVB_ICD_DIAGNOSTICS") != NULL) {
        for (uint32_t index = 0U;
             index < create_info->enabledExtensionCount; ++index) {
            fprintf(stderr,
                    "BVB_ICD_CREATE_DEVICE_EXTENSION index=%u name=%s\n",
                    index, create_info->ppEnabledExtensionNames[index]);
        }
    }
    if (create_info->pEnabledFeatures != NULL) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    const VkDeviceQueueCreateInfo *queue_info = create_info->pQueueCreateInfos;
    if (queue_info->sType != VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO ||
        queue_info->pNext != NULL || queue_info->flags != 0U ||
        queue_info->queueCount != 1U || queue_info->pQueuePriorities == NULL ||
        !(queue_info->pQueuePriorities[0] >= 0.0F &&
          queue_info->pQueuePriorities[0] <= 1.0F)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    struct bvb_device_proxy *proxy = calloc(1, sizeof(*proxy));
    if (proxy == NULL) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    uint32_t priority_bits = 0U;
    memcpy(&priority_bits, &queue_info->pQueuePriorities[0],
           sizeof(priority_bits));
    const struct bvb_vulkan_device_create_request create_request = {
        .physical_device_id = physical->wire_id,
        .flags = create_info->flags,
        .queue_family_index = queue_info->queueFamilyIndex,
        .queue_count = queue_info->queueCount,
        .queue_priority_bits = priority_bits,
        .enabled_layer_count = create_info->enabledLayerCount,
        .enabled_extension_count = create_info->enabledExtensionCount,
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
        .opcode = create_info->enabledExtensionCount == 0U
                      ? BVB_OPCODE_VULKAN_DEVICE_CREATE
                      : BVB_OPCODE_VULKAN_DEVICE_CREATE_EXTENDED,
        .request_id = next_request_id_locked(),
    };
    if (result == 0) {
        if (create_info->enabledExtensionCount == 0U) {
            request.header.payload_length =
                BVB_VULKAN_DEVICE_CREATE_REQUEST_SIZE;
            result = bvb_protocol_encode_vulkan_device_create_request(
                request.payload, &create_request);
        } else {
            struct bvb_vulkan_device_create_extended_request extended = {
                .base = create_request,
            };
            for (uint32_t index = 0U;
                 index < create_info->enabledExtensionCount; ++index) {
                const char *name =
                    create_info->ppEnabledExtensionNames[index];
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
    if (result == 0) {
        queue_state = queue_proxy_locked(queue_id, proxy->wire_id);
        if (queue_state == NULL) {
            result = -ENOMEM;
        }
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
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
    if (result == 0) remove_resource_proxy_locked(state);
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
    BVB_DEVICE_MATCH("vkQueueWaitIdle", bvb_bridge_vkQueueWaitIdle)
    BVB_DEVICE_MATCH("vkDeviceWaitIdle", bvb_bridge_vkDeviceWaitIdle)
    BVB_DEVICE_MATCH("vkCreateCommandPool", bvb_bridge_vkCreateCommandPool)
    BVB_DEVICE_MATCH("vkDestroyCommandPool", bvb_bridge_vkDestroyCommandPool)
    BVB_DEVICE_MATCH("vkResetCommandPool", bvb_bridge_vkResetCommandPool)
    BVB_DEVICE_MATCH("vkAllocateCommandBuffers",
                     bvb_bridge_vkAllocateCommandBuffers)
    BVB_DEVICE_MATCH("vkFreeCommandBuffers", bvb_bridge_vkFreeCommandBuffers)
    BVB_DEVICE_MATCH("vkBeginCommandBuffer", bvb_bridge_vkBeginCommandBuffer)
    BVB_DEVICE_MATCH("vkEndCommandBuffer", bvb_bridge_vkEndCommandBuffer)
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
