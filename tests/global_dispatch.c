#define _POSIX_C_SOURCE 200809L
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

typedef VkResult(VKAPI_PTR *bvb_create_platform_surface_fn)(
    VkInstance instance, const void *create_info,
    const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface);
typedef VkBool32(VKAPI_PTR *bvb_xlib_presentation_support_fn)(
    VkPhysicalDevice physical_device, uint32_t queue_family_index,
    void *display, unsigned long visual_id);

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
    vulkan11_features.pNext = &vulkan12_features;
    vulkan12_features.pNext = &vulkan13_features;
    vulkan13_features.pNext = &depth_clip_features;
    depth_clip_features.pNext = &robustness2_features;
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

    PFN_vkGetDeviceQueue get_device_queue = NULL;
    PFN_vkDestroyDevice destroy_device = NULL;
    PFN_vkQueueSubmit queue_submit = NULL;
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
    PFN_vkAllocateMemory allocate_memory = NULL;
    PFN_vkFreeMemory free_memory = NULL;
    PFN_vkBindBufferMemory bind_buffer_memory = NULL;
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
    erased = vkGetDeviceProcAddr(device, "vkGetDeviceQueue");
    CHECK(erased != NULL);
    memcpy(&get_device_queue, &erased, sizeof(get_device_queue));
    erased = vkGetDeviceProcAddr(device, "vkDestroyDevice");
    CHECK(erased != NULL);
    memcpy(&destroy_device, &erased, sizeof(destroy_device));
    erased = vkGetDeviceProcAddr(device, "vkQueueSubmit");
    CHECK(erased != NULL);
    memcpy(&queue_submit, &erased, sizeof(queue_submit));
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
    erased = vkGetDeviceProcAddr(device, "vkAllocateMemory");
    CHECK(erased != NULL);
    memcpy(&allocate_memory, &erased, sizeof(allocate_memory));
    erased = vkGetDeviceProcAddr(device, "vkFreeMemory");
    CHECK(erased != NULL);
    memcpy(&free_memory, &erased, sizeof(free_memory));
    erased = vkGetDeviceProcAddr(device, "vkBindBufferMemory");
    CHECK(erased != NULL);
    memcpy(&bind_buffer_memory, &erased, sizeof(bind_buffer_memory));
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
    VkSwapchainKHR unavailable_swapchain = VK_NULL_HANDLE;
    CHECK(create_swapchain(device, &swapchain_create_info, NULL,
                           &unavailable_swapchain) ==
          VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(unavailable_swapchain == VK_NULL_HANDLE);
    uint32_t unavailable_image_count = UINT32_MAX;
    CHECK(get_swapchain_images(device, VK_NULL_HANDLE,
                               &unavailable_image_count, NULL) ==
          VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(unavailable_image_count == 0U);
    uint32_t unavailable_image_index = UINT32_MAX;
    CHECK(acquire_next_image(device, VK_NULL_HANDLE, 0U, VK_NULL_HANDLE,
                             VK_NULL_HANDLE, &unavailable_image_index) ==
          VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(unavailable_image_index == 0U);
    const VkAcquireNextImageInfoKHR acquire_info = {
        .sType = VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR,
    };
    CHECK(acquire_next_image2(device, &acquire_info,
                              &unavailable_image_index) ==
          VK_ERROR_FEATURE_NOT_PRESENT);
    destroy_swapchain(device, VK_NULL_HANDLE, NULL);
    VkQueue queue = VK_NULL_HANDLE;
    get_device_queue(device, queue_family_index, 0U, &queue);
    CHECK(queue != VK_NULL_HANDLE);
    const uint64_t queue_id = bvb_queue_proxy_id(queue);
    CHECK(bvb_handle_type(queue_id) == BVB_OBJECT_QUEUE);
    CHECK(bvb_handle_serial(queue_id) == 1U);
    VkQueue repeated_queue = VK_NULL_HANDLE;
    get_device_queue(device, queue_family_index, 0U, &repeated_queue);
    CHECK(repeated_queue == queue);
    const VkPresentInfoKHR unavailable_present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
    };
    CHECK(queue_present(queue, &unavailable_present) ==
          VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(queue_submit(queue, 0U, NULL, VK_NULL_HANDLE) == VK_SUCCESS);
    const VkSubmitInfo unsupported_submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    };
    CHECK(queue_submit(queue, 1U, &unsupported_submit, VK_NULL_HANDLE) ==
          VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(queue_wait_idle(queue) == VK_SUCCESS);
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
    const VkBufferCreateInfo buffer_create_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 4096U,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buffer = VK_NULL_HANDLE;
    CHECK(create_buffer(device, &buffer_create_info, NULL, &buffer) ==
          VK_SUCCESS);
    const uint64_t buffer_id = bvb_buffer_proxy_id(buffer);
    CHECK(bvb_handle_type(buffer_id) == BVB_OBJECT_BUFFER);
    CHECK(bvb_handle_serial(buffer_id) == 1U);
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
    const VkMemoryAllocateInfo memory_allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
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
    CHECK(reset_command_pool(device, command_pool, 0U) == VK_SUCCESS);
    free_command_buffers(device, command_pool, 1U, &command_buffer);
    destroy_command_pool(device, command_pool, NULL);
    destroy_buffer(device, buffer, NULL);
    free_memory(device, device_memory, NULL);
    destroy_fence(device, fence, NULL);
    destroy_device(device, NULL);

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
    const char *scaled_extensions[58];
    for (uint32_t index = 0U; index < 58U; ++index) {
        CHECK(snprintf(scaled_extension_names[index],
                       sizeof(scaled_extension_names[index]),
                       "VK_BVB_scale_extension_%02u", index) > 0);
        scaled_extensions[index] = scaled_extension_names[index];
    }
    VkDeviceCreateInfo scaled_create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 2U,
        .pQueueCreateInfos = scaled_queue_infos,
        .enabledExtensionCount = 58U,
        .ppEnabledExtensionNames = scaled_extensions,
    };
    VkDevice scaled_device = VK_NULL_HANDLE;
    const VkPhysicalDeviceVulkan12Features unsupported_scaled_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .timelineSemaphore = VK_TRUE,
    };
    scaled_create_info.pNext = &unsupported_scaled_features;
    CHECK(create_device(physical_device, &scaled_create_info, NULL,
                        &scaled_device) == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(scaled_device == VK_NULL_HANDLE);
    scaled_create_info.pNext = NULL;
    CHECK(create_device(physical_device, &scaled_create_info, NULL,
                        &scaled_device) == VK_SUCCESS);
    CHECK(scaled_device != VK_NULL_HANDLE);
    VkQueue scaled_transfer_queue = VK_NULL_HANDLE;
    get_device_queue(scaled_device, 1U, 0U, &scaled_transfer_queue);
    CHECK(scaled_transfer_queue != VK_NULL_HANDLE);
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
           memory_type_index, mapped_mismatches, mismatched_words,
           (unsigned long long)fence_id);
    return 0;
}
