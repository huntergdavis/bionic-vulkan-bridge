#define VK_NO_PROTOTYPES

#include <bvb/handle.h>
#include <bvb/vulkan_global.h>
#include <bvb/wsi_frame_ring.h>

#include <vulkan/vulkan.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #expression);                                               \
            return 1;                                                           \
        }                                                                       \
    } while (0)

int main(int argc, char **argv) {
    CHECK(argc == 2);
    char error[512] = {0};
    struct bvb_vulkan_global_context *context = NULL;
    CHECK(bvb_vulkan_global_context_create(
              argv[1], &context, error, sizeof(error)) == 0);

    const struct bvb_vulkan_instance_create_request instance_request = {
        .api_version = VK_API_VERSION_1_1,
    };
    struct bvb_vulkan_instance_create_response instance = {0};
    CHECK(bvb_vulkan_global_context_create_instance(
              context, &instance_request, NULL, &instance,
              error, sizeof(error)) == 0);
    CHECK(instance.vulkan_result == VK_SUCCESS);
    struct bvb_vulkan_physical_devices physical = {0};
    CHECK(bvb_vulkan_global_context_enumerate_physical_devices(
              context, instance.instance_id, &physical,
              error, sizeof(error)) == 0);
    CHECK(physical.vulkan_result == VK_SUCCESS && physical.count == 1U);

    static const char *const extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    };
    const float priority = 1.0F;
    uint32_t priority_bits = 0U;
    _Static_assert(sizeof(priority_bits) == sizeof(priority),
                   "queue priority wire width mismatch");
    __builtin_memcpy(&priority_bits, &priority, sizeof(priority_bits));
    const struct bvb_vulkan_device_create_request device_request = {
        .physical_device_id = physical.ids[0],
        .queue_family_index = 0U,
        .queue_count = 1U,
        .queue_priority_bits = priority_bits,
        .enabled_extension_count = 3U,
    };
    struct bvb_vulkan_device_create_response device = {0};
    CHECK(bvb_vulkan_global_context_create_device(
              context, &device_request, extensions, &device,
              error, sizeof(error)) == 0);
    CHECK(device.vulkan_result == VK_SUCCESS);
    const struct bvb_vulkan_device_queue_request queue_request = {
        .device_id = device.device_id,
        .queue_family_index = 0U,
    };
    uint64_t queue_id = 0U;
    CHECK(bvb_vulkan_global_context_get_device_queue(
              context, &queue_request, &queue_id,
              error, sizeof(error)) == 0);
    CHECK(bvb_handle_type(queue_id) == BVB_OBJECT_QUEUE);
    const struct bvb_vulkan_semaphore_create_request semaphore_request = {
        .device_id = device.device_id,
        .semaphore_type = VK_SEMAPHORE_TYPE_BINARY,
    };
    struct bvb_vulkan_object_create_response semaphore = {0};
    CHECK(bvb_vulkan_global_context_create_semaphore(
              context, &semaphore_request, &semaphore,
              error, sizeof(error)) == 0);
    CHECK(semaphore.vulkan_result == VK_SUCCESS);

    const struct bvb_vulkan_swapchain_prepare_request request = {
        .device_id = device.device_id,
        .width = 64U,
        .height = 64U,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .image_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .min_image_count = 3U,
        .generation = UINT64_C(0xe053000000000001),
    };
    struct bvb_vulkan_swapchain_prepare_response response = {0};
    int descriptors[BVB_WSI_FRAME_RING_MAX_SLOTS + 1U];
    size_t descriptor_count = 0U;
    void *hardware_buffers[BVB_WSI_FRAME_RING_MAX_SLOTS] = {0};
    size_t hardware_buffer_count = 0U;
    const int prepare_result =
        bvb_vulkan_global_context_prepare_swapchain(
            context, &request, &response, descriptors, &descriptor_count,
            hardware_buffers, &hardware_buffer_count, error, sizeof(error));
    const char *expected_missing =
        getenv("BVB_EXPECT_MISSING_SWAPCHAIN_ENTRY_POINT");
    if (expected_missing != NULL) {
        CHECK(prepare_result == -ENOSYS);
        CHECK(strstr(error, expected_missing) != NULL);
        CHECK(descriptor_count == 0U);
        bvb_vulkan_global_context_destroy(context);
        printf("PASS: exact missing swapchain entry point: %s\n",
               expected_missing);
        return 0;
    }
    CHECK(prepare_result == 0);
    CHECK(response.vulkan_result == VK_SUCCESS);
    CHECK(response.flags == 0U && hardware_buffer_count == 0U);
    CHECK(response.image_count == 3U && descriptor_count == 4U);
    CHECK(bvb_handle_type(response.swapchain_id) == BVB_OBJECT_SWAPCHAIN);
    CHECK(response.generation == request.generation);
    CHECK(response.control_region_bytes == BVB_WSI_FRAME_RING_REGION_BYTES);
    for (uint32_t index = 0U; index < response.image_count; ++index) {
        CHECK(bvb_handle_type(response.images[index].image_id) ==
              BVB_OBJECT_IMAGE);
        CHECK(response.images[index].allocation_size == 16384U);
        struct stat status;
        CHECK(fstat(descriptors[index], &status) == 0);
        CHECK((uint64_t)status.st_size ==
              response.images[index].allocation_size);
    }
    struct stat control_status;
    CHECK(fstat(descriptors[response.image_count], &control_status) == 0);
    CHECK(control_status.st_size == BVB_WSI_FRAME_RING_REGION_BYTES);
    struct bvb_wsi_frame_ring *ring = mmap(
        NULL, BVB_WSI_FRAME_RING_REGION_BYTES, PROT_READ | PROT_WRITE,
        MAP_SHARED, descriptors[response.image_count], 0U);
    CHECK(ring != MAP_FAILED);
    CHECK(bvb_wsi_frame_ring_validate(ring, request.generation) == 0);
    const struct bvb_vulkan_swapchain_acquire_request acquire_request = {
        .device_id = device.device_id,
        .swapchain_id = response.swapchain_id,
        .timeout_ns = UINT64_MAX,
        .semaphore_id = semaphore.object_id,
    };
    struct bvb_vulkan_swapchain_acquire_response acquired = {0};
    CHECK(bvb_vulkan_global_context_acquire_swapchain_image(
              context, &acquire_request, &acquired,
              error, sizeof(error)) == 0);
    CHECK(acquired.vulkan_result == VK_SUCCESS);
    const struct bvb_vulkan_swapchain_present_request present_request = {
        .queue_id = queue_id,
        .swapchain_id = response.swapchain_id,
        .image_index = acquired.image_index,
        .wait_semaphore_count = 1U,
        .wait_semaphore_ids = {semaphore.object_id},
    };
    struct bvb_vulkan_swapchain_present_response presented = {0};
    CHECK(bvb_vulkan_global_context_present_swapchain_image(
              context, &present_request, &presented,
              error, sizeof(error)) == 0);
    CHECK(presented.vulkan_result == VK_SUCCESS);
    uint32_t activity_slot = UINT32_MAX;
    uint32_t activity_sequence = 0U;
    CHECK(bvb_wsi_frame_ring_wait_present(
              ring, 0U, 100U, &activity_slot, &activity_sequence) == 0);
    CHECK(activity_slot == acquired.image_index &&
          activity_sequence == presented.sequence);
    CHECK(bvb_wsi_frame_ring_release(
              ring, activity_slot, activity_sequence) == 0);
    CHECK(munmap(ring, BVB_WSI_FRAME_RING_REGION_BYTES) == 0);
    for (size_t index = 0U; index < descriptor_count; ++index) {
        CHECK(close(descriptors[index]) == 0);
    }

    CHECK(bvb_vulkan_global_context_destroy_swapchain(
              context, response.swapchain_id, error, sizeof(error)) == 0);
    CHECK(bvb_vulkan_global_context_destroy_swapchain(
              context, response.swapchain_id, error, sizeof(error)) ==
          -ENOENT);
    CHECK(bvb_vulkan_global_context_destroy_semaphore(
              context, semaphore.object_id, error, sizeof(error)) == 0);
    CHECK(bvb_vulkan_global_context_destroy_device(
              context, device.device_id) == 0);
    CHECK(bvb_vulkan_global_context_destroy_instance(
              context, instance.instance_id) == 0);
    bvb_vulkan_global_context_destroy(context);
    puts("PASS: real exportable image ring preparation and teardown");
    return 0;
}
