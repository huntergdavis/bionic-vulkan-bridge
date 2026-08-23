#include <bvb/vulkan_selftest.h>

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define BVB_DEFAULT_LOADER "/system/lib64/libvulkan.so"

int main(int argc, char **argv) {
    const char *loader_path = BVB_DEFAULT_LOADER;
    int raw_fd_mmap = 0;
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--raw-fd-mmap") == 0 && !raw_fd_mmap) {
            raw_fd_mmap = 1;
        } else if (strcmp(argv[index], "--loader") == 0 &&
                   index + 1 < argc && argv[index + 1][0] == '/' &&
                   strcmp(loader_path, BVB_DEFAULT_LOADER) == 0) {
            loader_path = argv[++index];
        } else {
            fprintf(stderr,
                    "usage: %s [--loader ABSOLUTE_PATH] [--raw-fd-mmap]\n",
                    argv[0]);
            return 2;
        }
    }

    char error[512] = {0};
    struct bvb_vulkan_batch_context *context = NULL;
    int status = bvb_vulkan_batch_context_create(
        loader_path, &context, error, sizeof(error));
    struct bvb_vulkan_external_memory_result result = {0};
    if (status == 0) {
        status = raw_fd_mmap
            ? bvb_vulkan_batch_context_external_memory_mmap_test(
                  context, &result, error, sizeof(error))
            : bvb_vulkan_batch_context_external_memory_test(
                  context, &result, error, sizeof(error));
    }
    bvb_vulkan_batch_context_destroy(context);
    if (status != 0) {
        fprintf(stderr, "bvb: %s\n", error);
        if (status == -ENOENT || status == -ENOSYS) return 3;
        if (status == -ENOMEM) return 5;
        return 4;
    }

    printf("{\"schema_version\":1,\"gate\":\"%s\","
           "\"loader_path\":\"%s\",\"handle_type\":\"opaque_fd\","
           "\"logical_device_count\":2,"
           "\"external_memory_features\":%" PRIu32 ","
           "\"compatible_handle_types\":%" PRIu32 ","
           "\"export_from_imported_handle_types\":%" PRIu32 ","
           "\"memory_type_index\":%" PRIu32 ","
           "\"memory_property_flags\":%" PRIu32 ","
           "\"buffer_bytes\":%" PRIu32 ","
           "\"mismatched_bytes\":%" PRIu32 ","
           "\"raw_fd_mmap_bytes\":%" PRIu32 ","
           "\"raw_fd_source_mismatched_bytes\":%" PRIu32 ","
           "\"raw_fd_destination_mismatched_bytes\":%" PRIu32 "}\n",
           raw_fd_mmap ? "E138" : "E035", loader_path,
           result.external_memory_features,
           result.compatible_handle_types,
           result.export_from_imported_handle_types,
           result.memory_type_index, result.memory_property_flags,
           result.buffer_bytes, result.mismatched_bytes,
           result.raw_fd_mmap_bytes,
           result.raw_fd_source_mismatched_bytes,
           result.raw_fd_destination_mismatched_bytes);
    return 0;
}
