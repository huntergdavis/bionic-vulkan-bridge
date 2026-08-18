#ifndef BVB_VULKAN_SURFACE_PROBE_H
#define BVB_VULKAN_SURFACE_PROBE_H

#include <stddef.h>
#include <stdint.h>

enum {
    BVB_SURFACE_MAX_FORMATS = 64,
    BVB_SURFACE_MAX_PRESENT_MODES = 16,
};

struct bvb_surface_format {
    uint32_t format;
    uint32_t color_space;
};

struct bvb_vulkan_surface_result {
    uint32_t width;
    uint32_t height;
    uint64_t image_reader_usage;
    uint32_t physical_device_count;
    uint32_t queue_family_count;
    uint32_t present_queue_family_index;
    uint32_t present_queue_count;
    uint32_t min_image_count;
    uint32_t max_image_count;
    uint32_t current_width;
    uint32_t current_height;
    uint32_t min_width;
    uint32_t min_height;
    uint32_t max_width;
    uint32_t max_height;
    uint32_t supported_transforms;
    uint32_t current_transform;
    uint32_t supported_composite_alpha;
    uint32_t supported_usage_flags;
    uint32_t format_count;
    struct bvb_surface_format formats[BVB_SURFACE_MAX_FORMATS];
    uint32_t present_mode_count;
    uint32_t present_modes[BVB_SURFACE_MAX_PRESENT_MODES];
    uint64_t elapsed_ns;
};

int bvb_vulkan_probe_surface(const char *loader_path,
                             const char *media_loader_path,
                             struct bvb_vulkan_surface_result *result,
                             char *error, size_t error_size);

#endif
