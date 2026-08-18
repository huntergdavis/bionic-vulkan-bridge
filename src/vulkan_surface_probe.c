#define _POSIX_C_SOURCE 200809L
#define VK_NO_PROTOTYPES

#include <bvb/vulkan_surface_probe.h>

#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct ANativeWindow;
typedef struct AImageReader AImageReader;
typedef int32_t media_status_t;

enum {
    BVB_AMEDIA_OK = 0,
    BVB_AIMAGE_FORMAT_RGBA_8888 = 1,
    BVB_SURFACE_WIDTH = 64,
    BVB_SURFACE_HEIGHT = 64,
    BVB_SURFACE_MAX_IMAGES = 3,
};

static const uint64_t BVB_AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE =
    UINT64_C(1) << 8;

typedef media_status_t (*bvb_image_reader_new_with_usage_fn)(
    int32_t width, int32_t height, int32_t format, uint64_t usage,
    int32_t max_images, AImageReader **reader);
typedef media_status_t (*bvb_image_reader_get_window_fn)(
    AImageReader *reader, struct ANativeWindow **window);
typedef void (*bvb_image_reader_delete_fn)(AImageReader *reader);

typedef VkFlags VkAndroidSurfaceCreateFlagsKHR;
typedef struct VkAndroidSurfaceCreateInfoKHR {
    VkStructureType sType;
    const void *pNext;
    VkAndroidSurfaceCreateFlagsKHR flags;
    struct ANativeWindow *window;
} VkAndroidSurfaceCreateInfoKHR;
typedef VkResult(VKAPI_PTR *bvb_create_android_surface_fn)(
    VkInstance instance, const VkAndroidSurfaceCreateInfoKHR *create_info,
    const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface);

struct bvb_surface_objects {
    void *loader;
    void *media_loader;
    AImageReader *reader;
    VkInstance instance;
    VkSurfaceKHR surface;
    bvb_image_reader_delete_fn delete_reader;
    PFN_vkDestroySurfaceKHR destroy_surface;
    PFN_vkDestroyInstance destroy_instance;
};

static void set_error(char *output, size_t output_size, const char *format, ...) {
    if (output == NULL || output_size == 0U) {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(output, output_size, format, arguments);
    va_end(arguments);
}

static void *raw_symbol(void *loader, const char *name) {
    return dlsym(loader, name);
}

static PFN_vkVoidFunction vulkan_symbol(void *loader, const char *name) {
    void *raw = raw_symbol(loader, name);
    PFN_vkVoidFunction function = NULL;
    if (raw != NULL) {
        memcpy(&function, &raw, sizeof(function));
    }
    return function;
}

#define BVB_MEDIA_SYMBOL(destination, loader, name)                             \
    do {                                                                        \
        void *raw = raw_symbol((loader), #name);                                \
        if (raw != NULL) {                                                      \
            memcpy(&(destination), &raw, sizeof(destination));                  \
        }                                                                       \
    } while (0)

static int monotonic_ns(uint64_t *output) {
    struct timespec timestamp;
    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
        return -errno;
    }
    *output = (uint64_t)timestamp.tv_sec * UINT64_C(1000000000) +
              (uint64_t)timestamp.tv_nsec;
    return 0;
}

static void cleanup(struct bvb_surface_objects *objects) {
    if (objects->surface != VK_NULL_HANDLE &&
        objects->destroy_surface != NULL &&
        objects->instance != VK_NULL_HANDLE) {
        objects->destroy_surface(objects->instance, objects->surface, NULL);
    }
    if (objects->instance != VK_NULL_HANDLE &&
        objects->destroy_instance != NULL) {
        objects->destroy_instance(objects->instance, NULL);
    }
    if (objects->reader != NULL && objects->delete_reader != NULL) {
        objects->delete_reader(objects->reader);
    }
    if (objects->media_loader != NULL) {
        (void)dlclose(objects->media_loader);
    }
    if (objects->loader != NULL) {
        (void)dlclose(objects->loader);
    }
}

static bool has_required_instance_extensions(
    PFN_vkEnumerateInstanceExtensionProperties enumerate_extensions) {
    uint32_t count = 0;
    if (enumerate_extensions(NULL, &count, NULL) != VK_SUCCESS ||
        count == 0U || count > 256U) {
        return false;
    }
    VkExtensionProperties *properties = calloc(count, sizeof(*properties));
    if (properties == NULL) {
        return false;
    }
    VkResult result = enumerate_extensions(NULL, &count, properties);
    bool has_surface = false;
    bool has_android_surface = false;
    if (result == VK_SUCCESS || result == VK_INCOMPLETE) {
        for (uint32_t index = 0; index < count; ++index) {
            has_surface |= strcmp(properties[index].extensionName,
                                  "VK_KHR_surface") == 0;
            has_android_surface |= strcmp(properties[index].extensionName,
                                          "VK_KHR_android_surface") == 0;
        }
    }
    free(properties);
    return has_surface && has_android_surface;
}

int bvb_vulkan_probe_surface(const char *loader_path,
                             const char *media_loader_path,
                             struct bvb_vulkan_surface_result *output,
                             char *error, size_t error_size) {
    struct bvb_surface_objects objects;
    memset(&objects, 0, sizeof(objects));
    if (error != NULL && error_size > 0U) {
        error[0] = '\0';
    }
    if (loader_path == NULL || loader_path[0] != '/' ||
        media_loader_path == NULL || media_loader_path[0] != '/' ||
        output == NULL) {
        set_error(error, error_size, "loader paths must be absolute");
        return -EINVAL;
    }
    memset(output, 0, sizeof(*output));
    output->width = BVB_SURFACE_WIDTH;
    output->height = BVB_SURFACE_HEIGHT;
    output->image_reader_usage =
        BVB_AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;

    uint64_t start_ns = 0;
    if (monotonic_ns(&start_ns) != 0) {
        set_error(error, error_size, "could not read monotonic clock");
        return -EIO;
    }

    int status = 0;
    objects.media_loader = dlopen(media_loader_path, RTLD_NOW | RTLD_LOCAL);
    if (objects.media_loader == NULL) {
        set_error(error, error_size, "could not load %s: %s",
                  media_loader_path, dlerror());
        return -ENOENT;
    }
    bvb_image_reader_new_with_usage_fn new_reader = NULL;
    bvb_image_reader_get_window_fn get_window = NULL;
    BVB_MEDIA_SYMBOL(new_reader, objects.media_loader,
                     AImageReader_newWithUsage);
    BVB_MEDIA_SYMBOL(get_window, objects.media_loader, AImageReader_getWindow);
    BVB_MEDIA_SYMBOL(objects.delete_reader, objects.media_loader,
                     AImageReader_delete);
    if (new_reader == NULL || get_window == NULL ||
        objects.delete_reader == NULL) {
        set_error(error, error_size, "Media NDK is missing AImageReader APIs");
        status = -ENOSYS;
        goto done;
    }
    media_status_t media_result = new_reader(
        BVB_SURFACE_WIDTH, BVB_SURFACE_HEIGHT,
        BVB_AIMAGE_FORMAT_RGBA_8888,
        BVB_AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
        BVB_SURFACE_MAX_IMAGES, &objects.reader);
    if (media_result != BVB_AMEDIA_OK || objects.reader == NULL) {
        set_error(error, error_size, "AImageReader_newWithUsage failed: %d",
                  (int)media_result);
        status = -EIO;
        goto done;
    }
    struct ANativeWindow *window = NULL;
    media_result = get_window(objects.reader, &window);
    if (media_result != BVB_AMEDIA_OK || window == NULL) {
        set_error(error, error_size, "AImageReader_getWindow failed: %d",
                  (int)media_result);
        status = -EIO;
        goto done;
    }

    objects.loader = dlopen(loader_path, RTLD_NOW | RTLD_LOCAL);
    if (objects.loader == NULL) {
        set_error(error, error_size, "could not load %s: %s", loader_path,
                  dlerror());
        status = -ENOENT;
        goto done;
    }
    PFN_vkGetInstanceProcAddr gipa =
        (PFN_vkGetInstanceProcAddr)vulkan_symbol(objects.loader,
                                                  "vkGetInstanceProcAddr");
    if (gipa == NULL) {
        set_error(error, error_size, "loader has no vkGetInstanceProcAddr");
        status = -ENOSYS;
        goto done;
    }
    PFN_vkEnumerateInstanceExtensionProperties enumerate_extensions =
        (PFN_vkEnumerateInstanceExtensionProperties)gipa(
            VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    PFN_vkCreateInstance create_instance =
        (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (enumerate_extensions == NULL || create_instance == NULL) {
        set_error(error, error_size, "loader is missing global entry points");
        status = -ENOSYS;
        goto done;
    }
    if (!has_required_instance_extensions(enumerate_extensions)) {
        set_error(error, error_size,
                  "loader lacks required Android surface extensions");
        status = -ENOTSUP;
        goto done;
    }

    static const char *const extensions[] = {
        "VK_KHR_surface",
        "VK_KHR_android_surface",
    };
    const VkApplicationInfo application_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "bvb-vulkan-surface-probe",
        .applicationVersion = VK_MAKE_API_VERSION(0, 0, 6, 0),
        .pEngineName = "none",
        .apiVersion = VK_API_VERSION_1_0,
    };
    const VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application_info,
        .enabledExtensionCount =
            (uint32_t)(sizeof(extensions) / sizeof(extensions[0])),
        .ppEnabledExtensionNames = extensions,
    };
    VkResult vk_result = create_instance(&instance_info, NULL,
                                         &objects.instance);
    if (vk_result != VK_SUCCESS) {
        set_error(error, error_size, "vkCreateInstance failed: %d",
                  (int)vk_result);
        status = -EIO;
        goto done;
    }

#define LOAD_INSTANCE(type, name)                                               \
    type name = (type)gipa(objects.instance, #name)
    LOAD_INSTANCE(PFN_vkDestroyInstance, vkDestroyInstance);
    LOAD_INSTANCE(PFN_vkEnumeratePhysicalDevices, vkEnumeratePhysicalDevices);
    LOAD_INSTANCE(PFN_vkGetPhysicalDeviceQueueFamilyProperties,
                  vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD_INSTANCE(bvb_create_android_surface_fn, vkCreateAndroidSurfaceKHR);
    LOAD_INSTANCE(PFN_vkDestroySurfaceKHR, vkDestroySurfaceKHR);
    LOAD_INSTANCE(PFN_vkGetPhysicalDeviceSurfaceSupportKHR,
                  vkGetPhysicalDeviceSurfaceSupportKHR);
    LOAD_INSTANCE(PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR,
                  vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    LOAD_INSTANCE(PFN_vkGetPhysicalDeviceSurfaceFormatsKHR,
                  vkGetPhysicalDeviceSurfaceFormatsKHR);
    LOAD_INSTANCE(PFN_vkGetPhysicalDeviceSurfacePresentModesKHR,
                  vkGetPhysicalDeviceSurfacePresentModesKHR);
#undef LOAD_INSTANCE
    objects.destroy_instance = vkDestroyInstance;
    objects.destroy_surface = vkDestroySurfaceKHR;
    if (vkDestroyInstance == NULL || vkEnumeratePhysicalDevices == NULL ||
        vkGetPhysicalDeviceQueueFamilyProperties == NULL ||
        vkCreateAndroidSurfaceKHR == NULL || vkDestroySurfaceKHR == NULL ||
        vkGetPhysicalDeviceSurfaceSupportKHR == NULL ||
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR == NULL ||
        vkGetPhysicalDeviceSurfaceFormatsKHR == NULL ||
        vkGetPhysicalDeviceSurfacePresentModesKHR == NULL) {
        set_error(error, error_size, "loader is missing surface entry points");
        status = -ENOSYS;
        goto done;
    }

    const VkAndroidSurfaceCreateInfoKHR surface_info = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        .window = window,
    };
    vk_result = vkCreateAndroidSurfaceKHR(objects.instance, &surface_info,
                                          NULL, &objects.surface);
    if (vk_result != VK_SUCCESS) {
        set_error(error, error_size, "vkCreateAndroidSurfaceKHR failed: %d",
                  (int)vk_result);
        status = -EIO;
        goto done;
    }

    vk_result = vkEnumeratePhysicalDevices(objects.instance,
                                           &output->physical_device_count,
                                           NULL);
    if (vk_result != VK_SUCCESS || output->physical_device_count == 0U ||
        output->physical_device_count > 16U) {
        set_error(error, error_size, "physical-device query failed: %d",
                  (int)vk_result);
        status = -ENODEV;
        goto done;
    }
    VkPhysicalDevice devices[16];
    uint32_t device_count = output->physical_device_count;
    vk_result = vkEnumeratePhysicalDevices(objects.instance, &device_count,
                                           devices);
    if (vk_result != VK_SUCCESS && vk_result != VK_INCOMPLETE) {
        set_error(error, error_size, "physical-device list failed: %d",
                  (int)vk_result);
        status = -EIO;
        goto done;
    }
    VkPhysicalDevice physical_device = devices[0];

    vkGetPhysicalDeviceQueueFamilyProperties(
        physical_device, &output->queue_family_count, NULL);
    if (output->queue_family_count == 0U ||
        output->queue_family_count > 256U) {
        set_error(error, error_size, "no bounded queue-family list");
        status = -ENODEV;
        goto done;
    }
    VkQueueFamilyProperties *queue_properties =
        calloc(output->queue_family_count, sizeof(*queue_properties));
    if (queue_properties == NULL) {
        set_error(error, error_size, "could not allocate queue-family list");
        status = -ENOMEM;
        goto done;
    }
    uint32_t queue_count = output->queue_family_count;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_count,
                                             queue_properties);
    bool present_queue_found = false;
    for (uint32_t index = 0; index < queue_count; ++index) {
        VkBool32 supported = VK_FALSE;
        vk_result = vkGetPhysicalDeviceSurfaceSupportKHR(
            physical_device, index, objects.surface, &supported);
        if (vk_result == VK_SUCCESS && supported == VK_TRUE &&
            queue_properties[index].queueCount > 0U) {
            output->present_queue_family_index = index;
            output->present_queue_count = queue_properties[index].queueCount;
            present_queue_found = true;
            break;
        }
    }
    free(queue_properties);
    if (!present_queue_found) {
        set_error(error, error_size, "no queue supports the Android surface");
        status = -ENOTSUP;
        goto done;
    }

    VkSurfaceCapabilitiesKHR capabilities;
    vk_result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        physical_device, objects.surface, &capabilities);
    if (vk_result != VK_SUCCESS) {
        set_error(error, error_size, "surface-capability query failed: %d",
                  (int)vk_result);
        status = -EIO;
        goto done;
    }
    output->min_image_count = capabilities.minImageCount;
    output->max_image_count = capabilities.maxImageCount;
    output->current_width = capabilities.currentExtent.width;
    output->current_height = capabilities.currentExtent.height;
    output->min_width = capabilities.minImageExtent.width;
    output->min_height = capabilities.minImageExtent.height;
    output->max_width = capabilities.maxImageExtent.width;
    output->max_height = capabilities.maxImageExtent.height;
    output->supported_transforms = capabilities.supportedTransforms;
    output->current_transform = capabilities.currentTransform;
    output->supported_composite_alpha = capabilities.supportedCompositeAlpha;
    output->supported_usage_flags = capabilities.supportedUsageFlags;

    uint32_t format_count = 0;
    vk_result = vkGetPhysicalDeviceSurfaceFormatsKHR(
        physical_device, objects.surface, &format_count, NULL);
    if (vk_result != VK_SUCCESS || format_count == 0U ||
        format_count > BVB_SURFACE_MAX_FORMATS) {
        set_error(error, error_size, "surface-format count invalid: %u",
                  format_count);
        status = -EOVERFLOW;
        goto done;
    }
    VkSurfaceFormatKHR formats[BVB_SURFACE_MAX_FORMATS];
    vk_result = vkGetPhysicalDeviceSurfaceFormatsKHR(
        physical_device, objects.surface, &format_count, formats);
    if (vk_result != VK_SUCCESS) {
        set_error(error, error_size, "surface-format query failed: %d",
                  (int)vk_result);
        status = -EIO;
        goto done;
    }
    output->format_count = format_count;
    for (uint32_t index = 0; index < format_count; ++index) {
        output->formats[index].format = formats[index].format;
        output->formats[index].color_space = formats[index].colorSpace;
    }

    uint32_t mode_count = 0;
    vk_result = vkGetPhysicalDeviceSurfacePresentModesKHR(
        physical_device, objects.surface, &mode_count, NULL);
    if (vk_result != VK_SUCCESS || mode_count == 0U ||
        mode_count > BVB_SURFACE_MAX_PRESENT_MODES) {
        set_error(error, error_size, "present-mode count invalid: %u",
                  mode_count);
        status = -EOVERFLOW;
        goto done;
    }
    VkPresentModeKHR modes[BVB_SURFACE_MAX_PRESENT_MODES];
    vk_result = vkGetPhysicalDeviceSurfacePresentModesKHR(
        physical_device, objects.surface, &mode_count, modes);
    if (vk_result != VK_SUCCESS) {
        set_error(error, error_size, "present-mode query failed: %d",
                  (int)vk_result);
        status = -EIO;
        goto done;
    }
    output->present_mode_count = mode_count;
    for (uint32_t index = 0; index < mode_count; ++index) {
        output->present_modes[index] = modes[index];
    }

    uint64_t end_ns = 0;
    if (monotonic_ns(&end_ns) != 0 || end_ns < start_ns) {
        set_error(error, error_size, "could not measure surface probe");
        status = -EIO;
        goto done;
    }
    output->elapsed_ns = end_ns - start_ns;

done:
    cleanup(&objects);
    return status;
}
