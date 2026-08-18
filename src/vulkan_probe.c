#define VK_NO_PROTOTYPES

#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BVB_DEFAULT_LOADER "/system/lib64/libvulkan.so"

enum bvb_exit_code {
    BVB_OK = 0,
    BVB_USAGE = 2,
    BVB_LOADER_ERROR = 3,
    BVB_VULKAN_ERROR = 4,
    BVB_ALLOCATION_ERROR = 5,
};

static void print_usage(const char *program) {
    fprintf(stderr, "usage: %s [--loader ABSOLUTE_PATH]\n", program);
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

static void print_version(uint32_t version) {
    printf(
        "{\"raw\":%" PRIu32 ",\"major\":%" PRIu32
        ",\"minor\":%" PRIu32 ",\"patch\":%" PRIu32 "}",
        version,
        VK_API_VERSION_MAJOR(version),
        VK_API_VERSION_MINOR(version),
        VK_API_VERSION_PATCH(version));
}

static PFN_vkVoidFunction symbol_from_loader(void *loader, const char *name) {
    void *raw_symbol = dlsym(loader, name);
    PFN_vkVoidFunction function = NULL;

    if (raw_symbol != NULL) {
        memcpy(&function, &raw_symbol, sizeof(function));
    }
    return function;
}

static int parse_arguments(int argc, char **argv, const char **loader_path) {
    *loader_path = BVB_DEFAULT_LOADER;
    if (argc == 1) {
        return BVB_OK;
    }
    if (argc == 3 && strcmp(argv[1], "--loader") == 0 && argv[2][0] == '/') {
        *loader_path = argv[2];
        return BVB_OK;
    }
    print_usage(argv[0]);
    return BVB_USAGE;
}

int main(int argc, char **argv) {
    const char *loader_path = NULL;
    void *loader = NULL;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr = NULL;
    PFN_vkEnumerateInstanceVersion enumerate_instance_version = NULL;
    PFN_vkEnumerateInstanceExtensionProperties enumerate_extensions = NULL;
    PFN_vkCreateInstance create_instance = NULL;
    PFN_vkEnumeratePhysicalDevices enumerate_devices = NULL;
    PFN_vkGetPhysicalDeviceProperties get_device_properties = NULL;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_properties = NULL;
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties = NULL;
    PFN_vkDestroyInstance destroy_instance = NULL;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice *devices = NULL;
    uint32_t loader_api_version = VK_API_VERSION_1_0;
    uint32_t extension_count = 0;
    uint32_t device_count = 0;
    int exit_code = parse_arguments(argc, argv, &loader_path);

    if (exit_code != BVB_OK) {
        return exit_code;
    }

    loader = dlopen(loader_path, RTLD_NOW | RTLD_LOCAL);
    if (loader == NULL) {
        fprintf(stderr, "bvb: could not load %s: %s\n", loader_path, dlerror());
        return BVB_LOADER_ERROR;
    }

    get_instance_proc_addr =
        (PFN_vkGetInstanceProcAddr)symbol_from_loader(loader, "vkGetInstanceProcAddr");
    if (get_instance_proc_addr == NULL) {
        fprintf(stderr, "bvb: %s has no vkGetInstanceProcAddr\n", loader_path);
        exit_code = BVB_LOADER_ERROR;
        goto cleanup;
    }

    enumerate_instance_version = (PFN_vkEnumerateInstanceVersion)
        get_instance_proc_addr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    enumerate_extensions = (PFN_vkEnumerateInstanceExtensionProperties)
        get_instance_proc_addr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    create_instance = (PFN_vkCreateInstance)
        get_instance_proc_addr(VK_NULL_HANDLE, "vkCreateInstance");

    if (enumerate_extensions == NULL || create_instance == NULL) {
        fputs("bvb: Vulkan loader is missing required global entry points\n", stderr);
        exit_code = BVB_VULKAN_ERROR;
        goto cleanup;
    }

    if (enumerate_instance_version != NULL &&
        enumerate_instance_version(&loader_api_version) != VK_SUCCESS) {
        fputs("bvb: vkEnumerateInstanceVersion failed\n", stderr);
        exit_code = BVB_VULKAN_ERROR;
        goto cleanup;
    }
    if (enumerate_extensions(NULL, &extension_count, NULL) != VK_SUCCESS) {
        fputs("bvb: vkEnumerateInstanceExtensionProperties failed\n", stderr);
        exit_code = BVB_VULKAN_ERROR;
        goto cleanup;
    }

    const VkApplicationInfo application_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "bvb-vulkan-probe",
        .applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
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
        fprintf(stderr, "bvb: vkCreateInstance failed: %d\n", (int)result);
        exit_code = BVB_VULKAN_ERROR;
        goto cleanup;
    }

    enumerate_devices = (PFN_vkEnumeratePhysicalDevices)
        get_instance_proc_addr(instance, "vkEnumeratePhysicalDevices");
    get_device_properties = (PFN_vkGetPhysicalDeviceProperties)
        get_instance_proc_addr(instance, "vkGetPhysicalDeviceProperties");
    get_queue_properties = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
        get_instance_proc_addr(instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    get_memory_properties = (PFN_vkGetPhysicalDeviceMemoryProperties)
        get_instance_proc_addr(instance, "vkGetPhysicalDeviceMemoryProperties");
    destroy_instance = (PFN_vkDestroyInstance)
        get_instance_proc_addr(instance, "vkDestroyInstance");

    if (enumerate_devices == NULL || get_device_properties == NULL ||
        get_queue_properties == NULL || get_memory_properties == NULL ||
        destroy_instance == NULL) {
        fputs("bvb: Vulkan loader is missing required instance entry points\n", stderr);
        exit_code = BVB_VULKAN_ERROR;
        goto cleanup;
    }

    result = enumerate_devices(instance, &device_count, NULL);
    if (result != VK_SUCCESS || device_count == 0U) {
        fprintf(stderr, "bvb: physical-device enumeration failed or returned zero: %d\n",
                (int)result);
        exit_code = BVB_VULKAN_ERROR;
        goto cleanup;
    }

    devices = calloc(device_count, sizeof(*devices));
    if (devices == NULL) {
        fputs("bvb: could not allocate the physical-device list\n", stderr);
        exit_code = BVB_ALLOCATION_ERROR;
        goto cleanup;
    }
    result = enumerate_devices(instance, &device_count, devices);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        fprintf(stderr, "bvb: physical-device enumeration failed: %d\n", (int)result);
        exit_code = BVB_VULKAN_ERROR;
        goto cleanup;
    }

    fputs("{\"schema_version\":1,\"loader_path\":", stdout);
    print_json_string(loader_path);
    fputs(",\"loader_api_version\":", stdout);
    print_version(loader_api_version);
    printf(",\"instance_extension_count\":%" PRIu32
           ",\"physical_device_count\":%" PRIu32 ",\"physical_devices\":[",
           extension_count, device_count);

    for (uint32_t device_index = 0; device_index < device_count; ++device_index) {
        VkPhysicalDeviceProperties properties;
        VkPhysicalDeviceMemoryProperties memory_properties;
        uint32_t queue_family_count = 0;
        VkDeviceSize device_local_bytes = 0;

        memset(&properties, 0, sizeof(properties));
        memset(&memory_properties, 0, sizeof(memory_properties));
        get_device_properties(devices[device_index], &properties);
        get_queue_properties(devices[device_index], &queue_family_count, NULL);
        get_memory_properties(devices[device_index], &memory_properties);

        for (uint32_t heap_index = 0;
             heap_index < memory_properties.memoryHeapCount;
             ++heap_index) {
            if ((memory_properties.memoryHeaps[heap_index].flags &
                 VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0U) {
                device_local_bytes += memory_properties.memoryHeaps[heap_index].size;
            }
        }

        if (device_index != 0U) {
            putchar(',');
        }
        printf("{\"index\":%" PRIu32 ",\"name\":", device_index);
        print_json_string(properties.deviceName);
        fputs(",\"api_version\":", stdout);
        print_version(properties.apiVersion);
        printf(",\"driver_version\":%" PRIu32
               ",\"vendor_id\":%" PRIu32 ",\"device_id\":%" PRIu32
               ",\"device_type\":%u,\"queue_family_count\":%" PRIu32
               ",\"memory_heap_count\":%" PRIu32
               ",\"device_local_bytes\":%" PRIu64 "}",
               properties.driverVersion,
               properties.vendorID,
               properties.deviceID,
               (unsigned int)properties.deviceType,
               queue_family_count,
               memory_properties.memoryHeapCount,
               (uint64_t)device_local_bytes);
    }
    fputs("]}\n", stdout);
    exit_code = BVB_OK;

cleanup:
    free(devices);
    if (instance != VK_NULL_HANDLE && destroy_instance != NULL) {
        destroy_instance(instance, NULL);
    }
    dlclose(loader);
    return exit_code;
}

