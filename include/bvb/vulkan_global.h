#ifndef BVB_VULKAN_GLOBAL_H
#define BVB_VULKAN_GLOBAL_H

#include <bvb/protocol.h>

#include <stddef.h>

struct bvb_vulkan_global_context;

int bvb_vulkan_global_context_create(
    const char *loader_path, struct bvb_vulkan_global_context **context,
    char *error, size_t error_size);
void bvb_vulkan_global_context_destroy(
    struct bvb_vulkan_global_context *context);
int bvb_vulkan_global_context_info(
    const struct bvb_vulkan_global_context *context,
    struct bvb_vulkan_global_info *info);
int bvb_vulkan_global_context_create_instance(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_instance_create_request *request,
    struct bvb_vulkan_instance_create_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_destroy_instance(
    struct bvb_vulkan_global_context *context, uint64_t instance_id);
int bvb_vulkan_global_context_enumerate_physical_devices(
    struct bvb_vulkan_global_context *context, uint64_t instance_id,
    struct bvb_vulkan_physical_devices *devices,
    char *error, size_t error_size);

#endif
