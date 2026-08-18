#define VK_USE_PLATFORM_ANDROID_KHR

#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/window.h>
#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BVB_LOG_TAG "BVBVisibleHost"
#define BVB_LOGI(...)                                                           \
    ((void)__android_log_print(ANDROID_LOG_INFO, BVB_LOG_TAG, __VA_ARGS__))
#define BVB_LOGE(...)                                                           \
    ((void)__android_log_print(ANDROID_LOG_ERROR, BVB_LOG_TAG, __VA_ARGS__))

enum { BVB_MAX_IMAGES = 64 };

enum {
    BVB_SYSTEM_UI_FLAG_HIDE_NAVIGATION = 0x00000002,
    BVB_SYSTEM_UI_FLAG_FULLSCREEN = 0x00000004,
    BVB_SYSTEM_UI_FLAG_LAYOUT_STABLE = 0x00000100,
    BVB_SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION = 0x00000200,
    BVB_SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN = 0x00000400,
    BVB_SYSTEM_UI_FLAG_IMMERSIVE_STICKY = 0x00001000,
};

struct bvb_visible_state {
    ANativeWindow *window;
    VkInstance instance;
    VkSurfaceKHR surface;
    VkDevice device;
    VkQueue queue;
    VkSwapchainKHR swapchain;
    VkCommandPool command_pool;
    VkSemaphore acquire_semaphore;
    VkSemaphore render_semaphore;
};

static struct bvb_visible_state state;

static void apply_immersive_mode(ANativeActivity *activity) {
    JNIEnv *env = activity->env;
    jclass activity_class = (*env)->GetObjectClass(env, activity->clazz);
    jmethodID get_window = (*env)->GetMethodID(
        env, activity_class, "getWindow", "()Landroid/view/Window;");
    jobject window = get_window == NULL
                         ? NULL
                         : (*env)->CallObjectMethod(env, activity->clazz,
                                                    get_window);
    jclass window_class = window == NULL
                              ? NULL
                              : (*env)->GetObjectClass(env, window);
    jmethodID get_decor_view = window_class == NULL
                                   ? NULL
                                   : (*env)->GetMethodID(
                                         env, window_class, "getDecorView",
                                         "()Landroid/view/View;");
    jobject decor_view = get_decor_view == NULL
                             ? NULL
                             : (*env)->CallObjectMethod(env, window,
                                                        get_decor_view);
    jclass view_class = decor_view == NULL
                            ? NULL
                            : (*env)->GetObjectClass(env, decor_view);
    jmethodID set_visibility = view_class == NULL
                                   ? NULL
                                   : (*env)->GetMethodID(
                                         env, view_class,
                                         "setSystemUiVisibility", "(I)V");
    if (set_visibility != NULL) {
        const jint flags = BVB_SYSTEM_UI_FLAG_HIDE_NAVIGATION |
                           BVB_SYSTEM_UI_FLAG_FULLSCREEN |
                           BVB_SYSTEM_UI_FLAG_LAYOUT_STABLE |
                           BVB_SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
                           BVB_SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN |
                           BVB_SYSTEM_UI_FLAG_IMMERSIVE_STICKY;
        (*env)->CallVoidMethod(env, decor_view, set_visibility, flags);
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        BVB_LOGE("E009_FAIL immersive_jni_exception");
    } else if (set_visibility == NULL) {
        BVB_LOGE("E009_FAIL immersive_method_lookup");
    } else {
        BVB_LOGI("E009_IMMERSIVE_APPLIED");
    }
    if (view_class != NULL) {
        (*env)->DeleteLocalRef(env, view_class);
    }
    if (decor_view != NULL) {
        (*env)->DeleteLocalRef(env, decor_view);
    }
    if (window_class != NULL) {
        (*env)->DeleteLocalRef(env, window_class);
    }
    if (window != NULL) {
        (*env)->DeleteLocalRef(env, window);
    }
    if (activity_class != NULL) {
        (*env)->DeleteLocalRef(env, activity_class);
    }
}

static void destroy_renderer(void) {
    if (state.device != VK_NULL_HANDLE) {
        (void)vkDeviceWaitIdle(state.device);
        if (state.render_semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(state.device, state.render_semaphore, NULL);
        }
        if (state.acquire_semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(state.device, state.acquire_semaphore, NULL);
        }
        if (state.command_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(state.device, state.command_pool, NULL);
        }
        if (state.swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(state.device, state.swapchain, NULL);
        }
        vkDestroyDevice(state.device, NULL);
    }
    if (state.surface != VK_NULL_HANDLE && state.instance != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(state.instance, state.surface, NULL);
    }
    if (state.instance != VK_NULL_HANDLE) {
        vkDestroyInstance(state.instance, NULL);
    }
    memset(&state, 0, sizeof(state));
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

static bool has_device_extension(VkPhysicalDevice physical_device,
                                 const char *wanted) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(physical_device, NULL, &count,
                                             NULL) != VK_SUCCESS ||
        count == 0U || count > 1024U) {
        return false;
    }
    VkExtensionProperties *properties = calloc(count, sizeof(*properties));
    if (properties == NULL) {
        return false;
    }
    VkResult result = vkEnumerateDeviceExtensionProperties(
        physical_device, NULL, &count, properties);
    bool found = false;
    if (result == VK_SUCCESS || result == VK_INCOMPLETE) {
        for (uint32_t index = 0; index < count; ++index) {
            found |= strcmp(properties[index].extensionName, wanted) == 0;
        }
    }
    free(properties);
    return found;
}

static VkExtent2D choose_extent(const VkSurfaceCapabilitiesKHR *capabilities,
                                ANativeWindow *window) {
    if (capabilities->currentExtent.width != UINT32_MAX) {
        return capabilities->currentExtent;
    }
    VkExtent2D extent = {
        .width = (uint32_t)ANativeWindow_getWidth(window),
        .height = (uint32_t)ANativeWindow_getHeight(window),
    };
    if (extent.width < capabilities->minImageExtent.width) {
        extent.width = capabilities->minImageExtent.width;
    }
    if (extent.width > capabilities->maxImageExtent.width) {
        extent.width = capabilities->maxImageExtent.width;
    }
    if (extent.height < capabilities->minImageExtent.height) {
        extent.height = capabilities->minImageExtent.height;
    }
    if (extent.height > capabilities->maxImageExtent.height) {
        extent.height = capabilities->maxImageExtent.height;
    }
    return extent;
}

static bool create_renderer(ANativeWindow *window) {
    static const char *const instance_extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
    };
    const VkApplicationInfo application_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "bvb-visible-host",
        .applicationVersion = VK_MAKE_API_VERSION(0, 0, 8, 0),
        .pEngineName = "none",
        .apiVersion = VK_API_VERSION_1_0,
    };
    const VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application_info,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = instance_extensions,
    };
    VkResult result = vkCreateInstance(&instance_info, NULL, &state.instance);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E008_FAIL vkCreateInstance=%d", (int)result);
        return false;
    }

    const VkAndroidSurfaceCreateInfoKHR surface_info = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        .window = window,
    };
    result = vkCreateAndroidSurfaceKHR(state.instance, &surface_info, NULL,
                                       &state.surface);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E008_FAIL vkCreateAndroidSurfaceKHR=%d", (int)result);
        return false;
    }

    uint32_t device_count = 0;
    result = vkEnumeratePhysicalDevices(state.instance, &device_count, NULL);
    if (result != VK_SUCCESS || device_count == 0U || device_count > 16U) {
        BVB_LOGE("E008_FAIL physical_devices result=%d count=%u", (int)result,
                 device_count);
        return false;
    }
    VkPhysicalDevice devices[16];
    result = vkEnumeratePhysicalDevices(state.instance, &device_count, devices);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        BVB_LOGE("E008_FAIL physical_device_list=%d", (int)result);
        return false;
    }
    VkPhysicalDevice physical_device = devices[0];
    if (!has_device_extension(physical_device, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        BVB_LOGE("E008_FAIL no_swapchain_extension");
        return false;
    }

    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device,
                                             &queue_family_count, NULL);
    if (queue_family_count == 0U || queue_family_count > 256U) {
        BVB_LOGE("E008_FAIL queue_family_count=%u", queue_family_count);
        return false;
    }
    VkQueueFamilyProperties *queue_properties =
        calloc(queue_family_count, sizeof(*queue_properties));
    if (queue_properties == NULL) {
        BVB_LOGE("E008_FAIL queue_allocation");
        return false;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device,
                                             &queue_family_count,
                                             queue_properties);
    bool queue_found = false;
    uint32_t queue_family_index = 0;
    for (uint32_t index = 0; index < queue_family_count; ++index) {
        VkBool32 present_support = VK_FALSE;
        result = vkGetPhysicalDeviceSurfaceSupportKHR(
            physical_device, index, state.surface, &present_support);
        if (result == VK_SUCCESS && present_support == VK_TRUE &&
            queue_properties[index].queueCount > 0U &&
            (queue_properties[index].queueFlags &
             (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT |
              VK_QUEUE_TRANSFER_BIT)) != 0U) {
            queue_family_index = index;
            queue_found = true;
            break;
        }
    }
    free(queue_properties);
    if (!queue_found) {
        BVB_LOGE("E008_FAIL no_present_queue");
        return false;
    }

    VkSurfaceCapabilitiesKHR capabilities;
    result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        physical_device, state.surface, &capabilities);
    if (result != VK_SUCCESS || capabilities.minImageCount == 0U ||
        (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ==
            0U) {
        BVB_LOGE("E008_FAIL surface_capabilities=%d usage=%u", (int)result,
                 capabilities.supportedUsageFlags);
        return false;
    }
    VkCompositeAlphaFlagBitsKHR composite_alpha =
        choose_composite_alpha(capabilities.supportedCompositeAlpha);
    if (composite_alpha == 0) {
        BVB_LOGE("E008_FAIL composite_alpha");
        return false;
    }

    uint32_t format_count = 0;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(
        physical_device, state.surface, &format_count, NULL);
    if (result != VK_SUCCESS || format_count == 0U || format_count > 64U) {
        BVB_LOGE("E008_FAIL format_count=%u result=%d", format_count,
                 (int)result);
        return false;
    }
    VkSurfaceFormatKHR formats[64];
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(
        physical_device, state.surface, &format_count, formats);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E008_FAIL formats=%d", (int)result);
        return false;
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
        BVB_LOGE("E008_FAIL no_rgba8_surface_format");
        return false;
    }

    const float queue_priority = 1.0F;
    const VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queue_family_index,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };
    static const char *const device_extensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    const VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = device_extensions,
    };
    result = vkCreateDevice(physical_device, &device_info, NULL,
                            &state.device);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E008_FAIL vkCreateDevice=%d", (int)result);
        return false;
    }
    vkGetDeviceQueue(state.device, queue_family_index, 0, &state.queue);

    VkExtent2D extent = choose_extent(&capabilities, window);
    const VkSwapchainCreateInfoKHR swapchain_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = state.surface,
        .minImageCount = capabilities.minImageCount,
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
    result = vkCreateSwapchainKHR(state.device, &swapchain_info, NULL,
                                  &state.swapchain);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E008_FAIL vkCreateSwapchainKHR=%d", (int)result);
        return false;
    }
    uint32_t image_count = 0;
    result = vkGetSwapchainImagesKHR(state.device, state.swapchain,
                                     &image_count, NULL);
    if (result != VK_SUCCESS || image_count == 0U ||
        image_count > BVB_MAX_IMAGES) {
        BVB_LOGE("E008_FAIL swapchain_images=%u result=%d", image_count,
                 (int)result);
        return false;
    }
    VkImage images[BVB_MAX_IMAGES];
    result = vkGetSwapchainImagesKHR(state.device, state.swapchain,
                                     &image_count, images);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E008_FAIL swapchain_image_list=%d", (int)result);
        return false;
    }

    const VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = queue_family_index,
    };
    result = vkCreateCommandPool(state.device, &pool_info, NULL,
                                 &state.command_pool);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E008_FAIL command_pool=%d", (int)result);
        return false;
    }
    const VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = state.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    result = vkAllocateCommandBuffers(state.device, &command_info,
                                      &command_buffer);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E008_FAIL command_buffer=%d", (int)result);
        return false;
    }
    const VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    result = vkCreateSemaphore(state.device, &semaphore_info, NULL,
                               &state.acquire_semaphore);
    if (result == VK_SUCCESS) {
        result = vkCreateSemaphore(state.device, &semaphore_info, NULL,
                                   &state.render_semaphore);
    }
    if (result != VK_SUCCESS) {
        BVB_LOGE("E008_FAIL semaphore=%d", (int)result);
        return false;
    }

    uint32_t image_index = 0;
    result = vkAcquireNextImageKHR(state.device, state.swapchain, UINT64_MAX,
                                   state.acquire_semaphore, VK_NULL_HANDLE,
                                   &image_index);
    if ((result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) ||
        image_index >= image_count) {
        BVB_LOGE("E008_FAIL acquire=%d index=%u", (int)result, image_index);
        return false;
    }
    const VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    result = vkBeginCommandBuffer(command_buffer, &begin_info);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E008_FAIL command_begin=%d", (int)result);
        return false;
    }
    const VkImageSubresourceRange range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    VkImageMemoryBarrier to_clear = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = images[image_index],
        .subresourceRange = range,
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         1, &to_clear);
    const VkClearColorValue clear_color = {
        .float32 = {1.0F, 0.0F, 1.0F, 1.0F},
    };
    vkCmdClearColorImage(command_buffer, images[image_index],
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_color, 1,
                         &range);
    VkImageMemoryBarrier to_present = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = images[image_index],
        .subresourceRange = range,
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0,
                         NULL, 1, &to_present);
    result = vkEndCommandBuffer(command_buffer);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E008_FAIL command_end=%d", (int)result);
        return false;
    }

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &state.acquire_semaphore,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &state.render_semaphore,
    };
    result = vkQueueSubmit(state.queue, 1, &submit_info, VK_NULL_HANDLE);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E008_FAIL submit=%d", (int)result);
        return false;
    }
    const VkPresentInfoKHR present_info = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &state.render_semaphore,
        .swapchainCount = 1,
        .pSwapchains = &state.swapchain,
        .pImageIndices = &image_index,
    };
    result = vkQueuePresentKHR(state.queue, &present_info);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        BVB_LOGE("E008_FAIL present=%d", (int)result);
        return false;
    }
    result = vkQueueWaitIdle(state.queue);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E008_FAIL queue_idle=%d", (int)result);
        return false;
    }
    state.window = window;
    BVB_LOGI("E008_PASS width=%u height=%u images=%u index=%u format=%d",
             extent.width, extent.height, image_count, image_index,
             (int)surface_format.format);
    return true;
}

static void on_window_created(ANativeActivity *activity,
                              ANativeWindow *window) {
    (void)activity;
    destroy_renderer();
    BVB_LOGI("E008_WINDOW_CREATED width=%d height=%d",
             ANativeWindow_getWidth(window), ANativeWindow_getHeight(window));
    if (!create_renderer(window)) {
        destroy_renderer();
    }
}

static void on_window_resized(ANativeActivity *activity,
                              ANativeWindow *window) {
    (void)activity;
    if (state.window != window || state.swapchain == VK_NULL_HANDLE) {
        destroy_renderer();
        (void)create_renderer(window);
    }
}

static void on_window_redraw_needed(ANativeActivity *activity,
                                    ANativeWindow *window) {
    (void)activity;
    if (state.window != window || state.swapchain == VK_NULL_HANDLE) {
        destroy_renderer();
        (void)create_renderer(window);
    }
}

static void on_window_destroyed(ANativeActivity *activity,
                                ANativeWindow *window) {
    (void)activity;
    (void)window;
    BVB_LOGI("E008_WINDOW_DESTROYED");
    destroy_renderer();
}

static void on_window_focus_changed(ANativeActivity *activity, int has_focus) {
    if (has_focus != 0) {
        apply_immersive_mode(activity);
    }
}

__attribute__((visibility("default"))) void
ANativeActivity_onCreate(ANativeActivity *activity, void *saved_state,
                         size_t saved_state_size) {
    (void)saved_state;
    (void)saved_state_size;
    memset(&state, 0, sizeof(state));
    activity->callbacks->onNativeWindowCreated = on_window_created;
    activity->callbacks->onNativeWindowResized = on_window_resized;
    activity->callbacks->onNativeWindowRedrawNeeded = on_window_redraw_needed;
    activity->callbacks->onNativeWindowDestroyed = on_window_destroyed;
    activity->callbacks->onWindowFocusChanged = on_window_focus_changed;
    ANativeActivity_setWindowFormat(activity, WINDOW_FORMAT_RGBA_8888);
    ANativeActivity_setWindowFlags(
        activity, AWINDOW_FLAG_FULLSCREEN | AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
    apply_immersive_mode(activity);
    BVB_LOGI("E008_ACTIVITY_CREATED");
}
