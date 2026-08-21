#define _POSIX_C_SOURCE 200809L
#define VK_NO_PROTOTYPES

#include <bvb/global_dispatch.h>
#include <bvb/handle.h>
#include <bvb/protocol.h>
#include <bvb/vulkan_discovery.h>

#include <vulkan/vk_layer.h>

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

typedef VkResult(VKAPI_PTR *bvb_create_platform_surface_fn)(
    VkInstance instance, const void *create_info,
    const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface);
typedef VkBool32(VKAPI_PTR *bvb_xlib_presentation_support_fn)(
    VkPhysicalDevice physical_device, uint32_t queue_family_index,
    void *display, unsigned long visual_id);

static const uint64_t test_loader_dispatch =
    UINT64_C(0x4256424c4f414445);
static uint32_t test_loader_data_calls;
static const uint32_t test_dxvk_dummy_frag[] = {
    UINT32_C(0x07230203), UINT32_C(0x00010600), UINT32_C(0x0008000b),
    UINT32_C(0x00000006), UINT32_C(0x00000000), UINT32_C(0x00020011),
    UINT32_C(0x00000001), UINT32_C(0x0006000b), UINT32_C(0x00000001),
    UINT32_C(0x4c534c47), UINT32_C(0x6474732e), UINT32_C(0x3035342e),
    UINT32_C(0x00000000), UINT32_C(0x0003000e), UINT32_C(0x00000000),
    UINT32_C(0x00000001), UINT32_C(0x0005000f), UINT32_C(0x00000004),
    UINT32_C(0x00000004), UINT32_C(0x6e69616d), UINT32_C(0x00000000),
    UINT32_C(0x00030010), UINT32_C(0x00000004), UINT32_C(0x00000007),
    UINT32_C(0x00030003), UINT32_C(0x00000002), UINT32_C(0x000001c2),
    UINT32_C(0x00040005), UINT32_C(0x00000004), UINT32_C(0x6e69616d),
    UINT32_C(0x00000000), UINT32_C(0x00020013), UINT32_C(0x00000002),
    UINT32_C(0x00030021), UINT32_C(0x00000003), UINT32_C(0x00000002),
    UINT32_C(0x00050036), UINT32_C(0x00000002), UINT32_C(0x00000004),
    UINT32_C(0x00000000), UINT32_C(0x00000003), UINT32_C(0x000200f8),
    UINT32_C(0x00000005), UINT32_C(0x000100fd), UINT32_C(0x00010038),
};

static VkResult VKAPI_CALL test_set_device_loader_data(
    VkDevice device, void *object) {
    if (device == VK_NULL_HANDLE || object == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *(const void **)object = &test_loader_dispatch;
    ++test_loader_data_calls;
    return VK_SUCCESS;
}

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
    uint32_t icd_interface_version = 7U;
    CHECK(vk_icdNegotiateLoaderICDInterfaceVersion(
              &icd_interface_version) == VK_SUCCESS);
    CHECK(icd_interface_version == 5U);
    CHECK(vk_icdGetInstanceProcAddr(
              VK_NULL_HANDLE,
              "vk_icdNegotiateLoaderICDInterfaceVersion") != NULL);
    CHECK(vk_icdGetInstanceProcAddr(
              VK_NULL_HANDLE, "vkCreateInstance") != NULL);
    CHECK(vk_icdGetPhysicalDeviceProcAddr(
              VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties") == NULL);

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
    CHECK(extension_count == 7U);
    VkExtensionProperties extensions[7] = {0};
    extension_count = 7U;
    CHECK(vkEnumerateInstanceExtensionProperties(
              NULL, &extension_count, extensions) == VK_SUCCESS);
    CHECK(extension_count == 7U);
    CHECK(strcmp(extensions[0].extensionName,
                 VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) ==
          0);
    CHECK(strcmp(extensions[1].extensionName,
                 VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME) == 0);
    CHECK(strcmp(extensions[2].extensionName,
                 VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME) ==
          0);
    CHECK(strcmp(extensions[3].extensionName, "VK_KHR_surface") == 0);
    CHECK(strcmp(extensions[4].extensionName,
                 "VK_KHR_xlib_surface") == 0);
    CHECK(strcmp(extensions[5].extensionName,
                 "VK_KHR_xcb_surface") == 0);
    CHECK(strcmp(extensions[6].extensionName,
                 "VK_KHR_wayland_surface") == 0);
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
    const char *unsupported_extension = "VK_EXT_debug_utils";
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

    const char *enabled_instance_extensions[6] = {
        extensions[0].extensionName,
        extensions[1].extensionName,
        extensions[2].extensionName,
        extensions[0].extensionName,
        extensions[3].extensionName,
        extensions[4].extensionName,
    };
    create_info.enabledExtensionCount = 6U;
    create_info.ppEnabledExtensionNames = enabled_instance_extensions;
    const uint32_t loader_private_chain_marker = UINT32_C(0x7ffffffe);
    create_info.pNext = &loader_private_chain_marker;
    const VkAllocationCallbacks loader_private_allocator = {0};
    CHECK(vkCreateInstance(&create_info, &loader_private_allocator,
                           &instance_one) == VK_SUCCESS);
    CHECK(instance_one != VK_NULL_HANDLE);
    const uint64_t instance_one_id = bvb_instance_proxy_id(instance_one);
    CHECK(bvb_handle_type(instance_one_id) == BVB_OBJECT_INSTANCE);
    CHECK(bvb_handle_serial(instance_one_id) == 1U);
    CHECK(vkGetInstanceProcAddr(instance_one, "vkCmdDraw") != NULL);
    CHECK(vkGetInstanceProcAddr(instance_one,
                                "vkGetPhysicalDeviceProperties") != NULL);
    CHECK(vkGetInstanceProcAddr(
              instance_one, "vkGetPhysicalDeviceFormatProperties") != NULL);
    CHECK(vkGetInstanceProcAddr(
              instance_one,
              "vkGetPhysicalDeviceImageFormatProperties") != NULL);
    CHECK(vkGetInstanceProcAddr(
              instance_one,
              "vkGetPhysicalDeviceSparseImageFormatProperties") != NULL);

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

    bvb_create_platform_surface_fn create_xlib_surface = NULL;
    PFN_vkDestroySurfaceKHR destroy_surface = NULL;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR get_surface_support = NULL;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR get_surface_capabilities =
        NULL;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR get_surface_formats = NULL;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR get_present_modes = NULL;
    bvb_xlib_presentation_support_fn get_xlib_presentation_support = NULL;
#define RESOLVE_INSTANCE(name, output)                                        \
    do {                                                                       \
        erased = vkGetInstanceProcAddr(instance_one, (name));                 \
        CHECK(erased != NULL);                                                 \
        _Static_assert(sizeof(output) == sizeof(erased),                       \
                       "Vulkan function pointer width mismatch");             \
        memcpy(&(output), &erased, sizeof(output));                            \
    } while (0)
    RESOLVE_INSTANCE("vkCreateXlibSurfaceKHR", create_xlib_surface);
    RESOLVE_INSTANCE("vkDestroySurfaceKHR", destroy_surface);
    RESOLVE_INSTANCE("vkGetPhysicalDeviceSurfaceSupportKHR",
                     get_surface_support);
    RESOLVE_INSTANCE("vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
                     get_surface_capabilities);
    RESOLVE_INSTANCE("vkGetPhysicalDeviceSurfaceFormatsKHR",
                     get_surface_formats);
    RESOLVE_INSTANCE("vkGetPhysicalDeviceSurfacePresentModesKHR",
                     get_present_modes);
    RESOLVE_INSTANCE("vkGetPhysicalDeviceXlibPresentationSupportKHR",
                     get_xlib_presentation_support);
#undef RESOLVE_INSTANCE
    const uint32_t platform_create_info = UINT32_C(0xe051c0de);
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    CHECK(create_xlib_surface(instance_one, &platform_create_info, NULL,
                              &surface) == VK_SUCCESS);
    CHECK(surface != VK_NULL_HANDLE);
    VkBool32 surface_supported = VK_FALSE;
    CHECK(get_surface_support(physical_device, 0U, surface,
                              &surface_supported) == VK_SUCCESS);
    CHECK(surface_supported == VK_TRUE);
    CHECK(get_xlib_presentation_support(physical_device, 0U, NULL, 0U) ==
          VK_TRUE);
    VkSurfaceCapabilitiesKHR surface_capabilities = {0};
    CHECK(get_surface_capabilities(physical_device, surface,
                                   &surface_capabilities) == VK_SUCCESS);
    CHECK(surface_capabilities.currentExtent.width == 2800U);
    CHECK(surface_capabilities.currentExtent.height == 1752U);
    CHECK(surface_capabilities.minImageCount == 2U);
    uint32_t surface_format_count = 0U;
    CHECK(get_surface_formats(physical_device, surface,
                              &surface_format_count, NULL) == VK_SUCCESS);
    CHECK(surface_format_count == 4U);
    VkSurfaceFormatKHR surface_formats[4] = {0};
    CHECK(get_surface_formats(physical_device, surface,
                              &surface_format_count,
                              surface_formats) == VK_SUCCESS);
    CHECK(surface_formats[0].format == VK_FORMAT_B8G8R8A8_UNORM);
    uint32_t present_mode_count = 0U;
    CHECK(get_present_modes(physical_device, surface, &present_mode_count,
                            NULL) == VK_SUCCESS);
    CHECK(present_mode_count == 1U);
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_MAX_ENUM_KHR;
    CHECK(get_present_modes(physical_device, surface, &present_mode_count,
                            &present_mode) == VK_SUCCESS);
    CHECK(present_mode == VK_PRESENT_MODE_FIFO_KHR);

    PFN_vkGetPhysicalDeviceProperties get_physical_device_properties = NULL;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_properties = NULL;
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties = NULL;
    PFN_vkGetPhysicalDeviceFormatProperties get_format_properties = NULL;
    PFN_vkGetPhysicalDeviceImageFormatProperties get_image_format_properties =
        NULL;
    PFN_vkGetPhysicalDeviceFeatures2 get_features2 = NULL;
    PFN_vkGetPhysicalDeviceProperties2 get_properties2 = NULL;
    PFN_vkGetPhysicalDeviceFormatProperties2 get_format_properties2 = NULL;
    PFN_vkGetPhysicalDeviceImageFormatProperties2 get_image_properties2 = NULL;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties2 get_queue_properties2 = NULL;
    PFN_vkGetPhysicalDeviceMemoryProperties2 get_memory_properties2 = NULL;
    PFN_vkGetPhysicalDeviceSparseImageFormatProperties2 get_sparse_properties2 =
        NULL;
    PFN_vkGetPhysicalDeviceExternalBufferProperties get_external_buffer = NULL;
    PFN_vkGetPhysicalDeviceExternalSemaphoreProperties get_external_semaphore =
        NULL;
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
    RESOLVE_INSTANCE(vkGetPhysicalDeviceFormatProperties,
                     get_format_properties);
    RESOLVE_INSTANCE(vkGetPhysicalDeviceImageFormatProperties,
                     get_image_format_properties);
    RESOLVE_INSTANCE(vkGetPhysicalDeviceFeatures2, get_features2);
    RESOLVE_INSTANCE(vkGetPhysicalDeviceProperties2, get_properties2);
    RESOLVE_INSTANCE(vkGetPhysicalDeviceFormatProperties2,
                     get_format_properties2);
    RESOLVE_INSTANCE(vkGetPhysicalDeviceImageFormatProperties2,
                     get_image_properties2);
    RESOLVE_INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties2,
                     get_queue_properties2);
    RESOLVE_INSTANCE(vkGetPhysicalDeviceMemoryProperties2,
                     get_memory_properties2);
    RESOLVE_INSTANCE(vkGetPhysicalDeviceSparseImageFormatProperties2,
                     get_sparse_properties2);
    RESOLVE_INSTANCE(vkGetPhysicalDeviceExternalBufferProperties,
                     get_external_buffer);
    RESOLVE_INSTANCE(vkGetPhysicalDeviceExternalSemaphoreProperties,
                     get_external_semaphore);
    RESOLVE_INSTANCE(vkEnumerateDeviceExtensionProperties,
                     enumerate_device_extensions);
#undef RESOLVE_INSTANCE

    VkPhysicalDeviceProperties properties;
    get_physical_device_properties(physical_device, &properties);
    CHECK(properties.apiVersion >= VK_API_VERSION_1_0);
    CHECK(properties.vendorID != 0U);
    CHECK(properties.deviceName[0] != '\0');
    CHECK(properties.limits.maxPushConstantsSize == 256U);

    VkFormatProperties format_properties;
    get_format_properties(
        physical_device, VK_FORMAT_R8G8B8A8_UNORM, &format_properties);
    CHECK((format_properties.optimalTilingFeatures &
           VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0U);
    CHECK((format_properties.optimalTilingFeatures &
           VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0U);
    CHECK((format_properties.bufferFeatures &
           VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT) != 0U);

    VkImageFormatProperties image_format_properties;
    CHECK(get_image_format_properties(
              physical_device, VK_FORMAT_R8G8B8A8_UNORM,
              VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
              VK_IMAGE_USAGE_SAMPLED_BIT, 0U,
              &image_format_properties) == VK_SUCCESS);
    CHECK(image_format_properties.maxExtent.width == 4096U);
    CHECK(image_format_properties.maxExtent.height == 2048U);
    CHECK(image_format_properties.maxMipLevels == 12U);
    CHECK(image_format_properties.maxArrayLayers == 256U);
    CHECK(image_format_properties.maxResourceSize == UINT64_C(0x100000000));
    memset(&image_format_properties, 0xff, sizeof(image_format_properties));
    CHECK(get_image_format_properties(
              physical_device, VK_FORMAT_UNDEFINED, VK_IMAGE_TYPE_2D,
              VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT, 0U,
              &image_format_properties) == VK_ERROR_FORMAT_NOT_SUPPORTED);
    CHECK(image_format_properties.maxExtent.width == 0U);

    VkPhysicalDeviceVulkan11Features vulkan11_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
    };
    VkPhysicalDeviceVulkan12Features vulkan12_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
    };
    VkPhysicalDeviceVulkan13Features vulkan13_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
    };
    VkPhysicalDeviceDepthClipEnableFeaturesEXT depth_clip_features = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT,
    };
    VkPhysicalDeviceRobustness2FeaturesEXT robustness2_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
    };
    VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5_features = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR,
    };
    VkPhysicalDeviceMaintenance6FeaturesKHR maintenance6_features = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR,
    };
    vulkan11_features.pNext = &vulkan12_features;
    vulkan12_features.pNext = &vulkan13_features;
    vulkan13_features.pNext = &depth_clip_features;
    depth_clip_features.pNext = &robustness2_features;
    robustness2_features.pNext = &maintenance5_features;
    maintenance5_features.pNext = &maintenance6_features;
    VkPhysicalDeviceFeatures2 features2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &vulkan11_features,
    };
    get_features2(physical_device, &features2);
    CHECK(features2.features.samplerAnisotropy == VK_TRUE);
    CHECK(vulkan11_features.shaderDrawParameters == VK_TRUE);
    CHECK(vulkan12_features.bufferDeviceAddress == VK_TRUE);
    CHECK(vulkan12_features.descriptorIndexing == VK_TRUE);
    CHECK(vulkan12_features.descriptorBindingSampledImageUpdateAfterBind ==
          VK_TRUE);
    CHECK(vulkan12_features.descriptorBindingUpdateUnusedWhilePending ==
          VK_TRUE);
    CHECK(vulkan12_features.descriptorBindingPartiallyBound == VK_TRUE);
    CHECK(vulkan12_features.hostQueryReset == VK_TRUE);
    CHECK(vulkan12_features.runtimeDescriptorArray == VK_TRUE);
    CHECK(vulkan12_features.samplerMirrorClampToEdge == VK_TRUE);
    CHECK(vulkan12_features.scalarBlockLayout == VK_TRUE);
    CHECK(vulkan12_features.timelineSemaphore == VK_TRUE);
    CHECK(vulkan12_features.uniformBufferStandardLayout == VK_TRUE);
    CHECK(vulkan12_features.vulkanMemoryModel == VK_TRUE);
    CHECK(vulkan13_features.computeFullSubgroups == VK_TRUE);
    CHECK(vulkan13_features.dynamicRendering == VK_TRUE);
    CHECK(vulkan13_features.maintenance4 == VK_TRUE);
    CHECK(vulkan13_features.shaderDemoteToHelperInvocation == VK_TRUE);
    CHECK(vulkan13_features.shaderZeroInitializeWorkgroupMemory == VK_TRUE);
    CHECK(vulkan13_features.subgroupSizeControl == VK_TRUE);
    CHECK(vulkan13_features.synchronization2 == VK_TRUE);
    CHECK(depth_clip_features.depthClipEnable == VK_TRUE);
    CHECK(robustness2_features.robustBufferAccess2 == VK_TRUE);
    CHECK(robustness2_features.nullDescriptor == VK_TRUE);
    CHECK(maintenance5_features.maintenance5 == VK_TRUE);
    CHECK(maintenance6_features.maintenance6 == VK_TRUE);
    VkPhysicalDeviceProperties2 properties2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
    };
    get_properties2(physical_device, &properties2);
    CHECK(properties2.properties.vendorID == properties.vendorID);
    VkFormatProperties2 format_properties2 = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
    };
    get_format_properties2(
        physical_device, VK_FORMAT_R8G8B8A8_UNORM, &format_properties2);
    CHECK(format_properties2.formatProperties.optimalTilingFeatures ==
          format_properties.optimalTilingFeatures);
    const VkPhysicalDeviceImageFormatInfo2 image_info2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
    };
    VkImageFormatProperties2 image_properties2 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
    };
    CHECK(get_image_properties2(
              physical_device, &image_info2, &image_properties2) ==
          VK_SUCCESS);
    CHECK(image_properties2.imageFormatProperties.maxExtent.width == 4096U);
    const VkPhysicalDeviceExternalBufferInfo external_buffer_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    VkExternalBufferProperties external_buffer_properties = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES,
    };
    get_external_buffer(
        physical_device, &external_buffer_info, &external_buffer_properties);
    CHECK((external_buffer_properties.externalMemoryProperties
               .externalMemoryFeatures &
           VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0U);
    CHECK((external_buffer_properties.externalMemoryProperties
               .externalMemoryFeatures &
           VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0U);
    CHECK((external_buffer_properties.externalMemoryProperties
               .compatibleHandleTypes &
           VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) != 0U);
    const VkPhysicalDeviceExternalSemaphoreInfo external_semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    VkExternalSemaphoreProperties external_semaphore_properties = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
    };
    get_external_semaphore(
        physical_device, &external_semaphore_info,
        &external_semaphore_properties);
    CHECK((external_semaphore_properties.externalSemaphoreFeatures &
           VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) != 0U);
    CHECK((external_semaphore_properties.externalSemaphoreFeatures &
           VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) != 0U);
    CHECK((external_semaphore_properties.compatibleHandleTypes &
           VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT) != 0U);
    const VkPhysicalDeviceSparseImageFormatInfo2 sparse_info2 = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SPARSE_IMAGE_FORMAT_INFO_2,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .type = VK_IMAGE_TYPE_2D,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
    };
    uint32_t sparse_count = 99U;
    get_sparse_properties2(
        physical_device, &sparse_info2, &sparse_count, NULL);
    CHECK(sparse_count == 0U);

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
    uint32_t queue2_count = available_queue_count;
    VkQueueFamilyProperties2 *queues2 =
        calloc(queue2_count, sizeof(*queues2));
    CHECK(queues2 != NULL);
    for (uint32_t index = 0U; index < queue2_count; ++index) {
        queues2[index].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
    }
    get_queue_properties2(physical_device, &queue2_count, queues2);
    CHECK(queue2_count == available_queue_count);
    CHECK(queues2[queue_family_index].queueFamilyProperties.queueCount > 0U);
    free(queues2);
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
    VkPhysicalDeviceMemoryProperties2 memory2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
    };
    get_memory_properties2(physical_device, &memory2);
    CHECK(memory2.memoryProperties.memoryTypeCount == memory.memoryTypeCount);
    CHECK(memory2.memoryProperties.memoryHeapCount == memory.memoryHeapCount);

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
    bool swapchain_extension_found = false;
    for (uint32_t index = 0U; index < device_extension_count; ++index) {
        CHECK(device_extensions[index].extensionName[0] != '\0');
        swapchain_extension_found |=
            strcmp(device_extensions[index].extensionName,
                   VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
    }
    CHECK(swapchain_extension_found);
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
    const char *enabled_device_extension = "VK_KHR_swapchain";
    device_create_info.enabledExtensionCount = 1U;
    device_create_info.ppEnabledExtensionNames = &enabled_device_extension;
    device_create_info.pNext = &loader_private_chain_marker;
    VkDevice device = VK_NULL_HANDLE;
    CHECK(create_device(physical_device, &device_create_info, NULL, &device) ==
          VK_SUCCESS);
    CHECK(device != VK_NULL_HANDLE);
    const uint64_t device_id = bvb_device_proxy_id(device);
    CHECK(bvb_handle_type(device_id) == BVB_OBJECT_DEVICE);
    CHECK(bvb_handle_serial(device_id) == 1U);
    VkDevice ownership_device = VK_NULL_HANDLE;
    CHECK(create_device(
              physical_device, &device_create_info, NULL,
              &ownership_device) == VK_SUCCESS);
    CHECK(ownership_device != VK_NULL_HANDLE);

    PFN_vkGetDeviceQueue get_device_queue = NULL;
    PFN_vkDestroyDevice destroy_device = NULL;
    PFN_vkQueueSubmit queue_submit = NULL;
    PFN_vkQueueSubmit2 queue_submit_2 = NULL;
    PFN_vkQueueWaitIdle queue_wait_idle = NULL;
    PFN_vkDeviceWaitIdle device_wait_idle = NULL;
    PFN_vkCreateCommandPool create_command_pool = NULL;
    PFN_vkDestroyCommandPool destroy_command_pool = NULL;
    PFN_vkResetCommandPool reset_command_pool = NULL;
    PFN_vkAllocateCommandBuffers allocate_command_buffers = NULL;
    PFN_vkFreeCommandBuffers free_command_buffers = NULL;
    PFN_vkBeginCommandBuffer begin_command_buffer = NULL;
    PFN_vkEndCommandBuffer end_command_buffer = NULL;
    PFN_vkCreateBuffer create_buffer = NULL;
    PFN_vkDestroyBuffer destroy_buffer = NULL;
    PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements = NULL;
    PFN_vkGetBufferMemoryRequirements2 get_buffer_memory_requirements_2 = NULL;
    PFN_vkGetDeviceBufferMemoryRequirements
        get_device_buffer_memory_requirements = NULL;
    PFN_vkGetBufferDeviceAddress get_buffer_device_address = NULL;
    PFN_vkAllocateMemory allocate_memory = NULL;
    PFN_vkFreeMemory free_memory = NULL;
    PFN_vkBindBufferMemory bind_buffer_memory = NULL;
    PFN_vkCreateImage create_image = NULL;
    PFN_vkDestroyImage destroy_image = NULL;
    PFN_vkGetImageMemoryRequirements get_image_memory_requirements = NULL;
    PFN_vkGetImageMemoryRequirements2 get_image_memory_requirements_2 = NULL;
    PFN_vkBindImageMemory bind_image_memory = NULL;
    PFN_vkCreateImageView create_image_view = NULL;
    PFN_vkDestroyImageView destroy_image_view = NULL;
    PFN_vkMapMemory map_memory = NULL;
    PFN_vkUnmapMemory unmap_memory = NULL;
    PFN_vkFlushMappedMemoryRanges flush_mapped_memory_ranges = NULL;
    PFN_vkInvalidateMappedMemoryRanges invalidate_mapped_memory_ranges = NULL;
    PFN_vkCmdFillBuffer cmd_fill_buffer = NULL;
    PFN_vkCreateFence create_fence = NULL;
    PFN_vkDestroyFence destroy_fence = NULL;
    PFN_vkGetFenceStatus get_fence_status = NULL;
    PFN_vkWaitForFences wait_for_fences = NULL;
    PFN_vkResetFences reset_fences = NULL;
    PFN_vkCreateSemaphore create_semaphore = NULL;
    PFN_vkDestroySemaphore destroy_semaphore = NULL;
    PFN_vkGetSemaphoreCounterValue get_semaphore_counter = NULL;
    PFN_vkWaitSemaphores wait_semaphores = NULL;
    PFN_vkSignalSemaphore signal_semaphore = NULL;
    PFN_vkCreateDescriptorSetLayout create_descriptor_set_layout = NULL;
    PFN_vkDestroyDescriptorSetLayout destroy_descriptor_set_layout = NULL;
    PFN_vkCreateDescriptorPool create_descriptor_pool = NULL;
    PFN_vkDestroyDescriptorPool destroy_descriptor_pool = NULL;
    PFN_vkAllocateDescriptorSets allocate_descriptor_sets = NULL;
    PFN_vkCreateSampler create_sampler = NULL;
    PFN_vkDestroySampler destroy_sampler = NULL;
    PFN_vkUpdateDescriptorSets update_descriptor_sets = NULL;
    PFN_vkCreatePipelineLayout create_pipeline_layout = NULL;
    PFN_vkDestroyPipelineLayout destroy_pipeline_layout = NULL;
    PFN_vkCreateGraphicsPipelines create_graphics_pipelines = NULL;
    PFN_vkDestroyPipeline destroy_pipeline = NULL;
    erased = vkGetDeviceProcAddr(device, "vkGetDeviceQueue");
    CHECK(erased != NULL);
    memcpy(&get_device_queue, &erased, sizeof(get_device_queue));
    erased = vkGetDeviceProcAddr(device, "vkDestroyDevice");
    CHECK(erased != NULL);
    memcpy(&destroy_device, &erased, sizeof(destroy_device));
    erased = vkGetDeviceProcAddr(device, "vkQueueSubmit");
    CHECK(erased != NULL);
    memcpy(&queue_submit, &erased, sizeof(queue_submit));
    erased = vkGetDeviceProcAddr(device, "vkQueueSubmit2");
    CHECK(erased != NULL);
    memcpy(&queue_submit_2, &erased, sizeof(queue_submit_2));
    CHECK(vkGetDeviceProcAddr(device, "vkQueueSubmit2KHR") == erased);
    erased = vkGetDeviceProcAddr(device, "vkQueueWaitIdle");
    CHECK(erased != NULL);
    memcpy(&queue_wait_idle, &erased, sizeof(queue_wait_idle));
    erased = vkGetDeviceProcAddr(device, "vkDeviceWaitIdle");
    CHECK(erased != NULL);
    memcpy(&device_wait_idle, &erased, sizeof(device_wait_idle));
    erased = vkGetDeviceProcAddr(device, "vkCreateCommandPool");
    CHECK(erased != NULL);
    memcpy(&create_command_pool, &erased, sizeof(create_command_pool));
    erased = vkGetDeviceProcAddr(device, "vkDestroyCommandPool");
    CHECK(erased != NULL);
    memcpy(&destroy_command_pool, &erased, sizeof(destroy_command_pool));
    erased = vkGetDeviceProcAddr(device, "vkResetCommandPool");
    CHECK(erased != NULL);
    memcpy(&reset_command_pool, &erased, sizeof(reset_command_pool));
    erased = vkGetDeviceProcAddr(device, "vkAllocateCommandBuffers");
    CHECK(erased != NULL);
    memcpy(&allocate_command_buffers, &erased,
           sizeof(allocate_command_buffers));
    erased = vkGetDeviceProcAddr(device, "vkFreeCommandBuffers");
    CHECK(erased != NULL);
    memcpy(&free_command_buffers, &erased, sizeof(free_command_buffers));
    erased = vkGetDeviceProcAddr(device, "vkBeginCommandBuffer");
    CHECK(erased != NULL);
    memcpy(&begin_command_buffer, &erased, sizeof(begin_command_buffer));
    erased = vkGetDeviceProcAddr(device, "vkEndCommandBuffer");
    CHECK(erased != NULL);
    memcpy(&end_command_buffer, &erased, sizeof(end_command_buffer));
    erased = vkGetDeviceProcAddr(device, "vkCreateBuffer");
    CHECK(erased != NULL);
    memcpy(&create_buffer, &erased, sizeof(create_buffer));
    erased = vkGetDeviceProcAddr(device, "vkDestroyBuffer");
    CHECK(erased != NULL);
    memcpy(&destroy_buffer, &erased, sizeof(destroy_buffer));
    erased = vkGetDeviceProcAddr(device, "vkGetBufferMemoryRequirements");
    CHECK(erased != NULL);
    memcpy(&get_buffer_memory_requirements, &erased,
           sizeof(get_buffer_memory_requirements));
    erased = vkGetDeviceProcAddr(device, "vkGetBufferMemoryRequirements2");
    CHECK(erased != NULL);
    memcpy(&get_buffer_memory_requirements_2, &erased,
           sizeof(get_buffer_memory_requirements_2));
    CHECK(vkGetDeviceProcAddr(
              device, "vkGetBufferMemoryRequirements2KHR") == NULL);
    erased = vkGetDeviceProcAddr(
        device, "vkGetDeviceBufferMemoryRequirements");
    CHECK(erased != NULL);
    memcpy(&get_device_buffer_memory_requirements, &erased,
           sizeof(get_device_buffer_memory_requirements));
    CHECK(vkGetDeviceProcAddr(
              device, "vkGetDeviceBufferMemoryRequirementsKHR") == NULL);
    erased = vkGetDeviceProcAddr(device, "vkGetBufferDeviceAddress");
    CHECK(erased != NULL);
    memcpy(&get_buffer_device_address, &erased,
           sizeof(get_buffer_device_address));
    CHECK(vkGetDeviceProcAddr(device, "vkGetBufferDeviceAddressKHR") ==
          NULL);
    CHECK(vkGetDeviceProcAddr(device, "vkGetBufferDeviceAddressEXT") ==
          NULL);
    erased = vkGetDeviceProcAddr(device, "vkAllocateMemory");
    CHECK(erased != NULL);
    memcpy(&allocate_memory, &erased, sizeof(allocate_memory));
    erased = vkGetDeviceProcAddr(device, "vkFreeMemory");
    CHECK(erased != NULL);
    memcpy(&free_memory, &erased, sizeof(free_memory));
    erased = vkGetDeviceProcAddr(device, "vkBindBufferMemory");
    CHECK(erased != NULL);
    memcpy(&bind_buffer_memory, &erased, sizeof(bind_buffer_memory));
    erased = vkGetDeviceProcAddr(device, "vkCreateImage");
    CHECK(erased != NULL);
    memcpy(&create_image, &erased, sizeof(create_image));
    erased = vkGetDeviceProcAddr(device, "vkDestroyImage");
    CHECK(erased != NULL);
    memcpy(&destroy_image, &erased, sizeof(destroy_image));
    erased = vkGetDeviceProcAddr(device, "vkGetImageMemoryRequirements");
    CHECK(erased != NULL);
    memcpy(&get_image_memory_requirements, &erased,
           sizeof(get_image_memory_requirements));
    erased = vkGetDeviceProcAddr(device, "vkGetImageMemoryRequirements2");
    CHECK(erased != NULL);
    memcpy(&get_image_memory_requirements_2, &erased,
           sizeof(get_image_memory_requirements_2));
    erased = vkGetDeviceProcAddr(device, "vkBindImageMemory");
    CHECK(erased != NULL);
    memcpy(&bind_image_memory, &erased, sizeof(bind_image_memory));
    erased = vkGetDeviceProcAddr(device, "vkCreateImageView");
    CHECK(erased != NULL);
    memcpy(&create_image_view, &erased, sizeof(create_image_view));
    erased = vkGetDeviceProcAddr(device, "vkDestroyImageView");
    CHECK(erased != NULL);
    memcpy(&destroy_image_view, &erased, sizeof(destroy_image_view));
    erased = vkGetDeviceProcAddr(device, "vkMapMemory");
    CHECK(erased != NULL);
    memcpy(&map_memory, &erased, sizeof(map_memory));
    erased = vkGetDeviceProcAddr(device, "vkUnmapMemory");
    CHECK(erased != NULL);
    memcpy(&unmap_memory, &erased, sizeof(unmap_memory));
    erased = vkGetDeviceProcAddr(device, "vkFlushMappedMemoryRanges");
    CHECK(erased != NULL);
    memcpy(&flush_mapped_memory_ranges, &erased,
           sizeof(flush_mapped_memory_ranges));
    erased = vkGetDeviceProcAddr(device, "vkInvalidateMappedMemoryRanges");
    CHECK(erased != NULL);
    memcpy(&invalidate_mapped_memory_ranges, &erased,
           sizeof(invalidate_mapped_memory_ranges));
    erased = vkGetDeviceProcAddr(device, "vkCmdFillBuffer");
    CHECK(erased != NULL);
    memcpy(&cmd_fill_buffer, &erased, sizeof(cmd_fill_buffer));
    erased = vkGetDeviceProcAddr(device, "vkCreateFence");
    CHECK(erased != NULL);
    memcpy(&create_fence, &erased, sizeof(create_fence));
    erased = vkGetDeviceProcAddr(device, "vkDestroyFence");
    CHECK(erased != NULL);
    memcpy(&destroy_fence, &erased, sizeof(destroy_fence));
    erased = vkGetDeviceProcAddr(device, "vkGetFenceStatus");
    CHECK(erased != NULL);
    memcpy(&get_fence_status, &erased, sizeof(get_fence_status));
    erased = vkGetDeviceProcAddr(device, "vkWaitForFences");
    CHECK(erased != NULL);
    memcpy(&wait_for_fences, &erased, sizeof(wait_for_fences));
    erased = vkGetDeviceProcAddr(device, "vkResetFences");
    CHECK(erased != NULL);
    memcpy(&reset_fences, &erased, sizeof(reset_fences));
    erased = vkGetDeviceProcAddr(device, "vkCreateSemaphore");
    CHECK(erased != NULL);
    memcpy(&create_semaphore, &erased, sizeof(create_semaphore));
    erased = vkGetDeviceProcAddr(device, "vkDestroySemaphore");
    CHECK(erased != NULL);
    memcpy(&destroy_semaphore, &erased, sizeof(destroy_semaphore));
    erased = vkGetDeviceProcAddr(device, "vkGetSemaphoreCounterValue");
    CHECK(erased != NULL);
    memcpy(&get_semaphore_counter, &erased, sizeof(get_semaphore_counter));
    CHECK(vkGetDeviceProcAddr(device, "vkGetSemaphoreCounterValueKHR") ==
          erased);
    erased = vkGetDeviceProcAddr(device, "vkWaitSemaphores");
    CHECK(erased != NULL);
    memcpy(&wait_semaphores, &erased, sizeof(wait_semaphores));
    CHECK(vkGetDeviceProcAddr(device, "vkWaitSemaphoresKHR") == erased);
    erased = vkGetDeviceProcAddr(device, "vkSignalSemaphore");
    CHECK(erased != NULL);
    memcpy(&signal_semaphore, &erased, sizeof(signal_semaphore));
    CHECK(vkGetDeviceProcAddr(device, "vkSignalSemaphoreKHR") == erased);
#define RESOLVE_DESCRIPTOR(entry_name, variable)                             \
    do {                                                                      \
        erased = vkGetDeviceProcAddr(device, #entry_name);                   \
        CHECK(erased != NULL);                                                \
        memcpy(&(variable), &erased, sizeof(variable));                       \
    } while (0)
    RESOLVE_DESCRIPTOR(vkCreateDescriptorSetLayout,
                       create_descriptor_set_layout);
    RESOLVE_DESCRIPTOR(vkDestroyDescriptorSetLayout,
                       destroy_descriptor_set_layout);
    RESOLVE_DESCRIPTOR(vkCreateDescriptorPool, create_descriptor_pool);
    RESOLVE_DESCRIPTOR(vkDestroyDescriptorPool, destroy_descriptor_pool);
    RESOLVE_DESCRIPTOR(vkAllocateDescriptorSets, allocate_descriptor_sets);
    RESOLVE_DESCRIPTOR(vkCreateSampler, create_sampler);
    RESOLVE_DESCRIPTOR(vkDestroySampler, destroy_sampler);
    RESOLVE_DESCRIPTOR(vkUpdateDescriptorSets, update_descriptor_sets);
    RESOLVE_DESCRIPTOR(vkCreatePipelineLayout, create_pipeline_layout);
    RESOLVE_DESCRIPTOR(vkDestroyPipelineLayout, destroy_pipeline_layout);
    RESOLVE_DESCRIPTOR(vkCreateGraphicsPipelines, create_graphics_pipelines);
    RESOLVE_DESCRIPTOR(vkDestroyPipeline, destroy_pipeline);
#undef RESOLVE_DESCRIPTOR
    CHECK(vkGetDeviceProcAddr(device, "vkCmdDraw") != NULL);
    PFN_vkCreateSwapchainKHR create_swapchain = NULL;
    PFN_vkDestroySwapchainKHR destroy_swapchain = NULL;
    PFN_vkGetSwapchainImagesKHR get_swapchain_images = NULL;
    PFN_vkAcquireNextImageKHR acquire_next_image = NULL;
    PFN_vkAcquireNextImage2KHR acquire_next_image2 = NULL;
    PFN_vkQueuePresentKHR queue_present = NULL;
#define RESOLVE_WSI(entry_name, variable)                                    \
    do {                                                                      \
        erased = vkGetDeviceProcAddr(device, #entry_name);                   \
        CHECK(erased != NULL);                                                \
        memcpy(&(variable), &erased, sizeof(variable));                       \
    } while (0)
    RESOLVE_WSI(vkCreateSwapchainKHR, create_swapchain);
    RESOLVE_WSI(vkDestroySwapchainKHR, destroy_swapchain);
    RESOLVE_WSI(vkGetSwapchainImagesKHR, get_swapchain_images);
    RESOLVE_WSI(vkAcquireNextImageKHR, acquire_next_image);
    RESOLVE_WSI(vkAcquireNextImage2KHR, acquire_next_image2);
    RESOLVE_WSI(vkQueuePresentKHR, queue_present);
#undef RESOLVE_WSI
    const VkSwapchainCreateInfoKHR swapchain_create_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = 2U,
        .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = {2800U, 1752U},
        .imageArrayLayers = 1U,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    VkSwapchainKHR virtual_swapchain = VK_NULL_HANDLE;
    CHECK(create_swapchain(device, &swapchain_create_info, NULL,
                           &virtual_swapchain) == VK_SUCCESS);
    CHECK(virtual_swapchain != VK_NULL_HANDLE);
    CHECK(bvb_handle_type((uint64_t)virtual_swapchain) ==
          BVB_OBJECT_SWAPCHAIN);
    uint32_t virtual_image_count = 0U;
    CHECK(get_swapchain_images(device, virtual_swapchain,
                               &virtual_image_count, NULL) == VK_SUCCESS);
    CHECK(virtual_image_count == 3U);
    VkImage virtual_images[3] = {0};
    CHECK(get_swapchain_images(device, virtual_swapchain,
                               &virtual_image_count, virtual_images) ==
          VK_SUCCESS);
    CHECK(virtual_image_count == 3U);
    for (uint32_t index = 0U; index < virtual_image_count; ++index)
        CHECK(bvb_handle_type((uint64_t)virtual_images[index]) ==
              BVB_OBJECT_IMAGE);
    const VkSemaphoreCreateInfo binary_semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    VkSemaphore acquire_semaphore = VK_NULL_HANDLE;
    CHECK(create_semaphore(device, &binary_semaphore_info, NULL,
                           &acquire_semaphore) == VK_SUCCESS);
    uint32_t virtual_image_index = UINT32_MAX;
    CHECK(acquire_next_image(device, virtual_swapchain, UINT64_MAX,
                             acquire_semaphore, VK_NULL_HANDLE,
                             &virtual_image_index) == VK_SUCCESS);
    CHECK(virtual_image_index < virtual_image_count);
    const VkAcquireNextImageInfoKHR unsupported_acquire_info = {
        .sType = VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR,
        .swapchain = virtual_swapchain,
        .semaphore = acquire_semaphore,
        .deviceMask = 0U,
    };
    CHECK(acquire_next_image2(device, &unsupported_acquire_info,
                              &virtual_image_index) ==
          VK_ERROR_INITIALIZATION_FAILED);
    VkQueue queue = VK_NULL_HANDLE;
    get_device_queue(device, queue_family_index, 0U, &queue);
    CHECK(queue != VK_NULL_HANDLE);
    const uint64_t queue_id = bvb_queue_proxy_id(queue);
    CHECK(bvb_handle_type(queue_id) == BVB_OBJECT_QUEUE);
    CHECK(bvb_handle_serial(queue_id) == 1U);
    VkQueue repeated_queue = VK_NULL_HANDLE;
    get_device_queue(device, queue_family_index, 0U, &repeated_queue);
    CHECK(repeated_queue == queue);
    VkResult per_swapchain_result = VK_ERROR_UNKNOWN;
    const VkPresentInfoKHR virtual_present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1U,
        .pWaitSemaphores = &acquire_semaphore,
        .swapchainCount = 1U,
        .pSwapchains = &virtual_swapchain,
        .pImageIndices = &virtual_image_index,
        .pResults = &per_swapchain_result,
    };
    CHECK(queue_present(queue, &virtual_present) == VK_SUCCESS);
    CHECK(per_swapchain_result == VK_SUCCESS);
    destroy_swapchain(device, virtual_swapchain, NULL);
    destroy_semaphore(device, acquire_semaphore, NULL);
    CHECK(queue_submit(queue, 0U, NULL, VK_NULL_HANDLE) == VK_SUCCESS);
    const VkSubmitInfo unsupported_submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    };
    CHECK(queue_submit(queue, 1U, &unsupported_submit, VK_NULL_HANDLE) ==
          VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(queue_wait_idle(queue) == VK_SUCCESS);
    CHECK(device_wait_idle(device) == VK_SUCCESS);

    const VkDescriptorBindingFlags sampler_binding_flags =
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT |
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    const VkDescriptorSetLayoutBindingFlagsCreateInfo layout_flags_info = {
        .sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = 1U,
        .pBindingFlags = &sampler_binding_flags,
    };
    const VkDescriptorSetLayoutBinding sampler_binding = {
        .binding = 0U,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
        .descriptorCount = 4096U,
        .stageFlags =
            VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT,
    };
    const VkDescriptorSetLayoutCreateInfo descriptor_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = &layout_flags_info,
        .flags =
            VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
        .bindingCount = 1U,
        .pBindings = &sampler_binding,
    };
    VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
    const VkAllocationCallbacks unsupported_allocator = {0};
    CHECK(create_descriptor_set_layout(
              device, &descriptor_layout_info, &unsupported_allocator,
              &descriptor_layout) == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(descriptor_layout == VK_NULL_HANDLE);
    CHECK(create_descriptor_set_layout(
              device, &descriptor_layout_info, NULL,
              &descriptor_layout) == VK_SUCCESS);
    uint64_t descriptor_layout_id = 0U;
    memcpy(&descriptor_layout_id, &descriptor_layout,
           sizeof(descriptor_layout));
    CHECK(bvb_handle_type(descriptor_layout_id) ==
          BVB_OBJECT_DESCRIPTOR_SET_LAYOUT);

    const VkDescriptorPoolSize descriptor_pool_size = {
        .type = VK_DESCRIPTOR_TYPE_SAMPLER,
        .descriptorCount = 4096U,
    };
    const VkDescriptorPoolCreateInfo descriptor_pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
        .maxSets = 1U,
        .poolSizeCount = 1U,
        .pPoolSizes = &descriptor_pool_size,
    };
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    CHECK(create_descriptor_pool(
              device, &descriptor_pool_info, NULL,
              &descriptor_pool) == VK_SUCCESS);
    uint64_t descriptor_pool_id = 0U;
    memcpy(&descriptor_pool_id, &descriptor_pool, sizeof(descriptor_pool));
    CHECK(bvb_handle_type(descriptor_pool_id) ==
          BVB_OBJECT_DESCRIPTOR_POOL);

    const VkDescriptorSetAllocateInfo descriptor_allocate_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool,
        .descriptorSetCount = 1U,
        .pSetLayouts = &descriptor_layout,
    };
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    CHECK(allocate_descriptor_sets(
              device, &descriptor_allocate_info,
              &descriptor_set) == VK_SUCCESS);
    uint64_t descriptor_set_id = 0U;
    memcpy(&descriptor_set_id, &descriptor_set, sizeof(descriptor_set));
    CHECK(bvb_handle_type(descriptor_set_id) == BVB_OBJECT_DESCRIPTOR_SET);

    VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        .mipLodBias = 0.25F,
        .anisotropyEnable = VK_TRUE,
        .maxAnisotropy = 8.0F,
        .compareEnable = VK_TRUE,
        .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
        .minLod = 0.0F,
        .maxLod = 12.0F,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
    };
    const uint32_t unsupported_sampler_chain = UINT32_C(0x7ffffffe);
    sampler_info.pNext = &unsupported_sampler_chain;
    VkSampler sampler = VK_NULL_HANDLE;
    CHECK(create_sampler(device, &sampler_info, NULL, &sampler) ==
          VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(sampler == VK_NULL_HANDLE);
    sampler_info.pNext = NULL;
    CHECK(create_sampler(device, &sampler_info, NULL, &sampler) == VK_SUCCESS);
    uint64_t sampler_id = 0U;
    memcpy(&sampler_id, &sampler, sizeof(sampler));
    CHECK(bvb_handle_type(sampler_id) == BVB_OBJECT_SAMPLER);

    const VkDescriptorImageInfo sampler_descriptor = {
        .sampler = sampler,
        .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VkWriteDescriptorSet sampler_write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = descriptor_set,
        .dstBinding = 0U,
        .dstArrayElement = 7U,
        .descriptorCount = 1U,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
        .pImageInfo = &sampler_descriptor,
    };
    update_descriptor_sets(device, 1U, &sampler_write, 0U, NULL);

    const VkDescriptorSetLayoutCreateInfo empty_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    };
    VkDescriptorSetLayout empty_layout = VK_NULL_HANDLE;
    CHECK(create_descriptor_set_layout(
              device, &empty_layout_info, NULL, &empty_layout) == VK_SUCCESS);
    uint64_t empty_layout_id = 0U;
    memcpy(&empty_layout_id, &empty_layout, sizeof(empty_layout));
    CHECK(bvb_handle_type(empty_layout_id) ==
          BVB_OBJECT_DESCRIPTOR_SET_LAYOUT);

    const VkDescriptorSetLayout pipeline_set_layouts[3] = {
        descriptor_layout, empty_layout, VK_NULL_HANDLE,
    };
    const VkPushConstantRange pipeline_push_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
            VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0U,
        .size = 160U,
    };
    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT,
        .setLayoutCount = 3U,
        .pSetLayouts = pipeline_set_layouts,
        .pushConstantRangeCount = 1U,
        .pPushConstantRanges = &pipeline_push_range,
    };
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    CHECK(create_pipeline_layout(
              device, &pipeline_layout_info, &unsupported_allocator,
              &pipeline_layout) == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(pipeline_layout == VK_NULL_HANDLE);
    const uint32_t unsupported_pipeline_chain = UINT32_C(0x7ffffffd);
    pipeline_layout_info.pNext = &unsupported_pipeline_chain;
    CHECK(create_pipeline_layout(
              device, &pipeline_layout_info, NULL,
              &pipeline_layout) == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(pipeline_layout == VK_NULL_HANDLE);
    pipeline_layout_info.pNext = NULL;
    pipeline_layout_info.flags = 0U;
    CHECK(create_pipeline_layout(
              device, &pipeline_layout_info, NULL,
              &pipeline_layout) == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(pipeline_layout == VK_NULL_HANDLE);
    pipeline_layout_info.flags =
        VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT;
    CHECK(create_pipeline_layout(
              device, &pipeline_layout_info, NULL,
              &pipeline_layout) == VK_SUCCESS);
    uint64_t pipeline_layout_id = 0U;
    memcpy(&pipeline_layout_id, &pipeline_layout, sizeof(pipeline_layout));
    CHECK(bvb_handle_type(pipeline_layout_id) == BVB_OBJECT_PIPELINE_LAYOUT);

    const VkDynamicState null_fragment_dynamic_states[] = {
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP,
    };
    const VkPipelineRenderingCreateInfo rendering_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
    };
    const VkPipelineCreateFlags2CreateInfo pipeline_flags_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
        .pNext = &rendering_info,
        .flags = VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR,
    };
    const VkGraphicsPipelineLibraryCreateInfoEXT pipeline_library_info = {
        .sType =
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
        .pNext = &pipeline_flags_info,
        .flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT,
    };
    const VkShaderModuleCreateInfo embedded_module_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(test_dxvk_dummy_frag),
        .pCode = test_dxvk_dummy_frag,
    };
    const VkPipelineShaderStageCreateInfo fragment_stage_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .pNext = &embedded_module_info,
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
        .pName = "main",
    };
    const VkPipelineDepthStencilStateCreateInfo depth_stencil_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
    };
    VkPipelineDynamicStateCreateInfo dynamic_state_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount =
            (uint32_t)(sizeof(null_fragment_dynamic_states) /
                       sizeof(null_fragment_dynamic_states[0])),
        .pDynamicStates = null_fragment_dynamic_states,
    };
    VkGraphicsPipelineCreateInfo graphics_pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &pipeline_library_info,
        .stageCount = 1U,
        .pStages = &fragment_stage_info,
        .pDepthStencilState = &depth_stencil_info,
        .pDynamicState = &dynamic_state_info,
        .layout = pipeline_layout,
        .basePipelineIndex = -1,
    };
    VkPipeline graphics_pipeline = VK_NULL_HANDLE;
    CHECK(create_graphics_pipelines(
              device, VK_NULL_HANDLE, 1U, &graphics_pipeline_info,
              &unsupported_allocator,
              &graphics_pipeline) == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(graphics_pipeline == VK_NULL_HANDLE);
    const VkBaseInStructure unknown_graphics_chain = {
        .sType = (VkStructureType)UINT32_C(0x7ffffffc),
    };
    graphics_pipeline_info.pNext = &unknown_graphics_chain;
    CHECK(create_graphics_pipelines(
              device, VK_NULL_HANDLE, 1U, &graphics_pipeline_info, NULL,
              &graphics_pipeline) == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(graphics_pipeline == VK_NULL_HANDLE);
    graphics_pipeline_info.pNext = &pipeline_library_info;
    --dynamic_state_info.dynamicStateCount;
    CHECK(create_graphics_pipelines(
              device, VK_NULL_HANDLE, 1U, &graphics_pipeline_info, NULL,
              &graphics_pipeline) == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(graphics_pipeline == VK_NULL_HANDLE);
    ++dynamic_state_info.dynamicStateCount;
    CHECK(create_graphics_pipelines(
              device, VK_NULL_HANDLE, 1U, &graphics_pipeline_info, NULL,
              &graphics_pipeline) == VK_SUCCESS);
    uint64_t graphics_pipeline_id = 0U;
    memcpy(&graphics_pipeline_id, &graphics_pipeline,
           sizeof(graphics_pipeline));
    CHECK(bvb_handle_type(graphics_pipeline_id) == BVB_OBJECT_PIPELINE);
    destroy_pipeline(device, graphics_pipeline, NULL);
    destroy_pipeline_layout(device, pipeline_layout, NULL);
    destroy_descriptor_set_layout(device, empty_layout, NULL);
    destroy_sampler(device, sampler, NULL);
    destroy_descriptor_pool(device, descriptor_pool, NULL);
    destroy_descriptor_set_layout(device, descriptor_layout, NULL);
    CHECK(device_wait_idle(device) == VK_SUCCESS);

    const VkCommandPoolCreateInfo pool_create_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = queue_family_index,
    };
    VkCommandPool command_pool = VK_NULL_HANDLE;
    CHECK(create_command_pool(
              device, &pool_create_info, NULL, &command_pool) == VK_SUCCESS);
    CHECK(command_pool != VK_NULL_HANDLE);
    const uint64_t command_pool_id =
        bvb_command_pool_proxy_id(command_pool);
    CHECK(bvb_handle_type(command_pool_id) == BVB_OBJECT_COMMAND_POOL);
    CHECK(bvb_handle_serial(command_pool_id) == 1U);
    const VkCommandBufferAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1U,
    };
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    CHECK(allocate_command_buffers(
              device, &allocate_info, &command_buffer) == VK_SUCCESS);
    CHECK(command_buffer != VK_NULL_HANDLE);
    const uint64_t command_buffer_id =
        bvb_command_buffer_proxy_id(command_buffer);
    CHECK(bvb_handle_type(command_buffer_id) == BVB_OBJECT_COMMAND_BUFFER);
    CHECK(bvb_handle_serial(command_buffer_id) == 1U);
    const VkBufferCreateInfo device_buffer_create_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 65536U,
        .usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VkDeviceBufferMemoryRequirements device_buffer_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS,
        .pCreateInfo = &device_buffer_create_info,
    };
    VkMemoryDedicatedRequirements device_buffer_dedicated_requirements = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
    };
    VkMemoryRequirements2 device_buffer_requirements = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = &device_buffer_dedicated_requirements,
    };
    get_device_buffer_memory_requirements(
        device, &device_buffer_info, &device_buffer_requirements);
    CHECK(device_buffer_requirements.memoryRequirements.size == 65792U);
    CHECK(device_buffer_requirements.memoryRequirements.alignment == 256U);
    CHECK(device_buffer_requirements.memoryRequirements.memoryTypeBits == 5U);
    CHECK(device_buffer_dedicated_requirements.prefersDedicatedAllocation ==
          VK_FALSE);
    CHECK(device_buffer_dedicated_requirements.requiresDedicatedAllocation ==
          VK_TRUE);
    const uint32_t unsupported_marker = 0U;
    VkDeviceBufferMemoryRequirements unsupported_device_buffer_info =
        device_buffer_info;
    unsupported_device_buffer_info.pNext = &unsupported_marker;
    device_buffer_requirements.memoryRequirements.size = UINT64_MAX;
    device_buffer_dedicated_requirements.prefersDedicatedAllocation = VK_TRUE;
    device_buffer_dedicated_requirements.requiresDedicatedAllocation =
        VK_TRUE;
    get_device_buffer_memory_requirements(
        device, &unsupported_device_buffer_info, &device_buffer_requirements);
    CHECK(device_buffer_requirements.memoryRequirements.size == 0U);
    CHECK(device_buffer_dedicated_requirements.prefersDedicatedAllocation ==
          VK_FALSE);
    CHECK(device_buffer_dedicated_requirements.requiresDedicatedAllocation ==
          VK_FALSE);
    const VkBufferCreateInfo buffer_create_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 4096U,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBufferCreateInfo oversized_buffer_info = buffer_create_info;
    oversized_buffer_info.size =
        (VkDeviceSize)BVB_VULKAN_MAX_MEMORY_ALLOCATION_SIZE + 1U;
    VkBuffer rejected_buffer = VK_NULL_HANDLE;
    CHECK(create_buffer(
              device, &oversized_buffer_info, NULL, &rejected_buffer) ==
          VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(rejected_buffer == VK_NULL_HANDLE);
    VkBuffer buffer = VK_NULL_HANDLE;
    CHECK(create_buffer(device, &buffer_create_info, NULL, &buffer) ==
          VK_SUCCESS);
    const uint64_t buffer_id = bvb_buffer_proxy_id(buffer);
    CHECK(bvb_handle_type(buffer_id) == BVB_OBJECT_BUFFER);
    CHECK(bvb_handle_serial(buffer_id) == 1U);
    const VkBufferMemoryRequirementsInfo2 buffer_requirements_info_2 = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
        .buffer = buffer,
    };
    VkMemoryRequirements2 buffer_requirements_2 = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
    };
    get_buffer_memory_requirements_2(
        device, &buffer_requirements_info_2, &buffer_requirements_2);
    CHECK(buffer_requirements_2.memoryRequirements.size == 4096U);
    CHECK(buffer_requirements_2.memoryRequirements.alignment == 256U);
    CHECK(buffer_requirements_2.memoryRequirements.memoryTypeBits == 1U);
    VkMemoryDedicatedRequirements buffer_dedicated_requirements = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
    };
    buffer_requirements_2.pNext = &buffer_dedicated_requirements;
    get_buffer_memory_requirements_2(
        device, &buffer_requirements_info_2, &buffer_requirements_2);
    CHECK(buffer_requirements_2.memoryRequirements.size == 4096U);
    CHECK(buffer_dedicated_requirements.prefersDedicatedAllocation ==
          VK_TRUE);
    CHECK(buffer_dedicated_requirements.requiresDedicatedAllocation ==
          VK_FALSE);
    VkApplicationInfo unsupported_buffer_requirements_tail = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    };
    buffer_dedicated_requirements.pNext =
        &unsupported_buffer_requirements_tail;
    buffer_dedicated_requirements.prefersDedicatedAllocation = VK_TRUE;
    buffer_dedicated_requirements.requiresDedicatedAllocation = VK_TRUE;
    buffer_requirements_2.memoryRequirements.size = UINT64_MAX;
    get_buffer_memory_requirements_2(
        device, &buffer_requirements_info_2, &buffer_requirements_2);
    CHECK(buffer_requirements_2.memoryRequirements.size == 0U);
    CHECK(buffer_dedicated_requirements.prefersDedicatedAllocation ==
          VK_FALSE);
    CHECK(buffer_dedicated_requirements.requiresDedicatedAllocation ==
          VK_FALSE);
    buffer_dedicated_requirements.pNext = NULL;
    VkBufferMemoryRequirementsInfo2 unsupported_buffer_requirements_info =
        buffer_requirements_info_2;
    unsupported_buffer_requirements_info.pNext =
        &unsupported_buffer_requirements_tail;
    buffer_requirements_2.memoryRequirements.size = UINT64_MAX;
    buffer_dedicated_requirements.prefersDedicatedAllocation = VK_TRUE;
    buffer_dedicated_requirements.requiresDedicatedAllocation = VK_TRUE;
    get_buffer_memory_requirements_2(
        device, &unsupported_buffer_requirements_info,
        &buffer_requirements_2);
    CHECK(buffer_requirements_2.memoryRequirements.size == 0U);
    CHECK(buffer_dedicated_requirements.prefersDedicatedAllocation ==
          VK_FALSE);
    CHECK(buffer_dedicated_requirements.requiresDedicatedAllocation ==
          VK_FALSE);
    buffer_requirements_2.memoryRequirements.size = UINT64_MAX;
    buffer_dedicated_requirements.prefersDedicatedAllocation = VK_TRUE;
    buffer_dedicated_requirements.requiresDedicatedAllocation = VK_TRUE;
    get_buffer_memory_requirements_2(
        ownership_device, &buffer_requirements_info_2,
        &buffer_requirements_2);
    CHECK(buffer_requirements_2.memoryRequirements.size == 0U);
    CHECK(buffer_dedicated_requirements.prefersDedicatedAllocation ==
          VK_FALSE);
    CHECK(buffer_dedicated_requirements.requiresDedicatedAllocation ==
          VK_FALSE);
    VkBufferDeviceAddressInfo prebind_address_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = buffer,
    };
    CHECK(get_buffer_device_address(device, &prebind_address_info) == 0U);
    VkMemoryRequirements requirements = {0};
    get_buffer_memory_requirements(device, buffer, &requirements);
    CHECK(requirements.size >= 4096U);
    CHECK(requirements.alignment != 0U);
    CHECK(requirements.memoryTypeBits != 0U);
    uint32_t memory_type_index = UINT32_MAX;
    for (uint32_t index = 0U; index < memory.memoryTypeCount; ++index) {
        const VkMemoryPropertyFlags flags =
            memory.memoryTypes[index].propertyFlags;
        if ((requirements.memoryTypeBits & (1U << index)) != 0U &&
            (flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            memory_type_index = index;
            break;
        }
    }
    CHECK(memory_type_index != UINT32_MAX);
    const VkMemoryAllocateFlagsInfo buffer_memory_flags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    const VkMemoryAllocateInfo memory_allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &buffer_memory_flags,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type_index,
    };
    VkDeviceMemory device_memory = VK_NULL_HANDLE;
    CHECK(allocate_memory(device, &memory_allocate_info, NULL, &device_memory) ==
          VK_SUCCESS);
    const uint64_t memory_id = bvb_memory_proxy_id(device_memory);
    CHECK(bvb_handle_type(memory_id) == BVB_OBJECT_DEVICE_MEMORY);
    CHECK(bvb_handle_serial(memory_id) == 1U);
    CHECK(bind_buffer_memory(device, buffer, device_memory, 0U) == VK_SUCCESS);
    VkBufferDeviceAddressInfo buffer_address_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = buffer,
    };
    const VkDeviceAddress buffer_device_address =
        get_buffer_device_address(device, &buffer_address_info);
    CHECK(buffer_device_address == UINT64_C(0x123456780000));
    CHECK(get_buffer_device_address(
              ownership_device, &buffer_address_info) == 0U);
    buffer_address_info.pNext = &unsupported_buffer_requirements_tail;
    CHECK(get_buffer_device_address(device, &buffer_address_info) == 0U);
    buffer_address_info.pNext = NULL;
    VkBuffer missing_buffer;
    const uint64_t missing_buffer_id = bvb_handle_id(
        BVB_OBJECT_BUFFER, UINT64_C(0xffff));
    memcpy(&missing_buffer, &missing_buffer_id, sizeof(missing_buffer));
    buffer_address_info.buffer = missing_buffer;
    CHECK(get_buffer_device_address(device, &buffer_address_info) == 0U);
    buffer_address_info.buffer = buffer;
    const VkImageStencilUsageCreateInfo stencil_usage = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO,
        .stencilUsage = VK_IMAGE_USAGE_SAMPLED_BIT,
    };
    const VkFormat view_format = VK_FORMAT_R8G8B8A8_UNORM;
    const VkImageFormatListCreateInfo format_list = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .pNext = &stencil_usage,
        .viewFormatCount = 1U,
        .pViewFormats = &view_format,
    };
    const VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &format_list,
        .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {64U, 64U, 1U},
        .mipLevels = 1U,
        .arrayLayers = 1U,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage image = VK_NULL_HANDLE;
    CHECK(create_image(device, &image_create_info, NULL, &image) == VK_SUCCESS);
    const uint64_t image_id = bvb_image_proxy_id(image);
    CHECK(bvb_handle_type(image_id) == BVB_OBJECT_IMAGE);
    CHECK(bvb_handle_serial(image_id) != 0U);
    VkMemoryRequirements image_requirements = {0};
    get_image_memory_requirements(device, image, &image_requirements);
    CHECK(image_requirements.size == 64U * 64U * sizeof(uint32_t));
    CHECK(image_requirements.alignment == 4096U);
    CHECK(image_requirements.memoryTypeBits == 1U);

    VkMemoryDedicatedRequirements dedicated_requirements = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
    };
    VkMemoryRequirements2 image_requirements_2 = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = &dedicated_requirements,
    };
    const VkImageMemoryRequirementsInfo2 requirements_info_2 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = image,
    };
    get_image_memory_requirements_2(
        device, &requirements_info_2, &image_requirements_2);
    CHECK(image_requirements_2.memoryRequirements.size == UINT64_C(19623936));
    CHECK(image_requirements_2.memoryRequirements.alignment == 4096U);
    CHECK(image_requirements_2.memoryRequirements.memoryTypeBits == 1U);
    CHECK(dedicated_requirements.prefersDedicatedAllocation == VK_TRUE);
    CHECK(dedicated_requirements.requiresDedicatedAllocation == VK_TRUE);

    const VkMemoryDedicatedAllocateInfo dedicated_allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = image,
    };
    const VkMemoryAllocateFlagsInfo allocate_flags_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .pNext = &dedicated_allocate_info,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    const VkMemoryAllocateInfo image_memory_allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &allocate_flags_info,
        .allocationSize = image_requirements_2.memoryRequirements.size,
        .memoryTypeIndex = memory_type_index,
    };
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    CHECK(allocate_memory(
              device, &image_memory_allocate_info, NULL, &image_memory) ==
          VK_SUCCESS);
    CHECK(bind_image_memory(device, image, image_memory, 0U) == VK_SUCCESS);

    VkMemoryAllocateInfo oversized_image_allocation =
        image_memory_allocate_info;
    oversized_image_allocation.pNext = NULL;
    oversized_image_allocation.allocationSize =
        (VkDeviceSize)BVB_VULKAN_MAX_MEMORY_ALLOCATION_SIZE + 1U;
    VkDeviceMemory rejected_memory = (VkDeviceMemory)(uintptr_t)1U;
    CHECK(allocate_memory(
              device, &oversized_image_allocation, NULL, &rejected_memory) ==
          VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(rejected_memory == VK_NULL_HANDLE);

    VkApplicationInfo unsupported_allocate_pnext = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    };
    VkMemoryAllocateInfo unsupported_image_allocation =
        image_memory_allocate_info;
    unsupported_image_allocation.pNext = &unsupported_allocate_pnext;
    rejected_memory = (VkDeviceMemory)(uintptr_t)1U;
    CHECK(allocate_memory(
              device, &unsupported_image_allocation, NULL,
              &rejected_memory) == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(rejected_memory == VK_NULL_HANDLE);

    VkMemoryDedicatedRequirements unsupported_requirements_pnext = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
        .pNext = &unsupported_allocate_pnext,
        .prefersDedicatedAllocation = VK_TRUE,
        .requiresDedicatedAllocation = VK_TRUE,
    };
    VkMemoryRequirements2 rejected_requirements = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = &unsupported_requirements_pnext,
        .memoryRequirements = {
            .size = 1U,
            .alignment = 1U,
            .memoryTypeBits = 1U,
        },
    };
    get_image_memory_requirements_2(
        device, &requirements_info_2, &rejected_requirements);
    CHECK(rejected_requirements.memoryRequirements.size == 0U);
    CHECK(rejected_requirements.memoryRequirements.alignment == 0U);
    CHECK(rejected_requirements.memoryRequirements.memoryTypeBits == 0U);
    CHECK(unsupported_requirements_pnext.prefersDedicatedAllocation ==
          VK_FALSE);
    CHECK(unsupported_requirements_pnext.requiresDedicatedAllocation ==
          VK_FALSE);
    const VkImageViewUsageCreateInfo image_view_usage = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
    };
    const VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = &image_view_usage,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .components = {
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1U,
            .layerCount = 1U,
        },
    };
    VkImageView image_view = VK_NULL_HANDLE;
    CHECK(create_image_view(
              device, &image_view_create_info, NULL, &image_view) ==
          VK_SUCCESS);
    const uint64_t image_view_id = bvb_image_view_proxy_id(image_view);
    CHECK(bvb_handle_type(image_view_id) == BVB_OBJECT_IMAGE_VIEW);
    CHECK(bvb_handle_serial(image_view_id) == 1U);

    const VkBaseInStructure unsupported_image_chain = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    };
    VkImageCreateInfo rejected_image_info = image_create_info;
    rejected_image_info.pNext = &unsupported_image_chain;
    VkImage rejected_image = (VkImage)(uintptr_t)1U;
    CHECK(create_image(device, &rejected_image_info, NULL, &rejected_image) ==
          VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(rejected_image == VK_NULL_HANDLE);
    const VkAllocationCallbacks unsupported_image_allocator = {0};
    CHECK(create_image(
              device, &image_create_info, &unsupported_image_allocator,
              &rejected_image) == VK_ERROR_FEATURE_NOT_PRESENT);
    VkImageViewCreateInfo rejected_view_info = image_view_create_info;
    rejected_view_info.subresourceRange.levelCount = 0U;
    VkImageView rejected_view = (VkImageView)(uintptr_t)1U;
    CHECK(create_image_view(
              device, &rejected_view_info, NULL, &rejected_view) ==
          VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(rejected_view == VK_NULL_HANDLE);
    uint8_t *mapped = NULL;
    CHECK(map_memory(device, device_memory, 0U, VK_WHOLE_SIZE, 0U,
                     (void **)&mapped) == VK_SUCCESS);
    CHECK(mapped != NULL);
    uint8_t expected_mapping[4096];
    for (size_t index = 0U; index < sizeof(expected_mapping); ++index) {
        expected_mapping[index] = (uint8_t)(index ^ (index >> 4));
    }
    memcpy(mapped, expected_mapping, sizeof(expected_mapping));
    const VkMappedMemoryRange mapped_range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = device_memory,
        .offset = 0U,
        .size = VK_WHOLE_SIZE,
    };
    CHECK(flush_mapped_memory_ranges(device, 1U, &mapped_range) ==
          VK_SUCCESS);
    memset(mapped, 0, sizeof(expected_mapping));
    CHECK(invalidate_mapped_memory_ranges(device, 1U, &mapped_range) ==
          VK_SUCCESS);
    uint32_t mapped_mismatches = 0U;
    for (size_t index = 0U; index < sizeof(expected_mapping); ++index) {
        if (mapped[index] != expected_mapping[index]) ++mapped_mismatches;
    }
    CHECK(mapped_mismatches == 0U);
    unmap_memory(device, device_memory);
    const VkFenceCreateInfo fence_create_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    VkFence fence = VK_NULL_HANDLE;
    CHECK(create_fence(device, &fence_create_info, NULL, &fence) == VK_SUCCESS);
    const uint64_t fence_id = bvb_fence_proxy_id(fence);
    CHECK(bvb_handle_type(fence_id) == BVB_OBJECT_FENCE);
    CHECK(bvb_handle_serial(fence_id) == 1U);
    CHECK(get_fence_status(device, fence) == VK_NOT_READY);
    const VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    CHECK(begin_command_buffer(command_buffer, &begin_info) == VK_SUCCESS);
    cmd_fill_buffer(command_buffer, buffer, 0U, 4096U, UINT32_C(0xa5c3f00d));
    CHECK(end_command_buffer(command_buffer) == VK_SUCCESS);
    const VkSubmitInfo command_submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1U,
        .pCommandBuffers = &command_buffer,
    };
    CHECK(queue_submit(queue, 1U, &command_submit, fence) ==
          VK_SUCCESS);
    CHECK(get_fence_status(device, fence) == VK_SUCCESS);
    CHECK(wait_for_fences(device, 1U, &fence, VK_TRUE, UINT64_MAX) ==
          VK_SUCCESS);
    uint32_t mismatched_words = UINT32_MAX;
    CHECK(bvb_verify_memory_fill(
              device_memory, 0U, 4096U, UINT32_C(0xa5c3f00d),
              &mismatched_words) == 0);
    CHECK(mismatched_words == 0U);
    CHECK(reset_fences(device, 1U, &fence) == VK_SUCCESS);
    CHECK(get_fence_status(device, fence) == VK_NOT_READY);
    const VkSemaphoreTypeCreateInfo timeline_type = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = UINT64_C(7),
    };
    const VkSemaphoreCreateInfo semaphore_create_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &timeline_type,
    };
    VkSemaphore timeline = VK_NULL_HANDLE;
    CHECK(create_semaphore(device, &semaphore_create_info, NULL, &timeline) ==
          VK_SUCCESS);
    uint64_t timeline_id = 0U;
    memcpy(&timeline_id, &timeline, sizeof(timeline));
    CHECK(bvb_handle_type(timeline_id) == BVB_OBJECT_SEMAPHORE);
    CHECK(bvb_handle_serial(timeline_id) == 2U);
    uint64_t timeline_value = 0U;
    CHECK(get_semaphore_counter(device, timeline, &timeline_value) ==
          VK_SUCCESS);
    CHECK(timeline_value == UINT64_C(7));
    const VkSemaphoreSignalInfo signal_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
        .semaphore = timeline,
        .value = UINT64_C(11),
    };
    CHECK(signal_semaphore(device, &signal_info) == VK_SUCCESS);
    CHECK(get_semaphore_counter(device, timeline, &timeline_value) ==
          VK_SUCCESS);
    CHECK(timeline_value == UINT64_C(11));
    uint64_t wait_value = UINT64_C(11);
    const VkSemaphoreWaitInfo wait_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1U,
        .pSemaphores = &timeline,
        .pValues = &wait_value,
    };
    CHECK(wait_semaphores(device, &wait_info, 0U) == VK_SUCCESS);
    wait_value = UINT64_C(12);
    CHECK(wait_semaphores(device, &wait_info, 0U) == VK_TIMEOUT);
    const VkSemaphoreSubmitInfo submit_wait = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = timeline,
        .value = UINT64_C(11),
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    const VkCommandBufferSubmitInfo submit_command = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = command_buffer,
    };
    const VkSemaphoreSubmitInfo submit_signal = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = timeline,
        .value = UINT64_C(13),
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    const VkSubmitInfo2 submit_info_2 = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = 1U,
        .pWaitSemaphoreInfos = &submit_wait,
        .commandBufferInfoCount = 1U,
        .pCommandBufferInfos = &submit_command,
        .signalSemaphoreInfoCount = 1U,
        .pSignalSemaphoreInfos = &submit_signal,
    };
    CHECK(queue_submit_2(queue, 1U, &submit_info_2, VK_NULL_HANDLE) ==
          VK_SUCCESS);
    CHECK(get_semaphore_counter(device, timeline, &timeline_value) ==
          VK_SUCCESS);
    CHECK(timeline_value == UINT64_C(13));
    destroy_semaphore(device, timeline, NULL);
    destroy_image_view(device, image_view, NULL);
    CHECK(bvb_image_view_proxy_id(image_view) == 0U);
    destroy_image(device, image, NULL);
    CHECK(bvb_image_proxy_id(image) == 0U);
    free_memory(device, image_memory, NULL);

    VkImage teardown_image = VK_NULL_HANDLE;
    CHECK(create_image(
              device, &image_create_info, NULL, &teardown_image) == VK_SUCCESS);
    VkImageViewCreateInfo teardown_view_info = image_view_create_info;
    teardown_view_info.image = teardown_image;
    VkImageView teardown_view = VK_NULL_HANDLE;
    CHECK(create_image_view(
              device, &teardown_view_info, NULL, &teardown_view) == VK_SUCCESS);
    CHECK(bvb_image_proxy_id(teardown_image) != 0U);
    CHECK(bvb_image_view_proxy_id(teardown_view) != 0U);
    CHECK(reset_command_pool(device, command_pool, 0U) == VK_SUCCESS);
    free_command_buffers(device, command_pool, 1U, &command_buffer);
    destroy_command_pool(device, command_pool, NULL);
    destroy_buffer(device, buffer, NULL);
    free_memory(device, device_memory, NULL);
    destroy_fence(device, fence, NULL);
    destroy_device(device, NULL);
    destroy_device(ownership_device, NULL);

    const float scaled_queue_priorities[2] = {0.75F, 0.25F};
    const VkDeviceQueueCreateInfo scaled_queue_infos[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = 0U,
            .queueCount = 1U,
            .pQueuePriorities = &scaled_queue_priorities[0],
        },
        {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = 1U,
            .queueCount = 1U,
            .pQueuePriorities = &scaled_queue_priorities[1],
        },
    };
    char scaled_extension_names[58][64];
    const char *scaled_extensions[59];
    for (uint32_t index = 0U; index < 58U; ++index) {
        CHECK(snprintf(scaled_extension_names[index],
                       sizeof(scaled_extension_names[index]),
                       "VK_BVB_scale_extension_%02u", index) > 0);
        scaled_extensions[index] = scaled_extension_names[index];
    }
    scaled_extensions[58] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo scaled_create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 2U,
        .pQueueCreateInfos = scaled_queue_infos,
        .enabledExtensionCount = 59U,
        .ppEnabledExtensionNames = scaled_extensions,
    };
    VkPhysicalDeviceFeatures enabled_base_features = {
        .robustBufferAccess = VK_TRUE,
        .geometryShader = VK_TRUE,
        .samplerAnisotropy = VK_TRUE,
    };
    scaled_create_info.pEnabledFeatures = &enabled_base_features;
    VkDevice scaled_device = VK_NULL_HANDLE;
    const VkPhysicalDeviceVulkan12Features unsupported_scaled_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .drawIndirectCount = VK_TRUE,
    };
    scaled_create_info.pNext = &unsupported_scaled_features;
    CHECK(create_device(physical_device, &scaled_create_info, NULL,
                        &scaled_device) == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(scaled_device == VK_NULL_HANDLE);
    VkPhysicalDeviceVulkan11Features enabled_vulkan11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .shaderDrawParameters = VK_TRUE,
    };
    VkPhysicalDeviceVulkan12Features enabled_vulkan12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .samplerMirrorClampToEdge = VK_TRUE,
        .descriptorIndexing = VK_TRUE,
        .descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
        .descriptorBindingUpdateUnusedWhilePending = VK_TRUE,
        .descriptorBindingPartiallyBound = VK_TRUE,
        .runtimeDescriptorArray = VK_TRUE,
        .scalarBlockLayout = VK_TRUE,
        .uniformBufferStandardLayout = VK_TRUE,
        .hostQueryReset = VK_TRUE,
        .timelineSemaphore = VK_TRUE,
        .bufferDeviceAddress = VK_TRUE,
        .vulkanMemoryModel = VK_TRUE,
    };
    VkPhysicalDeviceVulkan13Features enabled_vulkan13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .shaderDemoteToHelperInvocation = VK_TRUE,
        .subgroupSizeControl = VK_TRUE,
        .computeFullSubgroups = VK_TRUE,
        .synchronization2 = VK_TRUE,
        .shaderZeroInitializeWorkgroupMemory = VK_TRUE,
        .dynamicRendering = VK_TRUE,
        .maintenance4 = VK_TRUE,
    };
    VkPhysicalDeviceDepthClipEnableFeaturesEXT enabled_depth_clip = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT,
        .depthClipEnable = VK_TRUE,
    };
    VkPhysicalDeviceRobustness2FeaturesEXT enabled_robustness2 = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
        .robustBufferAccess2 = VK_TRUE,
        .nullDescriptor = VK_TRUE,
    };
    VkPhysicalDeviceMaintenance5FeaturesKHR enabled_maintenance5 = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR,
        .maintenance5 = VK_TRUE,
    };
    VkPhysicalDeviceMaintenance6FeaturesKHR enabled_maintenance6 = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR,
        .maintenance6 = VK_TRUE,
    };
    enabled_vulkan11.pNext = &enabled_vulkan12;
    enabled_vulkan12.pNext = &enabled_vulkan13;
    enabled_vulkan13.pNext = &enabled_depth_clip;
    enabled_depth_clip.pNext = &enabled_robustness2;
    enabled_robustness2.pNext = &enabled_maintenance5;
    enabled_maintenance5.pNext = &enabled_maintenance6;
    const VkLayerDeviceCreateInfo loader_private_device_info = {
        .sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO,
        .pNext = &enabled_vulkan11,
        .function = VK_LOADER_DATA_CALLBACK,
        .u.pfnSetDeviceLoaderData = test_set_device_loader_data,
    };
    scaled_create_info.pNext = &loader_private_device_info;
    CHECK(create_device(physical_device, &scaled_create_info, NULL,
                        &scaled_device) == VK_SUCCESS);
    CHECK(scaled_device != VK_NULL_HANDLE);
    VkQueue scaled_transfer_queue = VK_NULL_HANDLE;
    get_device_queue(scaled_device, 1U, 0U, &scaled_transfer_queue);
    CHECK(scaled_transfer_queue != VK_NULL_HANDLE);
    CHECK(test_loader_data_calls == 1U);
    get_device_queue(scaled_device, 1U, 0U, &scaled_transfer_queue);
    CHECK(test_loader_data_calls == 1U);
    const VkCommandPoolCreateInfo scaled_pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = 0U,
    };
    VkCommandPool scaled_pool = VK_NULL_HANDLE;
    CHECK(create_command_pool(scaled_device, &scaled_pool_info, NULL,
                              &scaled_pool) == VK_SUCCESS);
    const VkCommandBufferAllocateInfo scaled_command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = scaled_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1U,
    };
    VkCommandBuffer scaled_command = VK_NULL_HANDLE;
    CHECK(allocate_command_buffers(
              scaled_device, &scaled_command_info, &scaled_command) ==
          VK_SUCCESS);
    CHECK(scaled_command != VK_NULL_HANDLE);
    CHECK(test_loader_data_calls == 2U);
    free_command_buffers(scaled_device, scaled_pool, 1U, &scaled_command);
    destroy_command_pool(scaled_device, scaled_pool, NULL);
    destroy_device(scaled_device, NULL);
    destroy_surface(instance_one, surface, NULL);

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
           "exposed_extensions=7 exposed_layers=0 "
           "instance_one=%llu instance_two=%llu physical_device=%llu "
           "device=%s device_api=%u driver=%u vendor=%u device_id=%u "
           "queues=%u memory_types=%u memory_heaps=%u device_extensions=%u "
           "sampler_anisotropy=%u logical_device=%llu queue=%llu "
           "empty_submit=0 queue_wait=0 device_wait=0 "
           "command_pool=%llu command_buffer=%llu command_submit=0 "
           "pool_reset=0 buffer=%llu memory=%llu memory_type=%u "
           "buffer_requirements2=4096,256,1 buffer_address=%llu "
           "image=%llu image_view=%llu image_bytes=%llu "
           "image_allocation_bytes=%llu image_dedicated=1,1 "
           "mapped_bytes=4096 mapped_mismatches=%u "
           "fill_words=1024 mismatches=%u fence=%llu fence_before=1 "
           "fenced_submit=0 fence_after=0 fence_wait=0 fence_reset=0 "
           "fence_after_reset=1\n",
           api_version, (unsigned long long)instance_one_id,
           (unsigned long long)instance_two_id,
           (unsigned long long)physical_id, properties.deviceName,
           properties.apiVersion, properties.driverVersion,
           properties.vendorID, properties.deviceID,
           available_queue_count, memory.memoryTypeCount,
           memory.memoryHeapCount, available_device_extension_count,
           features.samplerAnisotropy,
           (unsigned long long)device_id,
           (unsigned long long)queue_id,
           (unsigned long long)command_pool_id,
           (unsigned long long)command_buffer_id,
           (unsigned long long)buffer_id,
           (unsigned long long)memory_id,
           memory_type_index, (unsigned long long)buffer_device_address,
           (unsigned long long)image_id,
           (unsigned long long)image_view_id,
           (unsigned long long)image_requirements.size,
           (unsigned long long)image_requirements_2.memoryRequirements.size,
           mapped_mismatches, mismatched_words,
           (unsigned long long)fence_id);
    return 0;
}
