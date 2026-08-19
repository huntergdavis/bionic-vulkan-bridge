#ifndef BVB_HANDLE_H
#define BVB_HANDLE_H

#include <stddef.h>
#include <stdint.h>

enum bvb_object_type {
    BVB_OBJECT_INVALID = 0,
    BVB_OBJECT_INSTANCE = 1,
    BVB_OBJECT_PHYSICAL_DEVICE = 2,
    BVB_OBJECT_DEVICE = 3,
    BVB_OBJECT_QUEUE = 4,
    BVB_OBJECT_SURFACE = 5,
    BVB_OBJECT_SWAPCHAIN = 6,
    BVB_OBJECT_IMAGE = 7,
    BVB_OBJECT_IMAGE_VIEW = 8,
    BVB_OBJECT_DEVICE_MEMORY = 9,
    BVB_OBJECT_COMMAND_POOL = 10,
    BVB_OBJECT_COMMAND_BUFFER = 11,
    BVB_OBJECT_SHADER_MODULE = 12,
    BVB_OBJECT_RENDER_PASS = 13,
    BVB_OBJECT_FRAMEBUFFER = 14,
    BVB_OBJECT_PIPELINE_LAYOUT = 15,
    BVB_OBJECT_PIPELINE = 16,
    BVB_OBJECT_SEMAPHORE = 17,
    BVB_OBJECT_FENCE = 18,
};

enum {
    BVB_HANDLE_TYPE_SHIFT = 56,
};

#define BVB_HANDLE_SERIAL_MASK UINT64_C(0x00ffffffffffffff)

struct bvb_handle_entry {
    uint64_t wire_id;
    uint64_t parent_id;
    uint64_t native_bits;
    uint8_t state;
};

struct bvb_handle_table {
    struct bvb_handle_entry *entries;
    size_t capacity;
    size_t count;
};

uint64_t bvb_handle_id(enum bvb_object_type type, uint64_t serial);
enum bvb_object_type bvb_handle_type(uint64_t wire_id);
uint64_t bvb_handle_serial(uint64_t wire_id);
int bvb_handle_expect(uint64_t wire_id, enum bvb_object_type expected_type);

int bvb_handle_table_init(struct bvb_handle_table *table,
                          struct bvb_handle_entry *entries, size_t capacity);
int bvb_handle_table_insert(struct bvb_handle_table *table, uint64_t wire_id,
                            uint64_t parent_id, uint64_t native_bits);
int bvb_handle_table_lookup(const struct bvb_handle_table *table,
                            uint64_t wire_id,
                            enum bvb_object_type expected_type,
                            uint64_t *parent_id, uint64_t *native_bits);
int bvb_handle_table_remove(struct bvb_handle_table *table, uint64_t wire_id,
                            enum bvb_object_type expected_type,
                            uint64_t *native_bits);

#endif
