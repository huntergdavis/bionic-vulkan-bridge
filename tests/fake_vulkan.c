#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define VK_NO_PROTOTYPES

#include <vulkan/vulkan.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#if !defined(SYS_memfd_create) && defined(__NR_memfd_create)
#define SYS_memfd_create __NR_memfd_create
#endif

#if defined(_WIN32)
#define BVB_EXPORT __declspec(dllexport)
#else
#define BVB_EXPORT __attribute__((visibility("default")))
#endif

static VkDeviceMemory fake_bound_memory = VK_NULL_HANDLE;
static VkDeviceMemory fake_bound_image_memory = VK_NULL_HANDLE;
static VkDeviceSize fake_buffer_size = 4096U;
static int fake_android_surface_enabled;
static int fake_swapchain_enabled;
static int fake_swapchain_created;
static int fake_image_acquired;
static int fake_to_clear_barrier;
static int fake_clear_recorded;
static int fake_to_present_barrier;
static int fake_submitted;
static int fake_fence_created;
static int fake_fence_signaled;
static const VkFence fake_fence_handle =
    (VkFence)(uintptr_t)UINT64_C(0x9000);

enum { BVB_FAKE_MEMORY_CAPACITY = 8 };

struct bvb_fake_memory_record {
    void *address;
    size_t size;
    int fd;
};

static struct bvb_fake_memory_record
    fake_memory_records[BVB_FAKE_MEMORY_CAPACITY];

static struct bvb_fake_memory_record *fake_memory_record(void *address) {
    for (size_t index = 0U; index < BVB_FAKE_MEMORY_CAPACITY; ++index) {
        if (fake_memory_records[index].address == address) {
            return &fake_memory_records[index];
        }
    }
    return NULL;
}

static struct bvb_fake_memory_record *fake_memory_slot(void) {
    for (size_t index = 0U; index < BVB_FAKE_MEMORY_CAPACITY; ++index) {
        if (fake_memory_records[index].address == NULL) {
            return &fake_memory_records[index];
        }
    }
    return NULL;
}

struct ANativeWindow;
typedef VkFlags VkAndroidSurfaceCreateFlagsKHR;
typedef struct VkAndroidSurfaceCreateInfoKHR {
    VkStructureType sType;
    const void *pNext;
    VkAndroidSurfaceCreateFlagsKHR flags;
    struct ANativeWindow *window;
} VkAndroidSurfaceCreateInfoKHR;
typedef VkResult(VKAPI_PTR *bvb_create_android_surface_fn)(
    VkInstance instance, const VkAndroidSurfaceCreateInfoKHR *create_info,
    const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface);

static VkResult VKAPI_CALL fake_enumerate_instance_version(uint32_t *version) {
    *version = VK_MAKE_API_VERSION(0, 1, 4, 354);
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_enumerate_extensions(
    const char *layer_name,
    uint32_t *count,
    VkExtensionProperties *properties) {
    (void)layer_name;
    static const char *const names[] = {
        "VK_KHR_surface",
        "VK_KHR_android_surface",
        "VK_KHR_get_physical_device_properties2",
        "VK_KHR_external_memory_capabilities",
        "VK_KHR_external_semaphore_capabilities",
    };
    const uint32_t available = (uint32_t)(sizeof(names) / sizeof(names[0]));
    if (properties == NULL) {
        *count = available;
        return VK_SUCCESS;
    }
    uint32_t written = *count < available ? *count : available;
    for (uint32_t index = 0; index < written; ++index) {
        memset(&properties[index], 0, sizeof(properties[index]));
        (void)snprintf(properties[index].extensionName,
                       sizeof(properties[index].extensionName), "%s",
                       names[index]);
        properties[index].specVersion = 1;
    }
    *count = written;
    return written < available ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_enumerate_layers(
    uint32_t *count, VkLayerProperties *properties) {
    if (count == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (properties != NULL && *count != 0U) {
        memset(properties, 0, sizeof(*properties));
        (void)snprintf(properties[0].layerName,
                       sizeof(properties[0].layerName), "%s",
                       "VK_LAYER_BVB_fake_native_only");
        properties[0].specVersion = VK_API_VERSION_1_0;
        properties[0].implementationVersion = 1U;
        *count = 1U;
        return VK_SUCCESS;
    }
    *count = 1U;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_create_instance(
    const VkInstanceCreateInfo *create_info,
    const VkAllocationCallbacks *allocator,
    VkInstance *instance) {
    (void)allocator;
    bool has_surface = false;
    bool has_android_surface = false;
    if (create_info != NULL) {
        for (uint32_t index = 0;
             index < create_info->enabledExtensionCount; ++index) {
            const char *name = create_info->ppEnabledExtensionNames[index];
            has_surface |= strcmp(name, "VK_KHR_surface") == 0;
            has_android_surface |=
                strcmp(name, "VK_KHR_android_surface") == 0;
        }
    }
    fake_android_surface_enabled = has_surface && has_android_surface;
    *instance = (VkInstance)(uintptr_t)0x1000U;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_create_android_surface(
    VkInstance instance, const VkAndroidSurfaceCreateInfoKHR *create_info,
    const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface) {
    (void)instance;
    (void)allocator;
    if (create_info == NULL ||
        create_info->sType !=
            VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR ||
        create_info->window == NULL || surface == NULL ||
        fake_android_surface_enabled == 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *surface = (VkSurfaceKHR)(uintptr_t)UINT64_C(0x6000);
    return VK_SUCCESS;
}

static void VKAPI_CALL fake_destroy_surface(
    VkInstance instance, VkSurfaceKHR surface,
    const VkAllocationCallbacks *allocator) {
    (void)instance;
    (void)surface;
    (void)allocator;
}

static VkResult VKAPI_CALL fake_get_surface_support(
    VkPhysicalDevice device, uint32_t queue_family_index,
    VkSurfaceKHR surface, VkBool32 *supported) {
    (void)device;
    (void)surface;
    if (supported == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *supported = queue_family_index == 0U ? VK_TRUE : VK_FALSE;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_get_surface_capabilities(
    VkPhysicalDevice device, VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR *capabilities) {
    (void)device;
    (void)surface;
    if (capabilities == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *capabilities = (VkSurfaceCapabilitiesKHR){
        .minImageCount = 2,
        .maxImageCount = 4,
        .currentExtent = {64, 64},
        .minImageExtent = {16, 16},
        .maxImageExtent = {4096, 4096},
        .maxImageArrayLayers = 1,
        .supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    };
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_get_surface_formats(
    VkPhysicalDevice device, VkSurfaceKHR surface, uint32_t *count,
    VkSurfaceFormatKHR *formats) {
    (void)device;
    (void)surface;
    static const VkSurfaceFormatKHR available[] = {
        {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    };
    const uint32_t available_count =
        (uint32_t)(sizeof(available) / sizeof(available[0]));
    if (formats == NULL) {
        *count = available_count;
        return VK_SUCCESS;
    }
    uint32_t written = *count < available_count ? *count : available_count;
    memcpy(formats, available, written * sizeof(*formats));
    *count = written;
    return written < available_count ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_get_present_modes(
    VkPhysicalDevice device, VkSurfaceKHR surface, uint32_t *count,
    VkPresentModeKHR *modes) {
    (void)device;
    (void)surface;
    static const VkPresentModeKHR available[] = {
        VK_PRESENT_MODE_FIFO_KHR,
        VK_PRESENT_MODE_IMMEDIATE_KHR,
    };
    const uint32_t available_count =
        (uint32_t)(sizeof(available) / sizeof(available[0]));
    if (modes == NULL) {
        *count = available_count;
        return VK_SUCCESS;
    }
    uint32_t written = *count < available_count ? *count : available_count;
    memcpy(modes, available, written * sizeof(*modes));
    *count = written;
    return written < available_count ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_enumerate_devices(
    VkInstance instance,
    uint32_t *count,
    VkPhysicalDevice *devices) {
    (void)instance;
    if (devices == NULL) {
        *count = 1;
        return VK_SUCCESS;
    }
    if (*count == 0U) {
        return VK_INCOMPLETE;
    }
    devices[0] = (VkPhysicalDevice)(uintptr_t)0x2000U;
    *count = 1;
    return VK_SUCCESS;
}

static void VKAPI_CALL fake_get_device_properties(
    VkPhysicalDevice device,
    VkPhysicalDeviceProperties *properties) {
    (void)device;
    memset(properties, 0, sizeof(*properties));
    properties->apiVersion = VK_MAKE_API_VERSION(0, 1, 3, 275);
    properties->driverVersion = 0x01020304U;
    properties->vendorID = 0x5143U;
    properties->deviceID = 0x0730U;
    properties->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    strncpy(properties->deviceName, "BVB Fake Adreno 730 \"quoted\"",
            sizeof(properties->deviceName) - 1U);
    properties->limits.maxPushConstantsSize = 256U;
}

static void VKAPI_CALL fake_get_device_features(
    VkPhysicalDevice device, VkPhysicalDeviceFeatures *features) {
    (void)device;
    memset(features, 0, sizeof(*features));
    features->robustBufferAccess = VK_TRUE;
    features->geometryShader = VK_TRUE;
    features->samplerAnisotropy = VK_TRUE;
    features->shaderInt64 = VK_TRUE;
}

static void VKAPI_CALL fake_get_device_features2(
    VkPhysicalDevice device, VkPhysicalDeviceFeatures2 *features) {
    fake_get_device_features(device, &features->features);
    VkBaseOutStructure *entry = (VkBaseOutStructure *)features->pNext;
    while (entry != NULL) {
        if (entry->sType ==
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) {
            ((VkPhysicalDeviceVulkan11Features *)entry)
                ->shaderDrawParameters = VK_TRUE;
        } else if (entry->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES) {
            ((VkPhysicalDeviceShaderDrawParametersFeatures *)entry)
                ->shaderDrawParameters = VK_TRUE;
        } else if (entry->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            VkPhysicalDeviceVulkan12Features *vulkan12 =
                (VkPhysicalDeviceVulkan12Features *)entry;
            vulkan12->bufferDeviceAddress = VK_TRUE;
            vulkan12->descriptorIndexing = VK_TRUE;
            vulkan12->descriptorBindingSampledImageUpdateAfterBind =
                VK_TRUE;
            vulkan12->descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
            vulkan12->descriptorBindingPartiallyBound = VK_TRUE;
            vulkan12->hostQueryReset = VK_TRUE;
            vulkan12->runtimeDescriptorArray = VK_TRUE;
            vulkan12->samplerMirrorClampToEdge = VK_TRUE;
            vulkan12->scalarBlockLayout = VK_TRUE;
            vulkan12->timelineSemaphore = VK_TRUE;
            vulkan12->uniformBufferStandardLayout = VK_TRUE;
            vulkan12->vulkanMemoryModel = VK_TRUE;
        } else if (entry->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
            VkPhysicalDeviceVulkan13Features *vulkan13 =
                (VkPhysicalDeviceVulkan13Features *)entry;
            vulkan13->computeFullSubgroups = VK_TRUE;
            vulkan13->dynamicRendering = VK_TRUE;
            vulkan13->maintenance4 = VK_TRUE;
            vulkan13->shaderDemoteToHelperInvocation = VK_TRUE;
            vulkan13->shaderZeroInitializeWorkgroupMemory = VK_TRUE;
            vulkan13->subgroupSizeControl = VK_TRUE;
            vulkan13->synchronization2 = VK_TRUE;
        } else if (entry->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT) {
            ((VkPhysicalDeviceDepthClipEnableFeaturesEXT *)entry)
                ->depthClipEnable = VK_TRUE;
        } else if (entry->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT) {
            VkPhysicalDeviceRobustness2FeaturesEXT *robustness2 =
                (VkPhysicalDeviceRobustness2FeaturesEXT *)entry;
            robustness2->robustBufferAccess2 = VK_TRUE;
            robustness2->nullDescriptor = VK_TRUE;
        } else if (entry->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES) {
            ((VkPhysicalDeviceBufferDeviceAddressFeatures *)entry)
                ->bufferDeviceAddress = VK_TRUE;
        } else if (entry->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR) {
            ((VkPhysicalDeviceMaintenance5FeaturesKHR *)entry)
                ->maintenance5 = VK_TRUE;
        } else if (entry->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR) {
            ((VkPhysicalDeviceMaintenance6FeaturesKHR *)entry)
                ->maintenance6 = VK_TRUE;
        }
        entry = entry->pNext;
    }
}

static void VKAPI_CALL fake_get_format_properties(
    VkPhysicalDevice device, VkFormat format,
    VkFormatProperties *properties) {
    (void)device;
    memset(properties, 0, sizeof(*properties));
    if (format == VK_FORMAT_R8G8B8A8_UNORM) {
        properties->linearTilingFeatures =
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        properties->optimalTilingFeatures =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        properties->bufferFeatures =
            VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
    }
}

static VkResult VKAPI_CALL fake_get_image_format_properties(
    VkPhysicalDevice device, VkFormat format, VkImageType type,
    VkImageTiling tiling, VkImageUsageFlags usage,
    VkImageCreateFlags flags, VkImageFormatProperties *properties) {
    (void)device;
    (void)flags;
    if (format != VK_FORMAT_R8G8B8A8_UNORM ||
        type != VK_IMAGE_TYPE_2D || tiling != VK_IMAGE_TILING_OPTIMAL ||
        usage == 0U || properties == NULL) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    *properties = (VkImageFormatProperties){
        .maxExtent = {4096U, 2048U, 1U},
        .maxMipLevels = 12U,
        .maxArrayLayers = 256U,
        .sampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT,
        .maxResourceSize = UINT64_C(0x100000000),
    };
    return VK_SUCCESS;
}

static void VKAPI_CALL fake_get_queue_properties(
    VkPhysicalDevice device,
    uint32_t *count,
    VkQueueFamilyProperties *properties) {
    (void)device;
    if (properties == NULL) {
        *count = 2;
        return;
    }
    memset(properties, 0, sizeof(*properties) * 2U);
    properties[0].queueFlags = VK_QUEUE_GRAPHICS_BIT |
                               VK_QUEUE_COMPUTE_BIT |
                               VK_QUEUE_TRANSFER_BIT;
    properties[0].queueCount = 1;
    properties[1].queueFlags = VK_QUEUE_TRANSFER_BIT;
    properties[1].queueCount = 1;
}

static void VKAPI_CALL fake_get_memory_properties(
    VkPhysicalDevice device,
    VkPhysicalDeviceMemoryProperties *properties) {
    (void)device;
    memset(properties, 0, sizeof(*properties));
    properties->memoryHeapCount = 2;
    properties->memoryHeaps[0].size = 512ULL * 1024ULL * 1024ULL;
    properties->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    properties->memoryHeaps[1].size = 256ULL * 1024ULL * 1024ULL;
    properties->memoryTypeCount = 1;
    properties->memoryTypes[0].heapIndex = 0;
    properties->memoryTypes[0].propertyFlags =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}

static void VKAPI_CALL fake_get_external_buffer_properties(
    VkPhysicalDevice device,
    const VkPhysicalDeviceExternalBufferInfo *info,
    VkExternalBufferProperties *properties) {
    (void)device;
    *properties = (VkExternalBufferProperties){
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES,
    };
    if (info != NULL &&
        info->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT &&
        info->usage != 0U) {
        properties->externalMemoryProperties = (VkExternalMemoryProperties){
            .externalMemoryFeatures =
                VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
                VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT,
            .exportFromImportedHandleTypes =
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
            .compatibleHandleTypes =
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
        };
    }
}

static VkResult VKAPI_CALL fake_get_image_format_properties2(
    VkPhysicalDevice device,
    const VkPhysicalDeviceImageFormatInfo2 *info,
    VkImageFormatProperties2 *properties) {
    (void)device;
    if (info == NULL || properties == NULL ||
        info->format != VK_FORMAT_R8G8B8A8_UNORM ||
        info->type != VK_IMAGE_TYPE_2D ||
        info->tiling != VK_IMAGE_TILING_OPTIMAL || info->usage == 0U) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    properties->imageFormatProperties = (VkImageFormatProperties){
        .maxExtent = {4096U, 4096U, 1U},
        .maxMipLevels = 1U,
        .maxArrayLayers = 1U,
        .sampleCounts = VK_SAMPLE_COUNT_1_BIT,
        .maxResourceSize = UINT64_C(16) * 1024U * 1024U,
    };
    for (VkBaseOutStructure *next = (VkBaseOutStructure *)properties->pNext;
         next != NULL; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES) {
            VkExternalImageFormatProperties *external =
                (VkExternalImageFormatProperties *)next;
            external->externalMemoryProperties =
                (VkExternalMemoryProperties){
                    .externalMemoryFeatures =
                        VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
                        VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT,
                    .exportFromImportedHandleTypes =
                        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
                    .compatibleHandleTypes =
                        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
                };
        }
    }
    return VK_SUCCESS;
}

static void VKAPI_CALL fake_get_external_semaphore_properties(
    VkPhysicalDevice device,
    const VkPhysicalDeviceExternalSemaphoreInfo *info,
    VkExternalSemaphoreProperties *properties) {
    (void)device;
    *properties = (VkExternalSemaphoreProperties){
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
    };
    if (info != NULL &&
        (info->handleType ==
             VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT ||
         info->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT)) {
        properties->externalSemaphoreFeatures =
            VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT |
            VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
        properties->compatibleHandleTypes = info->handleType;
        properties->exportFromImportedHandleTypes = info->handleType;
    }
}

static VkResult VKAPI_CALL fake_enumerate_device_extensions(
    VkPhysicalDevice device,
    const char *layer_name,
    uint32_t *count,
    VkExtensionProperties *properties) {
    (void)device;
    (void)layer_name;
    static const char *const names[] = {
        "VK_KHR_swapchain",
        "VK_KHR_external_memory",
        "VK_KHR_external_memory_fd",
        "VK_ANDROID_external_memory_android_hardware_buffer",
        "VK_KHR_external_semaphore",
        "VK_KHR_external_semaphore_fd",
        "VK_KHR_dynamic_rendering",
    };
    const bool hide_swapchain = getenv("BVB_FAKE_HIDE_SWAPCHAIN") != NULL;
    const uint32_t name_count =
        (uint32_t)(sizeof(names) / sizeof(names[0]));
    const uint32_t available = name_count - (hide_swapchain ? 1U : 0U);
    if (properties == NULL) {
        *count = available;
        return VK_SUCCESS;
    }
    const uint32_t capacity = *count;
    uint32_t written = 0U;
    for (uint32_t index = 0U;
         index < name_count && written < capacity; ++index) {
        if (hide_swapchain &&
            strcmp(names[index], VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
            continue;
        }
        memset(&properties[written], 0, sizeof(properties[written]));
        (void)snprintf(properties[written].extensionName,
                       sizeof(properties[written].extensionName), "%s",
                       names[index]);
        properties[written].specVersion = 1;
        ++written;
    }
    *count = written;
    return written < available ? VK_INCOMPLETE : VK_SUCCESS;
}

static bool fake_feature_bool_count_is(
    const VkBool32 *features, uint32_t count, uint32_t expected_enabled) {
    uint32_t enabled = 0U;
    for (uint32_t index = 0U; index < count; ++index) {
        if (features[index] > VK_TRUE) {
            return false;
        }
        enabled += features[index] == VK_TRUE ? 1U : 0U;
    }
    return enabled == expected_enabled;
}

static bool fake_scaled_device_features_are_enabled(
    const void *feature_chain) {
    const VkPhysicalDeviceVulkan11Features *vulkan11 = feature_chain;
    if (vulkan11 == NULL ||
        vulkan11->sType !=
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES ||
        vulkan11->shaderDrawParameters != VK_TRUE ||
        !fake_feature_bool_count_is(
            &vulkan11->storageBuffer16BitAccess, 12U, 1U)) {
        return false;
    }
    const VkPhysicalDeviceVulkan12Features *vulkan12 = vulkan11->pNext;
    if (vulkan12 == NULL ||
        vulkan12->sType !=
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES ||
        vulkan12->samplerMirrorClampToEdge != VK_TRUE ||
        vulkan12->descriptorIndexing != VK_TRUE ||
        vulkan12->descriptorBindingSampledImageUpdateAfterBind != VK_TRUE ||
        vulkan12->descriptorBindingUpdateUnusedWhilePending != VK_TRUE ||
        vulkan12->descriptorBindingPartiallyBound != VK_TRUE ||
        vulkan12->runtimeDescriptorArray != VK_TRUE ||
        vulkan12->scalarBlockLayout != VK_TRUE ||
        vulkan12->uniformBufferStandardLayout != VK_TRUE ||
        vulkan12->hostQueryReset != VK_TRUE ||
        vulkan12->timelineSemaphore != VK_TRUE ||
        vulkan12->bufferDeviceAddress != VK_TRUE ||
        vulkan12->vulkanMemoryModel != VK_TRUE ||
        !fake_feature_bool_count_is(
            &vulkan12->samplerMirrorClampToEdge, 47U, 12U)) {
        return false;
    }
    const VkPhysicalDeviceVulkan13Features *vulkan13 = vulkan12->pNext;
    if (vulkan13 == NULL ||
        vulkan13->sType !=
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES ||
        vulkan13->shaderDemoteToHelperInvocation != VK_TRUE ||
        vulkan13->subgroupSizeControl != VK_TRUE ||
        vulkan13->computeFullSubgroups != VK_TRUE ||
        vulkan13->synchronization2 != VK_TRUE ||
        vulkan13->shaderZeroInitializeWorkgroupMemory != VK_TRUE ||
        vulkan13->dynamicRendering != VK_TRUE ||
        vulkan13->maintenance4 != VK_TRUE ||
        !fake_feature_bool_count_is(
            &vulkan13->robustImageAccess, 15U, 7U)) {
        return false;
    }
    const VkPhysicalDeviceDepthClipEnableFeaturesEXT *depth_clip =
        vulkan13->pNext;
    if (depth_clip == NULL ||
        depth_clip->sType !=
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT ||
        depth_clip->depthClipEnable != VK_TRUE) {
        return false;
    }
    const VkPhysicalDeviceRobustness2FeaturesEXT *robustness2 =
        depth_clip->pNext;
    if (robustness2 == NULL ||
        robustness2->sType !=
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT ||
        robustness2->robustBufferAccess2 != VK_TRUE ||
        robustness2->robustImageAccess2 != VK_FALSE ||
        robustness2->nullDescriptor != VK_TRUE) {
        return false;
    }
    const VkPhysicalDeviceMaintenance5FeaturesKHR *maintenance5 =
        robustness2->pNext;
    if (maintenance5 == NULL ||
        maintenance5->sType !=
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR ||
        maintenance5->maintenance5 != VK_TRUE) {
        return false;
    }
    const VkPhysicalDeviceMaintenance6FeaturesKHR *maintenance6 =
        maintenance5->pNext;
    return maintenance6 != NULL &&
        maintenance6->sType ==
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR &&
        maintenance6->maintenance6 == VK_TRUE &&
        maintenance6->pNext == NULL;
}

static VkResult VKAPI_CALL fake_create_device(
    VkPhysicalDevice physical_device,
    const VkDeviceCreateInfo *create_info,
    const VkAllocationCallbacks *allocator,
    VkDevice *device) {
    (void)physical_device;
    (void)allocator;
    fake_swapchain_enabled = 0;
    if (create_info == NULL || device == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (create_info->enabledExtensionCount == 58U) {
        if (create_info->queueCreateInfoCount != 2U ||
            create_info->pQueueCreateInfos == NULL ||
            create_info->pQueueCreateInfos[0].queueFamilyIndex != 0U ||
            create_info->pQueueCreateInfos[0].queueCount != 1U ||
            create_info->pQueueCreateInfos[1].queueFamilyIndex != 1U ||
            create_info->pQueueCreateInfos[1].queueCount != 1U ||
            create_info->pQueueCreateInfos[0].pQueuePriorities == NULL ||
            create_info->pQueueCreateInfos[1].pQueuePriorities == NULL ||
            create_info->pQueueCreateInfos[0].pQueuePriorities[0] != 0.75F ||
            create_info->pQueueCreateInfos[1].pQueuePriorities[0] != 0.25F ||
            create_info->pEnabledFeatures == NULL ||
            create_info->pEnabledFeatures->robustBufferAccess != VK_TRUE ||
            create_info->pEnabledFeatures->geometryShader != VK_TRUE ||
            create_info->pEnabledFeatures->samplerAnisotropy != VK_TRUE ||
            !fake_scaled_device_features_are_enabled(create_info->pNext)) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        for (uint32_t index = 0U; index < 58U; ++index) {
            char expected[64];
            (void)snprintf(expected, sizeof(expected),
                           "VK_BVB_scale_extension_%02u", index);
            if (create_info->ppEnabledExtensionNames == NULL ||
                strcmp(create_info->ppEnabledExtensionNames[index],
                       expected) != 0) {
                return VK_ERROR_EXTENSION_NOT_PRESENT;
            }
        }
    }
    if (create_info != NULL) {
        for (uint32_t index = 0;
             index < create_info->enabledExtensionCount; ++index) {
            fake_swapchain_enabled |=
                strcmp(create_info->ppEnabledExtensionNames[index],
                       "VK_KHR_swapchain") == 0;
        }
    }
    if (getenv("BVB_FAKE_HIDE_SWAPCHAIN") != NULL &&
        fake_swapchain_enabled != 0) {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    *device = (VkDevice)(uintptr_t)0x3000U;
    return VK_SUCCESS;
}

static void VKAPI_CALL fake_destroy_device(
    VkDevice device,
    const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)allocator;
    fake_swapchain_enabled = 0;
    fake_swapchain_created = 0;
    fake_image_acquired = 0;
    fake_to_clear_barrier = 0;
    fake_clear_recorded = 0;
    fake_to_present_barrier = 0;
    fake_submitted = 0;
}

static VkResult VKAPI_CALL fake_create_swapchain(
    VkDevice device, const VkSwapchainCreateInfoKHR *create_info,
    const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain) {
    (void)device;
    (void)allocator;
    if (fake_swapchain_enabled == 0 || create_info == NULL ||
        create_info->surface == VK_NULL_HANDLE ||
        create_info->minImageCount != 2U ||
        create_info->imageFormat != VK_FORMAT_R8G8B8A8_UNORM ||
        create_info->imageColorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR ||
        create_info->imageExtent.width != 64U ||
        create_info->imageExtent.height != 64U ||
        (create_info->imageUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0U ||
        create_info->presentMode != VK_PRESENT_MODE_FIFO_KHR ||
        swapchain == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *swapchain = (VkSwapchainKHR)(uintptr_t)UINT64_C(0x7000);
    fake_swapchain_created = 1;
    return VK_SUCCESS;
}

static void VKAPI_CALL fake_destroy_swapchain(
    VkDevice device, VkSwapchainKHR swapchain,
    const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)swapchain;
    (void)allocator;
    fake_swapchain_created = 0;
}

static VkResult VKAPI_CALL fake_get_swapchain_images(
    VkDevice device, VkSwapchainKHR swapchain, uint32_t *count,
    VkImage *images) {
    (void)device;
    (void)swapchain;
    if (fake_swapchain_created == 0 || count == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (images == NULL) {
        *count = 3;
        return VK_SUCCESS;
    }
    if (*count < 3U) {
        return VK_INCOMPLETE;
    }
    for (uint32_t index = 0; index < 3U; ++index) {
        images[index] = (VkImage)(uintptr_t)(UINT64_C(0x7100) + index);
    }
    *count = 3;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_create_semaphore(
    VkDevice device, const VkSemaphoreCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkSemaphore *semaphore) {
    static uintptr_t next_semaphore = 0x8000U;
    (void)device;
    (void)allocator;
    if (create_info == NULL || semaphore == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *semaphore = (VkSemaphore)next_semaphore++;
    return VK_SUCCESS;
}

static void VKAPI_CALL fake_destroy_semaphore(
    VkDevice device, VkSemaphore semaphore,
    const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)semaphore;
    (void)allocator;
}

static VkResult VKAPI_CALL fake_import_semaphore_fd(
    VkDevice device, const VkImportSemaphoreFdInfoKHR *import_info) {
    (void)device;
    if (import_info == NULL || import_info->semaphore == VK_NULL_HANDLE ||
        import_info->flags != VK_SEMAPHORE_IMPORT_TEMPORARY_BIT ||
        import_info->handleType !=
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT ||
        import_info->fd < 0) {
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    (void)close(import_info->fd);
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_create_fence(
    VkDevice device, const VkFenceCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkFence *fence) {
    (void)device;
    (void)allocator;
    if (create_info == NULL || fence == NULL || fake_fence_created != 0 ||
        create_info->sType != VK_STRUCTURE_TYPE_FENCE_CREATE_INFO ||
        create_info->pNext != NULL ||
        (create_info->flags & ~VK_FENCE_CREATE_SIGNALED_BIT) != 0U)
        return VK_ERROR_INITIALIZATION_FAILED;
    fake_fence_created = 1;
    fake_fence_signaled =
        (create_info->flags & VK_FENCE_CREATE_SIGNALED_BIT) != 0U;
    *fence = fake_fence_handle;
    return VK_SUCCESS;
}

static void VKAPI_CALL fake_destroy_fence(
    VkDevice device, VkFence fence, const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)allocator;
    if (fence == fake_fence_handle) {
        fake_fence_created = 0;
        fake_fence_signaled = 0;
    }
}

static VkResult VKAPI_CALL fake_get_fence_status(
    VkDevice device, VkFence fence) {
    (void)device;
    if (fake_fence_created == 0 || fence != fake_fence_handle)
        return VK_ERROR_INITIALIZATION_FAILED;
    return fake_fence_signaled != 0 ? VK_SUCCESS : VK_NOT_READY;
}

static VkResult VKAPI_CALL fake_wait_for_fences(
    VkDevice device, uint32_t fence_count, const VkFence *fences,
    VkBool32 wait_all, uint64_t timeout) {
    (void)device;
    (void)wait_all;
    (void)timeout;
    if (fake_fence_created == 0 || fence_count != 1U || fences == NULL ||
        fences[0] != fake_fence_handle)
        return VK_ERROR_INITIALIZATION_FAILED;
    return fake_fence_signaled != 0 ? VK_SUCCESS : VK_TIMEOUT;
}

static VkResult VKAPI_CALL fake_reset_fences(
    VkDevice device, uint32_t fence_count, const VkFence *fences) {
    (void)device;
    if (fake_fence_created == 0 || fence_count != 1U || fences == NULL ||
        fences[0] != fake_fence_handle)
        return VK_ERROR_INITIALIZATION_FAILED;
    fake_fence_signaled = 0;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_acquire_next_image(
    VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, uint32_t *image_index) {
    (void)device;
    (void)swapchain;
    (void)timeout;
    (void)fence;
    if (fake_swapchain_created == 0 || semaphore == VK_NULL_HANDLE ||
        image_index == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *image_index = 0;
    fake_image_acquired = 1;
    return VK_SUCCESS;
}

static void VKAPI_CALL fake_get_device_queue(
    VkDevice device,
    uint32_t queue_family_index,
    uint32_t queue_index,
    VkQueue *queue) {
    (void)device;
    (void)queue_family_index;
    (void)queue_index;
    *queue = (VkQueue)(uintptr_t)0x3100U;
}

static VkResult VKAPI_CALL fake_create_buffer(
    VkDevice device,
    const VkBufferCreateInfo *create_info,
    const VkAllocationCallbacks *allocator,
    VkBuffer *buffer) {
    (void)device;
    (void)allocator;
    if (create_info == NULL || create_info->size == 0U) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    fake_buffer_size = create_info->size;
    *buffer = (VkBuffer)(uintptr_t)0x4000U;
    return VK_SUCCESS;
}

static void VKAPI_CALL fake_destroy_buffer(
    VkDevice device,
    VkBuffer buffer,
    const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)buffer;
    (void)allocator;
}

static void VKAPI_CALL fake_get_buffer_memory_requirements(
    VkDevice device,
    VkBuffer buffer,
    VkMemoryRequirements *requirements) {
    (void)device;
    (void)buffer;
    *requirements = (VkMemoryRequirements){
        .size = fake_buffer_size,
        .alignment = 4,
        .memoryTypeBits = 1,
    };
}

static VkResult VKAPI_CALL fake_allocate_memory(
    VkDevice device,
    const VkMemoryAllocateInfo *allocate_info,
    const VkAllocationCallbacks *allocator,
    VkDeviceMemory *memory) {
    (void)device;
    (void)allocator;
    const VkImportMemoryFdInfoKHR *import_info = NULL;
    const VkExportMemoryAllocateInfo *export_info = NULL;
    for (const VkBaseInStructure *next = allocate_info->pNext;
         next != NULL; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR) {
            import_info = (const VkImportMemoryFdInfoKHR *)next;
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO) {
            export_info = (const VkExportMemoryAllocateInfo *)next;
        }
    }
    struct bvb_fake_memory_record *record = fake_memory_slot();
    if (record == NULL) return VK_ERROR_TOO_MANY_OBJECTS;
    const size_t size = (size_t)allocate_info->allocationSize;
    void *allocation = NULL;
    int descriptor = -1;
    if (import_info != NULL) {
        if (import_info->handleType !=
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ||
            import_info->fd < 0) {
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }
        descriptor = import_info->fd;
        allocation = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                          descriptor, 0);
        if (allocation == MAP_FAILED) allocation = NULL;
    } else if (export_info != NULL) {
        if ((export_info->handleTypes &
             VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) == 0U) {
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }
        descriptor = (int)syscall(
            SYS_memfd_create, "bvb-fake-external", MFD_CLOEXEC);
        if (descriptor >= 0 && ftruncate(descriptor, (off_t)size) == 0) {
            allocation = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                              descriptor, 0);
            if (allocation == MAP_FAILED) allocation = NULL;
        }
    } else {
        allocation = calloc(1, size);
    }
    if (allocation == NULL) {
        if (descriptor >= 0 && import_info == NULL) (void)close(descriptor);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *record = (struct bvb_fake_memory_record){
        .address = allocation,
        .size = size,
        .fd = descriptor,
    };
    *memory = (VkDeviceMemory)(uintptr_t)allocation;
    return VK_SUCCESS;
}

static void VKAPI_CALL fake_free_memory(
    VkDevice device,
    VkDeviceMemory memory,
    const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)allocator;
    if (fake_bound_memory == memory) {
        fake_bound_memory = VK_NULL_HANDLE;
    }
    if (fake_bound_image_memory == memory) {
        fake_bound_image_memory = VK_NULL_HANDLE;
    }
    void *address = (void *)(uintptr_t)memory;
    struct bvb_fake_memory_record *record = fake_memory_record(address);
    if (record == NULL) {
        free(address);
        return;
    }
    if (record->fd >= 0) {
        (void)munmap(record->address, record->size);
        (void)close(record->fd);
    } else {
        free(record->address);
    }
    *record = (struct bvb_fake_memory_record){0};
    record->fd = -1;
}

static VkResult VKAPI_CALL fake_get_memory_fd(
    VkDevice device, const VkMemoryGetFdInfoKHR *info, int *descriptor) {
    (void)device;
    if (info == NULL || descriptor == NULL ||
        info->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) {
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    struct bvb_fake_memory_record *record = fake_memory_record(
        (void *)(uintptr_t)info->memory);
    if (record == NULL || record->fd < 0) {
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    *descriptor = dup(record->fd);
    return *descriptor >= 0 ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

static VkResult VKAPI_CALL fake_bind_buffer_memory(
    VkDevice device,
    VkBuffer buffer,
    VkDeviceMemory memory,
    VkDeviceSize offset) {
    (void)device;
    (void)buffer;
    (void)offset;
    fake_bound_memory = memory;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_create_image(
    VkDevice device, const VkImageCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkImage *image) {
    (void)device;
    (void)allocator;
    if (create_info == NULL || image == NULL ||
        create_info->format != VK_FORMAT_R8G8B8A8_UNORM ||
        create_info->extent.width != 64U ||
        create_info->extent.height != 64U) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    *image = (VkImage)(uintptr_t)UINT64_C(0xa000);
    return VK_SUCCESS;
}

static void VKAPI_CALL fake_destroy_image(
    VkDevice device, VkImage image, const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)image;
    (void)allocator;
}

static void VKAPI_CALL fake_get_image_memory_requirements(
    VkDevice device, VkImage image, VkMemoryRequirements *requirements) {
    (void)device;
    (void)image;
    *requirements = (VkMemoryRequirements){
        .size = 64U * 64U * sizeof(uint32_t),
        .alignment = 4096U,
        .memoryTypeBits = 1U,
    };
}

static VkResult VKAPI_CALL fake_bind_image_memory(
    VkDevice device, VkImage image, VkDeviceMemory memory,
    VkDeviceSize offset) {
    (void)device;
    (void)image;
    (void)offset;
    fake_bound_image_memory = memory;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_create_command_pool(
    VkDevice device,
    const VkCommandPoolCreateInfo *create_info,
    const VkAllocationCallbacks *allocator,
    VkCommandPool *command_pool) {
    (void)device;
    (void)create_info;
    (void)allocator;
    *command_pool = (VkCommandPool)(uintptr_t)0x5000U;
    return VK_SUCCESS;
}

static void VKAPI_CALL fake_destroy_command_pool(
    VkDevice device,
    VkCommandPool command_pool,
    const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)command_pool;
    (void)allocator;
}

static VkResult VKAPI_CALL fake_allocate_command_buffers(
    VkDevice device,
    const VkCommandBufferAllocateInfo *allocate_info,
    VkCommandBuffer *command_buffers) {
    (void)device;
    (void)allocate_info;
    command_buffers[0] = (VkCommandBuffer)(uintptr_t)0x5100U;
    return VK_SUCCESS;
}

static void VKAPI_CALL fake_free_command_buffers(
    VkDevice device, VkCommandPool command_pool,
    uint32_t command_buffer_count, const VkCommandBuffer *command_buffers) {
    (void)device;
    (void)command_pool;
    (void)command_buffer_count;
    (void)command_buffers;
}

static VkResult VKAPI_CALL fake_begin_command_buffer(
    VkCommandBuffer command_buffer,
    const VkCommandBufferBeginInfo *begin_info) {
    (void)command_buffer;
    (void)begin_info;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_end_command_buffer(
    VkCommandBuffer command_buffer) {
    (void)command_buffer;
    return VK_SUCCESS;
}

static void VKAPI_CALL fake_cmd_fill_buffer(
    VkCommandBuffer command_buffer,
    VkBuffer buffer,
    VkDeviceSize destination_offset,
    VkDeviceSize size,
    uint32_t data) {
    (void)command_buffer;
    (void)buffer;
    if (fake_bound_memory == VK_NULL_HANDLE) {
        return;
    }
    uint32_t *words = (uint32_t *)((uint8_t *)(uintptr_t)fake_bound_memory +
                                   destination_offset);
    for (VkDeviceSize index = 0; index < size / sizeof(uint32_t); ++index) {
        words[index] = data;
    }
}

static void VKAPI_CALL fake_cmd_pipeline_barrier(
    VkCommandBuffer command_buffer,
    VkPipelineStageFlags source_stage_mask,
    VkPipelineStageFlags destination_stage_mask,
    VkDependencyFlags dependency_flags,
    uint32_t memory_barrier_count,
    const VkMemoryBarrier *memory_barriers,
    uint32_t buffer_barrier_count,
    const VkBufferMemoryBarrier *buffer_barriers,
    uint32_t image_barrier_count,
    const VkImageMemoryBarrier *image_barriers) {
    (void)command_buffer;
    (void)source_stage_mask;
    (void)destination_stage_mask;
    (void)dependency_flags;
    (void)memory_barrier_count;
    (void)memory_barriers;
    (void)buffer_barrier_count;
    (void)buffer_barriers;
    if (fake_swapchain_created != 0 && image_barrier_count == 1U &&
        image_barriers != NULL) {
        const VkImageMemoryBarrier *barrier = &image_barriers[0];
        if (barrier->oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
            barrier->newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
            (barrier->dstAccessMask & VK_ACCESS_TRANSFER_WRITE_BIT) != 0U) {
            fake_to_clear_barrier = 1;
        }
        if (fake_clear_recorded != 0 &&
            barrier->oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
            barrier->newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR &&
            (barrier->srcAccessMask & VK_ACCESS_TRANSFER_WRITE_BIT) != 0U) {
            fake_to_present_barrier = 1;
        }
    }
}

static void VKAPI_CALL fake_cmd_clear_color_image(
    VkCommandBuffer command_buffer, VkImage image, VkImageLayout image_layout,
    const VkClearColorValue *color, uint32_t range_count,
    const VkImageSubresourceRange *ranges) {
    (void)command_buffer;
    (void)image;
    (void)ranges;
    if (fake_image_acquired != 0 && fake_to_clear_barrier != 0 &&
        image_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
        color != NULL && color->float32[0] == 1.0F &&
        color->float32[1] == 0.0F && color->float32[2] == 1.0F &&
        color->float32[3] == 1.0F && range_count == 1U) {
        fake_clear_recorded = 1;
    }
}

static void VKAPI_CALL fake_cmd_copy_image_to_buffer(
    VkCommandBuffer command_buffer, VkImage source_image,
    VkImageLayout source_layout, VkBuffer destination_buffer,
    uint32_t region_count, const VkBufferImageCopy *regions) {
    (void)command_buffer;
    (void)source_image;
    (void)destination_buffer;
    if (fake_bound_image_memory == VK_NULL_HANDLE ||
        fake_bound_memory == VK_NULL_HANDLE ||
        source_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
        region_count != 1U || regions == NULL) {
        return;
    }
    const size_t bytes = (size_t)regions[0].imageExtent.width *
                         (size_t)regions[0].imageExtent.height *
                         sizeof(uint32_t);
    memcpy((void *)(uintptr_t)fake_bound_memory,
           (const void *)(uintptr_t)fake_bound_image_memory, bytes);
}

static VkResult VKAPI_CALL fake_queue_submit(
    VkQueue queue,
    uint32_t submit_count,
    const VkSubmitInfo *submits,
    VkFence fence) {
    (void)queue;
    if (fence != VK_NULL_HANDLE) {
        if (fake_fence_created == 0 || fence != fake_fence_handle)
            return VK_ERROR_INITIALIZATION_FAILED;
        fake_fence_signaled = 1;
    }
    if (fake_swapchain_created != 0) {
        if (submit_count != 1U || submits == NULL ||
            submits[0].waitSemaphoreCount != 1U ||
            submits[0].commandBufferCount != 1U ||
            submits[0].signalSemaphoreCount != 1U ||
            fake_clear_recorded == 0 || fake_to_present_barrier == 0) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        fake_submitted = 1;
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_queue_present(
    VkQueue queue, const VkPresentInfoKHR *present_info) {
    (void)queue;
    if (fake_submitted == 0 || present_info == NULL ||
        present_info->waitSemaphoreCount != 1U ||
        present_info->swapchainCount != 1U ||
        present_info->pImageIndices == NULL ||
        present_info->pImageIndices[0] != 0U) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_queue_wait_idle(VkQueue queue) {
    (void)queue;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_reset_command_pool(
    VkDevice device, VkCommandPool command_pool,
    VkCommandPoolResetFlags flags) {
    (void)device;
    (void)command_pool;
    (void)flags;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_device_wait_idle(VkDevice device) {
    (void)device;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_map_memory(
    VkDevice device,
    VkDeviceMemory memory,
    VkDeviceSize offset,
    VkDeviceSize size,
    VkMemoryMapFlags flags,
    void **data) {
    (void)device;
    (void)size;
    (void)flags;
    *data = (uint8_t *)(uintptr_t)memory + offset;
    return VK_SUCCESS;
}

static void VKAPI_CALL fake_unmap_memory(
    VkDevice device,
    VkDeviceMemory memory) {
    (void)device;
    (void)memory;
}

static VkResult VKAPI_CALL fake_invalidate_mapped_ranges(
    VkDevice device,
    uint32_t range_count,
    const VkMappedMemoryRange *ranges) {
    (void)device;
    (void)range_count;
    (void)ranges;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_flush_mapped_ranges(
    VkDevice device,
    uint32_t range_count,
    const VkMappedMemoryRange *ranges) {
    (void)device;
    (void)range_count;
    (void)ranges;
    return VK_SUCCESS;
}

static void VKAPI_CALL fake_destroy_instance(
    VkInstance instance,
    const VkAllocationCallbacks *allocator) {
    (void)instance;
    (void)allocator;
    fake_android_surface_enabled = 0;
}

static PFN_vkVoidFunction VKAPI_CALL fake_get_device_proc_addr(
    VkDevice device,
    const char *name);

static PFN_vkVoidFunction function_pointer(const char *name) {
#define BVB_MATCH(vulkan_name, fake_name)                                         \
    if (strcmp(name, vulkan_name) == 0) {                                        \
        return (PFN_vkVoidFunction)(fake_name);                                  \
    }
    BVB_MATCH("vkEnumerateInstanceVersion", fake_enumerate_instance_version)
    BVB_MATCH("vkEnumerateInstanceExtensionProperties", fake_enumerate_extensions)
    BVB_MATCH("vkEnumerateInstanceLayerProperties", fake_enumerate_layers)
    BVB_MATCH("vkCreateInstance", fake_create_instance)
    BVB_MATCH("vkEnumeratePhysicalDevices", fake_enumerate_devices)
    BVB_MATCH("vkGetPhysicalDeviceProperties", fake_get_device_properties)
    BVB_MATCH("vkGetPhysicalDeviceFeatures", fake_get_device_features)
    BVB_MATCH("vkGetPhysicalDeviceFeatures2", fake_get_device_features2)
    BVB_MATCH("vkGetPhysicalDeviceFeatures2KHR", fake_get_device_features2)
    BVB_MATCH("vkGetPhysicalDeviceFormatProperties", fake_get_format_properties)
    BVB_MATCH("vkGetPhysicalDeviceImageFormatProperties",
              fake_get_image_format_properties)
    BVB_MATCH("vkGetPhysicalDeviceQueueFamilyProperties", fake_get_queue_properties)
    BVB_MATCH("vkGetPhysicalDeviceMemoryProperties", fake_get_memory_properties)
    BVB_MATCH("vkGetPhysicalDeviceExternalBufferProperties",
              fake_get_external_buffer_properties)
    BVB_MATCH("vkGetPhysicalDeviceExternalBufferPropertiesKHR",
              fake_get_external_buffer_properties)
    BVB_MATCH("vkGetPhysicalDeviceImageFormatProperties2",
              fake_get_image_format_properties2)
    BVB_MATCH("vkGetPhysicalDeviceImageFormatProperties2KHR",
              fake_get_image_format_properties2)
    BVB_MATCH("vkGetPhysicalDeviceExternalSemaphoreProperties",
              fake_get_external_semaphore_properties)
    BVB_MATCH("vkGetPhysicalDeviceExternalSemaphorePropertiesKHR",
              fake_get_external_semaphore_properties)
    BVB_MATCH("vkEnumerateDeviceExtensionProperties", fake_enumerate_device_extensions)
    BVB_MATCH("vkCreateDevice", fake_create_device)
    BVB_MATCH("vkGetDeviceProcAddr", fake_get_device_proc_addr)
    BVB_MATCH("vkDestroyInstance", fake_destroy_instance)
    BVB_MATCH("vkCreateAndroidSurfaceKHR", fake_create_android_surface)
    BVB_MATCH("vkDestroySurfaceKHR", fake_destroy_surface)
    BVB_MATCH("vkGetPhysicalDeviceSurfaceSupportKHR", fake_get_surface_support)
    BVB_MATCH("vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
              fake_get_surface_capabilities)
    BVB_MATCH("vkGetPhysicalDeviceSurfaceFormatsKHR", fake_get_surface_formats)
    BVB_MATCH("vkGetPhysicalDeviceSurfacePresentModesKHR",
              fake_get_present_modes)
#undef BVB_MATCH
    return NULL;
}

static PFN_vkVoidFunction VKAPI_CALL fake_get_device_proc_addr(
    VkDevice device,
    const char *name) {
    (void)device;
#define BVB_DEVICE_MATCH(vulkan_name, fake_name)                                \
    if (strcmp(name, vulkan_name) == 0) {                                       \
        return (PFN_vkVoidFunction)(fake_name);                                 \
    }
    BVB_DEVICE_MATCH("vkDestroyDevice", fake_destroy_device)
    BVB_DEVICE_MATCH("vkGetDeviceQueue", fake_get_device_queue)
    BVB_DEVICE_MATCH("vkCreateSwapchainKHR", fake_create_swapchain)
    BVB_DEVICE_MATCH("vkDestroySwapchainKHR", fake_destroy_swapchain)
    BVB_DEVICE_MATCH("vkGetSwapchainImagesKHR", fake_get_swapchain_images)
    BVB_DEVICE_MATCH("vkCreateSemaphore", fake_create_semaphore)
    BVB_DEVICE_MATCH("vkDestroySemaphore", fake_destroy_semaphore)
    BVB_DEVICE_MATCH("vkImportSemaphoreFdKHR", fake_import_semaphore_fd)
    BVB_DEVICE_MATCH("vkCreateFence", fake_create_fence)
    BVB_DEVICE_MATCH("vkDestroyFence", fake_destroy_fence)
    BVB_DEVICE_MATCH("vkGetFenceStatus", fake_get_fence_status)
    BVB_DEVICE_MATCH("vkWaitForFences", fake_wait_for_fences)
    BVB_DEVICE_MATCH("vkResetFences", fake_reset_fences)
    BVB_DEVICE_MATCH("vkAcquireNextImageKHR", fake_acquire_next_image)
    BVB_DEVICE_MATCH("vkCreateBuffer", fake_create_buffer)
    BVB_DEVICE_MATCH("vkDestroyBuffer", fake_destroy_buffer)
    BVB_DEVICE_MATCH("vkGetBufferMemoryRequirements",
                     fake_get_buffer_memory_requirements)
    BVB_DEVICE_MATCH("vkAllocateMemory", fake_allocate_memory)
    BVB_DEVICE_MATCH("vkFreeMemory", fake_free_memory)
    BVB_DEVICE_MATCH("vkBindBufferMemory", fake_bind_buffer_memory)
    BVB_DEVICE_MATCH("vkCreateImage", fake_create_image)
    BVB_DEVICE_MATCH("vkDestroyImage", fake_destroy_image)
    BVB_DEVICE_MATCH("vkGetImageMemoryRequirements",
                     fake_get_image_memory_requirements)
    BVB_DEVICE_MATCH("vkBindImageMemory", fake_bind_image_memory)
    BVB_DEVICE_MATCH("vkCreateCommandPool", fake_create_command_pool)
    BVB_DEVICE_MATCH("vkDestroyCommandPool", fake_destroy_command_pool)
    BVB_DEVICE_MATCH("vkResetCommandPool", fake_reset_command_pool)
    BVB_DEVICE_MATCH("vkAllocateCommandBuffers", fake_allocate_command_buffers)
    BVB_DEVICE_MATCH("vkFreeCommandBuffers", fake_free_command_buffers)
    BVB_DEVICE_MATCH("vkBeginCommandBuffer", fake_begin_command_buffer)
    BVB_DEVICE_MATCH("vkEndCommandBuffer", fake_end_command_buffer)
    BVB_DEVICE_MATCH("vkCmdFillBuffer", fake_cmd_fill_buffer)
    BVB_DEVICE_MATCH("vkCmdPipelineBarrier", fake_cmd_pipeline_barrier)
    BVB_DEVICE_MATCH("vkCmdClearColorImage", fake_cmd_clear_color_image)
    BVB_DEVICE_MATCH("vkCmdCopyImageToBuffer", fake_cmd_copy_image_to_buffer)
    BVB_DEVICE_MATCH("vkQueueSubmit", fake_queue_submit)
    BVB_DEVICE_MATCH("vkQueuePresentKHR", fake_queue_present)
    BVB_DEVICE_MATCH("vkQueueWaitIdle", fake_queue_wait_idle)
    BVB_DEVICE_MATCH("vkDeviceWaitIdle", fake_device_wait_idle)
    BVB_DEVICE_MATCH("vkMapMemory", fake_map_memory)
    BVB_DEVICE_MATCH("vkUnmapMemory", fake_unmap_memory)
    BVB_DEVICE_MATCH("vkFlushMappedMemoryRanges", fake_flush_mapped_ranges)
    BVB_DEVICE_MATCH("vkInvalidateMappedMemoryRanges",
                     fake_invalidate_mapped_ranges)
    BVB_DEVICE_MATCH("vkGetMemoryFdKHR", fake_get_memory_fd)
#undef BVB_DEVICE_MATCH
    return NULL;
}

BVB_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
    VkInstance instance,
    const char *name) {
    (void)instance;
    return function_pointer(name);
}

BVB_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(
    VkDevice device, const char *name) {
    return fake_get_device_proc_addr(device, name);
}
