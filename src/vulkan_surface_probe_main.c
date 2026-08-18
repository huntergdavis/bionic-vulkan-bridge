#include <bvb/vulkan_surface_probe.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BVB_DEFAULT_VULKAN_LOADER "/system/lib64/libvulkan.so"
#define BVB_DEFAULT_MEDIA_LOADER "/system/lib64/libmediandk.so"

static void print_formats(const struct bvb_vulkan_surface_result *result) {
    putchar('[');
    for (uint32_t index = 0; index < result->format_count; ++index) {
        if (index != 0U) {
            putchar(',');
        }
        printf("{\"format\":%" PRIu32 ",\"color_space\":%" PRIu32 "}",
               result->formats[index].format,
               result->formats[index].color_space);
    }
    putchar(']');
}

static void print_present_modes(
    const struct bvb_vulkan_surface_result *result) {
    putchar('[');
    for (uint32_t index = 0; index < result->present_mode_count; ++index) {
        if (index != 0U) {
            putchar(',');
        }
        printf("%" PRIu32, result->present_modes[index]);
    }
    putchar(']');
}

int main(int argc, char **argv) {
    const char *loader_path = BVB_DEFAULT_VULKAN_LOADER;
    const char *media_loader_path = BVB_DEFAULT_MEDIA_LOADER;
    if (argc == 5 && strcmp(argv[1], "--loader") == 0 &&
        strcmp(argv[3], "--media-loader") == 0 && argv[2][0] == '/' &&
        argv[4][0] == '/') {
        loader_path = argv[2];
        media_loader_path = argv[4];
    } else if (argc != 1) {
        fprintf(stderr,
                "usage: %s [--loader ABSOLUTE_PATH --media-loader "
                "ABSOLUTE_PATH]\n",
                argv[0]);
        return 2;
    }

    struct bvb_vulkan_surface_result result;
    char error[512];
    int status = bvb_vulkan_probe_surface(loader_path, media_loader_path,
                                          &result, error, sizeof(error));
    if (status != 0) {
        fprintf(stderr, "bvb: %s\n", error);
        if (status == -ENOENT || status == -ENOSYS) {
            return 3;
        }
        if (status == -ENOMEM) {
            return 5;
        }
        return 4;
    }

    printf("{\"schema_version\":1,\"loader_path\":\"%s\","
           "\"media_loader_path\":\"%s\",\"width\":%" PRIu32
           ",\"height\":%" PRIu32 ",\"image_reader_usage\":%" PRIu64
           ",\"physical_device_count\":%" PRIu32
           ",\"queue_family_count\":%" PRIu32
           ",\"present_queue_family_index\":%" PRIu32
           ",\"present_queue_count\":%" PRIu32
           ",\"min_image_count\":%" PRIu32
           ",\"max_image_count\":%" PRIu32
           ",\"current_extent\":{\"width\":%" PRIu32
           ",\"height\":%" PRIu32 "}"
           ",\"min_extent\":{\"width\":%" PRIu32
           ",\"height\":%" PRIu32 "}"
           ",\"max_extent\":{\"width\":%" PRIu32
           ",\"height\":%" PRIu32 "}"
           ",\"supported_transforms\":%" PRIu32
           ",\"current_transform\":%" PRIu32
           ",\"supported_composite_alpha\":%" PRIu32
           ",\"supported_usage_flags\":%" PRIu32
           ",\"formats\":",
           loader_path, media_loader_path, result.width, result.height,
           result.image_reader_usage, result.physical_device_count,
           result.queue_family_count, result.present_queue_family_index,
           result.present_queue_count, result.min_image_count,
           result.max_image_count, result.current_width,
           result.current_height, result.min_width, result.min_height,
           result.max_width, result.max_height, result.supported_transforms,
           result.current_transform, result.supported_composite_alpha,
           result.supported_usage_flags);
    print_formats(&result);
    printf(",\"present_modes\":");
    print_present_modes(&result);
    printf(",\"elapsed_ns\":%" PRIu64 "}\n", result.elapsed_ns);
    return 0;
}
