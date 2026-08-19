#include <bvb/handle.h>

#include <errno.h>
#include <string.h>

enum {
    BVB_HANDLE_EMPTY = 0,
    BVB_HANDLE_OCCUPIED = 1,
    BVB_HANDLE_TOMBSTONE = 2,
};

static int object_type_is_valid(enum bvb_object_type type) {
    return type >= BVB_OBJECT_INSTANCE && type <= BVB_OBJECT_FENCE;
}

uint64_t bvb_handle_id(enum bvb_object_type type, uint64_t serial) {
    if (!object_type_is_valid(type) || serial == 0U ||
        serial > BVB_HANDLE_SERIAL_MASK) {
        return 0U;
    }
    return ((uint64_t)type << BVB_HANDLE_TYPE_SHIFT) | serial;
}

enum bvb_object_type bvb_handle_type(uint64_t wire_id) {
    enum bvb_object_type type =
        (enum bvb_object_type)(wire_id >> BVB_HANDLE_TYPE_SHIFT);
    return object_type_is_valid(type) ? type : BVB_OBJECT_INVALID;
}

uint64_t bvb_handle_serial(uint64_t wire_id) {
    return wire_id & BVB_HANDLE_SERIAL_MASK;
}

int bvb_handle_expect(uint64_t wire_id, enum bvb_object_type expected_type) {
    if (!object_type_is_valid(expected_type) ||
        bvb_handle_type(wire_id) != expected_type ||
        bvb_handle_serial(wire_id) == 0U) {
        return -EINVAL;
    }
    return 0;
}

static size_t hash_wire_id(uint64_t value, size_t capacity) {
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return (size_t)value & (capacity - 1U);
}

static int table_is_valid(const struct bvb_handle_table *table) {
    return table != NULL && table->entries != NULL && table->capacity >= 8U &&
           (table->capacity & (table->capacity - 1U)) == 0U &&
           table->count <= table->capacity;
}

int bvb_handle_table_init(struct bvb_handle_table *table,
                          struct bvb_handle_entry *entries, size_t capacity) {
    if (table == NULL || entries == NULL || capacity < 8U ||
        (capacity & (capacity - 1U)) != 0U) {
        return -EINVAL;
    }
    memset(entries, 0, capacity * sizeof(*entries));
    *table = (struct bvb_handle_table){
        .entries = entries,
        .capacity = capacity,
        .count = 0U,
    };
    return 0;
}

int bvb_handle_table_insert(struct bvb_handle_table *table, uint64_t wire_id,
                            uint64_t parent_id, uint64_t native_bits) {
    if (!table_is_valid(table) || bvb_handle_type(wire_id) == BVB_OBJECT_INVALID ||
        bvb_handle_serial(wire_id) == 0U || native_bits == 0U) {
        return -EINVAL;
    }
    if (parent_id != 0U &&
        (bvb_handle_type(parent_id) == BVB_OBJECT_INVALID ||
         bvb_handle_serial(parent_id) == 0U)) {
        return -EINVAL;
    }
    if (table->count == table->capacity) {
        return -ENOSPC;
    }

    size_t index = hash_wire_id(wire_id, table->capacity);
    size_t tombstone = table->capacity;
    for (size_t probe = 0; probe < table->capacity; ++probe) {
        struct bvb_handle_entry *entry = &table->entries[index];
        if (entry->state == BVB_HANDLE_OCCUPIED && entry->wire_id == wire_id) {
            return -EEXIST;
        }
        if (entry->state == BVB_HANDLE_TOMBSTONE &&
            tombstone == table->capacity) {
            tombstone = index;
        }
        if (entry->state == BVB_HANDLE_EMPTY) {
            if (tombstone != table->capacity) {
                entry = &table->entries[tombstone];
            }
            *entry = (struct bvb_handle_entry){
                .wire_id = wire_id,
                .parent_id = parent_id,
                .native_bits = native_bits,
                .state = BVB_HANDLE_OCCUPIED,
            };
            ++table->count;
            return 0;
        }
        index = (index + 1U) & (table->capacity - 1U);
    }

    if (tombstone != table->capacity) {
        table->entries[tombstone] = (struct bvb_handle_entry){
            .wire_id = wire_id,
            .parent_id = parent_id,
            .native_bits = native_bits,
            .state = BVB_HANDLE_OCCUPIED,
        };
        ++table->count;
        return 0;
    }
    return -ENOSPC;
}

static const struct bvb_handle_entry *find_entry(
    const struct bvb_handle_table *table, uint64_t wire_id) {
    size_t index = hash_wire_id(wire_id, table->capacity);
    for (size_t probe = 0; probe < table->capacity; ++probe) {
        const struct bvb_handle_entry *entry = &table->entries[index];
        if (entry->state == BVB_HANDLE_EMPTY) {
            return NULL;
        }
        if (entry->state == BVB_HANDLE_OCCUPIED && entry->wire_id == wire_id) {
            return entry;
        }
        index = (index + 1U) & (table->capacity - 1U);
    }
    return NULL;
}

int bvb_handle_table_lookup(const struct bvb_handle_table *table,
                            uint64_t wire_id,
                            enum bvb_object_type expected_type,
                            uint64_t *parent_id, uint64_t *native_bits) {
    if (!table_is_valid(table) || native_bits == NULL ||
        bvb_handle_expect(wire_id, expected_type) != 0) {
        return -EINVAL;
    }
    const struct bvb_handle_entry *entry = find_entry(table, wire_id);
    if (entry == NULL) {
        return -ENOENT;
    }
    if (parent_id != NULL) {
        *parent_id = entry->parent_id;
    }
    *native_bits = entry->native_bits;
    return 0;
}

int bvb_handle_table_remove(struct bvb_handle_table *table, uint64_t wire_id,
                            enum bvb_object_type expected_type,
                            uint64_t *native_bits) {
    if (!table_is_valid(table) ||
        bvb_handle_expect(wire_id, expected_type) != 0) {
        return -EINVAL;
    }
    size_t index = hash_wire_id(wire_id, table->capacity);
    for (size_t probe = 0; probe < table->capacity; ++probe) {
        struct bvb_handle_entry *entry = &table->entries[index];
        if (entry->state == BVB_HANDLE_EMPTY) {
            return -ENOENT;
        }
        if (entry->state == BVB_HANDLE_OCCUPIED && entry->wire_id == wire_id) {
            if (native_bits != NULL) {
                *native_bits = entry->native_bits;
            }
            entry->wire_id = 0U;
            entry->parent_id = 0U;
            entry->native_bits = 0U;
            entry->state = BVB_HANDLE_TOMBSTONE;
            --table->count;
            return 0;
        }
        index = (index + 1U) & (table->capacity - 1U);
    }
    return -ENOENT;
}
