#define VK_NO_PROTOTYPES

#include <bvb/global_dispatch.h>
#include <bvb/handle.h>
#include <bvb/protocol.h>
#include <bvb/transport.h>
#include <bvb/vulkan_discovery.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
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

struct bvb_global_client_state {
    pthread_mutex_t mutex;
    int socket_fd;
    uint32_t next_request_id;
    bool info_valid;
    struct bvb_vulkan_global_info info;
    struct bvb_physical_device_proxy *physical_devices;
    struct bvb_device_proxy *devices;
    struct bvb_queue_proxy *queues;
};

static const uint64_t bvb_dispatch_anchor = UINT64_C(0x4256424449535030);
static struct bvb_global_client_state bvb_global_client = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .socket_fd = -1,
    .next_request_id = UINT32_C(0x42565000),
};

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
        return -ENOENT;
    }
    int socket_fd = bvb_transport_connect(socket_path, geteuid());
    if (socket_fd < 0) {
        return socket_fd;
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
    if (proxy == NULL || proxy->dispatch != &bvb_dispatch_anchor ||
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
    if (proxy == NULL || proxy->dispatch != &bvb_dispatch_anchor ||
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
    proxy->dispatch = &bvb_dispatch_anchor;
    proxy->magic = BVB_PHYSICAL_DEVICE_PROXY_MAGIC;
    proxy->wire_id = wire_id;
    proxy->parent_id = parent_id;
    proxy->next = bvb_global_client.physical_devices;
    bvb_global_client.physical_devices = proxy;
    return proxy;
}

static struct bvb_device_proxy *device_proxy(VkDevice device) {
    struct bvb_device_proxy *proxy = (struct bvb_device_proxy *)device;
    if (proxy == NULL || proxy->dispatch != &bvb_dispatch_anchor ||
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
    if (proxy == NULL || proxy->dispatch != &bvb_dispatch_anchor ||
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
    proxy->dispatch = &bvb_dispatch_anchor;
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

static void remove_device_proxy_locked(struct bvb_device_proxy *target) {
    struct bvb_device_proxy **cursor = &bvb_global_client.devices;
    while (*cursor != NULL) {
        struct bvb_device_proxy *proxy = *cursor;
        if (proxy == target) {
            *cursor = proxy->next;
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

static VkResult VKAPI_CALL bvb_bridge_vkEnumerateInstanceExtensionProperties(
    const char *layer_name, uint32_t *property_count,
    VkExtensionProperties *properties) {
    if (property_count == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (layer_name != NULL) {
        return VK_ERROR_LAYER_NOT_PRESENT;
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
    return capacity < info.exposed_extension_count ? VK_INCOMPLETE : VK_SUCCESS;
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
    if (instance != NULL) {
        *instance = VK_NULL_HANDLE;
    }
    if (create_info == NULL || instance == NULL ||
        create_info->sType != VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO ||
        create_info->pNext != NULL || create_info->flags != 0U ||
        allocator != NULL ||
        (create_info->pApplicationInfo != NULL &&
         (create_info->pApplicationInfo->sType !=
              VK_STRUCTURE_TYPE_APPLICATION_INFO ||
          create_info->pApplicationInfo->pNext != NULL))) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (create_info->enabledLayerCount != 0U) {
        return VK_ERROR_LAYER_NOT_PRESENT;
    }
    if (create_info->enabledExtensionCount != 0U) {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
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
        .enabled_extension_count = create_info->enabledExtensionCount,
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
        .opcode = BVB_OPCODE_VULKAN_INSTANCE_CREATE,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_INSTANCE_CREATE_REQUEST_SIZE,
    };
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_instance_create_request(
            request.payload, &create_request);
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
    struct bvb_vulkan_instance_create_response create_response;
    if (result == 0) {
        result = bvb_protocol_decode_vulkan_instance_create_response(
            response.payload, &create_response);
    }
    if (result == 0 && create_response.vulkan_result == VK_SUCCESS &&
        bvb_handle_expect(create_response.instance_id,
                          BVB_OBJECT_INSTANCE) != 0) {
        result = -EPROTO;
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
    proxy->dispatch = &bvb_dispatch_anchor;
    proxy->magic = BVB_INSTANCE_PROXY_MAGIC;
    proxy->wire_id = create_response.instance_id;
    *instance = (VkInstance)proxy;
    return VK_SUCCESS;
}

static void VKAPI_CALL bvb_bridge_vkDestroyInstance(
    VkInstance instance, const VkAllocationCallbacks *allocator) {
    struct bvb_instance_proxy *proxy = instance_proxy(instance);
    if (proxy == NULL || allocator != NULL) {
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
        remove_physical_proxies_locked(proxy->wire_id);
        proxy->magic = 0U;
    }
    (void)pthread_mutex_unlock(&bvb_global_client.mutex);
    if (result == 0) {
        free(proxy);
    }
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

static VkResult VKAPI_CALL bvb_bridge_vkCreateDevice(
    VkPhysicalDevice physical_device, const VkDeviceCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkDevice *device) {
    if (device != NULL) {
        *device = VK_NULL_HANDLE;
    }
    struct bvb_physical_device_proxy *physical =
        physical_device_proxy(physical_device);
    if (physical == NULL || create_info == NULL || device == NULL ||
        allocator != NULL ||
        create_info->sType != VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO ||
        create_info->pNext != NULL || create_info->flags != 0U ||
        create_info->queueCreateInfoCount != 1U ||
        create_info->pQueueCreateInfos == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (create_info->enabledLayerCount != 0U) {
        return VK_ERROR_LAYER_NOT_PRESENT;
    }
    if (create_info->enabledExtensionCount != 0U) {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
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
        .opcode = BVB_OPCODE_VULKAN_DEVICE_CREATE,
        .request_id = next_request_id_locked(),
        .payload_length = BVB_VULKAN_DEVICE_CREATE_REQUEST_SIZE,
    };
    if (result == 0) {
        result = bvb_protocol_encode_vulkan_device_create_request(
            request.payload, &create_request);
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
    struct bvb_vulkan_device_create_response decoded;
    if (result == 0) {
        result = bvb_protocol_decode_vulkan_device_create_response(
            response.payload, &decoded);
    }
    if (result == 0 && decoded.vulkan_result == VK_SUCCESS &&
        bvb_handle_expect(decoded.device_id, BVB_OBJECT_DEVICE) != 0) {
        result = -EPROTO;
    }
    if (result == 0 && decoded.vulkan_result == VK_SUCCESS) {
        proxy->dispatch = &bvb_dispatch_anchor;
        proxy->magic = BVB_DEVICE_PROXY_MAGIC;
        proxy->wire_id = decoded.device_id;
        proxy->parent_id = physical->wire_id;
        proxy->instance_id = physical->parent_id;
        proxy->next = bvb_global_client.devices;
        bvb_global_client.devices = proxy;
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
#undef BVB_DEVICE_MATCH
    return NULL;
}

BVB_GLOBAL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *name) {
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
