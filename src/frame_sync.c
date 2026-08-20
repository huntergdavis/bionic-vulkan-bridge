#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <bvb/frame_sync.h>

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

static int64_t monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (int64_t)now.tv_sec * INT64_C(1000000000) + now.tv_nsec;
}

static int futex_wake(uint32_t *word) {
    const long result = syscall(SYS_futex, word, FUTEX_WAKE, INT_MAX, NULL, NULL,
                                0);
    return result < 0 ? -errno : 0;
}

static int futex_wait_until(uint32_t *word, uint32_t expected,
                            int64_t deadline_ns) {
    const int64_t now_ns = monotonic_ns();
    if (now_ns < 0) return -EIO;
    if (now_ns >= deadline_ns) return -ETIMEDOUT;
    const int64_t remaining_ns = deadline_ns - now_ns;
    const struct timespec timeout = {
        .tv_sec = (time_t)(remaining_ns / INT64_C(1000000000)),
        .tv_nsec = (long)(remaining_ns % INT64_C(1000000000)),
    };
    const long result = syscall(SYS_futex, word, FUTEX_WAIT, expected, &timeout,
                                NULL, 0);
    if (result == 0) return 0;
    if (errno == EAGAIN || errno == EINTR) return 0;
    return -errno;
}

int bvb_frame_sync_initialize(void *address, size_t length,
                              uint32_t frame_count) {
    if (address == NULL || !aligned_u32(address) ||
        length < BVB_FRAME_SYNC_REGION_BYTES || frame_count == 0U ||
        frame_count > BVB_FRAME_SYNC_MAX_FRAMES) {
        return -EINVAL;
    }
    memset(address, 0, BVB_FRAME_SYNC_REGION_BYTES);
    struct bvb_frame_sync_control *control = address;
    control->magic = BVB_FRAME_SYNC_MAGIC;
    control->version = BVB_FRAME_SYNC_VERSION;
    control->control_bytes = BVB_FRAME_SYNC_CONTROL_BYTES;
    control->frame_count = frame_count;
    return 0;
}

int bvb_frame_sync_validate(const struct bvb_frame_sync_control *control) {
    if (control == NULL || !aligned_u32(control) ||
        control->magic != BVB_FRAME_SYNC_MAGIC ||
        control->version != BVB_FRAME_SYNC_VERSION ||
        control->control_bytes != BVB_FRAME_SYNC_CONTROL_BYTES ||
        control->frame_count == 0U ||
        control->frame_count > BVB_FRAME_SYNC_MAX_FRAMES) {
        return -EINVAL;
    }
    return 0;
}

static int peer_failure(const struct bvb_frame_sync_control *control) {
    const int32_t producer = load_i32(&control->producer_status);
    if (producer < 0) return producer;
    const int32_t consumer = load_i32(&control->consumer_status);
    return consumer < 0 ? consumer : 0;
}

int bvb_frame_sync_publish_producer(struct bvb_frame_sync_control *control,
                                    uint32_t sequence) {
    int status = bvb_frame_sync_validate(control);
    if (status != 0) return status;
    status = peer_failure(control);
    if (status != 0) return status;
    const uint32_t current = load_u32(&control->producer_sequence);
    const uint32_t consumed = load_u32(&control->consumer_sequence);
    if (sequence == 0U || sequence > control->frame_count ||
        current == UINT32_MAX || sequence != current + 1U) {
        return -ERANGE;
    }
    if (consumed != current) return -EBUSY;
    store_u32(&control->producer_sequence, sequence);
    return futex_wake(&control->producer_sequence);
}

int bvb_frame_sync_publish_consumer(struct bvb_frame_sync_control *control,
                                    uint32_t sequence) {
    int status = bvb_frame_sync_validate(control);
    if (status != 0) return status;
    status = peer_failure(control);
    if (status != 0) return status;
    const uint32_t produced = load_u32(&control->producer_sequence);
    const uint32_t current = load_u32(&control->consumer_sequence);
    if (sequence == 0U || sequence > control->frame_count ||
        current == UINT32_MAX || sequence != current + 1U) {
        return -ERANGE;
    }
    if (produced != sequence) return -EBUSY;
    store_u32(&control->consumer_sequence, sequence);
    return futex_wake(&control->consumer_sequence);
}

static int wait_sequence(struct bvb_frame_sync_control *control,
                         uint32_t *word, uint32_t after_sequence,
                         uint32_t timeout_ms, uint32_t *sequence) {
    if (sequence == NULL || timeout_ms == 0U ||
        after_sequence > control->frame_count) {
        return -EINVAL;
    }
    const int64_t started_ns = monotonic_ns();
    if (started_ns < 0) return -EIO;
    const int64_t timeout_ns = (int64_t)timeout_ms * INT64_C(1000000);
    if (timeout_ns > INT64_MAX - started_ns) return -EOVERFLOW;
    const int64_t deadline_ns = started_ns + timeout_ns;
    for (;;) {
        const int failure = peer_failure(control);
        if (failure != 0) return failure;
        const uint32_t current = load_u32(word);
        if (current > after_sequence) {
            *sequence = current;
            return 0;
        }
        if (current < after_sequence) return -EPROTO;
        const int status = futex_wait_until(word, current, deadline_ns);
        if (status != 0) return status;
    }
}

int bvb_frame_sync_wait_producer(struct bvb_frame_sync_control *control,
                                 uint32_t after_sequence,
                                 uint32_t timeout_ms, uint32_t *sequence) {
    const int status = bvb_frame_sync_validate(control);
    if (status != 0) return status;
    return wait_sequence(control, &control->producer_sequence, after_sequence,
                         timeout_ms, sequence);
}

int bvb_frame_sync_wait_consumer(struct bvb_frame_sync_control *control,
                                 uint32_t after_sequence,
                                 uint32_t timeout_ms, uint32_t *sequence) {
    const int status = bvb_frame_sync_validate(control);
    if (status != 0) return status;
    return wait_sequence(control, &control->consumer_sequence, after_sequence,
                         timeout_ms, sequence);
}

static int publish_failure(struct bvb_frame_sync_control *control,
                           int32_t *status_word, int status) {
    int result = bvb_frame_sync_validate(control);
    if (result != 0) return result;
    if (status >= 0) return -EINVAL;
    store_i32(status_word, (int32_t)status);
    result = futex_wake(&control->producer_sequence);
    const int consumer_wake = futex_wake(&control->consumer_sequence);
    return result != 0 ? result : consumer_wake;
}

int bvb_frame_sync_fail_producer(struct bvb_frame_sync_control *control,
                                 int status) {
    return publish_failure(control, &control->producer_status, status);
}

int bvb_frame_sync_fail_consumer(struct bvb_frame_sync_control *control,
                                 int status) {
    return publish_failure(control, &control->consumer_status, status);
}
