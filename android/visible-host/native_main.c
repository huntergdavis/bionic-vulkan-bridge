#define VK_USE_PLATFORM_ANDROID_KHR

#include <bvb/command_batch.h>
#include <bvb/lifecycle.h>
#include <bvb/visible_ingress.h>

#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/window.h>
#include <vulkan/vulkan.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "triangle_shaders.inc"

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
    VkImageView image_view;
    VkRenderPass render_pass;
    VkFramebuffer framebuffer;
    VkShaderModule vertex_shader;
    VkShaderModule fragment_shader;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    VkCommandPool command_pool;
    VkSemaphore acquire_semaphore;
    VkSemaphore render_semaphore;
};

struct bvb_lifecycle_client {
    bool configured;
    uint16_t port;
    uint32_t next_sequence;
    uint8_t token[BVB_LIFECYCLE_TOKEN_SIZE];
};

struct bvb_renderer_control {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool thread_started;
    bool ready;
    uint64_t requested_generation;
    uint64_t completed_generation;
    uint32_t width;
    uint32_t height;
    ANativeWindow *window;
};

static struct bvb_visible_state state;
static struct bvb_lifecycle_client lifecycle;
static struct bvb_visible_ingress *visible_ingress;
static bool visible_inline_ingress;
static atomic_bool visible_brokered_ingress;
static pthread_mutex_t lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct bvb_renderer_control renderer = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .condition = PTHREAD_COND_INITIALIZER,
};

enum {
    BVB_E020_REGION_BYTES = 4096,
    BVB_E022_REGION_GENERATION = 1,
};

static bool token_matches(const uint8_t *left, const uint8_t *right) {
    uint8_t difference = 0U;
    for (size_t index = 0U; index < BVB_LIFECYCLE_TOKEN_SIZE; ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0U;
}

JNIEXPORT jint JNICALL
Java_io_github_huntergdavis_bvb_visiblehost_SharedRegionProvider_nativeOpenRegion(
    JNIEnv *env, jclass provider_class, jstring token_string) {
    (void)provider_class;
    if (token_string == NULL) {
        return -EINVAL;
    }
    const char *token = (*env)->GetStringUTFChars(env, token_string, NULL);
    if (token == NULL || (*env)->ExceptionCheck(env)) {
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
        }
        return -EINVAL;
    }
    uint8_t parsed_token[BVB_LIFECYCLE_TOKEN_SIZE];
    int result = bvb_lifecycle_token_from_hex(token, parsed_token);
    (*env)->ReleaseStringUTFChars(env, token_string, token);
    if (result != 0) {
        return result;
    }

    (void)pthread_mutex_lock(&lifecycle_mutex);
    bool authorized = lifecycle.configured &&
                      token_matches(parsed_token, lifecycle.token);
    (void)pthread_mutex_unlock(&lifecycle_mutex);
    if (!authorized) {
        return -EACCES;
    }

    int memory_fd = (int)syscall(SYS_memfd_create, "bvb-e020-region",
                                 MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (memory_fd < 0) {
        return -errno;
    }
    static const char marker[] =
        "BVB_E020_SHARED_REGION binder_parcel_fd=PASS\n";
    if (ftruncate(memory_fd, BVB_E020_REGION_BYTES) != 0) {
        int saved_error = errno;
        (void)close(memory_fd);
        return -saved_error;
    }
    ssize_t written = pwrite(memory_fd, marker, sizeof(marker) - 1U, 0);
    if (written != (ssize_t)(sizeof(marker) - 1U)) {
        int saved_error = written < 0 ? errno : EIO;
        (void)close(memory_fd);
        return -saved_error;
    }
    if (fcntl(memory_fd, F_ADD_SEALS,
              F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0) {
        int saved_error = errno;
        (void)close(memory_fd);
        return -saved_error;
    }
    (void)pthread_mutex_lock(&lifecycle_mutex);
    bool install_required = lifecycle.configured && visible_inline_ingress &&
                            visible_ingress != NULL;
    if (install_required &&
        !token_matches(parsed_token, lifecycle.token)) {
        result = -EACCES;
    } else if (install_required) {
        result = bvb_visible_ingress_install_region(
            visible_ingress, memory_fd, BVB_E020_REGION_BYTES,
            BVB_E022_REGION_GENERATION);
    }
    (void)pthread_mutex_unlock(&lifecycle_mutex);
    if (result != 0) {
        (void)close(memory_fd);
        BVB_LOGE("E022_REGION_INSTALL_FAIL status=%d", result);
        return result;
    }
    if (install_required) {
        atomic_store(&visible_brokered_ingress, true);
        BVB_LOGI("E022_REGION_INSTALLED bytes=%u generation=%u",
                 BVB_E020_REGION_BYTES, BVB_E022_REGION_GENERATION);
    }
    BVB_LOGI("E020_REGION_OPEN bytes=%u", BVB_E020_REGION_BYTES);
    return memory_fd;
}

static void configure_lifecycle(ANativeActivity *activity) {
    (void)pthread_mutex_lock(&lifecycle_mutex);
    memset(&lifecycle, 0, sizeof(lifecycle));
    JNIEnv *env = activity->env;
    jclass activity_class = (*env)->GetObjectClass(env, activity->clazz);
    jmethodID get_intent = activity_class == NULL
                               ? NULL
                               : (*env)->GetMethodID(
                                     env, activity_class, "getIntent",
                                     "()Landroid/content/Intent;");
    jobject intent = get_intent == NULL
                         ? NULL
                         : (*env)->CallObjectMethod(env, activity->clazz,
                                                    get_intent);
    jclass intent_class = intent == NULL
                              ? NULL
                              : (*env)->GetObjectClass(env, intent);
    jmethodID get_int_extra = intent_class == NULL
                                  ? NULL
                                  : (*env)->GetMethodID(
                                        env, intent_class, "getIntExtra",
                                        "(Ljava/lang/String;I)I");
    jmethodID get_string_extra = intent_class == NULL
                                     ? NULL
                                     : (*env)->GetMethodID(
                                           env, intent_class, "getStringExtra",
                                           "(Ljava/lang/String;)Ljava/lang/String;");
    jstring port_key = (*env)->NewStringUTF(env, "bvb_activity_port");
    jstring token_key = (*env)->NewStringUTF(env, "bvb_activity_token");
    jstring socket_key = (*env)->NewStringUTF(env, "bvb_visible_socket");
    jstring visible_port_key =
        (*env)->NewStringUTF(env, "bvb_visible_port");
    jint port = get_int_extra == NULL || port_key == NULL
                    ? 0
                    : (*env)->CallIntMethod(env, intent, get_int_extra, port_key,
                                            0);
    jint visible_port = get_int_extra == NULL || visible_port_key == NULL
                            ? 0
                            : (*env)->CallIntMethod(env, intent, get_int_extra,
                                                    visible_port_key, 0);
    jstring token_string = get_string_extra == NULL || token_key == NULL
                               ? NULL
                               : (jstring)(*env)->CallObjectMethod(
                                     env, intent, get_string_extra, token_key);
    jstring socket_string = get_string_extra == NULL || socket_key == NULL
                                ? NULL
                                : (jstring)(*env)->CallObjectMethod(
                                      env, intent, get_string_extra,
                                      socket_key);
    bool jni_ok = !(*env)->ExceptionCheck(env);
    const char *token =
        !jni_ok || token_string == NULL
            ? NULL
            : (*env)->GetStringUTFChars(env, token_string, NULL);
    jni_ok = jni_ok && !(*env)->ExceptionCheck(env);
    const char *socket_name =
        !jni_ok || socket_string == NULL
            ? NULL
            : (*env)->GetStringUTFChars(env, socket_string, NULL);
    jni_ok = jni_ok && !(*env)->ExceptionCheck(env);
    uint8_t parsed_token[BVB_LIFECYCLE_TOKEN_SIZE];
    const bool token_valid = jni_ok && token != NULL &&
                             bvb_lifecycle_token_from_hex(token,
                                                          parsed_token) == 0;
    if (token_valid && port > 0 && port <= UINT16_MAX) {
        lifecycle.configured = true;
        lifecycle.port = (uint16_t)port;
        lifecycle.next_sequence = 1;
        memcpy(lifecycle.token, parsed_token, sizeof(lifecycle.token));
        BVB_LOGI("E010_LIFECYCLE_CONFIGURED port=%u", (unsigned int)port);
    } else {
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
        }
        BVB_LOGI("E010_LIFECYCLE_DISABLED");
    }
    if (visible_ingress == NULL) {
        visible_inline_ingress = false;
        atomic_store(&visible_brokered_ingress, false);
    }
    if (token_valid && visible_port > 0 && visible_port <= UINT16_MAX &&
        visible_ingress == NULL) {
        uint16_t bound_port = 0U;
        int result = bvb_visible_ingress_create_loopback(
            &visible_ingress, (uint16_t)visible_port, &bound_port,
            parsed_token);
        if (result == 0 && bound_port == (uint16_t)visible_port) {
            visible_inline_ingress = true;
            BVB_LOGI("E019_INGRESS_READY port=%u", (unsigned int)bound_port);
        } else {
            if (result == 0) {
                bvb_visible_ingress_destroy(visible_ingress);
                visible_ingress = NULL;
                result = -EADDRNOTAVAIL;
            }
            BVB_LOGE("E019_INGRESS_FAIL status=%d", result);
        }
    } else if (token_valid && socket_name != NULL &&
               socket_name[0] != '\0' && visible_ingress == NULL) {
        int result = bvb_visible_ingress_create(
            &visible_ingress, (const uint8_t *)socket_name,
            strlen(socket_name), parsed_token);
        if (result == 0) {
            BVB_LOGI("E018_INGRESS_READY name=%s", socket_name);
        } else {
            BVB_LOGE("E018_INGRESS_FAIL status=%d", result);
        }
    } else if ((socket_name == NULL || socket_name[0] == '\0') &&
               visible_port == 0) {
        BVB_LOGI("E018_INGRESS_DISABLED");
    }
    if (socket_name != NULL) {
        (*env)->ReleaseStringUTFChars(env, socket_string, socket_name);
    }
    if (token != NULL) {
        (*env)->ReleaseStringUTFChars(env, token_string, token);
    }
    if (token_string != NULL) {
        (*env)->DeleteLocalRef(env, token_string);
    }
    if (socket_string != NULL) {
        (*env)->DeleteLocalRef(env, socket_string);
    }
    if (socket_key != NULL) {
        (*env)->DeleteLocalRef(env, socket_key);
    }
    if (visible_port_key != NULL) {
        (*env)->DeleteLocalRef(env, visible_port_key);
    }
    if (token_key != NULL) {
        (*env)->DeleteLocalRef(env, token_key);
    }
    if (port_key != NULL) {
        (*env)->DeleteLocalRef(env, port_key);
    }
    if (intent_class != NULL) {
        (*env)->DeleteLocalRef(env, intent_class);
    }
    if (intent != NULL) {
        (*env)->DeleteLocalRef(env, intent);
    }
    if (activity_class != NULL) {
        (*env)->DeleteLocalRef(env, activity_class);
    }
    (void)pthread_mutex_unlock(&lifecycle_mutex);
}

static int send_exact(int socket_fd, const uint8_t *input, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        ssize_t sent = send(socket_fd, input + offset, length - offset,
                            MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        offset += (size_t)sent;
    }
    return 0;
}

static int receive_exact(int socket_fd, uint8_t *output, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        ssize_t received = recv(socket_fd, output + offset, length - offset, 0);
        if (received == 0) {
            return -ECONNRESET;
        }
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        offset += (size_t)received;
    }
    return 0;
}

static void emit_lifecycle(uint16_t event, uint32_t width, uint32_t height) {
    (void)pthread_mutex_lock(&lifecycle_mutex);
    if (!lifecycle.configured) {
        (void)pthread_mutex_unlock(&lifecycle_mutex);
        return;
    }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        BVB_LOGE("E010_EVENT_FAIL event=%u clock=%d", (unsigned int)event,
                 errno);
        lifecycle.configured = false;
        (void)pthread_mutex_unlock(&lifecycle_mutex);
        return;
    }
    struct bvb_lifecycle_record record = {
        .event = event,
        .sequence = lifecycle.next_sequence,
        .width = width,
        .height = height,
        .activity_pid = (uint32_t)getpid(),
        .monotonic_ns = (uint64_t)now.tv_sec * UINT64_C(1000000000) +
                        (uint64_t)now.tv_nsec,
    };
    memcpy(record.token, lifecycle.token, sizeof(record.token));
    uint8_t wire[BVB_LIFECYCLE_RECORD_SIZE];
    int result = bvb_lifecycle_encode_record(wire, &record);
    int socket_fd = -1;
    if (result == 0) {
        socket_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
        result = socket_fd < 0 ? -errno : 0;
    }
    if (result == 0) {
        const struct timeval timeout = {.tv_sec = 0, .tv_usec = 500000};
        if (setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                       sizeof(timeout)) != 0 ||
            setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                       sizeof(timeout)) != 0) {
            result = -errno;
        }
    }
    if (result == 0) {
        const struct sockaddr_in address = {
            .sin_family = AF_INET,
            .sin_port = htons(lifecycle.port),
            .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)},
        };
        if (connect(socket_fd, (const struct sockaddr *)&address,
                    sizeof(address)) != 0) {
            result = -errno;
        }
    }
    if (result == 0) {
        result = send_exact(socket_fd, wire, sizeof(wire));
    }
    uint8_t ack_wire[BVB_LIFECYCLE_ACK_SIZE];
    if (result == 0) {
        result = receive_exact(socket_fd, ack_wire, sizeof(ack_wire));
    }
    struct bvb_lifecycle_ack ack;
    if (result == 0) {
        result = bvb_lifecycle_decode_ack(ack_wire, &ack);
    }
    if (result == 0 &&
        (ack.sequence != lifecycle.next_sequence || ack.status != 0)) {
        result = ack.status != 0 ? ack.status : -EPROTO;
    }
    if (socket_fd >= 0) {
        (void)close(socket_fd);
    }
    if (result != 0) {
        BVB_LOGE("E010_EVENT_FAIL event=%u sequence=%u status=%d",
                 (unsigned int)event, lifecycle.next_sequence, result);
        lifecycle.configured = false;
        (void)pthread_mutex_unlock(&lifecycle_mutex);
        return;
    }
    BVB_LOGI("E010_EVENT_ACK event=%u sequence=%u", (unsigned int)event,
             lifecycle.next_sequence);
    lifecycle.next_sequence += 1U;
    (void)pthread_mutex_unlock(&lifecycle_mutex);
}

static void disable_lifecycle(void) {
    (void)pthread_mutex_lock(&lifecycle_mutex);
    memset(&lifecycle, 0, sizeof(lifecycle));
    (void)pthread_mutex_unlock(&lifecycle_mutex);
}

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
        if (state.framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(state.device, state.framebuffer, NULL);
        }
        if (state.pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(state.device, state.pipeline, NULL);
        }
        if (state.pipeline_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(state.device, state.pipeline_layout, NULL);
        }
        if (state.fragment_shader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(state.device, state.fragment_shader, NULL);
        }
        if (state.vertex_shader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(state.device, state.vertex_shader, NULL);
        }
        if (state.render_pass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(state.device, state.render_pass, NULL);
        }
        if (state.image_view != VK_NULL_HANDLE) {
            vkDestroyImageView(state.device, state.image_view, NULL);
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

static uint64_t native_handle_bits(const void *handle, size_t size) {
    uint64_t bits = 0U;
    if (handle != NULL && size <= sizeof(bits)) {
        memcpy(&bits, handle, size);
    }
    return bits;
}

static VkCommandBuffer command_buffer_from_bits(uint64_t bits) {
    VkCommandBuffer handle = VK_NULL_HANDLE;
    memcpy(&handle, &bits, sizeof(handle));
    return handle;
}

static VkImageView image_view_from_bits(uint64_t bits) {
    VkImageView handle = VK_NULL_HANDLE;
    memcpy(&handle, &bits, sizeof(handle));
    return handle;
}

static VkPipeline pipeline_from_bits(uint64_t bits) {
    VkPipeline handle = VK_NULL_HANDLE;
    memcpy(&handle, &bits, sizeof(handle));
    return handle;
}

static int build_triangle_batch(uint8_t *batch, size_t capacity,
                                VkExtent2D extent, size_t *length) {
    const uint64_t command_buffer_id =
        bvb_handle_id(BVB_OBJECT_COMMAND_BUFFER, 1U);
    const uint64_t image_view_id =
        bvb_handle_id(BVB_OBJECT_IMAGE_VIEW, 1U);
    const uint64_t pipeline_id = bvb_handle_id(BVB_OBJECT_PIPELINE, 1U);
    struct bvb_command_batch_builder builder;
    int result = bvb_command_batch_begin(&builder, batch, capacity,
                                         command_buffer_id, 1U);
    if (result == 0) {
        result = bvb_command_batch_append_begin_rendering(
            &builder,
            &(const struct bvb_begin_rendering_command){
                .color_image_view_id = image_view_id,
                .width = extent.width,
                .height = extent.height,
                .image_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .store_op = VK_ATTACHMENT_STORE_OP_STORE,
                .layer_count = 1U,
                .clear_color = {0.015F, 0.02F, 0.06F, 1.0F},
            });
    }
    if (result == 0) {
        result = bvb_command_batch_append_bind_graphics_pipeline(
            &builder,
            &(const struct bvb_bind_graphics_pipeline_command){
                .pipeline_id = pipeline_id,
            });
    }
    if (result == 0) {
        result = bvb_command_batch_append_set_viewport(
            &builder,
            &(const struct bvb_set_viewport_command){
                .x = 0.0F,
                .y = 0.0F,
                .width = (float)extent.width,
                .height = (float)extent.height,
                .minimum_depth = 0.0F,
                .maximum_depth = 1.0F,
            });
    }
    if (result == 0) {
        result = bvb_command_batch_append_set_scissor(
            &builder,
            &(const struct bvb_set_scissor_command){
                .x = 0,
                .y = 0,
                .width = extent.width,
                .height = extent.height,
            });
    }
    if (result == 0) {
        result = bvb_command_batch_append_draw(
            &builder,
            &(const struct bvb_draw_command){
                .vertex_count = 3U,
                .instance_count = 1U,
            });
    }
    if (result == 0) {
        result = bvb_command_batch_append_end_rendering(&builder);
    }
    if (result == 0) {
        result = bvb_command_batch_finish(&builder, length);
    }
    return result;
}

static int replay_triangle_batch(
    const uint8_t *batch, size_t length, VkCommandBuffer command_buffer,
    VkImageView image_view, VkPipeline pipeline, VkRenderPass render_pass,
    VkFramebuffer framebuffer, VkExtent2D extent) {
    struct bvb_command_batch_info info;
    int result = bvb_command_batch_validate(batch, length, &info);
    if (result != 0 || info.command_count != 6U ||
        render_pass == VK_NULL_HANDLE || framebuffer == VK_NULL_HANDLE) {
        return result != 0 ? result : -EPROTO;
    }
    const uint64_t image_view_id =
        bvb_handle_id(BVB_OBJECT_IMAGE_VIEW, 1U);
    const uint64_t pipeline_id = bvb_handle_id(BVB_OBJECT_PIPELINE, 1U);
    struct bvb_handle_entry entries[8];
    struct bvb_handle_table handles;
    result = bvb_handle_table_init(&handles, entries, 8U);
    if (result == 0) {
        result = bvb_handle_table_insert(
            &handles, info.command_buffer_id, 0U,
            native_handle_bits(&command_buffer, sizeof(command_buffer)));
    }
    if (result == 0) {
        result = bvb_handle_table_insert(
            &handles, image_view_id, 0U,
            native_handle_bits(&image_view, sizeof(image_view)));
    }
    if (result == 0) {
        result = bvb_handle_table_insert(
            &handles, pipeline_id, 0U,
            native_handle_bits(&pipeline, sizeof(pipeline)));
    }
    if (result != 0) {
        return result;
    }
    uint64_t native_command_buffer = 0U;
    result = bvb_handle_table_lookup(
        &handles, info.command_buffer_id, BVB_OBJECT_COMMAND_BUFFER, NULL,
        &native_command_buffer);
    if (result != 0 ||
        command_buffer_from_bits(native_command_buffer) != command_buffer) {
        return -EPROTO;
    }

    struct bvb_command_batch_iterator iterator;
    result = bvb_command_batch_iterator_init(&iterator, batch, length);
    if (result != 0) {
        return result;
    }
    struct bvb_command_record record;
    for (uint32_t index = 0U; index < 6U; ++index) {
        result = bvb_command_batch_next(&iterator, &record);
        if (result != 0) {
            return -EPROTO;
        }
        if (index == 0U && record.opcode == BVB_COMMAND_BEGIN_RENDERING) {
            struct bvb_begin_rendering_command command;
            uint64_t image_view_bits = 0U;
            result = bvb_command_decode_begin_rendering(&record, &command);
            if (result == 0) {
                result = bvb_handle_table_lookup(
                    &handles, command.color_image_view_id,
                    BVB_OBJECT_IMAGE_VIEW, NULL, &image_view_bits);
            }
            if (result != 0) {
                return result;
            }
            if (image_view_from_bits(image_view_bits) != image_view ||
                command.image_layout !=
                    (uint32_t)VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
                command.load_op != (uint32_t)VK_ATTACHMENT_LOAD_OP_CLEAR ||
                command.store_op != (uint32_t)VK_ATTACHMENT_STORE_OP_STORE ||
                command.layer_count != 1U || command.width != extent.width ||
                command.height != extent.height) {
                return -ENOTSUP;
            }
            const VkClearValue clear_value = {
                .color.float32 = {
                    command.clear_color[0], command.clear_color[1],
                    command.clear_color[2], command.clear_color[3],
                },
            };
            const VkRenderPassBeginInfo render_pass_begin = {
                .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                .renderPass = render_pass,
                .framebuffer = framebuffer,
                .renderArea = {
                    .offset = {0, 0},
                    .extent = {command.width, command.height},
                },
                .clearValueCount = 1U,
                .pClearValues = &clear_value,
            };
            vkCmdBeginRenderPass(command_buffer, &render_pass_begin,
                                 VK_SUBPASS_CONTENTS_INLINE);
        } else if (index == 1U &&
                   record.opcode == BVB_COMMAND_BIND_GRAPHICS_PIPELINE) {
            struct bvb_bind_graphics_pipeline_command command;
            uint64_t pipeline_bits = 0U;
            result = bvb_command_decode_bind_graphics_pipeline(&record,
                                                                &command);
            if (result == 0) {
                result = bvb_handle_table_lookup(
                    &handles, command.pipeline_id, BVB_OBJECT_PIPELINE, NULL,
                    &pipeline_bits);
            }
            if (result != 0) {
                return result;
            }
            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipeline_from_bits(pipeline_bits));
        } else if (index == 2U && record.opcode == BVB_COMMAND_SET_VIEWPORT) {
            struct bvb_set_viewport_command command;
            result = bvb_command_decode_set_viewport(&record, &command);
            if (result != 0 || command.x != 0.0F || command.y != 0.0F ||
                command.width != (float)extent.width ||
                command.height != (float)extent.height ||
                command.minimum_depth != 0.0F ||
                command.maximum_depth != 1.0F) {
                return result != 0 ? result : -ENOTSUP;
            }
            const VkViewport viewport = {
                .x = command.x,
                .y = command.y,
                .width = command.width,
                .height = command.height,
                .minDepth = command.minimum_depth,
                .maxDepth = command.maximum_depth,
            };
            vkCmdSetViewport(command_buffer, 0U, 1U, &viewport);
        } else if (index == 3U && record.opcode == BVB_COMMAND_SET_SCISSOR) {
            struct bvb_set_scissor_command command;
            result = bvb_command_decode_set_scissor(&record, &command);
            if (result != 0 || command.x != 0 || command.y != 0 ||
                command.width != extent.width ||
                command.height != extent.height) {
                return result != 0 ? result : -ENOTSUP;
            }
            const VkRect2D scissor = {
                .offset = {command.x, command.y},
                .extent = {command.width, command.height},
            };
            vkCmdSetScissor(command_buffer, 0U, 1U, &scissor);
        } else if (index == 4U && record.opcode == BVB_COMMAND_DRAW) {
            struct bvb_draw_command command;
            result = bvb_command_decode_draw(&record, &command);
            if (result != 0 || command.vertex_count != 3U ||
                command.instance_count != 1U || command.first_vertex != 0U ||
                command.first_instance != 0U) {
                return result != 0 ? result : -ENOTSUP;
            }
            vkCmdDraw(command_buffer, command.vertex_count,
                      command.instance_count, command.first_vertex,
                      command.first_instance);
        } else if (index == 5U &&
                   record.opcode == BVB_COMMAND_END_RENDERING) {
            vkCmdEndRenderPass(command_buffer);
        } else {
            return -EPROTO;
        }
    }
    return bvb_command_batch_next(&iterator, &record) == 1 ? 0 : -EPROTO;
}

static void complete_external_batch(bool claimed, int status) {
    if (!claimed || visible_ingress == NULL) {
        return;
    }
    int result = bvb_visible_ingress_complete(visible_ingress, status);
    if (result != 0) {
        BVB_LOGE("E018_COMPLETE_FAIL status=%d result=%d", status, result);
    }
}

static bool create_renderer(ANativeWindow *window) {
    static const char *const instance_extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
    };
    const VkApplicationInfo application_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "bvb-visible-host",
        .applicationVersion = VK_MAKE_API_VERSION(0, 0, 18, 0),
        .pEngineName = "none",
        .apiVersion = VK_API_VERSION_1_1,
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
    if (!has_device_extension(physical_device,
                              VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        BVB_LOGE("E016_FAIL required_device_extension");
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
        (capabilities.supportedUsageFlags &
         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ==
            0U) {
        BVB_LOGE("E016_FAIL surface_capabilities=%d usage=%u", (int)result,
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
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };
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
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
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

    const VkAttachmentDescription render_attachment = {
        .format = surface_format.format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    const VkAttachmentReference render_attachment_reference = {
        .attachment = 0U,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    const VkSubpassDescription render_subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1U,
        .pColorAttachments = &render_attachment_reference,
    };
    const VkRenderPassCreateInfo render_pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1U,
        .pAttachments = &render_attachment,
        .subpassCount = 1U,
        .pSubpasses = &render_subpass,
    };
    result = vkCreateRenderPass(state.device, &render_pass_info, NULL,
                                &state.render_pass);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E016_FAIL render_pass=%d", (int)result);
        return false;
    }

    const VkShaderModuleCreateInfo vertex_shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(BVB_TRIANGLE_VERTEX_SPIRV),
        .pCode = BVB_TRIANGLE_VERTEX_SPIRV,
    };
    result = vkCreateShaderModule(state.device, &vertex_shader_info, NULL,
                                  &state.vertex_shader);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E016_FAIL vertex_shader=%d", (int)result);
        return false;
    }
    const VkShaderModuleCreateInfo fragment_shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(BVB_TRIANGLE_FRAGMENT_SPIRV),
        .pCode = BVB_TRIANGLE_FRAGMENT_SPIRV,
    };
    result = vkCreateShaderModule(state.device, &fragment_shader_info, NULL,
                                  &state.fragment_shader);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E016_FAIL fragment_shader=%d", (int)result);
        return false;
    }
    const VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    };
    result = vkCreatePipelineLayout(state.device, &pipeline_layout_info, NULL,
                                    &state.pipeline_layout);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E016_FAIL pipeline_layout=%d", (int)result);
        return false;
    }
    const VkPipelineShaderStageCreateInfo shader_stages[] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = state.vertex_shader,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = state.fragment_shader,
            .pName = "main",
        },
    };
    const VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    const VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    const VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1U,
        .scissorCount = 1U,
    };
    const VkPipelineRasterizationStateCreateInfo rasterization = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0F,
    };
    const VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    const VkPipelineColorBlendAttachmentState color_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo color_blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1U,
        .pAttachments = &color_attachment,
    };
    static const VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    const VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2U,
        .pDynamicStates = dynamic_states,
    };
    const VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2U,
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pColorBlendState = &color_blend,
        .pDynamicState = &dynamic_state,
        .layout = state.pipeline_layout,
        .renderPass = state.render_pass,
        .subpass = 0U,
    };
    result = vkCreateGraphicsPipelines(state.device, VK_NULL_HANDLE, 1U,
                                       &pipeline_info, NULL, &state.pipeline);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E016_FAIL graphics_pipeline=%d", (int)result);
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
    const VkImageViewCreateInfo image_view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = images[image_index],
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = surface_format.format,
        .components = {
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0U,
            .levelCount = 1U,
            .baseArrayLayer = 0U,
            .layerCount = 1U,
        },
    };
    result = vkCreateImageView(state.device, &image_view_info, NULL,
                               &state.image_view);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E016_FAIL image_view=%d", (int)result);
        return false;
    }
    const VkFramebufferCreateInfo framebuffer_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = state.render_pass,
        .attachmentCount = 1U,
        .pAttachments = &state.image_view,
        .width = extent.width,
        .height = extent.height,
        .layers = 1U,
    };
    result = vkCreateFramebuffer(state.device, &framebuffer_info, NULL,
                                 &state.framebuffer);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E016_FAIL framebuffer=%d", (int)result);
        return false;
    }
    uint8_t triangle_batch[512];
    size_t triangle_batch_length = 0U;
    int batch_status = build_triangle_batch(triangle_batch,
                                            sizeof(triangle_batch), extent,
                                            &triangle_batch_length);
    if (batch_status != 0) {
        BVB_LOGE("E016_FAIL batch_build=%d", batch_status);
        return false;
    }
    const uint8_t *render_batch = triangle_batch;
    size_t render_batch_length = triangle_batch_length;
    uint64_t external_sequence = 0U;
    bool external_claimed = false;
    if (visible_ingress != NULL) {
        batch_status = bvb_visible_ingress_wait_batch(
            visible_ingress, 10000U, &render_batch, &render_batch_length,
            &external_sequence);
        if (batch_status == 0) {
            external_claimed = true;
            if (atomic_load(&visible_brokered_ingress)) {
                BVB_LOGI("E022_BATCH_CLAIM sequence=%llu bytes=%zu",
                         (unsigned long long)external_sequence,
                         render_batch_length);
            } else if (visible_inline_ingress) {
                BVB_LOGI("E019_BATCH_CLAIM sequence=%llu bytes=%zu",
                         (unsigned long long)external_sequence,
                         render_batch_length);
            } else {
                BVB_LOGI("E018_BATCH_CLAIM sequence=%llu bytes=%zu",
                         (unsigned long long)external_sequence,
                         render_batch_length);
            }
        } else if (batch_status == -ETIMEDOUT) {
            render_batch = triangle_batch;
            render_batch_length = triangle_batch_length;
            BVB_LOGE("E018_BATCH_TIMEOUT fallback=local");
        } else {
            render_batch = triangle_batch;
            render_batch_length = triangle_batch_length;
            BVB_LOGE("E018_BATCH_WAIT_FAIL status=%d fallback=local",
                     batch_status);
        }
    }
    const VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    result = vkBeginCommandBuffer(command_buffer, &begin_info);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E008_FAIL command_begin=%d", (int)result);
        complete_external_batch(external_claimed, -EIO);
        return false;
    }
    const VkImageSubresourceRange range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    VkImageMemoryBarrier to_render = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = images[image_index],
        .subresourceRange = range,
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         NULL, 0, NULL, 1, &to_render);
    batch_status = replay_triangle_batch(
        render_batch, render_batch_length, command_buffer, state.image_view,
        state.pipeline, state.render_pass, state.framebuffer, extent);
    if (batch_status != 0) {
        BVB_LOGE("E016_FAIL batch_replay=%d", batch_status);
        complete_external_batch(external_claimed, batch_status);
        return false;
    }
    VkImageMemoryBarrier to_present = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = images[image_index],
        .subresourceRange = range,
    };
    vkCmdPipelineBarrier(command_buffer,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0,
                         NULL, 1, &to_present);
    result = vkEndCommandBuffer(command_buffer);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E008_FAIL command_end=%d", (int)result);
        complete_external_batch(external_claimed, -EIO);
        return false;
    }

    VkPipelineStageFlags wait_stage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
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
        complete_external_batch(external_claimed, -EIO);
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
        complete_external_batch(external_claimed, -EIO);
        return false;
    }
    result = vkQueueWaitIdle(state.queue);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E008_FAIL queue_idle=%d", (int)result);
        complete_external_batch(external_claimed, -EIO);
        return false;
    }
    complete_external_batch(external_claimed, 0);
    state.window = window;
    BVB_LOGI("E016_PASS batch_bytes=%zu commands=6 backend=render_pass "
             "source=%s",
             render_batch_length, external_claimed ? "glibc" : "local");
    if (external_claimed) {
        if (atomic_load(&visible_brokered_ingress)) {
            BVB_LOGI("E022_PASS sequence=%llu bytes=%zu",
                     (unsigned long long)external_sequence,
                     render_batch_length);
        } else if (visible_inline_ingress) {
            BVB_LOGI("E019_PASS sequence=%llu bytes=%zu",
                     (unsigned long long)external_sequence,
                     render_batch_length);
        } else {
            BVB_LOGI("E018_PASS sequence=%llu bytes=%zu",
                     (unsigned long long)external_sequence,
                     render_batch_length);
        }
    }
    BVB_LOGI("E008_PASS width=%u height=%u images=%u index=%u format=%d",
             extent.width, extent.height, image_count, image_index,
             (int)surface_format.format);
    return true;
}

static void *renderer_worker_main(void *unused) {
    (void)unused;
    uint64_t handled_generation = 0U;
    for (;;) {
        (void)pthread_mutex_lock(&renderer.mutex);
        while (renderer.requested_generation == handled_generation) {
            (void)pthread_cond_wait(&renderer.condition, &renderer.mutex);
        }
        const uint64_t generation = renderer.requested_generation;
        ANativeWindow *window = renderer.window;
        if (window != NULL) {
            ANativeWindow_acquire(window);
        }
        (void)pthread_mutex_unlock(&renderer.mutex);

        destroy_renderer();
        bool success = false;
        uint32_t width = 0U;
        uint32_t height = 0U;
        if (window != NULL) {
            width = (uint32_t)ANativeWindow_getWidth(window);
            height = (uint32_t)ANativeWindow_getHeight(window);
            success = create_renderer(window);
            if (!success) {
                destroy_renderer();
            }
        }

        (void)pthread_mutex_lock(&renderer.mutex);
        const bool current =
            generation == renderer.requested_generation &&
            window == renderer.window;
        renderer.completed_generation = generation;
        if (current) {
            renderer.ready = success;
            if (window != NULL) {
                emit_lifecycle(success ? BVB_LIFECYCLE_EVENT_RENDERER_READY
                                       : BVB_LIFECYCLE_EVENT_RENDERER_FAILED,
                               width, height);
            }
        }
        (void)pthread_mutex_unlock(&renderer.mutex);

        if (success && !current) {
            destroy_renderer();
        }
        if (window != NULL) {
            ANativeWindow_release(window);
        }
        handled_generation = generation;
    }
    return NULL;
}

static int schedule_renderer(ANativeWindow *window, bool force) {
    const uint32_t width =
        window == NULL ? 0U : (uint32_t)ANativeWindow_getWidth(window);
    const uint32_t height =
        window == NULL ? 0U : (uint32_t)ANativeWindow_getHeight(window);
    if (window != NULL) {
        ANativeWindow_acquire(window);
    }

    (void)pthread_mutex_lock(&renderer.mutex);
    if (!renderer.thread_started && window != NULL) {
        pthread_t worker;
        int result = pthread_create(&worker, NULL, renderer_worker_main, NULL);
        if (result != 0) {
            (void)pthread_mutex_unlock(&renderer.mutex);
            ANativeWindow_release(window);
            return -result;
        }
        renderer.thread_started = true;
        result = pthread_detach(worker);
        if (result != 0) {
            BVB_LOGE("E017_WORKER_DETACH_FAIL status=%d", result);
        }
    }
    if (!renderer.thread_started) {
        (void)pthread_mutex_unlock(&renderer.mutex);
        return 0;
    }
    const bool pending =
        renderer.requested_generation != renderer.completed_generation;
    if (!force && renderer.window == window && renderer.width == width &&
        renderer.height == height && (renderer.ready || pending)) {
        (void)pthread_mutex_unlock(&renderer.mutex);
        if (window != NULL) {
            ANativeWindow_release(window);
        }
        return 0;
    }
    if (renderer.requested_generation == UINT64_MAX) {
        (void)pthread_mutex_unlock(&renderer.mutex);
        if (window != NULL) {
            ANativeWindow_release(window);
        }
        return -EOVERFLOW;
    }
    ANativeWindow *old_window = renderer.window;
    renderer.window = window;
    renderer.width = width;
    renderer.height = height;
    renderer.ready = false;
    renderer.requested_generation += 1U;
    (void)pthread_cond_signal(&renderer.condition);
    (void)pthread_mutex_unlock(&renderer.mutex);
    if (old_window != NULL) {
        ANativeWindow_release(old_window);
    }
    return 0;
}

static void on_window_created(ANativeActivity *activity,
                              ANativeWindow *window) {
    (void)activity;
    BVB_LOGI("E008_WINDOW_CREATED width=%d height=%d",
             ANativeWindow_getWidth(window), ANativeWindow_getHeight(window));
    uint32_t width = (uint32_t)ANativeWindow_getWidth(window);
    uint32_t height = (uint32_t)ANativeWindow_getHeight(window);
    emit_lifecycle(BVB_LIFECYCLE_EVENT_WINDOW_CREATED, width, height);
    int result = schedule_renderer(window, true);
    if (result != 0) {
        BVB_LOGE("E017_WORKER_START_FAIL status=%d", result);
        emit_lifecycle(BVB_LIFECYCLE_EVENT_RENDERER_FAILED, width, height);
    }
}

static void on_window_resized(ANativeActivity *activity,
                              ANativeWindow *window) {
    (void)activity;
    int result = schedule_renderer(window, false);
    if (result != 0) {
        BVB_LOGE("E017_RESIZE_SCHEDULE_FAIL status=%d", result);
    }
}

static void on_window_redraw_needed(ANativeActivity *activity,
                                    ANativeWindow *window) {
    (void)activity;
    int result = schedule_renderer(window, false);
    if (result != 0) {
        BVB_LOGE("E017_REDRAW_SCHEDULE_FAIL status=%d", result);
    }
}

static void on_window_destroyed(ANativeActivity *activity,
                                ANativeWindow *window) {
    (void)activity;
    (void)window;
    BVB_LOGI("E008_WINDOW_DESTROYED");
    int result = schedule_renderer(NULL, true);
    if (result != 0) {
        BVB_LOGE("E017_DESTROY_SCHEDULE_FAIL status=%d", result);
    }
    emit_lifecycle(BVB_LIFECYCLE_EVENT_WINDOW_DESTROYED, 0, 0);
}

static void on_window_focus_changed(ANativeActivity *activity, int has_focus) {
    if (has_focus != 0) {
        apply_immersive_mode(activity);
        emit_lifecycle(BVB_LIFECYCLE_EVENT_FOCUS_GAINED, 0, 0);
    } else {
        emit_lifecycle(BVB_LIFECYCLE_EVENT_FOCUS_LOST, 0, 0);
    }
}

static void on_start(ANativeActivity *activity) {
    (void)activity;
    emit_lifecycle(BVB_LIFECYCLE_EVENT_STARTED, 0, 0);
}

static void on_resume(ANativeActivity *activity) {
    (void)activity;
    emit_lifecycle(BVB_LIFECYCLE_EVENT_RESUMED, 0, 0);
}

static void on_pause(ANativeActivity *activity) {
    (void)activity;
    emit_lifecycle(BVB_LIFECYCLE_EVENT_PAUSED, 0, 0);
}

static void on_stop(ANativeActivity *activity) {
    (void)activity;
    emit_lifecycle(BVB_LIFECYCLE_EVENT_STOPPED, 0, 0);
}

static void on_destroy(ANativeActivity *activity) {
    (void)activity;
    int result = schedule_renderer(NULL, true);
    if (result != 0) {
        BVB_LOGE("E017_ACTIVITY_DESTROY_SCHEDULE_FAIL status=%d", result);
    }
    emit_lifecycle(BVB_LIFECYCLE_EVENT_DESTROYED, 0, 0);
    disable_lifecycle();
}

__attribute__((visibility("default"))) void
ANativeActivity_onCreate(ANativeActivity *activity, void *saved_state,
                         size_t saved_state_size) {
    (void)saved_state;
    (void)saved_state_size;
    configure_lifecycle(activity);
    activity->callbacks->onStart = on_start;
    activity->callbacks->onResume = on_resume;
    activity->callbacks->onPause = on_pause;
    activity->callbacks->onStop = on_stop;
    activity->callbacks->onDestroy = on_destroy;
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
    emit_lifecycle(BVB_LIFECYCLE_EVENT_CREATED, 0, 0);
}
