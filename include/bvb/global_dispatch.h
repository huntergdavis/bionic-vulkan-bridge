#ifndef BVB_GLOBAL_DISPATCH_H
#define BVB_GLOBAL_DISPATCH_H

#include <stdint.h>

#include <vulkan/vulkan.h>

#if defined(__GNUC__) || defined(__clang__)
#define BVB_GLOBAL_EXPORT __attribute__((visibility("default")))
#else
#define BVB_GLOBAL_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

BVB_GLOBAL_EXPORT uint64_t bvb_instance_proxy_id(VkInstance instance);
BVB_GLOBAL_EXPORT uint64_t bvb_physical_device_proxy_id(
    VkPhysicalDevice physical_device);
BVB_GLOBAL_EXPORT uint64_t bvb_device_proxy_id(VkDevice device);
BVB_GLOBAL_EXPORT uint64_t bvb_queue_proxy_id(VkQueue queue);
BVB_GLOBAL_EXPORT uint64_t bvb_command_pool_proxy_id(
    VkCommandPool command_pool);
BVB_GLOBAL_EXPORT uint64_t bvb_command_buffer_proxy_id(
    VkCommandBuffer command_buffer);
BVB_GLOBAL_EXPORT uint64_t bvb_buffer_proxy_id(VkBuffer buffer);
BVB_GLOBAL_EXPORT uint64_t bvb_image_proxy_id(VkImage image);
BVB_GLOBAL_EXPORT uint64_t bvb_image_view_proxy_id(VkImageView image_view);
BVB_GLOBAL_EXPORT uint64_t bvb_descriptor_set_proxy_id(
    VkDescriptorSet descriptor_set);
BVB_GLOBAL_EXPORT uint64_t bvb_memory_proxy_id(VkDeviceMemory memory);
BVB_GLOBAL_EXPORT int bvb_memory_proxy_is_mapped(VkDeviceMemory memory);
BVB_GLOBAL_EXPORT uint64_t bvb_fence_proxy_id(VkFence fence);
BVB_GLOBAL_EXPORT uint64_t bvb_global_dispatch_exchange_count(void);
BVB_GLOBAL_EXPORT uint64_t bvb_command_buffer_ownership_registry_reads(
    VkCommandBuffer command_buffer);
BVB_GLOBAL_EXPORT uint16_t bvb_global_dispatch_last_opcode(void);
BVB_GLOBAL_EXPORT int bvb_global_dispatch_connection_is_open(void);
BVB_GLOBAL_EXPORT void bvb_global_diagnostic_poison_command(
    VkCommandBuffer command_buffer, const char *entry, const char *reason,
    const char *shape, int status);
BVB_GLOBAL_EXPORT int bvb_verify_memory_fill(
    VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
    uint32_t expected_word, uint32_t *mismatched_words);
PFN_vkVoidFunction bvb_global_device_proc_addr(
    VkDevice device, const char *name);
BVB_GLOBAL_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *version);
BVB_GLOBAL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);
BVB_GLOBAL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *name);

#ifdef __cplusplus
}
#endif

#endif
