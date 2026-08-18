#include <bvb/vulkan_caps.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BVB_DEFAULT_LOADER "/system/lib64/libvulkan.so"

enum bvb_exit_code {
    BVB_OK = 0,
    BVB_USAGE = 2,
    BVB_LOADER_ERROR = 3,
    BVB_VULKAN_ERROR = 4,
    BVB_ALLOCATION_ERROR = 5,
};

static void print_usage(const char *program) {
    fprintf(stderr, "usage: %s [--loader ABSOLUTE_PATH]\n", program);
}

static int parse_arguments(int argc, char **argv, const char **loader_path) {
    *loader_path = BVB_DEFAULT_LOADER;
    if (argc == 1) {
        return BVB_OK;
    }
    if (argc == 3 && strcmp(argv[1], "--loader") == 0 && argv[2][0] == '/') {
        *loader_path = argv[2];
        return BVB_OK;
    }
    print_usage(argv[0]);
    return BVB_USAGE;
}

static void print_json_string(const char *value) {
    const unsigned char *cursor = (const unsigned char *)value;
    putchar('"');
    while (*cursor != '\0') {
        switch (*cursor) {
        case '"':
            fputs("\\\"", stdout);
            break;
        case '\\':
            fputs("\\\\", stdout);
            break;
        case '\b':
            fputs("\\b", stdout);
            break;
        case '\f':
            fputs("\\f", stdout);
            break;
        case '\n':
            fputs("\\n", stdout);
            break;
        case '\r':
            fputs("\\r", stdout);
            break;
        case '\t':
            fputs("\\t", stdout);
            break;
        default:
            if (*cursor < 0x20U) {
                printf("\\u%04x", (unsigned int)*cursor);
            } else {
                putchar((int)*cursor);
            }
            break;
        }
        ++cursor;
    }
    putchar('"');
}

static void print_version(uint32_t version) {
    printf(
        "{\"raw\":%" PRIu32 ",\"major\":%" PRIu32
        ",\"minor\":%" PRIu32 ",\"patch\":%" PRIu32 "}",
        version,
        version >> 22,
        (version >> 12) & 0x3ffU,
        version & 0xfffU);
}

int main(int argc, char **argv) {
    const char *loader_path = NULL;
    int exit_code = parse_arguments(argc, argv, &loader_path);
    if (exit_code != BVB_OK) {
        return exit_code;
    }

    struct bvb_vulkan_caps caps;
    char error[512];
    int result = bvb_vulkan_collect(loader_path, &caps, error, sizeof(error));
    if (result != 0) {
        fprintf(stderr, "bvb: %s\n", error);
        if (result == -ENOMEM) {
            return BVB_ALLOCATION_ERROR;
        }
        if (result == -ENOENT || result == -ENOSYS) {
            return BVB_LOADER_ERROR;
        }
        return BVB_VULKAN_ERROR;
    }

    fputs("{\"schema_version\":1,\"loader_path\":", stdout);
    print_json_string(loader_path);
    fputs(",\"loader_api_version\":", stdout);
    print_version(caps.loader_api_version);
    printf(",\"instance_extension_count\":%" PRIu32
           ",\"physical_device_count\":%" PRIu32
           ",\"physical_devices\":[",
           caps.instance_extension_count,
           caps.physical_device_count);

    for (uint32_t index = 0; index < caps.included_device_count; ++index) {
        const struct bvb_vulkan_device_caps *device = &caps.devices[index];
        if (index != 0U) {
            putchar(',');
        }
        printf("{\"index\":%" PRIu32 ",\"name\":", index);
        print_json_string(device->name);
        fputs(",\"api_version\":", stdout);
        print_version(device->api_version);
        printf(",\"driver_version\":%" PRIu32
               ",\"vendor_id\":%" PRIu32
               ",\"device_id\":%" PRIu32
               ",\"device_type\":%" PRIu32
               ",\"queue_family_count\":%" PRIu32
               ",\"memory_heap_count\":%" PRIu32
               ",\"device_local_bytes\":%" PRIu64 "}",
               device->driver_version,
               device->vendor_id,
               device->device_id,
               device->device_type,
               device->queue_family_count,
               device->memory_heap_count,
               device->device_local_bytes);
    }
    fputs("]}\n", stdout);
    return BVB_OK;
}

