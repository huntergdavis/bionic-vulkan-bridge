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

#ifdef __cplusplus
}
#endif

#endif
