#ifndef BVB_DESCRIPTOR_TRANSACTION_RING_H
#define BVB_DESCRIPTOR_TRANSACTION_RING_H

#include <stddef.h>
#include <stdint.h>

enum {
    BVB_DESCRIPTOR_TRANSACTION_RING_MAGIC = 0x31524442U,
    BVB_DESCRIPTOR_TRANSACTION_RING_VERSION = 4,
    BVB_DESCRIPTOR_TRANSACTION_RING_CONTROL_BYTES = 128,
    BVB_DESCRIPTOR_TRANSACTION_RING_LEASE_OFFSET = 8192,
    BVB_DESCRIPTOR_TRANSACTION_RING_REGION_BYTES = 131072,
    BVB_DESCRIPTOR_TRANSACTION_RING_SLOT_COUNT = 16,
    BVB_DESCRIPTOR_TRANSACTION_RING_SLOT_BYTES = 384,
    BVB_DESCRIPTOR_TRANSACTION_RING_REQUEST_BYTES = 176,
    BVB_DESCRIPTOR_TRANSACTION_RING_RESPONSE_BYTES = 136,
    BVB_DESCRIPTOR_TRANSACTION_SLOT_AVAILABLE = 0,
    BVB_DESCRIPTOR_TRANSACTION_SLOT_REQUESTED = 1,
    BVB_DESCRIPTOR_TRANSACTION_SLOT_COMPLETED = 2,
    BVB_DESCRIPTOR_TRANSACTION_WAIT_IDLE = 0,
    BVB_DESCRIPTOR_TRANSACTION_WAIT_SPINNING = 1,
    BVB_DESCRIPTOR_TRANSACTION_WAIT_SLEEPING = 2,
    BVB_DESCRIPTOR_LEASE_BANK_COUNT = 64,
    BVB_DESCRIPTOR_LEASE_BANK_CAPACITY = 64,
    BVB_DESCRIPTOR_LEASE_MAX_CLAIM = 16,
    BVB_DESCRIPTOR_LEASE_BANK_READY = 1,
};

/*
 * Cross-libc descriptor request/completion ABI. The glibc client publishes
 * one canonical E127 transaction record, then the Bionic worker returns the
 * canonical allocation response in the same fixed slot. No pointer or native
 * Vulkan handle crosses this mapping.
 */
struct bvb_descriptor_transaction_ring {
    uint32_t magic;
    uint16_t version;
    uint16_t control_bytes;
    uint32_t slot_count;
    uint32_t flags;
    uint64_t generation;
    uint32_t request_sequence;
    uint32_t completion_sequence;
    int32_t client_status;
    int32_t service_status;
    uint32_t next_slot;
    uint32_t request_wait_state;
    uint32_t completion_wait_state;
    uint32_t reserved[19];
};

struct bvb_descriptor_transaction_slot {
    uint32_t state;
    uint32_t sequence;
    int32_t status;
    uint32_t request_length;
    uint32_t response_length;
    uint32_t flags;
    uint8_t request[BVB_DESCRIPTOR_TRANSACTION_RING_REQUEST_BYTES];
    uint8_t response[BVB_DESCRIPTOR_TRANSACTION_RING_RESPONSE_BYTES];
    uint8_t reserved[48];
};

struct bvb_descriptor_lease_record {
    uint64_t layout_id;
    uint64_t descriptor_set_id;
};

struct bvb_descriptor_lease_bank {
    uint64_t pool_id;
    uint64_t epoch;
    uint32_t count;
    uint32_t cursor;
    uint32_t ready;
    uint32_t reserved;
    struct bvb_descriptor_lease_record
        records[BVB_DESCRIPTOR_LEASE_BANK_CAPACITY];
};

_Static_assert(sizeof(struct bvb_descriptor_transaction_ring) ==
                   BVB_DESCRIPTOR_TRANSACTION_RING_CONTROL_BYTES,
               "descriptor transaction ring header must remain 128 bytes");
_Static_assert(sizeof(struct bvb_descriptor_transaction_slot) ==
                   BVB_DESCRIPTOR_TRANSACTION_RING_SLOT_BYTES,
               "descriptor transaction ring slot must remain 384 bytes");
_Static_assert(sizeof(struct bvb_descriptor_lease_record) == 16,
               "descriptor lease record must remain 16 bytes");
_Static_assert(sizeof(struct bvb_descriptor_lease_bank) == 1056,
               "descriptor lease bank must remain 1056 bytes");
_Static_assert(BVB_DESCRIPTOR_TRANSACTION_RING_LEASE_OFFSET +
                       BVB_DESCRIPTOR_LEASE_BANK_COUNT *
                           sizeof(struct bvb_descriptor_lease_bank) <=
                   BVB_DESCRIPTOR_TRANSACTION_RING_REGION_BYTES,
               "descriptor lease banks must fit the bounded shared region");

int bvb_descriptor_transaction_ring_initialize(
    void *address, size_t length, uint32_t slot_count, uint64_t generation);
int bvb_descriptor_transaction_ring_validate(
    const struct bvb_descriptor_transaction_ring *ring, size_t length,
    uint64_t generation);

int bvb_descriptor_transaction_ring_call(
    struct bvb_descriptor_transaction_ring *ring, size_t length,
    uint64_t generation, uint32_t sequence, const uint8_t *request,
    uint32_t request_length, uint8_t *response, uint32_t response_capacity,
    uint32_t *response_length, uint32_t timeout_ms);

int bvb_descriptor_transaction_ring_wait_request(
    struct bvb_descriptor_transaction_ring *ring, size_t length,
    uint64_t generation, uint32_t after_sequence, uint8_t *request,
    uint32_t request_capacity, uint32_t *request_length, uint32_t *slot_index,
    uint32_t *sequence, uint32_t timeout_ms);

int bvb_descriptor_transaction_ring_complete(
    struct bvb_descriptor_transaction_ring *ring, size_t length,
    uint64_t generation, uint32_t slot_index, uint32_t sequence, int status,
    const uint8_t *response, uint32_t response_length);

int bvb_descriptor_transaction_ring_fail_client(
    struct bvb_descriptor_transaction_ring *ring, int status);
int bvb_descriptor_transaction_ring_fail_service(
    struct bvb_descriptor_transaction_ring *ring, int status);

int bvb_descriptor_lease_bank_publish(
    struct bvb_descriptor_transaction_ring *ring, size_t length,
    uint32_t bank_index, uint64_t pool_id, uint64_t epoch,
    const struct bvb_descriptor_lease_record *records, uint32_t count);
int bvb_descriptor_lease_bank_disable(
    struct bvb_descriptor_transaction_ring *ring, size_t length,
    uint32_t bank_index);
int bvb_descriptor_lease_claim(
    struct bvb_descriptor_transaction_ring *ring, size_t length,
    uint64_t pool_id, const uint64_t *layout_ids, uint32_t count,
    uint64_t *descriptor_set_ids, uint64_t *epoch);
int bvb_descriptor_lease_bank_cursor(
    const struct bvb_descriptor_transaction_ring *ring, size_t length,
    uint32_t bank_index, uint32_t *cursor, uint32_t *count);

#endif
