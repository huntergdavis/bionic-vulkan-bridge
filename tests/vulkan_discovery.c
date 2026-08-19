#include <bvb/vulkan_discovery.h>

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #expression);                                               \
            return EXIT_FAILURE;                                                \
        }                                                                       \
    } while (0)

int main(void) {
    uint8_t wire[BVB_VULKAN_DISCOVERY_MAX_PAYLOAD];
    uint32_t length = 0U;

    VkPhysicalDeviceFeatures features = {0};
    features.robustBufferAccess = VK_TRUE;
    features.geometryShader = VK_TRUE;
    features.samplerAnisotropy = VK_TRUE;
    features.shaderInt64 = VK_TRUE;
    features.inheritedQueries = VK_TRUE;
    CHECK(bvb_vulkan_encode_physical_device_features(
              wire, &features, &length) == 0);
    CHECK(length == 220U);
    VkPhysicalDeviceFeatures features_decoded;
    CHECK(bvb_vulkan_decode_physical_device_features(
              wire, length, &features_decoded) == 0);
    CHECK(features_decoded.robustBufferAccess == VK_TRUE);
    CHECK(features_decoded.geometryShader == VK_TRUE);
    CHECK(features_decoded.samplerAnisotropy == VK_TRUE);
    CHECK(features_decoded.shaderInt64 == VK_TRUE);
    CHECK(features_decoded.inheritedQueries == VK_TRUE);

    VkPhysicalDeviceProperties properties = {0};
    properties.apiVersion = VK_API_VERSION_1_3;
    properties.driverVersion = UINT32_C(0x12345678);
    properties.vendorID = UINT32_C(0x5143);
    properties.deviceID = UINT32_C(0x0730);
    properties.deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    (void)snprintf(properties.deviceName, sizeof(properties.deviceName), "%s",
                   "BVB Adreno 730");
    properties.pipelineCacheUUID[15] = UINT8_C(0xa5);
    properties.limits.maxImageDimension2D = 16384U;
    properties.limits.maxComputeWorkGroupCount[2] = 65535U;
    properties.limits.maxSamplerAnisotropy = 16.0F;
    properties.limits.viewportBoundsRange[0] = -32768.0F;
    properties.limits.minMemoryMapAlignment = 4096U;
    properties.limits.nonCoherentAtomSize = UINT64_C(0x100000000);
    properties.sparseProperties.residencyNonResidentStrict = VK_TRUE;
    CHECK(bvb_vulkan_encode_physical_device_properties(
              wire, &properties, &length) == 0);
    const uint32_t properties_length = length;
    CHECK(length > sizeof(properties.deviceName));
    CHECK(length < sizeof(wire));
    VkPhysicalDeviceProperties properties_decoded;
    CHECK(bvb_vulkan_decode_physical_device_properties(
              wire, length, &properties_decoded) == 0);
    CHECK(properties_decoded.apiVersion == properties.apiVersion);
    CHECK(properties_decoded.driverVersion == properties.driverVersion);
    CHECK(strcmp(properties_decoded.deviceName, properties.deviceName) == 0);
    CHECK(properties_decoded.pipelineCacheUUID[15] == UINT8_C(0xa5));
    CHECK(properties_decoded.limits.maxImageDimension2D == 16384U);
    CHECK(properties_decoded.limits.maxComputeWorkGroupCount[2] == 65535U);
    CHECK(properties_decoded.limits.maxSamplerAnisotropy == 16.0F);
    CHECK(properties_decoded.limits.viewportBoundsRange[0] == -32768.0F);
    CHECK(properties_decoded.limits.minMemoryMapAlignment == 4096U);
    CHECK(properties_decoded.limits.nonCoherentAtomSize ==
          UINT64_C(0x100000000));
    CHECK(properties_decoded.sparseProperties.residencyNonResidentStrict ==
          VK_TRUE);
    wire[20] = 'x';
    memset(wire + 20, 'x', VK_MAX_PHYSICAL_DEVICE_NAME_SIZE);
    CHECK(bvb_vulkan_decode_physical_device_properties(
              wire, length, &properties_decoded) == -EPROTO);

    const VkQueueFamilyProperties queues[2] = {
        {
            .queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT,
            .queueCount = 2U,
            .timestampValidBits = 48U,
            .minImageTransferGranularity = {1U, 2U, 3U},
        },
        {
            .queueFlags = VK_QUEUE_TRANSFER_BIT,
            .queueCount = 1U,
            .minImageTransferGranularity = {4U, 5U, 6U},
        },
    };
    CHECK(bvb_vulkan_encode_queue_family_properties(
              wire, queues, 2U, &length) == 0);
    CHECK(length == 52U);
    VkQueueFamilyProperties queues_decoded[BVB_VULKAN_MAX_QUEUE_FAMILIES];
    uint32_t queue_count = 0U;
    CHECK(bvb_vulkan_decode_queue_family_properties(
              wire, length, queues_decoded, &queue_count) == 0);
    CHECK(queue_count == 2U);
    CHECK(queues_decoded[0].timestampValidBits == 48U);
    CHECK(queues_decoded[1].minImageTransferGranularity.depth == 6U);

    VkPhysicalDeviceMemoryProperties memory = {0};
    memory.memoryTypeCount = 2U;
    memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    memory.memoryTypes[0].heapIndex = 0U;
    memory.memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    memory.memoryTypes[1].heapIndex = 1U;
    memory.memoryHeapCount = 2U;
    memory.memoryHeaps[0].size = UINT64_C(0x100000000);
    memory.memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    memory.memoryHeaps[1].size = UINT64_C(0x20000000);
    CHECK(bvb_vulkan_encode_memory_properties(wire, &memory, &length) == 0);
    CHECK(length == 456U);
    VkPhysicalDeviceMemoryProperties memory_decoded;
    CHECK(bvb_vulkan_decode_memory_properties(
              wire, length, &memory_decoded) == 0);
    CHECK(memory_decoded.memoryTypeCount == 2U);
    CHECK(memory_decoded.memoryTypes[1].heapIndex == 1U);
    CHECK(memory_decoded.memoryHeapCount == 2U);
    CHECK(memory_decoded.memoryHeaps[0].size == UINT64_C(0x100000000));

    struct bvb_vulkan_extension_page page = {
        .vulkan_result = VK_SUCCESS,
        .total_count = 31U,
        .first = 15U,
        .count = 2U,
    };
    (void)snprintf(page.properties[0].extensionName,
                   sizeof(page.properties[0].extensionName), "%s",
                   "VK_KHR_swapchain");
    page.properties[0].specVersion = 70U;
    (void)snprintf(page.properties[1].extensionName,
                   sizeof(page.properties[1].extensionName), "%s",
                   "VK_KHR_dynamic_rendering");
    page.properties[1].specVersion = 1U;
    CHECK(bvb_vulkan_encode_extension_page(wire, &page, &length) == 0);
    CHECK(length == 536U);
    struct bvb_vulkan_extension_page page_decoded;
    CHECK(bvb_vulkan_decode_extension_page(
              wire, length, &page_decoded) == 0);
    CHECK(page_decoded.total_count == 31U);
    CHECK(page_decoded.first == 15U);
    CHECK(page_decoded.count == 2U);
    CHECK(strcmp(page_decoded.properties[1].extensionName,
                 "VK_KHR_dynamic_rendering") == 0);

    printf("PASS: Vulkan discovery fixed-width wire features=220 "
           "properties=%u queues=52 memory=456 extension_page=536\n",
           properties_length);
    return EXIT_SUCCESS;
}
