#ifndef BVB_ACTIVITY_FRAME_TRANSPORT_H
#define BVB_ACTIVITY_FRAME_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#include <bvb/wsi_frame_ring.h>

enum {
    BVB_ACTIVITY_FRAME_SETUP_MAGIC = 0x31544642U,
    BVB_ACTIVITY_FRAME_SETUP_VERSION = 1,
    BVB_ACTIVITY_FRAME_SETUP_BYTES = 128,
    BVB_ACTIVITY_FRAME_FLAG_DMA_BUF = 1U << 0,
    BVB_ACTIVITY_FRAME_KNOWN_FLAGS = BVB_ACTIVITY_FRAME_FLAG_DMA_BUF,
};

/*
 * One-time same-UID native-socket envelope around the service's swapchain
 * preparation response. Exactly image_count image-memory FDs followed by the
 * frame-ring control FD accompany this fixed-width record. No part of this
 * setup envelope is used in the per-frame path.
 */
struct bvb_activity_frame_setup {
    uint32_t magic;
    uint16_t version;
    uint16_t header_bytes;
    uint32_t image_count;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t image_usage;
    uint32_t flags;
    uint64_t generation;
    uint64_t allocation_sizes[BVB_WSI_FRAME_RING_MAX_SLOTS];
    uint32_t memory_type_indices[BVB_WSI_FRAME_RING_MAX_SLOTS];
    uint32_t reserved[10];
};

_Static_assert(sizeof(struct bvb_activity_frame_setup) ==
                   BVB_ACTIVITY_FRAME_SETUP_BYTES,
               "Activity frame setup ABI must remain 128 bytes");

int bvb_activity_frame_setup_encode(
    uint8_t output[BVB_ACTIVITY_FRAME_SETUP_BYTES],
    const struct bvb_activity_frame_setup *setup);
int bvb_activity_frame_setup_decode(
    const uint8_t input[BVB_ACTIVITY_FRAME_SETUP_BYTES],
    struct bvb_activity_frame_setup *setup);

/* Sends setup plus image_count+1 FDs to a same-UID abstract Unix listener. */
int bvb_activity_frame_setup_send(
    const char *abstract_socket_name,
    const struct bvb_activity_frame_setup *setup, const int *descriptors,
    size_t descriptor_count);

#endif
