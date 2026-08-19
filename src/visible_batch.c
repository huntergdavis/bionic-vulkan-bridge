#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <bvb/command_batch.h>
#include <bvb/protocol.h>
#include <bvb/visible_batch.h>

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static bool token_matches(const uint8_t *left, const uint8_t *right) {
    uint8_t difference = 0U;
    for (size_t index = 0U; index < BVB_LIFECYCLE_TOKEN_SIZE; ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0U;
}

static bool token_is_nonzero(const uint8_t *token) {
    uint8_t combined = 0U;
    for (size_t index = 0U; index < BVB_LIFECYCLE_TOKEN_SIZE; ++index) {
        combined |= token[index];
    }
    return combined != 0U;
}

int bvb_visible_batch_region_init(
    struct bvb_visible_batch_region *region,
    const uint8_t token[BVB_LIFECYCLE_TOKEN_SIZE]) {
    if (region == NULL || token == NULL || !token_is_nonzero(token)) {
        return -EINVAL;
    }
    memset(region, 0, sizeof(*region));
    memcpy(region->token, token, sizeof(region->token));
    return 0;
}

int bvb_visible_batch_region_setup(struct bvb_visible_batch_region *region,
                                   const uint8_t *payload,
                                   size_t payload_length, int memory_fd) {
    if (region == NULL || payload == NULL ||
        payload_length != BVB_VISIBLE_BATCH_SETUP_SIZE || memory_fd < 0) {
        return -EINVAL;
    }
    if (region->address != NULL) {
        return -EALREADY;
    }
    struct bvb_visible_batch_setup setup;
    int result = bvb_protocol_decode_visible_batch_setup(payload, &setup);
    if (result != 0) {
        return result;
    }
    if (!token_matches(region->token, setup.token)) {
        return -EACCES;
    }
    struct stat status;
    if (fstat(memory_fd, &status) != 0) {
        return -errno;
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
        (uint64_t)status.st_size != setup.shared.region_bytes) {
        return -EINVAL;
    }
    int seals = fcntl(memory_fd, F_GET_SEALS);
    if (seals < 0) {
        return -errno;
    }
    const int required_seals = F_SEAL_GROW | F_SEAL_SHRINK;
    if ((seals & required_seals) != required_seals) {
        return -EPERM;
    }
    void *address = mmap(NULL, setup.shared.region_bytes, PROT_READ, MAP_SHARED,
                         memory_fd, 0);
    if (address == MAP_FAILED) {
        return -errno;
    }
    region->address = address;
    region->length = setup.shared.region_bytes;
    region->generation = setup.shared.generation;
    return 0;
}

int bvb_visible_batch_region_execute(
    struct bvb_visible_batch_region *region, const uint8_t *payload,
    size_t payload_length, const uint8_t **batch, size_t *batch_length,
    uint64_t *sequence) {
    if (region == NULL || payload == NULL || batch == NULL ||
        batch_length == NULL || sequence == NULL ||
        payload_length != BVB_VISIBLE_BATCH_EXECUTE_SIZE) {
        return -EINVAL;
    }
    *batch = NULL;
    *batch_length = 0U;
    *sequence = 0U;
    if (region->address == NULL) {
        return -ENXIO;
    }
    struct bvb_visible_batch_execute execute;
    int result = bvb_protocol_decode_visible_batch_execute(payload, &execute);
    if (result != 0) {
        return result;
    }
    if (!token_matches(region->token, execute.token)) {
        return -EACCES;
    }
    if (execute.shared.generation != region->generation) {
        return -ESTALE;
    }
    if (execute.shared.sequence <= region->last_sequence) {
        return -EALREADY;
    }
    if ((size_t)execute.shared.offset > region->length ||
        (size_t)execute.shared.length >
            region->length - (size_t)execute.shared.offset) {
        return -ERANGE;
    }
    atomic_thread_fence(memory_order_acquire);
    const uint8_t *candidate =
        region->address + (size_t)execute.shared.offset;
    struct bvb_command_batch_info info;
    result = bvb_command_batch_validate(candidate, execute.shared.length,
                                        &info);
    if (result != 0) {
        return result;
    }
    if (info.sequence != execute.shared.sequence) {
        return -EPROTO;
    }
    region->last_sequence = execute.shared.sequence;
    *batch = candidate;
    *batch_length = execute.shared.length;
    *sequence = execute.shared.sequence;
    return 0;
}

void bvb_visible_batch_region_destroy(struct bvb_visible_batch_region *region) {
    if (region == NULL) {
        return;
    }
    if (region->address != NULL) {
        (void)munmap((void *)region->address, region->length);
    }
    memset(region, 0, sizeof(*region));
}
