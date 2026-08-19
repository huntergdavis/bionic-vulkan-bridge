#define VK_NO_PROTOTYPES

#include <bvb/global_dispatch.h>
#include <bvb/handle.h>
#include <bvb/vulkan_discovery.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #expression);                                               \
            return 1;                                                           \
        }                                                                       \
    } while (0)

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *name);

#define RESOLVE_GLOBAL(name)                                                   \
    PFN_##name name = NULL;                                                    \
    do {                                                                       \
        PFN_vkVoidFunction erased =                                            \
            vkGetInstanceProcAddr(VK_NULL_HANDLE, #name);                      \
        CHECK(erased != NULL);                                                 \
        _Static_assert(sizeof(name) == sizeof(erased),                         \
                       "Vulkan function pointer width mismatch");             \
        memcpy(&name, &erased, sizeof(name));                                  \
    } while (0)

int main(void) {
    RESOLVE_GLOBAL(vkEnumerateInstanceVersion);
    RESOLVE_GLOBAL(vkEnumerateInstanceExtensionProperties);
    RESOLVE_GLOBAL(vkEnumerateInstanceLayerProperties);
    RESOLVE_GLOBAL(vkCreateInstance);

    CHECK(vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkNotARealCommand") == NULL);
    uint32_t api_version = 0U;
    CHECK(vkEnumerateInstanceVersion(&api_version) == VK_SUCCESS);
    CHECK(api_version >= VK_API_VERSION_1_0);

    uint32_t extension_count = 99U;
    CHECK(vkEnumerateInstanceExtensionProperties(
              NULL, &extension_count, NULL) == VK_SUCCESS);
    CHECK(extension_count == 0U);
    VkExtensionProperties extension = {{0}, 0U};
    extension_count = 1U;
    CHECK(vkEnumerateInstanceExtensionProperties(
              NULL, &extension_count, &extension) == VK_SUCCESS);
    CHECK(extension_count == 0U);
    CHECK(vkEnumerateInstanceExtensionProperties(
              "VK_LAYER_BVB_fake_native_only", &extension_count,
              NULL) == VK_ERROR_LAYER_NOT_PRESENT);

    uint32_t layer_count = 99U;
    CHECK(vkEnumerateInstanceLayerProperties(&layer_count, NULL) == VK_SUCCESS);
    CHECK(layer_count == 0U);
    VkLayerProperties layer = {{0}, 0U, 0U, {0}};
    layer_count = 1U;
    CHECK(vkEnumerateInstanceLayerProperties(&layer_count, &layer) ==
          VK_SUCCESS);
    CHECK(layer_count == 0U);

    const VkApplicationInfo application_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "bvb-e025-test",
        .apiVersion = VK_API_VERSION_1_1,
    };
    const char *unsupported_extension = "VK_KHR_surface";
    VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application_info,
        .enabledExtensionCount = 1U,
        .ppEnabledExtensionNames = &unsupported_extension,
    };
    VkInstance instance_one = VK_NULL_HANDLE;
    CHECK(vkCreateInstance(&create_info, NULL, &instance_one) ==
          VK_ERROR_EXTENSION_NOT_PRESENT);
    CHECK(instance_one == VK_NULL_HANDLE);

    create_info.enabledExtensionCount = 0U;
    create_info.ppEnabledExtensionNames = NULL;
    CHECK(vkCreateInstance(&create_info, NULL, &instance_one) == VK_SUCCESS);
    CHECK(instance_one != VK_NULL_HANDLE);
    const uint64_t instance_one_id = bvb_instance_proxy_id(instance_one);
    CHECK(bvb_handle_type(instance_one_id) == BVB_OBJECT_INSTANCE);
    CHECK(bvb_handle_serial(instance_one_id) == 1U);
    CHECK(vkGetInstanceProcAddr(instance_one, "vkCmdDraw") != NULL);
    CHECK(vkGetInstanceProcAddr(instance_one,
                                "vkGetPhysicalDeviceProperties") != NULL);

    PFN_vkEnumeratePhysicalDevices enumerate_physical_devices = NULL;
    PFN_vkDestroyInstance destroy_instance = NULL;
    PFN_vkVoidFunction erased = vkGetInstanceProcAddr(
        instance_one, "vkEnumeratePhysicalDevices");
    CHECK(erased != NULL);
    memcpy(&enumerate_physical_devices, &erased,
           sizeof(enumerate_physical_devices));
    erased = vkGetInstanceProcAddr(instance_one, "vkDestroyInstance");
    CHECK(erased != NULL);
    memcpy(&destroy_instance, &erased, sizeof(destroy_instance));
    uint32_t physical_count = 0U;
    CHECK(enumerate_physical_devices(instance_one, &physical_count, NULL) ==
          VK_SUCCESS);
    CHECK(physical_count == 1U);
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    CHECK(enumerate_physical_devices(instance_one, &physical_count,
                                     &physical_device) == VK_SUCCESS);
    CHECK(physical_count == 1U);
    CHECK(physical_device != VK_NULL_HANDLE);
    const uint64_t physical_id =
        bvb_physical_device_proxy_id(physical_device);
    CHECK(bvb_handle_type(physical_id) == BVB_OBJECT_PHYSICAL_DEVICE);
    CHECK(bvb_handle_serial(physical_id) == 1U);
    VkPhysicalDevice repeated_device = VK_NULL_HANDLE;
    physical_count = 1U;
    CHECK(enumerate_physical_devices(instance_one, &physical_count,
                                     &repeated_device) == VK_SUCCESS);
    CHECK(repeated_device == physical_device);

    PFN_vkGetPhysicalDeviceProperties get_physical_device_properties = NULL;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_properties = NULL;
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties = NULL;
    PFN_vkEnumerateDeviceExtensionProperties enumerate_device_extensions =
        NULL;
#define RESOLVE_INSTANCE(entry_name, variable)                                \
    do {                                                                      \
        erased = vkGetInstanceProcAddr(instance_one, #entry_name);            \
        CHECK(erased != NULL);                                                \
        memcpy(&(variable), &erased, sizeof(variable));                       \
    } while (0)
    RESOLVE_INSTANCE(vkGetPhysicalDeviceProperties,
                     get_physical_device_properties);
    RESOLVE_INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties,
                     get_queue_properties);
    RESOLVE_INSTANCE(vkGetPhysicalDeviceMemoryProperties,
                     get_memory_properties);
    RESOLVE_INSTANCE(vkEnumerateDeviceExtensionProperties,
                     enumerate_device_extensions);
#undef RESOLVE_INSTANCE

    VkPhysicalDeviceProperties properties;
    get_physical_device_properties(physical_device, &properties);
    CHECK(properties.apiVersion >= VK_API_VERSION_1_0);
    CHECK(properties.vendorID != 0U);
    CHECK(properties.deviceName[0] != '\0');

    uint32_t queue_count = 0U;
    get_queue_properties(physical_device, &queue_count, NULL);
    CHECK(queue_count > 0U);
    CHECK(queue_count <= BVB_VULKAN_MAX_QUEUE_FAMILIES);
    const uint32_t available_queue_count = queue_count;
    VkQueueFamilyProperties *queues =
        calloc(available_queue_count, sizeof(*queues));
    CHECK(queues != NULL);
    queue_count = available_queue_count - 1U;
    get_queue_properties(physical_device, &queue_count, queues);
    CHECK(queue_count == available_queue_count - 1U);
    queue_count = available_queue_count;
    get_queue_properties(physical_device, &queue_count, queues);
    CHECK(queue_count == available_queue_count);
    bool usable_queue = false;
    uint32_t queue_family_index = UINT32_MAX;
    for (uint32_t index = 0U; index < queue_count; ++index) {
        usable_queue |= queues[index].queueCount > 0U &&
                        queues[index].queueFlags != 0U;
        if (queue_family_index == UINT32_MAX &&
            queues[index].queueCount > 0U && queues[index].queueFlags != 0U) {
            queue_family_index = index;
        }
    }
    CHECK(usable_queue);
    CHECK(queue_family_index != UINT32_MAX);
    free(queues);

    VkPhysicalDeviceMemoryProperties memory;
    get_memory_properties(physical_device, &memory);
    CHECK(memory.memoryTypeCount > 0U);
    CHECK(memory.memoryTypeCount <= VK_MAX_MEMORY_TYPES);
    CHECK(memory.memoryHeapCount > 0U);
    CHECK(memory.memoryHeapCount <= VK_MAX_MEMORY_HEAPS);
    for (uint32_t index = 0U; index < memory.memoryTypeCount; ++index) {
        CHECK(memory.memoryTypes[index].heapIndex < memory.memoryHeapCount);
    }

    uint32_t device_extension_count = 0U;
    CHECK(enumerate_device_extensions(
              physical_device, NULL, &device_extension_count, NULL) ==
          VK_SUCCESS);
    CHECK(device_extension_count > 0U);
    CHECK(device_extension_count <= BVB_VULKAN_MAX_DEVICE_EXTENSIONS);
    const uint32_t available_device_extension_count = device_extension_count;
    VkExtensionProperties *device_extensions =
        calloc(available_device_extension_count, sizeof(*device_extensions));
    CHECK(device_extensions != NULL);
    device_extension_count = available_device_extension_count - 1U;
    CHECK(enumerate_device_extensions(
              physical_device, NULL, &device_extension_count,
              device_extensions) == VK_INCOMPLETE);
    CHECK(device_extension_count == available_device_extension_count - 1U);
    device_extension_count = available_device_extension_count;
    CHECK(enumerate_device_extensions(
              physical_device, NULL, &device_extension_count,
              device_extensions) == VK_SUCCESS);
    CHECK(device_extension_count == available_device_extension_count);
    for (uint32_t index = 0U; index < device_extension_count; ++index) {
        CHECK(device_extensions[index].extensionName[0] != '\0');
    }
    CHECK(enumerate_device_extensions(
              physical_device, "VK_LAYER_not_exposed",
              &device_extension_count, NULL) == VK_ERROR_LAYER_NOT_PRESENT);
    free(device_extensions);

    PFN_vkGetPhysicalDeviceFeatures get_physical_device_features = NULL;
    erased = vkGetInstanceProcAddr(
        instance_one, "vkGetPhysicalDeviceFeatures");
    CHECK(erased != NULL);
    memcpy(&get_physical_device_features, &erased,
           sizeof(get_physical_device_features));
    PFN_vkCreateDevice create_device = NULL;
    erased = vkGetInstanceProcAddr(instance_one, "vkCreateDevice");
    CHECK(erased != NULL);
    memcpy(&create_device, &erased, sizeof(create_device));
    VkPhysicalDeviceFeatures features;
    get_physical_device_features(physical_device, &features);
    CHECK(features.samplerAnisotropy == VK_TRUE);

    const float queue_priority = 1.0F;
    const VkDeviceQueueCreateInfo queue_create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queue_family_index,
        .queueCount = 1U,
        .pQueuePriorities = &queue_priority,
    };
    VkDeviceCreateInfo device_create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1U,
        .pQueueCreateInfos = &queue_create_info,
    };
    const char *unsupported_device_extension = "VK_KHR_swapchain";
    device_create_info.enabledExtensionCount = 1U;
    device_create_info.ppEnabledExtensionNames = &unsupported_device_extension;
    VkDevice device = VK_NULL_HANDLE;
    CHECK(create_device(physical_device, &device_create_info, NULL, &device) ==
          VK_ERROR_EXTENSION_NOT_PRESENT);
    CHECK(device == VK_NULL_HANDLE);
    device_create_info.enabledExtensionCount = 0U;
    device_create_info.ppEnabledExtensionNames = NULL;
    CHECK(create_device(physical_device, &device_create_info, NULL, &device) ==
          VK_SUCCESS);
    CHECK(device != VK_NULL_HANDLE);
    const uint64_t device_id = bvb_device_proxy_id(device);
    CHECK(bvb_handle_type(device_id) == BVB_OBJECT_DEVICE);
    CHECK(bvb_handle_serial(device_id) == 1U);

    PFN_vkGetDeviceQueue get_device_queue = NULL;
    PFN_vkDestroyDevice destroy_device = NULL;
    erased = vkGetDeviceProcAddr(device, "vkGetDeviceQueue");
    CHECK(erased != NULL);
    memcpy(&get_device_queue, &erased, sizeof(get_device_queue));
    erased = vkGetDeviceProcAddr(device, "vkDestroyDevice");
    CHECK(erased != NULL);
    memcpy(&destroy_device, &erased, sizeof(destroy_device));
    CHECK(vkGetDeviceProcAddr(device, "vkCmdDraw") != NULL);
    VkQueue queue = VK_NULL_HANDLE;
    get_device_queue(device, queue_family_index, 0U, &queue);
    CHECK(queue != VK_NULL_HANDLE);
    const uint64_t queue_id = bvb_queue_proxy_id(queue);
    CHECK(bvb_handle_type(queue_id) == BVB_OBJECT_QUEUE);
    CHECK(bvb_handle_serial(queue_id) == 1U);
    VkQueue repeated_queue = VK_NULL_HANDLE;
    get_device_queue(device, queue_family_index, 0U, &repeated_queue);
    CHECK(repeated_queue == queue);
    destroy_device(device, NULL);

    VkInstance instance_two = VK_NULL_HANDLE;
    create_info.pApplicationInfo = NULL;
    CHECK(vkCreateInstance(&create_info, NULL, &instance_two) == VK_SUCCESS);
    CHECK(instance_two != VK_NULL_HANDLE);
    const uint64_t instance_two_id = bvb_instance_proxy_id(instance_two);
    CHECK(bvb_handle_type(instance_two_id) == BVB_OBJECT_INSTANCE);
    CHECK(bvb_handle_serial(instance_two_id) == 2U);
    CHECK(instance_two_id != instance_one_id);

    destroy_instance(instance_one, NULL);
    PFN_vkDestroyInstance destroy_instance_two = NULL;
    erased = vkGetInstanceProcAddr(instance_two, "vkDestroyInstance");
    CHECK(erased != NULL);
    memcpy(&destroy_instance_two, &erased, sizeof(destroy_instance_two));
    destroy_instance_two(instance_two, NULL);

    printf("PASS: global Vulkan discovery api=%u "
           "exposed_extensions=0 exposed_layers=0 "
           "instance_one=%llu instance_two=%llu physical_device=%llu "
           "device=%s device_api=%u driver=%u vendor=%u device_id=%u "
           "queues=%u memory_types=%u memory_heaps=%u device_extensions=%u "
           "sampler_anisotropy=%u logical_device=%llu queue=%llu\n",
           api_version, (unsigned long long)instance_one_id,
           (unsigned long long)instance_two_id,
           (unsigned long long)physical_id, properties.deviceName,
           properties.apiVersion, properties.driverVersion,
           properties.vendorID, properties.deviceID,
           available_queue_count, memory.memoryTypeCount,
           memory.memoryHeapCount, available_device_extension_count,
           features.samplerAnisotropy,
           (unsigned long long)device_id,
           (unsigned long long)queue_id);
    return 0;
}
