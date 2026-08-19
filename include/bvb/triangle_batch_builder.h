#ifndef BVB_TRIANGLE_BATCH_BUILDER_H
#define BVB_TRIANGLE_BATCH_BUILDER_H

#include <stddef.h>
#include <stdint.h>

enum {
    BVB_TRIANGLE_ROTATION_FRAMES_PER_TURN = 600,
};

int bvb_triangle_batch_build(uint8_t *bytes, size_t capacity,
                             uint32_t width, uint32_t height,
                             size_t *batch_length);

int bvb_triangle_batch_build_sequence(uint8_t *bytes, size_t capacity,
                                      uint32_t width, uint32_t height,
                                      uint64_t sequence,
                                      size_t *batch_length);

#endif
