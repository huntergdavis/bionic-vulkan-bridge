#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <bvb/wsi_frame_ring.h>

#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#if !defined(SYS_futex) && defined(__NR_futex)
#define SYS_futex __NR_futex
#endif

static bool aligned_u32(const void *address) {
    return ((uintptr_t)address % _Alignof(uint32_t)) == 0U;
}

static uint32_t load_u32(const uint32_t *word) {
    return __atomic_load_n(word, __ATOMIC_ACQUIRE);
}

static int32_t load_i32(const int32_t *word) {
    return __atomic_load_n(word, __ATOMIC_ACQUIRE);
}

static void store_u32(uint32_t *word, uint32_t value) {
    __atomic_store_n(word, value, __ATOMIC_RELEASE);
}

static void store_i32(int32_t *word, int32_t value) {
    __atomic_store_n(word, value, __ATOMIC_RELEASE);
}

static bool compare_exchange_u32(uint32_t *word, uint32_t *expected,
                                 uint32_t desired) {
    return __atomic_compare_exchange_n(word, expected, desired, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static int64_t monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (int64_t)now.tv_sec * INT64_C(1000000000) + now.tv_nsec;
}

static int futex_wake(uint32_t *word) {
    const long result = syscall(SYS_futex, word, FUTEX_WAKE, INT_MAX, NULL,
                                NULL, 0);
    return result < 0 ? -errno : 0;
}

static int futex_wait(uint32_t *word, uint32_t expected,
                      int64_t deadline_ns) {
    const int64_t now_ns = monotonic_ns();
    if (now_ns < 0) return -EIO;
    if (now_ns >= deadline_ns) return -ETIMEDOUT;
    const int64_t remaining_ns = deadline_ns - now_ns;
    const struct timespec timeout = {
        .tv_sec = (time_t)(remaining_ns / INT64_C(1000000000)),
        .tv_nsec = (long)(remaining_ns % INT64_C(1000000000)),
    };
    const long result = syscall(SYS_futex, word, FUTEX_WAIT, expected,
                                &timeout, NULL, 0);
    if (result == 0 || errno == EAGAIN || errno == EINTR) return 0;
    return -errno;
}

static int peer_failure(const struct bvb_wsi_frame_ring *ring) {
    const int32_t producer = load_i32(&ring->producer_status);
    if (producer < 0) return producer;
    const int32_t consumer = load_i32(&ring->consumer_status);
    return consumer < 0 ? consumer : 0;
}

int bvb_wsi_frame_ring_initialize(void *address, size_t length,
                                  uint32_t slot_count, uint64_t generation) {
    if (address == NULL || !aligned_u32(address) ||
        length < BVB_WSI_FRAME_RING_REGION_BYTES || slot_count < 2U ||
        slot_count > BVB_WSI_FRAME_RING_MAX_SLOTS || generation == 0U) {
        return -EINVAL;
    }
    memset(address, 0, BVB_WSI_FRAME_RING_REGION_BYTES);
    struct bvb_wsi_frame_ring *ring = address;
    ring->magic = BVB_WSI_FRAME_RING_MAGIC;
    ring->version = BVB_WSI_FRAME_RING_VERSION;
    ring->control_bytes = BVB_WSI_FRAME_RING_CONTROL_BYTES;
    ring->slot_count = slot_count;
    ring->generation = generation;
    return 0;
}

int bvb_wsi_frame_ring_validate(const struct bvb_wsi_frame_ring *ring,
                                uint64_t generation) {
    if (ring == NULL || !aligned_u32(ring) ||
        ring->magic != BVB_WSI_FRAME_RING_MAGIC ||
        ring->version != BVB_WSI_FRAME_RING_VERSION ||
        ring->control_bytes != BVB_WSI_FRAME_RING_CONTROL_BYTES ||
        ring->slot_count < 2U ||
        ring->slot_count > BVB_WSI_FRAME_RING_MAX_SLOTS ||
        ring->flags != 0U || ring->generation == 0U ||
        (generation != 0U && ring->generation != generation)) {
        return -EINVAL;
    }
    for (uint32_t slot = 0U; slot < ring->slot_count; ++slot) {
        if (load_u32(&ring->slot_state[slot]) > BVB_WSI_SLOT_PRESENTED) {
            return -EPROTO;
        }
    }
    return 0;
}

static int deadline_from_timeout(uint32_t timeout_ms, int64_t *deadline_ns) {
    if (deadline_ns == NULL || timeout_ms == 0U) return -EINVAL;
    const int64_t now_ns = monotonic_ns();
    if (now_ns < 0) return -EIO;
    const int64_t timeout_ns = (int64_t)timeout_ms * INT64_C(1000000);
    if (timeout_ns > INT64_MAX - now_ns) return -EOVERFLOW;
    *deadline_ns = now_ns + timeout_ns;
    return 0;
}

int bvb_wsi_frame_ring_acquire(struct bvb_wsi_frame_ring *ring,
                               uint32_t timeout_ms, uint32_t *slot) {
    int status = bvb_wsi_frame_ring_validate(ring, 0U);
    if (status != 0 || slot == NULL) return status != 0 ? status : -EINVAL;
    int64_t deadline_ns = 0;
    if (timeout_ms != 0U) {
        status = deadline_from_timeout(timeout_ms, &deadline_ns);
        if (status != 0) return status;
    }
    for (;;) {
        status = peer_failure(ring);
        if (status != 0) return status;
        const uint32_t start = load_u32(&ring->next_acquire) % ring->slot_count;
        for (uint32_t offset = 0U; offset < ring->slot_count; ++offset) {
            const uint32_t candidate = (start + offset) % ring->slot_count;
            uint32_t expected = BVB_WSI_SLOT_AVAILABLE;
            if (compare_exchange_u32(&ring->slot_state[candidate], &expected,
                                     BVB_WSI_SLOT_ACQUIRED)) {
                store_u32(&ring->next_acquire,
                          (candidate + 1U) % ring->slot_count);
                *slot = candidate;
                return 0;
            }
        }
        if (timeout_ms == 0U) return -EAGAIN;
        const uint32_t observed = load_u32(&ring->consumer_sequence);
        status = futex_wait(&ring->consumer_sequence, observed, deadline_ns);
        if (status != 0) return status;
    }
}

int bvb_wsi_frame_ring_present(struct bvb_wsi_frame_ring *ring,
                               uint32_t slot, uint32_t *sequence) {
    int status = bvb_wsi_frame_ring_validate(ring, 0U);
    if (status != 0 || sequence == NULL || slot >= ring->slot_count) {
        return status != 0 ? status : -EINVAL;
    }
    status = peer_failure(ring);
    if (status != 0) return status;
    const uint32_t current = load_u32(&ring->producer_sequence);
    if (current == UINT32_MAX) return -EOVERFLOW;
    uint32_t expected = BVB_WSI_SLOT_ACQUIRED;
    if (!compare_exchange_u32(&ring->slot_state[slot], &expected,
                              BVB_WSI_SLOT_PRESENTED)) {
        return expected == BVB_WSI_SLOT_PRESENTED ? -EBUSY : -EPROTO;
    }
    const uint32_t next = current + 1U;
    store_u32(&ring->slot_sequence[slot], next);
    store_u32(&ring->producer_sequence, next);
    status = futex_wake(&ring->producer_sequence);
    if (status != 0) return status;
    *sequence = next;
    return 0;
}

static int find_presented(const struct bvb_wsi_frame_ring *ring,
                          uint32_t expected_sequence, uint32_t *slot) {
    for (uint32_t candidate = 0U; candidate < ring->slot_count; ++candidate) {
        if (load_u32(&ring->slot_state[candidate]) ==
                BVB_WSI_SLOT_PRESENTED &&
            load_u32(&ring->slot_sequence[candidate]) == expected_sequence) {
            *slot = candidate;
            return 0;
        }
    }
    return -EAGAIN;
}

int bvb_wsi_frame_ring_wait_present(struct bvb_wsi_frame_ring *ring,
                                    uint32_t after_sequence,
                                    uint32_t timeout_ms, uint32_t *slot,
                                    uint32_t *sequence) {
    int status = bvb_wsi_frame_ring_validate(ring, 0U);
    if (status != 0 || slot == NULL || sequence == NULL ||
        after_sequence == UINT32_MAX || timeout_ms == 0U) {
        return status != 0 ? status : -EINVAL;
    }
    int64_t deadline_ns = 0;
    status = deadline_from_timeout(timeout_ms, &deadline_ns);
    if (status != 0) return status;
    const uint32_t expected_sequence = after_sequence + 1U;
    for (;;) {
        status = peer_failure(ring);
        if (status != 0) return status;
        status = find_presented(ring, expected_sequence, slot);
        if (status == 0) {
            *sequence = expected_sequence;
            return 0;
        }
        const uint32_t produced = load_u32(&ring->producer_sequence);
        if (produced >= expected_sequence) {
            /* Publication may have raced the scan immediately above. */
            status = find_presented(ring, expected_sequence, slot);
            if (status == 0) {
                *sequence = expected_sequence;
                return 0;
            }
            return -EPROTO;
        }
        status = futex_wait(&ring->producer_sequence, produced, deadline_ns);
        if (status != 0) return status;
    }
}

int bvb_wsi_frame_ring_release(struct bvb_wsi_frame_ring *ring,
                               uint32_t slot, uint32_t sequence) {
    int status = bvb_wsi_frame_ring_validate(ring, 0U);
    if (status != 0 || slot >= ring->slot_count || sequence == 0U) {
        return status != 0 ? status : -EINVAL;
    }
    status = peer_failure(ring);
    if (status != 0) return status;
    if (load_u32(&ring->slot_sequence[slot]) != sequence ||
        load_u32(&ring->consumer_sequence) + 1U != sequence) {
        return -ESTALE;
    }
    /* Clear the old sequence before making the slot observable as reusable. */
    store_u32(&ring->slot_sequence[slot], 0U);
    uint32_t expected = BVB_WSI_SLOT_PRESENTED;
    if (!compare_exchange_u32(&ring->slot_state[slot], &expected,
                              BVB_WSI_SLOT_AVAILABLE)) {
        return -EPROTO;
    }
    store_u32(&ring->consumer_sequence, sequence);
    return futex_wake(&ring->consumer_sequence);
}

static int publish_failure(struct bvb_wsi_frame_ring *ring,
                           int32_t *status_word, int status) {
    int result = bvb_wsi_frame_ring_validate(ring, 0U);
    if (result != 0) return result;
    if (status >= 0) return -EINVAL;
    store_i32(status_word, (int32_t)status);
    result = futex_wake(&ring->producer_sequence);
    const int consumer_wake = futex_wake(&ring->consumer_sequence);
    return result != 0 ? result : consumer_wake;
}

int bvb_wsi_frame_ring_fail_producer(struct bvb_wsi_frame_ring *ring,
                                     int status) {
    return publish_failure(ring, &ring->producer_status, status);
}

int bvb_wsi_frame_ring_fail_consumer(struct bvb_wsi_frame_ring *ring,
                                     int status) {
    return publish_failure(ring, &ring->consumer_status, status);
}
