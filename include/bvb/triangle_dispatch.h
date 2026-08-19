#ifndef BVB_TRIANGLE_DISPATCH_H
#define BVB_TRIANGLE_DISPATCH_H

#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#if defined(__GNUC__) || defined(__clang__)
#define BVB_TRIANGLE_EXPORT __attribute__((visibility("default")))
#else
#define BVB_TRIANGLE_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

BVB_TRIANGLE_EXPORT VkCommandBuffer bvb_triangle_command_buffer_create(
    uint8_t *batch, size_t capacity, uint64_t command_buffer_id,
    uint64_t sequence);
BVB_TRIANGLE_EXPORT int bvb_triangle_command_buffer_finish(
    VkCommandBuffer command_buffer, size_t *batch_length);
BVB_TRIANGLE_EXPORT int bvb_triangle_command_buffer_status(
    VkCommandBuffer command_buffer);
BVB_TRIANGLE_EXPORT void bvb_triangle_command_buffer_destroy(
    VkCommandBuffer command_buffer);

#ifdef __cplusplus
}
#endif

#endif
