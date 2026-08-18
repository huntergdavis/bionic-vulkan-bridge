#define VK_NO_PROTOTYPES

#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define BVB_EXPORT __declspec(dllexport)
#else
#define BVB_EXPORT __attribute__((visibility("default")))
#endif

static VkDeviceMemory fake_bound_memory = VK_NULL_HANDLE;

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

static VkResult VKAPI_CALL fake_create_instance(
    const VkInstanceCreateInfo *create_info,
    const VkAllocationCallbacks *allocator,
    VkInstance *instance) {
    (void)create_info;
    (void)allocator;
    *instance = (VkInstance)(uintptr_t)0x1000U;
    return VK_SUCCESS;
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

static VkResult VKAPI_CALL fake_create_device(
    VkPhysicalDevice physical_device,
    const VkDeviceCreateInfo *create_info,
    const VkAllocationCallbacks *allocator,
    VkDevice *device) {
    (void)physical_device;
    (void)create_info;
    (void)allocator;
    *device = (VkDevice)(uintptr_t)0x3000U;
    return VK_SUCCESS;
}

static void VKAPI_CALL fake_destroy_device(
    VkDevice device,
    const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)allocator;
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
    (void)create_info;
    (void)allocator;
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
        .size = 4096,
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
    void *allocation = calloc(1, (size_t)allocate_info->allocationSize);
    if (allocation == NULL) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
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
    free((void *)(uintptr_t)memory);
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
    (void)image_barrier_count;
    (void)image_barriers;
}

static VkResult VKAPI_CALL fake_queue_submit(
    VkQueue queue,
    uint32_t submit_count,
    const VkSubmitInfo *submits,
    VkFence fence) {
    (void)queue;
    (void)submit_count;
    (void)submits;
    (void)fence;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_queue_wait_idle(VkQueue queue) {
    (void)queue;
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

static void VKAPI_CALL fake_destroy_instance(
    VkInstance instance,
    const VkAllocationCallbacks *allocator) {
    (void)instance;
    (void)allocator;
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
    BVB_MATCH("vkCreateInstance", fake_create_instance)
    BVB_MATCH("vkEnumeratePhysicalDevices", fake_enumerate_devices)
    BVB_MATCH("vkGetPhysicalDeviceProperties", fake_get_device_properties)
    BVB_MATCH("vkGetPhysicalDeviceQueueFamilyProperties", fake_get_queue_properties)
    BVB_MATCH("vkGetPhysicalDeviceMemoryProperties", fake_get_memory_properties)
    BVB_MATCH("vkEnumerateDeviceExtensionProperties", fake_enumerate_device_extensions)
    BVB_MATCH("vkCreateDevice", fake_create_device)
    BVB_MATCH("vkGetDeviceProcAddr", fake_get_device_proc_addr)
    BVB_MATCH("vkDestroyInstance", fake_destroy_instance)
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
    BVB_DEVICE_MATCH("vkCreateBuffer", fake_create_buffer)
    BVB_DEVICE_MATCH("vkDestroyBuffer", fake_destroy_buffer)
    BVB_DEVICE_MATCH("vkGetBufferMemoryRequirements",
                     fake_get_buffer_memory_requirements)
    BVB_DEVICE_MATCH("vkAllocateMemory", fake_allocate_memory)
    BVB_DEVICE_MATCH("vkFreeMemory", fake_free_memory)
    BVB_DEVICE_MATCH("vkBindBufferMemory", fake_bind_buffer_memory)
    BVB_DEVICE_MATCH("vkCreateCommandPool", fake_create_command_pool)
    BVB_DEVICE_MATCH("vkDestroyCommandPool", fake_destroy_command_pool)
    BVB_DEVICE_MATCH("vkAllocateCommandBuffers", fake_allocate_command_buffers)
    BVB_DEVICE_MATCH("vkBeginCommandBuffer", fake_begin_command_buffer)
    BVB_DEVICE_MATCH("vkEndCommandBuffer", fake_end_command_buffer)
    BVB_DEVICE_MATCH("vkCmdFillBuffer", fake_cmd_fill_buffer)
    BVB_DEVICE_MATCH("vkCmdPipelineBarrier", fake_cmd_pipeline_barrier)
    BVB_DEVICE_MATCH("vkQueueSubmit", fake_queue_submit)
    BVB_DEVICE_MATCH("vkQueueWaitIdle", fake_queue_wait_idle)
    BVB_DEVICE_MATCH("vkMapMemory", fake_map_memory)
    BVB_DEVICE_MATCH("vkUnmapMemory", fake_unmap_memory)
    BVB_DEVICE_MATCH("vkInvalidateMappedMemoryRanges",
                     fake_invalidate_mapped_ranges)
#undef BVB_DEVICE_MATCH
    return NULL;
}

BVB_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
    VkInstance instance,
    const char *name) {
    (void)instance;
    return function_pointer(name);
}
