#define VK_NO_PROTOTYPES

#include <bvb/global_dispatch.h>
#include <bvb/protocol.h>

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
        memcpy(&name, &erased, sizeof(name));                                  \
    } while (0)

#define RESOLVE_INSTANCE(instance, name)                                       \
    PFN_##name name = NULL;                                                    \
    do {                                                                       \
        PFN_vkVoidFunction erased = vkGetInstanceProcAddr(instance, #name);    \
        CHECK(erased != NULL);                                                 \
        memcpy(&name, &erased, sizeof(name));                                  \
    } while (0)

#define RESOLVE_DEVICE(device, name)                                           \
    PFN_##name name = NULL;                                                    \
    do {                                                                       \
        PFN_vkVoidFunction erased = vkGetDeviceProcAddr(device, #name);        \
        CHECK(erased != NULL);                                                 \
        memcpy(&name, &erased, sizeof(name));                                  \
    } while (0)

int main(void) {
    const char *mode = getenv("BVB_MAPPED_MEMORY");
    CHECK(mode != NULL && strcmp(mode, "direct") == 0);

    RESOLVE_GLOBAL(vkCreateInstance);
    const VkApplicationInfo application_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "bvb-direct-memory-hardware",
        .apiVersion = VK_API_VERSION_1_3,
    };
    const char *instance_extensions[] = {
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
    };
    const VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application_info,
        .enabledExtensionCount = 2U,
        .ppEnabledExtensionNames = instance_extensions,
    };
    VkInstance instance = VK_NULL_HANDLE;
    CHECK(vkCreateInstance(&instance_info, NULL, &instance) == VK_SUCCESS);

    RESOLVE_INSTANCE(instance, vkDestroyInstance);
    RESOLVE_INSTANCE(instance, vkEnumeratePhysicalDevices);
    RESOLVE_INSTANCE(instance, vkGetPhysicalDeviceQueueFamilyProperties);
    RESOLVE_INSTANCE(instance, vkGetPhysicalDeviceMemoryProperties);
    RESOLVE_INSTANCE(instance, vkCreateDevice);

    uint32_t physical_count = 1U;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    CHECK(vkEnumeratePhysicalDevices(
              instance, &physical_count, &physical_device) == VK_SUCCESS);
    CHECK(physical_count == 1U && physical_device != VK_NULL_HANDLE);

    uint32_t queue_count = 0U;
    vkGetPhysicalDeviceQueueFamilyProperties(
        physical_device, &queue_count, NULL);
    CHECK(queue_count > 0U && queue_count <= 64U);
    VkQueueFamilyProperties *queue_properties =
        calloc(queue_count, sizeof(*queue_properties));
    CHECK(queue_properties != NULL);
    vkGetPhysicalDeviceQueueFamilyProperties(
        physical_device, &queue_count, queue_properties);
    uint32_t queue_family = UINT32_MAX;
    for (uint32_t index = 0U; index < queue_count; ++index) {
        if (queue_properties[index].queueCount != 0U) {
            queue_family = index;
            break;
        }
    }
    CHECK(queue_family != UINT32_MAX);
    free(queue_properties);

    VkPhysicalDeviceMemoryProperties memory_properties = {0};
    vkGetPhysicalDeviceMemoryProperties(
        physical_device, &memory_properties);
    CHECK(memory_properties.memoryTypeCount > 0U);

    const float priority = 1.0F;
    const VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queue_family,
        .queueCount = 1U,
        .pQueuePriorities = &priority,
    };
    const VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1U,
        .pQueueCreateInfos = &queue_info,
    };
    VkDevice device = VK_NULL_HANDLE;
    CHECK(vkCreateDevice(
              physical_device, &device_info, NULL, &device) == VK_SUCCESS);

    RESOLVE_DEVICE(device, vkDestroyDevice);
    RESOLVE_DEVICE(device, vkCreateBuffer);
    RESOLVE_DEVICE(device, vkDestroyBuffer);
    RESOLVE_DEVICE(device, vkGetBufferMemoryRequirements);
    RESOLVE_DEVICE(device, vkAllocateMemory);
    RESOLVE_DEVICE(device, vkFreeMemory);
    RESOLVE_DEVICE(device, vkBindBufferMemory);
    RESOLVE_DEVICE(device, vkMapMemory);
    RESOLVE_DEVICE(device, vkUnmapMemory);
    RESOLVE_DEVICE(device, vkFlushMappedMemoryRanges);
    RESOLVE_DEVICE(device, vkInvalidateMappedMemoryRanges);

    const VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 4096U,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buffer = VK_NULL_HANDLE;
    CHECK(vkCreateBuffer(device, &buffer_info, NULL, &buffer) == VK_SUCCESS);
    VkMemoryRequirements requirements = {0};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    CHECK(requirements.size >= buffer_info.size);

    uint32_t memory_type = UINT32_MAX;
    const VkMemoryPropertyFlags required =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t index = 0U;
         index < memory_properties.memoryTypeCount; ++index) {
        if ((requirements.memoryTypeBits & (UINT32_C(1) << index)) != 0U &&
            (memory_properties.memoryTypes[index].propertyFlags & required) ==
                required) {
            memory_type = index;
            break;
        }
    }
    CHECK(memory_type != UINT32_MAX);
    const VkMemoryAllocateInfo allocation_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    CHECK(vkAllocateMemory(
              device, &allocation_info, NULL, &memory) == VK_SUCCESS);
    CHECK(vkBindBufferMemory(device, buffer, memory, 0U) == VK_SUCCESS);

    const uint64_t before_map = bvb_global_dispatch_exchange_count();
    uint32_t *mapped = NULL;
    CHECK(vkMapMemory(
              device, memory, 0U, VK_WHOLE_SIZE, 0U,
              (void **)&mapped) == VK_SUCCESS);
    CHECK(mapped != NULL);
    CHECK((uintptr_t)mapped <= UINT32_MAX);
    CHECK(bvb_global_dispatch_exchange_count() - before_map == 1U);
    CHECK(bvb_global_dispatch_last_opcode() ==
          BVB_OPCODE_VULKAN_MEMORY_DIRECT_MAP_SETUP);

    for (uint32_t index = 0U; index < 1024U; ++index)
        mapped[index] = UINT32_C(0xa5c3f00d);
    const VkMappedMemoryRange range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = memory,
        .offset = 0U,
        .size = VK_WHOLE_SIZE,
    };
    const uint64_t before_flush = bvb_global_dispatch_exchange_count();
    CHECK(vkFlushMappedMemoryRanges(device, 1U, &range) == VK_SUCCESS);
    CHECK(bvb_global_dispatch_exchange_count() == before_flush);
    uint32_t mismatches = UINT32_MAX;
    CHECK(bvb_verify_memory_fill(
              memory, 0U, 4096U, UINT32_C(0xa5c3f00d),
              &mismatches) == 0);
    CHECK(mismatches == 0U);
    const uint64_t before_invalidate = bvb_global_dispatch_exchange_count();
    CHECK(vkInvalidateMappedMemoryRanges(device, 1U, &range) == VK_SUCCESS);
    CHECK(bvb_global_dispatch_exchange_count() == before_invalidate);
    const uint64_t before_unmap = bvb_global_dispatch_exchange_count();
    vkUnmapMemory(device, memory);
    CHECK(bvb_global_dispatch_exchange_count() - before_unmap == 1U);
    CHECK(bvb_global_dispatch_last_opcode() ==
          BVB_OPCODE_VULKAN_MEMORY_MIRROR_UNMAP);

    vkDestroyBuffer(device, buffer, NULL);
    vkFreeMemory(device, memory, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    printf("direct_memory=PASS bytes=4096 mismatches=0 "
           "map_rtts=1 flush_rtts=0 invalidate_rtts=0 unmap_rtts=1\n");
    return 0;
}
