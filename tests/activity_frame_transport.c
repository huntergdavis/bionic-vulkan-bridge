#include <bvb/activity_frame_transport.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #expression);                                               \
            return 1;                                                           \
        }                                                                       \
    } while (0)

int main(void) {
    const struct bvb_activity_frame_setup expected = {
        .magic = BVB_ACTIVITY_FRAME_SETUP_MAGIC,
        .version = BVB_ACTIVITY_FRAME_SETUP_VERSION,
        .header_bytes = BVB_ACTIVITY_FRAME_SETUP_BYTES,
        .image_count = 3U,
        .width = 1280U,
        .height = 800U,
        .format = 37U,
        .image_usage = 0x10U,
        .flags = BVB_ACTIVITY_FRAME_FLAG_DMA_BUF,
        .generation = UINT64_C(0xe057000000000001),
        .allocation_sizes = {UINT64_C(0x100000), UINT64_C(0x100000),
                             UINT64_C(0x100000), 0U},
        .memory_type_indices = {2U, 2U, 2U, 0U},
    };
    uint8_t wire[BVB_ACTIVITY_FRAME_SETUP_BYTES];
    CHECK(bvb_activity_frame_setup_encode(wire, &expected) == 0);
    struct bvb_activity_frame_setup actual;
    CHECK(bvb_activity_frame_setup_decode(wire, &actual) == 0);
    CHECK(memcmp(&actual, &expected, sizeof(actual)) == 0);
    wire[127] = 1U;
    CHECK(bvb_activity_frame_setup_decode(wire, &actual) == -EPROTO);
    struct bvb_activity_frame_setup invalid = expected;
    invalid.flags = UINT32_C(1) << 1;
    CHECK(bvb_activity_frame_setup_encode(wire, &invalid) == -EINVAL);
    puts("PASS: fixed-width Activity frame setup envelope");
    return 0;
}
