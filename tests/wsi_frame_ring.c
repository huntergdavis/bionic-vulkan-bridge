#define _POSIX_C_SOURCE 200112L

#include <bvb/wsi_frame_ring.h>

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #expression);                                               \
            return 1;                                                           \
        }                                                                       \
    } while (0)

enum { TEST_FRAMES = 4096 };

struct threaded_ring {
    struct bvb_wsi_frame_ring *ring;
    int result;
};

static void *consume_frames(void *opaque) {
    struct threaded_ring *test = opaque;
    uint32_t after = 0U;
    for (uint32_t frame = 0U; frame < TEST_FRAMES; ++frame) {
        uint32_t slot = UINT32_MAX;
        uint32_t sequence = 0U;
        int result = bvb_wsi_frame_ring_wait_present(
            test->ring, after, 5000U, &slot, &sequence);
        if (result == 0 && sequence != after + 1U) result = -EPROTO;
        if (result == 0) {
            result = bvb_wsi_frame_ring_release(
                test->ring, slot, sequence);
        }
        if (result != 0) {
            test->result = result;
            (void)bvb_wsi_frame_ring_fail_consumer(test->ring, result);
            return NULL;
        }
        after = sequence;
    }
    return NULL;
}

int main(void) {
    void *region = NULL;
    CHECK(posix_memalign(&region, BVB_WSI_FRAME_RING_REGION_BYTES,
                        BVB_WSI_FRAME_RING_REGION_BYTES) == 0);
    CHECK(bvb_wsi_frame_ring_initialize(
              region, BVB_WSI_FRAME_RING_REGION_BYTES, 1U, 1U) == -EINVAL);
    CHECK(bvb_wsi_frame_ring_initialize(
              region, BVB_WSI_FRAME_RING_REGION_BYTES, 3U,
              UINT64_C(0x123456789abcdef0)) == 0);
    struct bvb_wsi_frame_ring *ring = region;
    CHECK(bvb_wsi_frame_ring_validate(
              ring, UINT64_C(0x123456789abcdef0)) == 0);
    CHECK(bvb_wsi_frame_ring_validate(ring, 99U) == -EINVAL);

    uint32_t slots[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
    CHECK(bvb_wsi_frame_ring_acquire(ring, 0U, &slots[0]) == 0);
    CHECK(bvb_wsi_frame_ring_acquire(ring, 0U, &slots[1]) == 0);
    CHECK(bvb_wsi_frame_ring_acquire(ring, 0U, &slots[2]) == 0);
    CHECK(slots[0] == 0U && slots[1] == 1U && slots[2] == 2U);
    uint32_t unavailable = UINT32_MAX;
    CHECK(bvb_wsi_frame_ring_acquire(ring, 0U, &unavailable) == -EAGAIN);
    uint32_t sequence = 0U;
    CHECK(bvb_wsi_frame_ring_present(ring, slots[0], &sequence) == 0);
    CHECK(sequence == 1U);
    uint32_t consumed_slot = UINT32_MAX;
    uint32_t consumed_sequence = 0U;
    CHECK(bvb_wsi_frame_ring_wait_present(
              ring, 0U, 100U, &consumed_slot, &consumed_sequence) == 0);
    CHECK(consumed_slot == slots[0] && consumed_sequence == 1U);
    CHECK(bvb_wsi_frame_ring_release(
              ring, consumed_slot, consumed_sequence) == 0);
    CHECK(bvb_wsi_frame_ring_present(ring, slots[1], &sequence) == 0);
    CHECK(sequence == 2U);
    CHECK(bvb_wsi_frame_ring_wait_present(
              ring, 1U, 100U, &consumed_slot, &consumed_sequence) == 0);
    CHECK(consumed_slot == slots[1] && consumed_sequence == 2U);
    CHECK(bvb_wsi_frame_ring_release(
              ring, consumed_slot, consumed_sequence) == 0);
    CHECK(bvb_wsi_frame_ring_present(ring, slots[2], &sequence) == 0);
    CHECK(sequence == 3U);
    CHECK(bvb_wsi_frame_ring_wait_present(
              ring, 2U, 100U, &consumed_slot, &consumed_sequence) == 0);
    CHECK(bvb_wsi_frame_ring_release(
              ring, consumed_slot, consumed_sequence) == 0);

    CHECK(bvb_wsi_frame_ring_initialize(
              region, BVB_WSI_FRAME_RING_REGION_BYTES, 3U, 2U) == 0);
    struct threaded_ring threaded = {.ring = ring};
    pthread_t consumer;
    CHECK(pthread_create(&consumer, NULL, consume_frames, &threaded) == 0);
    for (uint32_t frame = 0U; frame < TEST_FRAMES; ++frame) {
        uint32_t slot = UINT32_MAX;
        const int acquire_status =
            bvb_wsi_frame_ring_acquire(ring, 5000U, &slot);
        if (acquire_status != 0) {
            fprintf(stderr,
                    "acquire frame=%u status=%d consumer=%d produced=%u "
                    "consumed=%u states=%u,%u,%u sequences=%u,%u,%u\n",
                    frame, acquire_status, threaded.result,
                    ring->producer_sequence, ring->consumer_sequence,
                    ring->slot_state[0], ring->slot_state[1],
                    ring->slot_state[2], ring->slot_sequence[0],
                    ring->slot_sequence[1], ring->slot_sequence[2]);
            return 1;
        }
        CHECK(bvb_wsi_frame_ring_present(ring, slot, &sequence) == 0);
        CHECK(sequence == frame + 1U);
    }
    CHECK(pthread_join(consumer, NULL) == 0);
    CHECK(threaded.result == 0);
    CHECK(ring->producer_sequence == TEST_FRAMES);
    CHECK(ring->consumer_sequence == TEST_FRAMES);

    CHECK(bvb_wsi_frame_ring_fail_consumer(ring, -EIO) == 0);
    CHECK(bvb_wsi_frame_ring_acquire(ring, 0U, &unavailable) == -EIO);
    free(region);
    puts("PASS: persistent game-to-Activity WSI frame ring");
    return 0;
}
