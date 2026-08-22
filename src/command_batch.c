#include <bvb/command_batch.h>
#include <bvb/protocol.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum {
    BVB_RENDERING_ATTACHMENT_SIZE = 56,
    BVB_BEGIN_RENDERING_HEADER_SIZE = 40,
    BVB_BEGIN_RENDERING_SIZE = BVB_BEGIN_RENDERING_HEADER_SIZE +
        (BVB_COMMAND_VULKAN_MAX_COLOR_ATTACHMENTS + 2) *
            BVB_RENDERING_ATTACHMENT_SIZE,
    BVB_BIND_GRAPHICS_PIPELINE_SIZE = 16,
    BVB_PUSH_ROTATION_SIZE = 16,
    BVB_SET_VIEWPORT_SIZE = 24,
    BVB_SET_SCISSOR_SIZE = 16,
    BVB_DRAW_SIZE = 16,
    BVB_FILL_BUFFER_SIZE = 32,
    BVB_BUFFER_HOST_READ_BARRIER_SIZE = 24,
    BVB_VULKAN_BEGIN_SIZE = 8,
    BVB_VULKAN_CLEAR_COLOR_IMAGE_SIZE = 16,
    BVB_VULKAN_INIT_IMAGE_BARRIER_SIZE = 40,
    BVB_VULKAN_IMAGE_RANGE_SIZE = 24,
    BVB_VULKAN_MEMORY_BARRIER_2_RECORD_SIZE = 32,
    BVB_VULKAN_BUFFER_BARRIER_2_RECORD_SIZE = 64,
    BVB_VULKAN_IMAGE_BARRIER_2_RECORD_SIZE = 80,
    BVB_VULKAN_IMAGE_BARRIER_2_HEADER_SIZE = 16,
    BVB_VULKAN_IMAGE_BARRIER_2_MAX_SIZE =
        BVB_VULKAN_IMAGE_BARRIER_2_HEADER_SIZE +
        BVB_COMMAND_VULKAN_MAX_MEMORY_BARRIERS *
            BVB_VULKAN_MEMORY_BARRIER_2_RECORD_SIZE +
        BVB_COMMAND_VULKAN_MAX_BUFFER_BARRIERS *
            BVB_VULKAN_BUFFER_BARRIER_2_RECORD_SIZE +
        BVB_COMMAND_VULKAN_MAX_IMAGE_BARRIERS *
            BVB_VULKAN_IMAGE_BARRIER_2_RECORD_SIZE,
    BVB_VULKAN_CLEAR_COLOR_IMAGE_GENERAL_SIZE = 32 +
        BVB_COMMAND_VULKAN_MAX_CLEAR_RANGES * BVB_VULKAN_IMAGE_RANGE_SIZE,
    BVB_VULKAN_BIND_DESCRIPTOR_SETS_SIZE = 24 +
        BVB_COMMAND_VULKAN_MAX_BOUND_DESCRIPTOR_SETS * sizeof(uint64_t) +
        BVB_COMMAND_VULKAN_MAX_DYNAMIC_OFFSETS * sizeof(uint32_t),
    BVB_VULKAN_PUSH_CONSTANTS_SIZE = 24 +
        BVB_COMMAND_VULKAN_MAX_PUSH_CONSTANT_BYTES,
    BVB_VULKAN_TRANSFER_HEADER_SIZE = 32,
    BVB_VULKAN_TRANSFER_REGION_SIZE = 128,
    BVB_VULKAN_TRANSFER_MAX_SIZE = BVB_VULKAN_TRANSFER_HEADER_SIZE +
        BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS *
            BVB_VULKAN_TRANSFER_REGION_SIZE,
    BVB_VULKAN_BIND_VERTEX_BUFFERS_SIZE = 16 +
        4 * BVB_COMMAND_VULKAN_MAX_VERTEX_BINDINGS * sizeof(uint64_t),
    BVB_VULKAN_BIND_INDEX_BUFFER_SIZE = 32,
    BVB_VULKAN_DRAW_INDEXED_SIZE = 24,
    BVB_VULKAN_DRAW_INDIRECT_SIZE = 32,
    BVB_VULKAN_DRAW_INDIRECT_COUNT_SIZE = 48,
    BVB_VULKAN_DYNAMIC_STATE_SIZE = 8 +
        BVB_COMMAND_VULKAN_MAX_DYNAMIC_STATE_VALUES * sizeof(uint32_t),
    BVB_VULKAN_CLEAR_DEPTH_STENCIL_IMAGE_SIZE = 24 +
        BVB_COMMAND_VULKAN_MAX_CLEAR_RANGES * BVB_VULKAN_IMAGE_RANGE_SIZE,
    BVB_VULKAN_CLEAR_ATTACHMENT_SIZE = 24,
    BVB_VULKAN_CLEAR_RECT_SIZE = 24,
    BVB_VULKAN_CLEAR_ATTACHMENTS_SIZE = 8 +
        BVB_COMMAND_VULKAN_MAX_CLEAR_ATTACHMENTS *
            BVB_VULKAN_CLEAR_ATTACHMENT_SIZE +
        BVB_COMMAND_VULKAN_MAX_CLEAR_RECTS * BVB_VULKAN_CLEAR_RECT_SIZE,
};

_Static_assert(sizeof(float) == sizeof(uint32_t),
               "command batches require 32-bit float");

static int expected_payload_size(uint16_t opcode, uint32_t *payload_size);
static int validate_payload(uint16_t opcode, const uint8_t *payload);
static int transfer_opcode_is_valid(uint16_t opcode);

static uint32_t transfer_payload_size(uint32_t region_count) {
    return BVB_VULKAN_TRANSFER_HEADER_SIZE +
        region_count * BVB_VULKAN_TRANSFER_REGION_SIZE;
}

static uint32_t barrier_payload_size(uint32_t memory_count,
                                     uint32_t buffer_count,
                                     uint32_t image_count) {
    return BVB_VULKAN_IMAGE_BARRIER_2_HEADER_SIZE +
        memory_count * BVB_VULKAN_MEMORY_BARRIER_2_RECORD_SIZE +
        buffer_count * BVB_VULKAN_BUFFER_BARRIER_2_RECORD_SIZE +
        image_count * BVB_VULKAN_IMAGE_BARRIER_2_RECORD_SIZE;
}

static int payload_length_is_valid(uint16_t opcode, const uint8_t *payload,
                                   uint32_t payload_length) {
    uint32_t expected = 0U;
    if (expected_payload_size(opcode, &expected) != 0) return 0;
    if (opcode == BVB_COMMAND_VULKAN_IMAGE_BARRIER_2) {
        if (payload == NULL ||
            payload_length < BVB_VULKAN_IMAGE_BARRIER_2_HEADER_SIZE)
            return 0;
        const uint32_t memory_count = bvb_wire_get_u32(payload + 4);
        const uint32_t buffer_count = bvb_wire_get_u32(payload + 8);
        const uint32_t image_count = bvb_wire_get_u32(payload + 12);
        return memory_count <= BVB_COMMAND_VULKAN_MAX_MEMORY_BARRIERS &&
            buffer_count <= BVB_COMMAND_VULKAN_MAX_BUFFER_BARRIERS &&
            image_count <= BVB_COMMAND_VULKAN_MAX_IMAGE_BARRIERS &&
            payload_length == barrier_payload_size(
                memory_count, buffer_count, image_count);
    }
    if (!transfer_opcode_is_valid(opcode)) return payload_length == expected;
    if (payload == NULL || payload_length < BVB_VULKAN_TRANSFER_HEADER_SIZE)
        return 0;
    const uint32_t count = bvb_wire_get_u32(payload + 28);
    return count != 0U &&
        count <= BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS &&
        payload_length == transfer_payload_size(count);
}

static void put_float(uint8_t *output, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    bvb_wire_put_u32(output, bits);
}

static float get_float(const uint8_t *input) {
    uint32_t bits = bvb_wire_get_u32(input);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static bool float_word_is_finite(uint32_t word) {
    return (word & UINT32_C(0x7f800000)) != UINT32_C(0x7f800000);
}

static int float_bits_are_finite(const uint8_t *input) {
    return (bvb_wire_get_u32(input) & UINT32_C(0x7f800000)) !=
           UINT32_C(0x7f800000);
}

static int image_range_is_valid(
    const struct bvb_vulkan_image_subresource_range *range) {
    return range != NULL && range->aspect_mask != 0U &&
           range->level_count != 0U && range->layer_count != 0U;
}

static int rendering_attachment_is_valid(
    const struct bvb_vulkan_rendering_attachment *attachment,
    bool required) {
    if (attachment == NULL || attachment->feedback_loop_enable > 1U)
        return 0;
    if (attachment->image_view_id == 0U)
        return !required && attachment->resolve_image_view_id == 0U &&
               attachment->resolve_mode == 0U;
    if (bvb_handle_expect(attachment->image_view_id,
                          BVB_OBJECT_IMAGE_VIEW) != 0 ||
        attachment->image_layout == 0U) return 0;
    if (attachment->resolve_mode == 0U)
        return attachment->resolve_image_view_id == 0U;
    return bvb_handle_expect(attachment->resolve_image_view_id,
                             BVB_OBJECT_IMAGE_VIEW) == 0 &&
           attachment->resolve_image_layout != 0U;
}

static void encode_rendering_attachment(
    uint8_t output[BVB_RENDERING_ATTACHMENT_SIZE],
    const struct bvb_vulkan_rendering_attachment *attachment) {
    bvb_wire_put_u64(output, attachment->image_view_id);
    bvb_wire_put_u64(output + 8, attachment->resolve_image_view_id);
    bvb_wire_put_u32(output + 16, attachment->image_layout);
    bvb_wire_put_u32(output + 20, attachment->resolve_mode);
    bvb_wire_put_u32(output + 24, attachment->resolve_image_layout);
    bvb_wire_put_u32(output + 28, attachment->load_op);
    bvb_wire_put_u32(output + 32, attachment->store_op);
    bvb_wire_put_u32(output + 36, attachment->feedback_loop_enable);
    for (uint32_t index = 0U; index < 4U; ++index)
        bvb_wire_put_u32(output + 40U + index * sizeof(uint32_t),
                         attachment->clear_words[index]);
}

static struct bvb_vulkan_rendering_attachment decode_rendering_attachment(
    const uint8_t input[BVB_RENDERING_ATTACHMENT_SIZE]) {
    struct bvb_vulkan_rendering_attachment attachment = {
        .image_view_id = bvb_wire_get_u64(input),
        .resolve_image_view_id = bvb_wire_get_u64(input + 8),
        .image_layout = bvb_wire_get_u32(input + 16),
        .resolve_mode = bvb_wire_get_u32(input + 20),
        .resolve_image_layout = bvb_wire_get_u32(input + 24),
        .load_op = bvb_wire_get_u32(input + 28),
        .store_op = bvb_wire_get_u32(input + 32),
        .feedback_loop_enable = bvb_wire_get_u32(input + 36),
    };
    for (uint32_t index = 0U; index < 4U; ++index)
        attachment.clear_words[index] =
            bvb_wire_get_u32(input + 40U + index * sizeof(uint32_t));
    return attachment;
}

static void encode_image_range(
    uint8_t output[BVB_VULKAN_IMAGE_RANGE_SIZE],
    const struct bvb_vulkan_image_subresource_range *range) {
    bvb_wire_put_u32(output, range->aspect_mask);
    bvb_wire_put_u32(output + 4, range->base_mip_level);
    bvb_wire_put_u32(output + 8, range->level_count);
    bvb_wire_put_u32(output + 12, range->base_array_layer);
    bvb_wire_put_u32(output + 16, range->layer_count);
    bvb_wire_put_u32(output + 20, 0U);
}

static int validate_image_range_wire(const uint8_t *input) {
    return bvb_wire_get_u32(input) == 0U ||
                   bvb_wire_get_u32(input + 8) == 0U ||
                   bvb_wire_get_u32(input + 16) == 0U ||
                   bvb_wire_get_u32(input + 20) != 0U
               ? -EPROTO
               : 0;
}

static int image_range_wire_is_zero(const uint8_t *input) {
    for (size_t offset = 0U; offset < BVB_VULKAN_IMAGE_RANGE_SIZE;
         offset += sizeof(uint32_t)) {
        if (bvb_wire_get_u32(input + offset) != 0U) return 0;
    }
    return 1;
}

static int transfer_opcode_is_valid(uint16_t opcode) {
    return opcode >= BVB_COMMAND_VULKAN_COPY_BUFFER_2 &&
           opcode <= BVB_COMMAND_VULKAN_RESOLVE_IMAGE_2;
}

static int bytes_are_zero(const uint8_t *bytes, size_t size) {
    for (size_t index = 0U; index < size; ++index)
        if (bytes[index] != 0U) return 0;
    return 1;
}

static int transfer_layers_are_valid(
    const struct bvb_vulkan_image_subresource_layers *layers) {
    return layers != NULL && layers->aspect_mask != 0U &&
           layers->layer_count != 0U;
}

static void encode_transfer_layers(
    uint8_t output[16],
    const struct bvb_vulkan_image_subresource_layers *layers) {
    bvb_wire_put_u32(output, layers->aspect_mask);
    bvb_wire_put_u32(output + 4, layers->mip_level);
    bvb_wire_put_u32(output + 8, layers->base_array_layer);
    bvb_wire_put_u32(output + 12, layers->layer_count);
}

static struct bvb_vulkan_image_subresource_layers decode_transfer_layers(
    const uint8_t input[16]) {
    return (struct bvb_vulkan_image_subresource_layers){
        .aspect_mask = bvb_wire_get_u32(input),
        .mip_level = bvb_wire_get_u32(input + 4),
        .base_array_layer = bvb_wire_get_u32(input + 8),
        .layer_count = bvb_wire_get_u32(input + 12),
    };
}

static struct bvb_vulkan_image_subresource_range decode_image_range(
    const uint8_t input[BVB_VULKAN_IMAGE_RANGE_SIZE]) {
    return (struct bvb_vulkan_image_subresource_range){
        .aspect_mask = bvb_wire_get_u32(input),
        .base_mip_level = bvb_wire_get_u32(input + 4),
        .level_count = bvb_wire_get_u32(input + 8),
        .base_array_layer = bvb_wire_get_u32(input + 12),
        .layer_count = bvb_wire_get_u32(input + 16),
    };
}

static int append_record(struct bvb_command_batch_builder *builder,
                         uint16_t opcode, const uint8_t *payload,
                         uint32_t payload_length) {
    if (builder == NULL || builder->bytes == NULL || builder->finished ||
        builder->length < BVB_COMMAND_BATCH_HEADER_SIZE ||
        builder->length > builder->capacity ||
        builder->command_count >= BVB_COMMAND_BATCH_MAX_COMMANDS) {
        return -EINVAL;
    }
    size_t record_length = BVB_COMMAND_RECORD_HEADER_SIZE + payload_length;
    if (record_length > BVB_COMMAND_BATCH_MAX_BYTES ||
        record_length > builder->capacity ||
        builder->length > builder->capacity - record_length ||
        builder->length > BVB_COMMAND_BATCH_MAX_BYTES - record_length) {
        return -ENOSPC;
    }
    uint8_t *record = builder->bytes + builder->length;
    bvb_wire_put_u16(record, opcode);
    bvb_wire_put_u16(record + 2, 0U);
    bvb_wire_put_u32(record + 4, payload_length);
    if (payload_length != 0U) {
        memcpy(record + BVB_COMMAND_RECORD_HEADER_SIZE, payload, payload_length);
    }
    builder->length += record_length;
    ++builder->command_count;
    return 0;
}

int bvb_command_batch_begin(struct bvb_command_batch_builder *builder,
                            uint8_t *bytes, size_t capacity,
                            uint64_t command_buffer_id, uint64_t sequence) {
    if (builder == NULL || bytes == NULL ||
        capacity < BVB_COMMAND_BATCH_HEADER_SIZE ||
        capacity > BVB_COMMAND_BATCH_MAX_BYTES || sequence == 0U ||
        bvb_handle_expect(command_buffer_id, BVB_OBJECT_COMMAND_BUFFER) != 0) {
        return -EINVAL;
    }
    memset(bytes, 0, BVB_COMMAND_BATCH_HEADER_SIZE);
    *builder = (struct bvb_command_batch_builder){
        .bytes = bytes,
        .capacity = capacity,
        .length = BVB_COMMAND_BATCH_HEADER_SIZE,
        .command_buffer_id = command_buffer_id,
        .sequence = sequence,
        .command_count = 0U,
        .finished = false,
    };
    return 0;
}

int bvb_command_batch_append_begin_rendering(
    struct bvb_command_batch_builder *builder,
    const struct bvb_begin_rendering_command *command) {
    if (command == NULL || command->width == 0U || command->height == 0U ||
        command->layer_count == 0U ||
        command->color_attachment_count >
            BVB_COMMAND_VULKAN_MAX_COLOR_ATTACHMENTS ||
        command->has_depth_attachment > 1U ||
        command->has_stencil_attachment > 1U ||
        (command->color_attachment_count == 0U &&
         command->has_depth_attachment == 0U &&
         command->has_stencil_attachment == 0U)) {
        return -EINVAL;
    }
    uint8_t payload[BVB_BEGIN_RENDERING_SIZE];
    memset(payload, 0, sizeof(payload));
    bvb_wire_put_u32(payload, command->flags);
    bvb_wire_put_u32(payload + 4, (uint32_t)command->render_offset_x);
    bvb_wire_put_u32(payload + 8, (uint32_t)command->render_offset_y);
    bvb_wire_put_u32(payload + 12, command->width);
    bvb_wire_put_u32(payload + 16, command->height);
    bvb_wire_put_u32(payload + 20, command->layer_count);
    bvb_wire_put_u32(payload + 24, command->view_mask);
    bvb_wire_put_u32(payload + 28, command->color_attachment_count);
    bvb_wire_put_u32(payload + 32, command->has_depth_attachment);
    bvb_wire_put_u32(payload + 36, command->has_stencil_attachment);
    for (uint32_t index = 0U; index < command->color_attachment_count;
         ++index) {
        if (!rendering_attachment_is_valid(
                &command->color_attachments[index], false)) return -EINVAL;
        encode_rendering_attachment(
            payload + BVB_BEGIN_RENDERING_HEADER_SIZE +
                index * BVB_RENDERING_ATTACHMENT_SIZE,
            &command->color_attachments[index]);
    }
    if (command->has_depth_attachment != 0U &&
        !rendering_attachment_is_valid(&command->depth_attachment, true))
        return -EINVAL;
    if (command->has_stencil_attachment != 0U &&
        !rendering_attachment_is_valid(&command->stencil_attachment, true))
        return -EINVAL;
    if (command->has_depth_attachment != 0U)
        encode_rendering_attachment(
            payload + BVB_BEGIN_RENDERING_HEADER_SIZE +
                BVB_COMMAND_VULKAN_MAX_COLOR_ATTACHMENTS *
                    BVB_RENDERING_ATTACHMENT_SIZE,
            &command->depth_attachment);
    if (command->has_stencil_attachment != 0U)
        encode_rendering_attachment(
            payload + BVB_BEGIN_RENDERING_HEADER_SIZE +
                (BVB_COMMAND_VULKAN_MAX_COLOR_ATTACHMENTS + 1U) *
                    BVB_RENDERING_ATTACHMENT_SIZE,
            &command->stencil_attachment);
    return append_record(builder, BVB_COMMAND_BEGIN_RENDERING, payload,
                         sizeof(payload));
}

int bvb_command_batch_append_bind_graphics_pipeline(
    struct bvb_command_batch_builder *builder,
    const struct bvb_bind_graphics_pipeline_command *command) {
    if (command == NULL ||
        bvb_handle_expect(command->pipeline_id, BVB_OBJECT_PIPELINE) != 0) {
        return -EINVAL;
    }
    uint8_t payload[BVB_BIND_GRAPHICS_PIPELINE_SIZE];
    bvb_wire_put_u64(payload, command->pipeline_id);
    bvb_wire_put_u32(payload + 8, 0U);
    bvb_wire_put_u32(payload + 12, 0U);
    return append_record(builder, BVB_COMMAND_BIND_GRAPHICS_PIPELINE, payload,
                         sizeof(payload));
}

int bvb_command_batch_append_push_rotation(
    struct bvb_command_batch_builder *builder,
    const struct bvb_push_rotation_command *command) {
    if (command == NULL ||
        bvb_handle_expect(command->pipeline_layout_id,
                          BVB_OBJECT_PIPELINE_LAYOUT) != 0 ||
        command->angle_radians < 0.0F || command->angle_radians >= 6.283186F ||
        command->aspect_ratio <= 0.0F) {
        return -EINVAL;
    }
    uint8_t payload[BVB_PUSH_ROTATION_SIZE];
    bvb_wire_put_u64(payload, command->pipeline_layout_id);
    put_float(payload + 8, command->angle_radians);
    put_float(payload + 12, command->aspect_ratio);
    if (!float_bits_are_finite(payload + 8) ||
        !float_bits_are_finite(payload + 12)) {
        return -EINVAL;
    }
    return append_record(builder, BVB_COMMAND_PUSH_ROTATION, payload,
                         sizeof(payload));
}

int bvb_command_batch_append_set_viewport(
    struct bvb_command_batch_builder *builder,
    const struct bvb_set_viewport_command *command) {
    if (command == NULL) {
        return -EINVAL;
    }
    const float values[] = {command->x,
                            command->y,
                            command->width,
                            command->height,
                            command->minimum_depth,
                            command->maximum_depth};
    uint8_t payload[BVB_SET_VIEWPORT_SIZE];
    for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); ++index) {
        put_float(payload + index * 4U, values[index]);
        if (!float_bits_are_finite(payload + index * 4U)) {
            return -EINVAL;
        }
    }
    if (command->width == 0.0F || command->height == 0.0F ||
        command->minimum_depth < 0.0F || command->minimum_depth > 1.0F ||
        command->maximum_depth < 0.0F || command->maximum_depth > 1.0F ||
        command->minimum_depth > command->maximum_depth) {
        return -EINVAL;
    }
    return append_record(builder, BVB_COMMAND_SET_VIEWPORT, payload,
                         sizeof(payload));
}

int bvb_command_batch_append_set_scissor(
    struct bvb_command_batch_builder *builder,
    const struct bvb_set_scissor_command *command) {
    if (command == NULL || command->width == 0U || command->height == 0U) {
        return -EINVAL;
    }
    uint8_t payload[BVB_SET_SCISSOR_SIZE];
    bvb_wire_put_i32(payload, command->x);
    bvb_wire_put_i32(payload + 4, command->y);
    bvb_wire_put_u32(payload + 8, command->width);
    bvb_wire_put_u32(payload + 12, command->height);
    return append_record(builder, BVB_COMMAND_SET_SCISSOR, payload,
                         sizeof(payload));
}

int bvb_command_batch_append_draw(struct bvb_command_batch_builder *builder,
                                  const struct bvb_draw_command *command) {
    if (command == NULL || command->vertex_count == 0U ||
        command->instance_count == 0U) {
        return -EINVAL;
    }
    uint8_t payload[BVB_DRAW_SIZE];
    bvb_wire_put_u32(payload, command->vertex_count);
    bvb_wire_put_u32(payload + 4, command->instance_count);
    bvb_wire_put_u32(payload + 8, command->first_vertex);
    bvb_wire_put_u32(payload + 12, command->first_instance);
    return append_record(builder, BVB_COMMAND_DRAW, payload, sizeof(payload));
}

int bvb_command_batch_append_end_rendering(
    struct bvb_command_batch_builder *builder) {
    return append_record(builder, BVB_COMMAND_END_RENDERING, NULL, 0U);
}

int bvb_command_batch_append_fill_buffer(
    struct bvb_command_batch_builder *builder,
    const struct bvb_fill_buffer_command *command) {
    if (command == NULL || command->size == 0U ||
        (command->offset & 3U) != 0U || (command->size & 3U) != 0U ||
        bvb_handle_expect(command->buffer_id, BVB_OBJECT_BUFFER) != 0) {
        return -EINVAL;
    }
    uint8_t payload[BVB_FILL_BUFFER_SIZE];
    bvb_wire_put_u64(payload, command->buffer_id);
    bvb_wire_put_u64(payload + 8, command->offset);
    bvb_wire_put_u64(payload + 16, command->size);
    bvb_wire_put_u32(payload + 24, command->data);
    bvb_wire_put_u32(payload + 28, 0U);
    return append_record(builder, BVB_COMMAND_FILL_BUFFER, payload,
                         sizeof(payload));
}

int bvb_command_batch_append_buffer_host_read_barrier(
    struct bvb_command_batch_builder *builder,
    const struct bvb_buffer_host_read_barrier_command *command) {
    if (command == NULL || command->size == 0U ||
        bvb_handle_expect(command->buffer_id, BVB_OBJECT_BUFFER) != 0) {
        return -EINVAL;
    }
    uint8_t payload[BVB_BUFFER_HOST_READ_BARRIER_SIZE];
    bvb_wire_put_u64(payload, command->buffer_id);
    bvb_wire_put_u64(payload + 8, command->offset);
    bvb_wire_put_u64(payload + 16, command->size);
    return append_record(builder, BVB_COMMAND_BUFFER_HOST_READ_BARRIER, payload,
                         sizeof(payload));
}

int bvb_command_batch_append_vulkan_begin(
    struct bvb_command_batch_builder *builder,
    const struct bvb_vulkan_begin_command *command) {
    if (command == NULL || (command->flags & ~1U) != 0U) {
        return -EINVAL;
    }
    uint8_t payload[BVB_VULKAN_BEGIN_SIZE];
    bvb_wire_put_u32(payload, command->flags);
    bvb_wire_put_u32(payload + 4, 0U);
    return append_record(builder, BVB_COMMAND_VULKAN_BEGIN, payload,
                         sizeof(payload));
}

int bvb_command_batch_append_vulkan_clear_color_image(
    struct bvb_command_batch_builder *builder,
    const struct bvb_vulkan_clear_color_image_command *command) {
    if (command == NULL ||
        bvb_handle_expect(command->image_id, BVB_OBJECT_IMAGE) != 0) {
        return -EINVAL;
    }
    uint8_t payload[BVB_VULKAN_CLEAR_COLOR_IMAGE_SIZE];
    bvb_wire_put_u64(payload, command->image_id);
    bvb_wire_put_u64(payload + 8, 0U);
    return append_record(builder, BVB_COMMAND_VULKAN_CLEAR_COLOR_IMAGE,
                         payload, sizeof(payload));
}

int bvb_command_batch_append_vulkan_init_image_barrier(
    struct bvb_command_batch_builder *builder,
    const struct bvb_vulkan_init_image_barrier_command *command) {
    if (command == NULL || command->image_count == 0U ||
        command->image_count >
            BVB_COMMAND_VULKAN_MAX_INIT_IMAGE_BARRIERS) {
        return -EINVAL;
    }
    uint8_t payload[BVB_VULKAN_INIT_IMAGE_BARRIER_SIZE];
    memset(payload, 0, sizeof(payload));
    bvb_wire_put_u32(payload, command->image_count);
    for (uint32_t index = 0U; index < command->image_count; ++index) {
        if (bvb_handle_expect(command->image_ids[index], BVB_OBJECT_IMAGE) !=
            0) {
            return -EINVAL;
        }
        for (uint32_t earlier = 0U; earlier < index; ++earlier) {
            if (command->image_ids[earlier] == command->image_ids[index]) {
                return -EINVAL;
            }
        }
        bvb_wire_put_u64(payload + 8U + index * sizeof(uint64_t),
                         command->image_ids[index]);
    }
    return append_record(builder, BVB_COMMAND_VULKAN_INIT_IMAGE_BARRIER,
                         payload, sizeof(payload));
}

int bvb_command_batch_append_vulkan_image_barrier_2(
    struct bvb_command_batch_builder *builder,
    const struct bvb_vulkan_image_barrier_2_command *command) {
    if (command == NULL || (command->dependency_flags & ~UINT32_C(0x6f)) != 0U ||
        command->memory_count > BVB_COMMAND_VULKAN_MAX_MEMORY_BARRIERS ||
        command->buffer_count > BVB_COMMAND_VULKAN_MAX_BUFFER_BARRIERS ||
        command->image_count > BVB_COMMAND_VULKAN_MAX_IMAGE_BARRIERS) {
        return -EINVAL;
    }
    uint8_t payload[BVB_VULKAN_IMAGE_BARRIER_2_MAX_SIZE];
    memset(payload, 0, sizeof(payload));
    bvb_wire_put_u32(payload, command->dependency_flags);
    bvb_wire_put_u32(payload + 4, command->memory_count);
    bvb_wire_put_u32(payload + 8, command->buffer_count);
    bvb_wire_put_u32(payload + 12, command->image_count);
    size_t base = BVB_VULKAN_IMAGE_BARRIER_2_HEADER_SIZE;
    for (uint32_t index = 0U; index < command->memory_count; ++index) {
        const struct bvb_vulkan_memory_barrier_2 *barrier =
            &command->memory[index];
        uint8_t *record = payload + base +
            index * BVB_VULKAN_MEMORY_BARRIER_2_RECORD_SIZE;
        bvb_wire_put_u64(record, barrier->source_stage_mask);
        bvb_wire_put_u64(record + 8, barrier->source_access_mask);
        bvb_wire_put_u64(record + 16, barrier->destination_stage_mask);
        bvb_wire_put_u64(record + 24, barrier->destination_access_mask);
    }
    base += command->memory_count *
        BVB_VULKAN_MEMORY_BARRIER_2_RECORD_SIZE;
    for (uint32_t index = 0U; index < command->buffer_count; ++index) {
        const struct bvb_vulkan_buffer_barrier_2 *barrier =
            &command->buffers[index];
        if (bvb_handle_expect(barrier->buffer_id, BVB_OBJECT_BUFFER) != 0 ||
            barrier->size == 0U) return -EINVAL;
        uint8_t *record = payload + base +
            index * BVB_VULKAN_BUFFER_BARRIER_2_RECORD_SIZE;
        bvb_wire_put_u64(record, barrier->source_stage_mask);
        bvb_wire_put_u64(record + 8, barrier->source_access_mask);
        bvb_wire_put_u64(record + 16, barrier->destination_stage_mask);
        bvb_wire_put_u64(record + 24, barrier->destination_access_mask);
        bvb_wire_put_u32(record + 32, barrier->source_queue_family_index);
        bvb_wire_put_u32(record + 36,
                         barrier->destination_queue_family_index);
        bvb_wire_put_u64(record + 40, barrier->buffer_id);
        bvb_wire_put_u64(record + 48, barrier->offset);
        bvb_wire_put_u64(record + 56, barrier->size);
    }
    base += command->buffer_count *
        BVB_VULKAN_BUFFER_BARRIER_2_RECORD_SIZE;
    for (uint32_t index = 0U; index < command->image_count; ++index) {
        const struct bvb_vulkan_image_barrier_2 *barrier =
            &command->images[index];
        if (bvb_handle_expect(barrier->image_id, BVB_OBJECT_IMAGE) != 0 ||
            !image_range_is_valid(&barrier->range)) {
            return -EINVAL;
        }
        uint8_t *record = payload + base +
            index * BVB_VULKAN_IMAGE_BARRIER_2_RECORD_SIZE;
        bvb_wire_put_u64(record, barrier->source_stage_mask);
        bvb_wire_put_u64(record + 8, barrier->source_access_mask);
        bvb_wire_put_u64(record + 16, barrier->destination_stage_mask);
        bvb_wire_put_u64(record + 24, barrier->destination_access_mask);
        bvb_wire_put_u32(record + 32, barrier->old_layout);
        bvb_wire_put_u32(record + 36, barrier->new_layout);
        bvb_wire_put_u32(record + 40,
                         barrier->source_queue_family_index);
        bvb_wire_put_u32(record + 44,
                         barrier->destination_queue_family_index);
        bvb_wire_put_u64(record + 48, barrier->image_id);
        encode_image_range(record + 56, &barrier->range);
    }
    return append_record(builder, BVB_COMMAND_VULKAN_IMAGE_BARRIER_2,
                         payload, barrier_payload_size(
                             command->memory_count, command->buffer_count,
                             command->image_count));
}

int bvb_command_batch_append_vulkan_clear_color_image_general(
    struct bvb_command_batch_builder *builder,
    const struct bvb_vulkan_clear_color_image_general_command *command) {
    if (command == NULL || command->image_layout == 0U ||
        command->range_count == 0U ||
        command->range_count > BVB_COMMAND_VULKAN_MAX_CLEAR_RANGES ||
        bvb_handle_expect(command->image_id, BVB_OBJECT_IMAGE) != 0) {
        return -EINVAL;
    }
    uint8_t payload[BVB_VULKAN_CLEAR_COLOR_IMAGE_GENERAL_SIZE];
    memset(payload, 0, sizeof(payload));
    bvb_wire_put_u64(payload, command->image_id);
    bvb_wire_put_u32(payload + 8, command->image_layout);
    bvb_wire_put_u32(payload + 12, command->range_count);
    for (size_t index = 0U; index < 4U; ++index) {
        bvb_wire_put_u32(payload + 16U + index * sizeof(uint32_t),
                         command->color_words[index]);
    }
    for (uint32_t index = 0U; index < command->range_count; ++index) {
        if (!image_range_is_valid(&command->ranges[index])) return -EINVAL;
        encode_image_range(
            payload + 32U + index * BVB_VULKAN_IMAGE_RANGE_SIZE,
            &command->ranges[index]);
    }
    return append_record(builder,
                         BVB_COMMAND_VULKAN_CLEAR_COLOR_IMAGE_GENERAL,
                         payload, sizeof(payload));
}

int bvb_command_batch_append_vulkan_bind_descriptor_sets(
    struct bvb_command_batch_builder *builder,
    const struct bvb_vulkan_bind_descriptor_sets_command *command) {
    if (command == NULL ||
        bvb_handle_expect(command->pipeline_layout_id,
                          BVB_OBJECT_PIPELINE_LAYOUT) != 0 ||
        command->pipeline_bind_point > 1U ||
        command->descriptor_set_count == 0U ||
        command->descriptor_set_count >
            BVB_COMMAND_VULKAN_MAX_BOUND_DESCRIPTOR_SETS ||
        command->dynamic_offset_count >
            BVB_COMMAND_VULKAN_MAX_DYNAMIC_OFFSETS) return -EINVAL;
    uint8_t payload[BVB_VULKAN_BIND_DESCRIPTOR_SETS_SIZE];
    memset(payload, 0, sizeof(payload));
    bvb_wire_put_u64(payload, command->pipeline_layout_id);
    bvb_wire_put_u32(payload + 8, command->pipeline_bind_point);
    bvb_wire_put_u32(payload + 12, command->first_set);
    bvb_wire_put_u32(payload + 16, command->descriptor_set_count);
    bvb_wire_put_u32(payload + 20, command->dynamic_offset_count);
    uint32_t cursor = 24U;
    for (uint32_t index = 0U; index < command->descriptor_set_count; ++index) {
        if (bvb_handle_expect(command->descriptor_set_ids[index],
                              BVB_OBJECT_DESCRIPTOR_SET) != 0) return -EINVAL;
        bvb_wire_put_u64(payload + cursor,
                         command->descriptor_set_ids[index]);
        cursor += sizeof(uint64_t);
    }
    cursor = 24U + BVB_COMMAND_VULKAN_MAX_BOUND_DESCRIPTOR_SETS *
        sizeof(uint64_t);
    for (uint32_t index = 0U; index < command->dynamic_offset_count; ++index) {
        bvb_wire_put_u32(payload + cursor,
                         command->dynamic_offsets[index]);
        cursor += sizeof(uint32_t);
    }
    return append_record(builder, BVB_COMMAND_VULKAN_BIND_DESCRIPTOR_SETS,
                         payload, sizeof(payload));
}

int bvb_command_batch_append_vulkan_push_constants(
    struct bvb_command_batch_builder *builder,
    const struct bvb_vulkan_push_constants_command *command) {
    if (command == NULL || command->stage_flags == 0U ||
        command->size == 0U ||
        command->size > BVB_COMMAND_VULKAN_MAX_PUSH_CONSTANT_BYTES ||
        (command->offset & 3U) != 0U || (command->size & 3U) != 0U ||
        command->offset > BVB_COMMAND_VULKAN_MAX_PUSH_CONSTANT_BYTES -
                              command->size ||
        bvb_handle_expect(command->pipeline_layout_id,
                          BVB_OBJECT_PIPELINE_LAYOUT) != 0) return -EINVAL;
    uint8_t payload[BVB_VULKAN_PUSH_CONSTANTS_SIZE];
    memset(payload, 0, sizeof(payload));
    bvb_wire_put_u64(payload, command->pipeline_layout_id);
    bvb_wire_put_u32(payload + 8, command->stage_flags);
    bvb_wire_put_u32(payload + 12, command->offset);
    bvb_wire_put_u32(payload + 16, command->size);
    memcpy(payload + 24, command->data, command->size);
    return append_record(builder, BVB_COMMAND_VULKAN_PUSH_CONSTANTS,
                         payload, sizeof(payload));
}

int bvb_command_batch_append_vulkan_transfer(
    struct bvb_command_batch_builder *builder, uint16_t opcode,
    const struct bvb_vulkan_transfer_command *command) {
    if (!transfer_opcode_is_valid(opcode) || command == NULL ||
        command->region_count == 0U ||
        command->region_count > BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS)
        return -EINVAL;
    const bool source_buffer =
        opcode == BVB_COMMAND_VULKAN_COPY_BUFFER_2 ||
        opcode == BVB_COMMAND_VULKAN_COPY_BUFFER_TO_IMAGE_2;
    const bool destination_buffer =
        opcode == BVB_COMMAND_VULKAN_COPY_BUFFER_2 ||
        opcode == BVB_COMMAND_VULKAN_COPY_IMAGE_TO_BUFFER_2;
    if (bvb_handle_expect(command->source_id,
                          source_buffer ? BVB_OBJECT_BUFFER :
                                          BVB_OBJECT_IMAGE) != 0 ||
        bvb_handle_expect(command->destination_id,
                          destination_buffer ? BVB_OBJECT_BUFFER :
                                               BVB_OBJECT_IMAGE) != 0 ||
        ((!source_buffer && command->source_layout == 0U) ||
         (!destination_buffer && command->destination_layout == 0U)) ||
        (opcode == BVB_COMMAND_VULKAN_BLIT_IMAGE_2 &&
         command->filter > 1U)) return -EINVAL;
    uint8_t payload[BVB_VULKAN_TRANSFER_MAX_SIZE];
    memset(payload, 0, sizeof(payload));
    bvb_wire_put_u64(payload, command->source_id);
    bvb_wire_put_u64(payload + 8, command->destination_id);
    bvb_wire_put_u32(payload + 16, command->source_layout);
    bvb_wire_put_u32(payload + 20, command->destination_layout);
    bvb_wire_put_u32(payload + 24, command->filter);
    bvb_wire_put_u32(payload + 28, command->region_count);
    for (uint32_t index = 0U; index < command->region_count; ++index) {
        const struct bvb_vulkan_transfer_region *region =
            &command->regions[index];
        const bool buffer_copy =
            opcode == BVB_COMMAND_VULKAN_COPY_BUFFER_2;
        if ((buffer_copy && region->size == 0U) ||
            (!buffer_copy &&
             (region->extent.width == 0U || region->extent.height == 0U ||
              region->extent.depth == 0U)) ||
            (!source_buffer &&
             !transfer_layers_are_valid(&region->source_layers)) ||
            (!destination_buffer &&
             !transfer_layers_are_valid(&region->destination_layers)))
            return -EINVAL;
        uint8_t *wire = payload + BVB_VULKAN_TRANSFER_HEADER_SIZE +
            index * BVB_VULKAN_TRANSFER_REGION_SIZE;
        bvb_wire_put_u64(wire, region->source_buffer_offset);
        bvb_wire_put_u64(wire + 8, region->destination_buffer_offset);
        bvb_wire_put_u64(wire + 16, region->size);
        bvb_wire_put_u32(wire + 24, region->buffer_row_length);
        bvb_wire_put_u32(wire + 28, region->buffer_image_height);
        encode_transfer_layers(wire + 32, &region->source_layers);
        encode_transfer_layers(wire + 48, &region->destination_layers);
        for (uint32_t offset = 0U; offset < 2U; ++offset) {
            bvb_wire_put_u32(wire + 64U + offset * 12U,
                             (uint32_t)region->source_offsets[offset].x);
            bvb_wire_put_u32(wire + 68U + offset * 12U,
                             (uint32_t)region->source_offsets[offset].y);
            bvb_wire_put_u32(wire + 72U + offset * 12U,
                             (uint32_t)region->source_offsets[offset].z);
            bvb_wire_put_u32(wire + 88U + offset * 12U,
                             (uint32_t)region->destination_offsets[offset].x);
            bvb_wire_put_u32(wire + 92U + offset * 12U,
                             (uint32_t)region->destination_offsets[offset].y);
            bvb_wire_put_u32(wire + 96U + offset * 12U,
                             (uint32_t)region->destination_offsets[offset].z);
        }
        bvb_wire_put_u32(wire + 112, region->extent.width);
        bvb_wire_put_u32(wire + 116, region->extent.height);
        bvb_wire_put_u32(wire + 120, region->extent.depth);
    }
    return append_record(builder, opcode, payload,
                         transfer_payload_size(command->region_count));
}

int bvb_command_batch_append_vulkan_bind_vertex_buffers(
    struct bvb_command_batch_builder *builder, uint16_t opcode,
    const struct bvb_vulkan_bind_vertex_buffers_command *command) {
    if ((opcode != BVB_COMMAND_VULKAN_BIND_VERTEX_BUFFERS &&
         opcode != BVB_COMMAND_VULKAN_BIND_VERTEX_BUFFERS_2) ||
        command == NULL || command->binding_count == 0U ||
        command->binding_count > BVB_COMMAND_VULKAN_MAX_VERTEX_BINDINGS ||
        command->has_sizes > 1U || command->has_strides > 1U ||
        (opcode == BVB_COMMAND_VULKAN_BIND_VERTEX_BUFFERS &&
         (command->has_sizes != 0U || command->has_strides != 0U)))
        return -EINVAL;
    uint8_t payload[BVB_VULKAN_BIND_VERTEX_BUFFERS_SIZE];
    memset(payload, 0, sizeof(payload));
    bvb_wire_put_u32(payload, command->first_binding);
    bvb_wire_put_u32(payload + 4, command->binding_count);
    bvb_wire_put_u32(payload + 8, command->has_sizes);
    bvb_wire_put_u32(payload + 12, command->has_strides);
    const size_t ids = 16U;
    const size_t offsets = ids +
        BVB_COMMAND_VULKAN_MAX_VERTEX_BINDINGS * sizeof(uint64_t);
    const size_t sizes = offsets +
        BVB_COMMAND_VULKAN_MAX_VERTEX_BINDINGS * sizeof(uint64_t);
    const size_t strides = sizes +
        BVB_COMMAND_VULKAN_MAX_VERTEX_BINDINGS * sizeof(uint64_t);
    for (uint32_t index = 0U; index < command->binding_count; ++index) {
        if (command->buffer_ids[index] != 0U &&
            bvb_handle_expect(command->buffer_ids[index],
                              BVB_OBJECT_BUFFER) != 0)
            return -EINVAL;
        bvb_wire_put_u64(payload + ids + index * sizeof(uint64_t),
                         command->buffer_ids[index]);
        bvb_wire_put_u64(payload + offsets + index * sizeof(uint64_t),
                         command->offsets[index]);
        if (command->has_sizes != 0U)
            bvb_wire_put_u64(payload + sizes + index * sizeof(uint64_t),
                             command->sizes[index]);
        if (command->has_strides != 0U)
            bvb_wire_put_u64(payload + strides + index * sizeof(uint64_t),
                             command->strides[index]);
    }
    return append_record(builder, opcode, payload, sizeof(payload));
}

int bvb_command_batch_append_vulkan_bind_index_buffer(
    struct bvb_command_batch_builder *builder, uint16_t opcode,
    const struct bvb_vulkan_bind_index_buffer_command *command) {
    if ((opcode != BVB_COMMAND_VULKAN_BIND_INDEX_BUFFER &&
         opcode != BVB_COMMAND_VULKAN_BIND_INDEX_BUFFER_2) ||
        command == NULL ||
        (command->buffer_id != 0U &&
         bvb_handle_expect(command->buffer_id, BVB_OBJECT_BUFFER) != 0) ||
        (opcode == BVB_COMMAND_VULKAN_BIND_INDEX_BUFFER &&
         command->size != UINT64_MAX) ||
        (command->buffer_id != 0U && command->size == 0U))
        return -EINVAL;
    uint8_t payload[BVB_VULKAN_BIND_INDEX_BUFFER_SIZE] = {0};
    bvb_wire_put_u64(payload, command->buffer_id);
    bvb_wire_put_u64(payload + 8, command->offset);
    bvb_wire_put_u64(payload + 16, command->size);
    bvb_wire_put_u32(payload + 24, command->index_type);
    return append_record(builder, opcode, payload, sizeof(payload));
}

int bvb_command_batch_append_vulkan_draw_indexed(
    struct bvb_command_batch_builder *builder,
    const struct bvb_vulkan_draw_indexed_command *command) {
    if (command == NULL) return -EINVAL;
    uint8_t payload[BVB_VULKAN_DRAW_INDEXED_SIZE] = {0};
    bvb_wire_put_u32(payload, command->index_count);
    bvb_wire_put_u32(payload + 4, command->instance_count);
    bvb_wire_put_u32(payload + 8, command->first_index);
    bvb_wire_put_u32(payload + 12, (uint32_t)command->vertex_offset);
    bvb_wire_put_u32(payload + 16, command->first_instance);
    return append_record(builder, BVB_COMMAND_VULKAN_DRAW_INDEXED,
                         payload, sizeof(payload));
}

int bvb_command_batch_append_vulkan_draw_indirect(
    struct bvb_command_batch_builder *builder, uint16_t opcode,
    const struct bvb_vulkan_draw_indirect_command *command) {
    const bool indexed = opcode == BVB_COMMAND_VULKAN_DRAW_INDEXED_INDIRECT;
    if ((!indexed && opcode != BVB_COMMAND_VULKAN_DRAW_INDIRECT) ||
        command == NULL ||
        bvb_handle_expect(command->buffer_id, BVB_OBJECT_BUFFER) != 0 ||
        (command->stride & 3U) != 0U ||
        (command->draw_count > 1U &&
         command->stride < (indexed ? 20U : 16U)))
        return -EINVAL;
    uint8_t payload[BVB_VULKAN_DRAW_INDIRECT_SIZE] = {0};
    bvb_wire_put_u64(payload, command->buffer_id);
    bvb_wire_put_u64(payload + 8, command->offset);
    bvb_wire_put_u32(payload + 16, command->draw_count);
    bvb_wire_put_u32(payload + 20, command->stride);
    return append_record(builder, opcode, payload, sizeof(payload));
}

int bvb_command_batch_append_vulkan_draw_indirect_count(
    struct bvb_command_batch_builder *builder, uint16_t opcode,
    const struct bvb_vulkan_draw_indirect_count_command *command) {
    const bool indexed =
        opcode == BVB_COMMAND_VULKAN_DRAW_INDEXED_INDIRECT_COUNT;
    if ((!indexed && opcode != BVB_COMMAND_VULKAN_DRAW_INDIRECT_COUNT) ||
        command == NULL ||
        bvb_handle_expect(command->buffer_id, BVB_OBJECT_BUFFER) != 0 ||
        bvb_handle_expect(command->count_buffer_id, BVB_OBJECT_BUFFER) != 0 ||
        (command->stride & 3U) != 0U ||
        (command->maximum_draw_count > 1U &&
         command->stride < (indexed ? 20U : 16U)))
        return -EINVAL;
    uint8_t payload[BVB_VULKAN_DRAW_INDIRECT_COUNT_SIZE] = {0};
    bvb_wire_put_u64(payload, command->buffer_id);
    bvb_wire_put_u64(payload + 8, command->offset);
    bvb_wire_put_u64(payload + 16, command->count_buffer_id);
    bvb_wire_put_u64(payload + 24, command->count_buffer_offset);
    bvb_wire_put_u32(payload + 32, command->maximum_draw_count);
    bvb_wire_put_u32(payload + 36, command->stride);
    return append_record(builder, opcode, payload, sizeof(payload));
}

int bvb_command_batch_append_vulkan_dynamic_state(
    struct bvb_command_batch_builder *builder,
    const struct bvb_vulkan_dynamic_state_command *command) {
    if (command == NULL ||
        command->value_count > BVB_COMMAND_VULKAN_MAX_DYNAMIC_STATE_VALUES)
        return -EINVAL;
    uint8_t payload[BVB_VULKAN_DYNAMIC_STATE_SIZE] = {0};
    bvb_wire_put_u32(payload, command->kind);
    bvb_wire_put_u32(payload + 4, command->value_count);
    for (uint32_t index = 0U; index < command->value_count; ++index)
        bvb_wire_put_u32(payload + 8U + index * sizeof(uint32_t),
                         command->values[index]);
    if (validate_payload(BVB_COMMAND_VULKAN_DYNAMIC_STATE, payload) != 0)
        return -EINVAL;
    return append_record(builder, BVB_COMMAND_VULKAN_DYNAMIC_STATE,
                         payload, sizeof(payload));
}

int bvb_command_batch_append_vulkan_clear_depth_stencil_image(
    struct bvb_command_batch_builder *builder,
    const struct bvb_vulkan_clear_depth_stencil_image_command *command) {
    if (command == NULL ||
        bvb_handle_expect(command->image_id, BVB_OBJECT_IMAGE) != 0 ||
        (command->image_layout != 1U && command->image_layout != 7U) ||
        command->range_count == 0U ||
        command->range_count > BVB_COMMAND_VULKAN_MAX_CLEAR_RANGES ||
        !float_word_is_finite(command->depth_word))
        return -EINVAL;
    uint8_t payload[BVB_VULKAN_CLEAR_DEPTH_STENCIL_IMAGE_SIZE] = {0};
    bvb_wire_put_u64(payload, command->image_id);
    bvb_wire_put_u32(payload + 8, command->image_layout);
    bvb_wire_put_u32(payload + 12, command->range_count);
    bvb_wire_put_u32(payload + 16, command->depth_word);
    bvb_wire_put_u32(payload + 20, command->stencil);
    for (uint32_t index = 0U; index < command->range_count; ++index) {
        const struct bvb_vulkan_image_subresource_range *range =
            &command->ranges[index];
        if (!image_range_is_valid(range) ||
            (range->aspect_mask & ~UINT32_C(6)) != 0U)
            return -EINVAL;
        encode_image_range(
            payload + 24U + index * BVB_VULKAN_IMAGE_RANGE_SIZE, range);
    }
    return append_record(
        builder, BVB_COMMAND_VULKAN_CLEAR_DEPTH_STENCIL_IMAGE,
        payload, sizeof(payload));
}

int bvb_command_batch_append_vulkan_clear_attachments(
    struct bvb_command_batch_builder *builder,
    const struct bvb_vulkan_clear_attachments_command *command) {
    if (command == NULL || command->attachment_count == 0U ||
        command->attachment_count > BVB_COMMAND_VULKAN_MAX_CLEAR_ATTACHMENTS ||
        command->rect_count == 0U ||
        command->rect_count > BVB_COMMAND_VULKAN_MAX_CLEAR_RECTS)
        return -EINVAL;
    uint8_t payload[BVB_VULKAN_CLEAR_ATTACHMENTS_SIZE] = {0};
    bvb_wire_put_u32(payload, command->attachment_count);
    bvb_wire_put_u32(payload + 4, command->rect_count);
    for (uint32_t index = 0U; index < command->attachment_count; ++index) {
        const struct bvb_vulkan_clear_attachment *attachment =
            &command->attachments[index];
        if (attachment->aspect_mask == 0U ||
            (attachment->aspect_mask & ~UINT32_C(7)) != 0U)
            return -EINVAL;
        uint8_t *output = payload + 8U +
            index * BVB_VULKAN_CLEAR_ATTACHMENT_SIZE;
        bvb_wire_put_u32(output, attachment->aspect_mask);
        bvb_wire_put_u32(output + 4, attachment->color_attachment);
        for (uint32_t word = 0U; word < 4U; ++word)
            bvb_wire_put_u32(output + 8U + word * sizeof(uint32_t),
                             attachment->clear_words[word]);
    }
    const size_t rect_base = 8U + BVB_COMMAND_VULKAN_MAX_CLEAR_ATTACHMENTS *
        BVB_VULKAN_CLEAR_ATTACHMENT_SIZE;
    for (uint32_t index = 0U; index < command->rect_count; ++index) {
        const struct bvb_vulkan_clear_rect *rect = &command->rects[index];
        if (rect->width == 0U || rect->height == 0U ||
            rect->layer_count == 0U)
            return -EINVAL;
        uint8_t *output = payload + rect_base +
            index * BVB_VULKAN_CLEAR_RECT_SIZE;
        bvb_wire_put_u32(output, (uint32_t)rect->offset_x);
        bvb_wire_put_u32(output + 4, (uint32_t)rect->offset_y);
        bvb_wire_put_u32(output + 8, rect->width);
        bvb_wire_put_u32(output + 12, rect->height);
        bvb_wire_put_u32(output + 16, rect->base_array_layer);
        bvb_wire_put_u32(output + 20, rect->layer_count);
    }
    return append_record(
        builder, BVB_COMMAND_VULKAN_CLEAR_ATTACHMENTS,
        payload, sizeof(payload));
}

int bvb_command_batch_append_record(
    struct bvb_command_batch_builder *builder,
    const struct bvb_command_record *record) {
    if (builder == NULL || record == NULL ||
        !payload_length_is_valid(record->opcode, record->payload,
                                 record->payload_length) ||
        validate_payload(record->opcode, record->payload) != 0) {
        return -EINVAL;
    }
    return append_record(builder, record->opcode, record->payload,
                         record->payload_length);
}

int bvb_command_batch_append_vulkan_end(
    struct bvb_command_batch_builder *builder) {
    return append_record(builder, BVB_COMMAND_VULKAN_END, NULL, 0U);
}

int bvb_command_batch_finish(struct bvb_command_batch_builder *builder,
                             size_t *output_length) {
    if (builder == NULL || builder->bytes == NULL || output_length == NULL ||
        builder->finished || builder->command_count == 0U ||
        builder->length > UINT32_MAX) {
        return -EINVAL;
    }
    uint8_t *header = builder->bytes;
    bvb_wire_put_u32(header, BVB_COMMAND_BATCH_MAGIC);
    bvb_wire_put_u16(header + 4, BVB_COMMAND_BATCH_VERSION);
    bvb_wire_put_u16(header + 6, 0U);
    bvb_wire_put_u32(header + 8, (uint32_t)builder->length);
    bvb_wire_put_u32(header + 12, builder->command_count);
    bvb_wire_put_u64(header + 16, builder->command_buffer_id);
    bvb_wire_put_u64(header + 24, builder->sequence);
    builder->finished = true;
    *output_length = builder->length;
    return 0;
}

static int expected_payload_size(uint16_t opcode, uint32_t *payload_size) {
    if (payload_size == NULL) {
        return -EINVAL;
    }
    switch (opcode) {
        case BVB_COMMAND_BEGIN_RENDERING:
            *payload_size = BVB_BEGIN_RENDERING_SIZE;
            return 0;
        case BVB_COMMAND_BIND_GRAPHICS_PIPELINE:
            *payload_size = BVB_BIND_GRAPHICS_PIPELINE_SIZE;
            return 0;
        case BVB_COMMAND_SET_VIEWPORT:
            *payload_size = BVB_SET_VIEWPORT_SIZE;
            return 0;
        case BVB_COMMAND_SET_SCISSOR:
            *payload_size = BVB_SET_SCISSOR_SIZE;
            return 0;
        case BVB_COMMAND_DRAW:
            *payload_size = BVB_DRAW_SIZE;
            return 0;
        case BVB_COMMAND_END_RENDERING:
            *payload_size = 0U;
            return 0;
        case BVB_COMMAND_FILL_BUFFER:
            *payload_size = BVB_FILL_BUFFER_SIZE;
            return 0;
        case BVB_COMMAND_BUFFER_HOST_READ_BARRIER:
            *payload_size = BVB_BUFFER_HOST_READ_BARRIER_SIZE;
            return 0;
        case BVB_COMMAND_PUSH_ROTATION:
            *payload_size = BVB_PUSH_ROTATION_SIZE;
            return 0;
        case BVB_COMMAND_VULKAN_IMAGE_BARRIER_2:
            *payload_size = BVB_VULKAN_IMAGE_BARRIER_2_MAX_SIZE;
            return 0;
        case BVB_COMMAND_VULKAN_CLEAR_COLOR_IMAGE_GENERAL:
            *payload_size = BVB_VULKAN_CLEAR_COLOR_IMAGE_GENERAL_SIZE;
            return 0;
        case BVB_COMMAND_VULKAN_BIND_DESCRIPTOR_SETS:
            *payload_size = BVB_VULKAN_BIND_DESCRIPTOR_SETS_SIZE;
            return 0;
        case BVB_COMMAND_VULKAN_PUSH_CONSTANTS:
            *payload_size = BVB_VULKAN_PUSH_CONSTANTS_SIZE;
            return 0;
        case BVB_COMMAND_VULKAN_COPY_BUFFER_2:
        case BVB_COMMAND_VULKAN_COPY_BUFFER_TO_IMAGE_2:
        case BVB_COMMAND_VULKAN_COPY_IMAGE_TO_BUFFER_2:
        case BVB_COMMAND_VULKAN_COPY_IMAGE_2:
        case BVB_COMMAND_VULKAN_BLIT_IMAGE_2:
        case BVB_COMMAND_VULKAN_RESOLVE_IMAGE_2:
            *payload_size = BVB_VULKAN_TRANSFER_MAX_SIZE;
            return 0;
        case BVB_COMMAND_VULKAN_BIND_VERTEX_BUFFERS:
        case BVB_COMMAND_VULKAN_BIND_VERTEX_BUFFERS_2:
            *payload_size = BVB_VULKAN_BIND_VERTEX_BUFFERS_SIZE;
            return 0;
        case BVB_COMMAND_VULKAN_BIND_INDEX_BUFFER:
        case BVB_COMMAND_VULKAN_BIND_INDEX_BUFFER_2:
            *payload_size = BVB_VULKAN_BIND_INDEX_BUFFER_SIZE;
            return 0;
        case BVB_COMMAND_VULKAN_DRAW_INDEXED:
            *payload_size = BVB_VULKAN_DRAW_INDEXED_SIZE;
            return 0;
        case BVB_COMMAND_VULKAN_DRAW_INDIRECT:
        case BVB_COMMAND_VULKAN_DRAW_INDEXED_INDIRECT:
            *payload_size = BVB_VULKAN_DRAW_INDIRECT_SIZE;
            return 0;
        case BVB_COMMAND_VULKAN_DRAW_INDIRECT_COUNT:
        case BVB_COMMAND_VULKAN_DRAW_INDEXED_INDIRECT_COUNT:
            *payload_size = BVB_VULKAN_DRAW_INDIRECT_COUNT_SIZE;
            return 0;
        case BVB_COMMAND_VULKAN_DYNAMIC_STATE:
            *payload_size = BVB_VULKAN_DYNAMIC_STATE_SIZE;
            return 0;
        case BVB_COMMAND_VULKAN_CLEAR_DEPTH_STENCIL_IMAGE:
            *payload_size = BVB_VULKAN_CLEAR_DEPTH_STENCIL_IMAGE_SIZE;
            return 0;
        case BVB_COMMAND_VULKAN_CLEAR_ATTACHMENTS:
            *payload_size = BVB_VULKAN_CLEAR_ATTACHMENTS_SIZE;
            return 0;
        case BVB_COMMAND_VULKAN_BEGIN:
            *payload_size = BVB_VULKAN_BEGIN_SIZE;
            return 0;
        case BVB_COMMAND_VULKAN_CLEAR_COLOR_IMAGE:
            *payload_size = BVB_VULKAN_CLEAR_COLOR_IMAGE_SIZE;
            return 0;
        case BVB_COMMAND_VULKAN_INIT_IMAGE_BARRIER:
            *payload_size = BVB_VULKAN_INIT_IMAGE_BARRIER_SIZE;
            return 0;
        case BVB_COMMAND_VULKAN_END:
            *payload_size = 0U;
            return 0;
        default:
            return -EPROTO;
    }
}

static int validate_payload(uint16_t opcode, const uint8_t *payload) {
    switch (opcode) {
        case BVB_COMMAND_BEGIN_RENDERING:
        {
            const uint32_t color_count = bvb_wire_get_u32(payload + 28);
            const uint32_t has_depth = bvb_wire_get_u32(payload + 32);
            const uint32_t has_stencil = bvb_wire_get_u32(payload + 36);
            if (bvb_wire_get_u32(payload + 12) == 0U ||
                bvb_wire_get_u32(payload + 16) == 0U ||
                bvb_wire_get_u32(payload + 20) == 0U ||
                color_count > BVB_COMMAND_VULKAN_MAX_COLOR_ATTACHMENTS ||
                has_depth > 1U || has_stencil > 1U ||
                (color_count == 0U && has_depth == 0U && has_stencil == 0U))
                return -EPROTO;
            for (uint32_t index = 0U;
                 index < BVB_COMMAND_VULKAN_MAX_COLOR_ATTACHMENTS; ++index) {
                const uint8_t *wire = payload +
                    BVB_BEGIN_RENDERING_HEADER_SIZE +
                    index * BVB_RENDERING_ATTACHMENT_SIZE;
                if (index >= color_count) {
                    if (!bytes_are_zero(wire, BVB_RENDERING_ATTACHMENT_SIZE))
                        return -EPROTO;
                    continue;
                }
                const struct bvb_vulkan_rendering_attachment attachment =
                    decode_rendering_attachment(wire);
                if (!rendering_attachment_is_valid(&attachment, false))
                    return -EPROTO;
            }
            const uint8_t *depth = payload + BVB_BEGIN_RENDERING_HEADER_SIZE +
                BVB_COMMAND_VULKAN_MAX_COLOR_ATTACHMENTS *
                    BVB_RENDERING_ATTACHMENT_SIZE;
            const uint8_t *stencil = depth + BVB_RENDERING_ATTACHMENT_SIZE;
            if (has_depth != 0U) {
                const struct bvb_vulkan_rendering_attachment attachment =
                    decode_rendering_attachment(depth);
                if (!rendering_attachment_is_valid(&attachment, true))
                    return -EPROTO;
            } else if (!bytes_are_zero(depth, BVB_RENDERING_ATTACHMENT_SIZE))
                return -EPROTO;
            if (has_stencil != 0U) {
                const struct bvb_vulkan_rendering_attachment attachment =
                    decode_rendering_attachment(stencil);
                if (!rendering_attachment_is_valid(&attachment, true))
                    return -EPROTO;
            } else if (!bytes_are_zero(stencil,
                                       BVB_RENDERING_ATTACHMENT_SIZE))
                return -EPROTO;
            return 0;
        }
        case BVB_COMMAND_BIND_GRAPHICS_PIPELINE:
            if (bvb_handle_expect(bvb_wire_get_u64(payload),
                                  BVB_OBJECT_PIPELINE) != 0 ||
                bvb_wire_get_u32(payload + 8) != 0U ||
                bvb_wire_get_u32(payload + 12) != 0U) {
                return -EPROTO;
            }
            return 0;
        case BVB_COMMAND_SET_VIEWPORT:
            for (size_t index = 0; index < 6U; ++index) {
                if (!float_bits_are_finite(payload + index * 4U)) {
                    return -EPROTO;
                }
            }
            if (get_float(payload + 8) == 0.0F ||
                get_float(payload + 12) == 0.0F ||
                get_float(payload + 16) < 0.0F ||
                get_float(payload + 16) > 1.0F ||
                get_float(payload + 20) < 0.0F ||
                get_float(payload + 20) > 1.0F ||
                get_float(payload + 16) > get_float(payload + 20)) {
                return -EPROTO;
            }
            return 0;
        case BVB_COMMAND_SET_SCISSOR:
            return bvb_wire_get_u32(payload + 8) == 0U ||
                           bvb_wire_get_u32(payload + 12) == 0U
                       ? -EPROTO
                       : 0;
        case BVB_COMMAND_DRAW:
            return bvb_wire_get_u32(payload) == 0U ||
                           bvb_wire_get_u32(payload + 4) == 0U
                       ? -EPROTO
                       : 0;
        case BVB_COMMAND_END_RENDERING:
            return 0;
        case BVB_COMMAND_FILL_BUFFER:
            return bvb_handle_expect(bvb_wire_get_u64(payload),
                                     BVB_OBJECT_BUFFER) != 0 ||
                           bvb_wire_get_u64(payload + 16) == 0U ||
                           (bvb_wire_get_u64(payload + 8) & 3U) != 0U ||
                           (bvb_wire_get_u64(payload + 16) & 3U) != 0U ||
                           bvb_wire_get_u32(payload + 28) != 0U
                       ? -EPROTO
                       : 0;
        case BVB_COMMAND_BUFFER_HOST_READ_BARRIER:
            return bvb_handle_expect(bvb_wire_get_u64(payload),
                                     BVB_OBJECT_BUFFER) != 0 ||
                           bvb_wire_get_u64(payload + 16) == 0U
                       ? -EPROTO
                       : 0;
        case BVB_COMMAND_PUSH_ROTATION:
            return bvb_handle_expect(bvb_wire_get_u64(payload),
                                     BVB_OBJECT_PIPELINE_LAYOUT) != 0 ||
                           !float_bits_are_finite(payload + 8) ||
                           !float_bits_are_finite(payload + 12) ||
                           get_float(payload + 8) < 0.0F ||
                           get_float(payload + 8) >= 6.283186F ||
                           get_float(payload + 12) <= 0.0F
                       ? -EPROTO
                       : 0;
        case BVB_COMMAND_VULKAN_IMAGE_BARRIER_2: {
            const uint32_t memory_count = bvb_wire_get_u32(payload + 4);
            const uint32_t buffer_count = bvb_wire_get_u32(payload + 8);
            const uint32_t image_count = bvb_wire_get_u32(payload + 12);
            if ((bvb_wire_get_u32(payload) & ~UINT32_C(0x6f)) != 0U ||
                memory_count > BVB_COMMAND_VULKAN_MAX_MEMORY_BARRIERS ||
                buffer_count > BVB_COMMAND_VULKAN_MAX_BUFFER_BARRIERS ||
                image_count > BVB_COMMAND_VULKAN_MAX_IMAGE_BARRIERS) {
                return -EPROTO;
            }
            size_t base = BVB_VULKAN_IMAGE_BARRIER_2_HEADER_SIZE;
            base += memory_count *
                BVB_VULKAN_MEMORY_BARRIER_2_RECORD_SIZE;
            for (uint32_t index = 0U; index < buffer_count; ++index) {
                const uint8_t *barrier = payload + base +
                    index * BVB_VULKAN_BUFFER_BARRIER_2_RECORD_SIZE;
                if (bvb_handle_expect(bvb_wire_get_u64(barrier + 40),
                                      BVB_OBJECT_BUFFER) != 0 ||
                    bvb_wire_get_u64(barrier + 56) == 0U)
                    return -EPROTO;
            }
            base += buffer_count *
                BVB_VULKAN_BUFFER_BARRIER_2_RECORD_SIZE;
            for (uint32_t index = 0U; index < image_count; ++index) {
                const uint8_t *barrier = payload + base +
                    index * BVB_VULKAN_IMAGE_BARRIER_2_RECORD_SIZE;
                if (bvb_handle_expect(bvb_wire_get_u64(barrier + 48),
                                      BVB_OBJECT_IMAGE) != 0 ||
                    validate_image_range_wire(barrier + 56) != 0) {
                    return -EPROTO;
                }
            }
            return 0;
        }
        case BVB_COMMAND_VULKAN_CLEAR_COLOR_IMAGE_GENERAL: {
            const uint32_t count = bvb_wire_get_u32(payload + 12);
            if (bvb_handle_expect(bvb_wire_get_u64(payload),
                                  BVB_OBJECT_IMAGE) != 0 ||
                bvb_wire_get_u32(payload + 8) == 0U || count == 0U ||
                count > BVB_COMMAND_VULKAN_MAX_CLEAR_RANGES) {
                return -EPROTO;
            }
            for (uint32_t index = 0U;
                 index < BVB_COMMAND_VULKAN_MAX_CLEAR_RANGES; ++index) {
                const uint8_t *range = payload + 32U +
                    index * BVB_VULKAN_IMAGE_RANGE_SIZE;
                if (index < count) {
                    if (validate_image_range_wire(range) != 0) return -EPROTO;
                } else if (!image_range_wire_is_zero(range)) {
                    return -EPROTO;
                }
            }
            return 0;
        }
        case BVB_COMMAND_VULKAN_BIND_DESCRIPTOR_SETS: {
            const uint32_t set_count = bvb_wire_get_u32(payload + 16);
            const uint32_t dynamic_count = bvb_wire_get_u32(payload + 20);
            if (bvb_handle_expect(bvb_wire_get_u64(payload),
                                  BVB_OBJECT_PIPELINE_LAYOUT) != 0 ||
                bvb_wire_get_u32(payload + 8) > 1U || set_count == 0U ||
                set_count > BVB_COMMAND_VULKAN_MAX_BOUND_DESCRIPTOR_SETS ||
                dynamic_count > BVB_COMMAND_VULKAN_MAX_DYNAMIC_OFFSETS) {
                return -EPROTO;
            }
            for (uint32_t index = 0U;
                 index < BVB_COMMAND_VULKAN_MAX_BOUND_DESCRIPTOR_SETS;
                 ++index) {
                const uint64_t set_id = bvb_wire_get_u64(
                    payload + 24U + index * sizeof(uint64_t));
                if (index < set_count) {
                    if (bvb_handle_expect(set_id,
                                          BVB_OBJECT_DESCRIPTOR_SET) != 0)
                        return -EPROTO;
                } else if (set_id != 0U) return -EPROTO;
            }
            const uint8_t *offsets = payload + 24U +
                BVB_COMMAND_VULKAN_MAX_BOUND_DESCRIPTOR_SETS *
                    sizeof(uint64_t);
            for (uint32_t index = dynamic_count;
                 index < BVB_COMMAND_VULKAN_MAX_DYNAMIC_OFFSETS; ++index) {
                if (bvb_wire_get_u32(offsets + index * sizeof(uint32_t)) != 0U)
                    return -EPROTO;
            }
            return 0;
        }
        case BVB_COMMAND_VULKAN_PUSH_CONSTANTS: {
            const uint32_t size = bvb_wire_get_u32(payload + 16);
            if (bvb_handle_expect(bvb_wire_get_u64(payload),
                                  BVB_OBJECT_PIPELINE_LAYOUT) != 0 ||
                bvb_wire_get_u32(payload + 8) == 0U || size == 0U ||
                size > BVB_COMMAND_VULKAN_MAX_PUSH_CONSTANT_BYTES ||
                (bvb_wire_get_u32(payload + 12) & 3U) != 0U ||
                (size & 3U) != 0U ||
                bvb_wire_get_u32(payload + 12) >
                    BVB_COMMAND_VULKAN_MAX_PUSH_CONSTANT_BYTES - size ||
                bvb_wire_get_u32(payload + 20) != 0U) return -EPROTO;
            for (uint32_t index = size;
                 index < BVB_COMMAND_VULKAN_MAX_PUSH_CONSTANT_BYTES;
                 ++index) {
                if (payload[24U + index] != 0U) return -EPROTO;
            }
            return 0;
        }
        case BVB_COMMAND_VULKAN_COPY_BUFFER_2:
        case BVB_COMMAND_VULKAN_COPY_BUFFER_TO_IMAGE_2:
        case BVB_COMMAND_VULKAN_COPY_IMAGE_TO_BUFFER_2:
        case BVB_COMMAND_VULKAN_COPY_IMAGE_2:
        case BVB_COMMAND_VULKAN_BLIT_IMAGE_2:
        case BVB_COMMAND_VULKAN_RESOLVE_IMAGE_2: {
            const uint32_t count = bvb_wire_get_u32(payload + 28);
            const bool source_buffer =
                opcode == BVB_COMMAND_VULKAN_COPY_BUFFER_2 ||
                opcode == BVB_COMMAND_VULKAN_COPY_BUFFER_TO_IMAGE_2;
            const bool destination_buffer =
                opcode == BVB_COMMAND_VULKAN_COPY_BUFFER_2 ||
                opcode == BVB_COMMAND_VULKAN_COPY_IMAGE_TO_BUFFER_2;
            if (bvb_handle_expect(bvb_wire_get_u64(payload),
                                  source_buffer ? BVB_OBJECT_BUFFER :
                                                  BVB_OBJECT_IMAGE) != 0 ||
                bvb_handle_expect(bvb_wire_get_u64(payload + 8),
                                  destination_buffer ? BVB_OBJECT_BUFFER :
                                                       BVB_OBJECT_IMAGE) != 0 ||
                (!source_buffer && bvb_wire_get_u32(payload + 16) == 0U) ||
                (!destination_buffer &&
                 bvb_wire_get_u32(payload + 20) == 0U) ||
                (opcode == BVB_COMMAND_VULKAN_BLIT_IMAGE_2 &&
                 bvb_wire_get_u32(payload + 24) > 1U) ||
                (opcode != BVB_COMMAND_VULKAN_BLIT_IMAGE_2 &&
                 bvb_wire_get_u32(payload + 24) != 0U) ||
                count == 0U ||
                count > BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS)
                return -EPROTO;
            for (uint32_t index = 0U; index < count; ++index) {
                const uint8_t *wire = payload +
                    BVB_VULKAN_TRANSFER_HEADER_SIZE +
                    index * BVB_VULKAN_TRANSFER_REGION_SIZE;
                if ((opcode == BVB_COMMAND_VULKAN_COPY_BUFFER_2 &&
                     bvb_wire_get_u64(wire + 16) == 0U) ||
                    (opcode != BVB_COMMAND_VULKAN_COPY_BUFFER_2 &&
                     (bvb_wire_get_u32(wire + 112) == 0U ||
                      bvb_wire_get_u32(wire + 116) == 0U ||
                      bvb_wire_get_u32(wire + 120) == 0U)) ||
                    (!source_buffer &&
                     (bvb_wire_get_u32(wire + 32) == 0U ||
                      bvb_wire_get_u32(wire + 44) == 0U)) ||
                    (!destination_buffer &&
                     (bvb_wire_get_u32(wire + 48) == 0U ||
                      bvb_wire_get_u32(wire + 60) == 0U)) ||
                    bvb_wire_get_u32(wire + 124) != 0U)
                    return -EPROTO;
            }
            return 0;
        }
        case BVB_COMMAND_VULKAN_BIND_VERTEX_BUFFERS:
        case BVB_COMMAND_VULKAN_BIND_VERTEX_BUFFERS_2: {
            const uint32_t count = bvb_wire_get_u32(payload + 4);
            const uint32_t has_sizes = bvb_wire_get_u32(payload + 8);
            const uint32_t has_strides = bvb_wire_get_u32(payload + 12);
            if (count == 0U ||
                count > BVB_COMMAND_VULKAN_MAX_VERTEX_BINDINGS ||
                has_sizes > 1U || has_strides > 1U ||
                (opcode == BVB_COMMAND_VULKAN_BIND_VERTEX_BUFFERS &&
                 (has_sizes != 0U || has_strides != 0U)))
                return -EPROTO;
            const size_t ids = 16U;
            const size_t offsets = ids +
                BVB_COMMAND_VULKAN_MAX_VERTEX_BINDINGS * sizeof(uint64_t);
            const size_t sizes = offsets +
                BVB_COMMAND_VULKAN_MAX_VERTEX_BINDINGS * sizeof(uint64_t);
            const size_t strides = sizes +
                BVB_COMMAND_VULKAN_MAX_VERTEX_BINDINGS * sizeof(uint64_t);
            for (uint32_t index = 0U;
                 index < BVB_COMMAND_VULKAN_MAX_VERTEX_BINDINGS; ++index) {
                const uint64_t id = bvb_wire_get_u64(
                    payload + ids + index * sizeof(uint64_t));
                const uint64_t offset = bvb_wire_get_u64(
                    payload + offsets + index * sizeof(uint64_t));
                const uint64_t size = bvb_wire_get_u64(
                    payload + sizes + index * sizeof(uint64_t));
                const uint64_t stride = bvb_wire_get_u64(
                    payload + strides + index * sizeof(uint64_t));
                if (index >= count) {
                    if (id != 0U || offset != 0U || size != 0U ||
                        stride != 0U) return -EPROTO;
                    continue;
                }
                if ((id != 0U &&
                     bvb_handle_expect(id, BVB_OBJECT_BUFFER) != 0) ||
                    (has_sizes == 0U && size != 0U) ||
                    (has_strides == 0U && stride != 0U))
                    return -EPROTO;
            }
            return 0;
        }
        case BVB_COMMAND_VULKAN_BIND_INDEX_BUFFER:
        case BVB_COMMAND_VULKAN_BIND_INDEX_BUFFER_2: {
            const uint64_t id = bvb_wire_get_u64(payload);
            const uint64_t size = bvb_wire_get_u64(payload + 16);
            return (id != 0U &&
                    bvb_handle_expect(id, BVB_OBJECT_BUFFER) != 0) ||
                           (id != 0U && size == 0U) ||
                           (opcode == BVB_COMMAND_VULKAN_BIND_INDEX_BUFFER &&
                            size != UINT64_MAX) ||
                           bvb_wire_get_u32(payload + 28) != 0U
                       ? -EPROTO : 0;
        }
        case BVB_COMMAND_VULKAN_DRAW_INDEXED:
            return bvb_wire_get_u32(payload + 20) != 0U ? -EPROTO : 0;
        case BVB_COMMAND_VULKAN_DRAW_INDIRECT:
        case BVB_COMMAND_VULKAN_DRAW_INDEXED_INDIRECT: {
            const uint32_t count = bvb_wire_get_u32(payload + 16);
            const uint32_t stride = bvb_wire_get_u32(payload + 20);
            const uint32_t minimum =
                opcode == BVB_COMMAND_VULKAN_DRAW_INDEXED_INDIRECT
                    ? 20U : 16U;
            return bvb_handle_expect(bvb_wire_get_u64(payload),
                                     BVB_OBJECT_BUFFER) != 0 ||
                           (stride & 3U) != 0U ||
                           (count > 1U && stride < minimum) ||
                           bvb_wire_get_u64(payload + 24) != 0U
                       ? -EPROTO : 0;
        }
        case BVB_COMMAND_VULKAN_DRAW_INDIRECT_COUNT:
        case BVB_COMMAND_VULKAN_DRAW_INDEXED_INDIRECT_COUNT: {
            const uint32_t count = bvb_wire_get_u32(payload + 32);
            const uint32_t stride = bvb_wire_get_u32(payload + 36);
            const uint32_t minimum =
                opcode == BVB_COMMAND_VULKAN_DRAW_INDEXED_INDIRECT_COUNT
                    ? 20U : 16U;
            return bvb_handle_expect(bvb_wire_get_u64(payload),
                                     BVB_OBJECT_BUFFER) != 0 ||
                           bvb_handle_expect(bvb_wire_get_u64(payload + 16),
                                             BVB_OBJECT_BUFFER) != 0 ||
                           (stride & 3U) != 0U ||
                           (count > 1U && stride < minimum) ||
                           bvb_wire_get_u64(payload + 40) != 0U
                       ? -EPROTO : 0;
        }
        case BVB_COMMAND_VULKAN_DYNAMIC_STATE: {
            const uint32_t kind = bvb_wire_get_u32(payload);
            const uint32_t count = bvb_wire_get_u32(payload + 4);
            uint32_t expected = 0U;
            switch (kind) {
                case BVB_VULKAN_DYNAMIC_STATE_CULL_MODE:
                case BVB_VULKAN_DYNAMIC_STATE_FRONT_FACE:
                case BVB_VULKAN_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY:
                case BVB_VULKAN_DYNAMIC_STATE_DEPTH_TEST_ENABLE:
                case BVB_VULKAN_DYNAMIC_STATE_DEPTH_WRITE_ENABLE:
                case BVB_VULKAN_DYNAMIC_STATE_DEPTH_COMPARE_OP:
                case BVB_VULKAN_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE:
                case BVB_VULKAN_DYNAMIC_STATE_STENCIL_TEST_ENABLE:
                case BVB_VULKAN_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE:
                case BVB_VULKAN_DYNAMIC_STATE_DEPTH_BIAS_ENABLE:
                case BVB_VULKAN_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE:
                case BVB_VULKAN_DYNAMIC_STATE_LINE_WIDTH:
                    expected = 1U;
                    break;
                case BVB_VULKAN_DYNAMIC_STATE_DEPTH_BOUNDS:
                case BVB_VULKAN_DYNAMIC_STATE_STENCIL_COMPARE_MASK:
                case BVB_VULKAN_DYNAMIC_STATE_STENCIL_WRITE_MASK:
                case BVB_VULKAN_DYNAMIC_STATE_STENCIL_REFERENCE:
                    expected = 2U;
                    break;
                case BVB_VULKAN_DYNAMIC_STATE_DEPTH_BIAS:
                    expected = 3U;
                    break;
                case BVB_VULKAN_DYNAMIC_STATE_BLEND_CONSTANTS:
                    expected = 4U;
                    break;
                case BVB_VULKAN_DYNAMIC_STATE_STENCIL_OP:
                    expected = 5U;
                    break;
                default:
                    return -EPROTO;
            }
            if (count != expected) return -EPROTO;
            uint32_t values[BVB_COMMAND_VULKAN_MAX_DYNAMIC_STATE_VALUES] = {0};
            for (uint32_t index = 0U;
                 index < BVB_COMMAND_VULKAN_MAX_DYNAMIC_STATE_VALUES;
                 ++index) {
                values[index] = bvb_wire_get_u32(
                    payload + 8U + index * sizeof(uint32_t));
                if (index >= count && values[index] != 0U) return -EPROTO;
            }
            if ((kind == BVB_VULKAN_DYNAMIC_STATE_CULL_MODE &&
                 (values[0] & ~UINT32_C(3)) != 0U) ||
                (kind == BVB_VULKAN_DYNAMIC_STATE_FRONT_FACE &&
                 values[0] > 1U) ||
                (kind == BVB_VULKAN_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY &&
                 values[0] > 14U) ||
                (kind == BVB_VULKAN_DYNAMIC_STATE_DEPTH_COMPARE_OP &&
                 values[0] > 7U))
                return -EPROTO;
            if ((kind == BVB_VULKAN_DYNAMIC_STATE_DEPTH_TEST_ENABLE ||
                 kind == BVB_VULKAN_DYNAMIC_STATE_DEPTH_WRITE_ENABLE ||
                 kind ==
                     BVB_VULKAN_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE ||
                 kind == BVB_VULKAN_DYNAMIC_STATE_STENCIL_TEST_ENABLE ||
                 kind ==
                     BVB_VULKAN_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE ||
                 kind == BVB_VULKAN_DYNAMIC_STATE_DEPTH_BIAS_ENABLE ||
                 kind ==
                     BVB_VULKAN_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE) &&
                values[0] > 1U)
                return -EPROTO;
            if (kind == BVB_VULKAN_DYNAMIC_STATE_STENCIL_OP &&
                ((values[0] & ~UINT32_C(3)) != 0U || values[0] == 0U ||
                 values[1] > 7U || values[2] > 7U || values[3] > 7U ||
                 values[4] > 7U))
                return -EPROTO;
            if ((kind == BVB_VULKAN_DYNAMIC_STATE_STENCIL_COMPARE_MASK ||
                 kind == BVB_VULKAN_DYNAMIC_STATE_STENCIL_WRITE_MASK ||
                 kind == BVB_VULKAN_DYNAMIC_STATE_STENCIL_REFERENCE) &&
                ((values[0] & ~UINT32_C(3)) != 0U || values[0] == 0U))
                return -EPROTO;
            if ((kind == BVB_VULKAN_DYNAMIC_STATE_DEPTH_BIAS ||
                 kind == BVB_VULKAN_DYNAMIC_STATE_DEPTH_BOUNDS ||
                 kind == BVB_VULKAN_DYNAMIC_STATE_LINE_WIDTH ||
                 kind == BVB_VULKAN_DYNAMIC_STATE_BLEND_CONSTANTS)) {
                for (uint32_t index = 0U; index < count; ++index) {
                    if (!float_word_is_finite(values[index]))
                        return -EPROTO;
                }
            }
            if (kind == BVB_VULKAN_DYNAMIC_STATE_DEPTH_BOUNDS &&
                (get_float(payload + 8) < 0.0F ||
                 get_float(payload + 8) > 1.0F ||
                 get_float(payload + 12) < 0.0F ||
                 get_float(payload + 12) > 1.0F ||
                 get_float(payload + 8) > get_float(payload + 12)))
                return -EPROTO;
            if (kind == BVB_VULKAN_DYNAMIC_STATE_LINE_WIDTH &&
                get_float(payload + 8) <= 0.0F)
                return -EPROTO;
            return 0;
        }
        case BVB_COMMAND_VULKAN_CLEAR_DEPTH_STENCIL_IMAGE: {
            const uint32_t layout = bvb_wire_get_u32(payload + 8);
            const uint32_t count = bvb_wire_get_u32(payload + 12);
            if (bvb_handle_expect(bvb_wire_get_u64(payload),
                                  BVB_OBJECT_IMAGE) != 0 ||
                (layout != 1U && layout != 7U) || count == 0U ||
                count > BVB_COMMAND_VULKAN_MAX_CLEAR_RANGES ||
                !float_word_is_finite(bvb_wire_get_u32(payload + 16)))
                return -EPROTO;
            for (uint32_t index = 0U;
                 index < BVB_COMMAND_VULKAN_MAX_CLEAR_RANGES; ++index) {
                const uint8_t *range = payload + 24U +
                    index * BVB_VULKAN_IMAGE_RANGE_SIZE;
                if (index < count) {
                    const uint32_t aspect = bvb_wire_get_u32(range);
                    if (validate_image_range_wire(range) != 0 ||
                        (aspect & ~UINT32_C(6)) != 0U)
                        return -EPROTO;
                } else if (!image_range_wire_is_zero(range)) {
                    return -EPROTO;
                }
            }
            return 0;
        }
        case BVB_COMMAND_VULKAN_CLEAR_ATTACHMENTS: {
            const uint32_t attachment_count = bvb_wire_get_u32(payload);
            const uint32_t rect_count = bvb_wire_get_u32(payload + 4);
            if (attachment_count == 0U ||
                attachment_count > BVB_COMMAND_VULKAN_MAX_CLEAR_ATTACHMENTS ||
                rect_count == 0U ||
                rect_count > BVB_COMMAND_VULKAN_MAX_CLEAR_RECTS)
                return -EPROTO;
            for (uint32_t index = 0U;
                 index < BVB_COMMAND_VULKAN_MAX_CLEAR_ATTACHMENTS; ++index) {
                const uint8_t *attachment = payload + 8U +
                    index * BVB_VULKAN_CLEAR_ATTACHMENT_SIZE;
                if (index < attachment_count) {
                    const uint32_t aspect = bvb_wire_get_u32(attachment);
                    if (aspect == 0U || (aspect & ~UINT32_C(7)) != 0U)
                        return -EPROTO;
                } else if (!bytes_are_zero(
                               attachment,
                               BVB_VULKAN_CLEAR_ATTACHMENT_SIZE)) {
                    return -EPROTO;
                }
            }
            const size_t rect_base = 8U +
                BVB_COMMAND_VULKAN_MAX_CLEAR_ATTACHMENTS *
                    BVB_VULKAN_CLEAR_ATTACHMENT_SIZE;
            for (uint32_t index = 0U;
                 index < BVB_COMMAND_VULKAN_MAX_CLEAR_RECTS; ++index) {
                const uint8_t *rect = payload + rect_base +
                    index * BVB_VULKAN_CLEAR_RECT_SIZE;
                if (index < rect_count) {
                    if (bvb_wire_get_u32(rect + 8) == 0U ||
                        bvb_wire_get_u32(rect + 12) == 0U ||
                        bvb_wire_get_u32(rect + 20) == 0U)
                        return -EPROTO;
                } else if (!bytes_are_zero(rect,
                                           BVB_VULKAN_CLEAR_RECT_SIZE)) {
                    return -EPROTO;
                }
            }
            return 0;
        }
        case BVB_COMMAND_VULKAN_BEGIN:
            return (bvb_wire_get_u32(payload) & ~1U) != 0U ||
                           bvb_wire_get_u32(payload + 4) != 0U
                       ? -EPROTO
                       : 0;
        case BVB_COMMAND_VULKAN_CLEAR_COLOR_IMAGE:
            return bvb_handle_expect(bvb_wire_get_u64(payload),
                                     BVB_OBJECT_IMAGE) != 0 ||
                           bvb_wire_get_u64(payload + 8) != 0U
                       ? -EPROTO
                       : 0;
        case BVB_COMMAND_VULKAN_INIT_IMAGE_BARRIER: {
            const uint32_t count = bvb_wire_get_u32(payload);
            if (count == 0U ||
                count > BVB_COMMAND_VULKAN_MAX_INIT_IMAGE_BARRIERS ||
                bvb_wire_get_u32(payload + 4) != 0U) {
                return -EPROTO;
            }
            for (uint32_t index = 0U;
                 index < BVB_COMMAND_VULKAN_MAX_INIT_IMAGE_BARRIERS;
                 ++index) {
                const uint64_t image_id =
                    bvb_wire_get_u64(payload + 8U + index * sizeof(uint64_t));
                if (index >= count) {
                    if (image_id != 0U) return -EPROTO;
                    continue;
                }
                if (bvb_handle_expect(image_id, BVB_OBJECT_IMAGE) != 0) {
                    return -EPROTO;
                }
                for (uint32_t earlier = 0U; earlier < index; ++earlier) {
                    if (image_id == bvb_wire_get_u64(
                                        payload + 8U +
                                        earlier * sizeof(uint64_t))) {
                        return -EPROTO;
                    }
                }
            }
            return 0;
        }
        case BVB_COMMAND_VULKAN_END:
            return 0;
        default:
            return -EPROTO;
    }
}

int bvb_command_batch_validate(const uint8_t *bytes, size_t length,
                               struct bvb_command_batch_info *info) {
    if (bytes == NULL || info == NULL || length < BVB_COMMAND_BATCH_HEADER_SIZE ||
        length > BVB_COMMAND_BATCH_MAX_BYTES) {
        return -EINVAL;
    }
    uint32_t byte_length = bvb_wire_get_u32(bytes + 8);
    uint32_t command_count = bvb_wire_get_u32(bytes + 12);
    uint64_t command_buffer_id = bvb_wire_get_u64(bytes + 16);
    uint64_t sequence = bvb_wire_get_u64(bytes + 24);
    if (bvb_wire_get_u32(bytes) != BVB_COMMAND_BATCH_MAGIC ||
        bvb_wire_get_u16(bytes + 4) != BVB_COMMAND_BATCH_VERSION ||
        bvb_wire_get_u16(bytes + 6) != 0U || byte_length != length ||
        command_count == 0U || command_count > BVB_COMMAND_BATCH_MAX_COMMANDS ||
        bvb_handle_expect(command_buffer_id, BVB_OBJECT_COMMAND_BUFFER) != 0 ||
        sequence == 0U) {
        return -EPROTO;
    }

    size_t offset = BVB_COMMAND_BATCH_HEADER_SIZE;
    for (uint32_t index = 0; index < command_count; ++index) {
        if (offset > length || length - offset < BVB_COMMAND_RECORD_HEADER_SIZE) {
            return -EPROTO;
        }
        uint16_t opcode = bvb_wire_get_u16(bytes + offset);
        uint16_t flags = bvb_wire_get_u16(bytes + offset + 2);
        uint32_t payload_length = bvb_wire_get_u32(bytes + offset + 4);
        uint32_t maximum_length;
        if (flags != 0U || expected_payload_size(opcode, &maximum_length) != 0 ||
            payload_length > length - offset - BVB_COMMAND_RECORD_HEADER_SIZE) {
            return -EPROTO;
        }
        const uint8_t *payload =
            bytes + offset + BVB_COMMAND_RECORD_HEADER_SIZE;
        if (!payload_length_is_valid(opcode, payload, payload_length) ||
            validate_payload(opcode, payload) != 0) {
            return -EPROTO;
        }
        offset += BVB_COMMAND_RECORD_HEADER_SIZE + payload_length;
    }
    if (offset != length) {
        return -EPROTO;
    }
    *info = (struct bvb_command_batch_info){
        .command_buffer_id = command_buffer_id,
        .sequence = sequence,
        .command_count = command_count,
        .byte_length = byte_length,
    };
    return 0;
}

int bvb_command_batch_iterator_init(struct bvb_command_batch_iterator *iterator,
                                    const uint8_t *bytes, size_t length) {
    if (iterator == NULL) {
        return -EINVAL;
    }
    struct bvb_command_batch_info info;
    int result = bvb_command_batch_validate(bytes, length, &info);
    if (result != 0) {
        return result;
    }
    *iterator = (struct bvb_command_batch_iterator){
        .bytes = bytes,
        .length = length,
        .offset = BVB_COMMAND_BATCH_HEADER_SIZE,
        .remaining = info.command_count,
    };
    return 0;
}

int bvb_command_batch_next(struct bvb_command_batch_iterator *iterator,
                           struct bvb_command_record *record) {
    if (iterator == NULL || record == NULL || iterator->bytes == NULL ||
        iterator->offset > iterator->length) {
        return -EINVAL;
    }
    if (iterator->remaining == 0U) {
        return 1;
    }
    const uint8_t *header = iterator->bytes + iterator->offset;
    uint32_t payload_length = bvb_wire_get_u32(header + 4);
    *record = (struct bvb_command_record){
        .opcode = bvb_wire_get_u16(header),
        .payload = header + BVB_COMMAND_RECORD_HEADER_SIZE,
        .payload_length = payload_length,
    };
    iterator->offset += BVB_COMMAND_RECORD_HEADER_SIZE + payload_length;
    --iterator->remaining;
    return 0;
}

int bvb_command_batch_snapshot(const uint8_t *source, size_t length,
                               uint8_t **snapshot) {
    if (source == NULL || length == 0U ||
        length > BVB_COMMAND_BATCH_MAX_BYTES || snapshot == NULL) {
        return -EINVAL;
    }
    *snapshot = malloc(length);
    if (*snapshot == NULL) return -ENOMEM;
    memcpy(*snapshot, source, length);
    return 0;
}

int bvb_command_stream_generation_check(
    const struct bvb_command_stream_generation *generations,
    size_t generation_count, uint64_t command_buffer_id, uint64_t sequence,
    size_t *generation_index) {
    if (generations == NULL || generation_count == 0U ||
        generation_index == NULL || sequence == 0U ||
        bvb_handle_expect(command_buffer_id, BVB_OBJECT_COMMAND_BUFFER) != 0) {
        return -EINVAL;
    }
    size_t empty = SIZE_MAX;
    for (size_t index = 0U; index < generation_count; ++index) {
        if (generations[index].command_buffer_id == command_buffer_id) {
            if (sequence <= generations[index].last_sequence) return -ESTALE;
            *generation_index = index;
            return 0;
        }
        if (empty == SIZE_MAX && generations[index].command_buffer_id == 0U) {
            empty = index;
        }
    }
    if (empty == SIZE_MAX) return -ENOSPC;
    *generation_index = empty;
    return 0;
}

int bvb_command_stream_generation_commit(
    struct bvb_command_stream_generation *generations,
    size_t generation_count, size_t generation_index,
    uint64_t command_buffer_id, uint64_t sequence) {
    if (generations == NULL || generation_index >= generation_count ||
        sequence == 0U ||
        bvb_handle_expect(command_buffer_id, BVB_OBJECT_COMMAND_BUFFER) != 0 ||
        (generations[generation_index].command_buffer_id != 0U &&
         generations[generation_index].command_buffer_id != command_buffer_id) ||
        sequence <= generations[generation_index].last_sequence) {
        return -EINVAL;
    }
    generations[generation_index] = (struct bvb_command_stream_generation){
        .command_buffer_id = command_buffer_id,
        .last_sequence = sequence,
    };
    return 0;
}

int bvb_command_stream_generations_apply(
    struct bvb_command_stream_generation *generations,
    size_t generation_count,
    const struct bvb_command_stream_generation_update *updates,
    size_t update_count, bvb_command_stream_generation_live_fn is_live,
    void *user_data, size_t *reclaimed_count) {
    if (reclaimed_count != NULL) *reclaimed_count = 0U;
    if (generations == NULL || generation_count == 0U ||
        (update_count != 0U && updates == NULL) ||
        generation_count > SIZE_MAX / sizeof(*generations)) {
        return -EINVAL;
    }
    if (update_count == 0U) return 0;
    const size_t bytes = generation_count * sizeof(*generations);
    struct bvb_command_stream_generation *shadow = malloc(bytes);
    if (shadow == NULL) return -ENOMEM;
    memcpy(shadow, generations, bytes);
    size_t reclaimed = 0U;
    bool attempted_reclamation = false;
    int result = 0;
    for (size_t update = 0U; result == 0 && update < update_count; ++update) {
        size_t generation_index = SIZE_MAX;
        result = bvb_command_stream_generation_check(
            shadow, generation_count, updates[update].command_buffer_id,
            updates[update].sequence, &generation_index);
        if (result == -ENOSPC && is_live != NULL &&
            !attempted_reclamation) {
            attempted_reclamation = true;
            for (size_t index = 0U; index < generation_count; ++index) {
                if (shadow[index].command_buffer_id != 0U &&
                    !is_live(shadow[index].command_buffer_id, user_data)) {
                    shadow[index] =
                        (struct bvb_command_stream_generation){0};
                    ++reclaimed;
                }
            }
            result = bvb_command_stream_generation_check(
                shadow, generation_count,
                updates[update].command_buffer_id,
                updates[update].sequence, &generation_index);
        }
        if (result == 0) {
            result = bvb_command_stream_generation_commit(
                shadow, generation_count, generation_index,
                updates[update].command_buffer_id,
                updates[update].sequence);
        }
    }
    if (result == 0) {
        memcpy(generations, shadow, bytes);
        if (reclaimed_count != NULL) *reclaimed_count = reclaimed;
    }
    free(shadow);
    return result;
}

int bvb_command_decode_begin_rendering(
    const struct bvb_command_record *record,
    struct bvb_begin_rendering_command *command) {
    if (record == NULL || command == NULL ||
        record->opcode != BVB_COMMAND_BEGIN_RENDERING ||
        record->payload_length != BVB_BEGIN_RENDERING_SIZE) {
        return -EINVAL;
    }
    memset(command, 0, sizeof(*command));
    *command = (struct bvb_begin_rendering_command){
        .flags = bvb_wire_get_u32(record->payload),
        .render_offset_x = (int32_t)bvb_wire_get_u32(record->payload + 4),
        .render_offset_y = (int32_t)bvb_wire_get_u32(record->payload + 8),
        .width = bvb_wire_get_u32(record->payload + 12),
        .height = bvb_wire_get_u32(record->payload + 16),
        .layer_count = bvb_wire_get_u32(record->payload + 20),
        .view_mask = bvb_wire_get_u32(record->payload + 24),
        .color_attachment_count = bvb_wire_get_u32(record->payload + 28),
        .has_depth_attachment = bvb_wire_get_u32(record->payload + 32),
        .has_stencil_attachment = bvb_wire_get_u32(record->payload + 36),
    };
    for (uint32_t index = 0U; index < command->color_attachment_count;
         ++index)
        command->color_attachments[index] = decode_rendering_attachment(
            record->payload + BVB_BEGIN_RENDERING_HEADER_SIZE +
            index * BVB_RENDERING_ATTACHMENT_SIZE);
    if (command->has_depth_attachment != 0U)
        command->depth_attachment = decode_rendering_attachment(
            record->payload + BVB_BEGIN_RENDERING_HEADER_SIZE +
            BVB_COMMAND_VULKAN_MAX_COLOR_ATTACHMENTS *
                BVB_RENDERING_ATTACHMENT_SIZE);
    if (command->has_stencil_attachment != 0U)
        command->stencil_attachment = decode_rendering_attachment(
            record->payload + BVB_BEGIN_RENDERING_HEADER_SIZE +
            (BVB_COMMAND_VULKAN_MAX_COLOR_ATTACHMENTS + 1U) *
                BVB_RENDERING_ATTACHMENT_SIZE);
    return 0;
}

int bvb_command_decode_bind_graphics_pipeline(
    const struct bvb_command_record *record,
    struct bvb_bind_graphics_pipeline_command *command) {
    if (record == NULL || command == NULL ||
        record->opcode != BVB_COMMAND_BIND_GRAPHICS_PIPELINE ||
        record->payload_length != BVB_BIND_GRAPHICS_PIPELINE_SIZE) {
        return -EINVAL;
    }
    command->pipeline_id = bvb_wire_get_u64(record->payload);
    return 0;
}

int bvb_command_decode_push_rotation(
    const struct bvb_command_record *record,
    struct bvb_push_rotation_command *command) {
    if (record == NULL || command == NULL ||
        record->opcode != BVB_COMMAND_PUSH_ROTATION ||
        record->payload_length != BVB_PUSH_ROTATION_SIZE) {
        return -EINVAL;
    }
    *command = (struct bvb_push_rotation_command){
        .pipeline_layout_id = bvb_wire_get_u64(record->payload),
        .angle_radians = get_float(record->payload + 8),
        .aspect_ratio = get_float(record->payload + 12),
    };
    return 0;
}

int bvb_command_decode_set_viewport(const struct bvb_command_record *record,
                                    struct bvb_set_viewport_command *command) {
    if (record == NULL || command == NULL ||
        record->opcode != BVB_COMMAND_SET_VIEWPORT ||
        record->payload_length != BVB_SET_VIEWPORT_SIZE) {
        return -EINVAL;
    }
    *command = (struct bvb_set_viewport_command){
        .x = get_float(record->payload),
        .y = get_float(record->payload + 4),
        .width = get_float(record->payload + 8),
        .height = get_float(record->payload + 12),
        .minimum_depth = get_float(record->payload + 16),
        .maximum_depth = get_float(record->payload + 20),
    };
    return 0;
}

int bvb_command_decode_set_scissor(const struct bvb_command_record *record,
                                   struct bvb_set_scissor_command *command) {
    if (record == NULL || command == NULL ||
        record->opcode != BVB_COMMAND_SET_SCISSOR ||
        record->payload_length != BVB_SET_SCISSOR_SIZE) {
        return -EINVAL;
    }
    *command = (struct bvb_set_scissor_command){
        .x = bvb_wire_get_i32(record->payload),
        .y = bvb_wire_get_i32(record->payload + 4),
        .width = bvb_wire_get_u32(record->payload + 8),
        .height = bvb_wire_get_u32(record->payload + 12),
    };
    return 0;
}

int bvb_command_decode_draw(const struct bvb_command_record *record,
                            struct bvb_draw_command *command) {
    if (record == NULL || command == NULL ||
        record->opcode != BVB_COMMAND_DRAW ||
        record->payload_length != BVB_DRAW_SIZE) {
        return -EINVAL;
    }
    *command = (struct bvb_draw_command){
        .vertex_count = bvb_wire_get_u32(record->payload),
        .instance_count = bvb_wire_get_u32(record->payload + 4),
        .first_vertex = bvb_wire_get_u32(record->payload + 8),
        .first_instance = bvb_wire_get_u32(record->payload + 12),
    };
    return 0;
}

int bvb_command_decode_fill_buffer(const struct bvb_command_record *record,
                                   struct bvb_fill_buffer_command *command) {
    if (record == NULL || command == NULL ||
        record->opcode != BVB_COMMAND_FILL_BUFFER ||
        record->payload_length != BVB_FILL_BUFFER_SIZE) {
        return -EINVAL;
    }
    *command = (struct bvb_fill_buffer_command){
        .buffer_id = bvb_wire_get_u64(record->payload),
        .offset = bvb_wire_get_u64(record->payload + 8),
        .size = bvb_wire_get_u64(record->payload + 16),
        .data = bvb_wire_get_u32(record->payload + 24),
    };
    return 0;
}

int bvb_command_decode_buffer_host_read_barrier(
    const struct bvb_command_record *record,
    struct bvb_buffer_host_read_barrier_command *command) {
    if (record == NULL || command == NULL ||
        record->opcode != BVB_COMMAND_BUFFER_HOST_READ_BARRIER ||
        record->payload_length != BVB_BUFFER_HOST_READ_BARRIER_SIZE) {
        return -EINVAL;
    }
    *command = (struct bvb_buffer_host_read_barrier_command){
        .buffer_id = bvb_wire_get_u64(record->payload),
        .offset = bvb_wire_get_u64(record->payload + 8),
        .size = bvb_wire_get_u64(record->payload + 16),
    };
    return 0;
}

int bvb_command_decode_vulkan_begin(
    const struct bvb_command_record *record,
    struct bvb_vulkan_begin_command *command) {
    if (record == NULL || command == NULL ||
        record->opcode != BVB_COMMAND_VULKAN_BEGIN ||
        record->payload_length != BVB_VULKAN_BEGIN_SIZE) {
        return -EINVAL;
    }
    command->flags = bvb_wire_get_u32(record->payload);
    return 0;
}

int bvb_command_decode_vulkan_clear_color_image(
    const struct bvb_command_record *record,
    struct bvb_vulkan_clear_color_image_command *command) {
    if (record == NULL || command == NULL ||
        record->opcode != BVB_COMMAND_VULKAN_CLEAR_COLOR_IMAGE ||
        record->payload_length != BVB_VULKAN_CLEAR_COLOR_IMAGE_SIZE) {
        return -EINVAL;
    }
    command->image_id = bvb_wire_get_u64(record->payload);
    return 0;
}

int bvb_command_decode_vulkan_init_image_barrier(
    const struct bvb_command_record *record,
    struct bvb_vulkan_init_image_barrier_command *command) {
    if (record == NULL || command == NULL ||
        record->opcode != BVB_COMMAND_VULKAN_INIT_IMAGE_BARRIER ||
        record->payload_length != BVB_VULKAN_INIT_IMAGE_BARRIER_SIZE) {
        return -EINVAL;
    }
    memset(command, 0, sizeof(*command));
    command->image_count = bvb_wire_get_u32(record->payload);
    for (uint32_t index = 0U; index < command->image_count; ++index) {
        command->image_ids[index] = bvb_wire_get_u64(
            record->payload + 8U + index * sizeof(uint64_t));
    }
    return 0;
}

int bvb_command_decode_vulkan_image_barrier_2(
    const struct bvb_command_record *record,
    struct bvb_vulkan_image_barrier_2_command *command) {
    if (record == NULL || command == NULL ||
        record->opcode != BVB_COMMAND_VULKAN_IMAGE_BARRIER_2 ||
        !payload_length_is_valid(record->opcode, record->payload,
                                 record->payload_length)) {
        return -EINVAL;
    }
    memset(command, 0, sizeof(*command));
    command->dependency_flags = bvb_wire_get_u32(record->payload);
    command->memory_count = bvb_wire_get_u32(record->payload + 4);
    command->buffer_count = bvb_wire_get_u32(record->payload + 8);
    command->image_count = bvb_wire_get_u32(record->payload + 12);
    size_t base = BVB_VULKAN_IMAGE_BARRIER_2_HEADER_SIZE;
    for (uint32_t index = 0U; index < command->memory_count; ++index) {
        const uint8_t *input = record->payload + base +
            index * BVB_VULKAN_MEMORY_BARRIER_2_RECORD_SIZE;
        command->memory[index] = (struct bvb_vulkan_memory_barrier_2){
            .source_stage_mask = bvb_wire_get_u64(input),
            .source_access_mask = bvb_wire_get_u64(input + 8),
            .destination_stage_mask = bvb_wire_get_u64(input + 16),
            .destination_access_mask = bvb_wire_get_u64(input + 24),
        };
    }
    base += command->memory_count *
        BVB_VULKAN_MEMORY_BARRIER_2_RECORD_SIZE;
    for (uint32_t index = 0U; index < command->buffer_count; ++index) {
        const uint8_t *input = record->payload + base +
            index * BVB_VULKAN_BUFFER_BARRIER_2_RECORD_SIZE;
        command->buffers[index] = (struct bvb_vulkan_buffer_barrier_2){
            .source_stage_mask = bvb_wire_get_u64(input),
            .source_access_mask = bvb_wire_get_u64(input + 8),
            .destination_stage_mask = bvb_wire_get_u64(input + 16),
            .destination_access_mask = bvb_wire_get_u64(input + 24),
            .source_queue_family_index = bvb_wire_get_u32(input + 32),
            .destination_queue_family_index = bvb_wire_get_u32(input + 36),
            .buffer_id = bvb_wire_get_u64(input + 40),
            .offset = bvb_wire_get_u64(input + 48),
            .size = bvb_wire_get_u64(input + 56),
        };
    }
    base += command->buffer_count *
        BVB_VULKAN_BUFFER_BARRIER_2_RECORD_SIZE;
    for (uint32_t index = 0U; index < command->image_count; ++index) {
        const uint8_t *input = record->payload + base +
            index * BVB_VULKAN_IMAGE_BARRIER_2_RECORD_SIZE;
        command->images[index] = (struct bvb_vulkan_image_barrier_2){
            .source_stage_mask = bvb_wire_get_u64(input),
            .source_access_mask = bvb_wire_get_u64(input + 8),
            .destination_stage_mask = bvb_wire_get_u64(input + 16),
            .destination_access_mask = bvb_wire_get_u64(input + 24),
            .old_layout = bvb_wire_get_u32(input + 32),
            .new_layout = bvb_wire_get_u32(input + 36),
            .source_queue_family_index = bvb_wire_get_u32(input + 40),
            .destination_queue_family_index = bvb_wire_get_u32(input + 44),
            .image_id = bvb_wire_get_u64(input + 48),
            .range = decode_image_range(input + 56),
        };
    }
    return 0;
}

int bvb_command_decode_vulkan_clear_color_image_general(
    const struct bvb_command_record *record,
    struct bvb_vulkan_clear_color_image_general_command *command) {
    if (record == NULL || command == NULL ||
        record->opcode != BVB_COMMAND_VULKAN_CLEAR_COLOR_IMAGE_GENERAL ||
        record->payload_length !=
            BVB_VULKAN_CLEAR_COLOR_IMAGE_GENERAL_SIZE) {
        return -EINVAL;
    }
    memset(command, 0, sizeof(*command));
    command->image_id = bvb_wire_get_u64(record->payload);
    command->image_layout = bvb_wire_get_u32(record->payload + 8);
    command->range_count = bvb_wire_get_u32(record->payload + 12);
    for (size_t index = 0U; index < 4U; ++index) {
        command->color_words[index] = bvb_wire_get_u32(
            record->payload + 16U + index * sizeof(uint32_t));
    }
    for (uint32_t index = 0U; index < command->range_count; ++index) {
        command->ranges[index] = decode_image_range(
            record->payload + 32U + index * BVB_VULKAN_IMAGE_RANGE_SIZE);
    }
    return 0;
}

int bvb_command_decode_vulkan_bind_descriptor_sets(
    const struct bvb_command_record *record,
    struct bvb_vulkan_bind_descriptor_sets_command *command) {
    if (record == NULL || command == NULL ||
        record->opcode != BVB_COMMAND_VULKAN_BIND_DESCRIPTOR_SETS ||
        record->payload_length != BVB_VULKAN_BIND_DESCRIPTOR_SETS_SIZE) {
        return -EINVAL;
    }
    memset(command, 0, sizeof(*command));
    command->pipeline_layout_id = bvb_wire_get_u64(record->payload);
    command->pipeline_bind_point = bvb_wire_get_u32(record->payload + 8);
    command->first_set = bvb_wire_get_u32(record->payload + 12);
    command->descriptor_set_count = bvb_wire_get_u32(record->payload + 16);
    command->dynamic_offset_count = bvb_wire_get_u32(record->payload + 20);
    for (uint32_t index = 0U; index < command->descriptor_set_count; ++index) {
        command->descriptor_set_ids[index] = bvb_wire_get_u64(
            record->payload + 24U + index * sizeof(uint64_t));
    }
    const uint8_t *offsets = record->payload + 24U +
        BVB_COMMAND_VULKAN_MAX_BOUND_DESCRIPTOR_SETS * sizeof(uint64_t);
    for (uint32_t index = 0U; index < command->dynamic_offset_count; ++index) {
        command->dynamic_offsets[index] = bvb_wire_get_u32(
            offsets + index * sizeof(uint32_t));
    }
    return 0;
}

int bvb_command_decode_vulkan_push_constants(
    const struct bvb_command_record *record,
    struct bvb_vulkan_push_constants_command *command) {
    if (record == NULL || command == NULL ||
        record->opcode != BVB_COMMAND_VULKAN_PUSH_CONSTANTS ||
        record->payload_length != BVB_VULKAN_PUSH_CONSTANTS_SIZE) {
        return -EINVAL;
    }
    *command = (struct bvb_vulkan_push_constants_command){
        .pipeline_layout_id = bvb_wire_get_u64(record->payload),
        .stage_flags = bvb_wire_get_u32(record->payload + 8),
        .offset = bvb_wire_get_u32(record->payload + 12),
        .size = bvb_wire_get_u32(record->payload + 16),
    };
    memcpy(command->data, record->payload + 24, command->size);
    return 0;
}

int bvb_command_decode_vulkan_transfer(
    const struct bvb_command_record *record,
    struct bvb_vulkan_transfer_command *command) {
    if (record == NULL || command == NULL ||
        !transfer_opcode_is_valid(record->opcode) ||
        !payload_length_is_valid(record->opcode, record->payload,
                                 record->payload_length))
        return -EINVAL;
    memset(command, 0, sizeof(*command));
    command->source_id = bvb_wire_get_u64(record->payload);
    command->destination_id = bvb_wire_get_u64(record->payload + 8);
    command->source_layout = bvb_wire_get_u32(record->payload + 16);
    command->destination_layout = bvb_wire_get_u32(record->payload + 20);
    command->filter = bvb_wire_get_u32(record->payload + 24);
    command->region_count = bvb_wire_get_u32(record->payload + 28);
    for (uint32_t index = 0U; index < command->region_count; ++index) {
        const uint8_t *wire = record->payload +
            BVB_VULKAN_TRANSFER_HEADER_SIZE +
            index * BVB_VULKAN_TRANSFER_REGION_SIZE;
        struct bvb_vulkan_transfer_region *region =
            &command->regions[index];
        region->source_buffer_offset = bvb_wire_get_u64(wire);
        region->destination_buffer_offset = bvb_wire_get_u64(wire + 8);
        region->size = bvb_wire_get_u64(wire + 16);
        region->buffer_row_length = bvb_wire_get_u32(wire + 24);
        region->buffer_image_height = bvb_wire_get_u32(wire + 28);
        region->source_layers = decode_transfer_layers(wire + 32);
        region->destination_layers = decode_transfer_layers(wire + 48);
        for (uint32_t offset = 0U; offset < 2U; ++offset) {
            region->source_offsets[offset] = (struct bvb_vulkan_offset_3d){
                .x = (int32_t)bvb_wire_get_u32(wire + 64U + offset * 12U),
                .y = (int32_t)bvb_wire_get_u32(wire + 68U + offset * 12U),
                .z = (int32_t)bvb_wire_get_u32(wire + 72U + offset * 12U),
            };
            region->destination_offsets[offset] =
                (struct bvb_vulkan_offset_3d){
                    .x = (int32_t)bvb_wire_get_u32(
                        wire + 88U + offset * 12U),
                    .y = (int32_t)bvb_wire_get_u32(
                        wire + 92U + offset * 12U),
                    .z = (int32_t)bvb_wire_get_u32(
                        wire + 96U + offset * 12U),
                };
        }
        region->extent = (struct bvb_vulkan_extent_3d){
            .width = bvb_wire_get_u32(wire + 112),
            .height = bvb_wire_get_u32(wire + 116),
            .depth = bvb_wire_get_u32(wire + 120),
        };
    }
    return 0;
}

int bvb_command_decode_vulkan_bind_vertex_buffers(
    const struct bvb_command_record *record,
    struct bvb_vulkan_bind_vertex_buffers_command *command) {
    if (record == NULL || command == NULL ||
        (record->opcode != BVB_COMMAND_VULKAN_BIND_VERTEX_BUFFERS &&
         record->opcode != BVB_COMMAND_VULKAN_BIND_VERTEX_BUFFERS_2) ||
        record->payload_length != BVB_VULKAN_BIND_VERTEX_BUFFERS_SIZE)
        return -EINVAL;
    memset(command, 0, sizeof(*command));
    command->first_binding = bvb_wire_get_u32(record->payload);
    command->binding_count = bvb_wire_get_u32(record->payload + 4);
    command->has_sizes = bvb_wire_get_u32(record->payload + 8);
    command->has_strides = bvb_wire_get_u32(record->payload + 12);
    const size_t ids = 16U;
    const size_t offsets = ids +
        BVB_COMMAND_VULKAN_MAX_VERTEX_BINDINGS * sizeof(uint64_t);
    const size_t sizes = offsets +
        BVB_COMMAND_VULKAN_MAX_VERTEX_BINDINGS * sizeof(uint64_t);
    const size_t strides = sizes +
        BVB_COMMAND_VULKAN_MAX_VERTEX_BINDINGS * sizeof(uint64_t);
    for (uint32_t index = 0U; index < command->binding_count; ++index) {
        command->buffer_ids[index] = bvb_wire_get_u64(
            record->payload + ids + index * sizeof(uint64_t));
        command->offsets[index] = bvb_wire_get_u64(
            record->payload + offsets + index * sizeof(uint64_t));
        command->sizes[index] = bvb_wire_get_u64(
            record->payload + sizes + index * sizeof(uint64_t));
        command->strides[index] = bvb_wire_get_u64(
            record->payload + strides + index * sizeof(uint64_t));
    }
    return 0;
}

int bvb_command_decode_vulkan_bind_index_buffer(
    const struct bvb_command_record *record,
    struct bvb_vulkan_bind_index_buffer_command *command) {
    if (record == NULL || command == NULL ||
        (record->opcode != BVB_COMMAND_VULKAN_BIND_INDEX_BUFFER &&
         record->opcode != BVB_COMMAND_VULKAN_BIND_INDEX_BUFFER_2) ||
        record->payload_length != BVB_VULKAN_BIND_INDEX_BUFFER_SIZE)
        return -EINVAL;
    *command = (struct bvb_vulkan_bind_index_buffer_command){
        .buffer_id = bvb_wire_get_u64(record->payload),
        .offset = bvb_wire_get_u64(record->payload + 8),
        .size = bvb_wire_get_u64(record->payload + 16),
        .index_type = bvb_wire_get_u32(record->payload + 24),
    };
    return 0;
}

int bvb_command_decode_vulkan_draw_indexed(
    const struct bvb_command_record *record,
    struct bvb_vulkan_draw_indexed_command *command) {
    if (record == NULL || command == NULL ||
        record->opcode != BVB_COMMAND_VULKAN_DRAW_INDEXED ||
        record->payload_length != BVB_VULKAN_DRAW_INDEXED_SIZE)
        return -EINVAL;
    *command = (struct bvb_vulkan_draw_indexed_command){
        .index_count = bvb_wire_get_u32(record->payload),
        .instance_count = bvb_wire_get_u32(record->payload + 4),
        .first_index = bvb_wire_get_u32(record->payload + 8),
        .vertex_offset = (int32_t)bvb_wire_get_u32(record->payload + 12),
        .first_instance = bvb_wire_get_u32(record->payload + 16),
    };
    return 0;
}

int bvb_command_decode_vulkan_draw_indirect(
    const struct bvb_command_record *record,
    struct bvb_vulkan_draw_indirect_command *command) {
    if (record == NULL || command == NULL ||
        (record->opcode != BVB_COMMAND_VULKAN_DRAW_INDIRECT &&
         record->opcode != BVB_COMMAND_VULKAN_DRAW_INDEXED_INDIRECT) ||
        record->payload_length != BVB_VULKAN_DRAW_INDIRECT_SIZE)
        return -EINVAL;
    *command = (struct bvb_vulkan_draw_indirect_command){
        .buffer_id = bvb_wire_get_u64(record->payload),
        .offset = bvb_wire_get_u64(record->payload + 8),
        .draw_count = bvb_wire_get_u32(record->payload + 16),
        .stride = bvb_wire_get_u32(record->payload + 20),
    };
    return 0;
}

int bvb_command_decode_vulkan_draw_indirect_count(
    const struct bvb_command_record *record,
    struct bvb_vulkan_draw_indirect_count_command *command) {
    if (record == NULL || command == NULL ||
        (record->opcode != BVB_COMMAND_VULKAN_DRAW_INDIRECT_COUNT &&
         record->opcode !=
             BVB_COMMAND_VULKAN_DRAW_INDEXED_INDIRECT_COUNT) ||
        record->payload_length != BVB_VULKAN_DRAW_INDIRECT_COUNT_SIZE)
        return -EINVAL;
    *command = (struct bvb_vulkan_draw_indirect_count_command){
        .buffer_id = bvb_wire_get_u64(record->payload),
        .offset = bvb_wire_get_u64(record->payload + 8),
        .count_buffer_id = bvb_wire_get_u64(record->payload + 16),
        .count_buffer_offset = bvb_wire_get_u64(record->payload + 24),
        .maximum_draw_count = bvb_wire_get_u32(record->payload + 32),
        .stride = bvb_wire_get_u32(record->payload + 36),
    };
    return 0;
}

int bvb_command_decode_vulkan_dynamic_state(
    const struct bvb_command_record *record,
    struct bvb_vulkan_dynamic_state_command *command) {
    if (record == NULL || command == NULL ||
        record->opcode != BVB_COMMAND_VULKAN_DYNAMIC_STATE ||
        record->payload_length != BVB_VULKAN_DYNAMIC_STATE_SIZE)
        return -EINVAL;
    memset(command, 0, sizeof(*command));
    command->kind = bvb_wire_get_u32(record->payload);
    command->value_count = bvb_wire_get_u32(record->payload + 4);
    if (command->value_count > BVB_COMMAND_VULKAN_MAX_DYNAMIC_STATE_VALUES)
        return -EPROTO;
    for (uint32_t index = 0U; index < command->value_count; ++index)
        command->values[index] = bvb_wire_get_u32(
            record->payload + 8U + index * sizeof(uint32_t));
    return 0;
}

int bvb_command_decode_vulkan_clear_depth_stencil_image(
    const struct bvb_command_record *record,
    struct bvb_vulkan_clear_depth_stencil_image_command *command) {
    if (record == NULL || command == NULL ||
        record->opcode != BVB_COMMAND_VULKAN_CLEAR_DEPTH_STENCIL_IMAGE ||
        record->payload_length != BVB_VULKAN_CLEAR_DEPTH_STENCIL_IMAGE_SIZE)
        return -EINVAL;
    memset(command, 0, sizeof(*command));
    command->image_id = bvb_wire_get_u64(record->payload);
    command->image_layout = bvb_wire_get_u32(record->payload + 8);
    command->range_count = bvb_wire_get_u32(record->payload + 12);
    command->depth_word = bvb_wire_get_u32(record->payload + 16);
    command->stencil = bvb_wire_get_u32(record->payload + 20);
    if (command->range_count > BVB_COMMAND_VULKAN_MAX_CLEAR_RANGES)
        return -EPROTO;
    for (uint32_t index = 0U; index < command->range_count; ++index)
        command->ranges[index] = decode_image_range(
            record->payload + 24U +
            index * BVB_VULKAN_IMAGE_RANGE_SIZE);
    return 0;
}

int bvb_command_decode_vulkan_clear_attachments(
    const struct bvb_command_record *record,
    struct bvb_vulkan_clear_attachments_command *command) {
    if (record == NULL || command == NULL ||
        record->opcode != BVB_COMMAND_VULKAN_CLEAR_ATTACHMENTS ||
        record->payload_length != BVB_VULKAN_CLEAR_ATTACHMENTS_SIZE)
        return -EINVAL;
    memset(command, 0, sizeof(*command));
    command->attachment_count = bvb_wire_get_u32(record->payload);
    command->rect_count = bvb_wire_get_u32(record->payload + 4);
    if (command->attachment_count > BVB_COMMAND_VULKAN_MAX_CLEAR_ATTACHMENTS ||
        command->rect_count > BVB_COMMAND_VULKAN_MAX_CLEAR_RECTS)
        return -EPROTO;
    for (uint32_t index = 0U; index < command->attachment_count; ++index) {
        const uint8_t *input = record->payload + 8U +
            index * BVB_VULKAN_CLEAR_ATTACHMENT_SIZE;
        command->attachments[index].aspect_mask = bvb_wire_get_u32(input);
        command->attachments[index].color_attachment =
            bvb_wire_get_u32(input + 4);
        for (uint32_t word = 0U; word < 4U; ++word)
            command->attachments[index].clear_words[word] =
                bvb_wire_get_u32(input + 8U + word * sizeof(uint32_t));
    }
    const size_t rect_base = 8U + BVB_COMMAND_VULKAN_MAX_CLEAR_ATTACHMENTS *
        BVB_VULKAN_CLEAR_ATTACHMENT_SIZE;
    for (uint32_t index = 0U; index < command->rect_count; ++index) {
        const uint8_t *input = record->payload + rect_base +
            index * BVB_VULKAN_CLEAR_RECT_SIZE;
        command->rects[index] = (struct bvb_vulkan_clear_rect){
            .offset_x = (int32_t)bvb_wire_get_u32(input),
            .offset_y = (int32_t)bvb_wire_get_u32(input + 4),
            .width = bvb_wire_get_u32(input + 8),
            .height = bvb_wire_get_u32(input + 12),
            .base_array_layer = bvb_wire_get_u32(input + 16),
            .layer_count = bvb_wire_get_u32(input + 20),
        };
    }
    return 0;
}
