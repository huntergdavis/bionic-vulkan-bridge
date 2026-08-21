#ifndef BVB_WSI_FRAME_RING_H
#define BVB_WSI_FRAME_RING_H

#include <stddef.h>
#include <stdint.h>

enum {
    BVB_WSI_FRAME_RING_MAGIC = 0x31525742U,
    BVB_WSI_FRAME_RING_VERSION = 1,
    BVB_WSI_FRAME_RING_CONTROL_BYTES = 128,
    BVB_WSI_FRAME_RING_REGION_BYTES = 4096,
    BVB_WSI_FRAME_RING_MAX_SLOTS = 4,
    BVB_WSI_SLOT_AVAILABLE = 0,
    BVB_WSI_SLOT_ACQUIRED = 1,
    BVB_WSI_SLOT_PRESENTED = 2,
};

/*
 * Cross-libc shared ABI for the game-to-Activity image ring. All changing
 * words are accessed through this API with acquire/release atomics. The
 * exported image FDs and this control page are delivered once at setup.
 */
struct bvb_wsi_frame_ring {
    uint32_t magic;
    uint16_t version;
    uint16_t control_bytes;
    uint32_t slot_count;
    uint32_t flags;
    uint64_t generation;
    uint32_t next_acquire;
    uint32_t producer_sequence;
    uint32_t consumer_sequence;
    int32_t producer_status;
    int32_t consumer_status;
    uint32_t slot_state[BVB_WSI_FRAME_RING_MAX_SLOTS];
    uint32_t slot_sequence[BVB_WSI_FRAME_RING_MAX_SLOTS];
    uint32_t reserved[13];
};

_Static_assert(sizeof(struct bvb_wsi_frame_ring) ==
                   BVB_WSI_FRAME_RING_CONTROL_BYTES,
               "WSI frame-ring ABI must remain 128 bytes");

int bvb_wsi_frame_ring_initialize(void *address, size_t length,
                                  uint32_t slot_count, uint64_t generation);
int bvb_wsi_frame_ring_validate(const struct bvb_wsi_frame_ring *ring,
                                uint64_t generation);

/* Claims one reusable image. timeout_ms==0 is a non-blocking poll. */
int bvb_wsi_frame_ring_acquire(struct bvb_wsi_frame_ring *ring,
                               uint32_t timeout_ms, uint32_t *slot);

/* Publishes an acquired image after producer-local GPU completion. */
int bvb_wsi_frame_ring_present(struct bvb_wsi_frame_ring *ring,
                               uint32_t slot, uint32_t *sequence);

/* Waits for the next ordered image for Activity-local copy/present. */
int bvb_wsi_frame_ring_wait_present(struct bvb_wsi_frame_ring *ring,
                                    uint32_t after_sequence,
                                    uint32_t timeout_ms, uint32_t *slot,
                                    uint32_t *sequence);

/* Releases an image only after Activity-local GPU completion. */
int bvb_wsi_frame_ring_release(struct bvb_wsi_frame_ring *ring,
                               uint32_t slot, uint32_t sequence);

int bvb_wsi_frame_ring_fail_producer(struct bvb_wsi_frame_ring *ring,
                                     int status);
int bvb_wsi_frame_ring_fail_consumer(struct bvb_wsi_frame_ring *ring,
                                     int status);

#endif
