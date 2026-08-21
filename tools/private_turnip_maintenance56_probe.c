#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_symbol(void *symbol, void *function, size_t function_size) {
    if (function_size != sizeof(symbol)) {
        fprintf(stderr, "unsupported function-pointer representation\n");
        exit(2);
    }
    memcpy(function, &symbol, function_size);
}

static bool find_extension(const VkExtensionProperties *extensions,
                           uint32_t count,
                           const char *name,
                           uint32_t *spec_version) {
    for (uint32_t index = 0; index < count; ++index) {
        if (strcmp(extensions[index].extensionName, name) == 0) {
            *spec_version = extensions[index].specVersion;
            return true;
        }
    }
    return false;
}

static uint32_t choose_queue_family(
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_properties,
    VkPhysicalDevice physical_device) {
    uint32_t count = 0;
    get_properties(physical_device, &count, NULL);
    if (count == 0) {
        return UINT32_MAX;
    }

    VkQueueFamilyProperties *properties = calloc(count, sizeof(*properties));
    if (properties == NULL) {
        return UINT32_MAX;
    }
    get_properties(physical_device, &count, properties);

    uint32_t selected = UINT32_MAX;
    for (uint32_t index = 0; index < count; ++index) {
        const VkQueueFlags work = VK_QUEUE_GRAPHICS_BIT |
                                  VK_QUEUE_COMPUTE_BIT |
                                  VK_QUEUE_TRANSFER_BIT;
        if (properties[index].queueCount != 0 &&
            (properties[index].queueFlags & work) != 0) {
            selected = index;
            break;
        }
    }
    free(properties);
    return selected;
}

int main(int argc, char **argv) {
    if (argc != 2 || argv[1][0] != '/') {
        fprintf(stderr, "usage: %s /absolute/path/libvulkan_freedreno.so\n",
                argv[0]);
        return 2;
    }

    int status = 1;
    void *driver = NULL;
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice *physical_devices = NULL;
    VkExtensionProperties *extensions = NULL;
    PFN_vkDestroyInstance destroy_instance = NULL;
    PFN_vkDestroyDevice destroy_device = NULL;

    driver = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (driver == NULL) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        goto cleanup;
    }

    PFN_vkGetInstanceProcAddr get_instance_proc_addr = NULL;
    copy_symbol(dlsym(driver, "vk_icdGetInstanceProcAddr"),
                &get_instance_proc_addr, sizeof(get_instance_proc_addr));
    if (get_instance_proc_addr == NULL) {
        fprintf(stderr, "private ICD resolver is not exported\n");
        goto cleanup;
    }

    PFN_vkCreateInstance create_instance =
        (PFN_vkCreateInstance)get_instance_proc_addr(
            VK_NULL_HANDLE, "vkCreateInstance");
    if (create_instance == NULL) {
        fprintf(stderr, "vkCreateInstance is not resolvable\n");
        goto cleanup;
    }

    const VkApplicationInfo application_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "bvb-private-turnip-maintenance56-probe",
        .applicationVersion = 1,
        .pEngineName = "bvb",
        .engineVersion = 1,
        .apiVersion = VK_API_VERSION_1_3,
    };
    const VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application_info,
    };
    VkResult result = create_instance(&instance_info, NULL, &instance);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateInstance failed: %d\n", result);
        goto cleanup;
    }

#define RESOLVE_INSTANCE(type, variable, command)                              \
    type variable = (type)get_instance_proc_addr(instance, command);           \
    if (variable == NULL) {                                                    \
        fprintf(stderr, "%s is not resolvable\n", command);                  \
        goto cleanup;                                                          \
    }

    RESOLVE_INSTANCE(PFN_vkDestroyInstance, resolved_destroy_instance,
                     "vkDestroyInstance");
    destroy_instance = resolved_destroy_instance;
    RESOLVE_INSTANCE(PFN_vkEnumeratePhysicalDevices, enumerate_physical_devices,
                     "vkEnumeratePhysicalDevices");
    RESOLVE_INSTANCE(PFN_vkEnumerateDeviceExtensionProperties,
                     enumerate_device_extensions,
                     "vkEnumerateDeviceExtensionProperties");
    RESOLVE_INSTANCE(PFN_vkGetPhysicalDeviceProperties,
                     get_physical_device_properties,
                     "vkGetPhysicalDeviceProperties");
    RESOLVE_INSTANCE(PFN_vkGetPhysicalDeviceFeatures2,
                     get_physical_device_features2,
                     "vkGetPhysicalDeviceFeatures2");
    RESOLVE_INSTANCE(PFN_vkGetPhysicalDeviceQueueFamilyProperties,
                     get_queue_family_properties,
                     "vkGetPhysicalDeviceQueueFamilyProperties");
    RESOLVE_INSTANCE(PFN_vkCreateDevice, create_device, "vkCreateDevice");
    RESOLVE_INSTANCE(PFN_vkGetDeviceProcAddr, get_device_proc_addr,
                     "vkGetDeviceProcAddr");

    uint32_t physical_device_count = 0;
    result = enumerate_physical_devices(instance, &physical_device_count, NULL);
    if (result != VK_SUCCESS || physical_device_count == 0) {
        fprintf(stderr, "physical-device enumeration failed: %d count=%" PRIu32
                        "\n", result, physical_device_count);
        goto cleanup;
    }
    physical_devices = calloc(physical_device_count, sizeof(*physical_devices));
    if (physical_devices == NULL) {
        fprintf(stderr, "physical-device allocation failed\n");
        goto cleanup;
    }
    result = enumerate_physical_devices(instance, &physical_device_count,
                                        physical_devices);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "physical-device retrieval failed: %d\n", result);
        goto cleanup;
    }
    const VkPhysicalDevice physical_device = physical_devices[0];

    VkPhysicalDeviceProperties properties;
    get_physical_device_properties(physical_device, &properties);
    if (strstr(properties.deviceName, "Turnip") == NULL) {
        fprintf(stderr, "unexpected physical device: %s\n",
                properties.deviceName);
        goto cleanup;
    }

    uint32_t extension_count = 0;
    result = enumerate_device_extensions(physical_device, NULL,
                                         &extension_count, NULL);
    if (result != VK_SUCCESS || extension_count == 0) {
        fprintf(stderr, "device-extension enumeration failed: %d count=%" PRIu32
                        "\n", result, extension_count);
        goto cleanup;
    }
    extensions = calloc(extension_count, sizeof(*extensions));
    if (extensions == NULL) {
        fprintf(stderr, "device-extension allocation failed\n");
        goto cleanup;
    }
    result = enumerate_device_extensions(physical_device, NULL,
                                         &extension_count, extensions);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "device-extension retrieval failed: %d\n", result);
        goto cleanup;
    }

    uint32_t maintenance5_spec = 0;
    uint32_t maintenance6_spec = 0;
    if (!find_extension(extensions, extension_count,
                        VK_KHR_MAINTENANCE_5_EXTENSION_NAME,
                        &maintenance5_spec) ||
        !find_extension(extensions, extension_count,
                        VK_KHR_MAINTENANCE_6_EXTENSION_NAME,
                        &maintenance6_spec)) {
        fprintf(stderr, "maintenance5/6 are not both enumerated\n");
        goto cleanup;
    }

    VkPhysicalDeviceMaintenance6FeaturesKHR maintenance6 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR,
    };
    VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR,
        .pNext = &maintenance6,
    };
    VkPhysicalDeviceFeatures2 features2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &maintenance5,
    };
    get_physical_device_features2(physical_device, &features2);
    if (maintenance5.maintenance5 != VK_TRUE ||
        maintenance6.maintenance6 != VK_TRUE) {
        fprintf(stderr, "maintenance features are not both true: %u %u\n",
                maintenance5.maintenance5, maintenance6.maintenance6);
        goto cleanup;
    }

    const uint32_t queue_family = choose_queue_family(
        get_queue_family_properties, physical_device);
    if (queue_family == UINT32_MAX) {
        fprintf(stderr, "no usable queue family\n");
        goto cleanup;
    }

    const float queue_priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queue_family,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };
    const char *const enabled_extensions[] = {
        VK_KHR_MAINTENANCE_5_EXTENSION_NAME,
        VK_KHR_MAINTENANCE_6_EXTENSION_NAME,
    };
    maintenance5.maintenance5 = VK_TRUE;
    maintenance6.maintenance6 = VK_TRUE;
    const VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &maintenance5,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = enabled_extensions,
    };
    result = create_device(physical_device, &device_info, NULL, &device);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateDevice with maintenance5/6 failed: %d\n",
                result);
        goto cleanup;
    }

    destroy_device = (PFN_vkDestroyDevice)get_device_proc_addr(
        device, "vkDestroyDevice");
    PFN_vkDeviceWaitIdle device_wait_idle =
        (PFN_vkDeviceWaitIdle)get_device_proc_addr(device, "vkDeviceWaitIdle");
    if (destroy_device == NULL || device_wait_idle == NULL) {
        fprintf(stderr, "device teardown commands are not resolvable\n");
        goto cleanup;
    }
    result = device_wait_idle(device);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkDeviceWaitIdle failed: %d\n", result);
        goto cleanup;
    }

    printf("PASS: private Turnip maintenance5/6 device=%s api=%" PRIu32
           " extensions=%" PRIu32 " maintenance5_spec=%" PRIu32
           " maintenance6_spec=%" PRIu32 " features=1,1 create_device=pass"
           " device_idle=pass\n",
           properties.deviceName, properties.apiVersion, extension_count,
           maintenance5_spec, maintenance6_spec);
    status = 0;

cleanup:
    if (device != VK_NULL_HANDLE && destroy_device != NULL) {
        destroy_device(device, NULL);
    }
    free(extensions);
    free(physical_devices);
    if (instance != VK_NULL_HANDLE && destroy_instance != NULL) {
        destroy_instance(instance, NULL);
    }
    if (driver != NULL) {
        dlclose(driver);
    }
    return status;
}
