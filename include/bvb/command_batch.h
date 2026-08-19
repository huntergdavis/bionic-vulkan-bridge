#ifndef BVB_COMMAND_BATCH_H
#define BVB_COMMAND_BATCH_H

#include <bvb/handle.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    BVB_COMMAND_BATCH_MAGIC = 0x43425642,
    BVB_COMMAND_BATCH_VERSION = 1,
    BVB_COMMAND_BATCH_HEADER_SIZE = 32,
    BVB_COMMAND_RECORD_HEADER_SIZE = 8,
    BVB_COMMAND_BATCH_MAX_BYTES = 16 * 1024 * 1024,
    BVB_COMMAND_BATCH_MAX_COMMANDS = 1024 * 1024,
    BVB_COMMAND_BEGIN_RENDERING = 1,
    BVB_COMMAND_BIND_GRAPHICS_PIPELINE = 2,
    BVB_COMMAND_SET_VIEWPORT = 3,
    BVB_COMMAND_SET_SCISSOR = 4,
    BVB_COMMAND_DRAW = 5,
    BVB_COMMAND_END_RENDERING = 6,
};

struct bvb_begin_rendering_command {
    uint64_t color_image_view_id;
    uint32_t width;
    uint32_t height;
    uint32_t image_layout;
    uint32_t load_op;
    uint32_t store_op;
    uint32_t layer_count;
    float clear_color[4];
};

struct bvb_bind_graphics_pipeline_command {
    uint64_t pipeline_id;
};

struct bvb_set_viewport_command {
    float x;
    float y;
    float width;
    float height;
    float minimum_depth;
    float maximum_depth;
};

struct bvb_set_scissor_command {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
};

struct bvb_draw_command {
    uint32_t vertex_count;
    uint32_t instance_count;
    uint32_t first_vertex;
    uint32_t first_instance;
};

struct bvb_command_batch_builder {
    uint8_t *bytes;
    size_t capacity;
    size_t length;
    uint64_t command_buffer_id;
    uint64_t sequence;
    uint32_t command_count;
    bool finished;
};

struct bvb_command_batch_info {
    uint64_t command_buffer_id;
    uint64_t sequence;
    uint32_t command_count;
    uint32_t byte_length;
};

struct bvb_command_record {
    uint16_t opcode;
    const uint8_t *payload;
    uint32_t payload_length;
};

struct bvb_command_batch_iterator {
    const uint8_t *bytes;
    size_t length;
    size_t offset;
    uint32_t remaining;
};

int bvb_command_batch_begin(struct bvb_command_batch_builder *builder,
                            uint8_t *bytes, size_t capacity,
                            uint64_t command_buffer_id, uint64_t sequence);
int bvb_command_batch_append_begin_rendering(
    struct bvb_command_batch_builder *builder,
    const struct bvb_begin_rendering_command *command);
int bvb_command_batch_append_bind_graphics_pipeline(
    struct bvb_command_batch_builder *builder,
    const struct bvb_bind_graphics_pipeline_command *command);
int bvb_command_batch_append_set_viewport(
    struct bvb_command_batch_builder *builder,
    const struct bvb_set_viewport_command *command);
int bvb_command_batch_append_set_scissor(
    struct bvb_command_batch_builder *builder,
    const struct bvb_set_scissor_command *command);
int bvb_command_batch_append_draw(struct bvb_command_batch_builder *builder,
                                  const struct bvb_draw_command *command);
int bvb_command_batch_append_end_rendering(
    struct bvb_command_batch_builder *builder);
int bvb_command_batch_finish(struct bvb_command_batch_builder *builder,
                             size_t *output_length);

int bvb_command_batch_validate(const uint8_t *bytes, size_t length,
                               struct bvb_command_batch_info *info);
int bvb_command_batch_iterator_init(struct bvb_command_batch_iterator *iterator,
                                    const uint8_t *bytes, size_t length);
int bvb_command_batch_next(struct bvb_command_batch_iterator *iterator,
                           struct bvb_command_record *record);
int bvb_command_decode_begin_rendering(
    const struct bvb_command_record *record,
    struct bvb_begin_rendering_command *command);
int bvb_command_decode_bind_graphics_pipeline(
    const struct bvb_command_record *record,
    struct bvb_bind_graphics_pipeline_command *command);
int bvb_command_decode_set_viewport(const struct bvb_command_record *record,
                                    struct bvb_set_viewport_command *command);
int bvb_command_decode_set_scissor(const struct bvb_command_record *record,
                                   struct bvb_set_scissor_command *command);
int bvb_command_decode_draw(const struct bvb_command_record *record,
                            struct bvb_draw_command *command);

#endif
