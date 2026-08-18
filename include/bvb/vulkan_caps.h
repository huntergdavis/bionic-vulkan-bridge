#ifndef BVB_VULKAN_CAPS_H
#define BVB_VULKAN_CAPS_H

#include <stddef.h>
#include <stdint.h>

enum {
    BVB_VULKAN_MAX_DEVICES = 8,
    BVB_VULKAN_DEVICE_NAME_SIZE = 256,
};

struct bvb_vulkan_device_caps {
    uint32_t api_version;
    uint32_t driver_version;
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t device_type;
    uint32_t queue_family_count;
    uint32_t memory_heap_count;
    uint64_t device_local_bytes;
    char name[BVB_VULKAN_DEVICE_NAME_SIZE];
};

struct bvb_vulkan_caps {
    uint32_t loader_api_version;
    uint32_t instance_extension_count;
    uint32_t physical_device_count;
    uint32_t included_device_count;
    struct bvb_vulkan_device_caps devices[BVB_VULKAN_MAX_DEVICES];
};

/*
 * Query an absolute Vulkan loader path without linking to a Vulkan runtime.
 * Returns zero on success or a negative errno-style value on failure. The
 * optional error buffer receives a human-readable diagnostic.
 */
int bvb_vulkan_collect(const char *loader_path, struct bvb_vulkan_caps *caps,
                       char *error, size_t error_size);

#endif

