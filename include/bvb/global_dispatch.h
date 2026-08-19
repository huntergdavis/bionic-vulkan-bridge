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
PFN_vkVoidFunction bvb_global_device_proc_addr(
    VkDevice device, const char *name);

#ifdef __cplusplus
}
#endif

#endif
