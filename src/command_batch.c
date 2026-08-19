#include <bvb/command_batch.h>
#include <bvb/protocol.h>

#include <errno.h>
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
};

_Static_assert(sizeof(float) == sizeof(uint32_t),
               "command batches require 32-bit float");

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
