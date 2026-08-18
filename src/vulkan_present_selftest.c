#define _POSIX_C_SOURCE 200809L
#define VK_NO_PROTOTYPES

#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BVB_DEFAULT_VULKAN_LOADER "/system/lib64/libvulkan.so"
#define BVB_DEFAULT_MEDIA_LOADER "/system/lib64/libmediandk.so"

struct ANativeWindow;
typedef struct AImageReader AImageReader;
typedef struct AImage AImage;
typedef int32_t media_status_t;

enum {
    BVB_AMEDIA_OK = 0,
    BVB_NO_BUFFER_AVAILABLE = -30001,
    BVB_AIMAGE_FORMAT_RGBA_8888 = 1,
    BVB_WIDTH = 64,
    BVB_HEIGHT = 64,
    BVB_MAX_IMAGES = 3,
    BVB_MAX_SWAPCHAIN_IMAGES = 64,
    BVB_ACQUIRE_POLL_LIMIT = 200,
    BVB_ACQUIRE_POLL_NS = 10000000,
};

static const uint64_t BVB_CPU_READ_OFTEN = UINT64_C(3);

typedef media_status_t (*bvb_new_reader_fn)(
    int32_t, int32_t, int32_t, uint64_t, int32_t, AImageReader **);
typedef media_status_t (*bvb_get_window_fn)(AImageReader *,
                                             struct ANativeWindow **);
typedef void (*bvb_delete_reader_fn)(AImageReader *);
typedef media_status_t (*bvb_acquire_image_fn)(AImageReader *, AImage **);
typedef void (*bvb_delete_image_fn)(AImage *);
typedef media_status_t (*bvb_get_planes_fn)(const AImage *, int32_t *);
typedef media_status_t (*bvb_get_stride_fn)(const AImage *, int, int32_t *);
typedef media_status_t (*bvb_get_data_fn)(const AImage *, int, uint8_t **,
                                          int *);

typedef VkFlags VkAndroidSurfaceCreateFlagsKHR;
typedef struct VkAndroidSurfaceCreateInfoKHR {
    VkStructureType sType;
    const void *pNext;
    VkAndroidSurfaceCreateFlagsKHR flags;
    struct ANativeWindow *window;
} VkAndroidSurfaceCreateInfoKHR;
typedef VkResult(VKAPI_PTR *bvb_create_android_surface_fn)(
    VkInstance, const VkAndroidSurfaceCreateInfoKHR *,
    const VkAllocationCallbacks *, VkSurfaceKHR *);

struct bvb_media_api {
    bvb_new_reader_fn new_reader;
    bvb_get_window_fn get_window;
    bvb_delete_reader_fn delete_reader;
    bvb_acquire_image_fn acquire_image;
    bvb_delete_image_fn delete_image;
    bvb_get_planes_fn get_planes;
    bvb_get_stride_fn get_pixel_stride;
    bvb_get_stride_fn get_row_stride;
    bvb_get_data_fn get_data;
};

struct bvb_objects {
    void *vulkan_loader;
    void *media_loader;
    struct bvb_media_api media;
    AImageReader *reader;
    AImage *image;
    VkInstance instance;
    VkSurfaceKHR surface;
    VkDevice device;
    VkQueue queue;
    VkSwapchainKHR swapchain;
    VkCommandPool command_pool;
    VkSemaphore acquire_semaphore;
    VkSemaphore render_semaphore;
    PFN_vkDeviceWaitIdle device_wait_idle;
    PFN_vkDestroySemaphore destroy_semaphore;
    PFN_vkDestroyCommandPool destroy_command_pool;
    PFN_vkDestroySwapchainKHR destroy_swapchain;
    PFN_vkDestroyDevice destroy_device;
    PFN_vkDestroySurfaceKHR destroy_surface;
    PFN_vkDestroyInstance destroy_instance;
};

struct bvb_result {
    uint32_t queue_family_index;
    uint32_t surface_format;
    uint32_t color_space;
    uint32_t present_mode;
    uint32_t requested_image_count;
    uint32_t actual_image_count;
    uint32_t acquired_image_index;
    uint32_t width;
    uint32_t height;
    uint32_t plane_count;
    uint32_t pixel_stride;
    uint32_t row_stride;
    uint32_t data_length;
    uint32_t expected_rgba_word;
    uint32_t mismatched_pixels;
    uint32_t acquire_poll_count;
    uint64_t submit_present_ns;
    uint64_t total_elapsed_ns;
};

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

static uint64_t monotonic_ns(void) {
    struct timespec timestamp;
    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
        return 0;
    }
    return (uint64_t)timestamp.tv_sec * UINT64_C(1000000000) +
           (uint64_t)timestamp.tv_nsec;
}

static void cleanup(struct bvb_objects *objects) {
    if (objects->image != NULL && objects->media.delete_image != NULL) {
        objects->media.delete_image(objects->image);
    }
    if (objects->device != VK_NULL_HANDLE) {
        if (objects->device_wait_idle != NULL) {
            (void)objects->device_wait_idle(objects->device);
        }
        if (objects->render_semaphore != VK_NULL_HANDLE &&
            objects->destroy_semaphore != NULL) {
            objects->destroy_semaphore(objects->device,
                                       objects->render_semaphore, NULL);
        }
        if (objects->acquire_semaphore != VK_NULL_HANDLE &&
            objects->destroy_semaphore != NULL) {
            objects->destroy_semaphore(objects->device,
                                       objects->acquire_semaphore, NULL);
        }
        if (objects->command_pool != VK_NULL_HANDLE &&
            objects->destroy_command_pool != NULL) {
            objects->destroy_command_pool(objects->device,
                                          objects->command_pool, NULL);
        }
        if (objects->swapchain != VK_NULL_HANDLE &&
            objects->destroy_swapchain != NULL) {
            objects->destroy_swapchain(objects->device, objects->swapchain,
                                       NULL);
        }
        if (objects->destroy_device != NULL) {
            objects->destroy_device(objects->device, NULL);
        }
    }
    if (objects->surface != VK_NULL_HANDLE &&
        objects->destroy_surface != NULL &&
        objects->instance != VK_NULL_HANDLE) {
        objects->destroy_surface(objects->instance, objects->surface, NULL);
    }
    if (objects->instance != VK_NULL_HANDLE &&
        objects->destroy_instance != NULL) {
        objects->destroy_instance(objects->instance, NULL);
    }
    if (objects->reader != NULL && objects->media.delete_reader != NULL) {
        objects->media.delete_reader(objects->reader);
    }
    if (objects->media_loader != NULL) {
        (void)dlclose(objects->media_loader);
    }
    if (objects->vulkan_loader != NULL) {
        (void)dlclose(objects->vulkan_loader);
    }
}

static bool has_extension(PFN_vkEnumerateInstanceExtensionProperties enumerate,
                          const char *wanted) {
    uint32_t count = 0;
    if (enumerate(NULL, &count, NULL) != VK_SUCCESS || count > 256U) {
        return false;
    }
    VkExtensionProperties *properties = calloc(count, sizeof(*properties));
    if (properties == NULL && count > 0U) {
        return false;
    }
    VkResult status = enumerate(NULL, &count, properties);
    bool found = false;
    if (status == VK_SUCCESS || status == VK_INCOMPLETE) {
        for (uint32_t index = 0; index < count; ++index) {
            found |= strcmp(properties[index].extensionName, wanted) == 0;
        }
    }
    free(properties);
    return found;
}

static bool has_device_extension(
    PFN_vkEnumerateDeviceExtensionProperties enumerate,
    VkPhysicalDevice device, const char *wanted) {
    uint32_t count = 0;
    if (enumerate(device, NULL, &count, NULL) != VK_SUCCESS || count > 1024U) {
        return false;
    }
    VkExtensionProperties *properties = calloc(count, sizeof(*properties));
    if (properties == NULL && count > 0U) {
        return false;
    }
    VkResult status = enumerate(device, NULL, &count, properties);
    bool found = false;
    if (status == VK_SUCCESS || status == VK_INCOMPLETE) {
        for (uint32_t index = 0; index < count; ++index) {
            found |= strcmp(properties[index].extensionName, wanted) == 0;
        }
    }
    free(properties);
    return found;
}

static VkCompositeAlphaFlagBitsKHR choose_composite_alpha(VkFlags supported) {
    static const VkCompositeAlphaFlagBitsKHR choices[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };
    for (size_t index = 0; index < sizeof(choices) / sizeof(choices[0]);
         ++index) {
        if ((supported & choices[index]) != 0U) {
            return choices[index];
        }
    }
    return (VkCompositeAlphaFlagBitsKHR)0;
}

static int run_selftest(const char *vulkan_path, const char *media_path,
                        struct bvb_result *output, char *error,
                        size_t error_size) {
    struct bvb_objects objects;
    memset(&objects, 0, sizeof(objects));
    memset(output, 0, sizeof(*output));
    uint64_t start_ns = monotonic_ns();
    int status = 4;

#define BVB_FAIL(...)                                                           \
    do {                                                                        \
        (void)snprintf(error, error_size, __VA_ARGS__);                          \
        goto done;                                                              \
    } while (0)

    objects.media_loader = dlopen(media_path, RTLD_NOW | RTLD_LOCAL);
    if (objects.media_loader == NULL) {
        BVB_FAIL("could not load %s: %s", media_path, dlerror());
    }
    BVB_MEDIA_SYMBOL(objects.media.new_reader, objects.media_loader,
                     AImageReader_newWithUsage);
    BVB_MEDIA_SYMBOL(objects.media.get_window, objects.media_loader,
                     AImageReader_getWindow);
    BVB_MEDIA_SYMBOL(objects.media.delete_reader, objects.media_loader,
                     AImageReader_delete);
    BVB_MEDIA_SYMBOL(objects.media.acquire_image, objects.media_loader,
                     AImageReader_acquireNextImage);
    BVB_MEDIA_SYMBOL(objects.media.delete_image, objects.media_loader,
                     AImage_delete);
    BVB_MEDIA_SYMBOL(objects.media.get_planes, objects.media_loader,
                     AImage_getNumberOfPlanes);
    BVB_MEDIA_SYMBOL(objects.media.get_pixel_stride, objects.media_loader,
                     AImage_getPlanePixelStride);
    BVB_MEDIA_SYMBOL(objects.media.get_row_stride, objects.media_loader,
                     AImage_getPlaneRowStride);
    BVB_MEDIA_SYMBOL(objects.media.get_data, objects.media_loader,
                     AImage_getPlaneData);
    if (objects.media.new_reader == NULL || objects.media.get_window == NULL ||
        objects.media.delete_reader == NULL ||
        objects.media.acquire_image == NULL ||
        objects.media.delete_image == NULL || objects.media.get_planes == NULL ||
        objects.media.get_pixel_stride == NULL ||
        objects.media.get_row_stride == NULL || objects.media.get_data == NULL) {
        BVB_FAIL("Media NDK is missing image reader/readback APIs");
    }
    media_status_t media_status = objects.media.new_reader(
        BVB_WIDTH, BVB_HEIGHT, BVB_AIMAGE_FORMAT_RGBA_8888,
        BVB_CPU_READ_OFTEN, BVB_MAX_IMAGES, &objects.reader);
    if (media_status != BVB_AMEDIA_OK || objects.reader == NULL) {
        BVB_FAIL("AImageReader_newWithUsage failed: %d", (int)media_status);
    }
    struct ANativeWindow *window = NULL;
    media_status = objects.media.get_window(objects.reader, &window);
    if (media_status != BVB_AMEDIA_OK || window == NULL) {
        BVB_FAIL("AImageReader_getWindow failed: %d", (int)media_status);
    }

    objects.vulkan_loader = dlopen(vulkan_path, RTLD_NOW | RTLD_LOCAL);
    if (objects.vulkan_loader == NULL) {
        BVB_FAIL("could not load %s: %s", vulkan_path, dlerror());
    }
    PFN_vkGetInstanceProcAddr gipa =
        (PFN_vkGetInstanceProcAddr)vulkan_symbol(objects.vulkan_loader,
                                                  "vkGetInstanceProcAddr");
    if (gipa == NULL) {
        BVB_FAIL("Vulkan loader has no vkGetInstanceProcAddr");
    }
    PFN_vkEnumerateInstanceExtensionProperties enumerate_instance_extensions =
        (PFN_vkEnumerateInstanceExtensionProperties)gipa(
            VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    PFN_vkCreateInstance create_instance =
        (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (enumerate_instance_extensions == NULL || create_instance == NULL ||
        !has_extension(enumerate_instance_extensions, "VK_KHR_surface") ||
        !has_extension(enumerate_instance_extensions,
                       "VK_KHR_android_surface")) {
        BVB_FAIL("required Vulkan instance extensions are unavailable");
    }
    static const char *const instance_extensions[] = {
        "VK_KHR_surface", "VK_KHR_android_surface"};
    const VkApplicationInfo application_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "bvb-vulkan-present-selftest",
        .applicationVersion = VK_MAKE_API_VERSION(0, 0, 7, 0),
        .pEngineName = "none",
        .apiVersion = VK_API_VERSION_1_0,
    };
    const VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application_info,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = instance_extensions,
    };
    VkResult vk_status = create_instance(&instance_info, NULL,
                                         &objects.instance);
    if (vk_status != VK_SUCCESS) {
        BVB_FAIL("vkCreateInstance failed: %d", (int)vk_status);
    }

#define LOAD_INSTANCE(type, name) type name = (type)gipa(objects.instance, #name)
    LOAD_INSTANCE(PFN_vkDestroyInstance, vkDestroyInstance);
    LOAD_INSTANCE(PFN_vkEnumeratePhysicalDevices, vkEnumeratePhysicalDevices);
    LOAD_INSTANCE(PFN_vkGetPhysicalDeviceQueueFamilyProperties,
                  vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD_INSTANCE(PFN_vkEnumerateDeviceExtensionProperties,
                  vkEnumerateDeviceExtensionProperties);
    LOAD_INSTANCE(PFN_vkCreateDevice, vkCreateDevice);
    LOAD_INSTANCE(PFN_vkGetDeviceProcAddr, vkGetDeviceProcAddr);
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
        vkEnumerateDeviceExtensionProperties == NULL || vkCreateDevice == NULL ||
        vkGetDeviceProcAddr == NULL || vkCreateAndroidSurfaceKHR == NULL ||
        vkDestroySurfaceKHR == NULL ||
        vkGetPhysicalDeviceSurfaceSupportKHR == NULL ||
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR == NULL ||
        vkGetPhysicalDeviceSurfaceFormatsKHR == NULL ||
        vkGetPhysicalDeviceSurfacePresentModesKHR == NULL) {
        BVB_FAIL("Vulkan loader is missing WSI instance entry points");
    }

    const VkAndroidSurfaceCreateInfoKHR surface_info = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        .window = window,
    };
    vk_status = vkCreateAndroidSurfaceKHR(objects.instance, &surface_info,
                                          NULL, &objects.surface);
    if (vk_status != VK_SUCCESS) {
        BVB_FAIL("vkCreateAndroidSurfaceKHR failed: %d", (int)vk_status);
    }

    uint32_t device_count = 0;
    vk_status = vkEnumeratePhysicalDevices(objects.instance, &device_count,
                                           NULL);
    if (vk_status != VK_SUCCESS || device_count == 0U || device_count > 16U) {
        BVB_FAIL("physical-device query failed: %d", (int)vk_status);
    }
    VkPhysicalDevice devices[16];
    vk_status = vkEnumeratePhysicalDevices(objects.instance, &device_count,
                                           devices);
    if (vk_status != VK_SUCCESS && vk_status != VK_INCOMPLETE) {
        BVB_FAIL("physical-device list failed: %d", (int)vk_status);
    }
    VkPhysicalDevice physical_device = devices[0];
    if (!has_device_extension(vkEnumerateDeviceExtensionProperties,
                              physical_device, "VK_KHR_swapchain")) {
        BVB_FAIL("physical device has no VK_KHR_swapchain");
    }

    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device,
                                             &queue_family_count, NULL);
    if (queue_family_count == 0U || queue_family_count > 256U) {
        BVB_FAIL("invalid queue-family count: %u", queue_family_count);
    }
    VkQueueFamilyProperties *queue_properties =
        calloc(queue_family_count, sizeof(*queue_properties));
    if (queue_properties == NULL) {
        BVB_FAIL("could not allocate queue-family properties");
    }
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device,
                                             &queue_family_count,
                                             queue_properties);
    bool queue_found = false;
    for (uint32_t index = 0; index < queue_family_count; ++index) {
        VkBool32 present_support = VK_FALSE;
        vk_status = vkGetPhysicalDeviceSurfaceSupportKHR(
            physical_device, index, objects.surface, &present_support);
        if (vk_status == VK_SUCCESS && present_support == VK_TRUE &&
            queue_properties[index].queueCount > 0U &&
            (queue_properties[index].queueFlags &
             (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT |
              VK_QUEUE_TRANSFER_BIT)) != 0U) {
            output->queue_family_index = index;
            queue_found = true;
            break;
        }
    }
    free(queue_properties);
    if (!queue_found) {
        BVB_FAIL("no present-and-transfer capable queue family");
    }

    VkSurfaceCapabilitiesKHR capabilities;
    vk_status = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        physical_device, objects.surface, &capabilities);
    if (vk_status != VK_SUCCESS || capabilities.minImageCount == 0U ||
        (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ==
            0U) {
        BVB_FAIL("surface lacks bounded transfer-destination capability");
    }
    VkCompositeAlphaFlagBitsKHR composite_alpha =
        choose_composite_alpha(capabilities.supportedCompositeAlpha);
    if (composite_alpha == 0) {
        BVB_FAIL("surface has no supported composite-alpha mode");
    }

    uint32_t format_count = 0;
    vk_status = vkGetPhysicalDeviceSurfaceFormatsKHR(
        physical_device, objects.surface, &format_count, NULL);
    if (vk_status != VK_SUCCESS || format_count == 0U || format_count > 64U) {
        BVB_FAIL("surface-format count invalid: %u", format_count);
    }
    VkSurfaceFormatKHR formats[64];
    vk_status = vkGetPhysicalDeviceSurfaceFormatsKHR(
        physical_device, objects.surface, &format_count, formats);
    if (vk_status != VK_SUCCESS) {
        BVB_FAIL("surface-format query failed: %d", (int)vk_status);
    }
    bool format_found = false;
    VkSurfaceFormatKHR surface_format = {0};
    for (uint32_t index = 0; index < format_count; ++index) {
        if (formats[index].format == VK_FORMAT_R8G8B8A8_UNORM &&
            formats[index].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surface_format = formats[index];
            format_found = true;
            break;
        }
    }
    if (!format_found) {
        BVB_FAIL("surface has no CPU-verifiable RGBA8 format");
    }

    uint32_t mode_count = 0;
    vk_status = vkGetPhysicalDeviceSurfacePresentModesKHR(
        physical_device, objects.surface, &mode_count, NULL);
    if (vk_status != VK_SUCCESS || mode_count == 0U || mode_count > 32U) {
        BVB_FAIL("present-mode count invalid: %u", mode_count);
    }
    VkPresentModeKHR modes[32];
    vk_status = vkGetPhysicalDeviceSurfacePresentModesKHR(
        physical_device, objects.surface, &mode_count, modes);
    if (vk_status != VK_SUCCESS) {
        BVB_FAIL("present-mode query failed: %d", (int)vk_status);
    }
    bool fifo_found = false;
    for (uint32_t index = 0; index < mode_count; ++index) {
        fifo_found |= modes[index] == VK_PRESENT_MODE_FIFO_KHR;
    }
    if (!fifo_found) {
        BVB_FAIL("surface unexpectedly lacks FIFO present mode");
    }

    const float queue_priority = 1.0F;
    const VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = output->queue_family_index,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };
    static const char *const device_extensions[] = {"VK_KHR_swapchain"};
    const VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = device_extensions,
    };
    vk_status = vkCreateDevice(physical_device, &device_info, NULL,
                               &objects.device);
    if (vk_status != VK_SUCCESS) {
        BVB_FAIL("vkCreateDevice failed: %d", (int)vk_status);
    }

#define LOAD_DEVICE(name)                                                       \
    PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(objects.device, #name)
    LOAD_DEVICE(vkDestroyDevice);
    LOAD_DEVICE(vkGetDeviceQueue);
    LOAD_DEVICE(vkCreateSwapchainKHR);
    LOAD_DEVICE(vkDestroySwapchainKHR);
    LOAD_DEVICE(vkGetSwapchainImagesKHR);
    LOAD_DEVICE(vkCreateCommandPool);
    LOAD_DEVICE(vkDestroyCommandPool);
    LOAD_DEVICE(vkAllocateCommandBuffers);
    LOAD_DEVICE(vkBeginCommandBuffer);
    LOAD_DEVICE(vkEndCommandBuffer);
    LOAD_DEVICE(vkCmdPipelineBarrier);
    LOAD_DEVICE(vkCmdClearColorImage);
    LOAD_DEVICE(vkCreateSemaphore);
    LOAD_DEVICE(vkDestroySemaphore);
    LOAD_DEVICE(vkAcquireNextImageKHR);
    LOAD_DEVICE(vkQueueSubmit);
    LOAD_DEVICE(vkQueuePresentKHR);
    LOAD_DEVICE(vkQueueWaitIdle);
    LOAD_DEVICE(vkDeviceWaitIdle);
#undef LOAD_DEVICE
    objects.destroy_device = vkDestroyDevice;
    objects.destroy_swapchain = vkDestroySwapchainKHR;
    objects.destroy_command_pool = vkDestroyCommandPool;
    objects.destroy_semaphore = vkDestroySemaphore;
    objects.device_wait_idle = vkDeviceWaitIdle;
    if (vkDestroyDevice == NULL || vkGetDeviceQueue == NULL ||
        vkCreateSwapchainKHR == NULL || vkDestroySwapchainKHR == NULL ||
        vkGetSwapchainImagesKHR == NULL || vkCreateCommandPool == NULL ||
        vkDestroyCommandPool == NULL || vkAllocateCommandBuffers == NULL ||
        vkBeginCommandBuffer == NULL || vkEndCommandBuffer == NULL ||
        vkCmdPipelineBarrier == NULL || vkCmdClearColorImage == NULL ||
        vkCreateSemaphore == NULL || vkDestroySemaphore == NULL ||
        vkAcquireNextImageKHR == NULL || vkQueueSubmit == NULL ||
        vkQueuePresentKHR == NULL || vkQueueWaitIdle == NULL ||
        vkDeviceWaitIdle == NULL) {
        BVB_FAIL("Vulkan device is missing swapchain/command entry points");
    }
    vkGetDeviceQueue(objects.device, output->queue_family_index, 0,
                     &objects.queue);
    if (objects.queue == VK_NULL_HANDLE) {
        BVB_FAIL("vkGetDeviceQueue returned null");
    }

    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent = (VkExtent2D){BVB_WIDTH, BVB_HEIGHT};
    }
    output->width = extent.width;
    output->height = extent.height;
    output->surface_format = surface_format.format;
    output->color_space = surface_format.colorSpace;
    output->present_mode = VK_PRESENT_MODE_FIFO_KHR;
    output->requested_image_count = capabilities.minImageCount;
    const VkSwapchainCreateInfoKHR swapchain_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = objects.surface,
        .minImageCount = output->requested_image_count,
        .imageFormat = surface_format.format,
        .imageColorSpace = surface_format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = composite_alpha,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    vk_status = vkCreateSwapchainKHR(objects.device, &swapchain_info, NULL,
                                     &objects.swapchain);
    if (vk_status != VK_SUCCESS) {
        BVB_FAIL("vkCreateSwapchainKHR failed: %d", (int)vk_status);
    }
    vk_status = vkGetSwapchainImagesKHR(objects.device, objects.swapchain,
                                        &output->actual_image_count, NULL);
    if (vk_status != VK_SUCCESS || output->actual_image_count == 0U ||
        output->actual_image_count > BVB_MAX_SWAPCHAIN_IMAGES) {
        BVB_FAIL("swapchain image count invalid: %u",
                 output->actual_image_count);
    }
    VkImage images[BVB_MAX_SWAPCHAIN_IMAGES];
    uint32_t image_count = output->actual_image_count;
    vk_status = vkGetSwapchainImagesKHR(objects.device, objects.swapchain,
                                        &image_count, images);
    if (vk_status != VK_SUCCESS) {
        BVB_FAIL("swapchain image query failed: %d", (int)vk_status);
    }

    const VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = output->queue_family_index,
    };
    vk_status = vkCreateCommandPool(objects.device, &pool_info, NULL,
                                    &objects.command_pool);
    if (vk_status != VK_SUCCESS) {
        BVB_FAIL("vkCreateCommandPool failed: %d", (int)vk_status);
    }
    const VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = objects.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    vk_status = vkAllocateCommandBuffers(objects.device, &command_info,
                                         &command_buffer);
    if (vk_status != VK_SUCCESS) {
        BVB_FAIL("vkAllocateCommandBuffers failed: %d", (int)vk_status);
    }
    const VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    vk_status = vkCreateSemaphore(objects.device, &semaphore_info, NULL,
                                  &objects.acquire_semaphore);
    if (vk_status == VK_SUCCESS) {
        vk_status = vkCreateSemaphore(objects.device, &semaphore_info, NULL,
                                      &objects.render_semaphore);
    }
    if (vk_status != VK_SUCCESS) {
        BVB_FAIL("vkCreateSemaphore failed: %d", (int)vk_status);
    }

    vk_status = vkAcquireNextImageKHR(
        objects.device, objects.swapchain, UINT64_MAX,
        objects.acquire_semaphore, VK_NULL_HANDLE,
        &output->acquired_image_index);
    if (vk_status != VK_SUCCESS && vk_status != VK_SUBOPTIMAL_KHR) {
        BVB_FAIL("vkAcquireNextImageKHR failed: %d", (int)vk_status);
    }
    if (output->acquired_image_index >= image_count) {
        BVB_FAIL("acquired swapchain index is out of range");
    }

    const VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vk_status = vkBeginCommandBuffer(command_buffer, &begin_info);
    if (vk_status != VK_SUCCESS) {
        BVB_FAIL("vkBeginCommandBuffer failed: %d", (int)vk_status);
    }
    VkImageMemoryBarrier to_clear = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = images[output->acquired_image_index],
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         1, &to_clear);
    const VkClearColorValue clear_color = {
        .float32 = {1.0F, 0.0F, 1.0F, 1.0F},
    };
    const VkImageSubresourceRange clear_range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    vkCmdClearColorImage(command_buffer,
                         images[output->acquired_image_index],
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_color,
                         1, &clear_range);
    VkImageMemoryBarrier to_present = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = images[output->acquired_image_index],
        .subresourceRange = clear_range,
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0,
                         NULL, 1, &to_present);
    vk_status = vkEndCommandBuffer(command_buffer);
    if (vk_status != VK_SUCCESS) {
        BVB_FAIL("vkEndCommandBuffer failed: %d", (int)vk_status);
    }

    uint64_t present_start_ns = monotonic_ns();
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &objects.acquire_semaphore,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &objects.render_semaphore,
    };
    vk_status = vkQueueSubmit(objects.queue, 1, &submit_info, VK_NULL_HANDLE);
    if (vk_status != VK_SUCCESS) {
        BVB_FAIL("vkQueueSubmit failed: %d", (int)vk_status);
    }
    const VkPresentInfoKHR present_info = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &objects.render_semaphore,
        .swapchainCount = 1,
        .pSwapchains = &objects.swapchain,
        .pImageIndices = &output->acquired_image_index,
    };
    vk_status = vkQueuePresentKHR(objects.queue, &present_info);
    if (vk_status != VK_SUCCESS && vk_status != VK_SUBOPTIMAL_KHR) {
        BVB_FAIL("vkQueuePresentKHR failed: %d", (int)vk_status);
    }
    vk_status = vkQueueWaitIdle(objects.queue);
    if (vk_status != VK_SUCCESS) {
        BVB_FAIL("vkQueueWaitIdle failed: %d", (int)vk_status);
    }
    uint64_t present_end_ns = monotonic_ns();
    output->submit_present_ns = present_end_ns - present_start_ns;

    for (uint32_t poll = 0; poll < BVB_ACQUIRE_POLL_LIMIT; ++poll) {
        media_status = objects.media.acquire_image(objects.reader,
                                                    &objects.image);
        output->acquire_poll_count = poll + 1U;
        if (media_status == BVB_AMEDIA_OK && objects.image != NULL) {
            break;
        }
        if (media_status != BVB_NO_BUFFER_AVAILABLE) {
            BVB_FAIL("AImageReader_acquireNextImage failed: %d",
                     (int)media_status);
        }
        const struct timespec delay = {.tv_nsec = BVB_ACQUIRE_POLL_NS};
        (void)nanosleep(&delay, NULL);
    }
    if (objects.image == NULL) {
        BVB_FAIL("presented image was not available within poll bound");
    }

    int32_t plane_count = 0;
    int32_t pixel_stride = 0;
    int32_t row_stride = 0;
    uint8_t *data = NULL;
    int data_length = 0;
    if (objects.media.get_planes(objects.image, &plane_count) != BVB_AMEDIA_OK ||
        plane_count != 1 ||
        objects.media.get_pixel_stride(objects.image, 0, &pixel_stride) !=
            BVB_AMEDIA_OK ||
        objects.media.get_row_stride(objects.image, 0, &row_stride) !=
            BVB_AMEDIA_OK ||
        objects.media.get_data(objects.image, 0, &data, &data_length) !=
            BVB_AMEDIA_OK ||
        pixel_stride < 4 || row_stride < (int32_t)(output->width * 4U) ||
        data_length < row_stride * (int32_t)output->height || data == NULL) {
        BVB_FAIL("presented RGBA plane is not CPU-readable");
    }
    output->plane_count = (uint32_t)plane_count;
    output->pixel_stride = (uint32_t)pixel_stride;
    output->row_stride = (uint32_t)row_stride;
    output->data_length = (uint32_t)data_length;
    output->expected_rgba_word = UINT32_C(0xffff00ff);
    for (uint32_t y = 0; y < output->height; ++y) {
        for (uint32_t x = 0; x < output->width; ++x) {
            const uint8_t *pixel = data + (size_t)y * output->row_stride +
                                   (size_t)x * output->pixel_stride;
            if (pixel[0] != 255U || pixel[1] != 0U || pixel[2] != 255U ||
                pixel[3] != 255U) {
                ++output->mismatched_pixels;
            }
        }
    }
    if (output->mismatched_pixels != 0U) {
        BVB_FAIL("presented frame has %u mismatched pixels",
                 output->mismatched_pixels);
    }
    output->total_elapsed_ns = monotonic_ns() - start_ns;
    status = 0;

done:
    cleanup(&objects);
#undef BVB_FAIL
    return status;
}

int main(int argc, char **argv) {
    const char *vulkan_path = BVB_DEFAULT_VULKAN_LOADER;
    const char *media_path = BVB_DEFAULT_MEDIA_LOADER;
    if (argc == 5 && strcmp(argv[1], "--loader") == 0 &&
        strcmp(argv[3], "--media-loader") == 0 && argv[2][0] == '/' &&
        argv[4][0] == '/') {
        vulkan_path = argv[2];
        media_path = argv[4];
    } else if (argc != 1) {
        fprintf(stderr,
                "usage: %s [--loader ABSOLUTE_PATH --media-loader "
                "ABSOLUTE_PATH]\n",
                argv[0]);
        return 2;
    }
    struct bvb_result result;
    char error[512] = {0};
    int status = run_selftest(vulkan_path, media_path, &result, error,
                              sizeof(error));
    if (status != 0) {
        fprintf(stderr, "bvb: %s\n", error);
        return status;
    }
    printf("{\"schema_version\":1,\"loader_path\":\"%s\","
           "\"media_loader_path\":\"%s\","
           "\"queue_family_index\":%" PRIu32
           ",\"surface_format\":%" PRIu32
           ",\"color_space\":%" PRIu32
           ",\"present_mode\":%" PRIu32
           ",\"requested_image_count\":%" PRIu32
           ",\"actual_image_count\":%" PRIu32
           ",\"acquired_image_index\":%" PRIu32
           ",\"width\":%" PRIu32 ",\"height\":%" PRIu32
           ",\"plane_count\":%" PRIu32
           ",\"pixel_stride\":%" PRIu32
           ",\"row_stride\":%" PRIu32
           ",\"data_length\":%" PRIu32
           ",\"expected_rgba_word\":%" PRIu32
           ",\"mismatched_pixels\":%" PRIu32
           ",\"acquire_poll_count\":%" PRIu32
           ",\"submit_present_ns\":%" PRIu64
           ",\"total_elapsed_ns\":%" PRIu64 "}\n",
           vulkan_path, media_path, result.queue_family_index,
           result.surface_format, result.color_space, result.present_mode,
           result.requested_image_count, result.actual_image_count,
           result.acquired_image_index, result.width, result.height,
           result.plane_count, result.pixel_stride, result.row_stride,
           result.data_length, result.expected_rgba_word,
           result.mismatched_pixels, result.acquire_poll_count,
           result.submit_present_ns, result.total_elapsed_ns);
    return 0;
}
