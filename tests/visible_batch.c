#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <bvb/command_batch.h>
#include <bvb/protocol.h>
#include <bvb/visible_batch.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #expression);                                               \
            return EXIT_FAILURE;                                                \
        }                                                                       \
    } while (0)

int main(void) {
    enum { REGION_BYTES = 4096, BATCH_OFFSET = 64 };
    uint8_t token[BVB_LIFECYCLE_TOKEN_SIZE];
    for (size_t index = 0U; index < sizeof(token); ++index) {
        token[index] = (uint8_t)(index + 1U);
    }
    struct bvb_visible_batch_region region;
    CHECK(bvb_visible_batch_region_init(&region, token) == 0);

    int memory_fd = (int)syscall(SYS_memfd_create, "bvb-visible-test",
                                 MFD_CLOEXEC | MFD_ALLOW_SEALING);
    CHECK(memory_fd >= 0);
    CHECK(ftruncate(memory_fd, REGION_BYTES) == 0);
    uint8_t *mapping = mmap(NULL, REGION_BYTES, PROT_READ | PROT_WRITE,
                            MAP_SHARED, memory_fd, 0);
    CHECK(mapping != MAP_FAILED);
    struct bvb_command_batch_builder builder;
    const uint64_t command_buffer_id =
        bvb_handle_id(BVB_OBJECT_COMMAND_BUFFER, 1U);
    CHECK(bvb_command_batch_begin(&builder, mapping + BATCH_OFFSET,
                                  REGION_BYTES - BATCH_OFFSET,
                                  command_buffer_id, 7U) == 0);
    CHECK(bvb_command_batch_append_draw(
              &builder, &(const struct bvb_draw_command){
                            .vertex_count = 3U,
                            .instance_count = 1U,
                        }) == 0);
    size_t encoded_length = 0U;
    CHECK(bvb_command_batch_finish(&builder, &encoded_length) == 0);
    CHECK(fcntl(memory_fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK) == 0);

    struct bvb_visible_batch_setup setup = {
        .shared = {
            .region_bytes = REGION_BYTES,
            .generation = UINT64_C(0x1122334455667788),
        },
    };
    memcpy(setup.token, token, sizeof(token));
    uint8_t setup_wire[BVB_VISIBLE_BATCH_SETUP_SIZE];
    CHECK(bvb_protocol_encode_visible_batch_setup(setup_wire, &setup) == 0);
    CHECK(bvb_visible_batch_region_setup(&region, setup_wire,
                                         sizeof(setup_wire), memory_fd) == 0);
    CHECK(bvb_visible_batch_region_setup(&region, setup_wire,
                                         sizeof(setup_wire), memory_fd) ==
          -EALREADY);

    struct bvb_visible_batch_execute execute = {
        .shared = {
            .generation = setup.shared.generation,
            .offset = BATCH_OFFSET,
            .length = (uint32_t)encoded_length,
            .sequence = 7U,
        },
    };
    memcpy(execute.token, token, sizeof(token));
    uint8_t execute_wire[BVB_VISIBLE_BATCH_EXECUTE_SIZE];
    CHECK(bvb_protocol_encode_visible_batch_execute(execute_wire, &execute) ==
          0);
    atomic_thread_fence(memory_order_release);
    const uint8_t *batch = NULL;
    size_t batch_length = 0U;
    uint64_t sequence = 0U;
    CHECK(bvb_visible_batch_region_execute(
              &region, execute_wire, sizeof(execute_wire), &batch,
              &batch_length, &sequence) == 0);
    CHECK(batch == region.address + BATCH_OFFSET);
    CHECK(batch_length == encoded_length);
    CHECK(sequence == 7U);
    CHECK(bvb_visible_batch_region_execute(
              &region, execute_wire, sizeof(execute_wire), &batch,
              &batch_length, &sequence) == -EALREADY);

    execute.shared.sequence = 8U;
    CHECK(bvb_protocol_encode_visible_batch_execute(execute_wire, &execute) ==
          0);
    CHECK(bvb_visible_batch_region_execute(
              &region, execute_wire, sizeof(execute_wire), &batch,
              &batch_length, &sequence) == -EPROTO);
    execute.shared.sequence = 7U;
    execute.shared.generation += 1U;
    CHECK(bvb_protocol_encode_visible_batch_execute(execute_wire, &execute) ==
          0);
    CHECK(bvb_visible_batch_region_execute(
              &region, execute_wire, sizeof(execute_wire), &batch,
              &batch_length, &sequence) == -ESTALE);
    execute.shared.generation = setup.shared.generation;
    execute.shared.offset = REGION_BYTES - 16U;
    execute.shared.sequence = 9U;
    CHECK(bvb_protocol_encode_visible_batch_execute(execute_wire, &execute) ==
          0);
    CHECK(bvb_visible_batch_region_execute(
              &region, execute_wire, sizeof(execute_wire), &batch,
              &batch_length, &sequence) == -ERANGE);

    uint8_t inline_payload[BVB_PROTOCOL_MAX_PAYLOAD] = {0};
    memcpy(inline_payload, token, sizeof(token));
    memcpy(inline_payload + BVB_VISIBLE_BATCH_INLINE_PREFIX_SIZE,
           mapping + BATCH_OFFSET, encoded_length);
    const size_t inline_length =
        BVB_VISIBLE_BATCH_INLINE_PREFIX_SIZE + encoded_length;
    CHECK(bvb_visible_batch_inline_decode(
              token, inline_payload, inline_length, &batch, &batch_length,
              &sequence) == 0);
    CHECK(batch == inline_payload + BVB_VISIBLE_BATCH_INLINE_PREFIX_SIZE);
    CHECK(batch_length == encoded_length);
    CHECK(sequence == 7U);
    inline_payload[0] ^= 1U;
    CHECK(bvb_visible_batch_inline_decode(
              token, inline_payload, inline_length, &batch, &batch_length,
              &sequence) == -EACCES);
    inline_payload[0] ^= 1U;
    CHECK(bvb_visible_batch_inline_decode(
              token, inline_payload,
              BVB_VISIBLE_BATCH_INLINE_PREFIX_SIZE +
                  BVB_COMMAND_BATCH_HEADER_SIZE - 1U,
              &batch, &batch_length, &sequence) == -EINVAL);
    inline_payload[BVB_VISIBLE_BATCH_INLINE_PREFIX_SIZE] ^= 1U;
    CHECK(bvb_visible_batch_inline_decode(
              token, inline_payload, inline_length, &batch, &batch_length,
              &sequence) == -EPROTO);

    bvb_visible_batch_region_destroy(&region);
    CHECK(munmap(mapping, REGION_BYTES) == 0);
    CHECK(close(memory_fd) == 0);
    puts("PASS: authenticated visible shared-batch region");
    return EXIT_SUCCESS;
}
