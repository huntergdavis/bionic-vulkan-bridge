#define VK_NO_PROTOTYPES

#include <bvb/vulkan_caps.h>

#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    void *raw_symbol = dlsym(loader, name);
    PFN_vkVoidFunction function = NULL;
    if (raw_symbol != NULL) {
        memcpy(&function, &raw_symbol, sizeof(function));
    }
    return function;
}

int bvb_vulkan_collect(const char *loader_path, struct bvb_vulkan_caps *caps,
                       char *error, size_t error_size) {
    void *loader = NULL;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice *physical_devices = NULL;
    PFN_vkDestroyInstance destroy_instance = NULL;
    int status = 0;

    if (error != NULL && error_size > 0U) {
        error[0] = '\0';
    }
    if (loader_path == NULL || loader_path[0] != '/' || caps == NULL) {
        set_error(error, error_size, "loader path must be absolute");
        return -EINVAL;
    }
    memset(caps, 0, sizeof(*caps));
    caps->loader_api_version = VK_API_VERSION_1_0;

    loader = dlopen(loader_path, RTLD_NOW | RTLD_LOCAL);
    if (loader == NULL) {
        set_error(error, error_size, "could not load %s: %s", loader_path,
                  dlerror());
        return -ENOENT;
    }

    PFN_vkGetInstanceProcAddr get_instance_proc_addr =
        (PFN_vkGetInstanceProcAddr)symbol_from_loader(loader,
                                                      "vkGetInstanceProcAddr");
    if (get_instance_proc_addr == NULL) {
        set_error(error, error_size, "%s has no vkGetInstanceProcAddr",
                  loader_path);
        status = -ENOSYS;
        goto cleanup;
    }

    PFN_vkEnumerateInstanceVersion enumerate_instance_version =
        (PFN_vkEnumerateInstanceVersion)get_instance_proc_addr(
            VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    PFN_vkEnumerateInstanceExtensionProperties enumerate_extensions =
        (PFN_vkEnumerateInstanceExtensionProperties)get_instance_proc_addr(
            VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    PFN_vkCreateInstance create_instance =
        (PFN_vkCreateInstance)get_instance_proc_addr(VK_NULL_HANDLE,
                                                      "vkCreateInstance");
    if (enumerate_extensions == NULL || create_instance == NULL) {
        set_error(error, error_size,
                  "Vulkan loader is missing required global entry points");
        status = -ENOSYS;
        goto cleanup;
    }

    if (enumerate_instance_version != NULL &&
        enumerate_instance_version(&caps->loader_api_version) != VK_SUCCESS) {
        set_error(error, error_size, "vkEnumerateInstanceVersion failed");
        status = -EIO;
        goto cleanup;
    }
    if (enumerate_extensions(NULL, &caps->instance_extension_count, NULL) !=
        VK_SUCCESS) {
        set_error(error, error_size,
                  "vkEnumerateInstanceExtensionProperties failed");
        status = -EIO;
        goto cleanup;
    }

    const VkApplicationInfo application_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "bionic-vulkan-bridge",
        .applicationVersion = VK_MAKE_API_VERSION(0, 0, 3, 0),
        .pEngineName = "none",
        .engineVersion = 0,
        .apiVersion = VK_API_VERSION_1_0,
    };
    const VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application_info,
    };
    VkResult result = create_instance(&create_info, NULL, &instance);
    if (result != VK_SUCCESS) {
        set_error(error, error_size, "vkCreateInstance failed: %d", (int)result);
        status = -EIO;
        goto cleanup;
    }

    PFN_vkEnumeratePhysicalDevices enumerate_devices =
        (PFN_vkEnumeratePhysicalDevices)get_instance_proc_addr(
            instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties get_device_properties =
        (PFN_vkGetPhysicalDeviceProperties)get_instance_proc_addr(
            instance, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_properties =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)get_instance_proc_addr(
            instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)get_instance_proc_addr(
            instance, "vkGetPhysicalDeviceMemoryProperties");
    destroy_instance = (PFN_vkDestroyInstance)get_instance_proc_addr(
        instance, "vkDestroyInstance");
    if (enumerate_devices == NULL || get_device_properties == NULL ||
        get_queue_properties == NULL || get_memory_properties == NULL ||
        destroy_instance == NULL) {
        set_error(error, error_size,
                  "Vulkan loader is missing required instance entry points");
        status = -ENOSYS;
        goto cleanup;
    }

    result = enumerate_devices(instance, &caps->physical_device_count, NULL);
    if (result != VK_SUCCESS || caps->physical_device_count == 0U) {
        set_error(error, error_size,
                  "physical-device enumeration failed or returned zero: %d",
                  (int)result);
        status = -ENODEV;
        goto cleanup;
    }
    if (caps->physical_device_count > 64U) {
        set_error(error, error_size, "unreasonable physical-device count: %u",
                  caps->physical_device_count);
        status = -EOVERFLOW;
        goto cleanup;
    }

    uint32_t enumerated_count = caps->physical_device_count;
    physical_devices = calloc(enumerated_count, sizeof(*physical_devices));
    if (physical_devices == NULL) {
        set_error(error, error_size, "could not allocate physical-device list");
        status = -ENOMEM;
        goto cleanup;
    }
    result = enumerate_devices(instance, &enumerated_count, physical_devices);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        set_error(error, error_size, "physical-device enumeration failed: %d",
                  (int)result);
        status = -EIO;
        goto cleanup;
    }
    caps->physical_device_count = enumerated_count;
    caps->included_device_count = enumerated_count;
    if (caps->included_device_count > BVB_VULKAN_MAX_DEVICES) {
        caps->included_device_count = BVB_VULKAN_MAX_DEVICES;
    }

    for (uint32_t index = 0; index < caps->included_device_count; ++index) {
        VkPhysicalDeviceProperties properties;
        VkPhysicalDeviceMemoryProperties memory_properties;
        uint32_t queue_family_count = 0;
        memset(&properties, 0, sizeof(properties));
        memset(&memory_properties, 0, sizeof(memory_properties));
        get_device_properties(physical_devices[index], &properties);
        get_queue_properties(physical_devices[index], &queue_family_count, NULL);
        get_memory_properties(physical_devices[index], &memory_properties);

        struct bvb_vulkan_device_caps *device = &caps->devices[index];
        device->api_version = properties.apiVersion;
        device->driver_version = properties.driverVersion;
        device->vendor_id = properties.vendorID;
        device->device_id = properties.deviceID;
        device->device_type = (uint32_t)properties.deviceType;
        device->queue_family_count = queue_family_count;
        device->memory_heap_count = memory_properties.memoryHeapCount;
        (void)snprintf(device->name, sizeof(device->name), "%s",
                       properties.deviceName);
        for (uint32_t heap_index = 0;
             heap_index < memory_properties.memoryHeapCount; ++heap_index) {
            if ((memory_properties.memoryHeaps[heap_index].flags &
                 VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0U) {
                device->device_local_bytes +=
                    memory_properties.memoryHeaps[heap_index].size;
            }
        }
    }

cleanup:
    free(physical_devices);
    if (instance != VK_NULL_HANDLE && destroy_instance != NULL) {
        destroy_instance(instance, NULL);
    }
    (void)dlclose(loader);
    return status;
}

