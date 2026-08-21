#define VK_USE_PLATFORM_ANDROID_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <android/hardware_buffer.h>

#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define BVB_AHB_PROBE_MAGIC UINT32_C(0x42414842)
#define BVB_AHB_PROBE_WIDTH UINT32_C(64)
#define BVB_AHB_PROBE_HEIGHT UINT32_C(64)

struct bvb_ahb_probe_result {
    uint32_t magic;
    int32_t status;
    uint32_t stage;
    uint32_t api_version;
    uint32_t extension_seen;
    uint32_t importable;
    uint32_t dedicated_only;
    uint32_t memory_type_bits;
    uint64_t allocation_size;
};

struct bvb_vulkan_import {
    void *loader;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr gdpa;
    VkInstance instance;
    VkDevice device;
    VkImage image;
    VkDeviceMemory memory;
};

typedef int (*bvb_ahb_is_supported_fn)(const AHardwareBuffer_Desc *);
typedef int (*bvb_ahb_allocate_fn)(const AHardwareBuffer_Desc *,
                                   AHardwareBuffer **);
typedef void (*bvb_ahb_release_fn)(AHardwareBuffer *);
typedef int (*bvb_ahb_send_fn)(const AHardwareBuffer *, int);
typedef int (*bvb_ahb_receive_fn)(int, AHardwareBuffer **);

struct bvb_android_api {
    void *library;
    bvb_ahb_is_supported_fn is_supported;
    bvb_ahb_allocate_fn allocate;
    bvb_ahb_release_fn release;
    bvb_ahb_send_fn send;
    bvb_ahb_receive_fn receive;
};

enum bvb_ahb_probe_stage {
    BVB_AHB_STAGE_COMPLETE = 0,
    BVB_AHB_STAGE_OPEN_LOADER = 1,
    BVB_AHB_STAGE_GLOBAL_ENTRY = 2,
    BVB_AHB_STAGE_CREATE_INSTANCE = 3,
    BVB_AHB_STAGE_PHYSICAL_DEVICE = 4,
    BVB_AHB_STAGE_DEVICE_EXTENSION = 5,
    BVB_AHB_STAGE_IMAGE_FORMAT = 6,
    BVB_AHB_STAGE_CREATE_DEVICE = 7,
    BVB_AHB_STAGE_CREATE_IMAGE = 8,
    BVB_AHB_STAGE_BUFFER_PROPERTIES = 9,
    BVB_AHB_STAGE_ALLOCATE_MEMORY = 10,
    BVB_AHB_STAGE_BIND_IMAGE = 11,
    BVB_AHB_STAGE_CHANNEL = 12,
};

static void *function_object(PFN_vkVoidFunction function) {
    union {
        PFN_vkVoidFunction function;
        void *object;
    } conversion = {.function = function};
    return conversion.object;
}

static PFN_vkGetInstanceProcAddr object_gipa(void *object) {
    union {
        void *object;
        PFN_vkGetInstanceProcAddr function;
    } conversion = {.object = object};
    return conversion.function;
}

static PFN_vkGetDeviceProcAddr object_gdpa(void *object) {
    union {
        void *object;
        PFN_vkGetDeviceProcAddr function;
    } conversion = {.object = object};
    return conversion.function;
}

static int load_android_api(struct bvb_android_api *api) {
    memset(api, 0, sizeof(*api));
    api->library = dlopen("/system/lib64/libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (api->library == NULL) return -ENOENT;
#define BVB_ANDROID_SYMBOL(member, type, name)                                 \
    do {                                                                       \
        union {                                                                \
            void *object;                                                      \
            type function;                                                     \
        } conversion = {.object = dlsym(api->library, (name))};                \
        api->member = conversion.function;                                     \
    } while (0)
    BVB_ANDROID_SYMBOL(is_supported, bvb_ahb_is_supported_fn,
                       "AHardwareBuffer_isSupported");
    BVB_ANDROID_SYMBOL(allocate, bvb_ahb_allocate_fn,
                       "AHardwareBuffer_allocate");
    BVB_ANDROID_SYMBOL(release, bvb_ahb_release_fn,
                       "AHardwareBuffer_release");
    BVB_ANDROID_SYMBOL(send, bvb_ahb_send_fn,
                       "AHardwareBuffer_sendHandleToUnixSocket");
    BVB_ANDROID_SYMBOL(receive, bvb_ahb_receive_fn,
                       "AHardwareBuffer_recvHandleFromUnixSocket");
#undef BVB_ANDROID_SYMBOL
    if (api->is_supported == NULL || api->allocate == NULL ||
        api->release == NULL || api->send == NULL || api->receive == NULL) {
        dlclose(api->library);
        memset(api, 0, sizeof(*api));
        return -ENOSYS;
    }
    return 0;
}

static int write_exact(int descriptor, const void *data, size_t size) {
    const uint8_t *bytes = data;
    size_t offset = 0U;
    while (offset < size) {
        ssize_t count = write(descriptor, bytes + offset, size - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return count == 0 ? -EPIPE : -errno;
        offset += (size_t)count;
    }
    return 0;
}

static int read_exact(int descriptor, void *data, size_t size) {
    uint8_t *bytes = data;
    size_t offset = 0U;
    while (offset < size) {
        ssize_t count = read(descriptor, bytes + offset, size - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return count == 0 ? -EPIPE : -errno;
        offset += (size_t)count;
    }
    return 0;
}

static uint32_t first_set_bit(uint32_t bits) {
    for (uint32_t index = 0U; index < 32U; ++index) {
        if ((bits & (UINT32_C(1) << index)) != 0U) return index;
    }
    return UINT32_MAX;
}

static bool has_device_extension(PFN_vkEnumerateDeviceExtensionProperties enumerate,
                                 VkPhysicalDevice physical_device,
                                 const char *required) {
    uint32_t count = 0U;
    if (enumerate(physical_device, NULL, &count, NULL) != VK_SUCCESS ||
        count == 0U || count > 4096U) {
        return false;
    }
    VkExtensionProperties *properties = calloc(count, sizeof(*properties));
    if (properties == NULL) return false;
    VkResult result = enumerate(physical_device, NULL, &count, properties);
    bool found = false;
    if (result == VK_SUCCESS || result == VK_INCOMPLETE) {
        for (uint32_t index = 0U; index < count; ++index) {
            if (strcmp(properties[index].extensionName, required) == 0) {
                found = true;
                break;
            }
        }
    }
    free(properties);
    return found;
}

static void destroy_import(struct bvb_vulkan_import *objects) {
    if (objects == NULL) return;
    if (objects->device != VK_NULL_HANDLE && objects->gdpa != NULL) {
        PFN_vkFreeMemory free_memory = (PFN_vkFreeMemory)function_object(
            objects->gdpa(objects->device, "vkFreeMemory"));
        PFN_vkDestroyImage destroy_image = (PFN_vkDestroyImage)function_object(
            objects->gdpa(objects->device, "vkDestroyImage"));
        PFN_vkDestroyDevice destroy_device =
            (PFN_vkDestroyDevice)function_object(
                objects->gdpa(objects->device, "vkDestroyDevice"));
        if (objects->memory != VK_NULL_HANDLE && free_memory != NULL)
            free_memory(objects->device, objects->memory, NULL);
        if (objects->image != VK_NULL_HANDLE && destroy_image != NULL)
            destroy_image(objects->device, objects->image, NULL);
        if (destroy_device != NULL) destroy_device(objects->device, NULL);
    }
    if (objects->instance != VK_NULL_HANDLE && objects->gipa != NULL) {
        PFN_vkDestroyInstance destroy_instance =
            (PFN_vkDestroyInstance)function_object(
                objects->gipa(objects->instance, "vkDestroyInstance"));
        if (destroy_instance != NULL) destroy_instance(objects->instance, NULL);
    }
    if (objects->loader != NULL) dlclose(objects->loader);
    memset(objects, 0, sizeof(*objects));
}

static int import_buffer(const char *loader_path, AHardwareBuffer *buffer,
                         uint32_t width, uint32_t height,
                         struct bvb_ahb_probe_result *probe,
                         struct bvb_vulkan_import *objects) {
    memset(probe, 0, sizeof(*probe));
    probe->magic = BVB_AHB_PROBE_MAGIC;
    probe->status = -EIO;
    probe->stage = BVB_AHB_STAGE_OPEN_LOADER;

    objects->loader = dlopen(loader_path, RTLD_NOW | RTLD_LOCAL);
    if (objects->loader == NULL) return -ENOENT;
    objects->gipa = object_gipa(dlsym(objects->loader, "vkGetInstanceProcAddr"));
    if (objects->gipa == NULL) {
        objects->gipa = object_gipa(
            dlsym(objects->loader, "vk_icdGetInstanceProcAddr"));
    }
    if (objects->gipa == NULL) {
        probe->stage = BVB_AHB_STAGE_GLOBAL_ENTRY;
        return -ENOSYS;
    }

    PFN_vkEnumerateInstanceVersion enumerate_instance_version =
        (PFN_vkEnumerateInstanceVersion)function_object(objects->gipa(
            VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    probe->api_version = VK_API_VERSION_1_0;
    if (enumerate_instance_version != NULL &&
        enumerate_instance_version(&probe->api_version) != VK_SUCCESS) {
        return -EIO;
    }
    const VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "bvb-ahardwarebuffer-probe",
        .applicationVersion = 1U,
        .pEngineName = "bvb",
        .engineVersion = 1U,
        .apiVersion = probe->api_version < VK_API_VERSION_1_1
                          ? probe->api_version
                          : VK_API_VERSION_1_1,
    };
    const VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
    };
    PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance)function_object(
        objects->gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    probe->stage = BVB_AHB_STAGE_CREATE_INSTANCE;
    if (create_instance == NULL ||
        create_instance(&instance_info, NULL, &objects->instance) != VK_SUCCESS)
        return -EIO;

    objects->gdpa = object_gdpa(function_object(objects->gipa(
        objects->instance, "vkGetDeviceProcAddr")));
    PFN_vkEnumeratePhysicalDevices enumerate_physical_devices =
        (PFN_vkEnumeratePhysicalDevices)function_object(objects->gipa(
            objects->instance, "vkEnumeratePhysicalDevices"));
    PFN_vkEnumerateDeviceExtensionProperties enumerate_device_extensions =
        (PFN_vkEnumerateDeviceExtensionProperties)function_object(objects->gipa(
            objects->instance, "vkEnumerateDeviceExtensionProperties"));
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_properties =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)function_object(
            objects->gipa(objects->instance,
                          "vkGetPhysicalDeviceQueueFamilyProperties"));
    PFN_vkGetPhysicalDeviceImageFormatProperties2 get_image_format =
        (PFN_vkGetPhysicalDeviceImageFormatProperties2)function_object(
            objects->gipa(objects->instance,
                          "vkGetPhysicalDeviceImageFormatProperties2"));
    PFN_vkCreateDevice create_device = (PFN_vkCreateDevice)function_object(
        objects->gipa(objects->instance, "vkCreateDevice"));
    probe->stage = BVB_AHB_STAGE_PHYSICAL_DEVICE;
    if (objects->gdpa == NULL || enumerate_physical_devices == NULL ||
        enumerate_device_extensions == NULL || get_queue_properties == NULL ||
        get_image_format == NULL || create_device == NULL)
        return -ENOSYS;
    uint32_t physical_count = 1U;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    if (enumerate_physical_devices(objects->instance, &physical_count,
                                   &physical_device) != VK_SUCCESS ||
        physical_count == 0U || physical_device == VK_NULL_HANDLE)
        return -ENODEV;

    probe->stage = BVB_AHB_STAGE_DEVICE_EXTENSION;
    probe->extension_seen = has_device_extension(
        enumerate_device_extensions, physical_device,
        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
    if (probe->extension_seen == 0U) return -ENOTSUP;

    VkPhysicalDeviceExternalImageFormatInfo external_query = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .handleType =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
    };
    VkPhysicalDeviceImageFormatInfo2 format_query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &external_query,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .flags = 0U,
    };
    VkExternalImageFormatProperties external_properties = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
    };
    VkImageFormatProperties2 format_properties = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &external_properties,
    };
    probe->stage = BVB_AHB_STAGE_IMAGE_FORMAT;
    VkResult result =
        get_image_format(physical_device, &format_query, &format_properties);
    if (result != VK_SUCCESS) return -ENOTSUP;
    const VkExternalMemoryFeatureFlags external_features =
        external_properties.externalMemoryProperties.externalMemoryFeatures;
    probe->importable =
        (external_features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0U;
    probe->dedicated_only =
        (external_features & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0U;
    if (probe->importable == 0U) return -ENOTSUP;

    uint32_t queue_count = 0U;
    get_queue_properties(physical_device, &queue_count, NULL);
    if (queue_count == 0U || queue_count > 256U) return -ENODEV;
    VkQueueFamilyProperties *queue_properties =
        calloc(queue_count, sizeof(*queue_properties));
    if (queue_properties == NULL) return -ENOMEM;
    get_queue_properties(physical_device, &queue_count, queue_properties);
    uint32_t queue_family = UINT32_MAX;
    for (uint32_t index = 0U; index < queue_count; ++index) {
        if (queue_properties[index].queueCount != 0U) {
            queue_family = index;
            break;
        }
    }
    free(queue_properties);
    if (queue_family == UINT32_MAX) return -ENODEV;
    const float queue_priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queue_family,
        .queueCount = 1U,
        .pQueuePriorities = &queue_priority,
    };
    const char *device_extensions[] = {
        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
    };
    const VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1U,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 1U,
        .ppEnabledExtensionNames = device_extensions,
    };
    probe->stage = BVB_AHB_STAGE_CREATE_DEVICE;
    result = create_device(physical_device, &device_info, NULL, &objects->device);
    if (result != VK_SUCCESS) return -EIO;

    PFN_vkCreateImage create_image = (PFN_vkCreateImage)function_object(
        objects->gdpa(objects->device, "vkCreateImage"));
    PFN_vkGetImageMemoryRequirements get_memory_requirements =
        (PFN_vkGetImageMemoryRequirements)function_object(
            objects->gdpa(objects->device, "vkGetImageMemoryRequirements"));
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID get_buffer_properties =
        (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)function_object(
            objects->gdpa(objects->device,
                          "vkGetAndroidHardwareBufferPropertiesANDROID"));
    PFN_vkAllocateMemory allocate_memory = (PFN_vkAllocateMemory)function_object(
        objects->gdpa(objects->device, "vkAllocateMemory"));
    PFN_vkBindImageMemory bind_image_memory =
        (PFN_vkBindImageMemory)function_object(
            objects->gdpa(objects->device, "vkBindImageMemory"));
    if (create_image == NULL || get_memory_requirements == NULL ||
        get_buffer_properties == NULL || allocate_memory == NULL ||
        bind_image_memory == NULL)
        return -ENOSYS;

    VkExternalMemoryImageCreateInfo external_image = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
    };
    const VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_image,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {width, height, 1U},
        .mipLevels = 1U,
        .arrayLayers = 1U,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    probe->stage = BVB_AHB_STAGE_CREATE_IMAGE;
    result = create_image(objects->device, &image_info, NULL, &objects->image);
    if (result != VK_SUCCESS) return -EIO;

    VkAndroidHardwareBufferPropertiesANDROID buffer_properties = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
    };
    probe->stage = BVB_AHB_STAGE_BUFFER_PROPERTIES;
    result = get_buffer_properties(objects->device, buffer, &buffer_properties);
    if (result != VK_SUCCESS || buffer_properties.allocationSize == 0U)
        return -EIO;
    VkMemoryRequirements image_requirements;
    memset(&image_requirements, 0, sizeof(image_requirements));
    get_memory_requirements(objects->device, objects->image,
                            &image_requirements);
    const uint32_t memory_bits =
        buffer_properties.memoryTypeBits & image_requirements.memoryTypeBits;
    probe->memory_type_bits = memory_bits;
    probe->allocation_size = buffer_properties.allocationSize;
    const uint32_t memory_type = first_set_bit(memory_bits);
    if (memory_type == UINT32_MAX) return -ENOTSUP;

    VkImportAndroidHardwareBufferInfoANDROID import_info = {
        .sType =
            VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
        .buffer = buffer,
    };
    VkMemoryDedicatedAllocateInfo dedicated_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &import_info,
        .image = objects->image,
    };
    const VkMemoryAllocateInfo memory_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated_info,
        .allocationSize = buffer_properties.allocationSize,
        .memoryTypeIndex = memory_type,
    };
    probe->stage = BVB_AHB_STAGE_ALLOCATE_MEMORY;
    result = allocate_memory(objects->device, &memory_info, NULL,
                             &objects->memory);
    if (result != VK_SUCCESS) return -EIO;
    probe->stage = BVB_AHB_STAGE_BIND_IMAGE;
    result = bind_image_memory(objects->device, objects->image, objects->memory,
                               0U);
    if (result != VK_SUCCESS) return -EIO;
    probe->stage = BVB_AHB_STAGE_COMPLETE;
    probe->status = 0;
    return 0;
}

static const char *stage_name(uint32_t stage) {
    static const char *const names[] = {
        "complete",          "open_loader",       "global_entry",
        "create_instance",   "physical_device",   "device_extension",
        "image_format",      "create_device",     "create_image",
        "buffer_properties", "allocate_memory",   "bind_image",
        "channel",
    };
    return stage < sizeof(names) / sizeof(names[0]) ? names[stage] : "unknown";
}

static void print_side(const char *name, const char *loader,
                       const struct bvb_ahb_probe_result *result) {
    printf("\"%s\":{\"loader\":\"%s\",\"status\":%" PRId32
           ",\"stage\":\"%s\",\"api_version\":%" PRIu32
           ",\"extension_seen\":%" PRIu32 ",\"importable\":%" PRIu32
           ",\"dedicated_only\":%" PRIu32
           ",\"memory_type_bits\":%" PRIu32
           ",\"allocation_size\":%" PRIu64 "}",
           name, loader, result->status, stage_name(result->stage),
           result->api_version, result->extension_seen, result->importable,
           result->dedicated_only, result->memory_type_bits,
           result->allocation_size);
}

int main(int argc, char **argv) {
    const char *system_loader = "/system/lib64/libvulkan.so";
    const char *private_loader = NULL;
    uint32_t width = BVB_AHB_PROBE_WIDTH;
    uint32_t height = BVB_AHB_PROBE_HEIGHT;
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--system-loader") == 0 && index + 1 < argc) {
            system_loader = argv[++index];
        } else if (strcmp(argv[index], "--private-loader") == 0 &&
                   index + 1 < argc) {
            private_loader = argv[++index];
        } else if ((strcmp(argv[index], "--width") == 0 ||
                    strcmp(argv[index], "--height") == 0) &&
                   index + 1 < argc) {
            char *end = NULL;
            unsigned long value = strtoul(argv[index + 1], &end, 10);
            if (end == argv[index + 1] || *end != '\0' || value == 0UL ||
                value > UINT32_MAX) {
                fprintf(stderr, "invalid image extent\n");
                return 2;
            }
            if (strcmp(argv[index], "--width") == 0)
                width = (uint32_t)value;
            else
                height = (uint32_t)value;
            ++index;
        } else {
            fprintf(stderr,
                    "usage: %s --private-loader ABSOLUTE_PATH "
                    "[--system-loader ABSOLUTE_PATH] "
                    "[--width PIXELS --height PIXELS]\n",
                    argv[0]);
            return 2;
        }
    }
    if (private_loader == NULL || private_loader[0] != '/' ||
        system_loader[0] != '/') {
        fprintf(stderr, "both Vulkan loader paths must be absolute\n");
        return 2;
    }

    struct bvb_android_api android = {0};
    if (load_android_api(&android) != 0) {
        fprintf(stderr, "could not load the system AHardwareBuffer API\n");
        return 3;
    }

    AHardwareBuffer_Desc description = {
        .width = width,
        .height = height,
        .layers = 1U,
        .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT,
    };
    if (android.is_supported(&description) == 0) {
        fprintf(stderr, "requested AHardwareBuffer shape is unsupported\n");
        dlclose(android.library);
        return 3;
    }
    AHardwareBuffer *buffer = NULL;
    if (android.allocate(&description, &buffer) != 0 || buffer == NULL) {
        fprintf(stderr, "AHardwareBuffer allocation failed\n");
        dlclose(android.library);
        return 3;
    }
    int channel[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, channel) != 0) {
        android.release(buffer);
        dlclose(android.library);
        return 3;
    }
    pid_t child = fork();
    if (child < 0) {
        close(channel[0]);
        close(channel[1]);
        android.release(buffer);
        dlclose(android.library);
        return 3;
    }
    if (child == 0) {
        close(channel[0]);
        android.release(buffer);
        AHardwareBuffer *received = NULL;
        struct bvb_ahb_probe_result result = {
            .magic = BVB_AHB_PROBE_MAGIC,
            .status = -EIO,
            .stage = BVB_AHB_STAGE_CHANNEL,
        };
        struct bvb_vulkan_import objects = {0};
        if (android.receive(channel[1], &received) != 0 || received == NULL) {
            (void)write_exact(channel[1], &result, sizeof(result));
            close(channel[1]);
            _exit(4);
        }
        result.status = import_buffer(private_loader, received, width, height,
                                      &result,
                                      &objects);
        const int sent = write_exact(channel[1], &result, sizeof(result));
        uint8_t release = 0U;
        (void)read_exact(channel[1], &release, sizeof(release));
        destroy_import(&objects);
        android.release(received);
        close(channel[1]);
        _exit(sent == 0 && result.status == 0 ? 0 : 4);
    }

    close(channel[1]);
    int send_status = android.send(buffer, channel[0]);
    struct bvb_ahb_probe_result system_result = {0};
    struct bvb_ahb_probe_result private_result = {0};
    struct bvb_vulkan_import system_objects = {0};
    int system_status = -EPIPE;
    if (send_status == 0) {
        system_status = import_buffer(system_loader, buffer, width, height,
                                      &system_result, &system_objects);
    }
    int receive_status =
        read_exact(channel[0], &private_result, sizeof(private_result));
    uint8_t release = 1U;
    (void)write_exact(channel[0], &release, sizeof(release));
    close(channel[0]);
    int child_status = 0;
    if (waitpid(child, &child_status, 0) != child) child_status = -1;
    destroy_import(&system_objects);
    android.release(buffer);
    dlclose(android.library);

    const bool private_valid =
        receive_status == 0 && private_result.magic == BVB_AHB_PROBE_MAGIC &&
        private_result.status == 0 && WIFEXITED(child_status) &&
        WEXITSTATUS(child_status) == 0;
    const bool passed = send_status == 0 && system_status == 0 && private_valid;
    printf("{\"schema_version\":1,\"gate\":\"E091\","
           "\"result\":\"%s\",\"transport\":\"AHardwareBuffer\","
           "\"width\":%" PRIu32 ",\"height\":%" PRIu32 ",",
           passed ? "pass" : "fail", width, height);
    print_side("system_vulkan", system_loader, &system_result);
    putchar(',');
    print_side("private_turnip", private_loader, &private_result);
    printf(",\"simultaneous_import\":%s}\n", passed ? "true" : "false");
    return passed ? 0 : 5;
}
