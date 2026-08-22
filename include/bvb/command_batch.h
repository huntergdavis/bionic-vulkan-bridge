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
    BVB_COMMAND_FILL_BUFFER = 7,
    BVB_COMMAND_BUFFER_HOST_READ_BARRIER = 8,
    BVB_COMMAND_PUSH_ROTATION = 9,
    BVB_COMMAND_VULKAN_IMAGE_BARRIER_2 = 10,
    BVB_COMMAND_VULKAN_CLEAR_COLOR_IMAGE_GENERAL = 11,
    BVB_COMMAND_VULKAN_BIND_DESCRIPTOR_SETS = 12,
    BVB_COMMAND_VULKAN_PUSH_CONSTANTS = 13,
    BVB_COMMAND_VULKAN_BEGIN = 20,
    BVB_COMMAND_VULKAN_CLEAR_COLOR_IMAGE = 21,
    BVB_COMMAND_VULKAN_INIT_IMAGE_BARRIER = 22,
    BVB_COMMAND_VULKAN_END = 23,
    BVB_COMMAND_VULKAN_MAX_IMAGE_BARRIERS = 4,
    BVB_COMMAND_VULKAN_MAX_CLEAR_RANGES = 4,
    BVB_COMMAND_VULKAN_MAX_BOUND_DESCRIPTOR_SETS = 8,
    BVB_COMMAND_VULKAN_MAX_DYNAMIC_OFFSETS = 32,
    BVB_COMMAND_VULKAN_MAX_PUSH_CONSTANT_BYTES = 256,
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

struct bvb_push_rotation_command {
    uint64_t pipeline_layout_id;
    float angle_radians;
    float aspect_ratio;
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

struct bvb_fill_buffer_command {
    uint64_t buffer_id;
    uint64_t offset;
    uint64_t size;
    uint32_t data;
};

struct bvb_buffer_host_read_barrier_command {
    uint64_t buffer_id;
    uint64_t offset;
    uint64_t size;
};

struct bvb_vulkan_begin_command {
    uint32_t flags;
};

struct bvb_vulkan_clear_color_image_command {
    uint64_t image_id;
};

struct bvb_vulkan_init_image_barrier_command {
    uint32_t image_count;
    uint64_t image_ids[BVB_COMMAND_VULKAN_MAX_IMAGE_BARRIERS];
};

struct bvb_vulkan_image_subresource_range {
    uint32_t aspect_mask;
    uint32_t base_mip_level;
    uint32_t level_count;
    uint32_t base_array_layer;
    uint32_t layer_count;
};

struct bvb_vulkan_image_barrier_2 {
    uint64_t source_stage_mask;
    uint64_t source_access_mask;
    uint64_t destination_stage_mask;
    uint64_t destination_access_mask;
    uint32_t old_layout;
    uint32_t new_layout;
    uint32_t source_queue_family_index;
    uint32_t destination_queue_family_index;
    uint64_t image_id;
    struct bvb_vulkan_image_subresource_range range;
};

struct bvb_vulkan_image_barrier_2_command {
    uint32_t dependency_flags;
    uint32_t image_count;
    struct bvb_vulkan_image_barrier_2
        images[BVB_COMMAND_VULKAN_MAX_IMAGE_BARRIERS];
};

struct bvb_vulkan_clear_color_image_general_command {
    uint64_t image_id;
    uint32_t image_layout;
    uint32_t range_count;
    uint32_t color_words[4];
    struct bvb_vulkan_image_subresource_range
        ranges[BVB_COMMAND_VULKAN_MAX_CLEAR_RANGES];
};

struct bvb_vulkan_bind_descriptor_sets_command {
    uint64_t pipeline_layout_id;
    uint32_t pipeline_bind_point;
    uint32_t first_set;
    uint32_t descriptor_set_count;
    uint32_t dynamic_offset_count;
    uint64_t descriptor_set_ids[
        BVB_COMMAND_VULKAN_MAX_BOUND_DESCRIPTOR_SETS];
    uint32_t dynamic_offsets[BVB_COMMAND_VULKAN_MAX_DYNAMIC_OFFSETS];
};

struct bvb_vulkan_push_constants_command {
    uint64_t pipeline_layout_id;
    uint32_t stage_flags;
    uint32_t offset;
    uint32_t size;
    uint8_t data[BVB_COMMAND_VULKAN_MAX_PUSH_CONSTANT_BYTES];
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

struct bvb_command_stream_generation {
    uint64_t command_buffer_id;
    uint64_t last_sequence;
};

struct bvb_command_stream_generation_update {
    uint64_t command_buffer_id;
    uint64_t sequence;
};

typedef bool (*bvb_command_stream_generation_live_fn)(
    uint64_t command_buffer_id, void *user_data);

int bvb_command_batch_begin(struct bvb_command_batch_builder *builder,
                            uint8_t *bytes, size_t capacity,
                            uint64_t command_buffer_id, uint64_t sequence);
int bvb_command_batch_append_begin_rendering(
    struct bvb_command_batch_builder *builder,
    const struct bvb_begin_rendering_command *command);
int bvb_command_batch_append_bind_graphics_pipeline(
    struct bvb_command_batch_builder *builder,
    const struct bvb_bind_graphics_pipeline_command *command);
int bvb_command_batch_append_push_rotation(
    struct bvb_command_batch_builder *builder,
    const struct bvb_push_rotation_command *command);
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
int bvb_command_batch_append_fill_buffer(
    struct bvb_command_batch_builder *builder,
    const struct bvb_fill_buffer_command *command);
int bvb_command_batch_append_buffer_host_read_barrier(
    struct bvb_command_batch_builder *builder,
    const struct bvb_buffer_host_read_barrier_command *command);
int bvb_command_batch_append_vulkan_begin(
    struct bvb_command_batch_builder *builder,
    const struct bvb_vulkan_begin_command *command);
int bvb_command_batch_append_vulkan_clear_color_image(
    struct bvb_command_batch_builder *builder,
    const struct bvb_vulkan_clear_color_image_command *command);
int bvb_command_batch_append_vulkan_init_image_barrier(
    struct bvb_command_batch_builder *builder,
    const struct bvb_vulkan_init_image_barrier_command *command);
int bvb_command_batch_append_vulkan_end(
    struct bvb_command_batch_builder *builder);
int bvb_command_batch_append_vulkan_image_barrier_2(
    struct bvb_command_batch_builder *builder,
    const struct bvb_vulkan_image_barrier_2_command *command);
int bvb_command_batch_append_vulkan_clear_color_image_general(
    struct bvb_command_batch_builder *builder,
    const struct bvb_vulkan_clear_color_image_general_command *command);
int bvb_command_batch_append_vulkan_bind_descriptor_sets(
    struct bvb_command_batch_builder *builder,
    const struct bvb_vulkan_bind_descriptor_sets_command *command);
int bvb_command_batch_append_vulkan_push_constants(
    struct bvb_command_batch_builder *builder,
    const struct bvb_vulkan_push_constants_command *command);
int bvb_command_batch_append_record(
    struct bvb_command_batch_builder *builder,
    const struct bvb_command_record *record);
int bvb_command_batch_finish(struct bvb_command_batch_builder *builder,
                             size_t *output_length);

int bvb_command_batch_validate(const uint8_t *bytes, size_t length,
                               struct bvb_command_batch_info *info);
int bvb_command_batch_iterator_init(struct bvb_command_batch_iterator *iterator,
                                    const uint8_t *bytes, size_t length);
int bvb_command_batch_next(struct bvb_command_batch_iterator *iterator,
                           struct bvb_command_record *record);
int bvb_command_batch_snapshot(const uint8_t *source, size_t length,
                               uint8_t **snapshot);
int bvb_command_stream_generation_check(
    const struct bvb_command_stream_generation *generations,
    size_t generation_count, uint64_t command_buffer_id, uint64_t sequence,
    size_t *generation_index);
int bvb_command_stream_generation_commit(
    struct bvb_command_stream_generation *generations,
    size_t generation_count, size_t generation_index,
    uint64_t command_buffer_id, uint64_t sequence);
int bvb_command_stream_generations_apply(
    struct bvb_command_stream_generation *generations,
    size_t generation_count,
    const struct bvb_command_stream_generation_update *updates,
    size_t update_count, bvb_command_stream_generation_live_fn is_live,
    void *user_data, size_t *reclaimed_count);
int bvb_command_decode_begin_rendering(
    const struct bvb_command_record *record,
    struct bvb_begin_rendering_command *command);
int bvb_command_decode_bind_graphics_pipeline(
    const struct bvb_command_record *record,
    struct bvb_bind_graphics_pipeline_command *command);
int bvb_command_decode_push_rotation(
    const struct bvb_command_record *record,
    struct bvb_push_rotation_command *command);
int bvb_command_decode_set_viewport(const struct bvb_command_record *record,
                                    struct bvb_set_viewport_command *command);
int bvb_command_decode_set_scissor(const struct bvb_command_record *record,
                                   struct bvb_set_scissor_command *command);
int bvb_command_decode_draw(const struct bvb_command_record *record,
                            struct bvb_draw_command *command);
int bvb_command_decode_fill_buffer(const struct bvb_command_record *record,
                                   struct bvb_fill_buffer_command *command);
int bvb_command_decode_buffer_host_read_barrier(
    const struct bvb_command_record *record,
    struct bvb_buffer_host_read_barrier_command *command);
int bvb_command_decode_vulkan_begin(
    const struct bvb_command_record *record,
    struct bvb_vulkan_begin_command *command);
int bvb_command_decode_vulkan_clear_color_image(
    const struct bvb_command_record *record,
    struct bvb_vulkan_clear_color_image_command *command);
int bvb_command_decode_vulkan_init_image_barrier(
    const struct bvb_command_record *record,
    struct bvb_vulkan_init_image_barrier_command *command);
int bvb_command_decode_vulkan_image_barrier_2(
    const struct bvb_command_record *record,
    struct bvb_vulkan_image_barrier_2_command *command);
int bvb_command_decode_vulkan_clear_color_image_general(
    const struct bvb_command_record *record,
    struct bvb_vulkan_clear_color_image_general_command *command);
int bvb_command_decode_vulkan_bind_descriptor_sets(
    const struct bvb_command_record *record,
    struct bvb_vulkan_bind_descriptor_sets_command *command);
int bvb_command_decode_vulkan_push_constants(
    const struct bvb_command_record *record,
    struct bvb_vulkan_push_constants_command *command);

#endif
