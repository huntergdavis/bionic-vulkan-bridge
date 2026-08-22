#define VK_NO_PROTOTYPES

#include <bvb/command_batch.h>
#include <bvb/dxvk_dispatch_policy.h>
#include <bvb/triangle_dispatch.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expression)                                                        \
    do {                                                                         \
        if (!(expression)) {                                                     \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #expression);                                                \
            return 1;                                                            \
        }                                                                        \
    } while (0)

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *name);

static VkImageView image_view_from_id(uint64_t wire_id) {
    VkImageView handle = VK_NULL_HANDLE;
    memcpy(&handle, &wire_id, sizeof(handle));
    return handle;
}

static VkPipeline pipeline_from_id(uint64_t wire_id) {
    VkPipeline handle = VK_NULL_HANDLE;
    memcpy(&handle, &wire_id, sizeof(handle));
    return handle;
}

static VkPipelineLayout pipeline_layout_from_id(uint64_t wire_id) {
    VkPipelineLayout handle = VK_NULL_HANDLE;
    memcpy(&handle, &wire_id, sizeof(handle));
    return handle;
}

#define RESOLVE(name)                                                           \
    PFN_##name name = NULL;                                                     \
    do {                                                                        \
        PFN_vkVoidFunction generic = vkGetDeviceProcAddr(                       \
            VK_NULL_HANDLE, #name);                                             \
        CHECK(generic != NULL);                                                 \
        _Static_assert(sizeof(name) == sizeof(generic),                         \
                       "Vulkan function pointer size mismatch");               \
        memcpy(&name, &generic, sizeof(name));                                  \
    } while (0)

int main(void) {
    CHECK(bvb_dxvk_dispatch_policy_count() == 742U);
    size_t executable_count = 0U;
    size_t required_count = 0U;
    size_t probed_null_count = 0U;
    const char *previous_name = NULL;
    for (size_t index = 0U; index < bvb_dxvk_dispatch_policy_count();
         ++index) {
        const struct bvb_dxvk_dispatch_policy_entry *entry =
            bvb_dxvk_dispatch_policy_at(index);
        CHECK(entry != NULL);
        CHECK(previous_name == NULL || strcmp(previous_name, entry->name) < 0);
        CHECK(bvb_dxvk_dispatch_policy_lookup(entry->name) == entry);
        PFN_vkVoidFunction resolved =
            entry->scope == BVB_DXVK_SCOPE_GLOBAL
                ? vkGetInstanceProcAddr(VK_NULL_HANDLE, entry->name)
                : vkGetDeviceProcAddr(VK_NULL_HANDLE, entry->name);
        if (entry->support == BVB_DXVK_SUPPORT_EXECUTABLE) {
            ++executable_count;
            if (entry->scope == BVB_DXVK_SCOPE_INSTANCE) {
                CHECK(resolved == NULL);
                CHECK(strcmp(entry->name, "vkDestroyInstance") == 0 ||
                      strcmp(entry->name,
                             "vkEnumeratePhysicalDevices") == 0 ||
                      strcmp(entry->name,
                             "vkEnumerateDeviceExtensionProperties") == 0 ||
                      strcmp(entry->name,
                             "vkGetPhysicalDeviceMemoryProperties") == 0 ||
                      strcmp(entry->name,
                             "vkGetPhysicalDeviceProperties") == 0 ||
                      strcmp(entry->name,
                             "vkGetPhysicalDeviceQueueFamilyProperties") == 0 ||
                      strcmp(entry->name,
                             "vkGetPhysicalDeviceExternalBufferProperties") == 0 ||
                      strcmp(entry->name,
                             "vkGetPhysicalDeviceExternalBufferPropertiesKHR") == 0 ||
                      strcmp(entry->name,
                             "vkGetPhysicalDeviceExternalSemaphoreProperties") == 0 ||
                      strcmp(entry->name,
                             "vkGetPhysicalDeviceExternalSemaphorePropertiesKHR") == 0 ||
                      strcmp(entry->name, "vkCreateDevice") == 0 ||
                      strcmp(entry->name,
                             "vkGetPhysicalDeviceFeatures") == 0);
            } else if (entry->scope == BVB_DXVK_SCOPE_DEVICE &&
                       (strcmp(entry->name, "vkDestroyDevice") == 0 ||
                        strcmp(entry->name, "vkGetDeviceQueue") == 0 ||
                        strcmp(entry->name, "vkQueueSubmit") == 0 ||
                        strcmp(entry->name, "vkQueueSubmit2") == 0 ||
                        strcmp(entry->name, "vkQueueSubmit2KHR") == 0 ||
                        strcmp(entry->name, "vkQueueWaitIdle") == 0 ||
                        strcmp(entry->name, "vkDeviceWaitIdle") == 0 ||
                        strcmp(entry->name, "vkCreateCommandPool") == 0 ||
                        strcmp(entry->name, "vkDestroyCommandPool") == 0 ||
                        strcmp(entry->name, "vkResetCommandPool") == 0 ||
                        strcmp(entry->name,
                               "vkAllocateCommandBuffers") == 0 ||
                        strcmp(entry->name, "vkFreeCommandBuffers") == 0 ||
                        strcmp(entry->name, "vkBeginCommandBuffer") == 0 ||
                        strcmp(entry->name, "vkEndCommandBuffer") == 0 ||
                        strcmp(entry->name, "vkCreateBuffer") == 0 ||
                        strcmp(entry->name, "vkDestroyBuffer") == 0 ||
                        strcmp(entry->name,
                               "vkGetBufferMemoryRequirements") == 0 ||
                        strcmp(entry->name,
                               "vkGetBufferMemoryRequirements2") == 0 ||
                        strcmp(entry->name,
                               "vkGetBufferDeviceAddress") == 0 ||
                        strcmp(entry->name,
                               "vkGetDeviceBufferMemoryRequirements") == 0 ||
                        strcmp(entry->name, "vkAllocateMemory") == 0 ||
                        strcmp(entry->name, "vkFreeMemory") == 0 ||
                        strcmp(entry->name, "vkMapMemory") == 0 ||
                        strcmp(entry->name, "vkMapMemory2") == 0 ||
                        strcmp(entry->name, "vkMapMemory2KHR") == 0 ||
                        strcmp(entry->name, "vkUnmapMemory") == 0 ||
                        strcmp(entry->name,
                               "vkFlushMappedMemoryRanges") == 0 ||
                        strcmp(entry->name,
                               "vkInvalidateMappedMemoryRanges") == 0 ||
                        strcmp(entry->name, "vkBindBufferMemory") == 0 ||
                        strcmp(entry->name, "vkCmdFillBuffer") == 0 ||
                        strcmp(entry->name, "vkCreateFence") == 0 ||
                        strcmp(entry->name, "vkDestroyFence") == 0 ||
                        strcmp(entry->name, "vkGetFenceStatus") == 0 ||
                        strcmp(entry->name, "vkWaitForFences") == 0 ||
                        strcmp(entry->name, "vkResetFences") == 0 ||
                        strcmp(entry->name, "vkCreateSemaphore") == 0 ||
                        strcmp(entry->name, "vkDestroySemaphore") == 0 ||
                        strcmp(entry->name,
                               "vkGetSemaphoreCounterValue") == 0 ||
                        strcmp(entry->name,
                               "vkGetSemaphoreCounterValueKHR") == 0 ||
                        strcmp(entry->name, "vkWaitSemaphores") == 0 ||
                        strcmp(entry->name, "vkWaitSemaphoresKHR") == 0 ||
                        strcmp(entry->name, "vkSignalSemaphore") == 0 ||
                        strcmp(entry->name, "vkSignalSemaphoreKHR") == 0 ||
                        strcmp(entry->name,
                               "vkAllocateDescriptorSets") == 0 ||
                        strcmp(entry->name,
                               "vkCreateDescriptorPool") == 0 ||
                        strcmp(entry->name,
                               "vkCreateDescriptorSetLayout") == 0 ||
                        strcmp(entry->name,
                               "vkCreateDescriptorUpdateTemplate") == 0 ||
                        strcmp(entry->name, "vkCreateSampler") == 0 ||
                        strcmp(entry->name,
                               "vkDestroyDescriptorPool") == 0 ||
                        strcmp(entry->name,
                               "vkDestroyDescriptorSetLayout") == 0 ||
                        strcmp(entry->name,
                               "vkDestroyDescriptorUpdateTemplate") == 0 ||
                        strcmp(entry->name, "vkDestroySampler") == 0 ||
                        strcmp(entry->name,
                               "vkUpdateDescriptorSets") == 0 ||
                        strcmp(entry->name, "vkCreatePipelineLayout") == 0 ||
                        strcmp(entry->name,
                               "vkDestroyPipelineLayout") == 0 ||
                        strcmp(entry->name, "vkCreateImage") == 0 ||
                        strcmp(entry->name, "vkDestroyImage") == 0 ||
                        strcmp(entry->name,
                               "vkGetImageMemoryRequirements") == 0 ||
                        strcmp(entry->name,
                               "vkGetImageMemoryRequirements2") == 0 ||
                        strcmp(entry->name, "vkBindImageMemory") == 0 ||
                        strcmp(entry->name, "vkCreateImageView") == 0 ||
                        strcmp(entry->name, "vkDestroyImageView") == 0 ||
                        strcmp(entry->name,
                               "vkCreateGraphicsPipelines") == 0 ||
                        strcmp(entry->name, "vkDestroyPipeline") == 0 ||
                        strcmp(entry->name,
                               "vkCmdPipelineBarrier2") == 0 ||
                        strcmp(entry->name,
                               "vkCmdClearColorImage") == 0)) {
                CHECK(resolved == NULL);
            } else {
                CHECK(resolved != NULL);
            }
        } else {
            CHECK(resolved == NULL);
            if (entry->support ==
                BVB_DXVK_SUPPORT_REQUIRED_UNIMPLEMENTED) {
                ++required_count;
            } else {
                CHECK(entry->support == BVB_DXVK_SUPPORT_PROBED_NULL);
                ++probed_null_count;
            }
        }
        previous_name = entry->name;
    }
    CHECK(bvb_dxvk_dispatch_policy_at(742U) == NULL);
    CHECK(bvb_dxvk_dispatch_policy_lookup(NULL) == NULL);
    CHECK(bvb_dxvk_dispatch_policy_lookup("vkNotARealCommand") == NULL);
    CHECK(executable_count == 92U);
    CHECK(required_count == 348U);
    CHECK(probed_null_count == 302U);
    const struct bvb_dxvk_dispatch_policy_entry *create_instance =
        bvb_dxvk_dispatch_policy_lookup("vkCreateInstance");
    CHECK(create_instance != NULL);
    CHECK(create_instance->scope == BVB_DXVK_SCOPE_GLOBAL);
    CHECK(create_instance->lookup_count == 5U);
    CHECK(create_instance->support == BVB_DXVK_SUPPORT_EXECUTABLE);
    const struct bvb_dxvk_dispatch_policy_entry *private_entry =
        bvb_dxvk_dispatch_policy_lookup("wine_vkAcquireKeyedMutex");
    CHECK(private_entry != NULL);
    CHECK(private_entry->scope == BVB_DXVK_SCOPE_PRIVATE);
    CHECK(private_entry->support == BVB_DXVK_SUPPORT_PROBED_NULL);

    RESOLVE(vkCmdBeginRendering);
    RESOLVE(vkCmdBindPipeline);
    RESOLVE(vkCmdPushConstants);
    RESOLVE(vkCmdSetViewport);
    RESOLVE(vkCmdSetScissor);
    RESOLVE(vkCmdDraw);
    RESOLVE(vkCmdEndRendering);

    CHECK(vkGetDeviceProcAddr(VK_NULL_HANDLE, "vkCmdBeginRenderingKHR") ==
          vkGetDeviceProcAddr(VK_NULL_HANDLE, "vkCmdBeginRendering"));
    CHECK(vkGetDeviceProcAddr(VK_NULL_HANDLE, "vkCmdEndRenderingKHR") ==
          vkGetDeviceProcAddr(VK_NULL_HANDLE, "vkCmdEndRendering"));
    CHECK(vkGetDeviceProcAddr(VK_NULL_HANDLE, "vkCmdDispatch") == NULL);

    uint8_t batch[512];
    const uint64_t command_buffer_id =
        bvb_handle_id(BVB_OBJECT_COMMAND_BUFFER, 3U);
    const uint64_t image_view_id =
        bvb_handle_id(BVB_OBJECT_IMAGE_VIEW, 4U);
    const uint64_t pipeline_id = bvb_handle_id(BVB_OBJECT_PIPELINE, 5U);
    const uint64_t pipeline_layout_id =
        bvb_handle_id(BVB_OBJECT_PIPELINE_LAYOUT, 6U);
    VkCommandBuffer command_buffer = bvb_triangle_command_buffer_create(
        batch, sizeof(batch), command_buffer_id, 11U);
    CHECK(command_buffer != VK_NULL_HANDLE);

    const VkRenderingAttachmentInfo attachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = image_view_from_id(image_view_id),
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue.color.float32 = {0.1F, 0.2F, 0.3F, 1.0F},
    };
    const VkRenderingInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {.offset = {0, 0}, .extent = {1280U, 720U}},
        .layerCount = 1U,
        .colorAttachmentCount = 1U,
        .pColorAttachments = &attachment,
    };
    vkCmdBeginRendering(command_buffer, &rendering);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      pipeline_from_id(pipeline_id));
    const float rotation[] = {1.25F, 16.0F / 9.0F};
    vkCmdPushConstants(command_buffer,
                       pipeline_layout_from_id(pipeline_layout_id),
                       VK_SHADER_STAGE_VERTEX_BIT, 0U, sizeof(rotation),
                       rotation);
    const VkViewport viewport = {
        .x = 0.0F,
        .y = 0.0F,
        .width = 1280.0F,
        .height = 720.0F,
        .minDepth = 0.0F,
        .maxDepth = 1.0F,
    };
    vkCmdSetViewport(command_buffer, 0U, 1U, &viewport);
    const VkRect2D scissor = {
        .offset = {0, 0},
        .extent = {1280U, 720U},
    };
    vkCmdSetScissor(command_buffer, 0U, 1U, &scissor);
    vkCmdDraw(command_buffer, 3U, 1U, 0U, 0U);
    vkCmdEndRendering(command_buffer);

    size_t length = 0U;
    CHECK(bvb_triangle_command_buffer_finish(command_buffer, &length) == 0);
    CHECK(bvb_triangle_command_buffer_status(command_buffer) == 0);
    struct bvb_command_batch_info info;
    CHECK(bvb_command_batch_validate(batch, length, &info) == 0);
    CHECK(info.command_buffer_id == command_buffer_id);
    CHECK(info.sequence == 11U);
    CHECK(info.command_count == 7U);
    CHECK(info.byte_length == 224U);

    struct bvb_command_batch_iterator iterator;
    struct bvb_command_record record;
    CHECK(bvb_command_batch_iterator_init(&iterator, batch, length) == 0);
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    struct bvb_begin_rendering_command begin;
    CHECK(bvb_command_decode_begin_rendering(&record, &begin) == 0);
    CHECK(begin.color_image_view_id == image_view_id);
    CHECK(begin.width == 1280U && begin.height == 720U);
    CHECK(begin.clear_color[0] == 0.1F && begin.clear_color[3] == 1.0F);
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    struct bvb_bind_graphics_pipeline_command bind;
    CHECK(bvb_command_decode_bind_graphics_pipeline(&record, &bind) == 0);
    CHECK(bind.pipeline_id == pipeline_id);
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    struct bvb_push_rotation_command pushed_rotation;
    CHECK(bvb_command_decode_push_rotation(&record, &pushed_rotation) == 0);
    CHECK(pushed_rotation.pipeline_layout_id == pipeline_layout_id);
    CHECK(pushed_rotation.angle_radians == rotation[0]);
    CHECK(pushed_rotation.aspect_ratio == rotation[1]);
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    CHECK(record.opcode == BVB_COMMAND_SET_VIEWPORT);
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    CHECK(record.opcode == BVB_COMMAND_SET_SCISSOR);
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    struct bvb_draw_command draw;
    CHECK(bvb_command_decode_draw(&record, &draw) == 0);
    CHECK(draw.vertex_count == 3U && draw.instance_count == 1U);
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    CHECK(record.opcode == BVB_COMMAND_END_RENDERING);
    CHECK(bvb_command_batch_next(&iterator, &record) == 1);
    bvb_triangle_command_buffer_destroy(command_buffer);

    VkCommandBuffer rejected = bvb_triangle_command_buffer_create(
        batch, sizeof(batch), command_buffer_id, 12U);
    CHECK(rejected != VK_NULL_HANDLE);
    vkCmdSetViewport(rejected, 0U, 2U, &viewport);
    CHECK(bvb_triangle_command_buffer_status(rejected) == -ENOTSUP);
    CHECK(bvb_triangle_command_buffer_finish(rejected, &length) == -ENOTSUP);
    bvb_triangle_command_buffer_destroy(rejected);

    puts("PASS: generated executable triangle dispatch");
    return 0;
}
