#ifndef BVB_VISIBLE_BATCH_H
#define BVB_VISIBLE_BATCH_H

#include <bvb/lifecycle.h>

#include <stddef.h>
#include <stdint.h>

struct bvb_visible_batch_region {
    const uint8_t *address;
    size_t length;
    uint64_t generation;
    uint64_t last_sequence;
    uint8_t token[BVB_LIFECYCLE_TOKEN_SIZE];
};

int bvb_visible_batch_region_init(
    struct bvb_visible_batch_region *region,
    const uint8_t token[BVB_LIFECYCLE_TOKEN_SIZE]);
int bvb_visible_batch_region_setup(struct bvb_visible_batch_region *region,
                                   const uint8_t *payload,
                                   size_t payload_length, int memory_fd);
int bvb_visible_batch_region_execute(
    struct bvb_visible_batch_region *region, const uint8_t *payload,
    size_t payload_length, const uint8_t **batch, size_t *batch_length,
    uint64_t *sequence);
int bvb_visible_batch_inline_decode(
    const uint8_t token[BVB_LIFECYCLE_TOKEN_SIZE], const uint8_t *payload,
    size_t payload_length, const uint8_t **batch, size_t *batch_length,
    uint64_t *sequence);
void bvb_visible_batch_region_destroy(struct bvb_visible_batch_region *region);

#endif
