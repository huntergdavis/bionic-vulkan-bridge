#include <bvb/command_batch.h>
#include <bvb/protocol.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum {
    BVB_BEGIN_RENDERING_SIZE = 48,
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
    BVB_VULKAN_IMAGE_BARRIER_2_RECORD_SIZE = 80,
    BVB_VULKAN_IMAGE_BARRIER_2_SIZE = 8 +
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
    BVB_VULKAN_TRANSFER_SIZE = BVB_VULKAN_TRANSFER_HEADER_SIZE +
        BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS *
            BVB_VULKAN_TRANSFER_REGION_SIZE,
};

_Static_assert(sizeof(float) == sizeof(uint32_t),
               "command batches require 32-bit float");

static int expected_payload_size(uint16_t opcode, uint32_t *payload_size);
static int validate_payload(uint16_t opcode, const uint8_t *payload);

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

static int float_bits_are_finite(const uint8_t *input) {
    return (bvb_wire_get_u32(input) & UINT32_C(0x7f800000)) !=
           UINT32_C(0x7f800000);
}

static int image_range_is_valid(
    const struct bvb_vulkan_image_subresource_range *range) {
    return range != NULL && range->aspect_mask != 0U &&
           range->level_count != 0U && range->layer_count != 0U;
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
        bvb_handle_expect(command->color_image_view_id,
                          BVB_OBJECT_IMAGE_VIEW) != 0) {
        return -EINVAL;
    }
    uint8_t payload[BVB_BEGIN_RENDERING_SIZE];
    bvb_wire_put_u64(payload, command->color_image_view_id);
    bvb_wire_put_u32(payload + 8, command->width);
    bvb_wire_put_u32(payload + 12, command->height);
    bvb_wire_put_u32(payload + 16, command->image_layout);
    bvb_wire_put_u32(payload + 20, command->load_op);
    bvb_wire_put_u32(payload + 24, command->store_op);
    bvb_wire_put_u32(payload + 28, command->layer_count);
    for (size_t index = 0; index < 4U; ++index) {
        put_float(payload + 32U + index * 4U, command->clear_color[index]);
        if (!float_bits_are_finite(payload + 32U + index * 4U)) {
            return -EINVAL;
        }
    }
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
        command->image_count > BVB_COMMAND_VULKAN_MAX_IMAGE_BARRIERS) {
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
    if (command == NULL || command->dependency_flags != 0U ||
        command->image_count == 0U ||
        command->image_count > BVB_COMMAND_VULKAN_MAX_IMAGE_BARRIERS) {
        return -EINVAL;
    }
    uint8_t payload[BVB_VULKAN_IMAGE_BARRIER_2_SIZE];
    memset(payload, 0, sizeof(payload));
    bvb_wire_put_u32(payload, command->dependency_flags);
    bvb_wire_put_u32(payload + 4, command->image_count);
    for (uint32_t index = 0U; index < command->image_count; ++index) {
        const struct bvb_vulkan_image_barrier_2 *barrier =
            &command->images[index];
        if (bvb_handle_expect(barrier->image_id, BVB_OBJECT_IMAGE) != 0 ||
            !image_range_is_valid(&barrier->range)) {
            return -EINVAL;
        }
        uint8_t *record = payload + 8U +
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
                         payload, sizeof(payload));
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
    uint8_t payload[BVB_VULKAN_TRANSFER_SIZE];
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
    return append_record(builder, opcode, payload, sizeof(payload));
}

int bvb_command_batch_append_record(
    struct bvb_command_batch_builder *builder,
    const struct bvb_command_record *record) {
    uint32_t expected = 0U;
    if (builder == NULL || record == NULL ||
        expected_payload_size(record->opcode, &expected) != 0 ||
        expected != record->payload_length ||
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
            *payload_size = BVB_VULKAN_IMAGE_BARRIER_2_SIZE;
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
            *payload_size = BVB_VULKAN_TRANSFER_SIZE;
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
            if (bvb_handle_expect(bvb_wire_get_u64(payload),
                                  BVB_OBJECT_IMAGE_VIEW) != 0 ||
                bvb_wire_get_u32(payload + 8) == 0U ||
                bvb_wire_get_u32(payload + 12) == 0U ||
                bvb_wire_get_u32(payload + 28) == 0U) {
                return -EPROTO;
            }
            for (size_t index = 0; index < 4U; ++index) {
                if (!float_bits_are_finite(payload + 32U + index * 4U)) {
                    return -EPROTO;
                }
            }
            return 0;
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
            const uint32_t count = bvb_wire_get_u32(payload + 4);
            if (bvb_wire_get_u32(payload) != 0U || count == 0U ||
                count > BVB_COMMAND_VULKAN_MAX_IMAGE_BARRIERS) {
                return -EPROTO;
            }
            for (uint32_t index = 0U;
                 index < BVB_COMMAND_VULKAN_MAX_IMAGE_BARRIERS; ++index) {
                const uint8_t *barrier = payload + 8U +
                    index * BVB_VULKAN_IMAGE_BARRIER_2_RECORD_SIZE;
                if (index >= count) {
                    for (size_t offset = 0U;
                         offset < BVB_VULKAN_IMAGE_BARRIER_2_RECORD_SIZE;
                         offset += sizeof(uint32_t)) {
                        if (bvb_wire_get_u32(barrier + offset) != 0U) {
                            return -EPROTO;
                        }
                    }
                    continue;
                }
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
            for (uint32_t index = 0U;
                 index < BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS; ++index) {
                const uint8_t *wire = payload +
                    BVB_VULKAN_TRANSFER_HEADER_SIZE +
                    index * BVB_VULKAN_TRANSFER_REGION_SIZE;
                if (index >= count) {
                    if (!bytes_are_zero(wire,
                                        BVB_VULKAN_TRANSFER_REGION_SIZE))
                        return -EPROTO;
                    continue;
                }
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
                count > BVB_COMMAND_VULKAN_MAX_IMAGE_BARRIERS ||
                bvb_wire_get_u32(payload + 4) != 0U) {
                return -EPROTO;
            }
            for (uint32_t index = 0U;
                 index < BVB_COMMAND_VULKAN_MAX_IMAGE_BARRIERS; ++index) {
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
        uint32_t expected_length;
        if (flags != 0U || expected_payload_size(opcode, &expected_length) != 0 ||
            payload_length != expected_length ||
            payload_length > length - offset - BVB_COMMAND_RECORD_HEADER_SIZE) {
            return -EPROTO;
        }
        const uint8_t *payload =
            bytes + offset + BVB_COMMAND_RECORD_HEADER_SIZE;
        if (validate_payload(opcode, payload) != 0) {
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
    *command = (struct bvb_begin_rendering_command){
        .color_image_view_id = bvb_wire_get_u64(record->payload),
        .width = bvb_wire_get_u32(record->payload + 8),
        .height = bvb_wire_get_u32(record->payload + 12),
        .image_layout = bvb_wire_get_u32(record->payload + 16),
        .load_op = bvb_wire_get_u32(record->payload + 20),
        .store_op = bvb_wire_get_u32(record->payload + 24),
        .layer_count = bvb_wire_get_u32(record->payload + 28),
    };
    for (size_t index = 0; index < 4U; ++index) {
        command->clear_color[index] =
            get_float(record->payload + 32U + index * 4U);
    }
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
        record->payload_length != BVB_VULKAN_IMAGE_BARRIER_2_SIZE) {
        return -EINVAL;
    }
    memset(command, 0, sizeof(*command));
    command->dependency_flags = bvb_wire_get_u32(record->payload);
    command->image_count = bvb_wire_get_u32(record->payload + 4);
    for (uint32_t index = 0U; index < command->image_count; ++index) {
        const uint8_t *input = record->payload + 8U +
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
        record->payload_length != BVB_VULKAN_TRANSFER_SIZE)
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
