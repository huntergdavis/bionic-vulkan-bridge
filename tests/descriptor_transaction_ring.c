#include <bvb/descriptor_transaction_ring.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,         \
                    __LINE__, #condition);                                     \
            return 1;                                                          \
        }                                                                      \
    } while (0)

struct worker_args {
    struct bvb_descriptor_transaction_ring *ring;
    uint64_t generation;
    int status;
};

static void *worker_main(void *opaque) {
    struct worker_args *args = opaque;
    uint32_t after = 0U;
    for (uint32_t expected = 1U; expected <= 4096U; ++expected) {
        uint8_t request[BVB_DESCRIPTOR_TRANSACTION_RING_REQUEST_BYTES];
        uint32_t request_length = 0U;
        uint32_t slot = 0U;
        uint32_t sequence = 0U;
        int result = bvb_descriptor_transaction_ring_wait_request(
            args->ring, BVB_DESCRIPTOR_TRANSACTION_RING_REGION_BYTES,
            args->generation, after, request, sizeof(request),
            &request_length, &slot, &sequence, 1000U);
        if (result != 0 || sequence != expected || request_length != 8U) {
            args->status = result != 0 ? result : -1;
            return NULL;
        }
        uint8_t response[8];
        for (uint32_t index = 0U; index < sizeof(response); ++index) {
            response[index] = (uint8_t)(request[index] ^ UINT8_C(0xa5));
        }
        result = bvb_descriptor_transaction_ring_complete(
            args->ring, BVB_DESCRIPTOR_TRANSACTION_RING_REGION_BYTES,
            args->generation, slot, sequence, 0, response,
            sizeof(response));
        if (result != 0) {
            args->status = result;
            return NULL;
        }
        after = sequence;
    }
    return NULL;
}

int main(void) {
    _Alignas(4096) uint8_t region[
        BVB_DESCRIPTOR_TRANSACTION_RING_REGION_BYTES];
    const uint64_t generation = UINT64_C(0x9182736455aa7711);
    CHECK(bvb_descriptor_transaction_ring_initialize(
              region, sizeof(region),
              BVB_DESCRIPTOR_TRANSACTION_RING_SLOT_COUNT,
              generation) == 0);
    struct bvb_descriptor_transaction_ring *ring = (void *)region;
    CHECK(bvb_descriptor_transaction_ring_validate(
              ring, sizeof(region), generation) == 0);
    struct worker_args args = {
        .ring = ring,
        .generation = generation,
    };
    pthread_t worker;
    CHECK(pthread_create(&worker, NULL, worker_main, &args) == 0);
    for (uint32_t sequence = 1U; sequence <= 4096U; ++sequence) {
        uint8_t request[8];
        for (uint32_t index = 0U; index < sizeof(request); ++index) {
            request[index] = (uint8_t)(sequence + index);
        }
        uint8_t response[8] = {0};
        uint32_t response_length = 0U;
        CHECK(bvb_descriptor_transaction_ring_call(
                  ring, sizeof(region), generation, sequence,
                  request, sizeof(request), response, sizeof(response),
                  &response_length, 1000U) == 0);
        CHECK(response_length == sizeof(response));
        for (uint32_t index = 0U; index < sizeof(response); ++index) {
            CHECK(response[index] ==
                  (uint8_t)(request[index] ^ UINT8_C(0xa5)));
        }
    }
    CHECK(pthread_join(worker, NULL) == 0);
    CHECK(args.status == 0);
    CHECK(ring->request_sequence == 4096U);
    CHECK(ring->completion_sequence == 4096U);
    CHECK(bvb_descriptor_transaction_ring_fail_service(ring, -5) == 0);
    uint8_t byte = 0U;
    uint32_t length = 0U;
    CHECK(bvb_descriptor_transaction_ring_call(
              ring, sizeof(region), generation, 4097U, &byte, 1U,
              &byte, 1U, &length, 1U) == -5);
    puts("PASS: descriptor transaction ring 4096 ordered calls and wrap");
    return 0;
}
