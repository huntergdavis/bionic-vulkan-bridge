#define VK_NO_PROTOTYPES

#include <bvb/command_batch.h>
#include <bvb/triangle_batch_builder.h>
#include <bvb/triangle_dispatch.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *name);

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

#define RESOLVE_OR_FAIL(name)                                                  \
    PFN_##name name = NULL;                                                    \
    do {                                                                       \
        PFN_vkVoidFunction generic =                                           \
            vkGetDeviceProcAddr(VK_NULL_HANDLE, #name);                        \
        if (generic == NULL) {                                                  \
            return -ENOSYS;                                                     \
        }                                                                       \
        _Static_assert(sizeof(name) == sizeof(generic),                         \
                       "Vulkan function pointer size mismatch");              \
        memcpy(&name, &generic, sizeof(name));                                  \
    } while (0)

int bvb_triangle_batch_build_sequence(uint8_t *bytes, size_t capacity,
                                      uint32_t width, uint32_t height,
                                      uint64_t sequence,
                                      size_t *batch_length) {
    if (bytes == NULL || capacity == 0U || width == 0U || height == 0U ||
        sequence == 0U || batch_length == NULL) {
        return -EINVAL;
    }
    RESOLVE_OR_FAIL(vkCmdBeginRendering);
    RESOLVE_OR_FAIL(vkCmdBindPipeline);
    RESOLVE_OR_FAIL(vkCmdSetViewport);
    RESOLVE_OR_FAIL(vkCmdSetScissor);
    RESOLVE_OR_FAIL(vkCmdDraw);
    RESOLVE_OR_FAIL(vkCmdEndRendering);

    const uint64_t command_buffer_id =
        bvb_handle_id(BVB_OBJECT_COMMAND_BUFFER, 1U);
    const uint64_t image_view_id = bvb_handle_id(BVB_OBJECT_IMAGE_VIEW, 1U);
    const uint64_t pipeline_id = bvb_handle_id(BVB_OBJECT_PIPELINE, 1U);
    VkCommandBuffer command_buffer = bvb_triangle_command_buffer_create(
        bytes, capacity, command_buffer_id, sequence);
    if (command_buffer == VK_NULL_HANDLE) {
        return -ENOMEM;
    }

    const VkRenderingAttachmentInfo attachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = image_view_from_id(image_view_id),
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue.color.float32 = {0.25F, 0.02F, 0.02F, 1.0F},
    };
    const VkRenderingInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {.offset = {0, 0}, .extent = {width, height}},
        .layerCount = 1U,
        .colorAttachmentCount = 1U,
        .pColorAttachments = &attachment,
    };
    vkCmdBeginRendering(command_buffer, &rendering);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      pipeline_from_id(pipeline_id));
    const VkViewport viewport = {
        .x = 0.0F,
        .y = 0.0F,
        .width = (float)width,
        .height = (float)height,
        .minDepth = 0.0F,
        .maxDepth = 1.0F,
    };
    vkCmdSetViewport(command_buffer, 0U, 1U, &viewport);
    const VkRect2D scissor = {
        .offset = {0, 0},
        .extent = {width, height},
    };
    vkCmdSetScissor(command_buffer, 0U, 1U, &scissor);
    vkCmdDraw(command_buffer, 3U, 1U, 0U, 0U);
    vkCmdEndRendering(command_buffer);

    int result = bvb_triangle_command_buffer_status(command_buffer);
    if (result == 0) {
        result = bvb_triangle_command_buffer_finish(command_buffer,
                                                    batch_length);
    }
    bvb_triangle_command_buffer_destroy(command_buffer);
    if (result != 0) {
        return result;
    }
    struct bvb_command_batch_info info;
    result = bvb_command_batch_validate(bytes, *batch_length, &info);
    if (result != 0 || info.command_buffer_id != command_buffer_id ||
        info.sequence != sequence || info.command_count != 6U) {
        return result != 0 ? result : -EPROTO;
    }
    return 0;
}

int bvb_triangle_batch_build(uint8_t *bytes, size_t capacity,
                             uint32_t width, uint32_t height,
                             size_t *batch_length) {
    return bvb_triangle_batch_build_sequence(bytes, capacity, width, height,
                                             1U, batch_length);
}
