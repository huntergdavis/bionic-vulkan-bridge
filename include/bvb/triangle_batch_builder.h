#ifndef BVB_TRIANGLE_BATCH_BUILDER_H
#define BVB_TRIANGLE_BATCH_BUILDER_H

#include <stddef.h>
#include <stdint.h>

int bvb_triangle_batch_build(uint8_t *bytes, size_t capacity,
                             uint32_t width, uint32_t height,
                             size_t *batch_length);

#endif
