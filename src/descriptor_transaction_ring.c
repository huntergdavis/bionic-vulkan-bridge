#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <bvb/descriptor_transaction_ring.h>

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

static int deadline_from_timeout(uint32_t timeout_ms, int64_t *deadline_ns) {
    if (timeout_ms == 0U || deadline_ns == NULL) return -EINVAL;
    const int64_t now_ns = monotonic_ns();
    if (now_ns < 0) return -EIO;
    const int64_t timeout_ns = (int64_t)timeout_ms * INT64_C(1000000);
    if (timeout_ns > INT64_MAX - now_ns) return -EOVERFLOW;
    *deadline_ns = now_ns + timeout_ns;
    return 0;
}

static int futex_wake(uint32_t *word) {
    const long result = syscall(SYS_futex, word, FUTEX_WAKE, INT_MAX, NULL,
                                NULL, 0);
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
    const long result = syscall(SYS_futex, word, FUTEX_WAIT, expected,
                                &timeout, NULL, 0);
    if (result == 0 || errno == EAGAIN || errno == EINTR) return 0;
    return -errno;
}

static struct bvb_descriptor_transaction_slot *slot_at(
    struct bvb_descriptor_transaction_ring *ring, uint32_t index) {
    return (struct bvb_descriptor_transaction_slot *)
        ((uint8_t *)ring + BVB_DESCRIPTOR_TRANSACTION_RING_CONTROL_BYTES) +
        index;
}

static int peer_status(const struct bvb_descriptor_transaction_ring *ring) {
    const int32_t client = load_i32(&ring->client_status);
    if (client < 0) return client;
    const int32_t service = load_i32(&ring->service_status);
    return service < 0 ? service : 0;
}

static int validate_header(
    const struct bvb_descriptor_transaction_ring *ring, size_t length,
    uint64_t generation) {
    if (ring == NULL ||
        (uintptr_t)ring % _Alignof(uint64_t) != 0U ||
        length != BVB_DESCRIPTOR_TRANSACTION_RING_REGION_BYTES ||
        ring->magic != BVB_DESCRIPTOR_TRANSACTION_RING_MAGIC ||
        ring->version != BVB_DESCRIPTOR_TRANSACTION_RING_VERSION ||
        ring->control_bytes != BVB_DESCRIPTOR_TRANSACTION_RING_CONTROL_BYTES ||
        ring->slot_count == 0U ||
        ring->slot_count > BVB_DESCRIPTOR_TRANSACTION_RING_SLOT_COUNT ||
        ring->flags != 0U || ring->generation == 0U ||
        (generation != 0U && ring->generation != generation)) {
        return -EINVAL;
    }
    return 0;
}

int bvb_descriptor_transaction_ring_initialize(
    void *address, size_t length, uint32_t slot_count, uint64_t generation) {
    if (address == NULL ||
        (uintptr_t)address % _Alignof(uint64_t) != 0U ||
        length != BVB_DESCRIPTOR_TRANSACTION_RING_REGION_BYTES ||
        slot_count == 0U ||
        slot_count > BVB_DESCRIPTOR_TRANSACTION_RING_SLOT_COUNT ||
        generation == 0U) {
        return -EINVAL;
    }
    memset(address, 0, length);
    struct bvb_descriptor_transaction_ring *ring = address;
    ring->magic = BVB_DESCRIPTOR_TRANSACTION_RING_MAGIC;
    ring->version = BVB_DESCRIPTOR_TRANSACTION_RING_VERSION;
    ring->control_bytes = BVB_DESCRIPTOR_TRANSACTION_RING_CONTROL_BYTES;
    ring->slot_count = slot_count;
    ring->generation = generation;
    return 0;
}

int bvb_descriptor_transaction_ring_validate(
    const struct bvb_descriptor_transaction_ring *ring, size_t length,
    uint64_t generation) {
    int result = validate_header(ring, length, generation);
    if (result != 0 || load_u32(&ring->next_slot) >= ring->slot_count)
        return result != 0 ? result : -EINVAL;
    for (uint32_t index = 0U; index < ring->slot_count; ++index) {
        const struct bvb_descriptor_transaction_slot *slot =
            slot_at((struct bvb_descriptor_transaction_ring *)ring, index);
        if (load_u32(&slot->state) >
                BVB_DESCRIPTOR_TRANSACTION_SLOT_COMPLETED ||
            slot->flags != 0U ||
            slot->request_length >
                BVB_DESCRIPTOR_TRANSACTION_RING_REQUEST_BYTES ||
            slot->response_length >
                BVB_DESCRIPTOR_TRANSACTION_RING_RESPONSE_BYTES) {
            return -EPROTO;
        }
    }
    return 0;
}

int bvb_descriptor_transaction_ring_call(
    struct bvb_descriptor_transaction_ring *ring, size_t length,
    uint64_t generation, uint32_t sequence, const uint8_t *request,
    uint32_t request_length, uint8_t *response, uint32_t response_capacity,
    uint32_t *response_length, uint32_t timeout_ms) {
    int result = validate_header(
        ring, length, generation);
    if (result != 0 || sequence == 0U || request == NULL ||
        request_length == 0U ||
        request_length > BVB_DESCRIPTOR_TRANSACTION_RING_REQUEST_BYTES ||
        response == NULL || response_length == NULL || timeout_ms == 0U) {
        return result != 0 ? result : -EINVAL;
    }
    result = peer_status(ring);
    if (result != 0) return result;
    const uint32_t index = (sequence - 1U) % ring->slot_count;
    struct bvb_descriptor_transaction_slot *slot = slot_at(ring, index);
    if (load_u32(&slot->state) !=
        BVB_DESCRIPTOR_TRANSACTION_SLOT_AVAILABLE) {
        return -EBUSY;
    }
    memset(slot, 0, sizeof(*slot));
    memcpy(slot->request, request, request_length);
    slot->request_length = request_length;
    slot->sequence = sequence;
    store_u32(&slot->state, BVB_DESCRIPTOR_TRANSACTION_SLOT_REQUESTED);
    store_u32(&ring->request_sequence, sequence);
    result = futex_wake(&ring->request_sequence);
    if (result != 0) return result;

    int64_t deadline_ns = 0;
    result = deadline_from_timeout(timeout_ms, &deadline_ns);
    if (result != 0) return result;
    for (;;) {
        result = peer_status(ring);
        if (result != 0) return result;
        const uint32_t completed = load_u32(&ring->completion_sequence);
        if (completed >= sequence) break;
        result = futex_wait_until(
            &ring->completion_sequence, completed, deadline_ns);
        if (result != 0) return result;
    }
    if (slot->sequence != sequence ||
        load_u32(&slot->state) !=
            BVB_DESCRIPTOR_TRANSACTION_SLOT_COMPLETED ||
        slot->response_length > response_capacity) {
        return -EPROTO;
    }
    if (slot->status != 0) return slot->status;
    memcpy(response, slot->response, slot->response_length);
    *response_length = slot->response_length;
    memset(slot, 0, sizeof(*slot));
    store_u32(&slot->state, BVB_DESCRIPTOR_TRANSACTION_SLOT_AVAILABLE);
    store_u32(&ring->next_slot, (index + 1U) % ring->slot_count);
    return 0;
}

int bvb_descriptor_transaction_ring_wait_request(
    struct bvb_descriptor_transaction_ring *ring, size_t length,
    uint64_t generation, uint32_t after_sequence, uint8_t *request,
    uint32_t request_capacity, uint32_t *request_length, uint32_t *slot_index,
    uint32_t *sequence, uint32_t timeout_ms) {
    int result = validate_header(
        ring, length, generation);
    if (result != 0 || after_sequence == UINT32_MAX || request == NULL ||
        request_length == NULL || slot_index == NULL || sequence == NULL ||
        timeout_ms == 0U) {
        return result != 0 ? result : -EINVAL;
    }
    const uint32_t expected = after_sequence + 1U;
    int64_t deadline_ns = 0;
    result = deadline_from_timeout(timeout_ms, &deadline_ns);
    if (result != 0) return result;
    for (;;) {
        result = peer_status(ring);
        if (result != 0) return result;
        const uint32_t requested = load_u32(&ring->request_sequence);
        if (requested >= expected) break;
        result = futex_wait_until(&ring->request_sequence, requested,
                                  deadline_ns);
        if (result != 0) return result;
    }
    const uint32_t index = (expected - 1U) % ring->slot_count;
    struct bvb_descriptor_transaction_slot *slot = slot_at(ring, index);
    if (slot->sequence != expected ||
        load_u32(&slot->state) !=
            BVB_DESCRIPTOR_TRANSACTION_SLOT_REQUESTED ||
        slot->request_length == 0U ||
        slot->request_length > request_capacity) {
        return -EPROTO;
    }
    memcpy(request, slot->request, slot->request_length);
    *request_length = slot->request_length;
    *slot_index = index;
    *sequence = expected;
    return 0;
}

int bvb_descriptor_transaction_ring_complete(
    struct bvb_descriptor_transaction_ring *ring, size_t length,
    uint64_t generation, uint32_t slot_index, uint32_t sequence, int status,
    const uint8_t *response, uint32_t response_length) {
    int result = validate_header(
        ring, length, generation);
    if (result != 0 || slot_index >= ring->slot_count || sequence == 0U ||
        status > 0 || response_length >
            BVB_DESCRIPTOR_TRANSACTION_RING_RESPONSE_BYTES ||
        (response_length != 0U && response == NULL) ||
        (status != 0 && response_length != 0U)) {
        return result != 0 ? result : -EINVAL;
    }
    struct bvb_descriptor_transaction_slot *slot = slot_at(ring, slot_index);
    if (slot->sequence != sequence ||
        load_u32(&slot->state) !=
            BVB_DESCRIPTOR_TRANSACTION_SLOT_REQUESTED) {
        return -ESTALE;
    }
    slot->status = status;
    slot->response_length = response_length;
    if (response_length != 0U) {
        memcpy(slot->response, response, response_length);
    }
    store_u32(&slot->state, BVB_DESCRIPTOR_TRANSACTION_SLOT_COMPLETED);
    store_u32(&ring->completion_sequence, sequence);
    return futex_wake(&ring->completion_sequence);
}

static int fail_peer(struct bvb_descriptor_transaction_ring *ring,
                     int32_t *status_word, int status) {
    int result = validate_header(
        ring, BVB_DESCRIPTOR_TRANSACTION_RING_REGION_BYTES, 0U);
    if (result != 0) return result;
    if (status >= 0) return -EINVAL;
    store_i32(status_word, (int32_t)status);
    result = futex_wake(&ring->request_sequence);
    const int completion_result = futex_wake(&ring->completion_sequence);
    return result != 0 ? result : completion_result;
}

int bvb_descriptor_transaction_ring_fail_client(
    struct bvb_descriptor_transaction_ring *ring, int status) {
    return fail_peer(ring, &ring->client_status, status);
}

int bvb_descriptor_transaction_ring_fail_service(
    struct bvb_descriptor_transaction_ring *ring, int status) {
    return fail_peer(ring, &ring->service_status, status);
}
