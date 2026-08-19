#ifndef BVB_VISIBLE_INGRESS_H
#define BVB_VISIBLE_INGRESS_H

#include <bvb/lifecycle.h>

#include <stddef.h>
#include <stdint.h>

struct bvb_visible_ingress;

int bvb_visible_ingress_create(
    struct bvb_visible_ingress **output, const uint8_t *socket_name,
    size_t socket_name_length,
    const uint8_t token[BVB_LIFECYCLE_TOKEN_SIZE]);
int bvb_visible_ingress_create_loopback(
    struct bvb_visible_ingress **output, uint16_t requested_port,
    uint16_t *bound_port,
    const uint8_t token[BVB_LIFECYCLE_TOKEN_SIZE]);
int bvb_visible_ingress_install_region(struct bvb_visible_ingress *ingress,
                                       int memory_fd, size_t region_bytes,
                                       uint64_t generation);
int bvb_visible_ingress_wait_batch(struct bvb_visible_ingress *ingress,
                                   uint32_t timeout_ms,
                                   const uint8_t **batch,
                                   size_t *batch_length, uint64_t *sequence);
int bvb_visible_ingress_complete(struct bvb_visible_ingress *ingress,
                                 int status);
int bvb_visible_ingress_complete_and_accept_next(
    struct bvb_visible_ingress *ingress, int status);
void bvb_visible_ingress_destroy(struct bvb_visible_ingress *ingress);

#endif
