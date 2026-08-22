#include <bvb/command_batch.h>
#include <bvb/protocol.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression)                                                        \
    do {                                                                         \
        if (!(expression)) {                                                     \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #expression);                                                \
            return 1;                                                            \
        }                                                                        \
    } while (0)

static bool generation_is_live(uint64_t command_buffer_id, void *user_data) {
    return command_buffer_id == *(const uint64_t *)user_data;
}

static int test_vertex_index_draw_family(void) {
    uint8_t bytes[2048];
    const uint64_t command_buffer =
        bvb_handle_id(BVB_OBJECT_COMMAND_BUFFER, 41U);
    const uint64_t buffer = bvb_handle_id(BVB_OBJECT_BUFFER, 42U);
    struct bvb_command_batch_builder builder;
    CHECK(bvb_command_batch_begin(
              &builder, bytes, sizeof(bytes), command_buffer, 19U) == 0);
    const struct bvb_vulkan_bind_vertex_buffers_command vertex = {
        .first_binding = 2U,
        .binding_count = 2U,
        .has_sizes = 1U,
        .has_strides = 1U,
        .buffer_ids = {buffer, 0U},
        .offsets = {128U, 0U},
        .sizes = {256U, UINT64_MAX},
        .strides = {32U, 0U},
    };
    CHECK(bvb_command_batch_append_vulkan_bind_vertex_buffers(
              &builder, BVB_COMMAND_VULKAN_BIND_VERTEX_BUFFERS_2,
              &vertex) == 0);
    struct bvb_vulkan_bind_vertex_buffers_command legacy_vertex = vertex;
    legacy_vertex.binding_count = 1U;
    legacy_vertex.has_sizes = 0U;
    legacy_vertex.has_strides = 0U;
    CHECK(bvb_command_batch_append_vulkan_bind_vertex_buffers(
              &builder, BVB_COMMAND_VULKAN_BIND_VERTEX_BUFFERS,
              &legacy_vertex) == 0);
    CHECK(bvb_command_batch_append_vulkan_bind_index_buffer(
              &builder, BVB_COMMAND_VULKAN_BIND_INDEX_BUFFER,
              &(const struct bvb_vulkan_bind_index_buffer_command){
                  .buffer_id = buffer, .offset = 32U,
                  .size = UINT64_MAX, .index_type = 0U,
              }) == 0);
    CHECK(bvb_command_batch_append_vulkan_bind_index_buffer(
              &builder, BVB_COMMAND_VULKAN_BIND_INDEX_BUFFER_2,
              &(const struct bvb_vulkan_bind_index_buffer_command){
                  .buffer_id = buffer, .offset = 64U,
                  .size = 512U, .index_type = 1U,
              }) == 0);
    CHECK(bvb_command_batch_append_vulkan_draw_indexed(
              &builder,
              &(const struct bvb_vulkan_draw_indexed_command){
                  .index_count = 6U, .instance_count = 2U,
                  .first_index = 1U, .vertex_offset = -3,
                  .first_instance = 4U,
              }) == 0);
    CHECK(bvb_command_batch_append_vulkan_draw_indirect(
              &builder, BVB_COMMAND_VULKAN_DRAW_INDIRECT,
              &(const struct bvb_vulkan_draw_indirect_command){
                  .buffer_id = buffer, .offset = 128U,
                  .draw_count = 2U, .stride = 16U,
              }) == 0);
    CHECK(bvb_command_batch_append_vulkan_draw_indirect(
              &builder, BVB_COMMAND_VULKAN_DRAW_INDEXED_INDIRECT,
              &(const struct bvb_vulkan_draw_indirect_command){
                  .buffer_id = buffer, .offset = 256U,
                  .draw_count = 3U, .stride = 20U,
              }) == 0);
    CHECK(bvb_command_batch_append_vulkan_draw_indirect_count(
              &builder, BVB_COMMAND_VULKAN_DRAW_INDIRECT_COUNT,
              &(const struct bvb_vulkan_draw_indirect_count_command){
                  .buffer_id = buffer, .offset = 384U,
                  .count_buffer_id = buffer,
                  .count_buffer_offset = 12U,
                  .maximum_draw_count = 4U, .stride = 16U,
              }) == 0);
    CHECK(bvb_command_batch_append_vulkan_draw_indirect_count(
              &builder, BVB_COMMAND_VULKAN_DRAW_INDEXED_INDIRECT_COUNT,
              &(const struct bvb_vulkan_draw_indirect_count_command){
                  .buffer_id = buffer, .offset = 512U,
                  .count_buffer_id = buffer,
                  .count_buffer_offset = 16U,
                  .maximum_draw_count = 5U, .stride = 20U,
              }) == 0);
    size_t length = 0U;
    CHECK(bvb_command_batch_finish(&builder, &length) == 0);
    struct bvb_command_batch_info info;
    CHECK(bvb_command_batch_validate(bytes, length, &info) == 0);
    CHECK(info.command_buffer_id == command_buffer && info.sequence == 19U &&
          info.command_count == 9U && info.byte_length == length);
    struct bvb_command_batch_iterator iterator;
    struct bvb_command_record record;
    CHECK(bvb_command_batch_iterator_init(&iterator, bytes, length) == 0);
    struct bvb_vulkan_bind_vertex_buffers_command decoded_vertex;
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    CHECK(bvb_command_decode_vulkan_bind_vertex_buffers(
              &record, &decoded_vertex) == 0);
    CHECK(decoded_vertex.first_binding == 2U &&
          decoded_vertex.binding_count == 2U &&
          decoded_vertex.buffer_ids[0] == buffer &&
          decoded_vertex.buffer_ids[1] == 0U &&
          decoded_vertex.sizes[0] == 256U &&
          decoded_vertex.strides[0] == 32U);
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    CHECK(bvb_command_decode_vulkan_bind_vertex_buffers(
              &record, &decoded_vertex) == 0);
    CHECK(decoded_vertex.binding_count == 1U &&
          decoded_vertex.has_sizes == 0U &&
          decoded_vertex.has_strides == 0U);
    struct bvb_vulkan_bind_index_buffer_command decoded_index;
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    CHECK(bvb_command_decode_vulkan_bind_index_buffer(
              &record, &decoded_index) == 0);
    CHECK(decoded_index.size == UINT64_MAX && decoded_index.offset == 32U);
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    CHECK(bvb_command_decode_vulkan_bind_index_buffer(
              &record, &decoded_index) == 0);
    CHECK(decoded_index.size == 512U && decoded_index.index_type == 1U);
    struct bvb_vulkan_draw_indexed_command decoded_draw;
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    CHECK(bvb_command_decode_vulkan_draw_indexed(&record, &decoded_draw) == 0);
    CHECK(decoded_draw.vertex_offset == -3 && decoded_draw.index_count == 6U);
    for (uint32_t index = 0U; index < 2U; ++index) {
        struct bvb_vulkan_draw_indirect_command decoded;
        CHECK(bvb_command_batch_next(&iterator, &record) == 0);
        CHECK(bvb_command_decode_vulkan_draw_indirect(&record, &decoded) == 0);
        CHECK(decoded.buffer_id == buffer);
        CHECK(decoded.stride == (index == 0U ? 16U : 20U));
    }
    for (uint32_t index = 0U; index < 2U; ++index) {
        struct bvb_vulkan_draw_indirect_count_command decoded;
        CHECK(bvb_command_batch_next(&iterator, &record) == 0);
        CHECK(bvb_command_decode_vulkan_draw_indirect_count(
                  &record, &decoded) == 0);
        CHECK(decoded.buffer_id == buffer && decoded.count_buffer_id == buffer);
        CHECK(decoded.maximum_draw_count == (index == 0U ? 4U : 5U));
    }
    CHECK(bvb_command_batch_next(&iterator, &record) == 1);

    uint8_t corrupted[sizeof(bytes)];
    memcpy(corrupted, bytes, length);
    /* Inactive vertex slots are canonical zeroes. */
    bvb_wire_put_u64(corrupted + BVB_COMMAND_BATCH_HEADER_SIZE +
                         BVB_COMMAND_RECORD_HEADER_SIZE + 16U +
                         2U * sizeof(uint64_t),
                     buffer);
    CHECK(bvb_command_batch_validate(corrupted, length, &info) == -EPROTO);
    return 0;
}

static int test_dynamic_state_family(void) {
    uint8_t bytes[2048];
    const uint64_t command_buffer =
        bvb_handle_id(BVB_OBJECT_COMMAND_BUFFER, 43U);
    const struct bvb_vulkan_dynamic_state_command commands[] = {
        {.kind = BVB_VULKAN_DYNAMIC_STATE_CULL_MODE,
         .value_count = 1U, .values = {2U}},
        {.kind = BVB_VULKAN_DYNAMIC_STATE_FRONT_FACE,
         .value_count = 1U, .values = {1U}},
        {.kind = BVB_VULKAN_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
         .value_count = 1U, .values = {4U}},
        {.kind = BVB_VULKAN_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
         .value_count = 1U, .values = {1U}},
        {.kind = BVB_VULKAN_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
         .value_count = 1U, .values = {0U}},
        {.kind = BVB_VULKAN_DYNAMIC_STATE_DEPTH_COMPARE_OP,
         .value_count = 1U, .values = {3U}},
        {.kind = BVB_VULKAN_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE,
         .value_count = 1U, .values = {1U}},
        {.kind = BVB_VULKAN_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
         .value_count = 1U, .values = {1U}},
        {.kind = BVB_VULKAN_DYNAMIC_STATE_STENCIL_OP,
         .value_count = 5U, .values = {3U, 2U, 0U, 3U, 7U}},
        {.kind = BVB_VULKAN_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
         .value_count = 1U, .values = {0U}},
        {.kind = BVB_VULKAN_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
         .value_count = 1U, .values = {1U}},
        {.kind = BVB_VULKAN_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
         .value_count = 1U, .values = {1U}},
        {.kind = BVB_VULKAN_DYNAMIC_STATE_DEPTH_BIAS,
         .value_count = 3U,
         .values = {UINT32_C(0x3fa00000), UINT32_C(0x3f000000),
                    UINT32_C(0x40000000)}},
        {.kind = BVB_VULKAN_DYNAMIC_STATE_DEPTH_BOUNDS,
         .value_count = 2U,
         .values = {UINT32_C(0x3e800000), UINT32_C(0x3f400000)}},
        {.kind = BVB_VULKAN_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
         .value_count = 2U, .values = {1U, 0xffU}},
        {.kind = BVB_VULKAN_DYNAMIC_STATE_STENCIL_WRITE_MASK,
         .value_count = 2U, .values = {2U, 0x0fU}},
        {.kind = BVB_VULKAN_DYNAMIC_STATE_STENCIL_REFERENCE,
         .value_count = 2U, .values = {3U, 7U}},
        {.kind = BVB_VULKAN_DYNAMIC_STATE_LINE_WIDTH,
         .value_count = 1U, .values = {UINT32_C(0x3fc00000)}},
        {.kind = BVB_VULKAN_DYNAMIC_STATE_BLEND_CONSTANTS,
         .value_count = 4U,
         .values = {UINT32_C(0x3e000000), UINT32_C(0x3e800000),
                    UINT32_C(0x3f000000), UINT32_C(0x3f800000)}},
    };
    struct bvb_command_batch_builder builder;
    CHECK(bvb_command_batch_begin(
              &builder, bytes, sizeof(bytes), command_buffer, 23U) == 0);
    for (size_t index = 0U;
         index < sizeof(commands) / sizeof(commands[0]); ++index)
        CHECK(bvb_command_batch_append_vulkan_dynamic_state(
                  &builder, &commands[index]) == 0);
    size_t length = 0U;
    CHECK(bvb_command_batch_finish(&builder, &length) == 0);
    struct bvb_command_batch_info info;
    CHECK(bvb_command_batch_validate(bytes, length, &info) == 0);
    CHECK(info.command_buffer_id == command_buffer && info.sequence == 23U &&
          info.command_count == 19U);
    struct bvb_command_batch_iterator iterator;
    struct bvb_command_record record;
    CHECK(bvb_command_batch_iterator_init(&iterator, bytes, length) == 0);
    for (size_t index = 0U;
         index < sizeof(commands) / sizeof(commands[0]); ++index) {
        CHECK(bvb_command_batch_next(&iterator, &record) == 0);
        struct bvb_vulkan_dynamic_state_command decoded;
        CHECK(bvb_command_decode_vulkan_dynamic_state(&record, &decoded) ==
              0);
        CHECK(decoded.kind == commands[index].kind &&
              decoded.value_count == commands[index].value_count &&
              memcmp(decoded.values, commands[index].values,
                     sizeof(decoded.values)) == 0);
    }
    CHECK(bvb_command_batch_next(&iterator, &record) == 1);

    uint8_t corrupted[sizeof(bytes)];
    const size_t first_payload = BVB_COMMAND_BATCH_HEADER_SIZE +
        BVB_COMMAND_RECORD_HEADER_SIZE;
    memcpy(corrupted, bytes, length);
    bvb_wire_put_u32(corrupted + first_payload + 12U, 1U);
    CHECK(bvb_command_batch_validate(corrupted, length, &info) == -EPROTO);
    memcpy(corrupted, bytes, length);
    const size_t depth_bias_payload = first_payload + 12U * 48U;
    bvb_wire_put_u32(corrupted + depth_bias_payload + 8U,
                     UINT32_C(0x7fc00000));
    CHECK(bvb_command_batch_validate(corrupted, length, &info) == -EPROTO);
    return 0;
}

static int test_clear_family(void) {
    uint8_t bytes[1024];
    const uint64_t command_buffer =
        bvb_handle_id(BVB_OBJECT_COMMAND_BUFFER, 44U);
    const uint64_t image = bvb_handle_id(BVB_OBJECT_IMAGE, 45U);
    const struct bvb_vulkan_clear_depth_stencil_image_command depth = {
        .image_id = image,
        .image_layout = 7U,
        .range_count = 2U,
        .depth_word = UINT32_C(0x3f200000),
        .stencil = 9U,
        .ranges = {{.aspect_mask = 2U, .level_count = 1U,
                    .layer_count = 1U},
                   {.aspect_mask = 4U, .base_mip_level = 1U,
                    .level_count = 2U, .base_array_layer = 3U,
                    .layer_count = 4U}},
    };
    const struct bvb_vulkan_clear_attachments_command attachments = {
        .attachment_count = 2U,
        .rect_count = 1U,
        .attachments = {{
            .aspect_mask = 1U,
            .color_attachment = 0U,
            .clear_words = {UINT32_C(0x3e800000), 0U, 0U,
                            UINT32_C(0x3f800000)},
        }, {
            .aspect_mask = 6U,
            .clear_words = {UINT32_C(0x3f000000), 3U, 0U, 0U},
        }},
        .rects = {{.offset_x = -1, .offset_y = 2,
                   .width = 64U, .height = 32U,
                   .layer_count = 1U}},
    };
    struct bvb_command_batch_builder builder;
    CHECK(bvb_command_batch_begin(
              &builder, bytes, sizeof(bytes), command_buffer, 24U) == 0);
    CHECK(bvb_command_batch_append_vulkan_clear_depth_stencil_image(
              &builder, &depth) == 0);
    CHECK(bvb_command_batch_append_vulkan_clear_attachments(
              &builder, &attachments) == 0);
    size_t length = 0U;
    CHECK(bvb_command_batch_finish(&builder, &length) == 0);
    struct bvb_command_batch_info info;
    CHECK(bvb_command_batch_validate(bytes, length, &info) == 0);
    CHECK(info.command_count == 2U && info.sequence == 24U);
    struct bvb_command_batch_iterator iterator;
    struct bvb_command_record record;
    CHECK(bvb_command_batch_iterator_init(&iterator, bytes, length) == 0);
    struct bvb_vulkan_clear_depth_stencil_image_command decoded_depth;
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    CHECK(bvb_command_decode_vulkan_clear_depth_stencil_image(
              &record, &decoded_depth) == 0);
    CHECK(decoded_depth.image_id == image &&
          decoded_depth.range_count == 2U &&
          decoded_depth.ranges[1].base_array_layer == 3U);
    struct bvb_vulkan_clear_attachments_command decoded_attachments;
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    CHECK(bvb_command_decode_vulkan_clear_attachments(
              &record, &decoded_attachments) == 0);
    CHECK(decoded_attachments.attachment_count == 2U &&
          decoded_attachments.rect_count == 1U &&
          decoded_attachments.rects[0].offset_x == -1);
    CHECK(bvb_command_batch_next(&iterator, &record) == 1);

    uint8_t corrupted[sizeof(bytes)];
    memcpy(corrupted, bytes, length);
    const size_t first_payload = BVB_COMMAND_BATCH_HEADER_SIZE +
        BVB_COMMAND_RECORD_HEADER_SIZE;
    bvb_wire_put_u32(corrupted + first_payload + 24U + 2U * 24U, 2U);
    CHECK(bvb_command_batch_validate(corrupted, length, &info) == -EPROTO);
    memcpy(corrupted, bytes, length);
    bvb_wire_put_u32(corrupted + first_payload + 16U,
                     UINT32_C(0x7f800000));
    CHECK(bvb_command_batch_validate(corrupted, length, &info) == -EPROTO);
    return 0;
}

static int test_handles(void) {
    const uint64_t device = bvb_handle_id(BVB_OBJECT_DEVICE, 1U);
    const uint64_t pipeline = bvb_handle_id(BVB_OBJECT_PIPELINE, 9U);
    CHECK(device != 0U);
    CHECK(pipeline != 0U);
    CHECK(bvb_handle_type(pipeline) == BVB_OBJECT_PIPELINE);
    CHECK(bvb_handle_serial(pipeline) == 9U);
    CHECK(bvb_handle_id(BVB_OBJECT_INVALID, 1U) == 0U);
    CHECK(bvb_handle_id(BVB_OBJECT_DEVICE, 0U) == 0U);
    CHECK(bvb_handle_id(BVB_OBJECT_DEVICE, BVB_HANDLE_SERIAL_MASK + 1U) == 0U);
    CHECK(bvb_handle_expect(pipeline, BVB_OBJECT_PIPELINE) == 0);
    CHECK(bvb_handle_expect(pipeline, BVB_OBJECT_DEVICE) == -EINVAL);

    struct bvb_handle_entry entries[8];
    struct bvb_handle_table table;
    CHECK(bvb_handle_table_init(&table, entries, 8U) == 0);
    CHECK(bvb_handle_table_init(&table, entries, 7U) == -EINVAL);
    CHECK(bvb_handle_table_init(&table, entries, 8U) == 0);
    CHECK(bvb_handle_table_insert(&table, pipeline, device, UINT64_C(0xabc)) ==
          0);
    CHECK(bvb_handle_table_insert(&table, pipeline, device, UINT64_C(0xdef)) ==
          -EEXIST);
    uint64_t parent = 0U;
    uint64_t native = 0U;
    CHECK(bvb_handle_table_lookup(&table, pipeline, BVB_OBJECT_PIPELINE,
                                  &parent, &native) == 0);
    CHECK(parent == device);
    CHECK(native == UINT64_C(0xabc));
    CHECK(bvb_handle_table_lookup(&table, pipeline, BVB_OBJECT_DEVICE, NULL,
                                  &native) == -EINVAL);
    CHECK(bvb_handle_table_remove(&table, pipeline, BVB_OBJECT_PIPELINE,
                                  &native) == 0);
    CHECK(native == UINT64_C(0xabc));
    CHECK(table.count == 0U);
    CHECK(bvb_handle_table_lookup(&table, pipeline, BVB_OBJECT_PIPELINE, NULL,
                                  &native) == -ENOENT);
    CHECK(bvb_handle_table_insert(&table, pipeline, device, UINT64_C(0xdef)) ==
          0);
    CHECK(bvb_handle_table_lookup(&table, pipeline, BVB_OBJECT_PIPELINE, NULL,
                                  &native) == 0);
    CHECK(native == UINT64_C(0xdef));
    return 0;
}

static int test_batch(void) {
    uint8_t bytes[1024];
    const uint64_t command_buffer =
        bvb_handle_id(BVB_OBJECT_COMMAND_BUFFER, 3U);
    const uint64_t image_view = bvb_handle_id(BVB_OBJECT_IMAGE_VIEW, 4U);
    const uint64_t resolve_view = bvb_handle_id(BVB_OBJECT_IMAGE_VIEW, 7U);
    const uint64_t depth_view = bvb_handle_id(BVB_OBJECT_IMAGE_VIEW, 8U);
    const uint64_t stencil_view = bvb_handle_id(BVB_OBJECT_IMAGE_VIEW, 9U);
    const uint64_t pipeline = bvb_handle_id(BVB_OBJECT_PIPELINE, 5U);
    const uint64_t pipeline_layout =
        bvb_handle_id(BVB_OBJECT_PIPELINE_LAYOUT, 6U);
    struct bvb_command_batch_builder builder;
    CHECK(bvb_command_batch_begin(&builder, bytes, sizeof(bytes), command_buffer,
                                  11U) == 0);
    CHECK(bvb_command_batch_append_begin_rendering(
              &builder,
              &(const struct bvb_begin_rendering_command){
                  .flags = 5U,
                  .render_offset_x = -2,
                  .render_offset_y = 3,
                  .width = 1280U,
                  .height = 720U,
                  .layer_count = 1U,
                  .view_mask = 3U,
                  .color_attachment_count = 2U,
                  .has_depth_attachment = 1U,
                  .has_stencil_attachment = 1U,
                  .color_attachments = {{
                      .image_view_id = image_view,
                      .resolve_image_view_id = resolve_view,
                      .image_layout = 2U,
                      .resolve_mode = 2U,
                      .resolve_image_layout = 2U,
                      .load_op = 1U,
                      .store_op = 0U,
                      .feedback_loop_enable = 1U,
                      .clear_words = {0U, 0U, 0U, UINT32_C(0x3f800000)},
                  }, {0}},
                  .depth_attachment = {
                      .image_view_id = depth_view,
                      .image_layout = 3U,
                      .load_op = 1U,
                      .store_op = 0U,
                      .clear_words = {UINT32_C(0x3f000000), 7U},
                  },
                  .stencil_attachment = {
                      .image_view_id = stencil_view,
                      .image_layout = 3U,
                      .load_op = 1U,
                      .store_op = 0U,
                      .clear_words = {UINT32_C(0x3f000000), 7U},
                  },
              }) == 0);
    CHECK(bvb_command_batch_append_bind_graphics_pipeline(
              &builder,
              &(const struct bvb_bind_graphics_pipeline_command){
                  .pipeline_id = pipeline,
              }) == 0);
    CHECK(bvb_command_batch_append_push_rotation(
              &builder,
              &(const struct bvb_push_rotation_command){
                  .pipeline_layout_id = pipeline_layout,
                  .angle_radians = 1.25F,
                  .aspect_ratio = 16.0F / 9.0F,
              }) == 0);
    CHECK(bvb_command_batch_append_set_viewport(
              &builder,
              &(const struct bvb_set_viewport_command){
                  .x = 0.0F,
                  .y = 0.0F,
                  .width = 1280.0F,
                  .height = 720.0F,
                  .minimum_depth = 0.0F,
                  .maximum_depth = 1.0F,
              }) == 0);
    CHECK(bvb_command_batch_append_set_scissor(
              &builder,
              &(const struct bvb_set_scissor_command){
                  .x = 0,
                  .y = 0,
                  .width = 1280U,
                  .height = 720U,
              }) == 0);
    CHECK(bvb_command_batch_append_draw(
              &builder,
              &(const struct bvb_draw_command){
                  .vertex_count = 3U,
                  .instance_count = 1U,
                  .first_vertex = 0U,
                  .first_instance = 0U,
              }) == 0);
    CHECK(bvb_command_batch_append_end_rendering(&builder) == 0);
    size_t length = 0U;
    CHECK(bvb_command_batch_finish(&builder, &length) == 0);
    CHECK(bvb_command_batch_finish(&builder, &length) == -EINVAL);

    struct bvb_command_batch_info info;
    CHECK(bvb_command_batch_validate(bytes, length, &info) == 0);
    CHECK(info.command_buffer_id == command_buffer);
    CHECK(info.sequence == 11U);
    CHECK(info.command_count == 7U);
    CHECK(info.byte_length == 776U);
    CHECK(info.byte_length == length);

    struct bvb_command_batch_iterator iterator;
    CHECK(bvb_command_batch_iterator_init(&iterator, bytes, length) == 0);
    struct bvb_command_record record;
    struct bvb_begin_rendering_command begin;
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    CHECK(bvb_command_decode_begin_rendering(&record, &begin) == 0);
    CHECK(begin.flags == 5U && begin.render_offset_x == -2 &&
          begin.render_offset_y == 3 && begin.view_mask == 3U);
    CHECK(begin.color_attachment_count == 2U);
    CHECK(begin.color_attachments[0].image_view_id == image_view);
    CHECK(begin.color_attachments[0].resolve_image_view_id == resolve_view);
    CHECK(begin.color_attachments[0].feedback_loop_enable == 1U);
    CHECK(begin.color_attachments[1].image_view_id == 0U);
    CHECK(begin.has_depth_attachment == 1U &&
          begin.depth_attachment.image_view_id == depth_view);
    CHECK(begin.has_stencil_attachment == 1U &&
          begin.stencil_attachment.image_view_id == stencil_view);
    CHECK(begin.width == 1280U && begin.height == 720U);
    CHECK(begin.color_attachments[0].clear_words[3] ==
          UINT32_C(0x3f800000));

    struct bvb_bind_graphics_pipeline_command bind;
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    CHECK(bvb_command_decode_bind_graphics_pipeline(&record, &bind) == 0);
    CHECK(bind.pipeline_id == pipeline);

    struct bvb_push_rotation_command rotation;
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    CHECK(bvb_command_decode_push_rotation(&record, &rotation) == 0);
    CHECK(rotation.pipeline_layout_id == pipeline_layout);
    CHECK(rotation.angle_radians == 1.25F);
    CHECK(rotation.aspect_ratio == 16.0F / 9.0F);

    struct bvb_set_viewport_command viewport;
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    CHECK(bvb_command_decode_set_viewport(&record, &viewport) == 0);
    CHECK(viewport.width == 1280.0F && viewport.maximum_depth == 1.0F);

    struct bvb_set_scissor_command scissor;
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    CHECK(bvb_command_decode_set_scissor(&record, &scissor) == 0);
    CHECK(scissor.width == 1280U && scissor.height == 720U);

    struct bvb_draw_command draw;
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    CHECK(bvb_command_decode_draw(&record, &draw) == 0);
    CHECK(draw.vertex_count == 3U && draw.instance_count == 1U);

    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    CHECK(record.opcode == BVB_COMMAND_END_RENDERING);
    CHECK(record.payload_length == 0U);
    CHECK(bvb_command_batch_next(&iterator, &record) == 1);

    uint8_t corrupted[sizeof(bytes)];
    memcpy(corrupted, bytes, length);
    corrupted[0] ^= 1U;
    CHECK(bvb_command_batch_validate(corrupted, length, &info) == -EPROTO);
    memcpy(corrupted, bytes, length);
    bvb_wire_put_u32(corrupted + 8, (uint32_t)length - 1U);
    CHECK(bvb_command_batch_validate(corrupted, length, &info) == -EPROTO);
    memcpy(corrupted, bytes, length);
    bvb_wire_put_u16(corrupted + BVB_COMMAND_BATCH_HEADER_SIZE, UINT16_MAX);
    CHECK(bvb_command_batch_validate(corrupted, length, &info) == -EPROTO);

    uint8_t small[40];
    CHECK(bvb_command_batch_begin(&builder, small, sizeof(small), command_buffer,
                                  1U) == 0);
    CHECK(bvb_command_batch_append_draw(
              &builder,
              &(const struct bvb_draw_command){
                  .vertex_count = 3U,
                  .instance_count = 1U,
              }) == -ENOSPC);
    return 0;
}

static int test_transfer_batch(void) {
    uint8_t bytes[256];
    const uint64_t command_buffer =
        bvb_handle_id(BVB_OBJECT_COMMAND_BUFFER, 1U);
    const uint64_t buffer = bvb_handle_id(BVB_OBJECT_BUFFER, 1U);
    struct bvb_command_batch_builder builder;
    CHECK(bvb_command_batch_begin(&builder, bytes, sizeof(bytes), command_buffer,
                                  7U) == 0);
    CHECK(bvb_command_batch_append_fill_buffer(
              &builder,
              &(const struct bvb_fill_buffer_command){
                  .buffer_id = buffer,
                  .offset = 0U,
                  .size = 4096U,
                  .data = UINT32_C(0xa5c3f00d),
              }) == 0);
    CHECK(bvb_command_batch_append_buffer_host_read_barrier(
              &builder,
              &(const struct bvb_buffer_host_read_barrier_command){
                  .buffer_id = buffer,
                  .offset = 0U,
                  .size = 4096U,
              }) == 0);
    size_t length;
    CHECK(bvb_command_batch_finish(&builder, &length) == 0);
    struct bvb_command_batch_info info;
    CHECK(bvb_command_batch_validate(bytes, length, &info) == 0);
    CHECK(info.command_count == 2U);

    struct bvb_command_batch_iterator iterator;
    struct bvb_command_record record;
    CHECK(bvb_command_batch_iterator_init(&iterator, bytes, length) == 0);
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    struct bvb_fill_buffer_command fill;
    CHECK(bvb_command_decode_fill_buffer(&record, &fill) == 0);
    CHECK(fill.buffer_id == buffer && fill.size == 4096U);
    CHECK(fill.data == UINT32_C(0xa5c3f00d));
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    struct bvb_buffer_host_read_barrier_command barrier;
    CHECK(bvb_command_decode_buffer_host_read_barrier(&record, &barrier) == 0);
    CHECK(barrier.buffer_id == buffer && barrier.size == 4096U);
    CHECK(bvb_command_batch_next(&iterator, &record) == 1);
    return 0;
}

static int test_vulkan_transfer_family(void) {
    uint8_t bytes[BVB_PROTOCOL_MAX_PAYLOAD];
    const uint64_t command_buffer =
        bvb_handle_id(BVB_OBJECT_COMMAND_BUFFER, 31U);
    const uint64_t source_buffer = bvb_handle_id(BVB_OBJECT_BUFFER, 32U);
    const uint64_t destination_buffer =
        bvb_handle_id(BVB_OBJECT_BUFFER, 33U);
    const uint64_t source_image = bvb_handle_id(BVB_OBJECT_IMAGE, 34U);
    const uint64_t destination_image =
        bvb_handle_id(BVB_OBJECT_IMAGE, 35U);
    const uint16_t opcodes[] = {
        BVB_COMMAND_VULKAN_COPY_BUFFER_2,
        BVB_COMMAND_VULKAN_COPY_BUFFER_TO_IMAGE_2,
        BVB_COMMAND_VULKAN_COPY_IMAGE_TO_BUFFER_2,
        BVB_COMMAND_VULKAN_COPY_IMAGE_2,
        BVB_COMMAND_VULKAN_BLIT_IMAGE_2,
        BVB_COMMAND_VULKAN_RESOLVE_IMAGE_2,
    };
    for (size_t opcode_index = 0U;
         opcode_index < sizeof(opcodes) / sizeof(opcodes[0]);
         ++opcode_index) {
        const uint16_t opcode = opcodes[opcode_index];
        const bool source_is_buffer =
            opcode == BVB_COMMAND_VULKAN_COPY_BUFFER_2 ||
            opcode == BVB_COMMAND_VULKAN_COPY_BUFFER_TO_IMAGE_2;
        const bool destination_is_buffer =
            opcode == BVB_COMMAND_VULKAN_COPY_BUFFER_2 ||
            opcode == BVB_COMMAND_VULKAN_COPY_IMAGE_TO_BUFFER_2;
        struct bvb_vulkan_transfer_command command = {
            .source_id = source_is_buffer ? source_buffer : source_image,
            .destination_id = destination_is_buffer ? destination_buffer :
                                                        destination_image,
            .source_layout = source_is_buffer ? 0U : 6U,
            .destination_layout = destination_is_buffer ? 0U : 7U,
            .filter = opcode == BVB_COMMAND_VULKAN_BLIT_IMAGE_2 ? 1U : 0U,
            .region_count = 1U,
            .regions = {{
                .source_buffer_offset = 64U,
                .destination_buffer_offset = 128U,
                .size = 256U,
                .buffer_row_length = 32U,
                .buffer_image_height = 16U,
                .source_layers = {.aspect_mask = 1U, .mip_level = 2U,
                                  .base_array_layer = 3U, .layer_count = 1U},
                .destination_layers = {
                    .aspect_mask = 1U, .mip_level = 4U,
                    .base_array_layer = 5U, .layer_count = 1U},
                .source_offsets = {{1, 2, 3}, {11, 12, 13}},
                .destination_offsets = {{4, 5, 6}, {14, 15, 16}},
                .extent = {64U, 32U, 1U},
            }},
        };
        struct bvb_command_batch_builder builder;
        CHECK(bvb_command_batch_begin(
                  &builder, bytes, sizeof(bytes), command_buffer,
                  opcode_index + 1U) == 0);
        CHECK(bvb_command_batch_append_vulkan_transfer(
                  &builder, opcode, &command) == 0);
        size_t length = 0U;
        CHECK(bvb_command_batch_finish(&builder, &length) == 0);
        CHECK(length < BVB_PROTOCOL_MAX_PAYLOAD);
        struct bvb_command_batch_info info;
        CHECK(bvb_command_batch_validate(bytes, length, &info) == 0);
        struct bvb_command_batch_iterator iterator;
        struct bvb_command_record record;
        CHECK(bvb_command_batch_iterator_init(&iterator, bytes, length) == 0);
        CHECK(bvb_command_batch_next(&iterator, &record) == 0);
        CHECK(record.payload_length == 160U);
        struct bvb_vulkan_transfer_command decoded;
        CHECK(bvb_command_decode_vulkan_transfer(&record, &decoded) == 0);
        CHECK(record.opcode == opcode);
        CHECK(decoded.source_id == command.source_id);
        CHECK(decoded.destination_id == command.destination_id);
        CHECK(decoded.region_count == 1U);
        CHECK(decoded.regions[0].source_buffer_offset == 64U);
        CHECK(decoded.regions[0].destination_buffer_offset == 128U);
        CHECK(decoded.regions[0].source_offsets[1].z == 13);
        CHECK(decoded.regions[0].destination_offsets[1].x == 14);
        CHECK(decoded.regions[0].extent.width == 64U);
        CHECK(bvb_command_batch_next(&iterator, &record) == 1);

        uint8_t corrupted[BVB_PROTOCOL_MAX_PAYLOAD];
        memcpy(corrupted, bytes, length);
        const size_t payload = BVB_COMMAND_BATCH_HEADER_SIZE +
            BVB_COMMAND_RECORD_HEADER_SIZE;
        bvb_wire_put_u32(corrupted + payload + 28,
                         BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS + 1U);
        CHECK(bvb_command_batch_validate(corrupted, length, &info) ==
              -EPROTO);
        memcpy(corrupted, bytes, length);
        bvb_wire_put_u32(
            corrupted + BVB_COMMAND_BATCH_HEADER_SIZE + 4U,
            record.payload_length + 128U);
        CHECK(bvb_command_batch_validate(corrupted, length, &info) ==
              -EPROTO);
    }
    return 0;
}

static int test_vulkan_update_buffer(void) {
    const size_t capacity = 96U * 1024U;
    uint8_t *bytes = malloc(capacity);
    uint8_t *corrupted = malloc(capacity);
    struct bvb_vulkan_update_buffer_command *command =
        calloc(1U, sizeof(*command));
    struct bvb_vulkan_update_buffer_command *decoded =
        calloc(1U, sizeof(*decoded));
    CHECK(bytes != NULL && corrupted != NULL && command != NULL &&
          decoded != NULL);
    command->buffer_id = bvb_handle_id(BVB_OBJECT_BUFFER, 36U);
    command->offset = 64U;
    command->data_size = BVB_COMMAND_VULKAN_MAX_UPDATE_BUFFER_BYTES;
    for (uint32_t index = 0U; index < command->data_size; ++index)
        command->data[index] = (uint8_t)(index * 37U);
    struct bvb_command_batch_builder builder;
    CHECK(bvb_command_batch_begin(
              &builder, bytes, capacity,
              bvb_handle_id(BVB_OBJECT_COMMAND_BUFFER, 37U), 1U) == 0);
    CHECK(bvb_command_batch_append_vulkan_update_buffer(
              &builder, command) == 0);
    size_t length = 0U;
    CHECK(bvb_command_batch_finish(&builder, &length) == 0);
    CHECK(length == BVB_COMMAND_BATCH_HEADER_SIZE +
                        BVB_COMMAND_RECORD_HEADER_SIZE + 24U +
                        BVB_COMMAND_VULKAN_MAX_UPDATE_BUFFER_BYTES);
    struct bvb_command_batch_info info;
    CHECK(bvb_command_batch_validate(bytes, length, &info) == 0);
    struct bvb_command_batch_iterator iterator;
    struct bvb_command_record record;
    CHECK(bvb_command_batch_iterator_init(&iterator, bytes, length) == 0);
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    CHECK(bvb_command_decode_vulkan_update_buffer(&record, decoded) == 0);
    CHECK(decoded->buffer_id == command->buffer_id);
    CHECK(decoded->offset == command->offset);
    CHECK(decoded->data_size == command->data_size);
    CHECK(memcmp(decoded->data, command->data, command->data_size) == 0);
    CHECK(bvb_command_batch_next(&iterator, &record) == 1);

    const size_t payload = BVB_COMMAND_BATCH_HEADER_SIZE +
        BVB_COMMAND_RECORD_HEADER_SIZE;
    memcpy(corrupted, bytes, length);
    bvb_wire_put_u32(corrupted + payload + 20U, 1U);
    CHECK(bvb_command_batch_validate(corrupted, length, &info) == -EPROTO);
    memcpy(corrupted, bytes, length);
    bvb_wire_put_u32(corrupted + payload + 16U,
                     BVB_COMMAND_VULKAN_MAX_UPDATE_BUFFER_BYTES - 2U);
    CHECK(bvb_command_batch_validate(corrupted, length, &info) == -EPROTO);
    memcpy(corrupted, bytes, length);
    bvb_wire_put_u64(corrupted + payload,
                     bvb_handle_id(BVB_OBJECT_IMAGE, 36U));
    CHECK(bvb_command_batch_validate(corrupted, length, &info) == -EPROTO);

    free(decoded);
    free(command);
    free(corrupted);
    free(bytes);
    return 0;
}

static int test_compact_transfer_capacity(void) {
    uint8_t bytes[64U * 1024U];
    const uint64_t command_buffer =
        bvb_handle_id(BVB_OBJECT_COMMAND_BUFFER, 36U);
    struct bvb_command_batch_builder builder;
    CHECK(bvb_command_batch_begin(&builder, bytes, sizeof(bytes),
                                  command_buffer, 1U) == 0);
    const struct bvb_vulkan_transfer_command command = {
        .source_id = bvb_handle_id(BVB_OBJECT_BUFFER, 37U),
        .destination_id = bvb_handle_id(BVB_OBJECT_BUFFER, 38U),
        .region_count = 1U,
        .regions = {{
            .source_buffer_offset = 64U,
            .destination_buffer_offset = 128U,
            .size = 256U,
        }},
    };
    for (uint32_t index = 0U; index < 300U; ++index)
        CHECK(bvb_command_batch_append_vulkan_transfer(
                  &builder, BVB_COMMAND_VULKAN_COPY_BUFFER_2,
                  &command) == 0);
    size_t length = 0U;
    CHECK(bvb_command_batch_finish(&builder, &length) == 0);
    CHECK(length == BVB_COMMAND_BATCH_HEADER_SIZE +
                        300U * (BVB_COMMAND_RECORD_HEADER_SIZE + 160U));
    struct bvb_command_batch_info info;
    CHECK(bvb_command_batch_validate(bytes, length, &info) == 0);
    CHECK(info.command_count == 300U);
    return 0;
}

static int test_expanded_stream_slot_capacity(void) {
    const size_t capacity = 256U * 1024U;
    uint8_t *bytes = malloc(capacity);
    CHECK(bytes != NULL);
    const uint64_t command_buffer =
        bvb_handle_id(BVB_OBJECT_COMMAND_BUFFER, 39U);
    struct bvb_command_batch_builder builder;
    CHECK(bvb_command_batch_begin(&builder, bytes, capacity,
                                  command_buffer, 1U) == 0);
    const struct bvb_vulkan_transfer_command command = {
        .source_id = bvb_handle_id(BVB_OBJECT_BUFFER, 40U),
        .destination_id = bvb_handle_id(BVB_OBJECT_BUFFER, 41U),
        .region_count = 1U,
        .regions = {{.size = 256U}},
    };
    for (uint32_t index = 0U; index < 1500U; ++index)
        CHECK(bvb_command_batch_append_vulkan_transfer(
                  &builder, BVB_COMMAND_VULKAN_COPY_BUFFER_2,
                  &command) == 0);
    size_t length = 0U;
    CHECK(bvb_command_batch_finish(&builder, &length) == 0);
    CHECK(length == BVB_COMMAND_BATCH_HEADER_SIZE +
                        1500U * (BVB_COMMAND_RECORD_HEADER_SIZE + 160U));
    struct bvb_command_batch_info info;
    CHECK(bvb_command_batch_validate(bytes, length, &info) == 0);
    CHECK(info.command_count == 1500U);
    free(bytes);
    return 0;
}

static int test_large_image_barrier_batch(void) {
    const uint32_t image_count = 1024U;
    const size_t capacity = 256U * 1024U;
    uint8_t *bytes = malloc(capacity);
    struct bvb_vulkan_image_barrier_2_command *command =
        calloc(1U, sizeof(*command));
    struct bvb_vulkan_image_barrier_2_command *decoded =
        calloc(1U, sizeof(*decoded));
    CHECK(bytes != NULL && command != NULL && decoded != NULL);
    command->image_count = image_count;
    const uint64_t image = bvb_handle_id(BVB_OBJECT_IMAGE, 42U);
    for (uint32_t index = 0U; index < image_count; ++index) {
        command->images[index] = (struct bvb_vulkan_image_barrier_2){
            .source_stage_mask = UINT64_C(0x1000),
            .source_access_mask = UINT64_C(0x1000),
            .destination_stage_mask = UINT64_C(0x2000),
            .destination_access_mask = UINT64_C(0x2000),
            .old_layout = 7U,
            .new_layout = UINT32_C(1000001002),
            .source_queue_family_index = UINT32_MAX,
            .destination_queue_family_index = UINT32_MAX,
            .image_id = image,
            .range = {.aspect_mask = 1U, .level_count = 1U,
                      .layer_count = 1U},
        };
    }
    struct bvb_command_batch_builder builder;
    CHECK(bvb_command_batch_begin(
              &builder, bytes, capacity,
              bvb_handle_id(BVB_OBJECT_COMMAND_BUFFER, 43U), 1U) == 0);
    CHECK(bvb_command_batch_append_vulkan_image_barrier_2(
              &builder, command) == 0);
    size_t length = 0U;
    CHECK(bvb_command_batch_finish(&builder, &length) == 0);
    struct bvb_command_batch_info info;
    CHECK(bvb_command_batch_validate(bytes, length, &info) == 0);
    struct bvb_command_batch_iterator iterator;
    struct bvb_command_record record;
    CHECK(bvb_command_batch_iterator_init(&iterator, bytes, length) == 0);
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    CHECK(record.payload_length == 16U + image_count * 80U);
    CHECK(bvb_command_decode_vulkan_image_barrier_2(&record, decoded) == 0);
    CHECK(decoded->image_count == image_count);
    CHECK(decoded->images[image_count - 1U].image_id == image);
    free(decoded);
    free(command);
    free(bytes);
    return 0;
}

static int test_vulkan_command_stream(void) {
    uint8_t bytes[8192];
    const uint64_t command_buffer =
        bvb_handle_id(BVB_OBJECT_COMMAND_BUFFER, 7U);
    const uint64_t buffer = bvb_handle_id(BVB_OBJECT_BUFFER, 8U);
    const uint64_t image_one = bvb_handle_id(BVB_OBJECT_IMAGE, 9U);
    const uint64_t image_two = bvb_handle_id(BVB_OBJECT_IMAGE, 10U);
    const uint64_t pipeline_layout =
        bvb_handle_id(BVB_OBJECT_PIPELINE_LAYOUT, 11U);
    const uint64_t descriptor_set_one =
        bvb_handle_id(BVB_OBJECT_DESCRIPTOR_SET, 12U);
    const uint64_t descriptor_set_two =
        bvb_handle_id(BVB_OBJECT_DESCRIPTOR_SET, 13U);
    struct bvb_command_batch_builder builder;
    CHECK(bvb_command_batch_begin(&builder, bytes, sizeof(bytes), command_buffer,
                                  17U) == 0);
    CHECK(bvb_command_batch_append_vulkan_begin(
              &builder,
              &(const struct bvb_vulkan_begin_command){.flags = 1U}) == 0);
    CHECK(bvb_command_batch_append_fill_buffer(
              &builder,
              &(const struct bvb_fill_buffer_command){
                  .buffer_id = buffer,
                  .size = 4096U,
                  .data = UINT32_C(0xa5c3f00d),
              }) == 0);
    CHECK(bvb_command_batch_append_vulkan_clear_color_image(
              &builder,
              &(const struct bvb_vulkan_clear_color_image_command){
                  .image_id = image_one,
              }) == 0);
    CHECK(bvb_command_batch_append_vulkan_init_image_barrier(
              &builder,
              &(const struct bvb_vulkan_init_image_barrier_command){
                  .image_count = 2U,
                  .image_ids = {image_one, image_two},
              }) == 0);
    CHECK(bvb_command_batch_append_vulkan_image_barrier_2(
              &builder,
              &(const struct bvb_vulkan_image_barrier_2_command){
                  .dependency_flags = 1U,
                  .memory_count = 1U,
                  .buffer_count = 1U,
                  .image_count = 1U,
                  .memory = {{
                      .source_stage_mask = UINT64_C(0x10),
                      .source_access_mask = UINT64_C(0x20),
                      .destination_stage_mask = UINT64_C(0x40),
                      .destination_access_mask = UINT64_C(0x80),
                  }},
                  .buffers = {{
                      .source_stage_mask = UINT64_C(0x100),
                      .source_access_mask = UINT64_C(0x200),
                      .destination_stage_mask = UINT64_C(0x400),
                      .destination_access_mask = UINT64_C(0x800),
                      .source_queue_family_index = 2U,
                      .destination_queue_family_index = 3U,
                      .buffer_id = buffer,
                      .offset = 256U,
                      .size = 1024U,
                  }},
                  .images = {{
                      .source_stage_mask = UINT64_C(0x1000),
                      .source_access_mask = UINT64_C(0x1000),
                      .destination_stage_mask = UINT64_C(0x2000),
                      .destination_access_mask = UINT64_C(0x2000),
                      .old_layout = 7U,
                      .new_layout = UINT32_C(1000001002),
                      .source_queue_family_index = UINT32_MAX,
                      .destination_queue_family_index = UINT32_MAX,
                      .image_id = image_one,
                      .range = {
                          .aspect_mask = 1U,
                          .level_count = 1U,
                          .layer_count = 1U,
                      },
                  }},
              }) == 0);
    CHECK(bvb_command_batch_append_vulkan_clear_color_image_general(
              &builder,
              &(const struct bvb_vulkan_clear_color_image_general_command){
                  .image_id = image_two,
                  .image_layout = 7U,
                  .color_words = {UINT32_C(0x3f800000), 0U, 0U,
                                  UINT32_C(0x3f800000)},
                  .range_count = 2U,
                  .ranges = {{
                                 .aspect_mask = 1U,
                                 .level_count = 1U,
                                 .layer_count = 1U,
                             },
                             {
                                 .aspect_mask = 1U,
                                 .base_mip_level = 1U,
                                 .level_count = 1U,
                                 .layer_count = 1U,
                             }},
              }) == 0);
    CHECK(bvb_command_batch_append_vulkan_bind_descriptor_sets(
              &builder,
              &(const struct bvb_vulkan_bind_descriptor_sets_command){
                  .pipeline_layout_id = pipeline_layout,
                  .pipeline_bind_point = 1U,
                  .first_set = 2U,
                  .descriptor_set_count = 2U,
                  .dynamic_offset_count = 2U,
                  .descriptor_set_ids = {descriptor_set_one,
                                         descriptor_set_two},
                  .dynamic_offsets = {64U, 128U},
              }) == 0);
    const struct bvb_vulkan_push_constants_command push_constants = {
        .pipeline_layout_id = pipeline_layout,
        .stage_flags = 16U,
        .offset = 4U,
        .size = 16U,
        .data = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U,
                 9U, 10U, 11U, 12U, 13U, 14U, 15U, 16U},
    };
    CHECK(bvb_command_batch_append_vulkan_push_constants(
              &builder, &push_constants) == 0);
    CHECK(bvb_command_batch_append_vulkan_end(&builder) == 0);
    size_t length = 0U;
    CHECK(bvb_command_batch_finish(&builder, &length) == 0);
    struct bvb_command_batch_info info;
    CHECK(bvb_command_batch_validate(bytes, length, &info) == 0);
    CHECK(info.command_count == 9U);
    CHECK(info.command_buffer_id == command_buffer);
    CHECK(info.sequence == 17U);

    struct bvb_command_batch_iterator iterator;
    struct bvb_command_record record;
    CHECK(bvb_command_batch_iterator_init(&iterator, bytes, length) == 0);
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    struct bvb_vulkan_begin_command begin;
    CHECK(bvb_command_decode_vulkan_begin(&record, &begin) == 0);
    CHECK(begin.flags == 1U);
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    struct bvb_fill_buffer_command fill;
    CHECK(bvb_command_decode_fill_buffer(&record, &fill) == 0);
    CHECK(fill.buffer_id == buffer && fill.size == 4096U);
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    struct bvb_vulkan_clear_color_image_command clear;
    CHECK(bvb_command_decode_vulkan_clear_color_image(&record, &clear) == 0);
    CHECK(clear.image_id == image_one);
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    struct bvb_vulkan_init_image_barrier_command barrier;
    CHECK(bvb_command_decode_vulkan_init_image_barrier(&record, &barrier) ==
          0);
    CHECK(barrier.image_count == 2U);
    CHECK(barrier.image_ids[0] == image_one);
    CHECK(barrier.image_ids[1] == image_two);
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    const size_t rich_barrier_payload_offset =
        (size_t)(record.payload - bytes);
    CHECK(record.payload_length == 192U);
    struct bvb_vulkan_image_barrier_2_command rich_barrier;
    CHECK(bvb_command_decode_vulkan_image_barrier_2(&record, &rich_barrier) ==
          0);
    CHECK(rich_barrier.dependency_flags == 1U);
    CHECK(rich_barrier.memory_count == 1U);
    CHECK(rich_barrier.memory[0].destination_access_mask == UINT64_C(0x80));
    CHECK(rich_barrier.buffer_count == 1U);
    CHECK(rich_barrier.buffers[0].buffer_id == buffer);
    CHECK(rich_barrier.buffers[0].source_queue_family_index == 2U);
    CHECK(rich_barrier.buffers[0].destination_queue_family_index == 3U);
    CHECK(rich_barrier.buffers[0].offset == 256U);
    CHECK(rich_barrier.buffers[0].size == 1024U);
    CHECK(rich_barrier.image_count == 1U);
    CHECK(rich_barrier.images[0].image_id == image_one);
    CHECK(rich_barrier.images[0].old_layout == 7U);
    CHECK(rich_barrier.images[0].new_layout == UINT32_C(1000001002));
    CHECK(rich_barrier.images[0].source_queue_family_index == UINT32_MAX);
    CHECK(rich_barrier.images[0].range.level_count == 1U);
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    const size_t rich_clear_payload_offset = (size_t)(record.payload - bytes);
    struct bvb_vulkan_clear_color_image_general_command rich_clear;
    CHECK(bvb_command_decode_vulkan_clear_color_image_general(
              &record, &rich_clear) == 0);
    CHECK(rich_clear.image_id == image_two);
    CHECK(rich_clear.image_layout == 7U);
    CHECK(rich_clear.color_words[0] == UINT32_C(0x3f800000));
    CHECK(rich_clear.color_words[3] == UINT32_C(0x3f800000));
    CHECK(rich_clear.range_count == 2U);
    CHECK(rich_clear.ranges[1].base_mip_level == 1U);
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    struct bvb_vulkan_bind_descriptor_sets_command bind_descriptor_sets;
    CHECK(bvb_command_decode_vulkan_bind_descriptor_sets(
              &record, &bind_descriptor_sets) == 0);
    CHECK(bind_descriptor_sets.pipeline_layout_id == pipeline_layout);
    CHECK(bind_descriptor_sets.descriptor_set_ids[1] == descriptor_set_two);
    CHECK(bind_descriptor_sets.dynamic_offsets[1] == 128U);
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    struct bvb_vulkan_push_constants_command decoded_push;
    CHECK(bvb_command_decode_vulkan_push_constants(
              &record, &decoded_push) == 0);
    CHECK(decoded_push.pipeline_layout_id == pipeline_layout);
    CHECK(decoded_push.stage_flags == 16U);
    CHECK(decoded_push.offset == 4U && decoded_push.size == 16U);
    CHECK(memcmp(decoded_push.data, push_constants.data, 16U) == 0);
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    CHECK(record.opcode == BVB_COMMAND_VULKAN_END);
    CHECK(bvb_command_batch_next(&iterator, &record) == 1);

    uint8_t corrupted[sizeof(bytes)];
    memcpy(corrupted, bytes, length);
    const size_t barrier_payload = BVB_COMMAND_BATCH_HEADER_SIZE +
        BVB_COMMAND_RECORD_HEADER_SIZE + 8U +
        BVB_COMMAND_RECORD_HEADER_SIZE + 32U +
        BVB_COMMAND_RECORD_HEADER_SIZE + 16U +
        BVB_COMMAND_RECORD_HEADER_SIZE;
    bvb_wire_put_u64(corrupted + barrier_payload + 8U + sizeof(uint64_t),
                     image_one);
    CHECK(bvb_command_batch_validate(corrupted, length, &info) == -EPROTO);
    memcpy(corrupted, bytes, length);
    bvb_wire_put_u32(corrupted + BVB_COMMAND_BATCH_HEADER_SIZE +
                         BVB_COMMAND_RECORD_HEADER_SIZE,
                     2U);
    CHECK(bvb_command_batch_validate(corrupted, length, &info) == -EPROTO);
    memcpy(corrupted, bytes, length);
    /* Unknown dependency bits and inactive fixed slots are non-canonical. */
    bvb_wire_put_u32(corrupted + rich_barrier_payload_offset,
                     UINT32_C(0x10));
    CHECK(bvb_command_batch_validate(corrupted, length, &info) == -EPROTO);
    memcpy(corrupted, bytes, length);
    bvb_wire_put_u32(corrupted + rich_barrier_payload_offset + 4U, 2U);
    CHECK(bvb_command_batch_validate(corrupted, length, &info) == -EPROTO);
    memcpy(corrupted, bytes, length);
    bvb_wire_put_u32(corrupted + rich_barrier_payload_offset + 8U, 2U);
    CHECK(bvb_command_batch_validate(corrupted, length, &info) == -EPROTO);
    memcpy(corrupted, bytes, length);
    bvb_wire_put_u32(corrupted + rich_barrier_payload_offset + 12U, 2U);
    CHECK(bvb_command_batch_validate(corrupted, length, &info) == -EPROTO);
    memcpy(corrupted, bytes, length);
    /* The third fixed clear range must remain an all-zero inactive slot. */
    bvb_wire_put_u32(corrupted + rich_clear_payload_offset + 32U + 48U, 1U);
    CHECK(bvb_command_batch_validate(corrupted, length, &info) == -EPROTO);
    memcpy(corrupted, bytes, length);
    bvb_wire_put_u64(corrupted + rich_clear_payload_offset,
                     bvb_handle_id(BVB_OBJECT_BUFFER, 12U));
    CHECK(bvb_command_batch_validate(corrupted, length, &info) == -EPROTO);

    uint8_t *snapshot = NULL;
    CHECK(bvb_command_batch_snapshot(bytes, length, &snapshot) == 0);
    const uint8_t first_snapshot_byte = snapshot[0];
    bytes[0] ^= UINT8_C(0xff);
    CHECK(snapshot[0] == first_snapshot_byte);
    CHECK(snapshot[0] != bytes[0]);
    bytes[0] ^= UINT8_C(0xff);
    free(snapshot);

    struct bvb_command_stream_generation generations[2] = {0};
    size_t generation_index = SIZE_MAX;
    CHECK(bvb_command_stream_generation_check(
              generations, 2U, command_buffer, 17U, &generation_index) == 0);
    CHECK(generation_index == 0U);
    CHECK(bvb_command_stream_generation_commit(
              generations, 2U, generation_index, command_buffer, 17U) == 0);
    CHECK(bvb_command_stream_generation_check(
              generations, 2U, command_buffer, 17U, &generation_index) ==
          -ESTALE);
    CHECK(bvb_command_stream_generation_check(
              generations, 2U, command_buffer, 16U, &generation_index) ==
          -ESTALE);
    CHECK(bvb_command_stream_generation_check(
              generations, 2U, command_buffer, 18U, &generation_index) == 0);
    CHECK(generation_index == 0U);
    CHECK(bvb_command_stream_generation_commit(
              generations, 2U, generation_index, command_buffer, 18U) == 0);
    const uint64_t second_command_buffer =
        bvb_handle_id(BVB_OBJECT_COMMAND_BUFFER, 8U);
    CHECK(bvb_command_stream_generation_check(
              generations, 2U, second_command_buffer, 1U,
              &generation_index) == 0);
    CHECK(generation_index == 1U);
    CHECK(bvb_command_stream_generation_commit(
              generations, 2U, generation_index, second_command_buffer,
              1U) == 0);
    CHECK(bvb_command_stream_generation_check(
              generations, 2U,
              bvb_handle_id(BVB_OBJECT_COMMAND_BUFFER, 9U), 1U,
              &generation_index) == -ENOSPC);
    const struct bvb_command_stream_generation committed[2] = {
        generations[0], generations[1],
    };
    const uint64_t third_command_buffer =
        bvb_handle_id(BVB_OBJECT_COMMAND_BUFFER, 9U);
    const struct bvb_command_stream_generation_update rejected_updates[2] = {
        {.command_buffer_id = command_buffer, .sequence = 19U},
        {.command_buffer_id = third_command_buffer, .sequence = 1U},
    };
    CHECK(bvb_command_stream_generations_apply(
              generations, 2U, rejected_updates, 2U, NULL, NULL, NULL) ==
          -ENOSPC);
    CHECK(memcmp(generations, committed, sizeof(committed)) == 0);

    const struct bvb_command_stream_generation_update reclaimed_update = {
        .command_buffer_id = third_command_buffer,
        .sequence = 1U,
    };
    size_t reclaimed = 0U;
    CHECK(bvb_command_stream_generations_apply(
              generations, 2U, &reclaimed_update, 1U, generation_is_live,
              (void *)&second_command_buffer, &reclaimed) == 0);
    CHECK(reclaimed == 1U);
    CHECK(generations[0].command_buffer_id == third_command_buffer);
    CHECK(generations[0].last_sequence == 1U);
    CHECK(generations[1].command_buffer_id == second_command_buffer);
    CHECK(generations[1].last_sequence == 1U);
    return 0;
}

int main(void) {
    CHECK(test_handles() == 0);
    CHECK(test_vertex_index_draw_family() == 0);
    CHECK(test_dynamic_state_family() == 0);
    CHECK(test_clear_family() == 0);
    CHECK(test_batch() == 0);
    CHECK(test_transfer_batch() == 0);
    CHECK(test_vulkan_transfer_family() == 0);
    CHECK(test_vulkan_update_buffer() == 0);
    CHECK(test_compact_transfer_capacity() == 0);
    CHECK(test_expanded_stream_slot_capacity() == 0);
    CHECK(test_large_image_barrier_batch() == 0);
    CHECK(test_vulkan_command_stream() == 0);
    puts("PASS: proxy handles and triangle command batch");
    return 0;
}
