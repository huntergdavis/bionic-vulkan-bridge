#ifndef BVB_VULKAN_GLOBAL_H
#define BVB_VULKAN_GLOBAL_H

#include <bvb/protocol.h>
#include <bvb/vulkan_discovery.h>

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
int bvb_vulkan_global_context_get_physical_device_properties(
    const struct bvb_vulkan_global_context *context,
    uint64_t physical_device_id, VkPhysicalDeviceProperties *properties,
    char *error, size_t error_size);
int bvb_vulkan_global_context_get_queue_family_properties(
    const struct bvb_vulkan_global_context *context,
    uint64_t physical_device_id,
    VkQueueFamilyProperties properties[BVB_VULKAN_MAX_QUEUE_FAMILIES],
    uint32_t *count, char *error, size_t error_size);
int bvb_vulkan_global_context_get_memory_properties(
    const struct bvb_vulkan_global_context *context,
    uint64_t physical_device_id,
    VkPhysicalDeviceMemoryProperties *properties,
    char *error, size_t error_size);
int bvb_vulkan_global_context_enumerate_device_extensions(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_device_extension_query *query,
    struct bvb_vulkan_extension_page *page,
    char *error, size_t error_size);
int bvb_vulkan_global_context_get_physical_device_features(
    const struct bvb_vulkan_global_context *context,
    uint64_t physical_device_id, VkPhysicalDeviceFeatures *features,
    char *error, size_t error_size);
int bvb_vulkan_global_context_create_device(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_device_create_request *request,
    struct bvb_vulkan_device_create_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_destroy_device(
    struct bvb_vulkan_global_context *context, uint64_t device_id);
int bvb_vulkan_global_context_get_device_queue(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_device_queue_request *request,
    uint64_t *queue_id, char *error, size_t error_size);
int bvb_vulkan_global_context_queue_submit_empty(
    const struct bvb_vulkan_global_context *context, uint64_t queue_id,
    int32_t *vulkan_result, char *error, size_t error_size);
int bvb_vulkan_global_context_queue_wait_idle(
    const struct bvb_vulkan_global_context *context, uint64_t queue_id,
    int32_t *vulkan_result, char *error, size_t error_size);
int bvb_vulkan_global_context_device_wait_idle(
    const struct bvb_vulkan_global_context *context, uint64_t device_id,
    int32_t *vulkan_result, char *error, size_t error_size);
int bvb_vulkan_global_context_create_command_pool(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_command_pool_create_request *request,
    struct bvb_vulkan_command_pool_create_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_destroy_command_pool(
    struct bvb_vulkan_global_context *context, uint64_t command_pool_id,
    char *error, size_t error_size);
int bvb_vulkan_global_context_reset_command_pool(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_command_pool_reset_request *request,
    int32_t *vulkan_result, char *error, size_t error_size);
int bvb_vulkan_global_context_allocate_command_buffer(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_command_buffer_allocate_request *request,
    struct bvb_vulkan_command_buffer_allocate_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_free_command_buffer(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_command_buffer_free_request *request,
    char *error, size_t error_size);
int bvb_vulkan_global_context_begin_command_buffer(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_command_buffer_begin_request *request,
    int32_t *vulkan_result, char *error, size_t error_size);
int bvb_vulkan_global_context_end_command_buffer(
    const struct bvb_vulkan_global_context *context,
    uint64_t command_buffer_id, int32_t *vulkan_result,
    char *error, size_t error_size);
int bvb_vulkan_global_context_queue_submit_command(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_queue_submit_command_request *request,
    int32_t *vulkan_result, char *error, size_t error_size);
int bvb_vulkan_global_context_create_buffer(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_buffer_create_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_destroy_buffer(
    struct bvb_vulkan_global_context *context, uint64_t buffer_id,
    char *error, size_t error_size);
int bvb_vulkan_global_context_get_buffer_requirements(
    const struct bvb_vulkan_global_context *context, uint64_t buffer_id,
    struct bvb_vulkan_buffer_requirements *requirements,
    char *error, size_t error_size);
int bvb_vulkan_global_context_allocate_memory(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_memory_allocate_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_free_memory(
    struct bvb_vulkan_global_context *context, uint64_t memory_id,
    char *error, size_t error_size);
int bvb_vulkan_global_context_bind_buffer_memory(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_buffer_bind_request *request,
    int32_t *vulkan_result, char *error, size_t error_size);
int bvb_vulkan_global_context_command_buffer_fill(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_command_buffer_fill_request *request,
    char *error, size_t error_size);
int bvb_vulkan_global_context_verify_memory_fill(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_memory_verify_fill_request *request,
    struct bvb_vulkan_memory_verify_fill_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_create_fence(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_fence_create_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_destroy_fence(
    struct bvb_vulkan_global_context *context, uint64_t fence_id,
    char *error, size_t error_size);
int bvb_vulkan_global_context_get_fence_status(
    const struct bvb_vulkan_global_context *context, uint64_t fence_id,
    int32_t *vulkan_result, char *error, size_t error_size);
int bvb_vulkan_global_context_wait_fence(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_fence_wait_request *request,
    int32_t *vulkan_result, char *error, size_t error_size);
int bvb_vulkan_global_context_reset_fence(
    const struct bvb_vulkan_global_context *context, uint64_t fence_id,
    int32_t *vulkan_result, char *error, size_t error_size);
int bvb_vulkan_global_context_queue_submit_command_fence(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_queue_submit_command_fence_request *request,
    int32_t *vulkan_result, char *error, size_t error_size);

#endif
