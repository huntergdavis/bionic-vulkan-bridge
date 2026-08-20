#ifndef BVB_FRAME_SYNC_H
#define BVB_FRAME_SYNC_H

#include <stddef.h>
#include <stdint.h>

enum {
    BVB_FRAME_SYNC_MAGIC = 0x31465342U,
    BVB_FRAME_SYNC_VERSION = 1,
    BVB_FRAME_SYNC_CONTROL_BYTES = 64,
    BVB_FRAME_SYNC_REGION_BYTES = 4096,
    BVB_FRAME_SYNC_MAX_FRAMES = 1048576,
};

/* Fixed-width shared ABI. Access sequence/status words only through the API. */
struct bvb_frame_sync_control {
    uint32_t magic;
    uint16_t version;
    uint16_t control_bytes;
    uint32_t frame_count;
    uint32_t producer_sequence;
    uint32_t consumer_sequence;
    int32_t producer_status;
    int32_t consumer_status;
    uint32_t reserved[9];
};

_Static_assert(sizeof(struct bvb_frame_sync_control) ==
                   BVB_FRAME_SYNC_CONTROL_BYTES,
               "frame sync ABI must remain 64 bytes");

int bvb_frame_sync_initialize(void *address, size_t length,
                              uint32_t frame_count);
int bvb_frame_sync_validate(const struct bvb_frame_sync_control *control);

/* Serialized ownership: producer N requires consumer N-1. */
int bvb_frame_sync_publish_producer(struct bvb_frame_sync_control *control,
                                    uint32_t sequence);
int bvb_frame_sync_wait_producer(struct bvb_frame_sync_control *control,
                                 uint32_t after_sequence,
                                 uint32_t timeout_ms, uint32_t *sequence);

/* Consumer N requires producer N. */
int bvb_frame_sync_publish_consumer(struct bvb_frame_sync_control *control,
                                    uint32_t sequence);
int bvb_frame_sync_wait_consumer(struct bvb_frame_sync_control *control,
                                 uint32_t after_sequence,
                                 uint32_t timeout_ms, uint32_t *sequence);

int bvb_frame_sync_fail_producer(struct bvb_frame_sync_control *control,
                                 int status);
int bvb_frame_sync_fail_consumer(struct bvb_frame_sync_control *control,
                                 int status);

#endif
