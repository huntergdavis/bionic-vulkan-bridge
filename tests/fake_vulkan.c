#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define VK_NO_PROTOTYPES

#include <vulkan/vulkan.h>

#ifndef VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME
#define VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME     \
    "VK_ANDROID_external_memory_android_hardware_buffer"
#endif

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
static VkDeviceMemory fake_upload_memory = VK_NULL_HANDLE;
static VkDeviceMemory fake_bound_image_memory = VK_NULL_HANDLE;
static VkDeviceSize fake_buffer_size = 4096U;
static uintptr_t fake_next_buffer_handle = UINT64_C(0x4000);
static const VkDeviceSize fake_native_image_allocation_size = UINT64_C(19623936);

static bool fake_real_hardware_values(void) {
    return getenv("BVB_FAKE_REAL_HARDWARE_VALUES") != NULL;
}
static uint32_t fake_live_images;
static uint32_t fake_live_image_views;
static int fake_android_surface_enabled;
static int fake_swapchain_enabled;
static int fake_external_memory_fd_enabled;
static int fake_external_memory_dma_buf_enabled;
static int fake_ahardwarebuffer_enabled;
static int fake_swapchain_created;
static int fake_image_acquired;
static int fake_to_clear_barrier;
static int fake_clear_recorded;
static int fake_to_present_barrier;
static int fake_submitted;
static int fake_fence_created;
static int fake_fence_signaled;
static uint32_t fake_queue_submit_2_calls;
static uint32_t fake_descriptor_step;
static uint32_t fake_init_image_step;
static VkCommandBuffer fake_init_image_command = VK_NULL_HANDLE;
static int fake_init_image_violation;
static uint32_t fake_animation_frame_count;
static uint32_t fake_memory_flush_count;
static uint32_t fake_memory_invalidate_count;
static uint32_t fake_animation_step;
static uint32_t fake_animation_submit_count;
static int fake_animation_violation;

static bool fake_animation_enabled(void) {
    return getenv("BVB_FAKE_REQUIRE_ANIMATED_WSI") != NULL;
}
static const VkDescriptorSetLayout fake_descriptor_layout =
    (VkDescriptorSetLayout)(uintptr_t)UINT64_C(0xa100);
static const VkDescriptorPool fake_descriptor_pool =
    (VkDescriptorPool)(uintptr_t)UINT64_C(0xa200);
static const VkDescriptorSet fake_descriptor_set =
    (VkDescriptorSet)(uintptr_t)UINT64_C(0xa300);
static const VkSampler fake_sampler =
    (VkSampler)(uintptr_t)UINT64_C(0xa400);
static const VkDescriptorSetLayout fake_empty_descriptor_layout =
    (VkDescriptorSetLayout)(uintptr_t)UINT64_C(0xa500);
static const VkPipelineLayout fake_pipeline_layout =
    (VkPipelineLayout)(uintptr_t)UINT64_C(0xa600);
static const VkPipeline fake_graphics_pipeline =
    (VkPipeline)(uintptr_t)UINT64_C(0xa700);
static const uint32_t fake_dxvk_dummy_frag[] = {
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
static const VkFence fake_fence_handle =
    (VkFence)(uintptr_t)UINT64_C(0x9000);

enum { BVB_FAKE_SEMAPHORE_CAPACITY = 32 };

struct bvb_fake_semaphore_record {
    VkSemaphore handle;
    VkSemaphoreType type;
    uint64_t value;
};

static struct bvb_fake_semaphore_record
    fake_semaphores[BVB_FAKE_SEMAPHORE_CAPACITY];

static struct bvb_fake_semaphore_record *fake_semaphore_record(
    VkSemaphore semaphore) {
    for (size_t index = 0U; index < BVB_FAKE_SEMAPHORE_CAPACITY; ++index)
        if (fake_semaphores[index].handle == semaphore)
            return &fake_semaphores[index];
    return NULL;
}

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
    properties->limits.maxPushConstantsSize =
        fake_real_hardware_values() ? 512U : 256U;
    properties->limits.nonCoherentAtomSize = 256U;
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
    if (fake_real_hardware_values()) {
        *properties = (VkImageFormatProperties){
            .maxExtent = {16384U, 8192U, 1U},
            .maxMipLevels = 15U,
            .maxArrayLayers = 2048U,
            .sampleCounts = VK_SAMPLE_COUNT_1_BIT,
            .maxResourceSize = UINT64_C(0x200000000),
        };
    } else {
        *properties = (VkImageFormatProperties){
            .maxExtent = {4096U, 2048U, 1U},
            .maxMipLevels = 12U,
            .maxArrayLayers = 256U,
            .sampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT,
            .maxResourceSize = UINT64_C(0x100000000),
        };
    }
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
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    if (getenv("BVB_FAKE_NONCOHERENT_MEMORY") == NULL)
        properties->memoryTypes[0].propertyFlags |=
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
        (info->format != VK_FORMAT_R8G8B8A8_UNORM &&
         info->format != VK_FORMAT_B8G8R8A8_UNORM) ||
        info->type != VK_IMAGE_TYPE_2D ||
        info->tiling != VK_IMAGE_TILING_OPTIMAL || info->usage == 0U) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    VkExternalMemoryHandleTypeFlagBits external_handle_type = 0;
    for (const VkBaseInStructure *next =
             (const VkBaseInStructure *)info->pNext;
         next != NULL; next = next->pNext) {
        if (next->sType ==
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO) {
            external_handle_type =
                ((const VkPhysicalDeviceExternalImageFormatInfo *)next)
                    ->handleType;
        }
    }
    if (external_handle_type != 0 &&
        external_handle_type !=
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT &&
        external_handle_type !=
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    if (fake_real_hardware_values()) {
        properties->imageFormatProperties = (VkImageFormatProperties){
            .maxExtent = {16384U, 8192U, 1U},
            .maxMipLevels = 15U,
            .maxArrayLayers = 2048U,
            .sampleCounts = VK_SAMPLE_COUNT_1_BIT,
            .maxResourceSize = UINT64_C(0x200000000),
        };
    } else {
        properties->imageFormatProperties = (VkImageFormatProperties){
            .maxExtent = {4096U, 4096U, 1U},
            .maxMipLevels = 1U,
            .maxArrayLayers = 1U,
            .sampleCounts = VK_SAMPLE_COUNT_1_BIT,
            .maxResourceSize = UINT64_C(16) * 1024U * 1024U,
        };
    }
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
                    .exportFromImportedHandleTypes = external_handle_type,
                    .compatibleHandleTypes = external_handle_type,
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
        "VK_EXT_external_memory_dma_buf",
        "VK_ANDROID_external_memory_android_hardware_buffer",
        "VK_KHR_external_semaphore",
        "VK_KHR_external_semaphore_fd",
        "VK_KHR_dynamic_rendering",
    };
    const bool hide_swapchain = getenv("BVB_FAKE_HIDE_SWAPCHAIN") != NULL;
    const bool hide_external_memory_fd =
        getenv("BVB_FAKE_HIDE_EXTERNAL_MEMORY_FD") != NULL;
    const bool hide_external_memory_dma_buf =
        getenv("BVB_FAKE_HIDE_EXTERNAL_MEMORY_DMA_BUF") != NULL;
    const uint32_t name_count =
        (uint32_t)(sizeof(names) / sizeof(names[0]));
    const uint32_t available =
        name_count - (hide_swapchain ? 1U : 0U) -
        (hide_external_memory_fd ? 1U : 0U) -
        (hide_external_memory_dma_buf ? 1U : 0U);
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
        if (hide_external_memory_fd &&
            strcmp(names[index],
                   VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) == 0) {
            continue;
        }
        if (hide_external_memory_dma_buf &&
            strcmp(names[index],
                   VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) == 0) {
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
    fake_external_memory_fd_enabled = 0;
    fake_external_memory_dma_buf_enabled = 0;
    fake_ahardwarebuffer_enabled = 0;
    if (create_info == NULL || device == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    uint32_t external_memory_fd_count = 0U;
    uint32_t external_memory_dma_buf_count = 0U;
    uint32_t ahardwarebuffer_count = 0U;
    for (uint32_t index = 0U;
         index < create_info->enabledExtensionCount; ++index) {
        fake_swapchain_enabled |=
            strcmp(create_info->ppEnabledExtensionNames[index],
                   VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
        external_memory_fd_count +=
            strcmp(create_info->ppEnabledExtensionNames[index],
                   VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) == 0
                ? 1U : 0U;
        external_memory_dma_buf_count +=
            strcmp(create_info->ppEnabledExtensionNames[index],
                   VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) == 0
                ? 1U : 0U;
        ahardwarebuffer_count +=
            strcmp(
                create_info->ppEnabledExtensionNames[index],
                VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME) ==
                0
            ? 1U : 0U;
    }
    const char *expected_external_memory_fd_count =
        getenv("BVB_FAKE_EXPECT_EXTERNAL_MEMORY_FD_COUNT");
    if (expected_external_memory_fd_count != NULL &&
        ((strcmp(expected_external_memory_fd_count, "0") == 0 &&
          external_memory_fd_count != 0U) ||
         (strcmp(expected_external_memory_fd_count, "1") == 0 &&
          external_memory_fd_count != 1U) ||
         (strcmp(expected_external_memory_fd_count, "0") != 0 &&
          strcmp(expected_external_memory_fd_count, "1") != 0))) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const char *expected_external_memory_dma_buf_count =
        getenv("BVB_FAKE_EXPECT_EXTERNAL_MEMORY_DMA_BUF_COUNT");
    if (expected_external_memory_dma_buf_count != NULL &&
        ((strcmp(expected_external_memory_dma_buf_count, "0") == 0 &&
          external_memory_dma_buf_count != 0U) ||
         (strcmp(expected_external_memory_dma_buf_count, "1") == 0 &&
          external_memory_dma_buf_count != 1U) ||
         (strcmp(expected_external_memory_dma_buf_count, "0") != 0 &&
          strcmp(expected_external_memory_dma_buf_count, "1") != 0))) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const char *expected_extension_count =
        getenv("BVB_FAKE_EXPECT_DEVICE_EXTENSION_COUNT");
    const char *expected_ahardwarebuffer_count =
        getenv("BVB_FAKE_EXPECT_AHARDWAREBUFFER_COUNT");
    if (expected_ahardwarebuffer_count != NULL &&
        ((strcmp(expected_ahardwarebuffer_count, "0") == 0 &&
          ahardwarebuffer_count != 0U) ||
         (strcmp(expected_ahardwarebuffer_count, "1") == 0 &&
          ahardwarebuffer_count != 1U) ||
         (strcmp(expected_ahardwarebuffer_count, "0") != 0 &&
          strcmp(expected_ahardwarebuffer_count, "1") != 0))) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (expected_extension_count != NULL &&
        ((strcmp(expected_extension_count, "0") == 0 &&
          create_info->enabledExtensionCount != 0U) ||
         (strcmp(expected_extension_count, "1") == 0 &&
          create_info->enabledExtensionCount != 1U) ||
         (strcmp(expected_extension_count, "2") == 0 &&
          create_info->enabledExtensionCount != 2U) ||
         (strcmp(expected_extension_count, "3") == 0 &&
          create_info->enabledExtensionCount != 3U) ||
         (strcmp(expected_extension_count, "0") != 0 &&
          strcmp(expected_extension_count, "1") != 0 &&
          strcmp(expected_extension_count, "2") != 0 &&
          strcmp(expected_extension_count, "3") != 0))) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (getenv("BVB_FAKE_FORBID_CREATE_DEVICE") != NULL) {
        return VK_ERROR_DEVICE_LOST;
    }
    if (getenv("BVB_FAKE_HIDE_EXTERNAL_MEMORY_FD") != NULL &&
        external_memory_fd_count != 0U) {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    if (getenv("BVB_FAKE_HIDE_EXTERNAL_MEMORY_DMA_BUF") != NULL &&
        external_memory_dma_buf_count != 0U) {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    if (getenv("BVB_FAKE_REQUIRE_NO_NATIVE_SWAPCHAIN") != NULL &&
        fake_swapchain_enabled != 0) {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    if (create_info->enabledExtensionCount == 58U ||
        create_info->enabledExtensionCount == 59U ||
        create_info->enabledExtensionCount == 60U ||
        create_info->enabledExtensionCount == 61U) {
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
        if (create_info->enabledExtensionCount == 59U &&
            strcmp(create_info->ppEnabledExtensionNames[58],
                   VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) != 0) {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
        if (create_info->enabledExtensionCount == 60U &&
            (strcmp(create_info->ppEnabledExtensionNames[58],
                    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) != 0 ||
             strcmp(create_info->ppEnabledExtensionNames[59],
                    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) != 0)) {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
        if (create_info->enabledExtensionCount == 61U &&
            (strcmp(create_info->ppEnabledExtensionNames[58],
                    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) != 0 ||
             strcmp(create_info->ppEnabledExtensionNames[59],
                    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) != 0 ||
             strcmp(
                 create_info->ppEnabledExtensionNames[60],
                 VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME) !=
                 0)) {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
    }
    if (getenv("BVB_FAKE_HIDE_SWAPCHAIN") != NULL &&
        fake_swapchain_enabled != 0) {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    fake_external_memory_fd_enabled = external_memory_fd_count != 0U;
    fake_external_memory_dma_buf_enabled =
        external_memory_dma_buf_count != 0U;
    fake_ahardwarebuffer_enabled = ahardwarebuffer_count != 0U;
    *device = (VkDevice)(uintptr_t)0x3000U;
    return VK_SUCCESS;
}

static void VKAPI_CALL fake_destroy_device(
    VkDevice device,
    const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)allocator;
    if (fake_live_image_views != 0U || fake_live_images != 0U) abort();
    fake_swapchain_enabled = 0;
    fake_external_memory_fd_enabled = 0;
    fake_external_memory_dma_buf_enabled = 0;
    fake_ahardwarebuffer_enabled = 0;
    fake_swapchain_created = 0;
    fake_image_acquired = 0;
    fake_to_clear_barrier = 0;
    fake_clear_recorded = 0;
    fake_to_present_barrier = 0;
    fake_submitted = 0;
    fake_descriptor_step = 0U;
    fake_init_image_step = 0U;
    fake_init_image_command = VK_NULL_HANDLE;
    fake_init_image_violation = 0;
}

static VkResult VKAPI_CALL fake_create_descriptor_set_layout(
    VkDevice device, const VkDescriptorSetLayoutCreateInfo *create_info,
    const VkAllocationCallbacks *allocator,
    VkDescriptorSetLayout *set_layout) {
    (void)device;
    if (fake_descriptor_step == 5U && allocator == NULL &&
        create_info != NULL && set_layout != NULL &&
        create_info->sType ==
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO &&
        create_info->pNext == NULL && create_info->flags == 0U &&
        create_info->bindingCount == 0U && create_info->pBindings == NULL) {
        *set_layout = fake_empty_descriptor_layout;
        fake_descriptor_step = 6U;
        return VK_SUCCESS;
    }
    if (fake_descriptor_step != 0U || allocator != NULL ||
        create_info == NULL || set_layout == NULL ||
        create_info->sType !=
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO ||
        create_info->flags !=
            VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT ||
        create_info->bindingCount != 1U || create_info->pBindings == NULL ||
        create_info->pBindings[0].binding != 0U ||
        create_info->pBindings[0].descriptorType !=
            VK_DESCRIPTOR_TYPE_SAMPLER ||
        create_info->pBindings[0].descriptorCount != 4096U ||
        create_info->pBindings[0].stageFlags !=
            (VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT) ||
        create_info->pBindings[0].pImmutableSamplers != NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkDescriptorSetLayoutBindingFlagsCreateInfo *flags =
        create_info->pNext;
    if (flags == NULL ||
        flags->sType !=
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO ||
        flags->pNext != NULL || flags->bindingCount != 1U ||
        flags->pBindingFlags == NULL || flags->pBindingFlags[0] !=
            (VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
             VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT |
             VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *set_layout = fake_descriptor_layout;
    fake_descriptor_step = 1U;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_create_descriptor_pool(
    VkDevice device, const VkDescriptorPoolCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkDescriptorPool *pool) {
    (void)device;
    if (fake_descriptor_step != 1U || allocator != NULL ||
        create_info == NULL || pool == NULL ||
        create_info->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO ||
        create_info->pNext != NULL ||
        create_info->flags != VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT ||
        create_info->maxSets != 1U || create_info->poolSizeCount != 1U ||
        create_info->pPoolSizes == NULL ||
        create_info->pPoolSizes[0].type != VK_DESCRIPTOR_TYPE_SAMPLER ||
        create_info->pPoolSizes[0].descriptorCount != 4096U) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *pool = fake_descriptor_pool;
    fake_descriptor_step = 2U;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_allocate_descriptor_sets(
    VkDevice device, const VkDescriptorSetAllocateInfo *allocate_info,
    VkDescriptorSet *descriptor_sets) {
    (void)device;
    if (fake_descriptor_step != 2U || allocate_info == NULL ||
        descriptor_sets == NULL ||
        allocate_info->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO ||
        allocate_info->pNext != NULL ||
        allocate_info->descriptorPool != fake_descriptor_pool ||
        allocate_info->descriptorSetCount != 1U ||
        allocate_info->pSetLayouts == NULL ||
        allocate_info->pSetLayouts[0] != fake_descriptor_layout) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    descriptor_sets[0] = fake_descriptor_set;
    fake_descriptor_step = 3U;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_create_sampler(
    VkDevice device, const VkSamplerCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkSampler *sampler) {
    (void)device;
    if (fake_descriptor_step != 3U || allocator != NULL ||
        create_info == NULL || sampler == NULL ||
        create_info->sType != VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO ||
        create_info->pNext != NULL || create_info->flags != 0U ||
        create_info->magFilter != VK_FILTER_LINEAR ||
        create_info->minFilter != VK_FILTER_NEAREST ||
        create_info->mipmapMode != VK_SAMPLER_MIPMAP_MODE_LINEAR ||
        create_info->addressModeU != VK_SAMPLER_ADDRESS_MODE_REPEAT ||
        create_info->addressModeV != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE ||
        create_info->addressModeW != VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT ||
        create_info->mipLodBias != 0.25F ||
        create_info->anisotropyEnable != VK_TRUE ||
        create_info->maxAnisotropy != 8.0F ||
        create_info->compareEnable != VK_TRUE ||
        create_info->compareOp != VK_COMPARE_OP_LESS_OR_EQUAL ||
        create_info->minLod != 0.0F || create_info->maxLod != 12.0F ||
        create_info->borderColor != VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE ||
        create_info->unnormalizedCoordinates != VK_FALSE) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *sampler = fake_sampler;
    fake_descriptor_step = 4U;
    return VK_SUCCESS;
}

static void VKAPI_CALL fake_update_descriptor_sets(
    VkDevice device, uint32_t write_count,
    const VkWriteDescriptorSet *writes, uint32_t copy_count,
    const VkCopyDescriptorSet *copies) {
    (void)device;
    if (fake_descriptor_step != 4U || write_count != 1U || writes == NULL ||
        copy_count != 0U || copies != NULL ||
        writes[0].sType != VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET ||
        writes[0].pNext != NULL || writes[0].dstSet != fake_descriptor_set ||
        writes[0].dstBinding != 0U || writes[0].dstArrayElement != 7U ||
        writes[0].descriptorCount != 1U ||
        writes[0].descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER ||
        writes[0].pImageInfo == NULL || writes[0].pBufferInfo != NULL ||
        writes[0].pTexelBufferView != NULL ||
        writes[0].pImageInfo[0].sampler != fake_sampler ||
        writes[0].pImageInfo[0].imageView != VK_NULL_HANDLE ||
        writes[0].pImageInfo[0].imageLayout != VK_IMAGE_LAYOUT_UNDEFINED) {
        return;
    }
    fake_descriptor_step = 5U;
}

static VkResult VKAPI_CALL fake_create_pipeline_layout(
    VkDevice device, const VkPipelineLayoutCreateInfo *create_info,
    const VkAllocationCallbacks *allocator,
    VkPipelineLayout *pipeline_layout) {
    (void)device;
    const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT |
        VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    if (fake_descriptor_step != 6U || allocator != NULL ||
        create_info == NULL || pipeline_layout == NULL ||
        create_info->sType != VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO ||
        create_info->pNext != NULL ||
        create_info->flags !=
            VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT ||
        create_info->setLayoutCount != 3U ||
        create_info->pSetLayouts == NULL ||
        create_info->pSetLayouts[0] != fake_descriptor_layout ||
        create_info->pSetLayouts[1] != fake_empty_descriptor_layout ||
        create_info->pSetLayouts[2] != VK_NULL_HANDLE ||
        create_info->pushConstantRangeCount != 1U ||
        create_info->pPushConstantRanges == NULL ||
        create_info->pPushConstantRanges[0].stageFlags != stages ||
        create_info->pPushConstantRanges[0].offset != 0U ||
        create_info->pPushConstantRanges[0].size != 160U) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *pipeline_layout = fake_pipeline_layout;
    fake_descriptor_step = 7U;
    return VK_SUCCESS;
}

static bool fake_stencil_state_is_zero(const VkStencilOpState *state) {
    return state->failOp == VK_STENCIL_OP_KEEP &&
        state->passOp == VK_STENCIL_OP_KEEP &&
        state->depthFailOp == VK_STENCIL_OP_KEEP &&
        state->compareOp == VK_COMPARE_OP_NEVER && state->compareMask == 0U &&
        state->writeMask == 0U && state->reference == 0U;
}

static VkResult VKAPI_CALL fake_create_graphics_pipelines(
    VkDevice device, VkPipelineCache pipeline_cache,
    uint32_t create_info_count,
    const VkGraphicsPipelineCreateInfo *create_infos,
    const VkAllocationCallbacks *allocator, VkPipeline *pipelines) {
    (void)device;
    static const VkDynamicState expected_dynamic[] = {
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
    if (fake_descriptor_step != 7U || pipeline_cache != VK_NULL_HANDLE ||
        create_info_count != 1U || create_infos == NULL ||
        allocator != NULL || pipelines == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkGraphicsPipelineCreateInfo *info = &create_infos[0];
    const VkGraphicsPipelineLibraryCreateInfoEXT *library = info->pNext;
    const VkPipelineCreateFlags2CreateInfo *flags = library == NULL
        ? NULL : library->pNext;
    const VkPipelineRenderingCreateInfo *rendering = flags == NULL
        ? NULL : flags->pNext;
    const VkPipelineShaderStageCreateInfo *stage = info->pStages;
    const VkShaderModuleCreateInfo *module = stage == NULL
        ? NULL : stage->pNext;
    const VkPipelineDepthStencilStateCreateInfo *depth =
        info->pDepthStencilState;
    const VkPipelineDynamicStateCreateInfo *dynamic = info->pDynamicState;
    if (info->sType != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO ||
        info->flags != 0U || library == NULL ||
        library->sType !=
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT ||
        library->flags !=
            VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT ||
        flags == NULL ||
        flags->sType !=
            VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO ||
        flags->flags != VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR ||
        rendering == NULL ||
        rendering->sType != VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO ||
        rendering->pNext != NULL || rendering->viewMask != 0U ||
        rendering->colorAttachmentCount != 0U ||
        rendering->pColorAttachmentFormats != NULL ||
        rendering->depthAttachmentFormat != VK_FORMAT_UNDEFINED ||
        rendering->stencilAttachmentFormat != VK_FORMAT_UNDEFINED ||
        info->stageCount != 1U || stage == NULL ||
        stage->sType != VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO ||
        stage->flags != 0U || stage->stage != VK_SHADER_STAGE_FRAGMENT_BIT ||
        stage->module != VK_NULL_HANDLE || stage->pName == NULL ||
        strcmp(stage->pName, "main") != 0 ||
        stage->pSpecializationInfo != NULL || module == NULL ||
        module->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO ||
        module->pNext != NULL || module->flags != 0U ||
        module->codeSize != sizeof(fake_dxvk_dummy_frag) ||
        module->pCode == NULL ||
        memcmp(module->pCode, fake_dxvk_dummy_frag,
               sizeof(fake_dxvk_dummy_frag)) != 0 ||
        info->pVertexInputState != NULL ||
        info->pInputAssemblyState != NULL ||
        info->pTessellationState != NULL || info->pViewportState != NULL ||
        info->pRasterizationState != NULL ||
        info->pMultisampleState != NULL || depth == NULL ||
        depth->sType !=
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO ||
        depth->pNext != NULL || depth->flags != 0U ||
        depth->depthTestEnable != VK_FALSE ||
        depth->depthWriteEnable != VK_FALSE ||
        depth->depthCompareOp != VK_COMPARE_OP_NEVER ||
        depth->depthBoundsTestEnable != VK_FALSE ||
        depth->stencilTestEnable != VK_FALSE ||
        !fake_stencil_state_is_zero(&depth->front) ||
        !fake_stencil_state_is_zero(&depth->back) ||
        depth->minDepthBounds != 0.0F || depth->maxDepthBounds != 0.0F ||
        info->pColorBlendState != NULL || dynamic == NULL ||
        dynamic->sType != VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO ||
        dynamic->pNext != NULL || dynamic->flags != 0U ||
        dynamic->dynamicStateCount !=
            (uint32_t)(sizeof(expected_dynamic) /
                       sizeof(expected_dynamic[0])) ||
        dynamic->pDynamicStates == NULL ||
        memcmp(dynamic->pDynamicStates, expected_dynamic,
               sizeof(expected_dynamic)) != 0 ||
        info->layout != fake_pipeline_layout ||
        info->renderPass != VK_NULL_HANDLE || info->subpass != 0U ||
        info->basePipelineHandle != VK_NULL_HANDLE ||
        info->basePipelineIndex != -1) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    pipelines[0] = fake_graphics_pipeline;
    fake_descriptor_step = 8U;
    return VK_SUCCESS;
}

static void VKAPI_CALL fake_destroy_pipeline(
    VkDevice device, VkPipeline pipeline,
    const VkAllocationCallbacks *allocator) {
    (void)device;
    if (fake_descriptor_step == 8U && pipeline == fake_graphics_pipeline &&
        allocator == NULL) {
        fake_descriptor_step = 9U;
    }
}

static void VKAPI_CALL fake_destroy_pipeline_layout(
    VkDevice device, VkPipelineLayout pipeline_layout,
    const VkAllocationCallbacks *allocator) {
    (void)device;
    if (fake_descriptor_step == 9U &&
        pipeline_layout == fake_pipeline_layout && allocator == NULL) {
        fake_descriptor_step = 10U;
    }
}

static void VKAPI_CALL fake_destroy_sampler(
    VkDevice device, VkSampler sampler,
    const VkAllocationCallbacks *allocator) {
    (void)device;
    if (fake_descriptor_step == 11U && sampler == fake_sampler &&
        allocator == NULL) {
        fake_descriptor_step = 12U;
    }
}

static void VKAPI_CALL fake_destroy_descriptor_pool(
    VkDevice device, VkDescriptorPool pool,
    const VkAllocationCallbacks *allocator) {
    (void)device;
    if (fake_descriptor_step == 12U && pool == fake_descriptor_pool &&
        allocator == NULL) {
        fake_descriptor_step = 13U;
    }
}

static void VKAPI_CALL fake_destroy_descriptor_set_layout(
    VkDevice device, VkDescriptorSetLayout set_layout,
    const VkAllocationCallbacks *allocator) {
    (void)device;
    if (allocator != NULL) return;
    if (fake_descriptor_step == 10U &&
        set_layout == fake_empty_descriptor_layout) {
        fake_descriptor_step = 11U;
    } else if (fake_descriptor_step == 13U &&
               set_layout == fake_descriptor_layout) {
        fake_descriptor_step = 14U;
    }
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
    if (create_info == NULL || semaphore == NULL || allocator != NULL ||
        create_info->sType != VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO ||
        create_info->flags != 0U) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkSemaphoreType type = VK_SEMAPHORE_TYPE_BINARY;
    uint64_t initial_value = 0U;
    if (create_info->pNext != NULL) {
        const VkSemaphoreTypeCreateInfo *type_info = create_info->pNext;
        if (type_info->sType !=
                VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO ||
            type_info->pNext != NULL ||
            (type_info->semaphoreType != VK_SEMAPHORE_TYPE_BINARY &&
             type_info->semaphoreType != VK_SEMAPHORE_TYPE_TIMELINE))
            return VK_ERROR_INITIALIZATION_FAILED;
        type = type_info->semaphoreType;
        initial_value = type_info->initialValue;
    }
    struct bvb_fake_semaphore_record *record = NULL;
    for (size_t index = 0U; index < BVB_FAKE_SEMAPHORE_CAPACITY; ++index) {
        if (fake_semaphores[index].handle == VK_NULL_HANDLE) {
            record = &fake_semaphores[index];
            break;
        }
    }
    if (record == NULL) return VK_ERROR_TOO_MANY_OBJECTS;
    *semaphore = (VkSemaphore)next_semaphore++;
    *record = (struct bvb_fake_semaphore_record){
        .handle = *semaphore,
        .type = type,
        .value = initial_value,
    };
    return VK_SUCCESS;
}

static void VKAPI_CALL fake_destroy_semaphore(
    VkDevice device, VkSemaphore semaphore,
    const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)allocator;
    struct bvb_fake_semaphore_record *record =
        fake_semaphore_record(semaphore);
    if (record != NULL)
        *record = (struct bvb_fake_semaphore_record){0};
}

static VkResult VKAPI_CALL fake_get_semaphore_counter_value(
    VkDevice device, VkSemaphore semaphore, uint64_t *value) {
    (void)device;
    struct bvb_fake_semaphore_record *record =
        fake_semaphore_record(semaphore);
    if (record == NULL || record->type != VK_SEMAPHORE_TYPE_TIMELINE ||
        value == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    *value = record->value;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_wait_semaphores(
    VkDevice device, const VkSemaphoreWaitInfo *wait_info, uint64_t timeout) {
    (void)device;
    (void)timeout;
    if (wait_info == NULL ||
        wait_info->sType != VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO ||
        wait_info->pNext != NULL || wait_info->semaphoreCount == 0U ||
        wait_info->pSemaphores == NULL || wait_info->pValues == NULL ||
        (wait_info->flags & ~VK_SEMAPHORE_WAIT_ANY_BIT) != 0U)
        return VK_ERROR_INITIALIZATION_FAILED;
    bool any = false;
    bool all = true;
    for (uint32_t index = 0U; index < wait_info->semaphoreCount; ++index) {
        struct bvb_fake_semaphore_record *record =
            fake_semaphore_record(wait_info->pSemaphores[index]);
        if (record == NULL || record->type != VK_SEMAPHORE_TYPE_TIMELINE)
            return VK_ERROR_INITIALIZATION_FAILED;
        const bool ready = record->value >= wait_info->pValues[index];
        any = any || ready;
        all = all && ready;
    }
    const bool ready = (wait_info->flags & VK_SEMAPHORE_WAIT_ANY_BIT) != 0U
                           ? any : all;
    return ready ? VK_SUCCESS : VK_TIMEOUT;
}

static VkResult VKAPI_CALL fake_signal_semaphore(
    VkDevice device, const VkSemaphoreSignalInfo *signal_info) {
    (void)device;
    if (signal_info == NULL ||
        signal_info->sType != VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO ||
        signal_info->pNext != NULL)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct bvb_fake_semaphore_record *record =
        fake_semaphore_record(signal_info->semaphore);
    if (record == NULL || record->type != VK_SEMAPHORE_TYPE_TIMELINE ||
        signal_info->value <= record->value)
        return VK_ERROR_INITIALIZATION_FAILED;
    record->value = signal_info->value;
    return VK_SUCCESS;
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
    if ((create_info->usage &
         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0U &&
        (create_info->sType != VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO ||
         create_info->pNext != NULL || create_info->flags != 0U ||
         create_info->sharingMode != VK_SHARING_MODE_EXCLUSIVE ||
         create_info->queueFamilyIndexCount != 0U ||
         create_info->pQueueFamilyIndices != NULL ||
         create_info->usage !=
             (VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
              VK_BUFFER_USAGE_TRANSFER_DST_BIT |
              VK_BUFFER_USAGE_TRANSFER_SRC_BIT))) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    fake_buffer_size = create_info->size;
    if (getenv("BVB_FAKE_REQUIRE_MEMORY_MIRROR") != NULL) {
        *buffer = (VkBuffer)fake_next_buffer_handle;
        fake_next_buffer_handle += UINT64_C(0x100);
    } else {
        *buffer = (VkBuffer)(uintptr_t)UINT64_C(0x4000);
    }
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

static void VKAPI_CALL fake_get_buffer_memory_requirements_2(
    VkDevice device, const VkBufferMemoryRequirementsInfo2 *info,
    VkMemoryRequirements2 *requirements) {
    (void)device;
    if (info == NULL || requirements == NULL ||
        info->sType != VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2 ||
        info->pNext != NULL ||
        info->buffer != (VkBuffer)(uintptr_t)UINT64_C(0x4000) ||
        requirements->sType != VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2) {
        abort();
    }
    VkMemoryDedicatedRequirements *dedicated = requirements->pNext;
    if (dedicated != NULL &&
        (dedicated->sType !=
             VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS ||
         dedicated->pNext != NULL)) {
        abort();
    }
    requirements->memoryRequirements = (VkMemoryRequirements){
        .size = fake_buffer_size,
        .alignment = fake_real_hardware_values() ? 4U : 256U,
        .memoryTypeBits = 1U,
    };
    if (dedicated != NULL) {
        dedicated->prefersDedicatedAllocation =
            fake_real_hardware_values() ? VK_FALSE : VK_TRUE;
        dedicated->requiresDedicatedAllocation = VK_FALSE;
    }
}

static void VKAPI_CALL fake_get_device_buffer_memory_requirements(
    VkDevice device, const VkDeviceBufferMemoryRequirements *info,
    VkMemoryRequirements2 *requirements) {
    (void)device;
    if (info == NULL || requirements == NULL ||
        info->sType != VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS ||
        info->pNext != NULL || info->pCreateInfo == NULL ||
        info->pCreateInfo->sType != VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO ||
        info->pCreateInfo->pNext != NULL ||
        info->pCreateInfo->flags != 0U ||
        info->pCreateInfo->size != 65536U ||
        info->pCreateInfo->usage !=
            (VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
             VK_BUFFER_USAGE_TRANSFER_DST_BIT |
             VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
        info->pCreateInfo->sharingMode != VK_SHARING_MODE_EXCLUSIVE ||
        info->pCreateInfo->queueFamilyIndexCount != 0U ||
        info->pCreateInfo->pQueueFamilyIndices != NULL ||
        requirements->sType != VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 ||
        requirements->pNext == NULL) {
        return;
    }
    VkMemoryDedicatedRequirements *dedicated = requirements->pNext;
    if (dedicated->sType !=
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS ||
        dedicated->pNext != NULL) {
        return;
    }
    requirements->memoryRequirements = (VkMemoryRequirements){
        .size = fake_real_hardware_values() ? 65536U : 65792U,
        .alignment = fake_real_hardware_values() ? 64U : 256U,
        .memoryTypeBits = fake_real_hardware_values() ? 1U : 5U,
    };
    dedicated->prefersDedicatedAllocation = VK_FALSE;
    dedicated->requiresDedicatedAllocation =
        fake_real_hardware_values() ? VK_FALSE : VK_TRUE;
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
    const VkMemoryDedicatedAllocateInfo *dedicated_info = NULL;
    const VkMemoryAllocateFlagsInfo *flags_info = NULL;
    for (const VkBaseInStructure *next = allocate_info->pNext;
         next != NULL; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR) {
            if (import_info != NULL) return VK_ERROR_INITIALIZATION_FAILED;
            import_info = (const VkImportMemoryFdInfoKHR *)next;
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO) {
            if (export_info != NULL) return VK_ERROR_INITIALIZATION_FAILED;
            export_info = (const VkExportMemoryAllocateInfo *)next;
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) {
            if (dedicated_info != NULL) return VK_ERROR_INITIALIZATION_FAILED;
            dedicated_info = (const VkMemoryDedicatedAllocateInfo *)next;
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO) {
            if (flags_info != NULL) return VK_ERROR_INITIALIZATION_FAILED;
            flags_info = (const VkMemoryAllocateFlagsInfo *)next;
        } else {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    if (flags_info != NULL &&
        (flags_info->flags != VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT ||
         flags_info->deviceMask != 0U ||
         flags_info->pNext != dedicated_info)) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (dedicated_info != NULL && export_info == NULL &&
        (dedicated_info->buffer != VK_NULL_HANDLE ||
         dedicated_info->image !=
             (VkImage)(uintptr_t)UINT64_C(0xa000) ||
         allocate_info->allocationSize !=
             (fake_real_hardware_values()
                  ? 64U * 64U * sizeof(uint32_t)
                  : fake_native_image_allocation_size))) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    struct bvb_fake_memory_record *record = fake_memory_slot();
    if (record == NULL) return VK_ERROR_TOO_MANY_OBJECTS;
    const size_t size = (size_t)allocate_info->allocationSize;
    void *allocation = NULL;
    int descriptor = -1;
    if (import_info != NULL) {
        if ((import_info->handleType !=
                 VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT &&
             import_info->handleType !=
                 VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) ||
            import_info->fd < 0) {
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }
        descriptor = import_info->fd;
        allocation = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                          descriptor, 0);
        if (allocation == MAP_FAILED) allocation = NULL;
    } else if (export_info != NULL) {
        if ((export_info->handleTypes &
             (VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT |
              VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)) == 0U) {
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
    if (getenv("BVB_FAKE_NONCOHERENT_MEMORY") != NULL)
        memset(allocation, UINT8_C(0x5a), size);
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
    if (fake_upload_memory == memory)
        fake_upload_memory = VK_NULL_HANDLE;
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
        (info->handleType !=
             VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT &&
         info->handleType !=
             VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)) {
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
    (void)offset;
    if (getenv("BVB_FAKE_REQUIRE_MEMORY_MIRROR") == NULL ||
        (uintptr_t)buffer == UINT64_C(0x4000))
        fake_bound_memory = memory;
    else
        fake_upload_memory = memory;
    return VK_SUCCESS;
}

static VkDeviceAddress VKAPI_CALL fake_get_buffer_device_address(
    VkDevice device, const VkBufferDeviceAddressInfo *info) {
    (void)device;
    if (info == NULL ||
        info->sType != VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO ||
        info->pNext != NULL ||
        info->buffer != (VkBuffer)(uintptr_t)UINT64_C(0x4000) ||
        fake_bound_memory == VK_NULL_HANDLE) {
        abort();
    }
    return fake_real_hardware_values()
               ? UINT64_C(0xabcdef010000)
               : UINT64_C(0x123456780000);
}

static VkResult VKAPI_CALL fake_create_image(
    VkDevice device, const VkImageCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkImage *image) {
    (void)device;
    (void)allocator;
    if (create_info == NULL || image == NULL ||
        (create_info->format != VK_FORMAT_R8G8B8A8_UNORM &&
         create_info->format != VK_FORMAT_B8G8R8A8_UNORM) ||
        create_info->extent.width == 0U ||
        create_info->extent.width > 4096U ||
        create_info->extent.height == 0U ||
        create_info->extent.height > 4096U ||
        create_info->extent.depth != 1U ||
        create_info->imageType != VK_IMAGE_TYPE_2D ||
        create_info->mipLevels != 1U || create_info->arrayLayers != 1U ||
        create_info->samples != VK_SAMPLE_COUNT_1_BIT ||
        create_info->tiling != VK_IMAGE_TILING_OPTIMAL ||
        create_info->sharingMode != VK_SHARING_MODE_EXCLUSIVE ||
        create_info->initialLayout != VK_IMAGE_LAYOUT_UNDEFINED) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    bool format_list_seen = false;
    bool stencil_usage_seen = false;
    for (const VkBaseInStructure *next = create_info->pNext;
         next != NULL; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
            const VkImageFormatListCreateInfo *format_list =
                (const VkImageFormatListCreateInfo *)next;
            if (format_list_seen || format_list->viewFormatCount != 1U ||
                format_list->pViewFormats == NULL ||
                format_list->pViewFormats[0] != VK_FORMAT_R8G8B8A8_UNORM) {
                return VK_ERROR_FORMAT_NOT_SUPPORTED;
            }
            format_list_seen = true;
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO) {
            const VkImageStencilUsageCreateInfo *stencil_usage =
                (const VkImageStencilUsageCreateInfo *)next;
            if (stencil_usage_seen ||
                stencil_usage->stencilUsage != VK_IMAGE_USAGE_SAMPLED_BIT) {
                return VK_ERROR_FORMAT_NOT_SUPPORTED;
            }
            stencil_usage_seen = true;
        } else if (next->sType !=
                   VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO) {
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        }
    }
    if (format_list_seen != stencil_usage_seen ||
        (format_list_seen &&
         (create_info->format != VK_FORMAT_R8G8B8A8_UNORM ||
          create_info->extent.width != 64U ||
          create_info->extent.height != 64U))) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    *image = (VkImage)(uintptr_t)UINT64_C(0xa000);
    ++fake_live_images;
    return VK_SUCCESS;
}

static void VKAPI_CALL fake_destroy_image(
    VkDevice device, VkImage image, const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)image;
    (void)allocator;
    if (fake_live_images != 0U) --fake_live_images;
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

static void VKAPI_CALL fake_get_image_memory_requirements_2(
    VkDevice device, const VkImageMemoryRequirementsInfo2 *info,
    VkMemoryRequirements2 *requirements) {
    (void)device;
    if (info == NULL || requirements == NULL ||
        info->sType != VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2 ||
        info->pNext != NULL ||
        info->image != (VkImage)(uintptr_t)UINT64_C(0xa000) ||
        requirements->sType != VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2) {
        abort();
    }
    VkMemoryDedicatedRequirements *dedicated = requirements->pNext;
    if (dedicated != NULL &&
        (dedicated->sType !=
             VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS ||
         dedicated->pNext != NULL)) {
        abort();
    }
    requirements->memoryRequirements = (VkMemoryRequirements){
        .size = fake_real_hardware_values()
                    ? 64U * 64U * sizeof(uint32_t)
                    : fake_native_image_allocation_size,
        .alignment = 4096U,
        .memoryTypeBits = 1U,
    };
    if (dedicated != NULL) {
        dedicated->prefersDedicatedAllocation =
            fake_real_hardware_values() ? VK_FALSE : VK_TRUE;
        dedicated->requiresDedicatedAllocation =
            fake_real_hardware_values() ? VK_FALSE : VK_TRUE;
    }
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

static VkResult VKAPI_CALL fake_create_image_view(
    VkDevice device, const VkImageViewCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkImageView *image_view) {
    (void)device;
    (void)allocator;
    if (create_info == NULL || image_view == NULL ||
        create_info->image != (VkImage)(uintptr_t)UINT64_C(0xa000) ||
        create_info->viewType != VK_IMAGE_VIEW_TYPE_2D ||
        create_info->format != VK_FORMAT_R8G8B8A8_UNORM ||
        create_info->components.r != VK_COMPONENT_SWIZZLE_IDENTITY ||
        create_info->components.g != VK_COMPONENT_SWIZZLE_IDENTITY ||
        create_info->components.b != VK_COMPONENT_SWIZZLE_IDENTITY ||
        create_info->components.a != VK_COMPONENT_SWIZZLE_IDENTITY ||
        create_info->subresourceRange.aspectMask !=
            VK_IMAGE_ASPECT_COLOR_BIT ||
        create_info->subresourceRange.baseMipLevel != 0U ||
        create_info->subresourceRange.levelCount != 1U ||
        create_info->subresourceRange.baseArrayLayer != 0U ||
        create_info->subresourceRange.layerCount != 1U) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    const VkImageViewUsageCreateInfo *usage = create_info->pNext;
    if (usage == NULL ||
        usage->sType != VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO ||
        usage->pNext != NULL || usage->usage != VK_IMAGE_USAGE_SAMPLED_BIT) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    *image_view = (VkImageView)(uintptr_t)UINT64_C(0xa100);
    ++fake_live_image_views;
    return VK_SUCCESS;
}

static void VKAPI_CALL fake_destroy_image_view(
    VkDevice device, VkImageView image_view,
    const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)image_view;
    (void)allocator;
    if (fake_live_image_views != 0U) --fake_live_image_views;
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
    if (allocate_info == NULL || command_buffers == NULL ||
        allocate_info->commandBufferCount == 0U)
        return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t index = 0U; index < allocate_info->commandBufferCount;
         ++index)
        command_buffers[index] =
            (VkCommandBuffer)(uintptr_t)(0x5100U + index);
    if (getenv("BVB_FAKE_REQUIRE_INIT_IMAGE_COMMANDS") != NULL &&
        (!fake_animation_enabled() || fake_animation_frame_count >= 4U))
        fake_init_image_command = allocate_info->commandBufferCount == 1U
            ? command_buffers[0] : VK_NULL_HANDLE;
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
    (void)begin_info;
    if (getenv("BVB_FAKE_REQUIRE_INIT_IMAGE_COMMANDS") != NULL &&
        command_buffer == fake_init_image_command) {
        fake_init_image_step = 1U;
        fake_init_image_violation = 0;
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_end_command_buffer(
    VkCommandBuffer command_buffer) {
    if (fake_animation_enabled() && fake_animation_step != 0U) {
        if (fake_animation_step != 3U || fake_animation_violation != 0 ||
            fake_animation_frame_count >= 4U)
            return VK_ERROR_INITIALIZATION_FAILED;
        ++fake_animation_frame_count;
        fake_animation_step = 0U;
    }
    if (command_buffer == fake_init_image_command) {
        if (fake_init_image_step != 3U || fake_init_image_violation != 0)
            return VK_ERROR_INITIALIZATION_FAILED;
        fake_init_image_step = 0U;
        fake_init_image_command = VK_NULL_HANDLE;
        fake_init_image_violation = 0;
    }
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
    const bool exact_init_clear =
        command_buffer == fake_init_image_command &&
        image == (VkImage)(uintptr_t)UINT64_C(0xa000) &&
        image_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
        color != NULL && color->uint32[0] == 0U &&
        color->uint32[1] == 0U && color->uint32[2] == 0U &&
        color->uint32[3] == 0U && range_count == 1U && ranges != NULL &&
        ranges[0].aspectMask == VK_IMAGE_ASPECT_COLOR_BIT &&
        ranges[0].baseMipLevel == 0U && ranges[0].levelCount == 1U &&
        ranges[0].baseArrayLayer == 0U && ranges[0].layerCount == 1U &&
        fake_init_image_step == 1U;
    if (command_buffer == fake_init_image_command &&
        fake_init_image_step == 1U) {
        if (exact_init_clear)
            fake_init_image_step = 2U;
        else
            fake_init_image_violation = 1;
    }
    if (fake_animation_enabled() && fake_animation_step == 1U) {
        const float expected_colors[4][4] = {
            {1.0F, 0.0F, 0.0F, 1.0F},
            {0.0F, 1.0F, 0.0F, 1.0F},
            {0.0F, 0.0F, 1.0F, 1.0F},
            {1.0F, 1.0F, 1.0F, 1.0F},
        };
        bool exact_animation_clear =
            fake_animation_frame_count < 4U &&
            image == (VkImage)(uintptr_t)UINT64_C(0xa000) &&
            image_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
            color != NULL && range_count == 1U && ranges != NULL &&
            ranges[0].aspectMask == VK_IMAGE_ASPECT_COLOR_BIT &&
            ranges[0].baseMipLevel == 0U && ranges[0].levelCount == 1U &&
            ranges[0].baseArrayLayer == 0U && ranges[0].layerCount == 1U;
        for (uint32_t component = 0U;
             exact_animation_clear && component < 4U; ++component) {
            exact_animation_clear =
                color->float32[component] ==
                expected_colors[fake_animation_frame_count][component];
        }
        if (exact_animation_clear)
            fake_animation_step = 2U;
        else
            fake_animation_violation = 1;
    }
    if (fake_image_acquired != 0 && fake_to_clear_barrier != 0 &&
        image_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
        color != NULL && color->float32[0] == 1.0F &&
        color->float32[1] == 0.0F && color->float32[2] == 1.0F &&
        color->float32[3] == 1.0F && range_count == 1U) {
        fake_clear_recorded = 1;
    }
}

static void VKAPI_CALL fake_cmd_pipeline_barrier_2(
    VkCommandBuffer command_buffer,
    const VkDependencyInfo *dependency_info) {
    if (fake_animation_enabled() && dependency_info != NULL &&
        dependency_info->sType == VK_STRUCTURE_TYPE_DEPENDENCY_INFO &&
        dependency_info->pNext == NULL &&
        dependency_info->dependencyFlags == 0U &&
        dependency_info->memoryBarrierCount == 0U &&
        dependency_info->pMemoryBarriers == NULL &&
        dependency_info->bufferMemoryBarrierCount == 0U &&
        dependency_info->pBufferMemoryBarriers == NULL &&
        dependency_info->imageMemoryBarrierCount == 1U &&
        dependency_info->pImageMemoryBarriers != NULL) {
        const VkImageMemoryBarrier2 *animation_barrier =
            &dependency_info->pImageMemoryBarriers[0];
        const bool exact_common =
            animation_barrier->sType ==
                VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 &&
            animation_barrier->pNext == NULL &&
            animation_barrier->srcQueueFamilyIndex ==
                VK_QUEUE_FAMILY_IGNORED &&
            animation_barrier->dstQueueFamilyIndex ==
                VK_QUEUE_FAMILY_IGNORED &&
            animation_barrier->image ==
                (VkImage)(uintptr_t)UINT64_C(0xa000) &&
            animation_barrier->subresourceRange.aspectMask ==
                VK_IMAGE_ASPECT_COLOR_BIT &&
            animation_barrier->subresourceRange.baseMipLevel == 0U &&
            animation_barrier->subresourceRange.levelCount == 1U &&
            animation_barrier->subresourceRange.baseArrayLayer == 0U &&
            animation_barrier->subresourceRange.layerCount == 1U;
        if (fake_animation_step == 0U &&
            fake_animation_frame_count < 4U) {
            const VkImageLayout expected_old_layout =
                fake_animation_frame_count < 3U
                    ? VK_IMAGE_LAYOUT_UNDEFINED
                    : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            if (exact_common &&
                animation_barrier->srcStageMask ==
                    VK_PIPELINE_STAGE_2_NONE &&
                animation_barrier->srcAccessMask == VK_ACCESS_2_NONE &&
                animation_barrier->dstStageMask ==
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT &&
                animation_barrier->dstAccessMask ==
                    VK_ACCESS_2_TRANSFER_WRITE_BIT &&
                animation_barrier->oldLayout == expected_old_layout &&
                animation_barrier->newLayout ==
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
                fake_animation_step = 1U;
                return;
            }
        } else if (fake_animation_step == 2U) {
            if (exact_common &&
                animation_barrier->srcStageMask ==
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT &&
                animation_barrier->srcAccessMask ==
                    VK_ACCESS_2_TRANSFER_WRITE_BIT &&
                animation_barrier->dstStageMask ==
                    VK_PIPELINE_STAGE_2_NONE &&
                animation_barrier->dstAccessMask == VK_ACCESS_2_NONE &&
                animation_barrier->oldLayout ==
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
                animation_barrier->newLayout ==
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
                fake_animation_step = 3U;
                return;
            }
        }
        if (fake_animation_step != 0U || fake_animation_frame_count < 4U) {
            fake_animation_violation = 1;
            return;
        }
    }
    if (fake_init_image_step != 2U ||
        command_buffer != fake_init_image_command ||
        dependency_info == NULL ||
        dependency_info->sType != VK_STRUCTURE_TYPE_DEPENDENCY_INFO ||
        dependency_info->pNext != NULL ||
        dependency_info->dependencyFlags != 0U ||
        dependency_info->memoryBarrierCount != 0U ||
        dependency_info->pMemoryBarriers != NULL ||
        dependency_info->bufferMemoryBarrierCount != 0U ||
        dependency_info->pBufferMemoryBarriers != NULL ||
        dependency_info->imageMemoryBarrierCount != 1U ||
        dependency_info->pImageMemoryBarriers == NULL) {
        if (command_buffer == fake_init_image_command)
            fake_init_image_violation = 1;
        return;
    }
    const VkImageMemoryBarrier2 *barrier =
        &dependency_info->pImageMemoryBarriers[0];
    if (barrier->sType == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 &&
        barrier->pNext == NULL &&
        barrier->srcStageMask == VK_PIPELINE_STAGE_2_NONE &&
        barrier->srcAccessMask == VK_ACCESS_2_NONE &&
        barrier->dstStageMask == VK_PIPELINE_STAGE_2_TRANSFER_BIT &&
        barrier->dstAccessMask == VK_ACCESS_2_TRANSFER_WRITE_BIT &&
        barrier->oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
        barrier->newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
        barrier->srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED &&
        barrier->dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED &&
        barrier->image == (VkImage)(uintptr_t)UINT64_C(0xa000) &&
        barrier->subresourceRange.aspectMask ==
            VK_IMAGE_ASPECT_COLOR_BIT &&
        barrier->subresourceRange.baseMipLevel == 0U &&
        barrier->subresourceRange.levelCount == 1U &&
        barrier->subresourceRange.baseArrayLayer == 0U &&
        barrier->subresourceRange.layerCount == 1U) {
        fake_init_image_step = 3U;
    } else
        fake_init_image_violation = 1;
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
    if (fake_animation_enabled()) {
        for (uint32_t submit_index = 0U; submit_index < submit_count;
             ++submit_index) {
            const VkSubmitInfo *submit = &submits[submit_index];
            if (submit->sType != VK_STRUCTURE_TYPE_SUBMIT_INFO ||
                submit->pNext != NULL ||
                (submit->waitSemaphoreCount != 0U &&
                 (submit->pWaitSemaphores == NULL ||
                  submit->pWaitDstStageMask == NULL)) ||
                (submit->signalSemaphoreCount != 0U &&
                 submit->pSignalSemaphores == NULL))
                return VK_ERROR_INITIALIZATION_FAILED;
            for (uint32_t index = 0U; index < submit->waitSemaphoreCount;
                 ++index) {
                const struct bvb_fake_semaphore_record *record =
                    fake_semaphore_record(submit->pWaitSemaphores[index]);
                if (record == NULL ||
                    record->type != VK_SEMAPHORE_TYPE_BINARY ||
                    record->value != 1U)
                    return VK_ERROR_INITIALIZATION_FAILED;
            }
            for (uint32_t index = 0U; index < submit->signalSemaphoreCount;
                 ++index) {
                const struct bvb_fake_semaphore_record *record =
                    fake_semaphore_record(submit->pSignalSemaphores[index]);
                if (record == NULL ||
                    record->type != VK_SEMAPHORE_TYPE_BINARY ||
                    record->value != 0U)
                    return VK_ERROR_INITIALIZATION_FAILED;
            }
        }
        for (uint32_t submit_index = 0U; submit_index < submit_count;
             ++submit_index) {
            const VkSubmitInfo *submit = &submits[submit_index];
            for (uint32_t index = 0U; index < submit->waitSemaphoreCount;
                 ++index)
                fake_semaphore_record(
                    submit->pWaitSemaphores[index])->value = 0U;
            for (uint32_t index = 0U; index < submit->signalSemaphoreCount;
                 ++index)
                fake_semaphore_record(
                    submit->pSignalSemaphores[index])->value = 1U;
        }
    }
    if (submit_count == 1U && submits != NULL &&
        submits[0].commandBufferCount == 1U &&
        fake_bound_memory != VK_NULL_HANDLE &&
        (getenv("BVB_FAKE_KEEP_MEMORY_MAPPED") != NULL ||
         getenv("BVB_FAKE_REQUIRE_MEMORY_MIRROR") != NULL ||
         getenv("BVB_FAKE_NONCOHERENT_MEMORY") != NULL)) {
        const uint8_t expected =
            getenv("BVB_FAKE_KEEP_MEMORY_MAPPED") != NULL
                ? UINT8_C(0x00) : UINT8_C(0x0d);
        const uint8_t *bytes =
            (const uint8_t *)(uintptr_t)fake_bound_memory;
        if (bytes[0] != expected)
            return VK_ERROR_INITIALIZATION_FAILED;
        if (getenv("BVB_FAKE_REQUIRE_MEMORY_MIRROR") != NULL &&
            (fake_upload_memory == VK_NULL_HANDLE ||
             *(const uint8_t *)(uintptr_t)fake_upload_memory !=
                 (getenv("BVB_FAKE_NONCOHERENT_MEMORY") != NULL
                      ? UINT8_C(0x00) : UINT8_C(0x7b))))
            return VK_ERROR_INITIALIZATION_FAILED;
        if (getenv("BVB_FAKE_REQUIRE_MEMORY_MIRROR") != NULL &&
            ((const uint32_t *)bytes)[1] != UINT32_C(0xa5c3f00d))
            return VK_ERROR_INITIALIZATION_FAILED;
        uint32_t *words = (uint32_t *)(uintptr_t)fake_bound_memory;
        for (size_t index = 0U; index < 4096U / sizeof(*words); ++index)
            words[index] = UINT32_C(0xa5c3f00d);
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

static VkResult VKAPI_CALL fake_queue_submit_2(
    VkQueue queue, uint32_t submit_count, const VkSubmitInfo2 *submits,
    VkFence fence) {
    (void)queue;
    if (submit_count != 1U || submits == NULL ||
        submits[0].sType != VK_STRUCTURE_TYPE_SUBMIT_INFO_2 ||
        submits[0].pNext != NULL || submits[0].flags != 0U)
        return VK_ERROR_INITIALIZATION_FAILED;
    ++fake_queue_submit_2_calls;
    const char *fail_at_text = getenv("BVB_FAKE_QUEUE_SUBMIT2_FAIL_AT");
    if (fail_at_text != NULL &&
        strtoul(fail_at_text, NULL, 10) == fake_queue_submit_2_calls) {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    if ((submits[0].waitSemaphoreInfoCount != 0U &&
         submits[0].pWaitSemaphoreInfos == NULL) ||
        (submits[0].commandBufferInfoCount != 0U &&
         submits[0].pCommandBufferInfos == NULL) ||
        (submits[0].signalSemaphoreInfoCount != 0U &&
         submits[0].pSignalSemaphoreInfos == NULL))
        return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t index = 0U;
         index < submits[0].waitSemaphoreInfoCount; ++index) {
        const VkSemaphoreSubmitInfo *info =
            &submits[0].pWaitSemaphoreInfos[index];
        struct bvb_fake_semaphore_record *record =
            fake_semaphore_record(info->semaphore);
        if (info->sType != VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO ||
            info->pNext != NULL || info->deviceIndex != 0U ||
            info->stageMask == VK_PIPELINE_STAGE_2_NONE || record == NULL ||
            (record->type == VK_SEMAPHORE_TYPE_TIMELINE
                 ? record->value < info->value
                 : info->value != 0U || record->value != 1U))
            return VK_ERROR_INITIALIZATION_FAILED;
    }
    for (uint32_t index = 0U;
         index < submits[0].commandBufferInfoCount; ++index) {
        const VkCommandBufferSubmitInfo *info =
            &submits[0].pCommandBufferInfos[index];
        if (info->sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO ||
            info->pNext != NULL || info->commandBuffer == VK_NULL_HANDLE ||
            info->deviceMask != 0U)
            return VK_ERROR_INITIALIZATION_FAILED;
    }
    for (uint32_t index = 0U;
         index < submits[0].signalSemaphoreInfoCount; ++index) {
        const VkSemaphoreSubmitInfo *info =
            &submits[0].pSignalSemaphoreInfos[index];
        struct bvb_fake_semaphore_record *record =
            fake_semaphore_record(info->semaphore);
        if (info->sType != VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO ||
            info->pNext != NULL || info->deviceIndex != 0U ||
            info->stageMask == VK_PIPELINE_STAGE_2_NONE || record == NULL ||
            (record->type == VK_SEMAPHORE_TYPE_TIMELINE
                 ? record->value >= info->value
                 : info->value != 0U || record->value != 0U))
            return VK_ERROR_INITIALIZATION_FAILED;
    }
    for (uint32_t index = 0U;
         index < submits[0].waitSemaphoreInfoCount; ++index) {
        struct bvb_fake_semaphore_record *record = fake_semaphore_record(
            submits[0].pWaitSemaphoreInfos[index].semaphore);
        if (record->type == VK_SEMAPHORE_TYPE_BINARY) record->value = 0U;
    }
    for (uint32_t index = 0U;
         index < submits[0].signalSemaphoreInfoCount; ++index) {
        const VkSemaphoreSubmitInfo *info =
            &submits[0].pSignalSemaphoreInfos[index];
        struct bvb_fake_semaphore_record *record =
            fake_semaphore_record(info->semaphore);
        record->value = record->type == VK_SEMAPHORE_TYPE_BINARY
                            ? 1U : info->value;
    }
    if (fake_animation_enabled() &&
        submits[0].waitSemaphoreInfoCount == 1U &&
        submits[0].commandBufferInfoCount == 1U &&
        submits[0].signalSemaphoreInfoCount == 1U) {
        const struct bvb_fake_semaphore_record *wait_record =
            fake_semaphore_record(
                submits[0].pWaitSemaphoreInfos[0].semaphore);
        const struct bvb_fake_semaphore_record *signal_record =
            fake_semaphore_record(
                submits[0].pSignalSemaphoreInfos[0].semaphore);
        if (wait_record != NULL && signal_record != NULL &&
            wait_record->type == VK_SEMAPHORE_TYPE_BINARY &&
            signal_record->type == VK_SEMAPHORE_TYPE_BINARY) {
            if (fake_animation_frame_count !=
                    fake_animation_submit_count + 1U ||
                fake_animation_submit_count >= 4U)
                return VK_ERROR_INITIALIZATION_FAILED;
            ++fake_animation_submit_count;
        }
    }
    if (fence != VK_NULL_HANDLE) {
        if (fake_fence_created == 0 || fence != fake_fence_handle)
            return VK_ERROR_INITIALIZATION_FAILED;
        fake_fence_signaled = 1;
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

static VkResult VKAPI_CALL fake_reset_command_buffer(
    VkCommandBuffer command_buffer, VkCommandBufferResetFlags flags) {
    (void)command_buffer;
    return flags == 0U ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
}

static VkResult VKAPI_CALL fake_device_wait_idle(VkDevice device) {
    (void)device;
    if (fake_descriptor_step != 0U && fake_descriptor_step != 14U) {
        fprintf(stderr, "fake Vulkan descriptor sequence stopped at step %u\n",
                fake_descriptor_step);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (fake_animation_enabled() &&
        (fake_animation_frame_count != 4U ||
         fake_animation_submit_count != 4U ||
         fake_animation_step != 0U || fake_animation_violation != 0)) {
        fprintf(stderr,
                "fake Vulkan animation stopped at frame=%u step=%u "
                "submits=%u violation=%d\n",
                fake_animation_frame_count, fake_animation_step,
                fake_animation_submit_count, fake_animation_violation);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
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
    if (getenv("BVB_FAKE_NONCOHERENT_MEMORY") != NULL &&
        range_count == 1U && ranges != NULL &&
        ranges[0].memory == fake_upload_memory) {
        if (range_count != 1U || ranges == NULL) return VK_ERROR_MEMORY_MAP_FAILED;
        const VkDeviceSize expected_offset =
            fake_memory_invalidate_count == 1U ? 256U : 0U;
        const VkDeviceSize expected_size =
            fake_memory_invalidate_count == 1U ? 256U
            : fake_memory_invalidate_count >= 3U ? VK_WHOLE_SIZE
                                                 : 4096U;
        if (ranges[0].offset != expected_offset ||
            ranges[0].size != expected_size)
            return VK_ERROR_MEMORY_MAP_FAILED;
        ++fake_memory_invalidate_count;
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_flush_mapped_ranges(
    VkDevice device,
    uint32_t range_count,
    const VkMappedMemoryRange *ranges) {
    (void)device;
    if (getenv("BVB_FAKE_NONCOHERENT_MEMORY") != NULL &&
        range_count == 1U && ranges != NULL &&
        ranges[0].memory == fake_upload_memory) {
        if (range_count != 1U || ranges == NULL) return VK_ERROR_MEMORY_MAP_FAILED;
        const VkDeviceSize expected_offset =
            fake_memory_flush_count == 1U ? 256U : 0U;
        const VkDeviceSize expected_size =
            fake_memory_flush_count == 1U ? 256U : 4096U;
        if (ranges[0].offset != expected_offset ||
            ranges[0].size != expected_size)
            return VK_ERROR_MEMORY_MAP_FAILED;
        ++fake_memory_flush_count;
    }
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
    if (strcmp(name, "vkGetMemoryFdKHR") == 0 &&
        (getenv("BVB_FAKE_HIDE_GET_MEMORY_FD") != NULL ||
         (getenv("BVB_FAKE_REQUIRE_EXTERNAL_MEMORY_FD") != NULL &&
          fake_external_memory_fd_enabled == 0) ||
         (getenv("BVB_FAKE_REQUIRE_EXTERNAL_MEMORY_DMA_BUF") != NULL &&
          fake_external_memory_dma_buf_enabled == 0))) {
        return NULL;
    }
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
    BVB_DEVICE_MATCH("vkGetSemaphoreCounterValue",
                     fake_get_semaphore_counter_value)
    BVB_DEVICE_MATCH("vkWaitSemaphores", fake_wait_semaphores)
    BVB_DEVICE_MATCH("vkSignalSemaphore", fake_signal_semaphore)
    BVB_DEVICE_MATCH("vkImportSemaphoreFdKHR", fake_import_semaphore_fd)
    BVB_DEVICE_MATCH("vkCreateFence", fake_create_fence)
    BVB_DEVICE_MATCH("vkDestroyFence", fake_destroy_fence)
    BVB_DEVICE_MATCH("vkGetFenceStatus", fake_get_fence_status)
    BVB_DEVICE_MATCH("vkWaitForFences", fake_wait_for_fences)
    BVB_DEVICE_MATCH("vkResetFences", fake_reset_fences)
    BVB_DEVICE_MATCH("vkAcquireNextImageKHR", fake_acquire_next_image)
    BVB_DEVICE_MATCH("vkCreateDescriptorSetLayout",
                     fake_create_descriptor_set_layout)
    BVB_DEVICE_MATCH("vkDestroyDescriptorSetLayout",
                     fake_destroy_descriptor_set_layout)
    BVB_DEVICE_MATCH("vkCreateDescriptorPool", fake_create_descriptor_pool)
    BVB_DEVICE_MATCH("vkDestroyDescriptorPool", fake_destroy_descriptor_pool)
    BVB_DEVICE_MATCH("vkAllocateDescriptorSets",
                     fake_allocate_descriptor_sets)
    BVB_DEVICE_MATCH("vkCreateSampler", fake_create_sampler)
    BVB_DEVICE_MATCH("vkDestroySampler", fake_destroy_sampler)
    BVB_DEVICE_MATCH("vkUpdateDescriptorSets", fake_update_descriptor_sets)
    BVB_DEVICE_MATCH("vkCreatePipelineLayout", fake_create_pipeline_layout)
    BVB_DEVICE_MATCH("vkDestroyPipelineLayout", fake_destroy_pipeline_layout)
    BVB_DEVICE_MATCH("vkCreateGraphicsPipelines", fake_create_graphics_pipelines)
    BVB_DEVICE_MATCH("vkDestroyPipeline", fake_destroy_pipeline)
    BVB_DEVICE_MATCH("vkCreateBuffer", fake_create_buffer)
    BVB_DEVICE_MATCH("vkDestroyBuffer", fake_destroy_buffer)
    BVB_DEVICE_MATCH("vkGetBufferMemoryRequirements",
                     fake_get_buffer_memory_requirements)
    BVB_DEVICE_MATCH("vkGetBufferMemoryRequirements2",
                     fake_get_buffer_memory_requirements_2)
    BVB_DEVICE_MATCH("vkGetDeviceBufferMemoryRequirements",
                     fake_get_device_buffer_memory_requirements)
    BVB_DEVICE_MATCH("vkGetBufferDeviceAddress",
                     fake_get_buffer_device_address)
    BVB_DEVICE_MATCH("vkAllocateMemory", fake_allocate_memory)
    BVB_DEVICE_MATCH("vkFreeMemory", fake_free_memory)
    BVB_DEVICE_MATCH("vkBindBufferMemory", fake_bind_buffer_memory)
    BVB_DEVICE_MATCH("vkCreateImage", fake_create_image)
    BVB_DEVICE_MATCH("vkDestroyImage", fake_destroy_image)
    BVB_DEVICE_MATCH("vkGetImageMemoryRequirements",
                     fake_get_image_memory_requirements)
    BVB_DEVICE_MATCH("vkGetImageMemoryRequirements2",
                     fake_get_image_memory_requirements_2)
    BVB_DEVICE_MATCH("vkBindImageMemory", fake_bind_image_memory)
    BVB_DEVICE_MATCH("vkCreateImageView", fake_create_image_view)
    BVB_DEVICE_MATCH("vkDestroyImageView", fake_destroy_image_view)
    BVB_DEVICE_MATCH("vkCreateCommandPool", fake_create_command_pool)
    BVB_DEVICE_MATCH("vkDestroyCommandPool", fake_destroy_command_pool)
    BVB_DEVICE_MATCH("vkResetCommandPool", fake_reset_command_pool)
    BVB_DEVICE_MATCH("vkResetCommandBuffer", fake_reset_command_buffer)
    BVB_DEVICE_MATCH("vkAllocateCommandBuffers", fake_allocate_command_buffers)
    BVB_DEVICE_MATCH("vkFreeCommandBuffers", fake_free_command_buffers)
    BVB_DEVICE_MATCH("vkBeginCommandBuffer", fake_begin_command_buffer)
    BVB_DEVICE_MATCH("vkEndCommandBuffer", fake_end_command_buffer)
    BVB_DEVICE_MATCH("vkCmdFillBuffer", fake_cmd_fill_buffer)
    BVB_DEVICE_MATCH("vkCmdPipelineBarrier", fake_cmd_pipeline_barrier)
    BVB_DEVICE_MATCH("vkCmdPipelineBarrier2", fake_cmd_pipeline_barrier_2)
    BVB_DEVICE_MATCH("vkCmdClearColorImage", fake_cmd_clear_color_image)
    BVB_DEVICE_MATCH("vkCmdCopyImageToBuffer", fake_cmd_copy_image_to_buffer)
    BVB_DEVICE_MATCH("vkQueueSubmit", fake_queue_submit)
    BVB_DEVICE_MATCH("vkQueueSubmit2", fake_queue_submit_2)
    BVB_DEVICE_MATCH("vkQueueSubmit2KHR", fake_queue_submit_2)
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
