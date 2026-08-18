#include <bvb/vulkan_selftest.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BVB_DEFAULT_LOADER "/system/lib64/libvulkan.so"

static void print_extension_array(uint64_t flags, int device) {
    struct extension_name {
        uint64_t flag;
        const char *name;
    };
    static const struct extension_name instance_extensions[] = {
        {BVB_INSTANCE_KHR_SURFACE, "VK_KHR_surface"},
        {BVB_INSTANCE_KHR_ANDROID_SURFACE, "VK_KHR_android_surface"},
        {BVB_INSTANCE_EXT_HEADLESS_SURFACE, "VK_EXT_headless_surface"},
        {BVB_INSTANCE_KHR_GET_PROPERTIES_2,
         "VK_KHR_get_physical_device_properties2"},
        {BVB_INSTANCE_KHR_EXTERNAL_MEMORY_CAPS,
         "VK_KHR_external_memory_capabilities"},
        {BVB_INSTANCE_KHR_EXTERNAL_SEMAPHORE_CAPS,
         "VK_KHR_external_semaphore_capabilities"},
    };
    static const struct extension_name device_extensions[] = {
        {BVB_DEVICE_KHR_SWAPCHAIN, "VK_KHR_swapchain"},
        {BVB_DEVICE_KHR_EXTERNAL_MEMORY, "VK_KHR_external_memory"},
        {BVB_DEVICE_KHR_EXTERNAL_MEMORY_FD, "VK_KHR_external_memory_fd"},
        {BVB_DEVICE_ANDROID_HARDWARE_BUFFER,
         "VK_ANDROID_external_memory_android_hardware_buffer"},
        {BVB_DEVICE_KHR_EXTERNAL_SEMAPHORE, "VK_KHR_external_semaphore"},
        {BVB_DEVICE_KHR_EXTERNAL_SEMAPHORE_FD,
         "VK_KHR_external_semaphore_fd"},
        {BVB_DEVICE_KHR_TIMELINE_SEMAPHORE, "VK_KHR_timeline_semaphore"},
        {BVB_DEVICE_KHR_EXTERNAL_FENCE_FD, "VK_KHR_external_fence_fd"},
    };
    const struct extension_name *extensions =
        device != 0 ? device_extensions : instance_extensions;
    size_t count = device != 0
                       ? sizeof(device_extensions) / sizeof(device_extensions[0])
                       : sizeof(instance_extensions) /
                             sizeof(instance_extensions[0]);
    int first = 1;
    putchar('[');
    for (size_t index = 0; index < count; ++index) {
        if ((flags & extensions[index].flag) == 0U) {
            continue;
        }
        if (!first) {
            putchar(',');
        }
        printf("\"%s\"", extensions[index].name);
        first = 0;
    }
    putchar(']');
}

int main(int argc, char **argv) {
    const char *loader_path = BVB_DEFAULT_LOADER;
    if (argc == 3 && strcmp(argv[1], "--loader") == 0 && argv[2][0] == '/') {
        loader_path = argv[2];
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [--loader ABSOLUTE_PATH]\n", argv[0]);
        return 2;
    }

    struct bvb_vulkan_selftest_result result;
    char error[512];
    int status = bvb_vulkan_run_selftest(loader_path, &result, error,
                                         sizeof(error));
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
           "\"instance_extension_count\":%" PRIu32
           ",\"known_instance_extensions\":",
           loader_path, result.instance_extension_count);
    print_extension_array(result.instance_extension_flags, 0);
    printf(",\"device_extension_count\":%" PRIu32
           ",\"known_device_extensions\":",
           result.device_extension_count);
    print_extension_array(result.device_extension_flags, 1);
    printf(",\"queue_family_index\":%" PRIu32
           ",\"queue_flags\":%" PRIu32
           ",\"memory_type_index\":%" PRIu32
           ",\"memory_property_flags\":%" PRIu32
           ",\"buffer_bytes\":%" PRIu32
           ",\"fill_word\":%" PRIu32
           ",\"mismatched_words\":%" PRIu32
           ",\"submit_wait_elapsed_ns\":%" PRIu64 "}\n",
           result.queue_family_index, result.queue_flags,
           result.memory_type_index, result.memory_property_flags,
           result.buffer_bytes, result.fill_word, result.mismatched_words,
           result.submit_wait_elapsed_ns);
    return 0;
}

