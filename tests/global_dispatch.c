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

    PFN_vkGetPhysicalDeviceProperties get_physical_device_properties = NULL;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_properties = NULL;
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties = NULL;
    PFN_vkGetPhysicalDeviceFormatProperties get_format_properties = NULL;
    PFN_vkGetPhysicalDeviceImageFormatProperties get_image_format_properties =
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
    RESOLVE_INSTANCE(vkEnumerateDeviceExtensionProperties,
                     enumerate_device_extensions);
#undef RESOLVE_INSTANCE

    VkPhysicalDeviceProperties properties;
    get_physical_device_properties(physical_device, &properties);
    CHECK(properties.apiVersion >= VK_API_VERSION_1_0);
    CHECK(properties.vendorID != 0U);
    CHECK(properties.deviceName[0] != '\0');

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
    VkQueue queue = VK_NULL_HANDLE;
    get_device_queue(device, queue_family_index, 0U, &queue);
    CHECK(queue != VK_NULL_HANDLE);
    const uint64_t queue_id = bvb_queue_proxy_id(queue);
    CHECK(bvb_handle_type(queue_id) == BVB_OBJECT_QUEUE);
    CHECK(bvb_handle_serial(queue_id) == 1U);
    VkQueue repeated_queue = VK_NULL_HANDLE;
    get_device_queue(device, queue_family_index, 0U, &repeated_queue);
    CHECK(repeated_queue == queue);
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
