#include <bvb/command_batch.h>
#include <bvb/protocol.h>

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
    uint8_t bytes[512];
    const uint64_t command_buffer =
        bvb_handle_id(BVB_OBJECT_COMMAND_BUFFER, 3U);
    const uint64_t image_view = bvb_handle_id(BVB_OBJECT_IMAGE_VIEW, 4U);
    const uint64_t pipeline = bvb_handle_id(BVB_OBJECT_PIPELINE, 5U);
    struct bvb_command_batch_builder builder;
    CHECK(bvb_command_batch_begin(&builder, bytes, sizeof(bytes), command_buffer,
                                  11U) == 0);
    CHECK(bvb_command_batch_append_begin_rendering(
              &builder,
              &(const struct bvb_begin_rendering_command){
                  .color_image_view_id = image_view,
                  .width = 1280U,
                  .height = 720U,
                  .image_layout = 2U,
                  .load_op = 1U,
                  .store_op = 0U,
                  .layer_count = 1U,
                  .clear_color = {0.0F, 0.0F, 0.0F, 1.0F},
              }) == 0);
    CHECK(bvb_command_batch_append_bind_graphics_pipeline(
              &builder,
              &(const struct bvb_bind_graphics_pipeline_command){
                  .pipeline_id = pipeline,
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
    CHECK(info.command_count == 6U);
    CHECK(info.byte_length == length);

    struct bvb_command_batch_iterator iterator;
    CHECK(bvb_command_batch_iterator_init(&iterator, bytes, length) == 0);
    struct bvb_command_record record;
    struct bvb_begin_rendering_command begin;
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    CHECK(bvb_command_decode_begin_rendering(&record, &begin) == 0);
    CHECK(begin.color_image_view_id == image_view);
    CHECK(begin.width == 1280U && begin.height == 720U);
    CHECK(begin.clear_color[3] == 1.0F);

    struct bvb_bind_graphics_pipeline_command bind;
    CHECK(bvb_command_batch_next(&iterator, &record) == 0);
    CHECK(bvb_command_decode_bind_graphics_pipeline(&record, &bind) == 0);
    CHECK(bind.pipeline_id == pipeline);

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

int main(void) {
    CHECK(test_handles() == 0);
    CHECK(test_batch() == 0);
    puts("PASS: proxy handles and triangle command batch");
    return 0;
}
