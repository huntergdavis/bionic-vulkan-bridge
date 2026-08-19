#define VK_NO_PROTOTYPES

#include <bvb/handle.h>
#include <bvb/vulkan_global.h>

#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    BVB_GLOBAL_OBJECT_CAPACITY = 64,
};

struct bvb_device_metadata {
    uint64_t device_id;
    uint32_t queue_family_index;
    uint32_t queue_count;
};

struct bvb_vulkan_global_context {
    void *loader;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    PFN_vkCreateInstance create_instance;
    PFN_vkDestroyInstance destroy_instance;
    PFN_vkGetDeviceProcAddr get_device_proc_addr;
    struct bvb_vulkan_global_info info;
    struct bvb_handle_entry object_entries[BVB_GLOBAL_OBJECT_CAPACITY];
    struct bvb_handle_table objects;
    uint64_t next_instance_serial;
    uint64_t next_physical_device_serial;
    uint64_t next_device_serial;
    uint64_t next_queue_serial;
    struct bvb_device_metadata device_metadata[BVB_GLOBAL_OBJECT_CAPACITY];
};

static void set_error(char *output, size_t output_size, const char *format, ...) {
    if (output == NULL || output_size == 0U) {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(output, output_size, format, arguments);
    va_end(arguments);
}

static PFN_vkVoidFunction symbol_from_loader(void *loader, const char *name) {
    void *raw = dlsym(loader, name);
    PFN_vkVoidFunction function = NULL;
    if (raw != NULL) {
        memcpy(&function, &raw, sizeof(function));
    }
    return function;
}

static uint64_t handle_bits(const void *handle, size_t size) {
    uint64_t bits = 0U;
    if (handle != NULL && size <= sizeof(bits)) {
        memcpy(&bits, handle, size);
    }
    return bits;
}

static VkInstance instance_from_bits(uint64_t bits) {
    VkInstance instance = VK_NULL_HANDLE;
    _Static_assert(sizeof(instance) <= sizeof(bits),
                   "VkInstance exceeds bridge handle width");
    memcpy(&instance, &bits, sizeof(instance));
    return instance;
}

static VkPhysicalDevice physical_device_from_bits(uint64_t bits) {
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    _Static_assert(sizeof(physical_device) <= sizeof(bits),
                   "VkPhysicalDevice exceeds bridge handle width");
    memcpy(&physical_device, &bits, sizeof(physical_device));
    return physical_device;
}

static VkDevice device_from_bits(uint64_t bits) {
    VkDevice device = VK_NULL_HANDLE;
    _Static_assert(sizeof(device) <= sizeof(bits),
                   "VkDevice exceeds bridge handle width");
    memcpy(&device, &bits, sizeof(device));
    return device;
}

static int resolve_physical_device(
    const struct bvb_vulkan_global_context *context,
    uint64_t physical_device_id, VkInstance *instance,
    VkPhysicalDevice *physical_device) {
    if (context == NULL || instance == NULL || physical_device == NULL) {
        return -EINVAL;
    }
    uint64_t parent_id = 0U;
    uint64_t physical_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, physical_device_id, BVB_OBJECT_PHYSICAL_DEVICE,
        &parent_id, &physical_bits);
    uint64_t instance_bits = 0U;
    if (result == 0) {
        result = bvb_handle_table_lookup(
            &context->objects, parent_id, BVB_OBJECT_INSTANCE, NULL,
            &instance_bits);
    }
    if (result == 0) {
        *instance = instance_from_bits(instance_bits);
        *physical_device = physical_device_from_bits(physical_bits);
    }
    return result;
}

int bvb_vulkan_global_context_create(
    const char *loader_path, struct bvb_vulkan_global_context **output,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (loader_path == NULL || loader_path[0] != '/' || output == NULL) {
        set_error(error, error_size, "loader path must be absolute");
        return -EINVAL;
    }
    *output = NULL;
    struct bvb_vulkan_global_context *context = calloc(1, sizeof(*context));
    if (context == NULL) {
        set_error(error, error_size, "could not allocate global context");
        return -ENOMEM;
    }
    context->loader = dlopen(loader_path, RTLD_NOW | RTLD_LOCAL);
    if (context->loader == NULL) {
        set_error(error, error_size, "could not load %s: %s", loader_path,
                  dlerror());
        free(context);
        return -ENOENT;
    }
    context->get_instance_proc_addr =
        (PFN_vkGetInstanceProcAddr)symbol_from_loader(
            context->loader, "vkGetInstanceProcAddr");
    if (context->get_instance_proc_addr == NULL) {
        set_error(error, error_size, "loader has no vkGetInstanceProcAddr");
        bvb_vulkan_global_context_destroy(context);
        return -ENOSYS;
    }
    context->get_device_proc_addr =
        (PFN_vkGetDeviceProcAddr)symbol_from_loader(
            context->loader, "vkGetDeviceProcAddr");
    if (context->get_device_proc_addr == NULL) {
        set_error(error, error_size, "loader has no vkGetDeviceProcAddr");
        bvb_vulkan_global_context_destroy(context);
        return -ENOSYS;
    }
    PFN_vkEnumerateInstanceVersion enumerate_version =
        (PFN_vkEnumerateInstanceVersion)context->get_instance_proc_addr(
            VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    PFN_vkEnumerateInstanceExtensionProperties enumerate_extensions =
        (PFN_vkEnumerateInstanceExtensionProperties)
            context->get_instance_proc_addr(
            VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    PFN_vkEnumerateInstanceLayerProperties enumerate_layers =
        (PFN_vkEnumerateInstanceLayerProperties)context->get_instance_proc_addr(
            VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
    context->create_instance = (PFN_vkCreateInstance)
        context->get_instance_proc_addr(
        VK_NULL_HANDLE, "vkCreateInstance");
    if (enumerate_extensions == NULL || enumerate_layers == NULL ||
        context->create_instance == NULL) {
        set_error(error, error_size, "loader lacks required global functions");
        bvb_vulkan_global_context_destroy(context);
        return -ENOSYS;
    }

    context->info.loader_api_version = VK_API_VERSION_1_0;
    VkResult result = VK_SUCCESS;
    if (enumerate_version != NULL) {
        result = enumerate_version(&context->info.loader_api_version);
    }
    if (result == VK_SUCCESS) {
        result = enumerate_extensions(
            NULL, &context->info.native_extension_count, NULL);
    }
    if (result == VK_SUCCESS) {
        result = enumerate_layers(&context->info.native_layer_count, NULL);
    }
    if (result != VK_SUCCESS || context->info.loader_api_version == 0U) {
        set_error(error, error_size,
                  "global Vulkan enumeration failed: %d", (int)result);
        bvb_vulkan_global_context_destroy(context);
        return -EIO;
    }
    context->info.exposed_extension_count = 0U;
    context->info.exposed_layer_count = 0U;
    int status = bvb_handle_table_init(
        &context->objects, context->object_entries,
        BVB_GLOBAL_OBJECT_CAPACITY);
    if (status != 0) {
        set_error(error, error_size, "instance table init failed: %d", status);
        bvb_vulkan_global_context_destroy(context);
        return status;
    }
    context->next_instance_serial = 1U;
    context->next_physical_device_serial = 1U;
    context->next_device_serial = 1U;
    context->next_queue_serial = 1U;
    *output = context;
    return 0;
}

void bvb_vulkan_global_context_destroy(
    struct bvb_vulkan_global_context *context) {
    if (context == NULL) {
        return;
    }
    if (context->get_device_proc_addr != NULL) {
        for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
            const struct bvb_handle_entry *entry =
                &context->object_entries[index];
            if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_DEVICE &&
                entry->native_bits != 0U) {
                const VkDevice device = device_from_bits(entry->native_bits);
                PFN_vkDestroyDevice destroy_device =
                    (PFN_vkDestroyDevice)context->get_device_proc_addr(
                        device, "vkDestroyDevice");
                if (destroy_device != NULL) {
                    destroy_device(device, NULL);
                }
            }
        }
    }
    if (context->destroy_instance != NULL) {
        for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
            const struct bvb_handle_entry *entry =
                &context->object_entries[index];
            if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_INSTANCE &&
                entry->native_bits != 0U) {
                context->destroy_instance(
                    instance_from_bits(entry->native_bits), NULL);
            }
        }
    }
    if (context->loader != NULL) {
        (void)dlclose(context->loader);
    }
    free(context);
}

int bvb_vulkan_global_context_info(
    const struct bvb_vulkan_global_context *context,
    struct bvb_vulkan_global_info *info) {
    if (context == NULL || info == NULL) {
        return -EINVAL;
    }
    *info = context->info;
    return 0;
}

int bvb_vulkan_global_context_create_instance(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_instance_create_request *request,
    struct bvb_vulkan_instance_create_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (context == NULL || request == NULL || response == NULL) {
        return -EINVAL;
    }
    *response = (struct bvb_vulkan_instance_create_response){0};
    if (request->flags != 0U) {
        response->vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
        return 0;
    }
    if (request->enabled_layer_count != 0U) {
        response->vulkan_result = VK_ERROR_LAYER_NOT_PRESENT;
        return 0;
    }
    if (request->enabled_extension_count != 0U) {
        response->vulkan_result = VK_ERROR_EXTENSION_NOT_PRESENT;
        return 0;
    }
    if (context->objects.count == context->objects.capacity) {
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    const VkApplicationInfo application_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "bionic-vulkan-bridge",
        .applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .pEngineName = "bvb-global-dispatch",
        .engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .apiVersion = request->api_version,
    };
    const VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application_info,
    };
    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = context->create_instance(&create_info, NULL, &instance);
    if (result != VK_SUCCESS) {
        response->vulkan_result = result;
        return 0;
    }
    if (context->destroy_instance == NULL) {
        context->destroy_instance =
            (PFN_vkDestroyInstance)context->get_instance_proc_addr(
                instance, "vkDestroyInstance");
        if (context->destroy_instance == NULL) {
            set_error(error, error_size,
                      "created instance has no vkDestroyInstance");
            response->vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
            return 0;
        }
    }
    const uint64_t native_bits = handle_bits(&instance, sizeof(instance));
    const uint64_t wire_id = bvb_handle_id(
        BVB_OBJECT_INSTANCE, context->next_instance_serial++);
    int status = bvb_handle_table_insert(
        &context->objects, wire_id, 0U, native_bits);
    if (status != 0) {
        context->destroy_instance(instance, NULL);
        set_error(error, error_size, "instance ownership failed: %d", status);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    response->vulkan_result = VK_SUCCESS;
    response->instance_id = wire_id;
    return 0;
}

int bvb_vulkan_global_context_destroy_instance(
    struct bvb_vulkan_global_context *context, uint64_t instance_id) {
    if (context == NULL || context->destroy_instance == NULL) {
        return -EINVAL;
    }
    uint64_t native_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, instance_id, BVB_OBJECT_INSTANCE, NULL,
        &native_bits);
    if (result != 0) {
        return result;
    }
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) != BVB_OBJECT_DEVICE) {
            continue;
        }
        uint64_t physical_bits = 0U;
        uint64_t physical_parent = 0U;
        result = bvb_handle_table_lookup(
            &context->objects, entry->parent_id, BVB_OBJECT_PHYSICAL_DEVICE,
            &physical_parent, &physical_bits);
        if (result == 0 && physical_parent == instance_id) {
            result = bvb_vulkan_global_context_destroy_device(
                context, entry->wire_id);
            if (result != 0) {
                return result;
            }
        }
    }
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_PHYSICAL_DEVICE &&
            entry->parent_id == instance_id) {
            const uint64_t child_id = entry->wire_id;
            result = bvb_handle_table_remove(
                &context->objects, child_id, BVB_OBJECT_PHYSICAL_DEVICE, NULL);
            if (result != 0) {
                return result;
            }
        }
    }
    result = bvb_handle_table_remove(
        &context->objects, instance_id, BVB_OBJECT_INSTANCE, &native_bits);
    if (result != 0) {
        return result;
    }
    context->destroy_instance(instance_from_bits(native_bits), NULL);
    return 0;
}

static uint64_t existing_physical_device_id(
    const struct bvb_vulkan_global_context *context, uint64_t instance_id,
    uint64_t native_bits) {
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_PHYSICAL_DEVICE &&
            entry->parent_id == instance_id &&
            entry->native_bits == native_bits) {
            return entry->wire_id;
        }
    }
    return 0U;
}

int bvb_vulkan_global_context_enumerate_physical_devices(
    struct bvb_vulkan_global_context *context, uint64_t instance_id,
    struct bvb_vulkan_physical_devices *devices,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (context == NULL || devices == NULL) {
        return -EINVAL;
    }
    *devices = (struct bvb_vulkan_physical_devices){0};
    uint64_t instance_bits = 0U;
    int status = bvb_handle_table_lookup(
        &context->objects, instance_id, BVB_OBJECT_INSTANCE, NULL,
        &instance_bits);
    if (status != 0) {
        return status;
    }
    const VkInstance instance = instance_from_bits(instance_bits);
    PFN_vkEnumeratePhysicalDevices enumerate_devices =
        (PFN_vkEnumeratePhysicalDevices)context->get_instance_proc_addr(
            instance, "vkEnumeratePhysicalDevices");
    if (enumerate_devices == NULL) {
        set_error(error, error_size,
                  "created instance has no vkEnumeratePhysicalDevices");
        return -ENOSYS;
    }
    uint32_t count = 0U;
    VkResult result = enumerate_devices(instance, &count, NULL);
    if (result != VK_SUCCESS) {
        devices->vulkan_result = result;
        return 0;
    }
    if (count > BVB_VULKAN_MAX_PHYSICAL_DEVICES) {
        set_error(error, error_size,
                  "physical-device count exceeds bridge bound: %u", count);
        return -EOVERFLOW;
    }
    VkPhysicalDevice native_devices[BVB_VULKAN_MAX_PHYSICAL_DEVICES] = {0};
    uint32_t returned = count;
    if (count != 0U) {
        result = enumerate_devices(instance, &returned, native_devices);
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            devices->vulkan_result = result;
            return 0;
        }
        if (returned > count) {
            return -EPROTO;
        }
    }
    devices->vulkan_result = result;
    devices->count = returned;
    for (uint32_t index = 0U; index < returned; ++index) {
        const uint64_t native_bits =
            handle_bits(&native_devices[index], sizeof(native_devices[index]));
        uint64_t wire_id = existing_physical_device_id(
            context, instance_id, native_bits);
        if (wire_id == 0U) {
            wire_id = bvb_handle_id(
                BVB_OBJECT_PHYSICAL_DEVICE,
                context->next_physical_device_serial++);
            status = bvb_handle_table_insert(
                &context->objects, wire_id, instance_id, native_bits);
            if (status != 0) {
                set_error(error, error_size,
                          "physical-device ownership failed: %d", status);
                return status;
            }
        }
        devices->ids[index] = wire_id;
    }
    return 0;
}

int bvb_vulkan_global_context_get_physical_device_properties(
    const struct bvb_vulkan_global_context *context,
    uint64_t physical_device_id, VkPhysicalDeviceProperties *properties,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (properties == NULL) {
        return -EINVAL;
    }
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    int result = resolve_physical_device(
        context, physical_device_id, &instance, &physical_device);
    if (result != 0) {
        set_error(error, error_size, "unknown physical-device handle");
        return result;
    }
    PFN_vkGetPhysicalDeviceProperties get_properties =
        (PFN_vkGetPhysicalDeviceProperties)context->get_instance_proc_addr(
            instance, "vkGetPhysicalDeviceProperties");
    if (get_properties == NULL) {
        set_error(error, error_size,
                  "instance has no vkGetPhysicalDeviceProperties");
        return -ENOSYS;
    }
    memset(properties, 0, sizeof(*properties));
    get_properties(physical_device, properties);
    if (memchr(properties->deviceName, '\0',
               sizeof(properties->deviceName)) == NULL) {
        set_error(error, error_size, "physical-device name is unterminated");
        return -EPROTO;
    }
    return 0;
}

int bvb_vulkan_global_context_get_queue_family_properties(
    const struct bvb_vulkan_global_context *context,
    uint64_t physical_device_id,
    VkQueueFamilyProperties properties[BVB_VULKAN_MAX_QUEUE_FAMILIES],
    uint32_t *count, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (properties == NULL || count == NULL) {
        return -EINVAL;
    }
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    int result = resolve_physical_device(
        context, physical_device_id, &instance, &physical_device);
    if (result != 0) {
        set_error(error, error_size, "unknown physical-device handle");
        return result;
    }
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_properties =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
            context->get_instance_proc_addr(
                instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (get_properties == NULL) {
        set_error(error, error_size,
                  "instance has no vkGetPhysicalDeviceQueueFamilyProperties");
        return -ENOSYS;
    }
    uint32_t available = 0U;
    get_properties(physical_device, &available, NULL);
    if (available > BVB_VULKAN_MAX_QUEUE_FAMILIES) {
        set_error(error, error_size,
                  "queue-family count exceeds bridge bound: %u", available);
        return -EOVERFLOW;
    }
    memset(properties, 0, sizeof(*properties) * BVB_VULKAN_MAX_QUEUE_FAMILIES);
    uint32_t returned = available;
    if (available != 0U) {
        get_properties(physical_device, &returned, properties);
    }
    if (returned > available) {
        set_error(error, error_size, "queue-family count changed unexpectedly");
        return -EPROTO;
    }
    *count = returned;
    return 0;
}

int bvb_vulkan_global_context_get_memory_properties(
    const struct bvb_vulkan_global_context *context,
    uint64_t physical_device_id,
    VkPhysicalDeviceMemoryProperties *properties,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (properties == NULL) {
        return -EINVAL;
    }
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    int result = resolve_physical_device(
        context, physical_device_id, &instance, &physical_device);
    if (result != 0) {
        set_error(error, error_size, "unknown physical-device handle");
        return result;
    }
    PFN_vkGetPhysicalDeviceMemoryProperties get_properties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)
            context->get_instance_proc_addr(
                instance, "vkGetPhysicalDeviceMemoryProperties");
    if (get_properties == NULL) {
        set_error(error, error_size,
                  "instance has no vkGetPhysicalDeviceMemoryProperties");
        return -ENOSYS;
    }
    memset(properties, 0, sizeof(*properties));
    get_properties(physical_device, properties);
    if (properties->memoryTypeCount > VK_MAX_MEMORY_TYPES ||
        properties->memoryHeapCount > VK_MAX_MEMORY_HEAPS) {
        set_error(error, error_size, "invalid physical-device memory counts");
        return -EPROTO;
    }
    return 0;
}

int bvb_vulkan_global_context_enumerate_device_extensions(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_device_extension_query *query,
    struct bvb_vulkan_extension_page *page,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (query == NULL || page == NULL ||
        query->max_count > BVB_VULKAN_EXTENSION_PAGE_CAPACITY) {
        return -EINVAL;
    }
    *page = (struct bvb_vulkan_extension_page){0};
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    int status = resolve_physical_device(
        context, query->physical_device_id, &instance, &physical_device);
    if (status != 0) {
        set_error(error, error_size, "unknown physical-device handle");
        return status;
    }
    PFN_vkEnumerateDeviceExtensionProperties enumerate =
        (PFN_vkEnumerateDeviceExtensionProperties)
            context->get_instance_proc_addr(
                instance, "vkEnumerateDeviceExtensionProperties");
    if (enumerate == NULL) {
        set_error(error, error_size,
                  "instance has no vkEnumerateDeviceExtensionProperties");
        return -ENOSYS;
    }
    uint32_t available = 0U;
    VkResult result = enumerate(physical_device, NULL, &available, NULL);
    page->vulkan_result = result;
    page->total_count = available;
    page->first = query->first;
    if (result != VK_SUCCESS || query->max_count == 0U) {
        return 0;
    }
    if (available > BVB_VULKAN_MAX_DEVICE_EXTENSIONS) {
        set_error(error, error_size,
                  "device-extension count exceeds bridge bound: %u",
                  available);
        return -EOVERFLOW;
    }
    if (query->first > available) {
        set_error(error, error_size,
                  "device-extension page starts beyond available records");
        return -ERANGE;
    }
    VkExtensionProperties *all = NULL;
    if (available != 0U) {
        all = calloc(available, sizeof(*all));
        if (all == NULL) {
            return -ENOMEM;
        }
    }
    uint32_t returned = available;
    if (available != 0U) {
        result = enumerate(physical_device, NULL, &returned, all);
    }
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) ||
        returned > available || query->first > returned) {
        free(all);
        page->vulkan_result = result;
        set_error(error, error_size,
                  "device-extension list changed unexpectedly: %d",
                  (int)result);
        return result == VK_SUCCESS ? -EPROTO : 0;
    }
    page->vulkan_result = result;
    page->total_count = returned;
    const uint32_t remaining = returned - query->first;
    page->count = query->max_count < remaining ? query->max_count : remaining;
    if (page->count != 0U) {
        memcpy(page->properties, all + query->first,
               page->count * sizeof(*page->properties));
    }
    free(all);
    return 0;
}

int bvb_vulkan_global_context_get_physical_device_features(
    const struct bvb_vulkan_global_context *context,
    uint64_t physical_device_id, VkPhysicalDeviceFeatures *features,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (features == NULL) {
        return -EINVAL;
    }
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    int result = resolve_physical_device(
        context, physical_device_id, &instance, &physical_device);
    if (result != 0) {
        set_error(error, error_size, "unknown physical-device handle");
        return result;
    }
    PFN_vkGetPhysicalDeviceFeatures get_features =
        (PFN_vkGetPhysicalDeviceFeatures)context->get_instance_proc_addr(
            instance, "vkGetPhysicalDeviceFeatures");
    if (get_features == NULL) {
        set_error(error, error_size,
                  "instance has no vkGetPhysicalDeviceFeatures");
        return -ENOSYS;
    }
    memset(features, 0, sizeof(*features));
    get_features(physical_device, features);
    return 0;
}

static struct bvb_device_metadata *device_metadata_slot(
    struct bvb_vulkan_global_context *context, uint64_t device_id) {
    struct bvb_device_metadata *empty = NULL;
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        struct bvb_device_metadata *metadata = &context->device_metadata[index];
        if (metadata->device_id == device_id) {
            return metadata;
        }
        if (metadata->device_id == 0U && empty == NULL) {
            empty = metadata;
        }
    }
    return empty;
}

int bvb_vulkan_global_context_create_device(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_device_create_request *request,
    struct bvb_vulkan_device_create_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (context == NULL || request == NULL || response == NULL) {
        return -EINVAL;
    }
    *response = (struct bvb_vulkan_device_create_response){0};
    float queue_priority = 0.0F;
    memcpy(&queue_priority, &request->queue_priority_bits,
           sizeof(queue_priority));
    if (request->flags != 0U || request->queue_count != 1U ||
        request->enabled_layer_count != 0U ||
        request->enabled_extension_count != 0U ||
        !(queue_priority >= 0.0F && queue_priority <= 1.0F)) {
        response->vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
        return 0;
    }
    if (context->objects.count == context->objects.capacity) {
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    int status = resolve_physical_device(
        context, request->physical_device_id, &instance, &physical_device);
    if (status != 0) {
        return status;
    }
    VkQueueFamilyProperties queue_properties[BVB_VULKAN_MAX_QUEUE_FAMILIES];
    uint32_t queue_family_count = 0U;
    status = bvb_vulkan_global_context_get_queue_family_properties(
        context, request->physical_device_id, queue_properties,
        &queue_family_count, error, error_size);
    if (status != 0) {
        return status;
    }
    if (request->queue_family_index >= queue_family_count ||
        queue_properties[request->queue_family_index].queueCount <
            request->queue_count) {
        response->vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
        return 0;
    }
    PFN_vkCreateDevice create_device =
        (PFN_vkCreateDevice)context->get_instance_proc_addr(
            instance, "vkCreateDevice");
    if (create_device == NULL) {
        set_error(error, error_size, "instance has no vkCreateDevice");
        return -ENOSYS;
    }
    const VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = request->queue_family_index,
        .queueCount = 1U,
        .pQueuePriorities = &queue_priority,
    };
    const VkDeviceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1U,
        .pQueueCreateInfos = &queue_info,
    };
    VkDevice device = VK_NULL_HANDLE;
    VkResult result = create_device(
        physical_device, &create_info, NULL, &device);
    response->vulkan_result = result;
    if (result != VK_SUCCESS) {
        return 0;
    }
    PFN_vkDestroyDevice destroy_device =
        (PFN_vkDestroyDevice)context->get_device_proc_addr(
            device, "vkDestroyDevice");
    if (destroy_device == NULL) {
        set_error(error, error_size, "created device has no vkDestroyDevice");
        response->vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
        return 0;
    }
    const uint64_t native_bits = handle_bits(&device, sizeof(device));
    const uint64_t wire_id = bvb_handle_id(
        BVB_OBJECT_DEVICE, context->next_device_serial++);
    struct bvb_device_metadata *metadata = device_metadata_slot(context, 0U);
    if (metadata == NULL) {
        destroy_device(device, NULL);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    status = bvb_handle_table_insert(
        &context->objects, wire_id, request->physical_device_id, native_bits);
    if (status != 0) {
        destroy_device(device, NULL);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    *metadata = (struct bvb_device_metadata){
        .device_id = wire_id,
        .queue_family_index = request->queue_family_index,
        .queue_count = request->queue_count,
    };
    response->device_id = wire_id;
    return 0;
}

int bvb_vulkan_global_context_destroy_device(
    struct bvb_vulkan_global_context *context, uint64_t device_id) {
    if (context == NULL || context->get_device_proc_addr == NULL) {
        return -EINVAL;
    }
    uint64_t device_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, device_id, BVB_OBJECT_DEVICE, NULL, &device_bits);
    if (result != 0) {
        return result;
    }
    const VkDevice device = device_from_bits(device_bits);
    PFN_vkDestroyDevice destroy_device =
        (PFN_vkDestroyDevice)context->get_device_proc_addr(
            device, "vkDestroyDevice");
    if (destroy_device == NULL) {
        return -ENOSYS;
    }
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_QUEUE &&
            entry->parent_id == device_id) {
            result = bvb_handle_table_remove(
                &context->objects, entry->wire_id, BVB_OBJECT_QUEUE, NULL);
            if (result != 0) {
                return result;
            }
        }
    }
    result = bvb_handle_table_remove(
        &context->objects, device_id, BVB_OBJECT_DEVICE, &device_bits);
    if (result != 0) {
        return result;
    }
    struct bvb_device_metadata *metadata =
        device_metadata_slot(context, device_id);
    if (metadata != NULL && metadata->device_id == device_id) {
        *metadata = (struct bvb_device_metadata){0};
    }
    destroy_device(device, NULL);
    return 0;
}

static uint64_t existing_queue_id(
    const struct bvb_vulkan_global_context *context, uint64_t device_id,
    uint64_t native_bits) {
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_QUEUE &&
            entry->parent_id == device_id && entry->native_bits == native_bits) {
            return entry->wire_id;
        }
    }
    return 0U;
}

int bvb_vulkan_global_context_get_device_queue(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_device_queue_request *request,
    uint64_t *queue_id, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (context == NULL || request == NULL || queue_id == NULL) {
        return -EINVAL;
    }
    *queue_id = 0U;
    uint64_t device_bits = 0U;
    int status = bvb_handle_table_lookup(
        &context->objects, request->device_id, BVB_OBJECT_DEVICE, NULL,
        &device_bits);
    struct bvb_device_metadata *metadata =
        device_metadata_slot(context, request->device_id);
    if (status != 0 || metadata == NULL ||
        metadata->device_id != request->device_id) {
        set_error(error, error_size, "unknown device handle");
        return status != 0 ? status : -ENOENT;
    }
    if (request->queue_family_index != metadata->queue_family_index ||
        request->queue_index >= metadata->queue_count) {
        set_error(error, error_size, "queue index was not created");
        return -ERANGE;
    }
    const VkDevice device = device_from_bits(device_bits);
    PFN_vkGetDeviceQueue get_queue =
        (PFN_vkGetDeviceQueue)context->get_device_proc_addr(
            device, "vkGetDeviceQueue");
    if (get_queue == NULL) {
        set_error(error, error_size, "device has no vkGetDeviceQueue");
        return -ENOSYS;
    }
    VkQueue queue = VK_NULL_HANDLE;
    get_queue(device, request->queue_family_index, request->queue_index, &queue);
    if (queue == VK_NULL_HANDLE) {
        set_error(error, error_size, "vkGetDeviceQueue returned null");
        return -EIO;
    }
    const uint64_t native_bits = handle_bits(&queue, sizeof(queue));
    uint64_t wire_id = existing_queue_id(
        context, request->device_id, native_bits);
    if (wire_id == 0U) {
        wire_id = bvb_handle_id(
            BVB_OBJECT_QUEUE, context->next_queue_serial++);
        status = bvb_handle_table_insert(
            &context->objects, wire_id, request->device_id, native_bits);
        if (status != 0) {
            return status;
        }
    }
    *queue_id = wire_id;
    return 0;
}

static int resolve_queue(
    const struct bvb_vulkan_global_context *context, uint64_t queue_id,
    VkDevice *device, VkQueue *queue) {
    if (context == NULL || device == NULL || queue == NULL) {
        return -EINVAL;
    }
    uint64_t device_id = 0U;
    uint64_t queue_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, queue_id, BVB_OBJECT_QUEUE, &device_id, &queue_bits);
    uint64_t device_bits = 0U;
    if (result == 0) {
        result = bvb_handle_table_lookup(
            &context->objects, device_id, BVB_OBJECT_DEVICE, NULL,
            &device_bits);
    }
    if (result == 0) {
        *device = device_from_bits(device_bits);
        *queue = VK_NULL_HANDLE;
        _Static_assert(sizeof(*queue) <= sizeof(queue_bits),
                       "VkQueue exceeds bridge handle width");
        memcpy(queue, &queue_bits, sizeof(*queue));
    }
    return result;
}

int bvb_vulkan_global_context_queue_submit_empty(
    const struct bvb_vulkan_global_context *context, uint64_t queue_id,
    int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (vulkan_result == NULL) {
        return -EINVAL;
    }
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    int result = resolve_queue(context, queue_id, &device, &queue);
    if (result != 0) {
        set_error(error, error_size, "unknown queue handle");
        return result;
    }
    PFN_vkQueueSubmit submit =
        (PFN_vkQueueSubmit)context->get_device_proc_addr(
            device, "vkQueueSubmit");
    if (submit == NULL) {
        set_error(error, error_size, "device has no vkQueueSubmit");
        return -ENOSYS;
    }
    *vulkan_result = submit(queue, 0U, NULL, VK_NULL_HANDLE);
    return 0;
}

int bvb_vulkan_global_context_queue_wait_idle(
    const struct bvb_vulkan_global_context *context, uint64_t queue_id,
    int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (vulkan_result == NULL) {
        return -EINVAL;
    }
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    int result = resolve_queue(context, queue_id, &device, &queue);
    if (result != 0) {
        set_error(error, error_size, "unknown queue handle");
        return result;
    }
    PFN_vkQueueWaitIdle wait_idle =
        (PFN_vkQueueWaitIdle)context->get_device_proc_addr(
            device, "vkQueueWaitIdle");
    if (wait_idle == NULL) {
        set_error(error, error_size, "device has no vkQueueWaitIdle");
        return -ENOSYS;
    }
    *vulkan_result = wait_idle(queue);
    return 0;
}

int bvb_vulkan_global_context_device_wait_idle(
    const struct bvb_vulkan_global_context *context, uint64_t device_id,
    int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (context == NULL || vulkan_result == NULL) {
        return -EINVAL;
    }
    uint64_t device_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, device_id, BVB_OBJECT_DEVICE, NULL, &device_bits);
    if (result != 0) {
        set_error(error, error_size, "unknown device handle");
        return result;
    }
    const VkDevice device = device_from_bits(device_bits);
    PFN_vkDeviceWaitIdle wait_idle =
        (PFN_vkDeviceWaitIdle)context->get_device_proc_addr(
            device, "vkDeviceWaitIdle");
    if (wait_idle == NULL) {
        set_error(error, error_size, "device has no vkDeviceWaitIdle");
        return -ENOSYS;
    }
    *vulkan_result = wait_idle(device);
    return 0;
}
