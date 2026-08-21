#define VK_NO_PROTOTYPES

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdio.h>
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

static PFN_vkVoidFunction resolve(
    VkInstance instance, const char *name) {
    return vkGetInstanceProcAddr(instance, name);
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    const bool inject = strcmp(argv[1], "inject") == 0;
    const bool explicit_dependency = strcmp(argv[1], "explicit") == 0;
    const bool no_swapchain = strcmp(argv[1], "no-swapchain") == 0;
    const bool unavailable = strcmp(argv[1], "unavailable") == 0;
    CHECK(inject || explicit_dependency || no_swapchain || unavailable);

    PFN_vkCreateInstance create_instance = NULL;
    PFN_vkEnumeratePhysicalDevices enumerate_physical_devices = NULL;
    PFN_vkCreateDevice create_device = NULL;
    PFN_vkDestroyDevice destroy_device = NULL;
    PFN_vkDestroyInstance destroy_instance = NULL;
    PFN_vkVoidFunction erased = resolve(VK_NULL_HANDLE, "vkCreateInstance");
    CHECK(erased != NULL);
    memcpy(&create_instance, &erased, sizeof(create_instance));

    const VkApplicationInfo application_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "bvb-e072-device-dependency",
        .apiVersion = VK_API_VERSION_1_1,
    };
    const VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application_info,
    };
    VkInstance instance = VK_NULL_HANDLE;
    CHECK(create_instance(&instance_info, NULL, &instance) == VK_SUCCESS);
    CHECK(instance != VK_NULL_HANDLE);

    erased = resolve(instance, "vkEnumeratePhysicalDevices");
    CHECK(erased != NULL);
    memcpy(&enumerate_physical_devices, &erased,
           sizeof(enumerate_physical_devices));
    erased = resolve(instance, "vkCreateDevice");
    CHECK(erased != NULL);
    memcpy(&create_device, &erased, sizeof(create_device));
    erased = resolve(instance, "vkDestroyInstance");
    CHECK(erased != NULL);
    memcpy(&destroy_instance, &erased, sizeof(destroy_instance));
    uint32_t physical_device_count = 1U;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    CHECK(enumerate_physical_devices(
              instance, &physical_device_count, &physical_device) ==
          VK_SUCCESS);
    CHECK(physical_device_count == 1U &&
          physical_device != VK_NULL_HANDLE);

    const float priority = 1.0F;
    const VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0U,
        .queueCount = 1U,
        .pQueuePriorities = &priority,
    };
    const char *const injected_extensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };
    const char *const explicit_extensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1U,
        .pQueueCreateInfos = &queue_info,
    };
    if (!no_swapchain) {
        device_info.enabledExtensionCount = 2U;
        device_info.ppEnabledExtensionNames =
            explicit_dependency ? explicit_extensions : injected_extensions;
    }
    VkDevice device = VK_NULL_HANDLE;
    const VkResult result = create_device(
        physical_device, &device_info, NULL, &device);
    if (unavailable) {
        CHECK(result == VK_ERROR_EXTENSION_NOT_PRESENT);
        CHECK(device == VK_NULL_HANDLE);
    } else {
        CHECK(result == VK_SUCCESS);
        CHECK(device != VK_NULL_HANDLE);
        erased = vkGetDeviceProcAddr(device, "vkDestroyDevice");
        CHECK(erased != NULL);
        memcpy(&destroy_device, &erased, sizeof(destroy_device));
        destroy_device(device, NULL);
    }
    destroy_instance(instance, NULL);
    printf("PASS: E072 device dependency mode=%s\n", argv[1]);
    return 0;
}
