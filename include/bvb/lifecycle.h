#ifndef BVB_LIFECYCLE_H
#define BVB_LIFECYCLE_H

#include <stdint.h>

enum {
    BVB_LIFECYCLE_MAGIC = 0x314c5642,
    BVB_LIFECYCLE_VERSION = 1,
    BVB_LIFECYCLE_TOKEN_SIZE = 32,
    BVB_LIFECYCLE_TOKEN_HEX_SIZE = 64,
    BVB_LIFECYCLE_RECORD_SIZE = 64,
    BVB_LIFECYCLE_ACK_SIZE = 16,
    BVB_LIFECYCLE_EVENT_CREATED = 1,
    BVB_LIFECYCLE_EVENT_STARTED = 2,
    BVB_LIFECYCLE_EVENT_RESUMED = 3,
    BVB_LIFECYCLE_EVENT_PAUSED = 4,
    BVB_LIFECYCLE_EVENT_STOPPED = 5,
    BVB_LIFECYCLE_EVENT_DESTROYED = 6,
    BVB_LIFECYCLE_EVENT_WINDOW_CREATED = 7,
    BVB_LIFECYCLE_EVENT_WINDOW_DESTROYED = 8,
    BVB_LIFECYCLE_EVENT_FOCUS_GAINED = 9,
    BVB_LIFECYCLE_EVENT_FOCUS_LOST = 10,
    BVB_LIFECYCLE_EVENT_RENDERER_READY = 11,
    BVB_LIFECYCLE_EVENT_RENDERER_FAILED = 12,
    BVB_ACTIVITY_CREATED = 1U << 0,
    BVB_ACTIVITY_STARTED = 1U << 1,
    BVB_ACTIVITY_RESUMED = 1U << 2,
    BVB_ACTIVITY_WINDOW_PRESENT = 1U << 3,
    BVB_ACTIVITY_RENDERER_READY = 1U << 4,
    BVB_ACTIVITY_FOCUSED = 1U << 5,
    BVB_ACTIVITY_DESTROYED = 1U << 6,
};

struct bvb_lifecycle_record {
    uint16_t event;
    uint32_t sequence;
    uint32_t width;
    uint32_t height;
    uint32_t activity_pid;
    uint64_t monotonic_ns;
    uint8_t token[BVB_LIFECYCLE_TOKEN_SIZE];
};

struct bvb_lifecycle_ack {
    uint32_t sequence;
    int32_t status;
};

struct bvb_activity_status {
    uint32_t ingress_configured;
    uint32_t authenticated_event_count;
    uint32_t rejected_event_count;
    uint32_t last_sequence;
    uint32_t last_event;
    uint32_t state_flags;
    uint32_t width;
    uint32_t height;
    uint32_t activity_pid;
    uint64_t last_event_monotonic_ns;
    uint64_t last_event_received_ns;
};

int bvb_lifecycle_token_from_hex(
    const char *input,
    uint8_t output[BVB_LIFECYCLE_TOKEN_SIZE]);
int bvb_lifecycle_encode_record(
    uint8_t output[BVB_LIFECYCLE_RECORD_SIZE],
    const struct bvb_lifecycle_record *record);
int bvb_lifecycle_decode_record(
    const uint8_t input[BVB_LIFECYCLE_RECORD_SIZE],
    struct bvb_lifecycle_record *record);
int bvb_lifecycle_encode_ack(
    uint8_t output[BVB_LIFECYCLE_ACK_SIZE],
    const struct bvb_lifecycle_ack *ack);
int bvb_lifecycle_decode_ack(
    const uint8_t input[BVB_LIFECYCLE_ACK_SIZE],
    struct bvb_lifecycle_ack *ack);

#endif
