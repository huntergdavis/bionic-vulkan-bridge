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
    BVB_GLOBAL_OBJECT_CAPACITY = 32,
};

struct bvb_vulkan_global_context {
    void *loader;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    PFN_vkCreateInstance create_instance;
    PFN_vkDestroyInstance destroy_instance;
    struct bvb_vulkan_global_info info;
    struct bvb_handle_entry object_entries[BVB_GLOBAL_OBJECT_CAPACITY];
    struct bvb_handle_table objects;
    uint64_t next_instance_serial;
    uint64_t next_physical_device_serial;
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
    *output = context;
    return 0;
}

void bvb_vulkan_global_context_destroy(
    struct bvb_vulkan_global_context *context) {
    if (context == NULL) {
        return;
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
