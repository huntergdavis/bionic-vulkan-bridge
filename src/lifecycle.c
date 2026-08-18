#include <bvb/lifecycle.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static void put_u16(uint8_t *output, uint16_t value) {
    output[0] = (uint8_t)(value & 0xffU);
    output[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *output, uint32_t value) {
    output[0] = (uint8_t)(value & 0xffU);
    output[1] = (uint8_t)((value >> 8) & 0xffU);
    output[2] = (uint8_t)((value >> 16) & 0xffU);
    output[3] = (uint8_t)(value >> 24);
}

static void put_u64(uint8_t *output, uint64_t value) {
    put_u32(output, (uint32_t)value);
    put_u32(output + 4, (uint32_t)(value >> 32));
}

static uint16_t get_u16(const uint8_t *input) {
    return (uint16_t)((uint16_t)input[0] | ((uint16_t)input[1] << 8));
}

static uint32_t get_u32(const uint8_t *input) {
    return (uint32_t)input[0] | ((uint32_t)input[1] << 8) |
           ((uint32_t)input[2] << 16) | ((uint32_t)input[3] << 24);
}

static uint64_t get_u64(const uint8_t *input) {
    return (uint64_t)get_u32(input) | ((uint64_t)get_u32(input + 4) << 32);
}

static int hex_nibble(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static int record_is_valid(const struct bvb_lifecycle_record *record) {
    if (record == NULL || record->event < BVB_LIFECYCLE_EVENT_CREATED ||
        record->event > BVB_LIFECYCLE_EVENT_RENDERER_FAILED ||
        record->sequence == 0U || record->activity_pid == 0U ||
        record->monotonic_ns == 0U) {
        return -EINVAL;
    }
    if ((record->event == BVB_LIFECYCLE_EVENT_WINDOW_CREATED ||
         record->event == BVB_LIFECYCLE_EVENT_RENDERER_READY ||
         record->event == BVB_LIFECYCLE_EVENT_RENDERER_FAILED) &&
        (record->width == 0U || record->height == 0U)) {
        return -EINVAL;
    }
    return 0;
}

int bvb_lifecycle_token_from_hex(
    const char *input,
    uint8_t output[BVB_LIFECYCLE_TOKEN_SIZE]) {
    if (input == NULL || output == NULL ||
        strlen(input) != BVB_LIFECYCLE_TOKEN_HEX_SIZE) {
        return -EINVAL;
    }
    for (size_t index = 0; index < BVB_LIFECYCLE_TOKEN_SIZE; ++index) {
        int high = hex_nibble(input[index * 2U]);
        int low = hex_nibble(input[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            memset(output, 0, BVB_LIFECYCLE_TOKEN_SIZE);
            return -EINVAL;
        }
        output[index] = (uint8_t)((unsigned int)high << 4U) | (uint8_t)low;
    }
    return 0;
}

int bvb_lifecycle_encode_record(
    uint8_t output[BVB_LIFECYCLE_RECORD_SIZE],
    const struct bvb_lifecycle_record *record) {
    if (output == NULL) {
        return -EINVAL;
    }
    int result = record_is_valid(record);
    if (result != 0) {
        return result;
    }
    put_u32(output, BVB_LIFECYCLE_MAGIC);
    put_u16(output + 4, BVB_LIFECYCLE_VERSION);
    put_u16(output + 6, record->event);
    put_u32(output + 8, record->sequence);
    put_u32(output + 12, record->width);
    put_u32(output + 16, record->height);
    put_u32(output + 20, record->activity_pid);
    put_u64(output + 24, record->monotonic_ns);
    memcpy(output + 32, record->token, BVB_LIFECYCLE_TOKEN_SIZE);
    return 0;
}

int bvb_lifecycle_decode_record(
    const uint8_t input[BVB_LIFECYCLE_RECORD_SIZE],
    struct bvb_lifecycle_record *record) {
    if (input == NULL || record == NULL) {
        return -EINVAL;
    }
    if (get_u32(input) != BVB_LIFECYCLE_MAGIC ||
        get_u16(input + 4) != BVB_LIFECYCLE_VERSION) {
        return -EPROTO;
    }
    struct bvb_lifecycle_record decoded = {
        .event = get_u16(input + 6),
        .sequence = get_u32(input + 8),
        .width = get_u32(input + 12),
        .height = get_u32(input + 16),
        .activity_pid = get_u32(input + 20),
        .monotonic_ns = get_u64(input + 24),
    };
    memcpy(decoded.token, input + 32, BVB_LIFECYCLE_TOKEN_SIZE);
    int result = record_is_valid(&decoded);
    if (result != 0) {
        return -EPROTO;
    }
    *record = decoded;
    return 0;
}

int bvb_lifecycle_encode_ack(
    uint8_t output[BVB_LIFECYCLE_ACK_SIZE],
    const struct bvb_lifecycle_ack *ack) {
    if (output == NULL || ack == NULL || ack->sequence == 0U ||
        ack->status > 0) {
        return -EINVAL;
    }
    put_u32(output, BVB_LIFECYCLE_MAGIC);
    put_u16(output + 4, BVB_LIFECYCLE_VERSION);
    put_u16(output + 6, 0U);
    put_u32(output + 8, ack->sequence);
    put_u32(output + 12, (uint32_t)ack->status);
    return 0;
}

int bvb_lifecycle_decode_ack(
    const uint8_t input[BVB_LIFECYCLE_ACK_SIZE],
    struct bvb_lifecycle_ack *ack) {
    if (input == NULL || ack == NULL) {
        return -EINVAL;
    }
    if (get_u32(input) != BVB_LIFECYCLE_MAGIC ||
        get_u16(input + 4) != BVB_LIFECYCLE_VERSION ||
        get_u16(input + 6) != 0U) {
        return -EPROTO;
    }
    struct bvb_lifecycle_ack decoded = {
        .sequence = get_u32(input + 8),
        .status = (int32_t)get_u32(input + 12),
    };
    if (decoded.sequence == 0U || decoded.status > 0) {
        return -EPROTO;
    }
    *ack = decoded;
    return 0;
}
