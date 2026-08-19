#define VK_NO_PROTOTYPES

#include <bvb/command_batch.h>
#include <bvb/triangle_dispatch.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const uint64_t BVB_TRIANGLE_COMMAND_BUFFER_MAGIC =
    UINT64_C(0x42564254434d4442);

struct bvb_triangle_command_buffer {
    uint64_t magic;
    struct bvb_command_batch_builder builder;
    int status;
    bool finished;
};

static struct bvb_triangle_command_buffer *command_state(
    VkCommandBuffer command_buffer) {
    struct bvb_triangle_command_buffer *state = NULL;
    _Static_assert(sizeof(state) == sizeof(command_buffer),
                   "VkCommandBuffer is not pointer-sized");
    memcpy(&state, &command_buffer, sizeof(state));
    if (state == NULL || state->magic != BVB_TRIANGLE_COMMAND_BUFFER_MAGIC) {
        return NULL;
    }
    return state;
}

static uint64_t non_dispatchable_bits(const void *handle, size_t size) {
    uint64_t bits = 0U;
    if (handle != NULL && size <= sizeof(bits)) {
        memcpy(&bits, handle, size);
    }
    return bits;
}

static void record_status(struct bvb_triangle_command_buffer *state,
                          int status) {
    if (state != NULL && state->status == 0 && status != 0) {
        state->status = status;
    }
}

static void VKAPI_CALL bvb_bridge_vkCmdBeginRendering(
    VkCommandBuffer command_buffer, const VkRenderingInfo *rendering_info) {
    struct bvb_triangle_command_buffer *state = command_state(command_buffer);
    if (state == NULL || state->status != 0 || state->finished) {
        record_status(state, -EINVAL);
        return;
    }
    if (rendering_info == NULL ||
        rendering_info->sType != VK_STRUCTURE_TYPE_RENDERING_INFO ||
        rendering_info->pNext != NULL || rendering_info->flags != 0U ||
        rendering_info->renderArea.offset.x != 0 ||
        rendering_info->renderArea.offset.y != 0 ||
        rendering_info->renderArea.extent.width == 0U ||
        rendering_info->renderArea.extent.height == 0U ||
        rendering_info->layerCount == 0U || rendering_info->viewMask != 0U ||
        rendering_info->colorAttachmentCount != 1U ||
        rendering_info->pColorAttachments == NULL ||
        rendering_info->pDepthAttachment != NULL ||
        rendering_info->pStencilAttachment != NULL) {
        record_status(state, -ENOTSUP);
        return;
    }
    const VkRenderingAttachmentInfo *attachment =
        rendering_info->pColorAttachments;
    const uint64_t image_view =
        non_dispatchable_bits(&attachment->imageView,
                              sizeof(attachment->imageView));
    if (attachment->sType != VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO ||
        attachment->pNext != NULL ||
        bvb_handle_expect(image_view, BVB_OBJECT_IMAGE_VIEW) != 0 ||
        attachment->resolveMode != VK_RESOLVE_MODE_NONE ||
        attachment->resolveImageView != VK_NULL_HANDLE) {
        record_status(state, -ENOTSUP);
        return;
    }
    const struct bvb_begin_rendering_command command = {
        .color_image_view_id = image_view,
        .width = rendering_info->renderArea.extent.width,
        .height = rendering_info->renderArea.extent.height,
        .image_layout = (uint32_t)attachment->imageLayout,
        .load_op = (uint32_t)attachment->loadOp,
        .store_op = (uint32_t)attachment->storeOp,
        .layer_count = rendering_info->layerCount,
        .clear_color = {
            attachment->clearValue.color.float32[0],
            attachment->clearValue.color.float32[1],
            attachment->clearValue.color.float32[2],
            attachment->clearValue.color.float32[3],
        },
    };
    record_status(state, bvb_command_batch_append_begin_rendering(
                             &state->builder, &command));
}

static void VKAPI_CALL bvb_bridge_vkCmdBindPipeline(
    VkCommandBuffer command_buffer, VkPipelineBindPoint pipeline_bind_point,
    VkPipeline pipeline) {
    struct bvb_triangle_command_buffer *state = command_state(command_buffer);
    const uint64_t pipeline_id =
        non_dispatchable_bits(&pipeline, sizeof(pipeline));
    if (state == NULL || state->status != 0 || state->finished) {
        record_status(state, -EINVAL);
        return;
    }
    if (pipeline_bind_point != VK_PIPELINE_BIND_POINT_GRAPHICS ||
        bvb_handle_expect(pipeline_id, BVB_OBJECT_PIPELINE) != 0) {
        record_status(state, -ENOTSUP);
        return;
    }
    record_status(
        state, bvb_command_batch_append_bind_graphics_pipeline(
                   &state->builder,
                   &(const struct bvb_bind_graphics_pipeline_command){
                       .pipeline_id = pipeline_id,
                   }));
}

static void VKAPI_CALL bvb_bridge_vkCmdSetViewport(
    VkCommandBuffer command_buffer, uint32_t first_viewport,
    uint32_t viewport_count, const VkViewport *viewports) {
    struct bvb_triangle_command_buffer *state = command_state(command_buffer);
    if (state == NULL || state->status != 0 || state->finished) {
        record_status(state, -EINVAL);
        return;
    }
    if (first_viewport != 0U || viewport_count != 1U || viewports == NULL) {
        record_status(state, -ENOTSUP);
        return;
    }
    record_status(state, bvb_command_batch_append_set_viewport(
                             &state->builder,
                             &(const struct bvb_set_viewport_command){
                                 .x = viewports[0].x,
                                 .y = viewports[0].y,
                                 .width = viewports[0].width,
                                 .height = viewports[0].height,
                                 .minimum_depth = viewports[0].minDepth,
                                 .maximum_depth = viewports[0].maxDepth,
                             }));
}

static void VKAPI_CALL bvb_bridge_vkCmdSetScissor(
    VkCommandBuffer command_buffer, uint32_t first_scissor,
    uint32_t scissor_count, const VkRect2D *scissors) {
    struct bvb_triangle_command_buffer *state = command_state(command_buffer);
    if (state == NULL || state->status != 0 || state->finished) {
        record_status(state, -EINVAL);
        return;
    }
    if (first_scissor != 0U || scissor_count != 1U || scissors == NULL) {
        record_status(state, -ENOTSUP);
        return;
    }
    record_status(state, bvb_command_batch_append_set_scissor(
                             &state->builder,
                             &(const struct bvb_set_scissor_command){
                                 .x = scissors[0].offset.x,
                                 .y = scissors[0].offset.y,
                                 .width = scissors[0].extent.width,
                                 .height = scissors[0].extent.height,
                             }));
}

static void VKAPI_CALL bvb_bridge_vkCmdDraw(
    VkCommandBuffer command_buffer, uint32_t vertex_count,
    uint32_t instance_count, uint32_t first_vertex,
    uint32_t first_instance) {
    struct bvb_triangle_command_buffer *state = command_state(command_buffer);
    if (state == NULL || state->status != 0 || state->finished) {
        record_status(state, -EINVAL);
        return;
    }
    record_status(state, bvb_command_batch_append_draw(
                             &state->builder,
                             &(const struct bvb_draw_command){
                                 .vertex_count = vertex_count,
                                 .instance_count = instance_count,
                                 .first_vertex = first_vertex,
                                 .first_instance = first_instance,
                             }));
}

static void VKAPI_CALL bvb_bridge_vkCmdEndRendering(
    VkCommandBuffer command_buffer) {
    struct bvb_triangle_command_buffer *state = command_state(command_buffer);
    if (state == NULL || state->status != 0 || state->finished) {
        record_status(state, -EINVAL);
        return;
    }
    record_status(state,
                  bvb_command_batch_append_end_rendering(&state->builder));
}

BVB_TRIANGLE_EXPORT VkCommandBuffer bvb_triangle_command_buffer_create(
    uint8_t *batch, size_t capacity, uint64_t command_buffer_id,
    uint64_t sequence) {
    struct bvb_triangle_command_buffer *state = calloc(1, sizeof(*state));
    if (state == NULL) {
        return VK_NULL_HANDLE;
    }
    state->magic = BVB_TRIANGLE_COMMAND_BUFFER_MAGIC;
    state->status = bvb_command_batch_begin(&state->builder, batch, capacity,
                                            command_buffer_id, sequence);
    if (state->status != 0) {
        free(state);
        return VK_NULL_HANDLE;
    }
    VkCommandBuffer result = VK_NULL_HANDLE;
    memcpy(&result, &state, sizeof(result));
    return result;
}

BVB_TRIANGLE_EXPORT int bvb_triangle_command_buffer_finish(
    VkCommandBuffer command_buffer, size_t *batch_length) {
    struct bvb_triangle_command_buffer *state = command_state(command_buffer);
    if (state == NULL || batch_length == NULL || state->finished) {
        return -EINVAL;
    }
    if (state->status == 0) {
        state->status = bvb_command_batch_finish(&state->builder, batch_length);
    }
    state->finished = true;
    return state->status;
}

BVB_TRIANGLE_EXPORT int bvb_triangle_command_buffer_status(
    VkCommandBuffer command_buffer) {
    struct bvb_triangle_command_buffer *state = command_state(command_buffer);
    return state == NULL ? -EINVAL : state->status;
}

BVB_TRIANGLE_EXPORT void bvb_triangle_command_buffer_destroy(
    VkCommandBuffer command_buffer) {
    struct bvb_triangle_command_buffer *state = command_state(command_buffer);
    if (state != NULL) {
        state->magic = 0U;
        free(state);
    }
}

#define BVB_TRIANGLE_DISPATCH_ENTRY(name, wrapper, type)                       \
    _Static_assert(_Generic(&(wrapper), type: 1, default: 0),                  \
                   "generated Vulkan signature mismatch: " #name);
#include "bvb_triangle_dispatch.inc"
#undef BVB_TRIANGLE_DISPATCH_ENTRY

BVB_TRIANGLE_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *name) {
    (void)device;
    if (name == NULL) {
        return NULL;
    }
#define BVB_TRIANGLE_DISPATCH_ENTRY(entry_name, wrapper, type)                 \
    if (strcmp(name, #entry_name) == 0) {                                      \
        type typed = &(wrapper);                                               \
        PFN_vkVoidFunction erased = NULL;                                      \
        _Static_assert(sizeof(typed) == sizeof(erased),                        \
                       "Vulkan function pointer size mismatch");              \
        memcpy(&erased, &typed, sizeof(erased));                               \
        return erased;                                                         \
    }
#include "bvb_triangle_dispatch.inc"
#undef BVB_TRIANGLE_DISPATCH_ENTRY
    return NULL;
}
