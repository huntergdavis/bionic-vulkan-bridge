#define VK_NO_PROTOTYPES

#include <bvb/global_dispatch.h>
#include <bvb/handle.h>

#include <stdint.h>
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
                                "vkGetPhysicalDeviceProperties") == NULL);

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

    printf("PASS: global Vulkan bootstrap api=%u "
           "exposed_extensions=0 exposed_layers=0 "
           "instance_one=%llu instance_two=%llu physical_device=%llu\n",
           api_version, (unsigned long long)instance_one_id,
           (unsigned long long)instance_two_id,
           (unsigned long long)physical_id);
    return 0;
}
