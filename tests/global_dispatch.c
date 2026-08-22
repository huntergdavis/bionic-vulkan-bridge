#define _POSIX_C_SOURCE 200809L
#define VK_NO_PROTOTYPES

#include <bvb/global_dispatch.h>
#include <bvb/handle.h>
#include <bvb/protocol.h>
#include <bvb/vulkan_discovery.h>
#include <bvb/vulkan_pipeline_wire.h>

#include <vulkan/vk_layer.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
static const uint32_t test_builtin_vertex[
    BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_VERTEX_CODE_SIZE / 4U] = {
    [0] = UINT32_C(0x07230203), [312] = UINT32_C(0x11223344),
};
static const uint32_t test_builtin_fragment[
    BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_FRAGMENT_CODE_SIZE / 4U] = {
    [0] = UINT32_C(0x07230203), [3913] = UINT32_C(0x55667788),
};

static bool bvb_hardware_validation_enabled(void) {
    const char *value = getenv("BVB_GLOBAL_DISPATCH_HARDWARE");
    return value != NULL && strcmp(value, "1") == 0;
}

static bool bvb_read_u32_environment(const char *name, uint32_t fallback,
                                     uint32_t minimum, uint32_t maximum,
                                     uint32_t *output) {
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        *output = fallback;
        return true;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < minimum ||
        parsed > maximum) {
        return false;
    }
    *output = (uint32_t)parsed;
    return true;
}

static bool bvb_sleep_milliseconds(uint32_t milliseconds) {
    struct timespec remaining = {
        .tv_sec = (time_t)(milliseconds / UINT32_C(1000)),
        .tv_nsec = (long)(milliseconds % UINT32_C(1000)) * 1000000L,
    };
    while (nanosleep(&remaining, &remaining) != 0) {
        if (errno != EINTR) return false;
    }
    return true;
}

static bool bvb_shared_command_stream_enabled(void) {
    const char *value = getenv("BVB_COMMAND_STREAM");
    return value != NULL && strcmp(value, "shared") == 0;
}

static bool bvb_shared_mapped_memory_enabled(void) {
    const char *value = getenv("BVB_MAPPED_MEMORY");
    return value != NULL && strcmp(value, "shared") == 0;
}

static bool bvb_keep_mapped_memory_enabled(void) {
    return getenv("BVB_TEST_KEEP_MEMORY_MAPPED") != NULL;
}

static bool bvb_noncoherent_mapped_memory_enabled(void) {
    return getenv("BVB_TEST_NONCOHERENT_MEMORY") != NULL;
}

static bool bvb_is_power_of_two(VkDeviceSize value) {
    return value != 0U && (value & (value - 1U)) == 0U;
}

static bool bvb_memory_requirements_are_valid(
    const VkMemoryRequirements *requirements, VkDeviceSize minimum_size,
    uint32_t memory_type_count) {
    const uint32_t valid_type_bits =
        memory_type_count == 32U
            ? UINT32_MAX
            : (UINT32_C(1) << memory_type_count) - UINT32_C(1);
    return requirements->size >= minimum_size &&
           bvb_is_power_of_two(requirements->alignment) &&
           requirements->memoryTypeBits != 0U &&
           (requirements->memoryTypeBits & ~valid_type_bits) == 0U;
}

static bool bvb_memory_requirements_match(
    const VkMemoryRequirements *left, const VkMemoryRequirements *right) {
    return left->size == right->size &&
           left->alignment == right->alignment &&
           left->memoryTypeBits == right->memoryTypeBits;
}

static bool bvb_image_format_properties_match(
    const VkImageFormatProperties *left,
    const VkImageFormatProperties *right) {
    return left->maxExtent.width == right->maxExtent.width &&
           left->maxExtent.height == right->maxExtent.height &&
           left->maxExtent.depth == right->maxExtent.depth &&
           left->maxMipLevels == right->maxMipLevels &&
           left->maxArrayLayers == right->maxArrayLayers &&
           left->sampleCounts == right->sampleCounts &&
           left->maxResourceSize == right->maxResourceSize;
}

struct bvb_concurrent_record_context {
    pthread_barrier_t *start;
    PFN_vkBeginCommandBuffer begin_command_buffer;
    PFN_vkCmdFillBuffer cmd_fill_buffer;
    PFN_vkEndCommandBuffer end_command_buffer;
    VkCommandBuffer command_buffer;
    VkBuffer buffer;
    VkResult begin_result;
    VkResult end_result;
};

static void *bvb_record_shared_command_buffer(void *opaque) {
    struct bvb_concurrent_record_context *context = opaque;
    const VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    const int barrier_result = pthread_barrier_wait(context->start);
    if (barrier_result != 0 &&
        barrier_result != PTHREAD_BARRIER_SERIAL_THREAD) {
        context->begin_result = VK_ERROR_INITIALIZATION_FAILED;
        context->end_result = VK_ERROR_INITIALIZATION_FAILED;
        return NULL;
    }
    context->begin_result = context->begin_command_buffer(
        context->command_buffer, &begin_info);
    if (context->begin_result != VK_SUCCESS) {
        context->end_result = VK_ERROR_INITIALIZATION_FAILED;
        return NULL;
    }
    for (uint32_t index = 0U; index < 256U; ++index) {
        context->cmd_fill_buffer(
            context->command_buffer, context->buffer, 0U, 4096U,
            UINT32_C(0xa5c3f00d));
    }
    context->end_result =
        context->end_command_buffer(context->command_buffer);
    return NULL;
}

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
    const bool hardware_mode = bvb_hardware_validation_enabled();
    const bool shared_command_stream = bvb_shared_command_stream_enabled();
    const bool concurrent_command_stream =
        shared_command_stream &&
        getenv("BVB_TEST_CONCURRENT_COMMAND_STREAMS") != NULL;
    const bool animated_wsi =
        shared_command_stream && getenv("BVB_TEST_ANIMATED_WSI") != NULL;
    const bool shared_mapped_memory = bvb_shared_mapped_memory_enabled();
    const bool keep_mapped_memory =
        shared_mapped_memory || bvb_keep_mapped_memory_enabled();
    const bool noncoherent_mapped_memory =
        bvb_noncoherent_mapped_memory_enabled();
    uint32_t wsi_width = 2800U;
    uint32_t wsi_height = 1752U;
    uint32_t present_hold_ms = 0U;
    CHECK(bvb_read_u32_environment("BVB_GLOBAL_DISPATCH_WSI_WIDTH", 2800U,
                                   1U, 16384U, &wsi_width));
    CHECK(bvb_read_u32_environment("BVB_GLOBAL_DISPATCH_WSI_HEIGHT", 1752U,
                                   1U, 16384U, &wsi_height));
    CHECK(bvb_read_u32_environment("BVB_GLOBAL_DISPATCH_PRESENT_HOLD_MS", 0U,
                                   0U, 30000U, &present_hold_ms));
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
        .apiVersion = hardware_mode ? VK_API_VERSION_1_3
                                    : VK_API_VERSION_1_1,
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
    if (!hardware_mode) {
        CHECK(bvb_handle_serial(instance_one_id) == 1U);
    }
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
    const uint32_t available_physical_count = physical_count;
    if (hardware_mode) {
        CHECK(available_physical_count > 0U);
        CHECK(available_physical_count <= BVB_VULKAN_MAX_PHYSICAL_DEVICES);
    } else {
        CHECK(available_physical_count == 1U);
    }
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    physical_count = 1U;
    const VkResult physical_enumeration_result = enumerate_physical_devices(
        instance_one, &physical_count, &physical_device);
    CHECK(physical_enumeration_result ==
          (available_physical_count > 1U ? VK_INCOMPLETE : VK_SUCCESS));
    CHECK(physical_count == 1U);
    CHECK(physical_device != VK_NULL_HANDLE);
    const uint64_t physical_id =
        bvb_physical_device_proxy_id(physical_device);
    CHECK(bvb_handle_type(physical_id) == BVB_OBJECT_PHYSICAL_DEVICE);
    if (!hardware_mode) {
        CHECK(bvb_handle_serial(physical_id) == 1U);
    }
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
    CHECK(surface_capabilities.currentExtent.width == wsi_width);
    CHECK(surface_capabilities.currentExtent.height == wsi_height);
    CHECK(surface_capabilities.minImageCount == 2U);
    uint32_t surface_format_count = 0U;
    CHECK(get_surface_formats(physical_device, surface,
                              &surface_format_count, NULL) == VK_SUCCESS);
    CHECK(surface_format_count == 4U);
    VkSurfaceFormatKHR surface_formats[4] = {0};
    CHECK(get_surface_formats(physical_device, surface,
                              &surface_format_count,
                              surface_formats) == VK_SUCCESS);
    CHECK(surface_formats[0].format == VK_FORMAT_R8G8B8A8_UNORM);
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
    if (hardware_mode) {
        CHECK(properties.limits.maxPushConstantsSize >= 256U);
    } else {
        CHECK(properties.limits.maxPushConstantsSize == 256U);
    }

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
    if (hardware_mode) {
        CHECK(image_format_properties.maxExtent.width >= 2800U);
        CHECK(image_format_properties.maxExtent.height >= 1752U);
        CHECK(image_format_properties.maxExtent.depth >= 1U);
        CHECK(image_format_properties.maxMipLevels >= 1U);
        CHECK(image_format_properties.maxArrayLayers >= 1U);
        CHECK((image_format_properties.sampleCounts &
               VK_SAMPLE_COUNT_1_BIT) != 0U);
        CHECK(image_format_properties.maxResourceSize >=
              64U * 64U * sizeof(uint32_t));
    } else {
        CHECK(image_format_properties.maxExtent.width == 4096U);
        CHECK(image_format_properties.maxExtent.height == 2048U);
        CHECK(image_format_properties.maxMipLevels == 12U);
        CHECK(image_format_properties.maxArrayLayers == 256U);
        CHECK(image_format_properties.maxResourceSize ==
              UINT64_C(0x100000000));
    }
    const VkImageFormatProperties supported_image_format_properties =
        image_format_properties;
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
    VkFormatProperties3 format_properties3 = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3,
    };
    VkFormatProperties2 format_properties2 = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &format_properties3,
    };
    get_format_properties2(
        physical_device, VK_FORMAT_R8G8B8A8_UNORM, &format_properties2);
    CHECK(format_properties2.formatProperties.optimalTilingFeatures ==
          format_properties.optimalTilingFeatures);
    CHECK((format_properties3.linearTilingFeatures &
           (UINT64_C(1) << 40U)) != 0U);
    CHECK((format_properties3.optimalTilingFeatures &
           (UINT64_C(1) << 41U)) != 0U);
    CHECK((format_properties3.bufferFeatures &
           (UINT64_C(1) << 42U)) != 0U);
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
    if (hardware_mode) {
        CHECK(bvb_image_format_properties_match(
            &image_properties2.imageFormatProperties,
            &supported_image_format_properties));
    } else {
        CHECK(image_properties2.imageFormatProperties.maxExtent.width ==
              4096U);
    }
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
    const VkExternalSemaphoreHandleTypeFlagBits external_semaphore_handle_type =
        hardware_mode ? VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT
                      : VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    const VkPhysicalDeviceExternalSemaphoreInfo external_semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
        .handleType = external_semaphore_handle_type,
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
           external_semaphore_handle_type) != 0U);
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
    if (!hardware_mode) {
        CHECK(bvb_handle_serial(device_id) == 1U);
    }
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
    PFN_vkMapMemory2 map_memory_2 = NULL;
    PFN_vkUnmapMemory unmap_memory = NULL;
    PFN_vkFlushMappedMemoryRanges flush_mapped_memory_ranges = NULL;
    PFN_vkInvalidateMappedMemoryRanges invalidate_mapped_memory_ranges = NULL;
    PFN_vkCmdFillBuffer cmd_fill_buffer = NULL;
    PFN_vkCmdClearColorImage cmd_clear_color_image = NULL;
    PFN_vkCmdPipelineBarrier2 cmd_pipeline_barrier_2 = NULL;
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
    PFN_vkCreateDescriptorUpdateTemplate create_descriptor_update_template =
        NULL;
    PFN_vkDestroyDescriptorUpdateTemplate destroy_descriptor_update_template =
        NULL;
    PFN_vkUpdateDescriptorSetWithTemplate
        update_descriptor_set_with_template = NULL;
    PFN_vkCmdBindDescriptorSets cmd_bind_descriptor_sets = NULL;
    PFN_vkCmdBeginRendering cmd_begin_rendering = NULL;
    PFN_vkCmdEndRendering cmd_end_rendering = NULL;
    PFN_vkCmdBindPipeline cmd_bind_pipeline = NULL;
    PFN_vkCmdPushConstants cmd_push_constants = NULL;
    PFN_vkCmdSetViewportWithCount cmd_set_viewport_with_count = NULL;
    PFN_vkCmdSetScissorWithCount cmd_set_scissor_with_count = NULL;
    PFN_vkCmdDraw cmd_draw = NULL;
    PFN_vkCmdBindVertexBuffers cmd_bind_vertex_buffers = NULL;
    PFN_vkCmdBindVertexBuffers2 cmd_bind_vertex_buffers_2 = NULL;
    PFN_vkCmdBindIndexBuffer cmd_bind_index_buffer = NULL;
    PFN_vkCmdBindIndexBuffer2 cmd_bind_index_buffer_2 = NULL;
    PFN_vkCmdDrawIndexed cmd_draw_indexed = NULL;
    PFN_vkCmdDrawIndirect cmd_draw_indirect = NULL;
    PFN_vkCmdDrawIndexedIndirect cmd_draw_indexed_indirect = NULL;
    PFN_vkCmdDrawIndirectCount cmd_draw_indirect_count = NULL;
    PFN_vkCmdDrawIndexedIndirectCount cmd_draw_indexed_indirect_count = NULL;
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
    erased = vkGetDeviceProcAddr(device, "vkMapMemory2");
    CHECK(erased != NULL);
    memcpy(&map_memory_2, &erased, sizeof(map_memory_2));
    CHECK(vkGetDeviceProcAddr(device, "vkMapMemory2KHR") == erased);
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
    erased = vkGetDeviceProcAddr(device, "vkCmdClearColorImage");
    CHECK(erased != NULL);
    memcpy(&cmd_clear_color_image, &erased, sizeof(cmd_clear_color_image));
    erased = vkGetDeviceProcAddr(device, "vkCmdPipelineBarrier2");
    CHECK(erased != NULL);
    memcpy(&cmd_pipeline_barrier_2, &erased,
           sizeof(cmd_pipeline_barrier_2));
    CHECK(vkGetDeviceProcAddr(device, "vkCmdPipelineBarrier2KHR") == NULL);
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
    RESOLVE_DESCRIPTOR(vkCreateDescriptorUpdateTemplate,
                       create_descriptor_update_template);
    RESOLVE_DESCRIPTOR(vkDestroyDescriptorUpdateTemplate,
                       destroy_descriptor_update_template);
    RESOLVE_DESCRIPTOR(vkUpdateDescriptorSetWithTemplate,
                       update_descriptor_set_with_template);
    CHECK(vkGetDeviceProcAddr(device,
                              "vkUpdateDescriptorSetWithTemplateKHR") ==
          erased);
    RESOLVE_DESCRIPTOR(vkCmdBindDescriptorSets, cmd_bind_descriptor_sets);
    RESOLVE_DESCRIPTOR(vkCmdBeginRendering, cmd_begin_rendering);
    CHECK(vkGetDeviceProcAddr(device, "vkCmdBeginRenderingKHR") == erased);
    RESOLVE_DESCRIPTOR(vkCmdEndRendering, cmd_end_rendering);
    CHECK(vkGetDeviceProcAddr(device, "vkCmdEndRenderingKHR") == erased);
    RESOLVE_DESCRIPTOR(vkCmdBindPipeline, cmd_bind_pipeline);
    RESOLVE_DESCRIPTOR(vkCmdPushConstants, cmd_push_constants);
    RESOLVE_DESCRIPTOR(vkCmdSetViewportWithCount, cmd_set_viewport_with_count);
    CHECK(vkGetDeviceProcAddr(device, "vkCmdSetViewportWithCountEXT") ==
          erased);
    RESOLVE_DESCRIPTOR(vkCmdSetScissorWithCount, cmd_set_scissor_with_count);
    CHECK(vkGetDeviceProcAddr(device, "vkCmdSetScissorWithCountEXT") ==
          erased);
    RESOLVE_DESCRIPTOR(vkCmdDraw, cmd_draw);
    RESOLVE_DESCRIPTOR(vkCmdBindVertexBuffers, cmd_bind_vertex_buffers);
    RESOLVE_DESCRIPTOR(vkCmdBindVertexBuffers2, cmd_bind_vertex_buffers_2);
    CHECK(vkGetDeviceProcAddr(device, "vkCmdBindVertexBuffers2EXT") ==
          erased);
    RESOLVE_DESCRIPTOR(vkCmdBindIndexBuffer, cmd_bind_index_buffer);
    RESOLVE_DESCRIPTOR(vkCmdBindIndexBuffer2, cmd_bind_index_buffer_2);
    CHECK(vkGetDeviceProcAddr(device, "vkCmdBindIndexBuffer2KHR") == erased);
    RESOLVE_DESCRIPTOR(vkCmdDrawIndexed, cmd_draw_indexed);
    RESOLVE_DESCRIPTOR(vkCmdDrawIndirect, cmd_draw_indirect);
    RESOLVE_DESCRIPTOR(vkCmdDrawIndexedIndirect, cmd_draw_indexed_indirect);
    RESOLVE_DESCRIPTOR(vkCmdDrawIndirectCount, cmd_draw_indirect_count);
    CHECK(vkGetDeviceProcAddr(device, "vkCmdDrawIndirectCountKHR") ==
          erased);
    RESOLVE_DESCRIPTOR(vkCmdDrawIndexedIndirectCount,
                       cmd_draw_indexed_indirect_count);
    CHECK(vkGetDeviceProcAddr(
              device, "vkCmdDrawIndexedIndirectCountKHR") == erased);
    RESOLVE_DESCRIPTOR(vkCreatePipelineLayout, create_pipeline_layout);
    RESOLVE_DESCRIPTOR(vkDestroyPipelineLayout, destroy_pipeline_layout);
    RESOLVE_DESCRIPTOR(vkCreateGraphicsPipelines, create_graphics_pipelines);
    RESOLVE_DESCRIPTOR(vkDestroyPipeline, destroy_pipeline);
#undef RESOLVE_DESCRIPTOR
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
        .imageFormat = VK_FORMAT_R8G8B8A8_UNORM,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = {wsi_width, wsi_height},
        .imageArrayLayers = 1U,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT,
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
    const VkImageViewUsageCreateInfo virtual_image_view_usage = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
    };
    const VkImageViewCreateInfo virtual_image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = &virtual_image_view_usage,
        .image = virtual_images[0],
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
    VkImageView virtual_image_view = VK_NULL_HANDLE;
    CHECK(create_image_view(device, &virtual_image_view_create_info, NULL,
                            &virtual_image_view) == VK_SUCCESS);
    CHECK(bvb_handle_type(bvb_image_view_proxy_id(virtual_image_view)) ==
          BVB_OBJECT_IMAGE_VIEW);
    const VkSemaphoreCreateInfo binary_semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    VkSemaphore acquire_semaphore = VK_NULL_HANDLE;
    VkSemaphore render_semaphore = VK_NULL_HANDLE;
    CHECK(create_semaphore(device, &binary_semaphore_info, NULL,
                           &acquire_semaphore) == VK_SUCCESS);
    if (animated_wsi)
        CHECK(create_semaphore(device, &binary_semaphore_info, NULL,
                               &render_semaphore) == VK_SUCCESS);
    uint32_t virtual_image_index = UINT32_MAX;
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
    if (!hardware_mode) {
        CHECK(bvb_handle_serial(queue_id) == 1U);
    }
    VkQueue repeated_queue = VK_NULL_HANDLE;
    get_device_queue(device, queue_family_index, 0U, &repeated_queue);
    CHECK(repeated_queue == queue);
    uint32_t animated_frame_count = 0U;
    bool animated_reused_image = false;
    uint64_t animated_recording_rtts = 0U;
    uint32_t animated_image_indices[4] = {0U, 0U, 0U, 0U};
    if (animated_wsi) {
        const VkCommandPoolCreateInfo animation_pool_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = queue_family_index,
        };
        VkCommandPool animation_pool = VK_NULL_HANDLE;
        CHECK(create_command_pool(device, &animation_pool_info, NULL,
                                  &animation_pool) == VK_SUCCESS);
        const VkCommandBufferAllocateInfo animation_command_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = animation_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1U,
        };
        VkCommandBuffer animation_command = VK_NULL_HANDLE;
        CHECK(allocate_command_buffers(device, &animation_command_info,
                                       &animation_command) == VK_SUCCESS);
        bool image_was_presented[3] = {false, false, false};
        const VkClearColorValue frame_colors[4] = {
            {.float32 = {1.0F, 0.0F, 0.0F, 1.0F}},
            {.float32 = {0.0F, 1.0F, 0.0F, 1.0F}},
            {.float32 = {0.0F, 0.0F, 1.0F, 1.0F}},
            {.float32 = {1.0F, 1.0F, 1.0F, 1.0F}},
        };
        const VkImageSubresourceRange animation_range = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1U,
            .layerCount = 1U,
        };
        for (uint32_t frame = 0U; frame < 4U; ++frame) {
            VkResult acquire_result = VK_NOT_READY;
            for (uint32_t attempt = 0U;
                 attempt < 5000U && acquire_result == VK_NOT_READY;
                 ++attempt) {
                acquire_result = acquire_next_image(
                    device, virtual_swapchain, 0U, acquire_semaphore,
                    VK_NULL_HANDLE, &virtual_image_index);
                if (acquire_result == VK_NOT_READY)
                    CHECK(bvb_sleep_milliseconds(1U));
            }
            if (acquire_result != VK_SUCCESS) {
                fprintf(stderr,
                        "E076_ACQUIRE_FAILED frame=%u result=%d\n",
                        frame + 1U, (int)acquire_result);
            }
            CHECK(acquire_result == VK_SUCCESS);
            CHECK(virtual_image_index < virtual_image_count);
            animated_image_indices[frame] = virtual_image_index;
            if (image_was_presented[virtual_image_index])
                animated_reused_image = true;
            CHECK(reset_command_pool(device, animation_pool, 0U) ==
                  VK_SUCCESS);
            const VkCommandBufferBeginInfo animation_begin = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            };
            const uint64_t exchanges_before_animation =
                bvb_global_dispatch_exchange_count();
            CHECK(begin_command_buffer(animation_command, &animation_begin) ==
                  VK_SUCCESS);
            const VkImageMemoryBarrier2 to_transfer = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                .srcAccessMask = VK_ACCESS_2_NONE,
                .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .oldLayout = image_was_presented[virtual_image_index]
                                 ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                 : VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = virtual_images[virtual_image_index],
                .subresourceRange = animation_range,
            };
            const VkDependencyInfo to_transfer_dependency = {
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .imageMemoryBarrierCount = 1U,
                .pImageMemoryBarriers = &to_transfer,
            };
            cmd_pipeline_barrier_2(animation_command,
                                   &to_transfer_dependency);
            cmd_clear_color_image(animation_command,
                                  virtual_images[virtual_image_index],
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  &frame_colors[frame], 1U,
                                  &animation_range);
            const VkImageMemoryBarrier2 to_present = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_NONE,
                .dstAccessMask = VK_ACCESS_2_NONE,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = virtual_images[virtual_image_index],
                .subresourceRange = animation_range,
            };
            const VkDependencyInfo to_present_dependency = {
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .imageMemoryBarrierCount = 1U,
                .pImageMemoryBarriers = &to_present,
            };
            cmd_pipeline_barrier_2(animation_command,
                                   &to_present_dependency);
            CHECK(end_command_buffer(animation_command) == VK_SUCCESS);
            const uint64_t exchanges_after_animation =
                bvb_global_dispatch_exchange_count();
            CHECK(exchanges_after_animation == exchanges_before_animation);
            animated_recording_rtts +=
                exchanges_after_animation - exchanges_before_animation;

            const VkSemaphoreSubmitInfo acquire_wait = {
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .semaphore = acquire_semaphore,
                .stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            };
            const VkCommandBufferSubmitInfo animation_command_submit = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .commandBuffer = animation_command,
            };
            const VkSemaphoreSubmitInfo render_signal = {
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .semaphore = render_semaphore,
                .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            };
            const VkSubmitInfo2 animation_submit = {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                .waitSemaphoreInfoCount = 1U,
                .pWaitSemaphoreInfos = &acquire_wait,
                .commandBufferInfoCount = 1U,
                .pCommandBufferInfos = &animation_command_submit,
                .signalSemaphoreInfoCount = 1U,
                .pSignalSemaphoreInfos = &render_signal,
            };
            CHECK(queue_submit_2(queue, 1U, &animation_submit,
                                 VK_NULL_HANDLE) == VK_SUCCESS);
            VkResult per_swapchain_result = VK_ERROR_UNKNOWN;
            const VkPresentInfoKHR virtual_present = {
                .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                .waitSemaphoreCount = 1U,
                .pWaitSemaphores = &render_semaphore,
                .swapchainCount = 1U,
                .pSwapchains = &virtual_swapchain,
                .pImageIndices = &virtual_image_index,
                .pResults = &per_swapchain_result,
            };
            CHECK(queue_present(queue, &virtual_present) == VK_SUCCESS);
            CHECK(per_swapchain_result == VK_SUCCESS);
            image_was_presented[virtual_image_index] = true;
            ++animated_frame_count;
        }
        CHECK(animated_frame_count == 4U);
        CHECK(animated_reused_image);
        CHECK(animated_recording_rtts == 0U);
        free_command_buffers(device, animation_pool, 1U,
                             &animation_command);
        destroy_command_pool(device, animation_pool, NULL);
    } else {
        CHECK(acquire_next_image(device, virtual_swapchain, UINT64_MAX,
                                 acquire_semaphore, VK_NULL_HANDLE,
                                 &virtual_image_index) == VK_SUCCESS);
        CHECK(virtual_image_index < virtual_image_count);
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
    }
    CHECK(bvb_sleep_milliseconds(present_hold_ms));
    if (render_semaphore != VK_NULL_HANDLE)
        destroy_semaphore(device, render_semaphore, NULL);
    destroy_semaphore(device, acquire_semaphore, NULL);
    CHECK(queue_submit(queue, 0U, NULL, VK_NULL_HANDLE) == VK_SUCCESS);
    const VkSubmitInfo unsupported_submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    };
    CHECK(queue_submit(queue, 1U, &unsupported_submit, VK_NULL_HANDLE) ==
          VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(queue_wait_idle(queue) == VK_SUCCESS);
    CHECK(device_wait_idle(device) == VK_SUCCESS);

    const VkAllocationCallbacks unsupported_allocator = {0};
    const VkDescriptorType core_descriptor_types[] = {
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    };
    VkDescriptorSetLayoutBinding core_bindings[6] = {0};
    VkDescriptorPoolSize core_pool_sizes[6] = {0};
    const uint32_t core_pool_counts[] = {
        512U, 16U, 512U, 16U, 2048U, 512U,
    };
    for (uint32_t index = 0U; index < 6U; ++index) {
        core_bindings[index] = (VkDescriptorSetLayoutBinding){
            .binding = index,
            .descriptorType = core_descriptor_types[index],
            .descriptorCount = 1U,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        };
        core_pool_sizes[index] = (VkDescriptorPoolSize){
            .type = core_descriptor_types[index],
            .descriptorCount = core_pool_counts[index],
        };
    }
    const VkDescriptorSetLayoutCreateInfo core_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 6U,
        .pBindings = core_bindings,
    };
    VkDescriptorSetLayout core_layout = VK_NULL_HANDLE;
    CHECK(create_descriptor_set_layout(
              device, &core_layout_info, NULL, &core_layout) == VK_SUCCESS);
    const VkDescriptorPoolCreateInfo core_pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1024U,
        .poolSizeCount = 6U,
        .pPoolSizes = core_pool_sizes,
    };
    VkDescriptorPool core_pool = VK_NULL_HANDLE;
    CHECK(create_descriptor_pool(
              device, &core_pool_info, NULL, &core_pool) == VK_SUCCESS);
    destroy_descriptor_pool(device, core_pool, NULL);
    destroy_descriptor_set_layout(device, core_layout, NULL);

    const VkDescriptorType dxvk_template_types[] = {
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
    };
    const VkShaderStageFlags dxvk_template_stages[] = {
        VK_SHADER_STAGE_VERTEX_BIT,
        VK_SHADER_STAGE_VERTEX_BIT,
        VK_SHADER_STAGE_VERTEX_BIT,
        VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutBinding dxvk_template_bindings[4] = {0};
    VkDescriptorUpdateTemplateEntry dxvk_template_entries[4] = {0};
    for (uint32_t index = 0U; index < 4U; ++index) {
        dxvk_template_bindings[index] = (VkDescriptorSetLayoutBinding){
            .binding = index,
            .descriptorType = dxvk_template_types[index],
            .descriptorCount = 1U,
            .stageFlags = dxvk_template_stages[index],
        };
        dxvk_template_entries[index] = (VkDescriptorUpdateTemplateEntry){
            .dstBinding = index,
            .descriptorCount = 1U,
            .descriptorType = dxvk_template_types[index],
            .offset = 24U * index,
            .stride = 24U,
        };
    }
    const VkDescriptorSetLayoutCreateInfo dxvk_template_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 4U,
        .pBindings = dxvk_template_bindings,
    };
    VkDescriptorSetLayout dxvk_template_layout = VK_NULL_HANDLE;
    CHECK(create_descriptor_set_layout(
              device, &dxvk_template_layout_info, NULL,
              &dxvk_template_layout) == VK_SUCCESS);
    VkDescriptorUpdateTemplateCreateInfo dxvk_template_info = {
        .sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
        .descriptorUpdateEntryCount = 4U,
        .pDescriptorUpdateEntries = dxvk_template_entries,
        .templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET,
        .descriptorSetLayout = dxvk_template_layout,
    };
    VkDescriptorUpdateTemplate dxvk_template =
        (VkDescriptorUpdateTemplate)(uintptr_t)1U;
    CHECK(create_descriptor_update_template(
              device, &dxvk_template_info, &unsupported_allocator,
              &dxvk_template) == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(dxvk_template == VK_NULL_HANDLE);
    const uint32_t unsupported_template_chain = UINT32_C(0x7ffffffc);
    dxvk_template_info.pNext = &unsupported_template_chain;
    CHECK(create_descriptor_update_template(
              device, &dxvk_template_info, NULL,
              &dxvk_template) == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(dxvk_template == VK_NULL_HANDLE);
    dxvk_template_info.pNext = NULL;
    CHECK(create_descriptor_update_template(
              ownership_device, &dxvk_template_info, NULL,
              &dxvk_template) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(dxvk_template == VK_NULL_HANDLE);
    CHECK(create_descriptor_update_template(
              device, &dxvk_template_info, NULL,
              &dxvk_template) == VK_SUCCESS);
    uint64_t dxvk_template_id = 0U;
    memcpy(&dxvk_template_id, &dxvk_template, sizeof(dxvk_template));
    CHECK(bvb_handle_type(dxvk_template_id) ==
          BVB_OBJECT_DESCRIPTOR_UPDATE_TEMPLATE);
    const VkDescriptorPoolSize dxvk_template_pool_sizes[3] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2U},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1U},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1U},
    };
    const VkDescriptorPoolCreateInfo dxvk_template_pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1U,
        .poolSizeCount = 3U,
        .pPoolSizes = dxvk_template_pool_sizes,
    };
    VkDescriptorPool dxvk_template_pool = VK_NULL_HANDLE;
    CHECK(create_descriptor_pool(
              device, &dxvk_template_pool_info, NULL,
              &dxvk_template_pool) == VK_SUCCESS);
    const VkDescriptorSetAllocateInfo dxvk_template_allocate_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dxvk_template_pool,
        .descriptorSetCount = 1U,
        .pSetLayouts = &dxvk_template_layout,
    };
    VkDescriptorSet dxvk_template_set = VK_NULL_HANDLE;
    CHECK(allocate_descriptor_sets(
              device, &dxvk_template_allocate_info,
              &dxvk_template_set) == VK_SUCCESS);
    uint8_t dxvk_template_data[96] = {0};
    for (uint32_t index = 0U; index < 2U; ++index) {
        const VkDescriptorBufferInfo null_buffer = {
            .range = VK_WHOLE_SIZE,
        };
        memcpy(dxvk_template_data + index * 24U, &null_buffer,
               sizeof(null_buffer));
    }
    const VkDescriptorImageInfo null_image = {
        .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    memcpy(dxvk_template_data + 72U, &null_image, sizeof(null_image));
    update_descriptor_set_with_template(
        device, dxvk_template_set, dxvk_template, dxvk_template_data);
    const VkDescriptorImageInfo virtual_descriptor = {
        .imageView = virtual_image_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    memcpy(dxvk_template_data + 72U, &virtual_descriptor,
           sizeof(virtual_descriptor));
    update_descriptor_set_with_template(
        device, dxvk_template_set, dxvk_template, dxvk_template_data);
    destroy_descriptor_pool(device, dxvk_template_pool, NULL);
    /* Pinned DXVK destroys the layout before its update template. */
    destroy_descriptor_set_layout(device, dxvk_template_layout, NULL);
    destroy_descriptor_update_template(device, dxvk_template, NULL);
    destroy_image_view(device, virtual_image_view, NULL);
    CHECK(bvb_image_view_proxy_id(virtual_image_view) == 0U);
    destroy_swapchain(device, virtual_swapchain, NULL);
    core_bindings[0].descriptorType =
        VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    core_layout = (VkDescriptorSetLayout)(uintptr_t)1U;
    CHECK(create_descriptor_set_layout(
              device, &core_layout_info, NULL, &core_layout) ==
          VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(core_layout == VK_NULL_HANDLE);

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

    const VkSpecializationMapEntry builtin_specialization_entries[8] = {
        {0U, 0U, 4U}, {1U, 4U, 4U}, {2U, 8U, 4U}, {3U, 12U, 4U},
        {4U, 16U, 4U}, {5U, 20U, 4U}, {6U, 24U, 4U}, {7U, 28U, 4U},
    };
    const uint32_t builtin_specialization_data[8] = {
        10U, 11U, 12U, 13U, 14U, 15U, 16U, 17U,
    };
    const VkSpecializationInfo builtin_specialization = {
        .mapEntryCount = 8U,
        .pMapEntries = builtin_specialization_entries,
        .dataSize = sizeof(builtin_specialization_data),
        .pData = builtin_specialization_data,
    };
    const VkShaderModuleCreateInfo builtin_modules[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(test_builtin_vertex),
            .pCode = test_builtin_vertex,
        },
        {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(test_builtin_fragment),
            .pCode = test_builtin_fragment,
        },
    };
    const VkPipelineShaderStageCreateInfo builtin_stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = &builtin_modules[0],
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = &builtin_modules[1],
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pName = "main",
            .pSpecializationInfo = &builtin_specialization,
        },
    };
    const VkFormat builtin_color_format = VK_FORMAT_R8G8B8A8_UNORM;
    const VkPipelineRenderingCreateInfo builtin_rendering = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1U,
        .pColorAttachmentFormats = &builtin_color_format,
    };
    const VkPipelineVertexInputStateCreateInfo builtin_vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    const VkPipelineInputAssemblyStateCreateInfo builtin_input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    const VkPipelineViewportStateCreateInfo builtin_viewport = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
    };
    const VkPipelineRasterizationStateCreateInfo builtin_rasterization = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0F,
    };
    const VkSampleMask builtin_sample_mask = 1U;
    const VkPipelineMultisampleStateCreateInfo builtin_multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        .minSampleShading = 1.0F,
        .pSampleMask = &builtin_sample_mask,
    };
    const VkPipelineColorBlendAttachmentState builtin_blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
            VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
            VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo builtin_blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1U,
        .pAttachments = &builtin_blend_attachment,
    };
    const VkDynamicState builtin_dynamic_states[2] = {
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,
        VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
    };
    const VkPipelineDynamicStateCreateInfo builtin_dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2U,
        .pDynamicStates = builtin_dynamic_states,
    };
    const VkGraphicsPipelineCreateInfo builtin_pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &builtin_rendering,
        .stageCount = 2U,
        .pStages = builtin_stages,
        .pVertexInputState = &builtin_vertex_input,
        .pInputAssemblyState = &builtin_input_assembly,
        .pViewportState = &builtin_viewport,
        .pRasterizationState = &builtin_rasterization,
        .pMultisampleState = &builtin_multisample,
        .pColorBlendState = &builtin_blend,
        .pDynamicState = &builtin_dynamic,
        .layout = pipeline_layout,
        .basePipelineIndex = -1,
    };
    VkPipeline builtin_pipeline = VK_NULL_HANDLE;
    CHECK(create_graphics_pipelines(
              device, VK_NULL_HANDLE, 1U, &builtin_pipeline_info, NULL,
              &builtin_pipeline) == VK_SUCCESS);
    uint64_t builtin_pipeline_id = 0U;
    memcpy(&builtin_pipeline_id, &builtin_pipeline, sizeof(builtin_pipeline));
    CHECK(bvb_handle_type(builtin_pipeline_id) == BVB_OBJECT_PIPELINE);
    CHECK(builtin_pipeline_id != graphics_pipeline_id);

    const VkShaderModuleCreateInfo general_modules[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(test_dxvk_dummy_frag),
            .pCode = test_dxvk_dummy_frag,
        },
        {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(test_dxvk_dummy_frag),
            .pCode = test_dxvk_dummy_frag,
        },
    };
    const VkPipelineShaderStageCreateInfo general_stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = &general_modules[0],
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = &general_modules[1],
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pName = "main",
        },
    };
    const VkVertexInputBindingDescription general_binding = {
        .binding = 0U,
        .stride = 20U,
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    const VkVertexInputAttributeDescription general_attributes[3] = {
        {.location = 0U, .binding = 0U, .format = VK_FORMAT_R32G32_SFLOAT,
         .offset = 0U},
        {.location = 1U, .binding = 0U, .format = VK_FORMAT_R32G32_SFLOAT,
         .offset = 8U},
        {.location = 2U, .binding = 0U, .format = VK_FORMAT_R32_SFLOAT,
         .offset = 16U},
    };
    const VkPipelineVertexInputStateCreateInfo general_vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1U,
        .pVertexBindingDescriptions = &general_binding,
        .vertexAttributeDescriptionCount = 3U,
        .pVertexAttributeDescriptions = general_attributes,
    };
    const VkPipelineInputAssemblyStateCreateInfo general_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
        .primitiveRestartEnable = VK_TRUE,
    };
    const VkPipelineRasterizationDepthClipStateCreateInfoEXT
        general_depth_clip = {
            .sType =
                VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT,
            .depthClipEnable = VK_TRUE,
        };
    const VkPipelineRasterizationStateCreateInfo general_raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pNext = &general_depth_clip,
        .depthClampEnable = VK_TRUE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0F,
    };
    const VkPipelineDepthStencilStateCreateInfo general_depth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
    };
    const VkPipelineColorBlendStateCreateInfo general_blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOp = (VkLogicOp)5,
    };
    const VkDynamicState general_dynamic_states[7] = {
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,
        VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
        VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
    };
    const VkPipelineDynamicStateCreateInfo general_dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 7U,
        .pDynamicStates = general_dynamic_states,
    };
    const VkPipelineRenderingCreateInfo general_rendering = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
    };
    const VkGraphicsPipelineCreateInfo general_pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &general_rendering,
        .stageCount = 2U,
        .pStages = general_stages,
        .pVertexInputState = &general_vertex_input,
        .pInputAssemblyState = &general_assembly,
        .pViewportState = &builtin_viewport,
        .pRasterizationState = &general_raster,
        .pMultisampleState = &builtin_multisample,
        .pDepthStencilState = &general_depth,
        .pColorBlendState = &general_blend,
        .pDynamicState = &general_dynamic,
        .layout = pipeline_layout,
        .basePipelineIndex = -1,
    };
    VkPipeline general_pipeline = VK_NULL_HANDLE;
    CHECK(create_graphics_pipelines(
              device, VK_NULL_HANDLE, 1U, &general_pipeline_info, NULL,
              &general_pipeline) == VK_SUCCESS);
    uint64_t general_pipeline_id = 0U;
    memcpy(&general_pipeline_id, &general_pipeline,
           sizeof(general_pipeline_id));
    CHECK(bvb_handle_type(general_pipeline_id) == BVB_OBJECT_PIPELINE);
    CHECK(general_pipeline_id != builtin_pipeline_id);

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
    if (!hardware_mode) {
        CHECK(bvb_handle_serial(command_pool_id) ==
              (animated_wsi ? 2U : 1U));
    }
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
    if (!hardware_mode) {
        CHECK(bvb_handle_serial(command_buffer_id) ==
              (animated_wsi ? 2U : 1U));
    }
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
    if (hardware_mode) {
        CHECK(bvb_memory_requirements_are_valid(
            &device_buffer_requirements.memoryRequirements,
            device_buffer_create_info.size, memory.memoryTypeCount));
        CHECK(device_buffer_dedicated_requirements
                  .prefersDedicatedAllocation <= VK_TRUE);
        CHECK(device_buffer_dedicated_requirements
                  .requiresDedicatedAllocation <= VK_TRUE);
    } else {
        CHECK(device_buffer_requirements.memoryRequirements.size == 65792U);
        CHECK(device_buffer_requirements.memoryRequirements.alignment == 256U);
        CHECK(device_buffer_requirements.memoryRequirements.memoryTypeBits ==
              5U);
        CHECK(device_buffer_dedicated_requirements
                  .prefersDedicatedAllocation == VK_FALSE);
        CHECK(device_buffer_dedicated_requirements
                  .requiresDedicatedAllocation == VK_TRUE);
    }
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
    if (!hardware_mode) {
        CHECK(bvb_handle_serial(buffer_id) == 1U);
    }
    const VkBufferMemoryRequirementsInfo2 buffer_requirements_info_2 = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
        .buffer = buffer,
    };
    VkMemoryRequirements2 buffer_requirements_2 = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
    };
    get_buffer_memory_requirements_2(
        device, &buffer_requirements_info_2, &buffer_requirements_2);
    if (hardware_mode) {
        CHECK(bvb_memory_requirements_are_valid(
            &buffer_requirements_2.memoryRequirements,
            buffer_create_info.size, memory.memoryTypeCount));
    } else {
        CHECK(buffer_requirements_2.memoryRequirements.size == 4096U);
        CHECK(buffer_requirements_2.memoryRequirements.alignment == 256U);
        CHECK(buffer_requirements_2.memoryRequirements.memoryTypeBits == 1U);
    }
    const VkMemoryRequirements first_buffer_requirements_2 =
        buffer_requirements_2.memoryRequirements;
    VkMemoryDedicatedRequirements buffer_dedicated_requirements = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
    };
    buffer_requirements_2.pNext = &buffer_dedicated_requirements;
    get_buffer_memory_requirements_2(
        device, &buffer_requirements_info_2, &buffer_requirements_2);
    if (hardware_mode) {
        CHECK(bvb_memory_requirements_match(
            &buffer_requirements_2.memoryRequirements,
            &first_buffer_requirements_2));
        CHECK(buffer_dedicated_requirements.prefersDedicatedAllocation <=
              VK_TRUE);
        CHECK(buffer_dedicated_requirements.requiresDedicatedAllocation <=
              VK_TRUE);
    } else {
        CHECK(buffer_requirements_2.memoryRequirements.size == 4096U);
        CHECK(buffer_dedicated_requirements.prefersDedicatedAllocation ==
              VK_TRUE);
        CHECK(buffer_dedicated_requirements.requiresDedicatedAllocation ==
              VK_FALSE);
    }
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
        const VkMemoryPropertyFlags required_flags =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            (noncoherent_mapped_memory
                 ? 0U : VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if ((requirements.memoryTypeBits & (1U << index)) != 0U &&
            (flags & required_flags) == required_flags &&
            (!noncoherent_mapped_memory ||
             (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0U)) {
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
    if (!hardware_mode) {
        CHECK(bvb_handle_serial(memory_id) == 1U);
    }
    CHECK(bind_buffer_memory(device, buffer, device_memory, 0U) == VK_SUCCESS);
    VkBuffer upload_buffer = VK_NULL_HANDLE;
    VkDeviceMemory upload_memory = VK_NULL_HANDLE;
    VkDeviceSize upload_memory_size = 0U;
    if (shared_mapped_memory) {
        const VkBufferCreateInfo upload_buffer_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 4096U,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        CHECK(create_buffer(device, &upload_buffer_info, NULL,
                            &upload_buffer) == VK_SUCCESS);
        VkMemoryRequirements upload_requirements = {0};
        get_buffer_memory_requirements(
            device, upload_buffer, &upload_requirements);
        upload_memory_size = upload_requirements.size;
        const VkMemoryAllocateInfo upload_allocate_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = upload_requirements.size,
            .memoryTypeIndex = memory_type_index,
        };
        CHECK(allocate_memory(device, &upload_allocate_info, NULL,
                              &upload_memory) == VK_SUCCESS);
        CHECK(bind_buffer_memory(device, upload_buffer, upload_memory, 0U) ==
              VK_SUCCESS);
    }
    VkBufferDeviceAddressInfo buffer_address_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = buffer,
    };
    const VkDeviceAddress buffer_device_address =
        get_buffer_device_address(device, &buffer_address_info);
    if (hardware_mode) {
        CHECK(buffer_device_address != 0U);
        CHECK(get_buffer_device_address(device, &buffer_address_info) ==
              buffer_device_address);
    } else {
        CHECK(buffer_device_address == UINT64_C(0x123456780000));
    }
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
    if (hardware_mode) {
        CHECK(bvb_memory_requirements_are_valid(
            &image_requirements, 64U * 64U * sizeof(uint32_t),
            memory.memoryTypeCount));
    } else {
        CHECK(image_requirements.size == 64U * 64U * sizeof(uint32_t));
        CHECK(image_requirements.alignment == 4096U);
        CHECK(image_requirements.memoryTypeBits == 1U);
    }

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
    if (hardware_mode) {
        CHECK(bvb_memory_requirements_are_valid(
            &image_requirements_2.memoryRequirements,
            64U * 64U * sizeof(uint32_t), memory.memoryTypeCount));
        CHECK(bvb_memory_requirements_match(
            &image_requirements_2.memoryRequirements, &image_requirements));
        CHECK(dedicated_requirements.prefersDedicatedAllocation <= VK_TRUE);
        CHECK(dedicated_requirements.requiresDedicatedAllocation <= VK_TRUE);
    } else {
        CHECK(image_requirements_2.memoryRequirements.size ==
              UINT64_C(19623936));
        CHECK(image_requirements_2.memoryRequirements.alignment == 4096U);
        CHECK(image_requirements_2.memoryRequirements.memoryTypeBits == 1U);
        CHECK(dedicated_requirements.prefersDedicatedAllocation == VK_TRUE);
        CHECK(dedicated_requirements.requiresDedicatedAllocation == VK_TRUE);
    }

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
    if (!hardware_mode) {
        CHECK(bvb_handle_serial(image_view_id) == 2U);
    }

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
    const VkDeviceMemory mapped_memory =
        shared_mapped_memory ? upload_memory : device_memory;
    const VkDeviceSize mapped_size =
        shared_mapped_memory ? upload_memory_size : requirements.size;
    uint8_t *mapped = NULL;
    const uint64_t exchanges_before_map =
        bvb_global_dispatch_exchange_count();
    const VkBaseInStructure unsupported_map_tail = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    };
    VkMemoryMapInfo map_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO,
        .pNext = &unsupported_map_tail,
        .memory = mapped_memory,
        .offset = 0U,
        .size = VK_WHOLE_SIZE,
    };
    CHECK(map_memory_2(device, &map_info, (void **)&mapped) ==
          VK_ERROR_MEMORY_MAP_FAILED);
    CHECK(mapped == NULL);
    map_info.pNext = NULL;
    CHECK(map_memory_2(device, &map_info, (void **)&mapped) == VK_SUCCESS);
    CHECK(mapped != NULL);
    CHECK((uintptr_t)mapped <= UINT32_MAX);
    CHECK(mapped_size - 1U <= UINT32_MAX - (uintptr_t)mapped);
    if (noncoherent_mapped_memory) CHECK(mapped[0] == UINT8_C(0x00));
    const uint64_t map_rtts =
        bvb_global_dispatch_exchange_count() - exchanges_before_map;
    const uint16_t map_opcode = bvb_global_dispatch_last_opcode();
    uint64_t ineligible_map_rtts = 0U;
    uint64_t ineligible_unmap_rtts = 0U;
    uint16_t ineligible_map_opcode = 0U;
    uint16_t ineligible_unmap_opcode = 0U;
    if (shared_mapped_memory) {
        void *ineligible_mapping = NULL;
        const uint64_t before_ineligible_map =
            bvb_global_dispatch_exchange_count();
        CHECK(map_memory(device, device_memory, 0U, VK_WHOLE_SIZE, 0U,
                         &ineligible_mapping) == VK_SUCCESS);
        CHECK(ineligible_mapping != NULL);
        CHECK((uintptr_t)ineligible_mapping <= UINT32_MAX);
        ineligible_map_rtts = bvb_global_dispatch_exchange_count() -
                              before_ineligible_map;
        ineligible_map_opcode = bvb_global_dispatch_last_opcode();
        const uint64_t before_ineligible_unmap =
            bvb_global_dispatch_exchange_count();
        unmap_memory(device, device_memory);
        ineligible_unmap_rtts = bvb_global_dispatch_exchange_count() -
                                before_ineligible_unmap;
        ineligible_unmap_opcode = bvb_global_dispatch_last_opcode();
        const VkBufferCreateInfo unsafe_buffer_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 4096U,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VkBuffer unsafe_buffer = VK_NULL_HANDLE;
        CHECK(create_buffer(device, &unsafe_buffer_info, NULL,
                            &unsafe_buffer) == VK_SUCCESS);
        CHECK(bind_buffer_memory(device, unsafe_buffer, upload_memory, 0U) !=
              VK_SUCCESS);
        destroy_buffer(device, unsafe_buffer, NULL);
        VkImage unsafe_image = VK_NULL_HANDLE;
        CHECK(create_image(device, &image_create_info, NULL, &unsafe_image) ==
              VK_SUCCESS);
        CHECK(bind_image_memory(device, unsafe_image, upload_memory, 0U) !=
              VK_SUCCESS);
        destroy_image(device, unsafe_image, NULL);
    }
    uint8_t expected_mapping[4096];
    for (size_t index = 0U; index < sizeof(expected_mapping); ++index) {
        expected_mapping[index] = (uint8_t)(index ^ (index >> 4));
    }
    memcpy(mapped, expected_mapping, sizeof(expected_mapping));
    const VkMappedMemoryRange mapped_range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = mapped_memory,
        .offset = 0U,
        .size = VK_WHOLE_SIZE,
    };
    const uint64_t exchanges_before_flush =
        bvb_global_dispatch_exchange_count();
    CHECK(flush_mapped_memory_ranges(device, 1U, &mapped_range) ==
          VK_SUCCESS);
    const uint64_t flush_rtts =
        bvb_global_dispatch_exchange_count() - exchanges_before_flush;
    const uint16_t flush_opcode = bvb_global_dispatch_last_opcode();
    memset(mapped, 0, sizeof(expected_mapping));
    const uint64_t exchanges_before_invalidate =
        bvb_global_dispatch_exchange_count();
    CHECK(invalidate_mapped_memory_ranges(device, 1U, &mapped_range) ==
          VK_SUCCESS);
    const uint64_t invalidate_rtts =
        bvb_global_dispatch_exchange_count() - exchanges_before_invalidate;
    const uint16_t invalidate_opcode = bvb_global_dispatch_last_opcode();
    uint32_t mapped_mismatches = 0U;
    for (size_t index = 0U; index < sizeof(expected_mapping); ++index) {
        if (mapped[index] != expected_mapping[index]) ++mapped_mismatches;
    }
    CHECK(mapped_mismatches == 0U);
    if (noncoherent_mapped_memory) {
        const VkMappedMemoryRange partial_range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = mapped_memory,
            .offset = 257U,
            .size = 7U,
        };
        memset(mapped + 257U, UINT8_C(0x66), 7U);
        CHECK(flush_mapped_memory_ranges(device, 1U, &partial_range) ==
              VK_SUCCESS);
        memset(mapped + 257U, 0, 7U);
        CHECK(invalidate_mapped_memory_ranges(
                  device, 1U, &partial_range) == VK_SUCCESS);
        for (size_t index = 257U; index < 264U; ++index) {
            CHECK(mapped[index] == UINT8_C(0x66));
            expected_mapping[index] = UINT8_C(0x66);
        }
    }
    uint64_t unmap_rtts = 0U;
    uint16_t unmap_opcode = 0U;
    if (!keep_mapped_memory) {
        const uint64_t exchanges_before_unmap =
            bvb_global_dispatch_exchange_count();
        unmap_memory(device, mapped_memory);
        unmap_rtts =
            bvb_global_dispatch_exchange_count() - exchanges_before_unmap;
        unmap_opcode = bvb_global_dispatch_last_opcode();
    }
    const VkFenceCreateInfo fence_create_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    VkFence fence = VK_NULL_HANDLE;
    CHECK(create_fence(device, &fence_create_info, NULL, &fence) == VK_SUCCESS);
    const uint64_t fence_id = bvb_fence_proxy_id(fence);
    CHECK(bvb_handle_type(fence_id) == BVB_OBJECT_FENCE);
    if (!hardware_mode) {
        CHECK(bvb_handle_serial(fence_id) == 1U);
    }
    CHECK(get_fence_status(device, fence) == VK_NOT_READY);
    const VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = 0U,
    };
    if (shared_command_stream) {
        CHECK(begin_command_buffer(command_buffer, &begin_info) == VK_SUCCESS);
        const VkClearColorValue unsupported_color = {
            .uint32 = {1U, 0U, 0U, 0U},
        };
        const VkImageSubresourceRange poison_range = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1U,
            .layerCount = 1U,
        };
        cmd_clear_color_image(command_buffer, image,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              &unsupported_color, 1U, &poison_range);
        CHECK(end_command_buffer(command_buffer) ==
              VK_ERROR_INITIALIZATION_FAILED);

        VkBuffer ownership_buffer = VK_NULL_HANDLE;
        CHECK(create_buffer(ownership_device, &buffer_create_info, NULL,
                            &ownership_buffer) == VK_SUCCESS);
        CHECK(begin_command_buffer(command_buffer, &begin_info) == VK_SUCCESS);
        cmd_fill_buffer(command_buffer, ownership_buffer, 0U, 4096U,
                        UINT32_C(0xa5c3f00d));
        CHECK(bvb_command_buffer_ownership_registry_reads(command_buffer) ==
              1U);
        CHECK(end_command_buffer(command_buffer) ==
              VK_ERROR_INITIALIZATION_FAILED);
        destroy_buffer(ownership_device, ownership_buffer, NULL);

        VkImage ownership_image = VK_NULL_HANDLE;
        CHECK(create_image(ownership_device, &image_create_info, NULL,
                           &ownership_image) == VK_SUCCESS);
        CHECK(begin_command_buffer(command_buffer, &begin_info) == VK_SUCCESS);
        cmd_clear_color_image(command_buffer, ownership_image,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              &unsupported_color, 1U, &poison_range);
        CHECK(bvb_command_buffer_ownership_registry_reads(command_buffer) ==
              1U);
        CHECK(end_command_buffer(command_buffer) ==
              VK_ERROR_INITIALIZATION_FAILED);
        destroy_image(ownership_device, ownership_image, NULL);
    }
    const uint64_t exchanges_before_recording =
        bvb_global_dispatch_exchange_count();
    CHECK(exchanges_before_recording != UINT64_MAX);
    CHECK(begin_command_buffer(command_buffer, &begin_info) == VK_SUCCESS);
    const VkAttachmentFeedbackLoopInfoEXT render_feedback = {
        .sType = VK_STRUCTURE_TYPE_ATTACHMENT_FEEDBACK_LOOP_INFO_EXT,
        .feedbackLoopEnable = VK_TRUE,
    };
    const VkRenderingAttachmentInfo render_attachments[2] = {{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = &render_feedback,
        .imageView = image_view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT,
        .resolveImageView = image_view,
        .resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    }, {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = VK_NULL_HANDLE,
        .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
    }};
    const VkRenderingAttachmentInfo depth_attachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = image_view,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue.depthStencil = {.depth = 0.5F, .stencil = 7U},
    };
    const VkRenderingInfo render_info = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {.offset = {3, 4}, .extent = {64U, 64U}},
        .layerCount = 1U,
        .viewMask = 3U,
        .colorAttachmentCount = 2U,
        .pColorAttachments = render_attachments,
        .pDepthAttachment = &depth_attachment,
        .pStencilAttachment = &depth_attachment,
    };
    cmd_begin_rendering(command_buffer, &render_info);
    cmd_bind_pipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      builtin_pipeline);
    const VkViewport render_viewport = {
        .x = 0.0F, .y = 0.0F, .width = 64.0F, .height = 64.0F,
        .minDepth = 0.0F, .maxDepth = 1.0F,
    };
    cmd_set_viewport_with_count(command_buffer, 1U, &render_viewport);
    const VkRect2D render_scissor = {
        .offset = {0, 0}, .extent = {64U, 64U},
    };
    cmd_set_scissor_with_count(command_buffer, 1U, &render_scissor);
    cmd_bind_descriptor_sets(
        command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
        0U, 1U, &descriptor_set, 0U, NULL);
    const uint32_t render_constants[4] = {1U, 2U, 3U, 4U};
    cmd_push_constants(command_buffer, pipeline_layout,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0U,
                       sizeof(render_constants), render_constants);
    cmd_draw(command_buffer, 3U, 1U, 0U, 0U);
    const VkBuffer vertex_buffers[2] = {buffer, VK_NULL_HANDLE};
    const VkDeviceSize vertex_offsets[2] = {128U, 0U};
    const VkDeviceSize vertex_sizes[2] = {256U, VK_WHOLE_SIZE};
    const VkDeviceSize vertex_strides[2] = {32U, 0U};
    cmd_bind_vertex_buffers(
        command_buffer, 2U, 1U, vertex_buffers, vertex_offsets);
    cmd_bind_vertex_buffers_2(
        command_buffer, 3U, 2U, vertex_buffers, vertex_offsets,
        vertex_sizes, vertex_strides);
    cmd_bind_index_buffer(
        command_buffer, buffer, 32U, VK_INDEX_TYPE_UINT16);
    cmd_bind_index_buffer_2(
        command_buffer, buffer, 64U, 512U, VK_INDEX_TYPE_UINT32);
    cmd_draw_indexed(command_buffer, 6U, 2U, 1U, -3, 4U);
    cmd_draw_indirect(command_buffer, buffer, 128U, 2U, 16U);
    cmd_draw_indexed_indirect(command_buffer, buffer, 256U, 3U, 20U);
    cmd_draw_indirect_count(
        command_buffer, buffer, 384U, buffer, 12U, 4U, 16U);
    cmd_draw_indexed_indirect_count(
        command_buffer, buffer, 512U, buffer, 16U, 5U, 20U);
    cmd_end_rendering(command_buffer);
    cmd_fill_buffer(command_buffer, buffer, 0U, 4096U, UINT32_C(0xa5c3f00d));
    const VkImageSubresourceRange init_image_range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .levelCount = 1U,
        .layerCount = 1U,
    };
    VkClearColorValue rejected_init_color = {
        .uint32 = {1U, 0U, 0U, 0U},
    };
    if (!shared_command_stream)
        cmd_clear_color_image(command_buffer, image,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              &rejected_init_color, 1U, &init_image_range);
    const VkClearColorValue init_color = {0};
    cmd_clear_color_image(command_buffer, image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          &init_color, 1U, &init_image_range);
    VkImageMemoryBarrier2 init_image_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = VK_ACCESS_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1U,
            .layerCount = 1U,
        },
    };
    const VkMemoryBarrier2 broad_memory_barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT,
    };
    const VkBufferMemoryBarrier2 broad_buffer_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
        .srcQueueFamilyIndex = 2U,
        .dstQueueFamilyIndex = 3U,
        .buffer = buffer,
        .offset = 128U,
        .size = 256U,
    };
    const VkDependencyInfo init_dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
        .memoryBarrierCount = 1U,
        .pMemoryBarriers = &broad_memory_barrier,
        .bufferMemoryBarrierCount = 1U,
        .pBufferMemoryBarriers = &broad_buffer_barrier,
        .imageMemoryBarrierCount = 1U,
        .pImageMemoryBarriers = &init_image_barrier,
    };
    cmd_pipeline_barrier_2(command_buffer, &init_dependency);
    CHECK(end_command_buffer(command_buffer) == VK_SUCCESS);
    if (shared_command_stream) {
        CHECK(bvb_command_buffer_ownership_registry_reads(command_buffer) ==
              6U);
    }
    const uint64_t exchanges_after_recording =
        bvb_global_dispatch_exchange_count();
    CHECK(exchanges_after_recording >= exchanges_before_recording);
    const uint64_t recording_rtts =
        exchanges_after_recording - exchanges_before_recording;
    CHECK(recording_rtts == (shared_command_stream ? 0U : 22U));
    if (shared_mapped_memory) mapped[0] = UINT8_C(0x7b);
    const uint64_t exchanges_before_submit =
        bvb_global_dispatch_exchange_count();
    if (shared_command_stream) {
        const VkCommandBufferSubmitInfo replay_command = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = command_buffer,
        };
        const VkSubmitInfo2 replay_submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .commandBufferInfoCount = 1U,
            .pCommandBufferInfos = &replay_command,
        };
        CHECK(queue_submit_2(queue, 1U, &replay_submit, fence) == VK_SUCCESS);
    } else {
        const VkSubmitInfo command_submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1U,
            .pCommandBuffers = &command_buffer,
        };
        CHECK(queue_submit(queue, 1U, &command_submit, fence) == VK_SUCCESS);
    }
    const uint64_t submit_rtts =
        bvb_global_dispatch_exchange_count() - exchanges_before_submit;
    const uint16_t submit_opcode = bvb_global_dispatch_last_opcode();
    CHECK(get_fence_status(device, fence) == VK_SUCCESS);
    CHECK(wait_for_fences(device, 1U, &fence, VK_TRUE, UINT64_MAX) ==
          VK_SUCCESS);
    uint32_t mismatched_words = 0U;
    if (shared_mapped_memory) {
        CHECK(invalidate_mapped_memory_ranges(
                  device, 1U, &mapped_range) == VK_SUCCESS);
        for (size_t index = 0U; index < sizeof(expected_mapping); ++index) {
            const uint8_t expected = index == 0U
                ? (noncoherent_mapped_memory ? UINT8_C(0x00)
                                             : UINT8_C(0x7b))
                : expected_mapping[index];
            if (mapped[index] != expected) ++mismatched_words;
        }
        CHECK(mismatched_words == 0U);
    }
    mismatched_words = UINT32_MAX;
    CHECK(bvb_verify_memory_fill(
              device_memory, 0U, 4096U, UINT32_C(0xa5c3f00d),
              &mismatched_words) == 0);
    CHECK(mismatched_words == 0U);
    uint32_t concurrent_streams = 0U;
    uint32_t concurrent_commands = 0U;
    uint64_t concurrent_ownership_registry_reads = 0U;
    uint64_t collision_ownership_registry_reads = 0U;
    uint64_t rerecord_ownership_registry_reads = 0U;
    uint64_t pool_reset_ownership_registry_reads = 0U;
    bool stale_resource_rejected = false;
    bool stale_native_replay_blocked = false;
    if (concurrent_command_stream) {
        VkCommandBuffer concurrent[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        CHECK(allocate_command_buffers(device, &allocate_info,
                                       &concurrent[0]) == VK_SUCCESS);
        CHECK(allocate_command_buffers(device, &allocate_info,
                                       &concurrent[1]) == VK_SUCCESS);
        pthread_barrier_t start;
        CHECK(pthread_barrier_init(&start, NULL, 3U) == 0);
        struct bvb_concurrent_record_context contexts[2] = {0};
        pthread_t threads[2];
        for (uint32_t index = 0U; index < 2U; ++index) {
            contexts[index] = (struct bvb_concurrent_record_context){
                .start = &start,
                .begin_command_buffer = begin_command_buffer,
                .cmd_fill_buffer = cmd_fill_buffer,
                .end_command_buffer = end_command_buffer,
                .command_buffer = concurrent[index],
                .buffer = buffer,
                .begin_result = VK_ERROR_INITIALIZATION_FAILED,
                .end_result = VK_ERROR_INITIALIZATION_FAILED,
            };
            CHECK(pthread_create(&threads[index], NULL,
                                 bvb_record_shared_command_buffer,
                                 &contexts[index]) == 0);
        }
        const uint64_t exchanges_before_concurrent =
            bvb_global_dispatch_exchange_count();
        const int barrier_result = pthread_barrier_wait(&start);
        CHECK(barrier_result == 0 ||
              barrier_result == PTHREAD_BARRIER_SERIAL_THREAD);
        for (uint32_t index = 0U; index < 2U; ++index)
            CHECK(pthread_join(threads[index], NULL) == 0);
        CHECK(pthread_barrier_destroy(&start) == 0);
        CHECK(bvb_global_dispatch_exchange_count() ==
              exchanges_before_concurrent);
        for (uint32_t index = 0U; index < 2U; ++index) {
            CHECK(contexts[index].begin_result == VK_SUCCESS);
            CHECK(contexts[index].end_result == VK_SUCCESS);
            const uint64_t reads =
                bvb_command_buffer_ownership_registry_reads(
                    concurrent[index]);
            CHECK(reads == 1U);
            concurrent_ownership_registry_reads += reads;
        }
        const VkCommandBufferSubmitInfo concurrent_infos[2] = {
            {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .commandBuffer = concurrent[0],
            },
            {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .commandBuffer = concurrent[1],
            },
        };
        const VkSubmitInfo2 concurrent_submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .commandBufferInfoCount = 2U,
            .pCommandBufferInfos = concurrent_infos,
        };
        CHECK(queue_submit_2(queue, 1U, &concurrent_submit,
                             VK_NULL_HANDLE) == VK_SUCCESS);
        CHECK(queue_wait_idle(queue) == VK_SUCCESS);

        VkBuffer collision_buffers[17] = {VK_NULL_HANDLE};
        for (uint32_t index = 0U; index < 17U; ++index) {
            CHECK(create_buffer(device, &buffer_create_info, NULL,
                                &collision_buffers[index]) == VK_SUCCESS);
        }
        VkCommandPool cache_pool = VK_NULL_HANDLE;
        CHECK(create_command_pool(device, &pool_create_info, NULL,
                                  &cache_pool) == VK_SUCCESS);
        VkCommandBufferAllocateInfo cache_allocate_info = allocate_info;
        cache_allocate_info.commandPool = cache_pool;
        VkCommandBuffer cache_command = VK_NULL_HANDLE;
        CHECK(allocate_command_buffers(device, &cache_allocate_info,
                                       &cache_command) == VK_SUCCESS);
        CHECK(begin_command_buffer(cache_command, &begin_info) == VK_SUCCESS);
        cmd_fill_buffer(cache_command, collision_buffers[0], 0U, 4096U, 1U);
        cmd_fill_buffer(cache_command, collision_buffers[0], 0U, 4096U, 2U);
        CHECK(bvb_command_buffer_ownership_registry_reads(cache_command) ==
              1U);
        cmd_fill_buffer(cache_command, collision_buffers[1], 0U, 4096U, 3U);
        cmd_fill_buffer(cache_command, collision_buffers[1], 0U, 4096U, 4U);
        CHECK(bvb_command_buffer_ownership_registry_reads(cache_command) ==
              2U);
        cmd_fill_buffer(cache_command, collision_buffers[16], 0U, 4096U, 5U);
        cmd_fill_buffer(cache_command, collision_buffers[0], 0U, 4096U, 6U);
        collision_ownership_registry_reads =
            bvb_command_buffer_ownership_registry_reads(cache_command);
        CHECK(collision_ownership_registry_reads == 4U);
        CHECK(end_command_buffer(cache_command) == VK_SUCCESS);

        CHECK(begin_command_buffer(cache_command, &begin_info) == VK_SUCCESS);
        CHECK(bvb_command_buffer_ownership_registry_reads(cache_command) ==
              0U);
        cmd_fill_buffer(cache_command, collision_buffers[0], 0U, 4096U, 7U);
        rerecord_ownership_registry_reads =
            bvb_command_buffer_ownership_registry_reads(cache_command);
        CHECK(rerecord_ownership_registry_reads == 1U);
        CHECK(end_command_buffer(cache_command) == VK_SUCCESS);

        CHECK(reset_command_pool(device, cache_pool, 0U) == VK_SUCCESS);
        CHECK(bvb_command_buffer_ownership_registry_reads(cache_command) ==
              0U);
        CHECK(begin_command_buffer(cache_command, &begin_info) == VK_SUCCESS);
        cmd_fill_buffer(cache_command, collision_buffers[1], 0U, 4096U, 8U);
        cmd_fill_buffer(cache_command, collision_buffers[1], 0U, 4096U, 9U);
        pool_reset_ownership_registry_reads =
            bvb_command_buffer_ownership_registry_reads(cache_command);
        CHECK(pool_reset_ownership_registry_reads == 1U);
        CHECK(end_command_buffer(cache_command) == VK_SUCCESS);

        VkBuffer stale_buffer = VK_NULL_HANDLE;
        CHECK(create_buffer(device, &buffer_create_info, NULL,
                            &stale_buffer) == VK_SUCCESS);
        CHECK(begin_command_buffer(cache_command, &begin_info) == VK_SUCCESS);
        cmd_fill_buffer(cache_command, stale_buffer, 0U, 4096U,
                        UINT32_C(0xdeadcafe));
        CHECK(bvb_command_buffer_ownership_registry_reads(cache_command) ==
              1U);
        CHECK(end_command_buffer(cache_command) == VK_SUCCESS);
        destroy_buffer(device, stale_buffer, NULL);
        const VkCommandBufferSubmitInfo stale_command_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = cache_command,
        };
        const VkSubmitInfo2 stale_submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .commandBufferInfoCount = 1U,
            .pCommandBufferInfos = &stale_command_info,
        };
        stale_resource_rejected =
            queue_submit_2(queue, 1U, &stale_submit, VK_NULL_HANDLE) ==
            VK_ERROR_INITIALIZATION_FAILED;
        CHECK(stale_resource_rejected);
        uint32_t stale_replay_mismatches = UINT32_MAX;
        CHECK(bvb_verify_memory_fill(
                  device_memory, 0U, 4096U, UINT32_C(0xa5c3f00d),
                  &stale_replay_mismatches) == 0);
        stale_native_replay_blocked = stale_replay_mismatches == 0U;
        CHECK(stale_native_replay_blocked);

        free_command_buffers(device, cache_pool, 1U, &cache_command);
        destroy_command_pool(device, cache_pool, NULL);
        for (uint32_t index = 0U; index < 17U; ++index)
            destroy_buffer(device, collision_buffers[index], NULL);
        free_command_buffers(device, command_pool, 1U, &concurrent[0]);
        free_command_buffers(device, command_pool, 1U, &concurrent[1]);
        concurrent_streams = 2U;
        concurrent_commands = 512U;
    }
    if (noncoherent_mapped_memory) mapped[0] = UINT8_C(0x44);
    if (keep_mapped_memory) {
        const uint64_t exchanges_before_unmap =
            bvb_global_dispatch_exchange_count();
        unmap_memory(device, mapped_memory);
        unmap_rtts =
            bvb_global_dispatch_exchange_count() - exchanges_before_unmap;
        unmap_opcode = bvb_global_dispatch_last_opcode();
    }
    if (getenv("BVB_TEST_DROP_MEMORY_UNMAP_ACK") != NULL) {
        CHECK(bvb_memory_proxy_is_mapped(mapped_memory) == 0);
        CHECK(bvb_global_dispatch_connection_is_open() == 0);
        const uint64_t exchanges_before_poison_retry =
            bvb_global_dispatch_exchange_count();
        CHECK(get_fence_status(device, fence) ==
              VK_ERROR_INITIALIZATION_FAILED);
        CHECK(bvb_global_dispatch_exchange_count() ==
              exchanges_before_poison_retry);
        CHECK(bvb_global_dispatch_connection_is_open() == 0);
        printf("PASS: E077 unmap lost-ack local_release=1 "
               "connection_poisoned=1 poison_retry_rtts=0 "
               "unmap_opcode=%u\n",
               unmap_opcode);
        return 0;
    }
    if (noncoherent_mapped_memory) {
        mismatched_words = UINT32_MAX;
        CHECK(bvb_verify_memory_fill(
                  upload_memory, 0U, 4U, UINT32_C(0x03020100),
                  &mismatched_words) == 0);
        CHECK(mismatched_words == 0U);
    }
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
    if (!hardware_mode) {
        CHECK(bvb_handle_serial(timeline_id) ==
              (animated_wsi ? 3U : 2U));
    }
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
    if (shared_command_stream) {
        CHECK(begin_command_buffer(command_buffer, &begin_info) == VK_SUCCESS);
        CHECK(end_command_buffer(command_buffer) == VK_SUCCESS);
        const VkCommandBufferSubmitInfo reused_slot_command = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = command_buffer,
        };
        const VkSubmitInfo2 reused_slot_submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .commandBufferInfoCount = 1U,
            .pCommandBufferInfos = &reused_slot_command,
        };
        if (getenv("BVB_EXPECT_STREAM_SUBMIT_FAILURE") != NULL) {
            const VkResult forced_submit_result = queue_submit_2(
                queue, 1U, &reused_slot_submit, VK_NULL_HANDLE);
            if (forced_submit_result != VK_ERROR_OUT_OF_DEVICE_MEMORY) {
                fprintf(stderr, "forced Submit2 result=%d expected=%d\n",
                        forced_submit_result, VK_ERROR_OUT_OF_DEVICE_MEMORY);
            }
            CHECK(forced_submit_result == VK_ERROR_OUT_OF_DEVICE_MEMORY);
            CHECK(bvb_global_dispatch_last_opcode() ==
                  BVB_OPCODE_VULKAN_QUEUE_SUBMIT_2_STREAM);
            CHECK(queue_submit_2(queue, 1U, &reused_slot_submit,
                                 VK_NULL_HANDLE) == VK_SUCCESS);
            CHECK(bvb_global_dispatch_last_opcode() ==
                  BVB_OPCODE_VULKAN_QUEUE_SUBMIT_2);
        } else {
            CHECK(queue_submit_2(queue, 1U, &reused_slot_submit,
                                 VK_NULL_HANDLE) == VK_SUCCESS);
        }
    }
    destroy_pipeline(device, general_pipeline, NULL);
    destroy_pipeline(device, builtin_pipeline, NULL);
    destroy_pipeline_layout(device, pipeline_layout, NULL);
    destroy_descriptor_set_layout(device, empty_layout, NULL);
    destroy_sampler(device, sampler, NULL);
    destroy_descriptor_pool(device, descriptor_pool, NULL);
    destroy_descriptor_set_layout(device, descriptor_layout, NULL);
    CHECK(device_wait_idle(device) == VK_SUCCESS);
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
    if (shared_command_stream) {
        VkCommandBuffer extra_commands[257] = {0};
        for (size_t index = 0U;
             index < sizeof(extra_commands) / sizeof(extra_commands[0]);
             ++index) {
            CHECK(allocate_command_buffers(
                      device, &allocate_info, &extra_commands[index]) ==
                  VK_SUCCESS);
        }
        for (size_t index = 0U;
             index < sizeof(extra_commands) / sizeof(extra_commands[0]);
             ++index) {
            free_command_buffers(
                device, command_pool, 1U, &extra_commands[index]);
        }
    }
    CHECK(reset_command_pool(device, command_pool, 0U) == VK_SUCCESS);
    free_command_buffers(device, command_pool, 1U, &command_buffer);
    destroy_command_pool(device, command_pool, NULL);
    if (upload_buffer != VK_NULL_HANDLE)
        destroy_buffer(device, upload_buffer, NULL);
    if (upload_memory != VK_NULL_HANDLE)
        free_memory(device, upload_memory, NULL);
    destroy_buffer(device, buffer, NULL);
    free_memory(device, device_memory, NULL);
    destroy_fence(device, fence, NULL);
    destroy_device(device, NULL);
    destroy_device(ownership_device, NULL);

    if (!hardware_mode) {
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
    }
    destroy_surface(instance_one, surface, NULL);

    VkInstance instance_two = VK_NULL_HANDLE;
    create_info.pApplicationInfo = NULL;
    CHECK(vkCreateInstance(&create_info, NULL, &instance_two) == VK_SUCCESS);
    CHECK(instance_two != VK_NULL_HANDLE);
    const uint64_t instance_two_id = bvb_instance_proxy_id(instance_two);
    CHECK(bvb_handle_type(instance_two_id) == BVB_OBJECT_INSTANCE);
    if (!hardware_mode) {
        CHECK(bvb_handle_serial(instance_two_id) == 2U);
    }
    CHECK(instance_two_id != instance_one_id);

    destroy_instance(instance_one, NULL);
    PFN_vkDestroyInstance destroy_instance_two = NULL;
    erased = vkGetInstanceProcAddr(instance_two, "vkDestroyInstance");
    CHECK(erased != NULL);
    memcpy(&destroy_instance_two, &erased, sizeof(destroy_instance_two));
    destroy_instance_two(instance_two, NULL);

    printf("PASS: global Vulkan discovery validation_mode=%s api=%u "
           "exposed_extensions=7 exposed_layers=0 "
           "instance_one=%llu instance_two=%llu physical_device=%llu "
           "device=%s device_api=%u driver=%u vendor=%u device_id=%u "
           "max_push_constants=%u image_format_max=%u,%u "
           "queues=%u memory_types=%u memory_heaps=%u device_extensions=%u "
           "sampler_anisotropy=%u logical_device=%llu queue=%llu "
           "empty_submit=0 queue_wait=0 device_wait=0 "
           "animated_frames=%u animated_reused_image=%u "
           "animated_recording_rtts=%llu "
           "command_pool=%llu command_buffer=%llu recording_rtts=%llu "
           "concurrent_streams=%u concurrent_commands=%u "
           "concurrent_registry_reads=%llu collision_registry_reads=%llu "
           "rerecord_registry_reads=%llu pool_reset_registry_reads=%llu "
           "stale_resource_rejected=%u stale_native_replay_blocked=%u "
           "command_submit=0 "
           "pool_reset=0 buffer=%llu memory=%llu memory_type=%u "
           "buffer_requirements2=%llu,%llu,%u buffer_address=%llu "
           "image=%llu image_view=%llu image_bytes=%llu "
           "image_allocation_bytes=%llu image_dedicated=%u,%u "
           "mapped_bytes=4096 mapped_mismatches=%u "
           "memory_rtts=%llu,%llu,%llu,%llu,%llu "
           "memory_opcodes=%u,%u,%u,%u,%u "
           "ineligible_memory_rtts=%llu,%llu "
           "ineligible_memory_opcodes=%u,%u "
           "fill_words=1024 mismatches=%u fence=%llu fence_before=1 "
           "fenced_submit=0 fence_after=0 fence_wait=0 fence_reset=0 "
           "fence_after_reset=1\n",
           hardware_mode ? "hardware"
                         : shared_command_stream ? "shared-command-stream"
                         : noncoherent_mapped_memory
                               ? "shared-noncoherent-memory"
                         : shared_mapped_memory ? "shared-mapped-memory"
                         : keep_mapped_memory ? "strict-mapped-memory"
                                                : "strict-fake",
           api_version,
           (unsigned long long)instance_one_id,
           (unsigned long long)instance_two_id,
           (unsigned long long)physical_id, properties.deviceName,
           properties.apiVersion, properties.driverVersion,
           properties.vendorID, properties.deviceID,
           properties.limits.maxPushConstantsSize,
           supported_image_format_properties.maxExtent.width,
           supported_image_format_properties.maxExtent.height,
           available_queue_count, memory.memoryTypeCount,
           memory.memoryHeapCount, available_device_extension_count,
           features.samplerAnisotropy,
           (unsigned long long)device_id,
           (unsigned long long)queue_id,
           animated_frame_count, animated_reused_image ? 1U : 0U,
           (unsigned long long)animated_recording_rtts,
           (unsigned long long)command_pool_id,
           (unsigned long long)command_buffer_id,
           (unsigned long long)recording_rtts,
           concurrent_streams, concurrent_commands,
           (unsigned long long)concurrent_ownership_registry_reads,
           (unsigned long long)collision_ownership_registry_reads,
           (unsigned long long)rerecord_ownership_registry_reads,
           (unsigned long long)pool_reset_ownership_registry_reads,
           stale_resource_rejected ? 1U : 0U,
           stale_native_replay_blocked ? 1U : 0U,
           (unsigned long long)buffer_id,
           (unsigned long long)memory_id,
           memory_type_index,
           (unsigned long long)first_buffer_requirements_2.size,
           (unsigned long long)first_buffer_requirements_2.alignment,
           first_buffer_requirements_2.memoryTypeBits,
           (unsigned long long)buffer_device_address,
           (unsigned long long)image_id,
           (unsigned long long)image_view_id,
           (unsigned long long)image_requirements.size,
           (unsigned long long)image_requirements_2.memoryRequirements.size,
           dedicated_requirements.prefersDedicatedAllocation,
           dedicated_requirements.requiresDedicatedAllocation,
           mapped_mismatches,
           (unsigned long long)map_rtts,
           (unsigned long long)flush_rtts,
           (unsigned long long)invalidate_rtts,
           (unsigned long long)unmap_rtts,
           (unsigned long long)submit_rtts,
           map_opcode, flush_opcode, invalidate_opcode, unmap_opcode,
           submit_opcode,
           (unsigned long long)ineligible_map_rtts,
           (unsigned long long)ineligible_unmap_rtts,
           ineligible_map_opcode, ineligible_unmap_opcode,
           mismatched_words,
           (unsigned long long)fence_id);
    if (animated_wsi) {
        static const char *const colors[4] = {
            "red", "green", "blue", "white",
        };
        for (uint32_t frame = 0U; frame < 4U; ++frame) {
            printf("E076_FRAME_EXPECTED sequence=%u color=%s slot=%u\n",
                   frame + 1U, colors[frame], animated_image_indices[frame]);
        }
    }
    return 0;
}
