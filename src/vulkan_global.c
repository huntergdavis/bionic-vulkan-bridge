#define VK_NO_PROTOTYPES

#include <bvb/handle.h>
#include <bvb/vulkan_global.h>

#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    BVB_GLOBAL_OBJECT_CAPACITY = 64,
    BVB_EXPOSED_INSTANCE_EXTENSION_CAPACITY = 3,
};

static const char *const bvb_instance_extension_allowlist[
    BVB_EXPOSED_INSTANCE_EXTENSION_CAPACITY] = {
    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
};

struct bvb_device_metadata {
    uint64_t device_id;
    uint32_t queue_family_index;
    uint32_t queue_count;
};

struct bvb_memory_metadata {
    uint64_t memory_id;
    uint64_t allocation_size;
    uint32_t property_flags;
};

struct bvb_vulkan_global_context {
    void *loader;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    PFN_vkCreateInstance create_instance;
    PFN_vkDestroyInstance destroy_instance;
    PFN_vkGetDeviceProcAddr get_device_proc_addr;
    struct bvb_vulkan_global_info info;
    VkExtensionProperties exposed_instance_extensions[
        BVB_EXPOSED_INSTANCE_EXTENSION_CAPACITY];
    struct bvb_handle_entry object_entries[BVB_GLOBAL_OBJECT_CAPACITY];
    struct bvb_handle_table objects;
    uint64_t next_instance_serial;
    uint64_t next_physical_device_serial;
    uint64_t next_device_serial;
    uint64_t next_queue_serial;
    uint64_t next_command_pool_serial;
    uint64_t next_command_buffer_serial;
    uint64_t next_buffer_serial;
    uint64_t next_memory_serial;
    uint64_t next_fence_serial;
    struct bvb_device_metadata device_metadata[BVB_GLOBAL_OBJECT_CAPACITY];
    struct bvb_memory_metadata memory_metadata[BVB_GLOBAL_OBJECT_CAPACITY];
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

static VkCommandPool command_pool_from_bits(uint64_t bits) {
    VkCommandPool command_pool = VK_NULL_HANDLE;
    _Static_assert(sizeof(command_pool) <= sizeof(bits),
                   "VkCommandPool exceeds bridge handle width");
    memcpy(&command_pool, &bits, sizeof(command_pool));
    return command_pool;
}

static VkCommandBuffer command_buffer_from_bits(uint64_t bits) {
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    _Static_assert(sizeof(command_buffer) <= sizeof(bits),
                   "VkCommandBuffer exceeds bridge handle width");
    memcpy(&command_buffer, &bits, sizeof(command_buffer));
    return command_buffer;
}

static VkBuffer buffer_from_bits(uint64_t bits) {
    VkBuffer buffer = VK_NULL_HANDLE;
    _Static_assert(sizeof(buffer) <= sizeof(bits),
                   "VkBuffer exceeds bridge handle width");
    memcpy(&buffer, &bits, sizeof(buffer));
    return buffer;
}

static VkDeviceMemory memory_from_bits(uint64_t bits) {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    _Static_assert(sizeof(memory) <= sizeof(bits),
                   "VkDeviceMemory exceeds bridge handle width");
    memcpy(&memory, &bits, sizeof(memory));
    return memory;
}

static VkFence fence_from_bits(uint64_t bits) {
    VkFence fence = VK_NULL_HANDLE;
    _Static_assert(sizeof(fence) <= sizeof(bits),
                   "VkFence exceeds bridge handle width");
    memcpy(&fence, &bits, sizeof(fence));
    return fence;
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
    if (result == VK_SUCCESS && context->info.native_extension_count != 0U) {
        const uint32_t available = context->info.native_extension_count;
        VkExtensionProperties *extensions =
            calloc(available, sizeof(*extensions));
        if (extensions == NULL) {
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
        } else {
            uint32_t returned = available;
            result = enumerate_extensions(NULL, &returned, extensions);
            if (result == VK_SUCCESS) {
                for (uint32_t allowed = 0U;
                     allowed < BVB_EXPOSED_INSTANCE_EXTENSION_CAPACITY;
                     ++allowed) {
                    for (uint32_t index = 0U; index < returned; ++index) {
                        if (strcmp(
                                extensions[index].extensionName,
                                bvb_instance_extension_allowlist[allowed]) ==
                            0) {
                            context->exposed_instance_extensions[
                                context->info.exposed_extension_count++] =
                                extensions[index];
                            break;
                        }
                    }
                }
            }
            free(extensions);
        }
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
    context->next_command_pool_serial = 1U;
    context->next_command_buffer_serial = 1U;
    context->next_buffer_serial = 1U;
    context->next_memory_serial = 1U;
    context->next_fence_serial = 1U;
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

int bvb_vulkan_global_context_enumerate_instance_extensions(
    const struct bvb_vulkan_global_context *context,
    struct bvb_vulkan_extension_page *page) {
    if (context == NULL || page == NULL) {
        return -EINVAL;
    }
    *page = (struct bvb_vulkan_extension_page){
        .vulkan_result = VK_SUCCESS,
        .total_count = context->info.exposed_extension_count,
        .count = context->info.exposed_extension_count,
    };
    for (uint32_t index = 0U; index < page->count; ++index) {
        page->properties[index] =
            context->exposed_instance_extensions[index];
    }
    return 0;
}

int bvb_vulkan_global_context_create_instance(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_instance_create_request *request,
    const char *const *enabled_extensions,
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
    if (request->enabled_extension_count >
            BVB_EXPOSED_INSTANCE_EXTENSION_CAPACITY ||
        (request->enabled_extension_count != 0U &&
         enabled_extensions == NULL)) {
        response->vulkan_result = VK_ERROR_EXTENSION_NOT_PRESENT;
        return 0;
    }
    for (uint32_t index = 0U;
         index < request->enabled_extension_count; ++index) {
        bool supported = false;
        for (uint32_t exposed = 0U;
             exposed < context->info.exposed_extension_count; ++exposed) {
            if (strcmp(
                    enabled_extensions[index],
                    context->exposed_instance_extensions[exposed]
                        .extensionName) == 0) {
                supported = true;
                break;
            }
        }
        if (!supported) {
            response->vulkan_result = VK_ERROR_EXTENSION_NOT_PRESENT;
            return 0;
        }
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
        .enabledExtensionCount = request->enabled_extension_count,
        .ppEnabledExtensionNames = enabled_extensions,
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

int bvb_vulkan_global_context_get_format_properties(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_format_query *query,
    struct bvb_vulkan_format_properties *properties,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (query == NULL || properties == NULL) {
        return -EINVAL;
    }
    *properties = (struct bvb_vulkan_format_properties){0};
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    int result = resolve_physical_device(
        context, query->physical_device_id, &instance, &physical_device);
    if (result != 0) {
        set_error(error, error_size, "unknown physical-device handle");
        return result;
    }
    PFN_vkGetPhysicalDeviceFormatProperties get_properties =
        (PFN_vkGetPhysicalDeviceFormatProperties)
            context->get_instance_proc_addr(
                instance, "vkGetPhysicalDeviceFormatProperties");
    if (get_properties == NULL) {
        set_error(error, error_size,
                  "instance has no vkGetPhysicalDeviceFormatProperties");
        return -ENOSYS;
    }
    VkFormatProperties native = {0};
    get_properties(physical_device, (VkFormat)query->format, &native);
    properties->linear_tiling_features = native.linearTilingFeatures;
    properties->optimal_tiling_features = native.optimalTilingFeatures;
    properties->buffer_features = native.bufferFeatures;
    return 0;
}

int bvb_vulkan_global_context_get_image_format_properties(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_image_format_query *query,
    struct bvb_vulkan_image_format_properties *properties,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (query == NULL || properties == NULL) {
        return -EINVAL;
    }
    *properties = (struct bvb_vulkan_image_format_properties){0};
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    int result = resolve_physical_device(
        context, query->physical_device_id, &instance, &physical_device);
    if (result != 0) {
        set_error(error, error_size, "unknown physical-device handle");
        return result;
    }
    PFN_vkGetPhysicalDeviceImageFormatProperties get_properties =
        (PFN_vkGetPhysicalDeviceImageFormatProperties)
            context->get_instance_proc_addr(
                instance, "vkGetPhysicalDeviceImageFormatProperties");
    if (get_properties == NULL) {
        set_error(
            error, error_size,
            "instance has no vkGetPhysicalDeviceImageFormatProperties");
        return -ENOSYS;
    }
    VkImageFormatProperties native = {0};
    const VkResult vulkan_result = get_properties(
        physical_device, (VkFormat)query->format, (VkImageType)query->type,
        (VkImageTiling)query->tiling, (VkImageUsageFlags)query->usage,
        (VkImageCreateFlags)query->flags, &native);
    properties->vulkan_result = vulkan_result;
    if (vulkan_result == VK_SUCCESS) {
        properties->max_extent_width = native.maxExtent.width;
        properties->max_extent_height = native.maxExtent.height;
        properties->max_extent_depth = native.maxExtent.depth;
        properties->max_mip_levels = native.maxMipLevels;
        properties->max_array_layers = native.maxArrayLayers;
        properties->sample_counts = native.sampleCounts;
        properties->max_resource_size = native.maxResourceSize;
    }
    return 0;
}

int bvb_vulkan_global_context_get_external_buffer_properties(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_external_buffer_query *query,
    struct bvb_vulkan_external_buffer_properties *properties,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (query == NULL || properties == NULL) {
        return -EINVAL;
    }
    *properties = (struct bvb_vulkan_external_buffer_properties){0};
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    int result = resolve_physical_device(
        context, query->physical_device_id, &instance, &physical_device);
    if (result != 0) {
        set_error(error, error_size, "unknown physical-device handle");
        return result;
    }
    PFN_vkGetPhysicalDeviceExternalBufferProperties get_properties =
        (PFN_vkGetPhysicalDeviceExternalBufferProperties)
            context->get_instance_proc_addr(
                instance, "vkGetPhysicalDeviceExternalBufferProperties");
    if (get_properties == NULL) {
        get_properties = (PFN_vkGetPhysicalDeviceExternalBufferProperties)
            context->get_instance_proc_addr(
                instance,
                "vkGetPhysicalDeviceExternalBufferPropertiesKHR");
    }
    if (get_properties == NULL) {
        set_error(error, error_size,
                  "instance has no external-buffer capability query");
        return -ENOSYS;
    }
    const VkPhysicalDeviceExternalBufferInfo info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .flags = (VkBufferCreateFlags)query->flags,
        .usage = (VkBufferUsageFlags)query->usage,
        .handleType = (VkExternalMemoryHandleTypeFlagBits)query->handle_type,
    };
    VkExternalBufferProperties native = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES,
    };
    get_properties(physical_device, &info, &native);
    properties->external_memory_features =
        native.externalMemoryProperties.externalMemoryFeatures;
    properties->export_from_imported_handle_types =
        native.externalMemoryProperties.exportFromImportedHandleTypes;
    properties->compatible_handle_types =
        native.externalMemoryProperties.compatibleHandleTypes;
    return 0;
}

int bvb_vulkan_global_context_get_external_semaphore_properties(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_external_semaphore_query *query,
    struct bvb_vulkan_external_semaphore_properties *properties,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (query == NULL || properties == NULL) {
        return -EINVAL;
    }
    *properties = (struct bvb_vulkan_external_semaphore_properties){0};
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    int result = resolve_physical_device(
        context, query->physical_device_id, &instance, &physical_device);
    if (result != 0) {
        set_error(error, error_size, "unknown physical-device handle");
        return result;
    }
    PFN_vkGetPhysicalDeviceExternalSemaphoreProperties get_properties =
        (PFN_vkGetPhysicalDeviceExternalSemaphoreProperties)
            context->get_instance_proc_addr(
                instance,
                "vkGetPhysicalDeviceExternalSemaphoreProperties");
    if (get_properties == NULL) {
        get_properties =
            (PFN_vkGetPhysicalDeviceExternalSemaphoreProperties)
                context->get_instance_proc_addr(
                    instance,
                    "vkGetPhysicalDeviceExternalSemaphorePropertiesKHR");
    }
    if (get_properties == NULL) {
        set_error(error, error_size,
                  "instance has no external-semaphore capability query");
        return -ENOSYS;
    }
    const VkPhysicalDeviceExternalSemaphoreInfo info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
        .handleType =
            (VkExternalSemaphoreHandleTypeFlagBits)query->handle_type,
    };
    VkExternalSemaphoreProperties native = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
    };
    get_properties(physical_device, &info, &native);
    properties->export_from_imported_handle_types =
        native.exportFromImportedHandleTypes;
    properties->compatible_handle_types = native.compatibleHandleTypes;
    properties->external_semaphore_features =
        native.externalSemaphoreFeatures;
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
    const char *const *enabled_extensions,
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
        (request->enabled_extension_count != 0U &&
         enabled_extensions == NULL) ||
        request->enabled_extension_count >
            BVB_VULKAN_MAX_ENABLED_EXTENSIONS ||
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
        .enabledExtensionCount = request->enabled_extension_count,
        .ppEnabledExtensionNames = enabled_extensions,
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
        if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_COMMAND_POOL &&
            entry->parent_id == device_id) {
            result = bvb_vulkan_global_context_destroy_command_pool(
                context, entry->wire_id, NULL, 0U);
            if (result != 0) {
                return result;
            }
        }
    }
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_BUFFER &&
            entry->parent_id == device_id) {
            result = bvb_vulkan_global_context_destroy_buffer(
                context, entry->wire_id, NULL, 0U);
            if (result != 0) return result;
        }
    }
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_DEVICE_MEMORY &&
            entry->parent_id == device_id) {
            result = bvb_vulkan_global_context_free_memory(
                context, entry->wire_id, NULL, 0U);
            if (result != 0) return result;
        }
    }
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_FENCE &&
            entry->parent_id == device_id) {
            result = bvb_vulkan_global_context_destroy_fence(
                context, entry->wire_id, NULL, 0U);
            if (result != 0) return result;
        }
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

static int resolve_command_pool(
    const struct bvb_vulkan_global_context *context, uint64_t command_pool_id,
    uint64_t *device_id, VkDevice *device, VkCommandPool *command_pool) {
    if (context == NULL || device_id == NULL || device == NULL ||
        command_pool == NULL) {
        return -EINVAL;
    }
    uint64_t pool_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, command_pool_id, BVB_OBJECT_COMMAND_POOL,
        device_id, &pool_bits);
    uint64_t device_bits = 0U;
    if (result == 0) {
        result = bvb_handle_table_lookup(
            &context->objects, *device_id, BVB_OBJECT_DEVICE, NULL,
            &device_bits);
    }
    if (result == 0) {
        *device = device_from_bits(device_bits);
        *command_pool = command_pool_from_bits(pool_bits);
    }
    return result;
}

static int resolve_command_buffer(
    const struct bvb_vulkan_global_context *context,
    uint64_t command_buffer_id, uint64_t *device_id,
    VkDevice *device, VkCommandPool *command_pool,
    VkCommandBuffer *command_buffer) {
    if (context == NULL || device_id == NULL || device == NULL ||
        command_pool == NULL || command_buffer == NULL) {
        return -EINVAL;
    }
    uint64_t command_pool_id = 0U;
    uint64_t command_buffer_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, command_buffer_id, BVB_OBJECT_COMMAND_BUFFER,
        &command_pool_id, &command_buffer_bits);
    if (result == 0) {
        result = resolve_command_pool(
            context, command_pool_id, device_id, device, command_pool);
    }
    if (result == 0) {
        *command_buffer = command_buffer_from_bits(command_buffer_bits);
    }
    return result;
}

int bvb_vulkan_global_context_create_command_pool(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_command_pool_create_request *request,
    struct bvb_vulkan_command_pool_create_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (context == NULL || request == NULL || response == NULL) {
        return -EINVAL;
    }
    *response = (struct bvb_vulkan_command_pool_create_response){0};
    const uint32_t supported_flags =
        VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if ((request->flags & ~supported_flags) != 0U) {
        response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return 0;
    }
    uint64_t device_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, request->device_id, BVB_OBJECT_DEVICE, NULL,
        &device_bits);
    struct bvb_device_metadata *metadata =
        device_metadata_slot(context, request->device_id);
    if (result != 0 || metadata == NULL ||
        metadata->device_id != request->device_id) {
        set_error(error, error_size, "unknown device handle");
        return result != 0 ? result : -ENOENT;
    }
    if (request->queue_family_index != metadata->queue_family_index) {
        response->vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
        return 0;
    }
    const VkDevice device = device_from_bits(device_bits);
    PFN_vkCreateCommandPool create_command_pool =
        (PFN_vkCreateCommandPool)context->get_device_proc_addr(
            device, "vkCreateCommandPool");
    PFN_vkDestroyCommandPool destroy_command_pool =
        (PFN_vkDestroyCommandPool)context->get_device_proc_addr(
            device, "vkDestroyCommandPool");
    if (create_command_pool == NULL || destroy_command_pool == NULL) {
        set_error(error, error_size, "device lacks command-pool lifecycle");
        return -ENOSYS;
    }
    const VkCommandPoolCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = request->flags,
        .queueFamilyIndex = request->queue_family_index,
    };
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkResult vulkan_result = create_command_pool(
        device, &create_info, NULL, &command_pool);
    response->vulkan_result = vulkan_result;
    if (vulkan_result != VK_SUCCESS) {
        return 0;
    }
    const uint64_t wire_id = bvb_handle_id(
        BVB_OBJECT_COMMAND_POOL, context->next_command_pool_serial++);
    result = bvb_handle_table_insert(
        &context->objects, wire_id, request->device_id,
        handle_bits(&command_pool, sizeof(command_pool)));
    if (result != 0) {
        destroy_command_pool(device, command_pool, NULL);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    response->command_pool_id = wire_id;
    return 0;
}

int bvb_vulkan_global_context_destroy_command_pool(
    struct bvb_vulkan_global_context *context, uint64_t command_pool_id,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    uint64_t device_id = 0U;
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    int result = resolve_command_pool(
        context, command_pool_id, &device_id, &device, &command_pool);
    if (result != 0) {
        set_error(error, error_size, "unknown command-pool handle");
        return result;
    }
    PFN_vkDestroyCommandPool destroy_command_pool =
        (PFN_vkDestroyCommandPool)context->get_device_proc_addr(
            device, "vkDestroyCommandPool");
    if (destroy_command_pool == NULL) {
        return -ENOSYS;
    }
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_COMMAND_BUFFER &&
            entry->parent_id == command_pool_id) {
            result = bvb_handle_table_remove(
                &context->objects, entry->wire_id,
                BVB_OBJECT_COMMAND_BUFFER, NULL);
            if (result != 0) {
                return result;
            }
        }
    }
    result = bvb_handle_table_remove(
        &context->objects, command_pool_id, BVB_OBJECT_COMMAND_POOL, NULL);
    if (result != 0) {
        return result;
    }
    destroy_command_pool(device, command_pool, NULL);
    return 0;
}

int bvb_vulkan_global_context_reset_command_pool(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_command_pool_reset_request *request,
    int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (request == NULL || vulkan_result == NULL ||
        (request->flags & ~VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT) != 0U) {
        return -EINVAL;
    }
    uint64_t device_id = 0U;
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    int result = resolve_command_pool(
        context, request->command_pool_id, &device_id, &device, &command_pool);
    if (result != 0) {
        set_error(error, error_size, "unknown command-pool handle");
        return result;
    }
    PFN_vkResetCommandPool reset_command_pool =
        (PFN_vkResetCommandPool)context->get_device_proc_addr(
            device, "vkResetCommandPool");
    if (reset_command_pool == NULL) {
        return -ENOSYS;
    }
    *vulkan_result = reset_command_pool(
        device, command_pool, request->flags);
    return 0;
}

int bvb_vulkan_global_context_allocate_command_buffer(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_command_buffer_allocate_request *request,
    struct bvb_vulkan_command_buffer_allocate_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (context == NULL || request == NULL || response == NULL) {
        return -EINVAL;
    }
    *response = (struct bvb_vulkan_command_buffer_allocate_response){0};
    if (request->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY ||
        request->count != 1U) {
        response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return 0;
    }
    uint64_t device_id = 0U;
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    int result = resolve_command_pool(
        context, request->command_pool_id, &device_id, &device, &command_pool);
    if (result != 0) {
        set_error(error, error_size, "unknown command-pool handle");
        return result;
    }
    PFN_vkAllocateCommandBuffers allocate =
        (PFN_vkAllocateCommandBuffers)context->get_device_proc_addr(
            device, "vkAllocateCommandBuffers");
    PFN_vkFreeCommandBuffers free_command_buffers =
        (PFN_vkFreeCommandBuffers)context->get_device_proc_addr(
            device, "vkFreeCommandBuffers");
    if (allocate == NULL || free_command_buffers == NULL) {
        set_error(error, error_size,
                  "device lacks command-buffer allocation lifecycle");
        return -ENOSYS;
    }
    const VkCommandBufferAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1U,
    };
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkResult vulkan_result = allocate(device, &allocate_info, &command_buffer);
    response->vulkan_result = vulkan_result;
    if (vulkan_result != VK_SUCCESS) {
        return 0;
    }
    const uint64_t wire_id = bvb_handle_id(
        BVB_OBJECT_COMMAND_BUFFER, context->next_command_buffer_serial++);
    result = bvb_handle_table_insert(
        &context->objects, wire_id, request->command_pool_id,
        handle_bits(&command_buffer, sizeof(command_buffer)));
    if (result != 0) {
        free_command_buffers(device, command_pool, 1U, &command_buffer);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    response->command_buffer_id = wire_id;
    return 0;
}

int bvb_vulkan_global_context_free_command_buffer(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_command_buffer_free_request *request,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (context == NULL || request == NULL) {
        return -EINVAL;
    }
    uint64_t parent_pool_id = 0U;
    uint64_t command_buffer_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, request->command_buffer_id,
        BVB_OBJECT_COMMAND_BUFFER, &parent_pool_id, &command_buffer_bits);
    if (result != 0 || parent_pool_id != request->command_pool_id) {
        set_error(error, error_size, "command buffer does not belong to pool");
        return result != 0 ? result : -EPROTO;
    }
    uint64_t device_id = 0U;
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    result = resolve_command_pool(
        context, request->command_pool_id, &device_id, &device, &command_pool);
    if (result != 0) {
        return result;
    }
    PFN_vkFreeCommandBuffers free_command_buffers =
        (PFN_vkFreeCommandBuffers)context->get_device_proc_addr(
            device, "vkFreeCommandBuffers");
    if (free_command_buffers == NULL) {
        return -ENOSYS;
    }
    const VkCommandBuffer command_buffer =
        command_buffer_from_bits(command_buffer_bits);
    free_command_buffers(device, command_pool, 1U, &command_buffer);
    return bvb_handle_table_remove(
        &context->objects, request->command_buffer_id,
        BVB_OBJECT_COMMAND_BUFFER, NULL);
}

int bvb_vulkan_global_context_begin_command_buffer(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_command_buffer_begin_request *request,
    int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (request == NULL || vulkan_result == NULL ||
        (request->flags & ~VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) != 0U) {
        return -EINVAL;
    }
    uint64_t device_id = 0U;
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    int result = resolve_command_buffer(
        context, request->command_buffer_id, &device_id, &device,
        &command_pool, &command_buffer);
    if (result != 0) {
        set_error(error, error_size, "unknown command-buffer handle");
        return result;
    }
    PFN_vkBeginCommandBuffer begin =
        (PFN_vkBeginCommandBuffer)context->get_device_proc_addr(
            device, "vkBeginCommandBuffer");
    if (begin == NULL) {
        return -ENOSYS;
    }
    const VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = request->flags,
    };
    *vulkan_result = begin(command_buffer, &begin_info);
    return 0;
}

int bvb_vulkan_global_context_end_command_buffer(
    const struct bvb_vulkan_global_context *context,
    uint64_t command_buffer_id, int32_t *vulkan_result,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (vulkan_result == NULL) {
        return -EINVAL;
    }
    uint64_t device_id = 0U;
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    int result = resolve_command_buffer(
        context, command_buffer_id, &device_id, &device,
        &command_pool, &command_buffer);
    if (result != 0) {
        set_error(error, error_size, "unknown command-buffer handle");
        return result;
    }
    PFN_vkEndCommandBuffer end =
        (PFN_vkEndCommandBuffer)context->get_device_proc_addr(
            device, "vkEndCommandBuffer");
    if (end == NULL) {
        return -ENOSYS;
    }
    *vulkan_result = end(command_buffer);
    return 0;
}

int bvb_vulkan_global_context_queue_submit_command(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_queue_submit_command_request *request,
    int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (request == NULL || vulkan_result == NULL) {
        return -EINVAL;
    }
    VkDevice queue_device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    int result = resolve_queue(
        context, request->queue_id, &queue_device, &queue);
    if (result != 0) {
        set_error(error, error_size, "unknown queue handle");
        return result;
    }
    uint64_t queue_device_id = 0U;
    uint64_t queue_bits = 0U;
    result = bvb_handle_table_lookup(
        &context->objects, request->queue_id, BVB_OBJECT_QUEUE,
        &queue_device_id, &queue_bits);
    uint64_t command_device_id = 0U;
    VkDevice command_device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    if (result == 0) {
        result = resolve_command_buffer(
            context, request->command_buffer_id, &command_device_id,
            &command_device, &command_pool, &command_buffer);
    }
    if (result != 0 || command_device_id != queue_device_id ||
        command_device != queue_device) {
        set_error(error, error_size,
                  "queue and command buffer have different devices");
        return result != 0 ? result : -EPROTO;
    }
    PFN_vkQueueSubmit submit =
        (PFN_vkQueueSubmit)context->get_device_proc_addr(
            queue_device, "vkQueueSubmit");
    if (submit == NULL) {
        return -ENOSYS;
    }
    const VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1U,
        .pCommandBuffers = &command_buffer,
    };
    *vulkan_result = submit(queue, 1U, &submit_info, VK_NULL_HANDLE);
    return 0;
}

static int resolve_device_child(
    const struct bvb_vulkan_global_context *context, uint64_t object_id,
    enum bvb_object_type type, uint64_t *device_id, VkDevice *device,
    uint64_t *native_bits) {
    if (context == NULL || device_id == NULL || device == NULL ||
        native_bits == NULL) return -EINVAL;
    int result = bvb_handle_table_lookup(
        &context->objects, object_id, type, device_id, native_bits);
    uint64_t device_bits = 0U;
    if (result == 0) {
        result = bvb_handle_table_lookup(
            &context->objects, *device_id, BVB_OBJECT_DEVICE, NULL,
            &device_bits);
    }
    if (result == 0) *device = device_from_bits(device_bits);
    return result;
}

static struct bvb_memory_metadata *memory_metadata_slot(
    struct bvb_vulkan_global_context *context, uint64_t memory_id) {
    struct bvb_memory_metadata *empty = NULL;
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        struct bvb_memory_metadata *metadata = &context->memory_metadata[index];
        if (metadata->memory_id == memory_id) return metadata;
        if (metadata->memory_id == 0U && empty == NULL) empty = metadata;
    }
    return empty;
}

int bvb_vulkan_global_context_create_buffer(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_buffer_create_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || response == NULL) return -EINVAL;
    *response = (struct bvb_vulkan_object_create_response){0};
    if (request->size == 0U || request->size > 16U * 1024U * 1024U ||
        request->flags != 0U ||
        request->usage != VK_BUFFER_USAGE_TRANSFER_DST_BIT) {
        response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return 0;
    }
    uint64_t device_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, request->device_id, BVB_OBJECT_DEVICE, NULL,
        &device_bits);
    if (result != 0) return result;
    const VkDevice device = device_from_bits(device_bits);
    PFN_vkCreateBuffer create_buffer =
        (PFN_vkCreateBuffer)context->get_device_proc_addr(device,
                                                           "vkCreateBuffer");
    PFN_vkDestroyBuffer destroy_buffer =
        (PFN_vkDestroyBuffer)context->get_device_proc_addr(
            device, "vkDestroyBuffer");
    if (create_buffer == NULL || destroy_buffer == NULL) return -ENOSYS;
    const VkBufferCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = request->size,
        .usage = request->usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buffer = VK_NULL_HANDLE;
    response->vulkan_result = create_buffer(
        device, &create_info, NULL, &buffer);
    if (response->vulkan_result != VK_SUCCESS) return 0;
    const uint64_t wire_id = bvb_handle_id(
        BVB_OBJECT_BUFFER, context->next_buffer_serial++);
    result = bvb_handle_table_insert(
        &context->objects, wire_id, request->device_id,
        handle_bits(&buffer, sizeof(buffer)));
    if (result != 0) {
        destroy_buffer(device, buffer, NULL);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    response->object_id = wire_id;
    return 0;
}

int bvb_vulkan_global_context_destroy_buffer(
    struct bvb_vulkan_global_context *context, uint64_t buffer_id,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    uint64_t device_id = 0U, buffer_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, buffer_id, BVB_OBJECT_BUFFER, &device_id, &device,
        &buffer_bits);
    if (result != 0) return result;
    PFN_vkDestroyBuffer destroy_buffer =
        (PFN_vkDestroyBuffer)context->get_device_proc_addr(
            device, "vkDestroyBuffer");
    if (destroy_buffer == NULL) return -ENOSYS;
    result = bvb_handle_table_remove(
        &context->objects, buffer_id, BVB_OBJECT_BUFFER, NULL);
    if (result == 0) destroy_buffer(device, buffer_from_bits(buffer_bits), NULL);
    return result;
}

int bvb_vulkan_global_context_get_buffer_requirements(
    const struct bvb_vulkan_global_context *context, uint64_t buffer_id,
    struct bvb_vulkan_buffer_requirements *requirements,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (requirements == NULL) return -EINVAL;
    uint64_t device_id = 0U, buffer_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, buffer_id, BVB_OBJECT_BUFFER, &device_id, &device,
        &buffer_bits);
    if (result != 0) return result;
    PFN_vkGetBufferMemoryRequirements get_requirements =
        (PFN_vkGetBufferMemoryRequirements)context->get_device_proc_addr(
            device, "vkGetBufferMemoryRequirements");
    if (get_requirements == NULL) return -ENOSYS;
    VkMemoryRequirements native = {0};
    get_requirements(device, buffer_from_bits(buffer_bits), &native);
    *requirements = (struct bvb_vulkan_buffer_requirements){
        .size = native.size,
        .alignment = native.alignment,
        .memory_type_bits = native.memoryTypeBits,
    };
    return native.size != 0U && native.alignment != 0U &&
                   native.memoryTypeBits != 0U
               ? 0 : -EPROTO;
}

int bvb_vulkan_global_context_allocate_memory(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_memory_allocate_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || response == NULL) return -EINVAL;
    *response = (struct bvb_vulkan_object_create_response){0};
    if (request->allocation_size == 0U ||
        request->allocation_size > 16U * 1024U * 1024U) {
        response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return 0;
    }
    uint64_t physical_id = 0U, device_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, request->device_id, BVB_OBJECT_DEVICE,
        &physical_id, &device_bits);
    if (result != 0) return result;
    VkPhysicalDeviceMemoryProperties properties;
    result = bvb_vulkan_global_context_get_memory_properties(
        context, physical_id, &properties, error, error_size);
    if (result != 0) return result;
    if (request->memory_type_index >= properties.memoryTypeCount) {
        response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return 0;
    }
    const VkDevice device = device_from_bits(device_bits);
    PFN_vkAllocateMemory allocate =
        (PFN_vkAllocateMemory)context->get_device_proc_addr(
            device, "vkAllocateMemory");
    PFN_vkFreeMemory free_memory =
        (PFN_vkFreeMemory)context->get_device_proc_addr(device,
                                                        "vkFreeMemory");
    if (allocate == NULL || free_memory == NULL) return -ENOSYS;
    const VkMemoryAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = request->allocation_size,
        .memoryTypeIndex = request->memory_type_index,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    response->vulkan_result = allocate(device, &allocate_info, NULL, &memory);
    if (response->vulkan_result != VK_SUCCESS) return 0;
    struct bvb_memory_metadata *metadata = memory_metadata_slot(context, 0U);
    if (metadata == NULL) {
        free_memory(device, memory, NULL);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    const uint64_t wire_id = bvb_handle_id(
        BVB_OBJECT_DEVICE_MEMORY, context->next_memory_serial++);
    result = bvb_handle_table_insert(
        &context->objects, wire_id, request->device_id,
        handle_bits(&memory, sizeof(memory)));
    if (result != 0) {
        free_memory(device, memory, NULL);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    *metadata = (struct bvb_memory_metadata){
        .memory_id = wire_id,
        .allocation_size = request->allocation_size,
        .property_flags =
            properties.memoryTypes[request->memory_type_index].propertyFlags,
    };
    response->object_id = wire_id;
    return 0;
}

int bvb_vulkan_global_context_free_memory(
    struct bvb_vulkan_global_context *context, uint64_t memory_id,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    uint64_t device_id = 0U, memory_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, memory_id, BVB_OBJECT_DEVICE_MEMORY, &device_id, &device,
        &memory_bits);
    if (result != 0) return result;
    PFN_vkFreeMemory free_memory =
        (PFN_vkFreeMemory)context->get_device_proc_addr(device,
                                                        "vkFreeMemory");
    if (free_memory == NULL) return -ENOSYS;
    result = bvb_handle_table_remove(
        &context->objects, memory_id, BVB_OBJECT_DEVICE_MEMORY, NULL);
    if (result != 0) return result;
    struct bvb_memory_metadata *metadata =
        memory_metadata_slot(context, memory_id);
    if (metadata != NULL && metadata->memory_id == memory_id)
        *metadata = (struct bvb_memory_metadata){0};
    free_memory(device, memory_from_bits(memory_bits), NULL);
    return 0;
}

int bvb_vulkan_global_context_bind_buffer_memory(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_buffer_bind_request *request,
    int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (request == NULL || vulkan_result == NULL) return -EINVAL;
    uint64_t buffer_device_id = 0U, buffer_bits = 0U;
    uint64_t memory_device_id = 0U, memory_bits = 0U;
    VkDevice buffer_device = VK_NULL_HANDLE, memory_device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, request->buffer_id, BVB_OBJECT_BUFFER, &buffer_device_id,
        &buffer_device, &buffer_bits);
    if (result == 0)
        result = resolve_device_child(
            context, request->memory_id, BVB_OBJECT_DEVICE_MEMORY,
            &memory_device_id, &memory_device, &memory_bits);
    if (result != 0 || buffer_device_id != memory_device_id ||
        buffer_device != memory_device) return result != 0 ? result : -EPROTO;
    PFN_vkBindBufferMemory bind =
        (PFN_vkBindBufferMemory)context->get_device_proc_addr(
            buffer_device, "vkBindBufferMemory");
    if (bind == NULL) return -ENOSYS;
    *vulkan_result = bind(
        buffer_device, buffer_from_bits(buffer_bits),
        memory_from_bits(memory_bits), request->offset);
    return 0;
}

int bvb_vulkan_global_context_command_buffer_fill(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_command_buffer_fill_request *request,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (request == NULL || request->size == 0U ||
        (request->offset & 3U) != 0U || (request->size & 3U) != 0U)
        return -EINVAL;
    uint64_t command_device_id = 0U, buffer_device_id = 0U, buffer_bits = 0U;
    VkDevice command_device = VK_NULL_HANDLE, buffer_device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    int result = resolve_command_buffer(
        context, request->command_buffer_id, &command_device_id,
        &command_device, &command_pool, &command_buffer);
    if (result == 0)
        result = resolve_device_child(
            context, request->buffer_id, BVB_OBJECT_BUFFER, &buffer_device_id,
            &buffer_device, &buffer_bits);
    if (result != 0 || command_device_id != buffer_device_id ||
        command_device != buffer_device) return result != 0 ? result : -EPROTO;
    PFN_vkCmdFillBuffer fill =
        (PFN_vkCmdFillBuffer)context->get_device_proc_addr(
            command_device, "vkCmdFillBuffer");
    PFN_vkCmdPipelineBarrier barrier =
        (PFN_vkCmdPipelineBarrier)context->get_device_proc_addr(
            command_device, "vkCmdPipelineBarrier");
    if (fill == NULL || barrier == NULL) return -ENOSYS;
    fill(command_buffer, buffer_from_bits(buffer_bits), request->offset,
         request->size, request->data);
    const VkBufferMemoryBarrier memory_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer_from_bits(buffer_bits),
        .offset = request->offset,
        .size = request->size,
    };
    barrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0U, 0U, NULL, 1U,
            &memory_barrier, 0U, NULL);
    return 0;
}

int bvb_vulkan_global_context_verify_memory_fill(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_memory_verify_fill_request *request,
    struct bvb_vulkan_memory_verify_fill_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || response == NULL ||
        request->size == 0U || (request->size & 3U) != 0U) return -EINVAL;
    *response = (struct bvb_vulkan_memory_verify_fill_response){0};
    struct bvb_memory_metadata *metadata = memory_metadata_slot(
        (struct bvb_vulkan_global_context *)context, request->memory_id);
    if (metadata == NULL || metadata->memory_id != request->memory_id ||
        request->offset > metadata->allocation_size ||
        request->size > metadata->allocation_size - request->offset ||
        (metadata->property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0U)
        return -ERANGE;
    uint64_t device_id = 0U, memory_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, request->memory_id, BVB_OBJECT_DEVICE_MEMORY, &device_id,
        &device, &memory_bits);
    if (result != 0) return result;
    PFN_vkMapMemory map = (PFN_vkMapMemory)context->get_device_proc_addr(
        device, "vkMapMemory");
    PFN_vkUnmapMemory unmap =
        (PFN_vkUnmapMemory)context->get_device_proc_addr(device,
                                                         "vkUnmapMemory");
    if (map == NULL || unmap == NULL) return -ENOSYS;
    void *mapped = NULL;
    response->vulkan_result = map(
        device, memory_from_bits(memory_bits), 0U, VK_WHOLE_SIZE, 0U, &mapped);
    if (response->vulkan_result != VK_SUCCESS || mapped == NULL) return 0;
    if ((metadata->property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0U) {
        PFN_vkInvalidateMappedMemoryRanges invalidate =
            (PFN_vkInvalidateMappedMemoryRanges)context->get_device_proc_addr(
                device, "vkInvalidateMappedMemoryRanges");
        const VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = memory_from_bits(memory_bits),
            .offset = 0U,
            .size = VK_WHOLE_SIZE,
        };
        if (invalidate == NULL)
            response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
        else
            response->vulkan_result = invalidate(device, 1U, &range);
    }
    if (response->vulkan_result == VK_SUCCESS) {
        const uint32_t *words = (const uint32_t *)(
            (const uint8_t *)mapped + request->offset);
        for (uint64_t index = 0U; index < request->size / 4U; ++index)
            if (words[index] != request->expected_word)
                ++response->mismatched_words;
    }
    unmap(device, memory_from_bits(memory_bits));
    return 0;
}

static int resolve_host_visible_memory(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_memory_io_request *request,
    const struct bvb_memory_metadata **metadata, VkDevice *device,
    VkDeviceMemory *memory) {
    if (context == NULL || request == NULL || metadata == NULL ||
        device == NULL || memory == NULL || request->length == 0U ||
        request->length > BVB_VULKAN_MEMORY_IO_MAX_BYTES) {
        return -EINVAL;
    }
    struct bvb_memory_metadata *found = memory_metadata_slot(
        (struct bvb_vulkan_global_context *)context, request->memory_id);
    if (found == NULL || found->memory_id != request->memory_id ||
        request->offset > found->allocation_size ||
        request->length > found->allocation_size - request->offset ||
        (found->property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0U) {
        return -ERANGE;
    }
    uint64_t device_id = 0U;
    uint64_t memory_bits = 0U;
    int result = resolve_device_child(
        context, request->memory_id, BVB_OBJECT_DEVICE_MEMORY, &device_id,
        device, &memory_bits);
    if (result == 0) {
        *metadata = found;
        *memory = memory_from_bits(memory_bits);
    }
    return result;
}

int bvb_vulkan_global_context_write_memory(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_memory_io_request *request, const uint8_t *data,
    int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (data == NULL || vulkan_result == NULL) return -EINVAL;
    *vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    const struct bvb_memory_metadata *metadata = NULL;
    VkDevice device = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    int result = resolve_host_visible_memory(
        context, request, &metadata, &device, &memory);
    if (result != 0) return result;
    PFN_vkMapMemory map = (PFN_vkMapMemory)context->get_device_proc_addr(
        device, "vkMapMemory");
    PFN_vkUnmapMemory unmap =
        (PFN_vkUnmapMemory)context->get_device_proc_addr(device,
                                                         "vkUnmapMemory");
    if (map == NULL || unmap == NULL) return -ENOSYS;
    void *mapped = NULL;
    *vulkan_result = map(device, memory, 0U, VK_WHOLE_SIZE, 0U, &mapped);
    if (*vulkan_result != VK_SUCCESS || mapped == NULL) return 0;
    memcpy((uint8_t *)mapped + request->offset, data, request->length);
    if ((metadata->property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ==
        0U) {
        PFN_vkFlushMappedMemoryRanges flush =
            (PFN_vkFlushMappedMemoryRanges)context->get_device_proc_addr(
                device, "vkFlushMappedMemoryRanges");
        const VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = memory,
            .offset = 0U,
            .size = VK_WHOLE_SIZE,
        };
        *vulkan_result = flush == NULL
                             ? VK_ERROR_FEATURE_NOT_PRESENT
                             : flush(device, 1U, &range);
    }
    unmap(device, memory);
    return 0;
}

int bvb_vulkan_global_context_read_memory(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_memory_io_request *request, uint8_t *data,
    uint32_t capacity, uint32_t *length, int32_t *vulkan_result,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (data == NULL || length == NULL || vulkan_result == NULL ||
        request == NULL || capacity < request->length) return -EINVAL;
    *length = 0U;
    *vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    const struct bvb_memory_metadata *metadata = NULL;
    VkDevice device = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    int result = resolve_host_visible_memory(
        context, request, &metadata, &device, &memory);
    if (result != 0) return result;
    PFN_vkMapMemory map = (PFN_vkMapMemory)context->get_device_proc_addr(
        device, "vkMapMemory");
    PFN_vkUnmapMemory unmap =
        (PFN_vkUnmapMemory)context->get_device_proc_addr(device,
                                                         "vkUnmapMemory");
    if (map == NULL || unmap == NULL) return -ENOSYS;
    void *mapped = NULL;
    *vulkan_result = map(device, memory, 0U, VK_WHOLE_SIZE, 0U, &mapped);
    if (*vulkan_result != VK_SUCCESS || mapped == NULL) return 0;
    if ((metadata->property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ==
        0U) {
        PFN_vkInvalidateMappedMemoryRanges invalidate =
            (PFN_vkInvalidateMappedMemoryRanges)
                context->get_device_proc_addr(
                    device, "vkInvalidateMappedMemoryRanges");
        const VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = memory,
            .offset = 0U,
            .size = VK_WHOLE_SIZE,
        };
        *vulkan_result = invalidate == NULL
                             ? VK_ERROR_FEATURE_NOT_PRESENT
                             : invalidate(device, 1U, &range);
    }
    if (*vulkan_result == VK_SUCCESS) {
        memcpy(data, (const uint8_t *)mapped + request->offset,
               request->length);
        *length = request->length;
    }
    unmap(device, memory);
    return 0;
}

int bvb_vulkan_global_context_create_fence(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_fence_create_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || response == NULL) return -EINVAL;
    *response = (struct bvb_vulkan_object_create_response){0};
    if ((request->flags & ~VK_FENCE_CREATE_SIGNALED_BIT) != 0U) {
        response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return 0;
    }
    uint64_t device_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, request->device_id, BVB_OBJECT_DEVICE, NULL,
        &device_bits);
    if (result != 0) return result;
    const VkDevice device = device_from_bits(device_bits);
    PFN_vkCreateFence create_fence =
        (PFN_vkCreateFence)context->get_device_proc_addr(device,
                                                         "vkCreateFence");
    PFN_vkDestroyFence destroy_fence =
        (PFN_vkDestroyFence)context->get_device_proc_addr(device,
                                                          "vkDestroyFence");
    if (create_fence == NULL || destroy_fence == NULL) return -ENOSYS;
    const VkFenceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = request->flags,
    };
    VkFence fence = VK_NULL_HANDLE;
    response->vulkan_result = create_fence(device, &create_info, NULL, &fence);
    if (response->vulkan_result != VK_SUCCESS) return 0;
    const uint64_t wire_id = bvb_handle_id(
        BVB_OBJECT_FENCE, context->next_fence_serial++);
    result = bvb_handle_table_insert(
        &context->objects, wire_id, request->device_id,
        handle_bits(&fence, sizeof(fence)));
    if (result != 0) {
        destroy_fence(device, fence, NULL);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    response->object_id = wire_id;
    return 0;
}

int bvb_vulkan_global_context_destroy_fence(
    struct bvb_vulkan_global_context *context, uint64_t fence_id,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    uint64_t device_id = 0U, fence_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, fence_id, BVB_OBJECT_FENCE, &device_id, &device, &fence_bits);
    if (result != 0) return result;
    PFN_vkDestroyFence destroy_fence =
        (PFN_vkDestroyFence)context->get_device_proc_addr(device,
                                                          "vkDestroyFence");
    if (destroy_fence == NULL) return -ENOSYS;
    result = bvb_handle_table_remove(
        &context->objects, fence_id, BVB_OBJECT_FENCE, NULL);
    if (result == 0) destroy_fence(device, fence_from_bits(fence_bits), NULL);
    return result;
}

int bvb_vulkan_global_context_get_fence_status(
    const struct bvb_vulkan_global_context *context, uint64_t fence_id,
    int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (vulkan_result == NULL) return -EINVAL;
    uint64_t device_id = 0U, fence_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, fence_id, BVB_OBJECT_FENCE, &device_id, &device, &fence_bits);
    if (result != 0) return result;
    PFN_vkGetFenceStatus get_status =
        (PFN_vkGetFenceStatus)context->get_device_proc_addr(
            device, "vkGetFenceStatus");
    if (get_status == NULL) return -ENOSYS;
    *vulkan_result = get_status(device, fence_from_bits(fence_bits));
    return 0;
}

int bvb_vulkan_global_context_wait_fence(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_fence_wait_request *request,
    int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (request == NULL || vulkan_result == NULL || request->wait_all > 1U)
        return -EINVAL;
    uint64_t device_id = 0U, fence_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, request->fence_id, BVB_OBJECT_FENCE, &device_id, &device,
        &fence_bits);
    if (result != 0) return result;
    PFN_vkWaitForFences wait =
        (PFN_vkWaitForFences)context->get_device_proc_addr(
            device, "vkWaitForFences");
    if (wait == NULL) return -ENOSYS;
    const VkFence fence = fence_from_bits(fence_bits);
    *vulkan_result = wait(device, 1U, &fence,
                          request->wait_all != 0U ? VK_TRUE : VK_FALSE,
                          request->timeout);
    return 0;
}

int bvb_vulkan_global_context_reset_fence(
    const struct bvb_vulkan_global_context *context, uint64_t fence_id,
    int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (vulkan_result == NULL) return -EINVAL;
    uint64_t device_id = 0U, fence_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, fence_id, BVB_OBJECT_FENCE, &device_id, &device, &fence_bits);
    if (result != 0) return result;
    PFN_vkResetFences reset =
        (PFN_vkResetFences)context->get_device_proc_addr(device,
                                                         "vkResetFences");
    if (reset == NULL) return -ENOSYS;
    const VkFence fence = fence_from_bits(fence_bits);
    *vulkan_result = reset(device, 1U, &fence);
    return 0;
}

int bvb_vulkan_global_context_queue_submit_command_fence(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_queue_submit_command_fence_request *request,
    int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || vulkan_result == NULL)
        return -EINVAL;
    VkDevice queue_device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    int result = resolve_queue(context, request->queue_id, &queue_device,
                               &queue);
    uint64_t queue_device_id = 0U, queue_bits = 0U;
    if (result == 0)
        result = bvb_handle_table_lookup(
            &context->objects, request->queue_id, BVB_OBJECT_QUEUE,
            &queue_device_id, &queue_bits);
    uint64_t command_device_id = 0U;
    VkDevice command_device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    if (result == 0)
        result = resolve_command_buffer(
            context, request->command_buffer_id, &command_device_id,
            &command_device, &command_pool, &command_buffer);
    uint64_t fence_device_id = 0U, fence_bits = 0U;
    VkDevice fence_device = VK_NULL_HANDLE;
    if (result == 0)
        result = resolve_device_child(
            context, request->fence_id, BVB_OBJECT_FENCE, &fence_device_id,
            &fence_device, &fence_bits);
    if (result != 0 || command_device_id != queue_device_id ||
        fence_device_id != queue_device_id || command_device != queue_device ||
        fence_device != queue_device) {
        set_error(error, error_size,
                  "queue, command buffer, and fence have different devices");
        return result != 0 ? result : -EPROTO;
    }
    PFN_vkQueueSubmit submit =
        (PFN_vkQueueSubmit)context->get_device_proc_addr(queue_device,
                                                         "vkQueueSubmit");
    if (submit == NULL) return -ENOSYS;
    const VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1U,
        .pCommandBuffers = &command_buffer,
    };
    *vulkan_result = submit(queue, 1U, &submit_info,
                            fence_from_bits(fence_bits));
    return 0;
}
