#ifndef BVB_VULKAN_GLOBAL_H
#define BVB_VULKAN_GLOBAL_H

#include <bvb/protocol.h>
#include <bvb/vulkan_descriptor_wire.h>
#include <bvb/vulkan_discovery.h>
#include <bvb/vulkan_pipeline_wire.h>

#include <stdbool.h>
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
int bvb_vulkan_global_context_enumerate_instance_extensions(
    const struct bvb_vulkan_global_context *context,
    struct bvb_vulkan_extension_page *page);
int bvb_vulkan_global_context_create_instance(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_instance_create_request *request,
    const char *const *enabled_extensions,
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
int bvb_vulkan_global_context_get_core_features(
    const struct bvb_vulkan_global_context *context,
    uint64_t physical_device_id,
    struct bvb_vulkan_core_features *features,
    char *error, size_t error_size);
int bvb_vulkan_global_context_get_format_properties(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_format_query *query,
    struct bvb_vulkan_format_properties *properties,
    char *error, size_t error_size);
int bvb_vulkan_global_context_get_format_properties_3(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_format_query *query,
    struct bvb_vulkan_format_properties_3 *properties,
    char *error, size_t error_size);
int bvb_vulkan_global_context_get_image_format_properties(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_image_format_query *query,
    struct bvb_vulkan_image_format_properties *properties,
    char *error, size_t error_size);
int bvb_vulkan_global_context_get_external_buffer_properties(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_external_buffer_query *query,
    struct bvb_vulkan_external_buffer_properties *properties,
    char *error, size_t error_size);
int bvb_vulkan_global_context_get_external_semaphore_properties(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_external_semaphore_query *query,
    struct bvb_vulkan_external_semaphore_properties *properties,
    char *error, size_t error_size);
int bvb_vulkan_global_context_create_device(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_device_create_request *request,
    const char *const *enabled_extensions,
    struct bvb_vulkan_device_create_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_create_device_packed(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_device_create_packed_request *request,
    const char *const *enabled_extensions,
    struct bvb_vulkan_device_create_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_destroy_device(
    struct bvb_vulkan_global_context *context, uint64_t device_id);
int bvb_vulkan_global_context_create_descriptor_set_layout(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_descriptor_set_layout_create_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_destroy_descriptor_set_layout(
    struct bvb_vulkan_global_context *context, uint64_t layout_id,
    char *error, size_t error_size);
int bvb_vulkan_global_context_create_descriptor_update_template(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_descriptor_update_template_create_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_destroy_descriptor_update_template(
    struct bvb_vulkan_global_context *context, uint64_t template_id,
    char *error, size_t error_size);
int bvb_vulkan_global_context_create_descriptor_pool(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_descriptor_pool_create_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_destroy_descriptor_pool(
    struct bvb_vulkan_global_context *context, uint64_t pool_id,
    char *error, size_t error_size);
int bvb_vulkan_global_context_create_query_pool(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_query_pool_create_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_destroy_query_pool(
    struct bvb_vulkan_global_context *context, uint64_t query_pool_id,
    char *error, size_t error_size);
int bvb_vulkan_global_context_get_query_pool_results(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_query_pool_results_request *request,
    struct bvb_vulkan_query_pool_results_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_reset_query_pool(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_query_pool_reset_request *request,
    char *error, size_t error_size);
int bvb_vulkan_global_context_reset_descriptor_pool(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_descriptor_pool_reset_request *request,
    int32_t *vulkan_result, char *error, size_t error_size);
int bvb_vulkan_global_context_allocate_descriptor_sets(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_descriptor_set_allocate_request *request,
    struct bvb_vulkan_descriptor_set_allocate_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_create_sampler(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_sampler_create_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_destroy_sampler(
    struct bvb_vulkan_global_context *context, uint64_t sampler_id,
    char *error, size_t error_size);
int bvb_vulkan_global_context_update_descriptors(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_descriptor_update_request *request,
    char *error, size_t error_size);
int bvb_vulkan_global_context_update_descriptor_set_with_template(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_descriptor_template_update_request *request,
    char *error, size_t error_size);
int bvb_vulkan_global_context_create_pipeline_layout(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_pipeline_layout_create_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_destroy_pipeline_layout(
    struct bvb_vulkan_global_context *context, uint64_t pipeline_layout_id,
    char *error, size_t error_size);
int bvb_vulkan_global_context_create_graphics_pipeline(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_graphics_pipeline_create_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_create_builtin_graphics_pipeline(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_builtin_graphics_pipeline_create_request *request,
    int blob_fd, struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_create_general_graphics_pipeline(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_builtin_graphics_pipeline_create_request *request,
    int blob_fd, struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_destroy_pipeline(
    struct bvb_vulkan_global_context *context, uint64_t pipeline_id,
    char *error, size_t error_size);
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
    struct bvb_vulkan_global_context *context,
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
int bvb_vulkan_global_context_get_device_buffer_requirements(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_device_buffer_requirements_request *request,
    struct bvb_vulkan_device_buffer_requirements_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_get_buffer_requirements_2(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_buffer_requirements_2_request *request,
    struct bvb_vulkan_buffer_requirements_2_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_get_buffer_device_address(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_buffer_device_address_request *request,
    struct bvb_vulkan_buffer_device_address_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_allocate_memory(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_memory_allocate_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_allocate_memory_extended(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_memory_allocate_extended_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_free_memory(
    struct bvb_vulkan_global_context *context, uint64_t memory_id,
    char *error, size_t error_size);
int bvb_vulkan_global_context_bind_buffer_memory(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_buffer_bind_request *request,
    int32_t *vulkan_result, char *error, size_t error_size);
int bvb_vulkan_global_context_create_image(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_image_create_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_destroy_image(
    struct bvb_vulkan_global_context *context, uint64_t image_id,
    char *error, size_t error_size);
int bvb_vulkan_global_context_get_image_requirements(
    const struct bvb_vulkan_global_context *context, uint64_t image_id,
    struct bvb_vulkan_image_requirements *requirements,
    char *error, size_t error_size);
int bvb_vulkan_global_context_get_image_requirements_2(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_image_requirements_2_request *request,
    struct bvb_vulkan_image_requirements_2_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_bind_image_memory(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_image_bind_request *request,
    int32_t *vulkan_result, char *error, size_t error_size);
int bvb_vulkan_global_context_create_image_view(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_image_view_create_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_destroy_image_view(
    struct bvb_vulkan_global_context *context, uint64_t image_view_id,
    char *error, size_t error_size);
int bvb_vulkan_global_context_command_buffer_fill(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_command_buffer_fill_request *request,
    char *error, size_t error_size);
int bvb_vulkan_global_context_command_buffer_image_barrier(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_command_buffer_image_barrier_request *request,
    char *error, size_t error_size);
int bvb_vulkan_global_context_command_buffer_clear_color_image(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_command_buffer_clear_color_image_request *request,
    char *error, size_t error_size);
int bvb_vulkan_global_context_command_buffer_bind_descriptor_sets(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_bind_descriptor_sets_request *request,
    char *error, size_t error_size);
int bvb_vulkan_global_context_execute_immediate_record(
    const struct bvb_vulkan_global_context *context,
    const uint8_t *batch, size_t batch_length,
    char *error, size_t error_size);
int bvb_vulkan_global_context_validate_queue_submit_2(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_queue_submit_2_request *request,
    uint64_t *device_id, char *error, size_t error_size);
int bvb_vulkan_global_context_validate_command_stream(
    const struct bvb_vulkan_global_context *context,
    const uint8_t *batch, size_t batch_length, uint64_t expected_device_id,
    char *error, size_t error_size);
int bvb_vulkan_global_context_replay_command_stream(
    const struct bvb_vulkan_global_context *context,
    const uint8_t *batch, size_t batch_length, uint64_t expected_device_id,
    char *error, size_t error_size);
bool bvb_vulkan_global_context_command_buffer_is_live(
    const struct bvb_vulkan_global_context *context,
    uint64_t command_buffer_id);
int bvb_vulkan_global_context_verify_memory_fill(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_memory_verify_fill_request *request,
    struct bvb_vulkan_memory_verify_fill_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_write_memory(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_memory_io_request *request, const uint8_t *data,
    int32_t *vulkan_result, char *error, size_t error_size);
int bvb_vulkan_global_context_read_memory(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_memory_io_request *request, uint8_t *data,
    uint32_t capacity, uint32_t *length, int32_t *vulkan_result,
    char *error, size_t error_size);
int bvb_vulkan_global_context_setup_memory_mirror(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_memory_mirror_setup_request *request,
    int mirror_fd, int32_t *vulkan_result, char *error, size_t error_size);
int bvb_vulkan_global_context_flush_memory_mirror(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_memory_mirror_range_request *request,
    int32_t *vulkan_result, char *error, size_t error_size);
int bvb_vulkan_global_context_invalidate_memory_mirror(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_memory_mirror_range_request *request,
    int32_t *vulkan_result, char *error, size_t error_size);
int bvb_vulkan_global_context_unmap_memory_mirror(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_memory_mirror_unmap_request *request,
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
int bvb_vulkan_global_context_create_semaphore(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_semaphore_create_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_destroy_semaphore(
    struct bvb_vulkan_global_context *context, uint64_t semaphore_id,
    char *error, size_t error_size);
int bvb_vulkan_global_context_get_semaphore_counter(
    const struct bvb_vulkan_global_context *context, uint64_t semaphore_id,
    struct bvb_vulkan_semaphore_counter_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_wait_semaphores(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_semaphore_wait_request *request,
    int32_t *vulkan_result, char *error, size_t error_size);
int bvb_vulkan_global_context_signal_semaphore(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_semaphore_signal_request *request,
    int32_t *vulkan_result, char *error, size_t error_size);
int bvb_vulkan_global_context_queue_submit_2(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_queue_submit_2_request *request,
    int32_t *vulkan_result, char *error, size_t error_size);
int bvb_vulkan_global_context_queue_submit_command_fence(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_queue_submit_command_fence_request *request,
    int32_t *vulkan_result, char *error, size_t error_size);
int bvb_vulkan_global_context_prepare_swapchain(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_swapchain_prepare_request *request,
    struct bvb_vulkan_swapchain_prepare_response *response,
    int descriptors[BVB_WSI_FRAME_RING_MAX_SLOTS + 1U],
    size_t *descriptor_count,
    void *hardware_buffers[BVB_WSI_FRAME_RING_MAX_SLOTS],
    size_t *hardware_buffer_count, char *error, size_t error_size);
int bvb_vulkan_global_context_acquire_swapchain_image(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_swapchain_acquire_request *request,
    struct bvb_vulkan_swapchain_acquire_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_present_swapchain_image(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_swapchain_present_request *request,
    struct bvb_vulkan_swapchain_present_response *response,
    char *error, size_t error_size);
int bvb_vulkan_global_context_destroy_swapchain(
    struct bvb_vulkan_global_context *context, uint64_t swapchain_id,
    char *error, size_t error_size);

#endif
