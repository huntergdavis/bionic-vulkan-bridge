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

enum {
    BVB_DESCRIPTOR_TRANSACTION_SPIN_NS = 250000,
    BVB_DESCRIPTOR_TRANSACTION_SPIN_CLOCK_INTERVAL = 64,
};

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

static void cpu_relax(void) {
#if defined(__aarch64__) || defined(__arm__)
    __asm__ volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("pause" ::: "memory");
#else
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
#endif
}

static struct bvb_descriptor_transaction_slot *slot_at(
    struct bvb_descriptor_transaction_ring *ring, uint32_t index) {
    return (struct bvb_descriptor_transaction_slot *)
        ((uint8_t *)ring + BVB_DESCRIPTOR_TRANSACTION_RING_CONTROL_BYTES) +
        index;
}

static struct bvb_descriptor_lease_bank *lease_bank_at(
    struct bvb_descriptor_transaction_ring *ring, uint32_t index) {
    return (struct bvb_descriptor_lease_bank *)
        ((uint8_t *)ring + BVB_DESCRIPTOR_TRANSACTION_RING_LEASE_OFFSET) +
        index;
}

static int peer_status(const struct bvb_descriptor_transaction_ring *ring) {
    const int32_t client = load_i32(&ring->client_status);
    if (client < 0) return client;
    const int32_t service = load_i32(&ring->service_status);
    return service < 0 ? service : 0;
}

static int wait_for_sequence(
    struct bvb_descriptor_transaction_ring *ring, uint32_t *word,
    uint32_t *wait_state, uint32_t expected, int64_t deadline_ns) {
    int result = peer_status(ring);
    if (result != 0) return result;
    if (load_u32(word) >= expected) return 0;

    const int64_t started_ns = monotonic_ns();
    if (started_ns < 0) return -EIO;
    int64_t spin_deadline_ns =
        started_ns + BVB_DESCRIPTOR_TRANSACTION_SPIN_NS;
    if (spin_deadline_ns < started_ns || spin_deadline_ns > deadline_ns)
        spin_deadline_ns = deadline_ns;
    store_u32(wait_state, BVB_DESCRIPTOR_TRANSACTION_WAIT_SPINNING);
    uint32_t iterations = 0U;
    for (;;) {
        result = peer_status(ring);
        if (result != 0 || load_u32(word) >= expected) {
            store_u32(wait_state, BVB_DESCRIPTOR_TRANSACTION_WAIT_IDLE);
            return result;
        }
        cpu_relax();
        ++iterations;
        if (iterations % BVB_DESCRIPTOR_TRANSACTION_SPIN_CLOCK_INTERVAL ==
            0U) {
            const int64_t now_ns = monotonic_ns();
            if (now_ns < 0) {
                store_u32(
                    wait_state, BVB_DESCRIPTOR_TRANSACTION_WAIT_IDLE);
                return -EIO;
            }
            if (now_ns >= spin_deadline_ns) break;
        }
    }

    store_u32(wait_state, BVB_DESCRIPTOR_TRANSACTION_WAIT_SLEEPING);
    for (;;) {
        result = peer_status(ring);
        if (result != 0) break;
        const uint32_t observed = load_u32(word);
        if (observed >= expected) break;
        result = futex_wait_until(word, observed, deadline_ns);
        if (result != 0) break;
    }
    store_u32(wait_state, BVB_DESCRIPTOR_TRANSACTION_WAIT_IDLE);
    return result;
}

static int wake_if_sleeping(uint32_t *word, const uint32_t *wait_state) {
    return load_u32(wait_state) == BVB_DESCRIPTOR_TRANSACTION_WAIT_SLEEPING
        ? futex_wake(word) : 0;
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
        load_u32(&ring->request_wait_state) >
            BVB_DESCRIPTOR_TRANSACTION_WAIT_SLEEPING ||
        load_u32(&ring->completion_wait_state) >
            BVB_DESCRIPTOR_TRANSACTION_WAIT_SLEEPING ||
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
    for (uint32_t index = 0U; index < BVB_DESCRIPTOR_LEASE_BANK_COUNT;
         ++index) {
        const struct bvb_descriptor_lease_bank *bank =
            lease_bank_at((struct bvb_descriptor_transaction_ring *)ring,
                          index);
        const uint32_t ready = load_u32(&bank->ready);
        const uint32_t count = load_u32(&bank->count);
        const uint32_t cursor = load_u32(&bank->cursor);
        if (ready > BVB_DESCRIPTOR_LEASE_BANK_READY ||
            count > BVB_DESCRIPTOR_LEASE_BANK_CAPACITY || cursor > count ||
            bank->reserved != 0U ||
            (ready == BVB_DESCRIPTOR_LEASE_BANK_READY &&
             (bank->pool_id == 0U || bank->epoch == 0U || count == 0U))) {
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
    result = wake_if_sleeping(
        &ring->request_sequence, &ring->request_wait_state);
    if (result != 0) return result;

    int64_t deadline_ns = 0;
    result = deadline_from_timeout(timeout_ms, &deadline_ns);
    if (result != 0) return result;
    result = wait_for_sequence(
        ring, &ring->completion_sequence, &ring->completion_wait_state,
        sequence, deadline_ns);
    if (result != 0) return result;
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
    result = wait_for_sequence(
        ring, &ring->request_sequence, &ring->request_wait_state,
        expected, deadline_ns);
    if (result != 0) return result;
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
    return wake_if_sleeping(
        &ring->completion_sequence, &ring->completion_wait_state);
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

int bvb_descriptor_lease_bank_disable(
    struct bvb_descriptor_transaction_ring *ring, size_t length,
    uint32_t bank_index) {
    int result = validate_header(ring, length, 0U);
    if (result != 0 || bank_index >= BVB_DESCRIPTOR_LEASE_BANK_COUNT)
        return result != 0 ? result : -EINVAL;
    struct bvb_descriptor_lease_bank *bank =
        lease_bank_at(ring, bank_index);
    store_u32(&bank->ready, 0U);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    memset(bank, 0, sizeof(*bank));
    return 0;
}

int bvb_descriptor_lease_bank_publish(
    struct bvb_descriptor_transaction_ring *ring, size_t length,
    uint32_t bank_index, uint64_t pool_id, uint64_t epoch,
    const struct bvb_descriptor_lease_record *records, uint32_t count) {
    int result = validate_header(ring, length, 0U);
    if (result != 0 || bank_index >= BVB_DESCRIPTOR_LEASE_BANK_COUNT ||
        pool_id == 0U || epoch == 0U || records == NULL || count == 0U ||
        count > BVB_DESCRIPTOR_LEASE_BANK_CAPACITY) {
        return result != 0 ? result : -EINVAL;
    }
    for (uint32_t index = 0U; index < count; ++index) {
        if (records[index].layout_id == 0U ||
            records[index].descriptor_set_id == 0U)
            return -EINVAL;
    }
    struct bvb_descriptor_lease_bank *bank =
        lease_bank_at(ring, bank_index);
    store_u32(&bank->ready, 0U);
    memset(bank, 0, sizeof(*bank));
    bank->pool_id = pool_id;
    bank->epoch = epoch;
    bank->count = count;
    memcpy(bank->records, records,
           (size_t)count * sizeof(bank->records[0]));
    __atomic_thread_fence(__ATOMIC_RELEASE);
    store_u32(&bank->ready, BVB_DESCRIPTOR_LEASE_BANK_READY);
    return 0;
}

int bvb_descriptor_lease_claim(
    struct bvb_descriptor_transaction_ring *ring, size_t length,
    uint64_t pool_id, const uint64_t *layout_ids, uint32_t count,
    uint64_t *descriptor_set_ids, uint64_t *epoch) {
    int result = validate_header(ring, length, 0U);
    if (result != 0 || pool_id == 0U || layout_ids == NULL || count == 0U ||
        count > BVB_DESCRIPTOR_LEASE_MAX_CLAIM ||
        descriptor_set_ids == NULL) {
        return result != 0 ? result : -EINVAL;
    }
    for (uint32_t bank_index = 0U;
         bank_index < BVB_DESCRIPTOR_LEASE_BANK_COUNT; ++bank_index) {
        struct bvb_descriptor_lease_bank *bank =
            lease_bank_at(ring, bank_index);
        if (load_u32(&bank->ready) != BVB_DESCRIPTOR_LEASE_BANK_READY ||
            bank->pool_id != pool_id) {
            continue;
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        const uint32_t cursor = load_u32(&bank->cursor);
        const uint32_t bank_count = load_u32(&bank->count);
        if (cursor > bank_count || count > bank_count - cursor)
            return -ENOENT;
        for (uint32_t index = 0U; index < count; ++index) {
            const struct bvb_descriptor_lease_record *record =
                &bank->records[cursor + index];
            if (record->layout_id != layout_ids[index] ||
                record->descriptor_set_id == 0U) {
                return -ENOENT;
            }
        }
        for (uint32_t index = 0U; index < count; ++index)
            descriptor_set_ids[index] =
                bank->records[cursor + index].descriptor_set_id;
        if (epoch != NULL) *epoch = bank->epoch;
        store_u32(&bank->cursor, cursor + count);
        return 0;
    }
    return -ENOENT;
}

int bvb_descriptor_lease_bank_cursor(
    const struct bvb_descriptor_transaction_ring *ring, size_t length,
    uint32_t bank_index, uint32_t *cursor, uint32_t *count) {
    int result = validate_header(ring, length, 0U);
    if (result != 0 || bank_index >= BVB_DESCRIPTOR_LEASE_BANK_COUNT ||
        cursor == NULL || count == NULL)
        return result != 0 ? result : -EINVAL;
    const struct bvb_descriptor_lease_bank *bank =
        lease_bank_at((struct bvb_descriptor_transaction_ring *)ring,
                      bank_index);
    if (load_u32(&bank->ready) != BVB_DESCRIPTOR_LEASE_BANK_READY)
        return -ENOENT;
    *cursor = load_u32(&bank->cursor);
    *count = load_u32(&bank->count);
    return *cursor <= *count ? 0 : -EPROTO;
}
