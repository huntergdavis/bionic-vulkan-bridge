#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <bvb/command_batch.h>
#include <bvb/lifecycle.h>
#include <bvb/triangle_batch_builder.h>
#include <bvb/visible_ingress.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/memfd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

enum {
    BROKERED_REGION_BYTES = 4096,
    BROKERED_BATCH_OFFSET = 64,
    BROKERED_BATCH_STRIDE = 256,
    BROKERED_RING_FRAMES = 2,
};

static int validate_triangle(const uint8_t *batch, size_t batch_length,
                             uint64_t sequence, uint32_t width,
                             uint32_t height) {
    struct bvb_command_batch_info info;
    int result = bvb_command_batch_validate(batch, batch_length, &info);
    if (result != 0 || info.command_count != 6U ||
        info.command_buffer_id !=
            bvb_handle_id(BVB_OBJECT_COMMAND_BUFFER, 1U) ||
        info.sequence != sequence) {
        return result != 0 ? result : -EPROTO;
    }
    struct bvb_command_batch_iterator iterator;
    result = bvb_command_batch_iterator_init(&iterator, batch, batch_length);
    if (result != 0) {
        return result;
    }
    static const uint16_t expected_opcodes[] = {
        BVB_COMMAND_BEGIN_RENDERING,
        BVB_COMMAND_BIND_GRAPHICS_PIPELINE,
        BVB_COMMAND_SET_VIEWPORT,
        BVB_COMMAND_SET_SCISSOR,
        BVB_COMMAND_DRAW,
        BVB_COMMAND_END_RENDERING,
    };
    struct bvb_command_record record;
    for (size_t index = 0U;
         index < sizeof(expected_opcodes) / sizeof(expected_opcodes[0]);
         ++index) {
        result = bvb_command_batch_next(&iterator, &record);
        if (result != 0 || record.opcode != expected_opcodes[index]) {
            return -EPROTO;
        }
        if (index == 0U) {
            struct bvb_begin_rendering_command begin;
            result = bvb_command_decode_begin_rendering(&record, &begin);
            if (result != 0 || begin.width != width ||
                begin.height != height || begin.clear_color[0] != 0.25F ||
                begin.clear_color[3] != 1.0F) {
                return result != 0 ? result : -EPROTO;
            }
        }
    }
    return bvb_command_batch_next(&iterator, &record) == 1 ? 0 : -EPROTO;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr,
                "usage: %s (SOCKET_NAME | --tcp | --tcp-brokered | "
                "--tcp-brokered-ring) "
                "TOKEN_HEX WIDTH HEIGHT\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    uint8_t token[BVB_LIFECYCLE_TOKEN_SIZE];
    char *width_end = NULL;
    char *height_end = NULL;
    unsigned long width_value = strtoul(argv[3], &width_end, 10);
    unsigned long height_value = strtoul(argv[4], &height_end, 10);
    if (bvb_lifecycle_token_from_hex(argv[2], token) != 0 ||
        width_end == argv[3] || *width_end != '\0' ||
        height_end == argv[4] || *height_end != '\0' || width_value == 0U ||
        width_value > UINT32_MAX || height_value == 0U ||
        height_value > UINT32_MAX) {
        return EXIT_FAILURE;
    }

    struct bvb_visible_ingress *ingress = NULL;
    uint16_t bound_port = 0U;
    const bool brokered = strcmp(argv[1], "--tcp-brokered") == 0 ||
                          strcmp(argv[1], "--tcp-brokered-ring") == 0;
    const size_t frame_count =
        strcmp(argv[1], "--tcp-brokered-ring") == 0
            ? BROKERED_RING_FRAMES
            : 1U;
    int result = (strcmp(argv[1], "--tcp") == 0 || brokered)
                     ? bvb_visible_ingress_create_loopback(
                           &ingress, 0U, &bound_port, token)
                     : bvb_visible_ingress_create(
                           &ingress, (const uint8_t *)argv[1],
                           strlen(argv[1]), token);
    if (result != 0) {
        fprintf(stderr, "ingress create failed: %d\n", result);
        return EXIT_FAILURE;
    }

    int region_fd = -1;
    uint8_t *region_mapping = MAP_FAILED;
    if (brokered) {
        region_fd = (int)syscall(SYS_memfd_create, "bvb-ingress-test",
                                 MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (region_fd < 0 || ftruncate(region_fd, BROKERED_REGION_BYTES) != 0) {
            result = -errno;
        }
        if (result == 0) {
            region_mapping = mmap(NULL, BROKERED_REGION_BYTES,
                                  PROT_READ | PROT_WRITE, MAP_SHARED,
                                  region_fd, 0);
            if (region_mapping == MAP_FAILED) {
                result = -errno;
            }
        }
        for (size_t index = 0U; result == 0 && index < frame_count;
             ++index) {
            const size_t offset =
                BROKERED_BATCH_OFFSET + index * BROKERED_BATCH_STRIDE;
            size_t encoded_length = 0U;
            result = bvb_triangle_batch_build_sequence(
                region_mapping + offset, BROKERED_REGION_BYTES - offset,
                (uint32_t)width_value, (uint32_t)height_value, index + 1U,
                &encoded_length);
            if (result == 0 && encoded_length != 200U) {
                result = -EPROTO;
            }
        }
        if (result == 0 &&
            fcntl(region_fd, F_ADD_SEALS,
                  F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0) {
            result = -errno;
        }
        if (result == 0) {
            result = bvb_visible_ingress_install_region(
                ingress, region_fd, BROKERED_REGION_BYTES, 1U);
        }
        if (result != 0) {
            fprintf(stderr, "brokered region install failed: %d\n", result);
            bvb_visible_ingress_destroy(ingress);
            if (region_mapping != MAP_FAILED) {
                (void)munmap(region_mapping, BROKERED_REGION_BYTES);
            }
            if (region_fd >= 0) {
                (void)close(region_fd);
            }
            return EXIT_FAILURE;
        }
    }
    if (bound_port != 0U) {
        printf("READY %u\n", (unsigned int)bound_port);
    } else {
        puts("READY");
    }
    (void)fflush(stdout);

    size_t batch_length = 0U;
    uint64_t sequence = 0U;
    for (size_t index = 0U; result == 0 && index < frame_count; ++index) {
        const uint8_t *batch = NULL;
        result = bvb_visible_ingress_wait_batch(
            ingress, 5000U, &batch, &batch_length, &sequence);
        if (result == 0) {
            result = validate_triangle(batch, batch_length, sequence,
                                       (uint32_t)width_value,
                                       (uint32_t)height_value);
            int complete_status =
                bvb_visible_ingress_complete(ingress, result);
            if (result == 0) {
                result = complete_status;
            }
        }
    }
    bvb_visible_ingress_destroy(ingress);
    if (region_mapping != MAP_FAILED) {
        (void)munmap(region_mapping, BROKERED_REGION_BYTES);
    }
    if (region_fd >= 0) {
        (void)close(region_fd);
    }
    if (result != 0) {
        fprintf(stderr, "ingress batch failed: %d\n", result);
        return EXIT_FAILURE;
    }
    printf("{\"batch_bytes\":%zu,\"sequence\":%" PRIu64
           ",\"commands\":6,\"frames\":%zu}\n",
           batch_length, sequence, frame_count);
    return EXIT_SUCCESS;
}
