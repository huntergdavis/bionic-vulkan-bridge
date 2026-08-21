#define VK_NO_PROTOTYPES

#include <bvb/command_batch.h>
#include <bvb/dxvk_dispatch_policy.h>
#include <bvb/first_rejection.h>
#include <bvb/global_dispatch.h>
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
    const char *rejection_entry;
    const char *rejection_reason;
    const char *rejection_shape;
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
                          VkCommandBuffer command_buffer, int status,
                          const char *entry, const char *reason,
                          const char *shape) {
    if (state == NULL && status != 0) {
        bvb_global_diagnostic_poison_command(
            command_buffer, entry, reason, shape, status);
        return;
    }
    if (state != NULL && state->status == 0 && status != 0) {
        state->status = status;
        state->rejection_entry = entry;
        state->rejection_reason = reason;
        state->rejection_shape = shape;
    }
}

static void VKAPI_CALL bvb_bridge_vkCmdBeginRendering(
    VkCommandBuffer command_buffer, const VkRenderingInfo *rendering_info) {
    struct bvb_triangle_command_buffer *state = command_state(command_buffer);
    if (state == NULL || state->status != 0 || state->finished) {
        record_status(state, command_buffer, -EINVAL, "vkCmdBeginRendering",
                      "invalid_command_state", "VkRenderingInfo_ptr");
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
        record_status(state, command_buffer, -ENOTSUP, "vkCmdBeginRendering",
                      "unsupported_rendering_info", "VkRenderingInfo_ptr");
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
        record_status(state, command_buffer, -ENOTSUP, "vkCmdBeginRendering",
                      "unsupported_color_attachment",
                      "VkRenderingAttachmentInfo_ptr");
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
    record_status(state, command_buffer,
                  bvb_command_batch_append_begin_rendering(
                      &state->builder, &command),
                  "vkCmdBeginRendering", "command_batch_append_failed",
                  "VkRenderingInfo_ptr");
}

static void VKAPI_CALL bvb_bridge_vkCmdBindPipeline(
    VkCommandBuffer command_buffer, VkPipelineBindPoint pipeline_bind_point,
    VkPipeline pipeline) {
    struct bvb_triangle_command_buffer *state = command_state(command_buffer);
    const uint64_t pipeline_id =
        non_dispatchable_bits(&pipeline, sizeof(pipeline));
    if (state == NULL || state->status != 0 || state->finished) {
        record_status(state, command_buffer, -EINVAL, "vkCmdBindPipeline",
                      "invalid_command_state",
                      "VkPipelineBindPoint_value,VkPipeline_value");
        return;
    }
    if (pipeline_bind_point != VK_PIPELINE_BIND_POINT_GRAPHICS ||
        bvb_handle_expect(pipeline_id, BVB_OBJECT_PIPELINE) != 0) {
        record_status(state, command_buffer, -ENOTSUP, "vkCmdBindPipeline",
                      "unsupported_pipeline_binding",
                      "VkPipelineBindPoint_value,VkPipeline_value");
        return;
    }
    record_status(
        state, command_buffer,
        bvb_command_batch_append_bind_graphics_pipeline(
                   &state->builder,
                   &(const struct bvb_bind_graphics_pipeline_command){
                       .pipeline_id = pipeline_id,
                   }),
        "vkCmdBindPipeline", "command_batch_append_failed",
        "VkPipelineBindPoint_value,VkPipeline_value");
}

static void VKAPI_CALL bvb_bridge_vkCmdPushConstants(
    VkCommandBuffer command_buffer, VkPipelineLayout layout,
    VkShaderStageFlags stage_flags, uint32_t offset, uint32_t size,
    const void *values) {
    struct bvb_triangle_command_buffer *state = command_state(command_buffer);
    const uint64_t pipeline_layout_id =
        non_dispatchable_bits(&layout, sizeof(layout));
    if (state == NULL || state->status != 0 || state->finished) {
        record_status(state, command_buffer, -EINVAL, "vkCmdPushConstants",
                      "invalid_command_state",
                      "VkPipelineLayout_value,VkShaderStageFlags_value,uint32_t_value,uint32_t_value,void_ptr");
        return;
    }
    if (bvb_handle_expect(pipeline_layout_id,
                          BVB_OBJECT_PIPELINE_LAYOUT) != 0 ||
        stage_flags != VK_SHADER_STAGE_VERTEX_BIT || offset != 0U ||
        size != 2U * sizeof(float) || values == NULL) {
        record_status(state, command_buffer, -ENOTSUP, "vkCmdPushConstants",
                      "unsupported_push_constant_shape",
                      "VkPipelineLayout_value,VkShaderStageFlags_value,uint32_t_value,uint32_t_value,void_ptr");
        return;
    }
    float constants[2];
    memcpy(constants, values, sizeof(constants));
    record_status(
        state, command_buffer,
        bvb_command_batch_append_push_rotation(
                   &state->builder,
                   &(const struct bvb_push_rotation_command){
                       .pipeline_layout_id = pipeline_layout_id,
                       .angle_radians = constants[0],
                       .aspect_ratio = constants[1],
                   }),
        "vkCmdPushConstants", "command_batch_append_failed",
        "VkPipelineLayout_value,VkShaderStageFlags_value,uint32_t_value,uint32_t_value,void_ptr");
}

static void VKAPI_CALL bvb_bridge_vkCmdSetViewport(
    VkCommandBuffer command_buffer, uint32_t first_viewport,
    uint32_t viewport_count, const VkViewport *viewports) {
    struct bvb_triangle_command_buffer *state = command_state(command_buffer);
    if (state == NULL || state->status != 0 || state->finished) {
        record_status(state, command_buffer, -EINVAL, "vkCmdSetViewport",
                      "invalid_command_state",
                      "uint32_t_value,uint32_t_value,VkViewport_ptr");
        return;
    }
    if (first_viewport != 0U || viewport_count != 1U || viewports == NULL) {
        record_status(state, command_buffer, -ENOTSUP, "vkCmdSetViewport",
                      "unsupported_viewport_shape",
                      "uint32_t_value,uint32_t_value,VkViewport_ptr");
        return;
    }
    record_status(
        state, command_buffer,
        bvb_command_batch_append_set_viewport(
            &state->builder,
            &(const struct bvb_set_viewport_command){
                .x = viewports[0].x,
                .y = viewports[0].y,
                .width = viewports[0].width,
                .height = viewports[0].height,
                .minimum_depth = viewports[0].minDepth,
                .maximum_depth = viewports[0].maxDepth,
            }),
        "vkCmdSetViewport", "command_batch_append_failed",
        "uint32_t_value,uint32_t_value,VkViewport_ptr");
}

static void VKAPI_CALL bvb_bridge_vkCmdSetScissor(
    VkCommandBuffer command_buffer, uint32_t first_scissor,
    uint32_t scissor_count, const VkRect2D *scissors) {
    struct bvb_triangle_command_buffer *state = command_state(command_buffer);
    if (state == NULL || state->status != 0 || state->finished) {
        record_status(state, command_buffer, -EINVAL, "vkCmdSetScissor",
                      "invalid_command_state",
                      "uint32_t_value,uint32_t_value,VkRect2D_ptr");
        return;
    }
    if (first_scissor != 0U || scissor_count != 1U || scissors == NULL) {
        record_status(state, command_buffer, -ENOTSUP, "vkCmdSetScissor",
                      "unsupported_scissor_shape",
                      "uint32_t_value,uint32_t_value,VkRect2D_ptr");
        return;
    }
    record_status(
        state, command_buffer,
        bvb_command_batch_append_set_scissor(
            &state->builder,
            &(const struct bvb_set_scissor_command){
                .x = scissors[0].offset.x,
                .y = scissors[0].offset.y,
                .width = scissors[0].extent.width,
                .height = scissors[0].extent.height,
            }),
        "vkCmdSetScissor", "command_batch_append_failed",
        "uint32_t_value,uint32_t_value,VkRect2D_ptr");
}

static void VKAPI_CALL bvb_bridge_vkCmdDraw(
    VkCommandBuffer command_buffer, uint32_t vertex_count,
    uint32_t instance_count, uint32_t first_vertex,
    uint32_t first_instance) {
    struct bvb_triangle_command_buffer *state = command_state(command_buffer);
    if (state == NULL || state->status != 0 || state->finished) {
        record_status(state, command_buffer, -EINVAL, "vkCmdDraw",
                      "invalid_command_state",
                      "uint32_t_value,uint32_t_value,uint32_t_value,uint32_t_value");
        return;
    }
    record_status(
        state, command_buffer,
        bvb_command_batch_append_draw(
            &state->builder,
            &(const struct bvb_draw_command){
                .vertex_count = vertex_count,
                .instance_count = instance_count,
                .first_vertex = first_vertex,
                .first_instance = first_instance,
            }),
        "vkCmdDraw", "command_batch_append_failed",
        "uint32_t_value,uint32_t_value,uint32_t_value,uint32_t_value");
}

static void VKAPI_CALL bvb_bridge_vkCmdEndRendering(
    VkCommandBuffer command_buffer) {
    struct bvb_triangle_command_buffer *state = command_state(command_buffer);
    if (state == NULL || state->status != 0 || state->finished) {
        record_status(state, command_buffer, -EINVAL, "vkCmdEndRendering",
                      "invalid_command_state", "none");
        return;
    }
    record_status(state, command_buffer,
                  bvb_command_batch_append_end_rendering(&state->builder),
                  "vkCmdEndRendering", "command_batch_append_failed",
                  "none");
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
    if (state->status != 0 && state->rejection_entry != NULL) {
        bvb_first_rejection_record_command_poison(
            state->rejection_entry, state->rejection_reason,
            state->rejection_shape, state->status,
            state->builder.command_buffer_id, state->builder.sequence);
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
    if (name == NULL) {
        return NULL;
    }
    if (device != VK_NULL_HANDLE) {
        if (bvb_device_proxy_id(device) == 0U) {
            return NULL;
        }
        PFN_vkVoidFunction global = bvb_global_device_proc_addr(device, name);
        if (global != NULL) {
            return global;
        }
    }
    if (strcmp(name, "vkGetDeviceProcAddr") == 0) {
        PFN_vkGetDeviceProcAddr typed = vkGetDeviceProcAddr;
        PFN_vkVoidFunction erased = NULL;
        memcpy(&erased, &typed, sizeof(erased));
        return bvb_first_rejection_wrap(
            name, BVB_DXVK_SCOPE_DEVICE, erased);
    }
#define BVB_TRIANGLE_DISPATCH_ENTRY(entry_name, wrapper, type)                 \
    if (strcmp(name, #entry_name) == 0) {                                      \
        type typed = &(wrapper);                                               \
        PFN_vkVoidFunction erased = NULL;                                      \
        _Static_assert(sizeof(typed) == sizeof(erased),                        \
                       "Vulkan function pointer size mismatch");              \
        memcpy(&erased, &typed, sizeof(erased));                               \
        return bvb_first_rejection_wrap(                                       \
            #entry_name, BVB_DXVK_SCOPE_DEVICE, erased);                       \
    }
#include "bvb_triangle_dispatch.inc"
#undef BVB_TRIANGLE_DISPATCH_ENTRY
    return bvb_first_rejection_required_stub(
        name, BVB_DXVK_SCOPE_DEVICE);
}
