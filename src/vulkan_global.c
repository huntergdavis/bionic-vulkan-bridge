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
    BVB_GLOBAL_INSTANCE_CAPACITY = 8,
};

struct bvb_vulkan_global_context {
    void *loader;
    PFN_vkCreateInstance create_instance;
    PFN_vkDestroyInstance destroy_instance;
    struct bvb_vulkan_global_info info;
    struct bvb_handle_entry instance_entries[BVB_GLOBAL_INSTANCE_CAPACITY];
    struct bvb_handle_table instances;
    uint64_t next_instance_serial;
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
    PFN_vkGetInstanceProcAddr gipa =
        (PFN_vkGetInstanceProcAddr)symbol_from_loader(
            context->loader, "vkGetInstanceProcAddr");
    if (gipa == NULL) {
        set_error(error, error_size, "loader has no vkGetInstanceProcAddr");
        bvb_vulkan_global_context_destroy(context);
        return -ENOSYS;
    }
    PFN_vkEnumerateInstanceVersion enumerate_version =
        (PFN_vkEnumerateInstanceVersion)gipa(
            VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    PFN_vkEnumerateInstanceExtensionProperties enumerate_extensions =
        (PFN_vkEnumerateInstanceExtensionProperties)gipa(
            VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    PFN_vkEnumerateInstanceLayerProperties enumerate_layers =
        (PFN_vkEnumerateInstanceLayerProperties)gipa(
            VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
    context->create_instance = (PFN_vkCreateInstance)gipa(
        VK_NULL_HANDLE, "vkCreateInstance");
    context->destroy_instance = (PFN_vkDestroyInstance)gipa(
        VK_NULL_HANDLE, "vkDestroyInstance");
    if (enumerate_extensions == NULL || enumerate_layers == NULL ||
        context->create_instance == NULL || context->destroy_instance == NULL) {
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
        &context->instances, context->instance_entries,
        BVB_GLOBAL_INSTANCE_CAPACITY);
    if (status != 0) {
        set_error(error, error_size, "instance table init failed: %d", status);
        bvb_vulkan_global_context_destroy(context);
        return status;
    }
    context->next_instance_serial = 1U;
    *output = context;
    return 0;
}

void bvb_vulkan_global_context_destroy(
    struct bvb_vulkan_global_context *context) {
    if (context == NULL) {
        return;
    }
    if (context->destroy_instance != NULL) {
        for (size_t index = 0U; index < BVB_GLOBAL_INSTANCE_CAPACITY; ++index) {
            const struct bvb_handle_entry *entry =
                &context->instance_entries[index];
            if (entry->wire_id != 0U && entry->native_bits != 0U) {
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
    if (context->instances.count == context->instances.capacity) {
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
    const uint64_t native_bits = handle_bits(&instance, sizeof(instance));
    const uint64_t wire_id = bvb_handle_id(
        BVB_OBJECT_INSTANCE, context->next_instance_serial++);
    int status = bvb_handle_table_insert(
        &context->instances, wire_id, 0U, native_bits);
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
