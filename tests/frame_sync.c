#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <bvb/frame_sync.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #expression);                                               \
            return 1;                                                           \
        }                                                                       \
    } while (0)

enum { TEST_FRAMES = 256 };

int main(void) {
    void *mapping = mmap(NULL, BVB_FRAME_SYNC_REGION_BYTES,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(mapping != MAP_FAILED);
    struct bvb_frame_sync_control *control = mapping;
    CHECK(bvb_frame_sync_initialize(mapping, BVB_FRAME_SYNC_REGION_BYTES,
                                    TEST_FRAMES) == 0);
    CHECK(bvb_frame_sync_validate(control) == 0);

    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        for (uint32_t expected = 1U; expected <= TEST_FRAMES; ++expected) {
            uint32_t sequence = 0U;
            if (bvb_frame_sync_wait_producer(
                    control, expected - 1U, 5000U, &sequence) != 0 ||
                sequence != expected ||
                bvb_frame_sync_publish_consumer(control, sequence) != 0) {
                _exit(2);
            }
        }
        _exit(0);
    }

    for (uint32_t sequence = 1U; sequence <= TEST_FRAMES; ++sequence) {
        CHECK(bvb_frame_sync_publish_producer(control, sequence) == 0);
        uint32_t consumed = 0U;
        CHECK(bvb_frame_sync_wait_consumer(control, sequence - 1U, 5000U,
                                           &consumed) == 0);
        CHECK(consumed == sequence);
    }
    int child_status = 0;
    CHECK(waitpid(child, &child_status, 0) == child);
    CHECK(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    CHECK(control->producer_sequence == TEST_FRAMES);
    CHECK(control->consumer_sequence == TEST_FRAMES);
    CHECK(bvb_frame_sync_publish_producer(control, TEST_FRAMES + 1U) ==
          -ERANGE);

    CHECK(bvb_frame_sync_initialize(mapping, BVB_FRAME_SYNC_REGION_BYTES, 1U) ==
          0);
    uint32_t sequence = 0U;
    CHECK(bvb_frame_sync_wait_producer(control, 0U, 5U, &sequence) ==
          -ETIMEDOUT);
    CHECK(bvb_frame_sync_fail_producer(control, -EIO) == 0);
    CHECK(bvb_frame_sync_wait_producer(control, 0U, 5U, &sequence) == -EIO);
    CHECK(bvb_frame_sync_initialize(mapping, BVB_FRAME_SYNC_REGION_BYTES, 1U) ==
          0);
    CHECK(bvb_frame_sync_fail_consumer(control, -ECANCELED) == 0);
    CHECK(bvb_frame_sync_publish_producer(control, 1U) == -ECANCELED);
    CHECK(munmap(mapping, BVB_FRAME_SYNC_REGION_BYTES) == 0);
    puts("PASS: cross-process frame sync contract");
    return 0;
}
