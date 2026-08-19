#ifndef BVB_VULKAN_DISCOVERY_H
#define BVB_VULKAN_DISCOVERY_H

#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

enum {
    BVB_VULKAN_DISCOVERY_MAX_PAYLOAD = 4096,
    BVB_VULKAN_MAX_QUEUE_FAMILIES = 64,
    BVB_VULKAN_MAX_DEVICE_EXTENSIONS = 1024,
    BVB_VULKAN_EXTENSION_PAGE_CAPACITY = 15,
};

struct bvb_vulkan_extension_page {
    int32_t vulkan_result;
    uint32_t total_count;
    uint32_t first;
    uint32_t count;
    VkExtensionProperties properties[BVB_VULKAN_EXTENSION_PAGE_CAPACITY];
};

int bvb_vulkan_encode_physical_device_properties(
    uint8_t output[BVB_VULKAN_DISCOVERY_MAX_PAYLOAD],
    const VkPhysicalDeviceProperties *properties, uint32_t *output_length);
int bvb_vulkan_decode_physical_device_properties(
    const uint8_t *input, uint32_t input_length,
    VkPhysicalDeviceProperties *properties);
int bvb_vulkan_encode_queue_family_properties(
    uint8_t output[BVB_VULKAN_DISCOVERY_MAX_PAYLOAD],
    const VkQueueFamilyProperties *properties, uint32_t count,
    uint32_t *output_length);
int bvb_vulkan_decode_queue_family_properties(
    const uint8_t *input, uint32_t input_length,
    VkQueueFamilyProperties properties[BVB_VULKAN_MAX_QUEUE_FAMILIES],
    uint32_t *count);
int bvb_vulkan_encode_memory_properties(
    uint8_t output[BVB_VULKAN_DISCOVERY_MAX_PAYLOAD],
    const VkPhysicalDeviceMemoryProperties *properties,
    uint32_t *output_length);
int bvb_vulkan_decode_memory_properties(
    const uint8_t *input, uint32_t input_length,
    VkPhysicalDeviceMemoryProperties *properties);
int bvb_vulkan_encode_extension_page(
    uint8_t output[BVB_VULKAN_DISCOVERY_MAX_PAYLOAD],
    const struct bvb_vulkan_extension_page *page, uint32_t *output_length);
int bvb_vulkan_decode_extension_page(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_extension_page *page);

#endif
