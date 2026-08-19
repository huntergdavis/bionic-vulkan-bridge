#ifndef BVB_VULKAN_SELFTEST_H
#define BVB_VULKAN_SELFTEST_H

#include <stddef.h>
#include <stdint.h>

enum bvb_instance_extension_flag {
    BVB_INSTANCE_KHR_SURFACE = UINT64_C(1) << 0,
    BVB_INSTANCE_KHR_ANDROID_SURFACE = UINT64_C(1) << 1,
    BVB_INSTANCE_EXT_HEADLESS_SURFACE = UINT64_C(1) << 2,
    BVB_INSTANCE_KHR_GET_PROPERTIES_2 = UINT64_C(1) << 3,
    BVB_INSTANCE_KHR_EXTERNAL_MEMORY_CAPS = UINT64_C(1) << 4,
    BVB_INSTANCE_KHR_EXTERNAL_SEMAPHORE_CAPS = UINT64_C(1) << 5,
};

enum bvb_device_extension_flag {
    BVB_DEVICE_KHR_SWAPCHAIN = UINT64_C(1) << 0,
    BVB_DEVICE_KHR_EXTERNAL_MEMORY = UINT64_C(1) << 1,
    BVB_DEVICE_KHR_EXTERNAL_MEMORY_FD = UINT64_C(1) << 2,
    BVB_DEVICE_ANDROID_HARDWARE_BUFFER = UINT64_C(1) << 3,
    BVB_DEVICE_KHR_EXTERNAL_SEMAPHORE = UINT64_C(1) << 4,
    BVB_DEVICE_KHR_EXTERNAL_SEMAPHORE_FD = UINT64_C(1) << 5,
    BVB_DEVICE_KHR_TIMELINE_SEMAPHORE = UINT64_C(1) << 6,
    BVB_DEVICE_KHR_EXTERNAL_FENCE_FD = UINT64_C(1) << 7,
    BVB_DEVICE_KHR_DYNAMIC_RENDERING = UINT64_C(1) << 8,
};

struct bvb_vulkan_selftest_result {
    uint32_t instance_extension_count;
    uint32_t device_extension_count;
    uint64_t instance_extension_flags;
    uint64_t device_extension_flags;
    uint32_t queue_family_index;
    uint32_t queue_flags;
    uint32_t memory_type_index;
    uint32_t memory_property_flags;
    uint32_t buffer_bytes;
    uint32_t fill_word;
    uint32_t mismatched_words;
    uint64_t submit_wait_elapsed_ns;
};

struct bvb_vulkan_external_memory_result {
    uint32_t external_memory_features;
    uint32_t compatible_handle_types;
    uint32_t export_from_imported_handle_types;
    uint32_t memory_type_index;
    uint32_t memory_property_flags;
    uint32_t buffer_bytes;
    uint32_t mismatched_bytes;
};

struct bvb_vulkan_batch_context;

int bvb_vulkan_batch_context_create(
    const char *loader_path, struct bvb_vulkan_batch_context **context,
    char *error, size_t error_size);
int bvb_vulkan_batch_context_execute(
    struct bvb_vulkan_batch_context *context, const uint8_t *batch,
    size_t batch_length, struct bvb_vulkan_selftest_result *result,
    char *error, size_t error_size);
int bvb_vulkan_batch_context_external_memory_test(
    struct bvb_vulkan_batch_context *context,
    struct bvb_vulkan_external_memory_result *result,
    char *error, size_t error_size);
/* Consumes external_fd on every return path. */
int bvb_vulkan_batch_context_import_external_memory_fd(
    struct bvb_vulkan_batch_context *context, int external_fd,
    uint64_t allocation_size, uint32_t memory_type_index,
    uint32_t buffer_bytes, struct bvb_vulkan_external_memory_result *result,
    char *error, size_t error_size);
void bvb_vulkan_batch_context_destroy(
    struct bvb_vulkan_batch_context *context);

int bvb_vulkan_run_selftest(const char *loader_path,
                            struct bvb_vulkan_selftest_result *result,
                            char *error, size_t error_size);
int bvb_vulkan_run_batched_selftest(
    const char *loader_path, const uint8_t *batch, size_t batch_length,
    struct bvb_vulkan_selftest_result *result, char *error, size_t error_size);

#endif
