#define _GNU_SOURCE
#define VK_USE_PLATFORM_ANDROID_KHR

#include <bvb/command_batch.h>
#include <bvb/activity_frame_transport.h>
#include <bvb/frame_sync.h>
#include <bvb/lifecycle.h>
#include <bvb/native_binder.h>
#include <bvb/protocol.h>
#include <bvb/triangle_batch_builder.h>
#include <bvb/visible_ingress.h>
#include <bvb/wsi_frame_ring.h>

#include <android/log.h>
#include <android/hardware_buffer.h>
#include <android/hardware_buffer_jni.h>
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
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/un.h>
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
    VkPhysicalDevice physical_device;
    VkQueue queue;
    uint32_t queue_family_index;
    VkSwapchainKHR swapchain;
    VkImage swapchain_images[BVB_MAX_IMAGES];
    uint32_t swapchain_image_count;
    VkFormat swapchain_format;
    VkExtent2D swapchain_extent;
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
    VkBuffer external_buffer;
    VkDeviceMemory external_memory;
    VkSemaphore external_sync_semaphore;
    VkCommandBuffer external_sync_command_buffer;
    VkDeviceSize external_allocation_size;
    uint32_t external_memory_type_index;
    VkMemoryPropertyFlags external_memory_property_flags;
    VkImage external_image;
    VkDeviceMemory external_image_memory;
    VkSemaphore external_image_semaphore;
    VkCommandBuffer external_image_command_buffer;
    VkDeviceSize external_image_allocation_size;
    uint32_t external_image_memory_type_index;
    PFN_vkGetMemoryFdKHR get_memory_fd;
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID
        get_hardware_buffer_properties;
    PFN_vkGetSemaphoreFdKHR get_semaphore_fd;
};

struct bvb_game_frame_transport {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool thread_started;
    bool installed;
    uint64_t generation;
    uint32_t image_count;
    uint32_t width;
    uint32_t height;
    VkFormat format;
    VkImage images[BVB_WSI_FRAME_RING_MAX_SLOTS];
    VkDeviceMemory memories[BVB_WSI_FRAME_RING_MAX_SLOTS];
    AHardwareBuffer *hardware_buffers[BVB_WSI_FRAME_RING_MAX_SLOTS];
    struct bvb_wsi_frame_ring *ring;
    VkCommandPool command_pool;
    VkCommandBuffer command_buffer;
    VkSemaphore acquire_semaphore;
    VkSemaphore copy_semaphore;
    VkFence copy_fence;
    uint32_t consumed_sequence;
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

struct bvb_external_sync_cache {
    int memory_fd;
    int semaphore_fd;
    uint64_t allocation_size;
    uint32_t memory_type_index;
    bool ready;
};

static struct bvb_visible_state state;
static struct bvb_lifecycle_client lifecycle;
static struct bvb_visible_ingress *visible_ingress;
static bool visible_inline_ingress;
static atomic_bool visible_brokered_ingress;
static atomic_bool retain_external_renderer;
static atomic_uint visible_frame_count = 1U;
static pthread_mutex_t lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t external_memory_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t renderer_device_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t external_broker_once = PTHREAD_ONCE_INIT;
static pthread_once_t native_binder_once = PTHREAD_ONCE_INIT;
static AIBinder *native_binder_service;
static int native_binder_registration_status = -EINPROGRESS;
static struct bvb_renderer_control renderer = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .condition = PTHREAD_COND_INITIALIZER,
};
static struct bvb_external_sync_cache external_sync_cache = {
    .memory_fd = -1,
    .semaphore_fd = -1,
};
static struct bvb_external_sync_cache external_image_cache = {
    .memory_fd = -1,
    .semaphore_fd = -1,
};
static struct bvb_game_frame_transport game_frames = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .condition = PTHREAD_COND_INITIALIZER,
};
static atomic_bool activity_resumed;
static atomic_bool activity_window_present;

enum {
    BVB_E020_REGION_BYTES = 4096,
    BVB_E022_REGION_GENERATION = 1,
    BVB_E023_MAX_FRAMES = 4096,
    BVB_E036_TOKEN_HEX_BYTES = BVB_LIFECYCLE_TOKEN_SIZE * 2,
    BVB_E036_REQUEST_BYTES = BVB_E036_TOKEN_HEX_BYTES + 1,
    BVB_E036_RESPONSE_BYTES = 24,
    BVB_E038_IMAGE_WIDTH = 64,
    BVB_E038_IMAGE_HEIGHT = 64,
    BVB_E042_FRAME_COUNT = 120,
    BVB_E042_FRAME_TIMEOUT_MS = 30000,
};

static const uint32_t BVB_E037_FILL_WORD = UINT32_C(0xe037c0de);
static const uint32_t BVB_E038_COLOR_WORD = UINT32_C(0xffff00ff);
static const uint32_t BVB_E042_GREEN_WORD = UINT32_C(0xff00ff00);

static const char BVB_E036_BROKER_SOCKET[] =
    "bvb-visible-external-memory";
static const char BVB_E039_DATAGRAM_BROKER_SOCKET[] =
    "bvb-visible-external-memory-dgram";
static const char BVB_E040_DIAGNOSTIC_PATH[] =
    "/data/user/0/io.github.huntergdavis.bvb.visiblehost/files/"
    "e040-native-binder.txt";

static void write_native_binder_diagnostic(const char *stage, int status,
                                           bool reset) {
    const int flags = O_WRONLY | O_CREAT | (reset ? O_TRUNC : O_APPEND);
    const int descriptor = open(BVB_E040_DIAGNOSTIC_PATH, flags, 0600);
    if (descriptor < 0) return;
    char line[160];
    const int length = snprintf(line, sizeof(line),
                                "pid=%ld stage=%s status=%d\n",
                                (long)getpid(), stage, status);
    if (length > 0 && (size_t)length < sizeof(line)) {
        (void)write(descriptor, line, (size_t)length);
    }
    (void)close(descriptor);
}

static bool token_matches(const uint8_t *left, const uint8_t *right) {
    uint8_t difference = 0U;
    for (size_t index = 0U; index < BVB_LIFECYCLE_TOKEN_SIZE; ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0U;
}

static void clear_external_sync_cache_locked(void) {
    if (external_sync_cache.memory_fd >= 0) {
        (void)close(external_sync_cache.memory_fd);
    }
    if (external_sync_cache.semaphore_fd >= 0) {
        (void)close(external_sync_cache.semaphore_fd);
    }
    external_sync_cache = (struct bvb_external_sync_cache){
        .memory_fd = -1,
        .semaphore_fd = -1,
    };
}

static void clear_external_image_cache_locked(void) {
    if (external_image_cache.memory_fd >= 0) {
        (void)close(external_image_cache.memory_fd);
    }
    if (external_image_cache.semaphore_fd >= 0) {
        (void)close(external_image_cache.semaphore_fd);
    }
    external_image_cache = (struct bvb_external_sync_cache){
        .memory_fd = -1,
        .semaphore_fd = -1,
    };
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

static int receive_broker_token_exact(int socket_fd, uint8_t *output,
                                      size_t length) {
    size_t offset = 0U;
    while (offset < length) {
        ssize_t count = recv(socket_fd, output + offset, length - offset, 0);
        if (count == 0) return -EPIPE;
        if (count < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        offset += (size_t)count;
    }
    return 0;
}

static int send_external_broker_response_to(
    int socket_fd, int status, const int *descriptors,
    size_t descriptor_count, uint64_t allocation_size,
    uint32_t memory_type_index, uint32_t metadata0, uint32_t metadata1,
    const struct sockaddr *destination, socklen_t destination_size) {
    uint8_t response[BVB_E036_RESPONSE_BYTES] = {0};
    bvb_wire_put_i32(response, status);
    bvb_wire_put_u64(response + 4, allocation_size);
    bvb_wire_put_u32(response + 12, memory_type_index);
    bvb_wire_put_u32(response + 16, metadata0);
    bvb_wire_put_u32(response + 20, metadata1);
    struct iovec vector = {
        .iov_base = response,
        .iov_len = sizeof(response),
    };
    uint8_t control[CMSG_SPACE(sizeof(int) * 2U)] = {0};
    struct msghdr message = {
        .msg_name = (void *)destination,
        .msg_namelen = destination_size,
        .msg_iov = &vector,
        .msg_iovlen = 1U,
    };
    if (status == 0 && descriptors != NULL && descriptor_count > 0U &&
        descriptor_count <= 2U) {
        message.msg_control = control;
        message.msg_controllen = CMSG_SPACE(sizeof(int) * descriptor_count);
        struct cmsghdr *rights = CMSG_FIRSTHDR(&message);
        rights->cmsg_level = SOL_SOCKET;
        rights->cmsg_type = SCM_RIGHTS;
        rights->cmsg_len = CMSG_LEN(sizeof(int) * descriptor_count);
        memcpy(CMSG_DATA(rights), descriptors,
               sizeof(int) * descriptor_count);
    }
    ssize_t sent;
    do {
        sent = sendmsg(socket_fd, &message, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0) return -errno;
    return sent == (ssize_t)sizeof(response) ? 0 : -EIO;
}

static int send_external_broker_response(
    int socket_fd, int status, const int *descriptors,
    size_t descriptor_count, uint64_t allocation_size,
    uint32_t memory_type_index, uint32_t metadata0, uint32_t metadata1) {
    return send_external_broker_response_to(
        socket_fd, status, descriptors, descriptor_count, allocation_size,
        memory_type_index, metadata0, metadata1, NULL, 0);
}

static void *native_binder_channel_main(void *argument) {
    const int channel = (int)(intptr_t)argument;
    uint8_t acknowledgement = 0U;
    int status = receive_broker_token_exact(
        channel, &acknowledgement, sizeof(acknowledgement));
    if (status == 0 && acknowledgement != UINT8_C(0xa5)) status = -EPROTO;
    uint8_t response[8] = {0};
    bvb_wire_put_i32(response, status);
    bvb_wire_put_u32(response + 4, UINT32_C(0xe040c0de));
    size_t offset = 0U;
    while (offset < sizeof(response)) {
        ssize_t sent = send(channel, response + offset,
                            sizeof(response) - offset, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) {
            status = sent == 0 ? -EPIPE : -errno;
            break;
        }
        offset += (size_t)sent;
    }
    (void)close(channel);
    if (status == 0) {
        BVB_LOGI("E040_NATIVE_BINDER_CHANNEL_PASS");
    } else {
        BVB_LOGE("E040_NATIVE_BINDER_CHANNEL_FAIL status=%d", status);
    }
    return NULL;
}

static void *native_binder_on_create(void *argument) {
    return argument;
}

static void native_binder_on_destroy(void *user_data) {
    (void)user_data;
}

static binder_status_t native_binder_on_transact(
    AIBinder *binder, transaction_code_t code, const AParcel *input,
    AParcel *output) {
    (void)binder;
    if (code != BVB_NATIVE_BINDER_TRANSACTION_OPEN) {
        return STATUS_UNKNOWN_TRANSACTION;
    }
    write_native_binder_diagnostic("transaction", 0, false);
    uint8_t parsed_token[BVB_LIFECYCLE_TOKEN_SIZE] = {0};
    binder_status_t parcel_status = STATUS_OK;
    for (size_t index = 0U;
         index < BVB_NATIVE_BINDER_TOKEN_WORDS && parcel_status == STATUS_OK;
         ++index) {
        int32_t word = 0;
        parcel_status = AParcel_readInt32(input, &word);
        const uint32_t value = (uint32_t)word;
        parsed_token[index * 4U] = (uint8_t)value;
        parsed_token[index * 4U + 1U] = (uint8_t)(value >> 8U);
        parsed_token[index * 4U + 2U] = (uint8_t)(value >> 16U);
        parsed_token[index * 4U + 3U] = (uint8_t)(value >> 24U);
    }
    if (parcel_status != STATUS_OK) return parcel_status;

    (void)pthread_mutex_lock(&lifecycle_mutex);
    const bool authorized = lifecycle.configured &&
                            token_matches(parsed_token, lifecycle.token);
    (void)pthread_mutex_unlock(&lifecycle_mutex);
    int status = authorized ? 0 : -EACCES;
    int descriptors[2] = {-1, -1};
    int channel_pair[2] = {-1, -1};
    uint64_t allocation_size = 0U;
    uint32_t memory_type_index = 0U;
    (void)pthread_mutex_lock(&external_memory_mutex);
    if (status == 0 &&
        (!external_image_cache.ready || external_image_cache.memory_fd < 0 ||
         external_image_cache.semaphore_fd < 0)) {
        status = -EAGAIN;
    }
    if (status == 0) {
        descriptors[0] = fcntl(external_image_cache.memory_fd,
                               F_DUPFD_CLOEXEC, 0);
        descriptors[1] = fcntl(external_image_cache.semaphore_fd,
                               F_DUPFD_CLOEXEC, 0);
        if (descriptors[0] < 0 || descriptors[1] < 0) {
            status = -errno;
        } else {
            allocation_size = external_image_cache.allocation_size;
            memory_type_index = external_image_cache.memory_type_index;
        }
    }
    (void)pthread_mutex_unlock(&external_memory_mutex);
    if (status == 0 &&
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, channel_pair) != 0) {
        status = -errno;
    }
    pthread_t channel_thread;
    if (status == 0) {
        const int thread_status = pthread_create(
            &channel_thread, NULL, native_binder_channel_main,
            (void *)(intptr_t)channel_pair[0]);
        if (thread_status != 0) {
            status = -thread_status;
        } else {
            channel_pair[0] = -1;
            const int detach_status = pthread_detach(channel_thread);
            if (detach_status != 0) {
                BVB_LOGE("E040_NATIVE_BINDER_DETACH_FAIL status=%d",
                         detach_status);
            }
        }
    }

    parcel_status = AParcel_writeInt32(output, status);
    if (status == 0 && parcel_status == STATUS_OK) {
        parcel_status = AParcel_writeInt64(output, (int64_t)allocation_size);
    }
    if (status == 0 && parcel_status == STATUS_OK) {
        parcel_status = AParcel_writeInt32(output,
                                           (int32_t)memory_type_index);
    }
    if (status == 0 && parcel_status == STATUS_OK) {
        parcel_status = AParcel_writeInt32(output, BVB_E038_IMAGE_WIDTH);
    }
    if (status == 0 && parcel_status == STATUS_OK) {
        parcel_status = AParcel_writeInt32(output, BVB_E038_IMAGE_HEIGHT);
    }
    if (status == 0 && parcel_status == STATUS_OK) {
        parcel_status = AParcel_writeInt32(output,
                                           (int32_t)BVB_E038_COLOR_WORD);
    }
    if (status == 0 && parcel_status == STATUS_OK) {
        parcel_status = AParcel_writeParcelFileDescriptor(output,
                                                           descriptors[0]);
    }
    if (status == 0 && parcel_status == STATUS_OK) {
        parcel_status = AParcel_writeParcelFileDescriptor(output,
                                                           descriptors[1]);
    }
    if (status == 0 && parcel_status == STATUS_OK) {
        parcel_status = AParcel_writeParcelFileDescriptor(output,
                                                           channel_pair[1]);
    }
    for (size_t index = 0U; index < 2U; ++index) {
        if (descriptors[index] >= 0) (void)close(descriptors[index]);
    }
    for (size_t index = 0U; index < 2U; ++index) {
        if (channel_pair[index] >= 0) (void)close(channel_pair[index]);
    }
    if (status == 0 && parcel_status == STATUS_OK) {
        BVB_LOGI("E040_NATIVE_BINDER_EXPORT_PASS allocation=%llu type=%u "
                 "descriptors=3",
                 (unsigned long long)allocation_size, memory_type_index);
    } else {
        BVB_LOGE("E040_NATIVE_BINDER_EXPORT_FAIL status=%d parcel=%d",
                 status, parcel_status);
    }
    return parcel_status;
}

static void start_native_binder_service(void) {
    write_native_binder_diagnostic("start", 0, true);
    AIBinder_Class *binder_class = AIBinder_Class_define(
        BVB_NATIVE_BINDER_DESCRIPTOR, native_binder_on_create,
        native_binder_on_destroy, native_binder_on_transact);
    if (binder_class == NULL) {
        native_binder_registration_status = -EIO;
        write_native_binder_diagnostic("class", -EIO, false);
        BVB_LOGE("E040_NATIVE_BINDER_SERVICE_FAIL class=null");
        return;
    }
    AIBinder *binder = AIBinder_new(binder_class, NULL);
    if (binder == NULL) {
        native_binder_registration_status = -EIO;
        write_native_binder_diagnostic("binder", -EIO, false);
        BVB_LOGE("E040_NATIVE_BINDER_SERVICE_FAIL binder=null");
        return;
    }
    const binder_status_t status = AServiceManager_addService(
        binder, BVB_NATIVE_BINDER_INSTANCE);
    native_binder_registration_status = status;
    write_native_binder_diagnostic("add_service", status, false);
    if (status != STATUS_OK) {
        BVB_LOGE("E040_NATIVE_BINDER_SERVICE_FAIL add=%d", status);
        AIBinder_decStrong(binder);
        return;
    }
    ABinderProcess_startThreadPool();
    native_binder_service = binder;
    write_native_binder_diagnostic("ready", 0, false);
    BVB_LOGI("E040_NATIVE_BINDER_SERVICE_READY instance=%s",
             BVB_NATIVE_BINDER_INSTANCE);
}

/* external_memory_mutex must be held. */
static int prepare_external_sync(int *semaphore_fd) {
    if (semaphore_fd == NULL || state.device == VK_NULL_HANDLE ||
        state.queue == VK_NULL_HANDLE || state.command_pool == VK_NULL_HANDLE ||
        state.external_buffer == VK_NULL_HANDLE ||
        state.get_semaphore_fd == NULL) {
        return -EAGAIN;
    }
    *semaphore_fd = -1;
    if (state.external_sync_semaphore != VK_NULL_HANDLE) {
        return -EALREADY;
    }
    const VkExportSemaphoreCreateInfo export_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    const VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &export_info,
    };
    VkSemaphore semaphore = VK_NULL_HANDLE;
    VkResult result = vkCreateSemaphore(
        state.device, &semaphore_info, NULL, &semaphore);
    if (result != VK_SUCCESS) return -EIO;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    bool submitted = false;
    (void)pthread_mutex_lock(&queue_mutex);
    const VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = state.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1U,
    };
    result = vkAllocateCommandBuffers(
        state.device, &command_info, &command_buffer);
    const VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (result == VK_SUCCESS) {
        result = vkBeginCommandBuffer(command_buffer, &begin_info);
    }
    if (result == VK_SUCCESS) {
        vkCmdFillBuffer(command_buffer, state.external_buffer, 0U,
                        BVB_E020_REGION_BYTES, BVB_E037_FILL_WORD);
        const VkBufferMemoryBarrier release_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = state.external_buffer,
            .offset = 0U,
            .size = BVB_E020_REGION_BYTES,
        };
        vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0U, 0U, NULL,
                             1U, &release_barrier, 0U, NULL);
        result = vkEndCommandBuffer(command_buffer);
    }
    const VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1U,
        .pCommandBuffers = &command_buffer,
        .signalSemaphoreCount = 1U,
        .pSignalSemaphores = &semaphore,
    };
    if (result == VK_SUCCESS) {
        result = vkQueueSubmit(state.queue, 1U, &submit_info, VK_NULL_HANDLE);
        submitted = result == VK_SUCCESS;
    }
    if (result == VK_SUCCESS) {
        const VkSemaphoreGetFdInfoKHR get_fd_info = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
            .semaphore = semaphore,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
        };
        result = state.get_semaphore_fd(
            state.device, &get_fd_info, semaphore_fd);
    }
    if ((result != VK_SUCCESS || *semaphore_fd < 0) && submitted) {
        (void)vkQueueWaitIdle(state.queue);
    }
    if ((result != VK_SUCCESS || *semaphore_fd < 0) &&
        command_buffer != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(state.device, state.command_pool, 1U,
                             &command_buffer);
        command_buffer = VK_NULL_HANDLE;
    }
    (void)pthread_mutex_unlock(&queue_mutex);
    if (result != VK_SUCCESS || *semaphore_fd < 0) {
        if (*semaphore_fd >= 0) (void)close(*semaphore_fd);
        *semaphore_fd = -1;
        vkDestroySemaphore(state.device, semaphore, NULL);
        return -EIO;
    }
    state.external_sync_semaphore = semaphore;
    state.external_sync_command_buffer = command_buffer;
    return 0;
}

/* external_memory_mutex must be held. */
static int cache_external_sync(void) {
    clear_external_sync_cache_locked();
    int semaphore_fd = -1;
    int status = prepare_external_sync(&semaphore_fd);
    int memory_fd = -1;
    if (status == 0) {
        const VkMemoryGetFdInfoKHR memory_fd_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
            .memory = state.external_memory,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
        };
        const VkResult result = state.get_memory_fd(
            state.device, &memory_fd_info, &memory_fd);
        if (result != VK_SUCCESS || memory_fd < 0) status = -EIO;
    }
    if (status != 0) {
        if (memory_fd >= 0) (void)close(memory_fd);
        if (semaphore_fd >= 0) (void)close(semaphore_fd);
        return status;
    }
    external_sync_cache = (struct bvb_external_sync_cache){
        .memory_fd = memory_fd,
        .semaphore_fd = semaphore_fd,
        .allocation_size = state.external_allocation_size,
        .memory_type_index = state.external_memory_type_index,
        .ready = true,
    };
    BVB_LOGI("E037_CACHE_READY bytes=%u allocation=%llu type=%u fill=%u",
             BVB_E020_REGION_BYTES,
             (unsigned long long)external_sync_cache.allocation_size,
             external_sync_cache.memory_type_index, BVB_E037_FILL_WORD);
    return 0;
}

/* external_memory_mutex must be held. */
static int prepare_external_image_sync(int *semaphore_fd) {
    if (semaphore_fd == NULL || state.device == VK_NULL_HANDLE ||
        state.queue == VK_NULL_HANDLE || state.command_pool == VK_NULL_HANDLE ||
        state.external_image == VK_NULL_HANDLE ||
        state.get_semaphore_fd == NULL) {
        return -EAGAIN;
    }
    *semaphore_fd = -1;
    if (state.external_image_semaphore != VK_NULL_HANDLE) {
        return -EALREADY;
    }
    const VkExportSemaphoreCreateInfo export_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    const VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &export_info,
    };
    VkSemaphore semaphore = VK_NULL_HANDLE;
    VkResult result = vkCreateSemaphore(
        state.device, &semaphore_info, NULL, &semaphore);
    if (result != VK_SUCCESS) return -EIO;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    bool submitted = false;
    (void)pthread_mutex_lock(&queue_mutex);
    const VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = state.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1U,
    };
    result = vkAllocateCommandBuffers(
        state.device, &command_info, &command_buffer);
    const VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (result == VK_SUCCESS) {
        result = vkBeginCommandBuffer(command_buffer, &begin_info);
    }
    const VkImageSubresourceRange color_range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0U,
        .levelCount = 1U,
        .baseArrayLayer = 0U,
        .layerCount = 1U,
    };
    if (result == VK_SUCCESS) {
        const VkImageMemoryBarrier to_general = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0U,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = state.external_image,
            .subresourceRange = color_range,
        };
        vkCmdPipelineBarrier(
            command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, NULL, 0U, NULL, 1U,
            &to_general);
        const VkClearColorValue color = {
            .float32 = {1.0F, 0.0F, 1.0F, 1.0F},
        };
        vkCmdClearColorImage(command_buffer, state.external_image,
                             VK_IMAGE_LAYOUT_GENERAL, &color, 1U,
                             &color_range);
        const VkImageMemoryBarrier release_barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = state.external_image,
            .subresourceRange = color_range,
        };
        vkCmdPipelineBarrier(
            command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0U, 0U, NULL, 0U, NULL, 1U,
            &release_barrier);
        result = vkEndCommandBuffer(command_buffer);
    }
    const VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1U,
        .pCommandBuffers = &command_buffer,
        .signalSemaphoreCount = 1U,
        .pSignalSemaphores = &semaphore,
    };
    if (result == VK_SUCCESS) {
        result = vkQueueSubmit(state.queue, 1U, &submit_info, VK_NULL_HANDLE);
        submitted = result == VK_SUCCESS;
    }
    if (result == VK_SUCCESS) {
        const VkSemaphoreGetFdInfoKHR get_fd_info = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
            .semaphore = semaphore,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
        };
        result = state.get_semaphore_fd(
            state.device, &get_fd_info, semaphore_fd);
    }
    if ((result != VK_SUCCESS || *semaphore_fd < 0) && submitted) {
        (void)vkQueueWaitIdle(state.queue);
    }
    if ((result != VK_SUCCESS || *semaphore_fd < 0) &&
        command_buffer != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(state.device, state.command_pool, 1U,
                             &command_buffer);
        command_buffer = VK_NULL_HANDLE;
    }
    (void)pthread_mutex_unlock(&queue_mutex);
    if (result != VK_SUCCESS || *semaphore_fd < 0) {
        if (*semaphore_fd >= 0) (void)close(*semaphore_fd);
        *semaphore_fd = -1;
        vkDestroySemaphore(state.device, semaphore, NULL);
        return -EIO;
    }
    state.external_image_semaphore = semaphore;
    state.external_image_command_buffer = command_buffer;
    return 0;
}

/* external_memory_mutex must be held. */
static int cache_external_image_sync(void) {
    clear_external_image_cache_locked();
    int semaphore_fd = -1;
    int status = prepare_external_image_sync(&semaphore_fd);
    int memory_fd = -1;
    if (status == 0) {
        const VkMemoryGetFdInfoKHR memory_fd_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
            .memory = state.external_image_memory,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
        };
        const VkResult result = state.get_memory_fd(
            state.device, &memory_fd_info, &memory_fd);
        if (result != VK_SUCCESS || memory_fd < 0) status = -EIO;
    }
    if (status != 0) {
        if (memory_fd >= 0) (void)close(memory_fd);
        if (semaphore_fd >= 0) (void)close(semaphore_fd);
        return status;
    }
    external_image_cache = (struct bvb_external_sync_cache){
        .memory_fd = memory_fd,
        .semaphore_fd = semaphore_fd,
        .allocation_size = state.external_image_allocation_size,
        .memory_type_index = state.external_image_memory_type_index,
        .ready = true,
    };
    BVB_LOGI("E038_CACHE_READY width=%u height=%u format=%u color=%u "
             "allocation=%llu type=%u",
             BVB_E038_IMAGE_WIDTH, BVB_E038_IMAGE_HEIGHT,
             (unsigned int)VK_FORMAT_R8G8B8A8_UNORM, BVB_E038_COLOR_WORD,
             (unsigned long long)external_image_cache.allocation_size,
             external_image_cache.memory_type_index);
    return 0;
}

struct bvb_e042_control_region {
    int descriptor;
    struct bvb_frame_sync_control *control;
};

static void close_e042_control_region(
    struct bvb_e042_control_region *region) {
    if (region->control != MAP_FAILED && region->control != NULL) {
        (void)munmap(region->control, BVB_FRAME_SYNC_REGION_BYTES);
    }
    if (region->descriptor >= 0) (void)close(region->descriptor);
    *region = (struct bvb_e042_control_region){
        .descriptor = -1,
        .control = MAP_FAILED,
    };
}

static int create_e042_control_region(
    struct bvb_e042_control_region *region) {
    *region = (struct bvb_e042_control_region){
        .descriptor = -1,
        .control = MAP_FAILED,
    };
    const int descriptor = (int)syscall(
        SYS_memfd_create, "bvb-e042-frame-sync",
        MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (descriptor < 0) return -errno;
    region->descriptor = descriptor;
    if (ftruncate(descriptor, BVB_FRAME_SYNC_REGION_BYTES) != 0) {
        const int status = -errno;
        close_e042_control_region(region);
        return status;
    }
    region->control = mmap(NULL, BVB_FRAME_SYNC_REGION_BYTES,
                           PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0U);
    if (region->control == MAP_FAILED) {
        const int status = -errno;
        close_e042_control_region(region);
        return status;
    }
    const int status = bvb_frame_sync_initialize(
        region->control, BVB_FRAME_SYNC_REGION_BYTES, BVB_E042_FRAME_COUNT);
    if (status != 0) close_e042_control_region(region);
    return status;
}

static uint32_t e042_frame_color(uint32_t sequence) {
    return (sequence & 1U) != 0U ? BVB_E038_COLOR_WORD
                                 : BVB_E042_GREEN_WORD;
}

static uint64_t e042_monotonic_nanoseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0U;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static int produce_e042_external_image_frame(uint32_t sequence) {
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    int status = 0;
    (void)pthread_mutex_lock(&queue_mutex);
    if (state.device == VK_NULL_HANDLE || state.queue == VK_NULL_HANDLE ||
        state.command_pool == VK_NULL_HANDLE ||
        state.external_image == VK_NULL_HANDLE) {
        status = -EPIPE;
        goto done;
    }
    const VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = state.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1U,
    };
    VkResult result = vkAllocateCommandBuffers(
        state.device, &command_info, &command_buffer);
    const VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (result == VK_SUCCESS) {
        result = vkBeginCommandBuffer(command_buffer, &begin_info);
    }
    const VkImageSubresourceRange color_range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .levelCount = 1U,
        .layerCount = 1U,
    };
    if (result == VK_SUCCESS) {
        const VkImageMemoryBarrier acquire = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = state.external_image,
            .subresourceRange = color_range,
        };
        vkCmdPipelineBarrier(
            command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, NULL, 0U, NULL, 1U,
            &acquire);
        const VkClearColorValue color = (sequence & 1U) != 0U
            ? (VkClearColorValue){.float32 = {1.0F, 0.0F, 1.0F, 1.0F}}
            : (VkClearColorValue){.float32 = {0.0F, 1.0F, 0.0F, 1.0F}};
        vkCmdClearColorImage(command_buffer, state.external_image,
                             VK_IMAGE_LAYOUT_GENERAL, &color, 1U,
                             &color_range);
        const VkImageMemoryBarrier release = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = state.external_image,
            .subresourceRange = color_range,
        };
        vkCmdPipelineBarrier(
            command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0U, 0U, NULL, 0U, NULL, 1U,
            &release);
        result = vkEndCommandBuffer(command_buffer);
    }
    const VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    if (result == VK_SUCCESS) {
        result = vkCreateFence(state.device, &fence_info, NULL, &fence);
    }
    const VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1U,
        .pCommandBuffers = &command_buffer,
    };
    if (result == VK_SUCCESS) {
        result = vkQueueSubmit(state.queue, 1U, &submit_info, fence);
    }
    if (result == VK_SUCCESS) {
        result = vkWaitForFences(state.device, 1U, &fence, VK_TRUE,
                                 UINT64_MAX);
    }
    if (result != VK_SUCCESS) status = -EIO;

done:
    if (fence != VK_NULL_HANDLE) {
        vkDestroyFence(state.device, fence, NULL);
    }
    if (command_buffer != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(state.device, state.command_pool, 1U,
                             &command_buffer);
    }
    (void)pthread_mutex_unlock(&queue_mutex);
    if (status != 0) {
        BVB_LOGE("E042_PRODUCER_GPU_FAIL sequence=%u status=%d color=%u",
                 sequence, status, e042_frame_color(sequence));
    }
    return status;
}

static int run_e042_producer(struct bvb_frame_sync_control *control) {
    int status = bvb_frame_sync_validate(control);
    const uint64_t started_ns = e042_monotonic_nanoseconds();
    for (uint32_t sequence = 1U; status == 0 &&
         sequence <= BVB_E042_FRAME_COUNT; ++sequence) {
        if (sequence > 1U) {
            uint32_t consumed = 0U;
            status = bvb_frame_sync_wait_consumer(
                control, sequence - 2U, BVB_E042_FRAME_TIMEOUT_MS,
                &consumed);
            if (status == 0 && consumed != sequence - 1U) status = -EPROTO;
        }
        if (status == 0) {
            status = produce_e042_external_image_frame(sequence);
        }
        if (status == 0) {
            status = bvb_frame_sync_publish_producer(control, sequence);
        }
    }
    if (status == 0) {
        uint32_t consumed = 0U;
        status = bvb_frame_sync_wait_consumer(
            control, BVB_E042_FRAME_COUNT - 1U,
            BVB_E042_FRAME_TIMEOUT_MS, &consumed);
        if (status == 0 && consumed != BVB_E042_FRAME_COUNT) {
            status = -EPROTO;
        }
    }
    if (status != 0) (void)bvb_frame_sync_fail_producer(control, status);
    const uint64_t finished_ns = e042_monotonic_nanoseconds();
    if (status == 0) {
        BVB_LOGI("E042_PRODUCER_PASS frames=%u elapsed_ns=%llu",
                 BVB_E042_FRAME_COUNT,
                 (unsigned long long)(finished_ns >= started_ns
                                          ? finished_ns - started_ns
                                          : 0U));
    } else {
        BVB_LOGE("E042_PRODUCER_FAIL status=%d", status);
    }
    return status;
}

static void handle_external_broker_connection(int connection) {
    int status = 0;
    struct bvb_e042_control_region frame_control = {
        .descriptor = -1,
        .control = MAP_FAILED,
    };
    struct ucred credentials;
    socklen_t credentials_size = sizeof(credentials);
    if (getsockopt(connection, SOL_SOCKET, SO_PEERCRED, &credentials,
                   &credentials_size) != 0 ||
        credentials_size != sizeof(credentials)) {
        status = -EACCES;
    }
    uint8_t request[BVB_E036_REQUEST_BYTES] = {0};
    if (status == 0) {
        status = receive_broker_token_exact(connection, request,
                                            sizeof(request));
    }
    const bool direct_channel =
        request[BVB_E036_TOKEN_HEX_BYTES] == 'P';
    const bool fenced_image =
        request[BVB_E036_TOKEN_HEX_BYTES] == 'F';
    const bool frame_ring =
        request[BVB_E036_TOKEN_HEX_BYTES] == 'R';
    const bool external_image = direct_channel || fenced_image || frame_ring ||
        request[BVB_E036_TOKEN_HEX_BYTES] == 'I';
    const bool synchronized = !fenced_image && !frame_ring &&
        (external_image || request[BVB_E036_TOKEN_HEX_BYTES] == 'S');
    if (status == 0 && !synchronized && !fenced_image && !frame_ring &&
        request[BVB_E036_TOKEN_HEX_BYTES] != 'M') {
        status = -EPROTO;
    }
    if (status == 0 && credentials.uid != getuid() && !direct_channel) {
        status = -EACCES;
    }
    uint8_t parsed_token[BVB_LIFECYCLE_TOKEN_SIZE] = {0};
    if (status == 0) {
        request[BVB_E036_TOKEN_HEX_BYTES] = '\0';
        status = bvb_lifecycle_token_from_hex((const char *)request,
                                              parsed_token);
    }
    if (status == 0) {
        (void)pthread_mutex_lock(&lifecycle_mutex);
        const bool authorized = lifecycle.configured &&
                                token_matches(parsed_token, lifecycle.token);
        (void)pthread_mutex_unlock(&lifecycle_mutex);
        if (!authorized) status = -EACCES;
    }
    int descriptors[2] = {-1, -1};
    size_t descriptor_count = 0U;
    uint64_t allocation_size = 0U;
    uint32_t memory_type_index = 0U;
    (void)pthread_mutex_lock(&external_memory_mutex);
    if (status == 0 && (fenced_image || frame_ring)) {
        if (!external_image_cache.ready || state.queue == VK_NULL_HANDLE) {
            status = -EAGAIN;
        } else {
            (void)pthread_mutex_lock(&queue_mutex);
            const VkResult wait_result = vkQueueWaitIdle(state.queue);
            (void)pthread_mutex_unlock(&queue_mutex);
            if (wait_result != VK_SUCCESS) status = -EIO;
        }
    }
    if (status == 0 && (synchronized || fenced_image || frame_ring)) {
        const struct bvb_external_sync_cache *cache = external_image
                                                          ? &external_image_cache
                                                          : &external_sync_cache;
        if (!cache->ready || cache->memory_fd < 0 ||
            (synchronized && cache->semaphore_fd < 0)) {
            status = -EAGAIN;
        } else {
            if (frame_ring) status = create_e042_control_region(&frame_control);
            if (status == 0) {
                descriptors[0] = fcntl(
                    cache->memory_fd, F_DUPFD_CLOEXEC, 0);
            }
            if (status == 0 && synchronized) {
                descriptors[1] = fcntl(
                    cache->semaphore_fd, F_DUPFD_CLOEXEC, 0);
            } else if (frame_ring && status == 0) {
                descriptors[1] = fcntl(
                    frame_control.descriptor, F_DUPFD_CLOEXEC, 0);
            }
            if (status == 0 &&
                (descriptors[0] < 0 ||
                 ((synchronized || frame_ring) && descriptors[1] < 0))) {
                status = -errno;
            } else if (status == 0) {
                allocation_size = cache->allocation_size;
                memory_type_index = cache->memory_type_index;
                descriptor_count = synchronized || frame_ring ? 2U : 1U;
            }
        }
    } else if (status == 0 && !fenced_image && !frame_ring &&
        (state.device == VK_NULL_HANDLE ||
         state.external_memory == VK_NULL_HANDLE ||
         state.get_memory_fd == NULL)) {
        status = -EAGAIN;
    }
    if (status == 0 && !synchronized && !fenced_image && !frame_ring) {
        const VkMemoryGetFdInfoKHR fd_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
            .memory = state.external_memory,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
        };
        const VkResult result = state.get_memory_fd(
            state.device, &fd_info, &descriptors[0]);
        if (result != VK_SUCCESS || descriptors[0] < 0) {
            status = -EIO;
            descriptors[0] = -1;
            BVB_LOGE("E036_EXPORT_FAIL vkGetMemoryFdKHR=%d", (int)result);
        } else {
            allocation_size = state.external_allocation_size;
            memory_type_index = state.external_memory_type_index;
            descriptor_count = 1U;
        }
    }
    (void)pthread_mutex_unlock(&external_memory_mutex);

    const int send_status = send_external_broker_response(
        connection, status, descriptors, descriptor_count, allocation_size,
        memory_type_index,
        external_image ? BVB_E038_IMAGE_WIDTH : BVB_E020_REGION_BYTES,
        external_image ? BVB_E038_IMAGE_HEIGHT
                       : synchronized ? BVB_E037_FILL_WORD : 0U);
    for (size_t index = 0U; index < 2U; ++index) {
        if (descriptors[index] >= 0) (void)close(descriptors[index]);
    }
    if (status == 0 && send_status == 0) {
        if (external_image) {
            BVB_LOGI("%s width=%u height=%u format=%u color=%u "
                     "allocation=%llu type=%u descriptors=%u "
                     "synchronization=%s peer_uid=%u",
                     direct_channel
                         ? "E039_DIRECT_EXPORT_PASS"
                         : frame_ring ? "E042_FRAME_RING_EXPORT_PASS"
                         : fenced_image ? "E041_FENCED_IMAGE_EXPORT_PASS"
                                        : "E038_EXPORT_PASS",
                     BVB_E038_IMAGE_WIDTH, BVB_E038_IMAGE_HEIGHT,
                     (unsigned int)VK_FORMAT_R8G8B8A8_UNORM,
                     BVB_E038_COLOR_WORD,
                     (unsigned long long)allocation_size, memory_type_index,
                     fenced_image ? 1U : 2U,
                     frame_ring ? "native_atomic_futex_control"
                         : fenced_image ? "producer_queue_idle" : "sync_fd",
                     (unsigned int)credentials.uid);
        } else if (synchronized) {
            BVB_LOGI("E037_EXPORT_PASS bytes=%u allocation=%llu type=%u "
                     "fill=%u descriptors=2 semaphore=sync_fd",
                     BVB_E020_REGION_BYTES,
                     (unsigned long long)allocation_size, memory_type_index,
                     BVB_E037_FILL_WORD);
        } else {
            BVB_LOGI("E036_EXPORT_PASS bytes=%u allocation=%llu type=%u",
                     BVB_E020_REGION_BYTES,
                     (unsigned long long)allocation_size, memory_type_index);
        }
    } else {
        BVB_LOGE("E036_BROKER_REQUEST_FAIL status=%d send=%d", status,
                 send_status);
    }
    if (frame_ring && status == 0 && send_status == 0) {
        (void)pthread_mutex_lock(&external_memory_mutex);
        (void)run_e042_producer(frame_control.control);
        (void)pthread_mutex_unlock(&external_memory_mutex);
    }
    close_e042_control_region(&frame_control);
    if (direct_channel && status == 0 && send_status == 0) {
        uint8_t acknowledgement = 0U;
        int channel_status = receive_broker_token_exact(
            connection, &acknowledgement, sizeof(acknowledgement));
        const uint8_t expected_ack = UINT8_C(0xa5);
        if (channel_status == 0 && acknowledgement != expected_ack) {
            channel_status = -EPROTO;
        }
        uint8_t response[8] = {0};
        bvb_wire_put_i32(response, channel_status);
        bvb_wire_put_u32(response + 4, UINT32_C(0xe039c0de));
        if (send(connection, response, sizeof(response), MSG_NOSIGNAL) !=
            (ssize_t)sizeof(response)) {
            channel_status = -EIO;
        }
        if (channel_status == 0) {
            BVB_LOGI("E039_DIRECT_CHANNEL_PASS peer_uid=%u",
                     (unsigned int)credentials.uid);
        } else {
            BVB_LOGE("E039_DIRECT_CHANNEL_FAIL status=%d", channel_status);
        }
    }
}

static ssize_t receive_external_datagram(
    int socket_fd, uint8_t *bytes, size_t capacity,
    struct sockaddr_un *source, socklen_t *source_size,
    struct ucred *credentials) {
    struct iovec vector = {
        .iov_base = bytes,
        .iov_len = capacity,
    };
    uint8_t control[CMSG_SPACE(sizeof(struct ucred))] = {0};
    struct msghdr message = {
        .msg_name = source,
        .msg_namelen = sizeof(*source),
        .msg_iov = &vector,
        .msg_iovlen = 1U,
        .msg_control = control,
        .msg_controllen = sizeof(control),
    };
    ssize_t count;
    do {
        count = recvmsg(socket_fd, &message, 0);
    } while (count < 0 && errno == EINTR);
    if (count < 0) return -errno;
    if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) return -EPROTO;
    *source_size = message.msg_namelen;
    credentials->pid = -1;
    credentials->uid = (uid_t)-1;
    credentials->gid = (gid_t)-1;
    for (struct cmsghdr *item = CMSG_FIRSTHDR(&message); item != NULL;
         item = CMSG_NXTHDR(&message, item)) {
        if (item->cmsg_level == SOL_SOCKET &&
            item->cmsg_type == SCM_CREDENTIALS &&
            item->cmsg_len == CMSG_LEN(sizeof(*credentials))) {
            memcpy(credentials, CMSG_DATA(item), sizeof(*credentials));
        }
    }
    return count;
}

static bool same_datagram_peer(const struct sockaddr_un *left,
                               socklen_t left_size,
                               const struct sockaddr_un *right,
                               socklen_t right_size) {
    return left_size == right_size &&
           left_size >= offsetof(struct sockaddr_un, sun_path) + 1U &&
           memcmp(left, right, left_size) == 0;
}

static void handle_external_datagram_request(int socket_fd) {
    uint8_t request[BVB_E036_REQUEST_BYTES] = {0};
    struct sockaddr_un peer_address;
    socklen_t peer_address_size = 0;
    struct ucred credentials;
    ssize_t count = receive_external_datagram(
        socket_fd, request, sizeof(request), &peer_address,
        &peer_address_size, &credentials);
    int status = count < 0 ? (int)count : 0;
    if (status == 0 && count != (ssize_t)sizeof(request)) status = -EPROTO;
    if (status == 0 && request[BVB_E036_TOKEN_HEX_BYTES] != 'D') {
        status = -EPROTO;
    }
    uint8_t parsed_token[BVB_LIFECYCLE_TOKEN_SIZE] = {0};
    if (status == 0) {
        request[BVB_E036_TOKEN_HEX_BYTES] = '\0';
        status = bvb_lifecycle_token_from_hex((const char *)request,
                                              parsed_token);
    }
    if (status == 0) {
        (void)pthread_mutex_lock(&lifecycle_mutex);
        const bool authorized = lifecycle.configured &&
                                token_matches(parsed_token, lifecycle.token);
        (void)pthread_mutex_unlock(&lifecycle_mutex);
        if (!authorized) status = -EACCES;
    }

    int descriptors[2] = {-1, -1};
    uint64_t allocation_size = 0U;
    uint32_t memory_type_index = 0U;
    (void)pthread_mutex_lock(&external_memory_mutex);
    if (status == 0 &&
        (!external_image_cache.ready || external_image_cache.memory_fd < 0 ||
         external_image_cache.semaphore_fd < 0)) {
        status = -EAGAIN;
    }
    if (status == 0) {
        descriptors[0] = fcntl(external_image_cache.memory_fd,
                               F_DUPFD_CLOEXEC, 0);
        descriptors[1] = fcntl(external_image_cache.semaphore_fd,
                               F_DUPFD_CLOEXEC, 0);
        if (descriptors[0] < 0 || descriptors[1] < 0) {
            status = -errno;
        } else {
            allocation_size = external_image_cache.allocation_size;
            memory_type_index = external_image_cache.memory_type_index;
        }
    }
    (void)pthread_mutex_unlock(&external_memory_mutex);

    int send_status = 0;
    if (peer_address_size > 0U) {
        send_status = send_external_broker_response_to(
            socket_fd, status, descriptors, status == 0 ? 2U : 0U,
            allocation_size, memory_type_index, BVB_E038_IMAGE_WIDTH,
            BVB_E038_IMAGE_HEIGHT,
            (const struct sockaddr *)&peer_address, peer_address_size);
    } else {
        send_status = -EDESTADDRREQ;
    }
    for (size_t index = 0U; index < 2U; ++index) {
        if (descriptors[index] >= 0) (void)close(descriptors[index]);
    }
    if (status != 0 || send_status != 0) {
        BVB_LOGE("E039_DATAGRAM_REQUEST_FAIL status=%d send=%d peer_uid=%u",
                 status, send_status, (unsigned int)credentials.uid);
        return;
    }
    BVB_LOGI("E039_DATAGRAM_EXPORT_PASS width=%u height=%u allocation=%llu "
             "type=%u descriptors=2 peer_uid=%u",
             BVB_E038_IMAGE_WIDTH, BVB_E038_IMAGE_HEIGHT,
             (unsigned long long)allocation_size, memory_type_index,
             (unsigned int)credentials.uid);

    uint8_t acknowledgement = 0U;
    struct sockaddr_un acknowledgement_address;
    socklen_t acknowledgement_address_size = 0;
    struct ucred acknowledgement_credentials;
    count = receive_external_datagram(
        socket_fd, &acknowledgement, sizeof(acknowledgement),
        &acknowledgement_address, &acknowledgement_address_size,
        &acknowledgement_credentials);
    int channel_status = count < 0 ? (int)count : 0;
    if (channel_status == 0 &&
        (count != 1 || acknowledgement != UINT8_C(0xa5) ||
         (credentials.pid >= 0 && acknowledgement_credentials.pid >= 0 &&
          (acknowledgement_credentials.uid != credentials.uid ||
           acknowledgement_credentials.pid != credentials.pid)) ||
         !same_datagram_peer(&peer_address, peer_address_size,
                             &acknowledgement_address,
                             acknowledgement_address_size))) {
        channel_status = -EPROTO;
    }
    uint8_t response[8] = {0};
    bvb_wire_put_i32(response, channel_status);
    bvb_wire_put_u32(response + 4, UINT32_C(0xe039c0de));
    ssize_t sent;
    do {
        sent = sendto(socket_fd, response, sizeof(response), MSG_NOSIGNAL,
                      (const struct sockaddr *)&peer_address,
                      peer_address_size);
    } while (sent < 0 && errno == EINTR);
    if (sent != (ssize_t)sizeof(response)) channel_status = -EIO;
    if (channel_status == 0) {
        BVB_LOGI("E039_DATAGRAM_CHANNEL_PASS peer_uid=%u",
                 (unsigned int)credentials.uid);
    } else {
        BVB_LOGE("E039_DATAGRAM_CHANNEL_FAIL status=%d", channel_status);
    }
}

static void *external_datagram_broker_main(void *unused) {
    const int socket_fd = (int)(intptr_t)unused;
    BVB_LOGI("E039_DATAGRAM_BROKER_READY socket=%s",
             BVB_E039_DATAGRAM_BROKER_SOCKET);
    for (;;) handle_external_datagram_request(socket_fd);
    return NULL;
}

static void *external_broker_main(void *unused) {
    const int listener = (int)(intptr_t)unused;
    BVB_LOGI("E036_BROKER_READY socket=%s", BVB_E036_BROKER_SOCKET);
    for (;;) {
        int connection = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
        if (connection < 0) {
            if (errno == EINTR) continue;
            BVB_LOGE("E036_BROKER_FAIL accept=%d", errno);
            break;
        }
        handle_external_broker_connection(connection);
        (void)close(connection);
    }
    (void)close(listener);
    return NULL;
}

static int create_external_broker_listener(void) {
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) return -errno;
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    const size_t name_length = strlen(BVB_E036_BROKER_SOCKET);
    memcpy(address.sun_path + 1, BVB_E036_BROKER_SOCKET, name_length);
    const socklen_t address_size =
        (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1U + name_length);
    if (bind(listener, (const struct sockaddr *)&address, address_size) != 0 ||
        listen(listener, 4) != 0) {
        const int result = -errno;
        (void)close(listener);
        return result;
    }
    return listener;
}

static int create_external_datagram_listener(void) {
    int socket_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (socket_fd < 0) return -errno;
    int enabled = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_PASSCRED, &enabled,
                   sizeof(enabled)) != 0) {
        const int result = -errno;
        (void)close(socket_fd);
        return result;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    const size_t name_length = strlen(BVB_E039_DATAGRAM_BROKER_SOCKET);
    memcpy(address.sun_path + 1, BVB_E039_DATAGRAM_BROKER_SOCKET,
           name_length);
    const socklen_t address_size =
        (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1U +
                    name_length);
    if (bind(socket_fd, (const struct sockaddr *)&address,
             address_size) != 0) {
        const int result = -errno;
        (void)close(socket_fd);
        return result;
    }
    return socket_fd;
}

static void start_external_broker(void) {
    const int listener = create_external_broker_listener();
    if (listener < 0) {
        BVB_LOGE("E036_BROKER_FAIL bind_or_listen=%d", -listener);
        return;
    }
    pthread_t thread;
    int result = pthread_create(&thread, NULL, external_broker_main,
                                (void *)(intptr_t)listener);
    if (result != 0) {
        (void)close(listener);
        BVB_LOGE("E036_BROKER_FAIL thread=%d", result);
        return;
    }
    result = pthread_detach(thread);
    if (result != 0) {
        BVB_LOGE("E036_BROKER_FAIL detach=%d", result);
    }

    const int datagram = create_external_datagram_listener();
    if (datagram < 0) {
        BVB_LOGE("E039_DATAGRAM_BROKER_FAIL bind=%d", -datagram);
        return;
    }
    result = pthread_create(&thread, NULL, external_datagram_broker_main,
                            (void *)(intptr_t)datagram);
    if (result != 0) {
        (void)close(datagram);
        BVB_LOGE("E039_DATAGRAM_BROKER_FAIL thread=%d", result);
        return;
    }
    result = pthread_detach(thread);
    if (result != 0) {
        BVB_LOGE("E039_DATAGRAM_BROKER_FAIL detach=%d", result);
    }
}

static void configure_lifecycle(ANativeActivity *activity) {
    (void)pthread_mutex_lock(&external_memory_mutex);
    clear_external_sync_cache_locked();
    clear_external_image_cache_locked();
    (void)pthread_mutex_unlock(&external_memory_mutex);
    (void)pthread_mutex_lock(&lifecycle_mutex);
    if (visible_ingress != NULL) {
        struct bvb_visible_ingress *stale_ingress = visible_ingress;
        visible_ingress = NULL;
        visible_inline_ingress = false;
        atomic_store(&visible_brokered_ingress, false);
        bvb_visible_ingress_destroy(stale_ingress);
        BVB_LOGI("E022_INGRESS_RESET");
    }
    memset(&lifecycle, 0, sizeof(lifecycle));
    atomic_store(&retain_external_renderer, false);
    atomic_store(&visible_frame_count, 1U);
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
    jstring visible_frames_key =
        (*env)->NewStringUTF(env, "bvb_visible_frames");
    jstring retain_external_renderer_key =
        (*env)->NewStringUTF(env, "bvb_retain_external_renderer");
    jint port = get_int_extra == NULL || port_key == NULL
                    ? 0
                    : (*env)->CallIntMethod(env, intent, get_int_extra, port_key,
                                            0);
    jint visible_port = get_int_extra == NULL || visible_port_key == NULL
                            ? 0
                            : (*env)->CallIntMethod(env, intent, get_int_extra,
                                                    visible_port_key, 0);
    jint visible_frames =
        get_int_extra == NULL || visible_frames_key == NULL
            ? 1
            : (*env)->CallIntMethod(env, intent, get_int_extra,
                                    visible_frames_key, 1);
    jint retain_renderer =
        get_int_extra == NULL || retain_external_renderer_key == NULL
            ? 0
            : (*env)->CallIntMethod(env, intent, get_int_extra,
                                    retain_external_renderer_key, 0);
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
    if (visible_frames > 0 && visible_frames <= BVB_E023_MAX_FRAMES) {
        atomic_store(&visible_frame_count, (unsigned int)visible_frames);
    } else {
        BVB_LOGE("E023_CONFIG_INVALID frames=%d fallback=1",
                 (int)visible_frames);
    }
    BVB_LOGI("E023_CONFIG frames=%u", atomic_load(&visible_frame_count));
    atomic_store(&retain_external_renderer, retain_renderer == 1);
    BVB_LOGI("E041_RETAIN_EXTERNAL_RENDERER enabled=%s",
             atomic_load(&retain_external_renderer) ? "true" : "false");
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
    if (visible_frames_key != NULL) {
        (*env)->DeleteLocalRef(env, visible_frames_key);
    }
    if (retain_external_renderer_key != NULL) {
        (*env)->DeleteLocalRef(env, retain_external_renderer_key);
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

static void close_frame_setup_descriptors(int *descriptors,
                                          uint32_t descriptor_count) {
    if (descriptors == NULL) return;
    for (uint32_t index = 0U; index < descriptor_count; ++index) {
        if (descriptors[index] >= 0) {
            (void)close(descriptors[index]);
            descriptors[index] = -1;
        }
    }
}

/* renderer_device_mutex and game_frames.mutex must both be held. */
static void clear_game_frame_transport_locked(void) {
    game_frames.installed = false;
    (void)pthread_cond_broadcast(&game_frames.condition);
    if (game_frames.ring != NULL) {
        (void)bvb_wsi_frame_ring_fail_consumer(game_frames.ring, -EPIPE);
    }
    if (state.device != VK_NULL_HANDLE) {
        if (game_frames.copy_fence != VK_NULL_HANDLE) {
            vkDestroyFence(state.device, game_frames.copy_fence, NULL);
        }
        if (game_frames.copy_semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(state.device, game_frames.copy_semaphore, NULL);
        }
        if (game_frames.acquire_semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(state.device, game_frames.acquire_semaphore,
                               NULL);
        }
        if (game_frames.command_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(state.device, game_frames.command_pool, NULL);
        }
        for (uint32_t index = 0U;
             index < BVB_WSI_FRAME_RING_MAX_SLOTS; ++index) {
            if (game_frames.images[index] != VK_NULL_HANDLE) {
                vkDestroyImage(state.device, game_frames.images[index], NULL);
            }
            if (game_frames.memories[index] != VK_NULL_HANDLE) {
                vkFreeMemory(state.device, game_frames.memories[index], NULL);
            }
            if (game_frames.hardware_buffers[index] != NULL) {
                AHardwareBuffer_release(game_frames.hardware_buffers[index]);
            }
        }
    }
    if (game_frames.ring != NULL) {
        (void)munmap(game_frames.ring, BVB_WSI_FRAME_RING_REGION_BYTES);
    }
    game_frames.generation = 0U;
    game_frames.image_count = 0U;
    game_frames.width = 0U;
    game_frames.height = 0U;
    game_frames.format = VK_FORMAT_UNDEFINED;
    memset(game_frames.images, 0, sizeof(game_frames.images));
    memset(game_frames.memories, 0, sizeof(game_frames.memories));
    memset(game_frames.hardware_buffers, 0,
           sizeof(game_frames.hardware_buffers));
    game_frames.ring = NULL;
    game_frames.command_pool = VK_NULL_HANDLE;
    game_frames.command_buffer = VK_NULL_HANDLE;
    game_frames.acquire_semaphore = VK_NULL_HANDLE;
    game_frames.copy_semaphore = VK_NULL_HANDLE;
    game_frames.copy_fence = VK_NULL_HANDLE;
    game_frames.consumed_sequence = 0U;
}

static bool game_frame_transport_installed(void) {
    (void)pthread_mutex_lock(&game_frames.mutex);
    const bool installed = game_frames.ring != NULL;
    (void)pthread_mutex_unlock(&game_frames.mutex);
    return installed;
}

static int copy_game_frame_to_window_locked(uint32_t slot) {
    if (slot >= game_frames.image_count || state.device == VK_NULL_HANDLE ||
        state.swapchain == VK_NULL_HANDLE || state.queue == VK_NULL_HANDLE) {
        return -ENODEV;
    }
    VkResult result = vkWaitForFences(state.device, 1U,
                                      &game_frames.copy_fence, VK_TRUE,
                                      UINT64_MAX);
    if (result == VK_SUCCESS) {
        result = vkResetFences(state.device, 1U, &game_frames.copy_fence);
    }
    if (result == VK_SUCCESS) {
        result = vkResetCommandPool(state.device, game_frames.command_pool, 0U);
    }
    uint32_t destination_index = 0U;
    if (result == VK_SUCCESS) {
        result = vkAcquireNextImageKHR(
            state.device, state.swapchain, UINT64_MAX,
            game_frames.acquire_semaphore, VK_NULL_HANDLE,
            &destination_index);
    }
    if ((result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) ||
        destination_index >= state.swapchain_image_count) {
        return -EIO;
    }
    const VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    result = vkBeginCommandBuffer(game_frames.command_buffer, &begin_info);
    if (result != VK_SUCCESS) return -EIO;
    const VkImageSubresourceRange range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0U,
        .levelCount = 1U,
        .baseArrayLayer = 0U,
        .layerCount = 1U,
    };
    const VkImageMemoryBarrier before_copy[] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
            .dstQueueFamilyIndex = state.queue_family_index,
            .image = game_frames.images[slot],
            .subresourceRange = range,
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = state.swapchain_images[destination_index],
            .subresourceRange = range,
        },
    };
    vkCmdPipelineBarrier(game_frames.command_buffer,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, NULL, 0U,
                         NULL, 2U, before_copy);
    if (game_frames.format == state.swapchain_format) {
        const VkImageCopy copy = {
            .srcSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .layerCount = 1U,
            },
            .dstSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .layerCount = 1U,
            },
            .extent = {game_frames.width, game_frames.height, 1U},
        };
        vkCmdCopyImage(game_frames.command_buffer, game_frames.images[slot],
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       state.swapchain_images[destination_index],
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &copy);
    } else {
        const VkImageBlit blit = {
            .srcSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .layerCount = 1U,
            },
            .srcOffsets = {{0, 0, 0},
                           {(int32_t)game_frames.width,
                            (int32_t)game_frames.height, 1}},
            .dstSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .layerCount = 1U,
            },
            .dstOffsets = {{0, 0, 0},
                           {(int32_t)state.swapchain_extent.width,
                            (int32_t)state.swapchain_extent.height, 1}},
        };
        vkCmdBlitImage(game_frames.command_buffer, game_frames.images[slot],
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       state.swapchain_images[destination_index],
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &blit,
                       VK_FILTER_NEAREST);
    }
    const VkImageMemoryBarrier after_copy[] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = state.queue_family_index,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
            .image = game_frames.images[slot],
            .subresourceRange = range,
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = state.swapchain_images[destination_index],
            .subresourceRange = range,
        },
    };
    vkCmdPipelineBarrier(game_frames.command_buffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0U, 0U, NULL,
                         0U, NULL, 2U, after_copy);
    result = vkEndCommandBuffer(game_frames.command_buffer);
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1U,
        .pWaitSemaphores = &game_frames.acquire_semaphore,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1U,
        .pCommandBuffers = &game_frames.command_buffer,
        .signalSemaphoreCount = 1U,
        .pSignalSemaphores = &game_frames.copy_semaphore,
    };
    if (result == VK_SUCCESS) {
        result = vkQueueSubmit(state.queue, 1U, &submit,
                               game_frames.copy_fence);
    }
    const VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1U,
        .pWaitSemaphores = &game_frames.copy_semaphore,
        .swapchainCount = 1U,
        .pSwapchains = &state.swapchain,
        .pImageIndices = &destination_index,
    };
    if (result == VK_SUCCESS) result = vkQueuePresentKHR(state.queue, &present);
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        result = vkWaitForFences(state.device, 1U, &game_frames.copy_fence,
                                 VK_TRUE, UINT64_MAX);
    }
    return result == VK_SUCCESS ? 0 : -EIO;
}

static void *game_frame_consumer_main(void *unused) {
    (void)unused;
    for (;;) {
        (void)pthread_mutex_lock(&game_frames.mutex);
        while (!game_frames.installed || !atomic_load(&activity_resumed) ||
               !atomic_load(&activity_window_present)) {
            (void)pthread_cond_wait(&game_frames.condition,
                                    &game_frames.mutex);
        }
        (void)pthread_mutex_unlock(&game_frames.mutex);

        (void)pthread_mutex_lock(&renderer_device_mutex);
        (void)pthread_mutex_lock(&game_frames.mutex);
        const bool ready = game_frames.installed &&
                           atomic_load(&activity_resumed) &&
                           atomic_load(&activity_window_present);
        struct bvb_wsi_frame_ring *ring = game_frames.ring;
        const uint32_t after_sequence = game_frames.consumed_sequence;
        (void)pthread_mutex_unlock(&game_frames.mutex);
        int status = ready ? 0 : -EAGAIN;
        uint32_t slot = UINT32_MAX;
        uint32_t sequence = 0U;
        if (status == 0) {
            status = bvb_wsi_frame_ring_wait_present(
                ring, after_sequence, 100U, &slot, &sequence);
        }
        if (status == 0) {
            if (!atomic_load(&activity_resumed) ||
                !atomic_load(&activity_window_present)) {
                status = -EAGAIN;
            }
        }
        if (status == 0) {
            (void)pthread_mutex_lock(&queue_mutex);
            status = copy_game_frame_to_window_locked(slot);
            (void)pthread_mutex_unlock(&queue_mutex);
        }
        if (status == 0) {
            status = bvb_wsi_frame_ring_release(ring, slot, sequence);
        }
        if (status == 0) {
            (void)pthread_mutex_lock(&game_frames.mutex);
            game_frames.consumed_sequence = sequence;
            (void)pthread_mutex_unlock(&game_frames.mutex);
            BVB_LOGI("E057_FRAME_PRESENTED generation=%llu sequence=%u slot=%u",
                     (unsigned long long)ring->generation, sequence, slot);
        } else if (status == -EPIPE && after_sequence != 0U) {
            BVB_LOGI("E057_FRAME_CONSUMER_STOP status=%d sequence=%u",
                     status, after_sequence);
            (void)pthread_mutex_lock(&game_frames.mutex);
            game_frames.installed = false;
            (void)pthread_mutex_unlock(&game_frames.mutex);
        } else if (status != -ETIMEDOUT && status != -EAGAIN) {
            (void)bvb_wsi_frame_ring_fail_consumer(ring, status);
            BVB_LOGE("E057_FRAME_CONSUMER_FAIL status=%d", status);
            (void)pthread_mutex_lock(&game_frames.mutex);
            game_frames.installed = false;
            (void)pthread_mutex_unlock(&game_frames.mutex);
        }
        (void)pthread_mutex_unlock(&renderer_device_mutex);
    }
    return NULL;
}

static int start_game_frame_consumer_locked(void) {
    if (game_frames.thread_started) return 0;
    pthread_t thread;
    int status = pthread_create(&thread, NULL, game_frame_consumer_main, NULL);
    if (status != 0) return -status;
    game_frames.thread_started = true;
    status = pthread_detach(thread);
    if (status != 0) {
        BVB_LOGE("E057_CONSUMER_DETACH_FAIL status=%d", status);
    }
    return 0;
}

static int import_game_frame_transport(
    const struct bvb_activity_frame_setup *setup, int *descriptors,
    AHardwareBuffer *const *hardware_buffers) {
    const uint32_t descriptor_count = setup->image_count + 1U;
    int status = 0;
    (void)pthread_mutex_lock(&renderer_device_mutex);
    (void)pthread_mutex_lock(&renderer.mutex);
    const uint32_t renderer_width = renderer.width;
    const uint32_t renderer_height = renderer.height;
    const bool renderer_retained = atomic_load(&retain_external_renderer);
    const bool renderer_ready = renderer.ready &&
                                renderer_width == setup->width &&
                                renderer_height == setup->height &&
                                renderer_retained;
    (void)pthread_mutex_unlock(&renderer.mutex);
    if (!renderer_ready || state.device == VK_NULL_HANDLE ||
        state.physical_device == VK_NULL_HANDLE ||
        state.swapchain == VK_NULL_HANDLE) {
        BVB_LOGE("E088_IMPORT_FAIL stage=renderer_ready ready=%u "
                 "renderer_width=%u renderer_height=%u setup_width=%u "
                 "setup_height=%u retained=%u device=%u physical=%u "
                 "swapchain=%u",
                 renderer_ready ? 1U : 0U, renderer_width, renderer_height,
                 setup->width, setup->height,
                 renderer_retained ? 1U : 0U,
                 state.device != VK_NULL_HANDLE ? 1U : 0U,
                 state.physical_device != VK_NULL_HANDLE ? 1U : 0U,
                 state.swapchain != VK_NULL_HANDLE ? 1U : 0U);
        status = -EAGAIN;
        goto done;
    }
    (void)pthread_mutex_lock(&game_frames.mutex);
    if (game_frames.ring != NULL) {
        status = -EALREADY;
        (void)pthread_mutex_unlock(&game_frames.mutex);
        goto done;
    }
    (void)pthread_mutex_unlock(&game_frames.mutex);
    if ((VkFormat)setup->format != state.swapchain_format) {
        VkFormatProperties source_properties = {0};
        VkFormatProperties destination_properties = {0};
        vkGetPhysicalDeviceFormatProperties(state.physical_device,
                                            (VkFormat)setup->format,
                                            &source_properties);
        vkGetPhysicalDeviceFormatProperties(state.physical_device,
                                            state.swapchain_format,
                                            &destination_properties);
        if ((source_properties.optimalTilingFeatures &
             VK_FORMAT_FEATURE_BLIT_SRC_BIT) == 0U ||
            (destination_properties.optimalTilingFeatures &
             VK_FORMAT_FEATURE_BLIT_DST_BIT) == 0U) {
            BVB_LOGE("E088_IMPORT_FAIL stage=format_blit source_format=%d "
                     "destination_format=%d source_features=0x%llx "
                     "destination_features=0x%llx",
                     (int)setup->format, (int)state.swapchain_format,
                     (unsigned long long)
                         source_properties.optimalTilingFeatures,
                     (unsigned long long)
                         destination_properties.optimalTilingFeatures);
            status = -ENOTSUP;
            goto done;
        }
    }
    struct bvb_game_frame_transport imported = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .condition = PTHREAD_COND_INITIALIZER,
        .generation = setup->generation,
        .image_count = setup->image_count,
        .width = setup->width,
        .height = setup->height,
        .format = (VkFormat)setup->format,
    };
    imported.ring = mmap(NULL, BVB_WSI_FRAME_RING_REGION_BYTES,
                         PROT_READ | PROT_WRITE, MAP_SHARED,
                         descriptors[setup->image_count], 0U);
    if (imported.ring == MAP_FAILED) {
        imported.ring = NULL;
        status = -errno;
        goto import_failed;
    }
    if (bvb_wsi_frame_ring_validate(imported.ring, setup->generation) != 0 ||
        imported.ring->slot_count != setup->image_count) {
        status = -EPROTO;
        goto import_failed;
    }
    (void)close(descriptors[setup->image_count]);
    descriptors[setup->image_count] = -1;
    const VkExternalMemoryHandleTypeFlagBits memory_handle_type =
        (setup->flags & BVB_ACTIVITY_FRAME_FLAG_AHARDWAREBUFFER) != 0U
            ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID
        : (setup->flags & BVB_ACTIVITY_FRAME_FLAG_DMA_BUF) != 0U
            ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
            : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    const VkExternalMemoryImageCreateInfo external_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = memory_handle_type,
    };
    const VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_info,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = (VkFormat)setup->format,
        .extent = {setup->width, setup->height, 1U},
        .mipLevels = 1U,
        .arrayLayers = 1U,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = (VkImageUsageFlags)setup->image_usage |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkPhysicalDeviceMemoryProperties consumer_memory_properties = {0};
    vkGetPhysicalDeviceMemoryProperties(state.physical_device,
                                        &consumer_memory_properties);
    state.get_hardware_buffer_properties =
        (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkGetDeviceProcAddr(
            state.device,
            "vkGetAndroidHardwareBufferPropertiesANDROID");
    for (uint32_t index = 0U; index < setup->image_count; ++index) {
        VkResult result = vkCreateImage(state.device, &image_info, NULL,
                                        &imported.images[index]);
        VkMemoryRequirements requirements = {0};
        if (result == VK_SUCCESS) {
            vkGetImageMemoryRequirements(state.device, imported.images[index],
                                         &requirements);
            if (requirements.size > setup->allocation_sizes[index]) {
                BVB_LOGE("E088_IMPORT_FAIL stage=allocation_size image=%u "
                         "required=%llu exported=%llu alignment=%llu "
                         "image_type_bits=0x%x",
                         index, (unsigned long long)requirements.size,
                         (unsigned long long)setup->allocation_sizes[index],
                         (unsigned long long)requirements.alignment,
                         requirements.memoryTypeBits);
                status = -EPROTO;
                goto import_failed;
            }
        }
        uint32_t compatible_types = requirements.memoryTypeBits;
        VkAndroidHardwareBufferPropertiesANDROID hardware_properties = {
            .sType =
                VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
        };
        VkMemoryFdPropertiesKHR fd_properties = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
        };
        if (result == VK_SUCCESS &&
            memory_handle_type ==
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID) {
            if (hardware_buffers == NULL || hardware_buffers[index] == NULL ||
                state.get_hardware_buffer_properties == NULL) {
                result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
            } else {
                result = state.get_hardware_buffer_properties(
                    state.device, hardware_buffers[index],
                    &hardware_properties);
                compatible_types &= hardware_properties.memoryTypeBits;
            }
        } else if (result == VK_SUCCESS &&
            memory_handle_type ==
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) {
            result = state.get_memory_fd_properties(
                state.device, memory_handle_type, descriptors[index],
                &fd_properties);
            compatible_types &= fd_properties.memoryTypeBits;
        }
        uint32_t consumer_memory_type = setup->memory_type_indices[index];
        bool consumer_type_valid =
            result == VK_SUCCESS &&
            consumer_memory_type < consumer_memory_properties.memoryTypeCount &&
            (compatible_types & (UINT32_C(1) << consumer_memory_type)) != 0U;
        if (!consumer_type_valid &&
            (memory_handle_type ==
                 VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT ||
             memory_handle_type ==
                 VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID)) {
            for (uint32_t candidate = 0U;
                 candidate < consumer_memory_properties.memoryTypeCount;
                 ++candidate) {
                if ((compatible_types & (UINT32_C(1) << candidate)) != 0U) {
                    consumer_memory_type = candidate;
                    consumer_type_valid = true;
                    break;
                }
            }
        }
        if (!consumer_type_valid) {
            BVB_LOGE("E090_IMPORT_FAIL stage=memory_type image=%u "
                     "vk_result=%d handle_type=0x%x image_type_bits=0x%x "
                     "fd_type_bits=0x%x compatible_type_bits=0x%x "
                     "producer_type=%u consumer_type_count=%u required=%llu "
                     "exported=%llu",
                     index, (int)result, (unsigned)memory_handle_type,
                     requirements.memoryTypeBits, fd_properties.memoryTypeBits,
                     compatible_types,
                     setup->memory_type_indices[index],
                     consumer_memory_properties.memoryTypeCount,
                     (unsigned long long)requirements.size,
                     (unsigned long long)setup->allocation_sizes[index]);
            status = -ENOTSUP;
            goto import_failed;
        }
        const VkMemoryDedicatedAllocateInfo dedicated = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .image = imported.images[index],
        };
        const VkImportAndroidHardwareBufferInfoANDROID hardware_import = {
            .sType =
                VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
            .pNext = &dedicated,
            .buffer = hardware_buffers == NULL ? NULL : hardware_buffers[index],
        };
        const VkImportMemoryFdInfoKHR import_info = {
            .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
            .pNext = &dedicated,
            .handleType = memory_handle_type,
            .fd = descriptors[index],
        };
        const VkMemoryAllocateInfo allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = memory_handle_type ==
                    VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID
                ? (const void *)&hardware_import
                : (const void *)&import_info,
            .allocationSize = memory_handle_type ==
                    VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID
                ? hardware_properties.allocationSize
                : setup->allocation_sizes[index],
            .memoryTypeIndex = consumer_memory_type,
        };
        if (result == VK_SUCCESS) {
            result = vkAllocateMemory(state.device, &allocation, NULL,
                                      &imported.memories[index]);
            if (result == VK_SUCCESS &&
                memory_handle_type !=
                    VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID)
                descriptors[index] = -1;
            if (result != VK_SUCCESS) {
                BVB_LOGE("E088_IMPORT_FAIL stage=allocate image=%u "
                         "vk_result=%d allocation=%llu consumer_type=%u "
                         "compatible_type_bits=0x%x",
                         index, (int)result,
                         (unsigned long long)allocation.allocationSize,
                         consumer_memory_type, compatible_types);
            }
        }
        if (result == VK_SUCCESS) {
            result = vkBindImageMemory(state.device, imported.images[index],
                                       imported.memories[index], 0U);
            if (result != VK_SUCCESS) {
                BVB_LOGE("E088_IMPORT_FAIL stage=bind image=%u vk_result=%d "
                         "consumer_type=%u",
                         index, (int)result, consumer_memory_type);
            }
        }
        if (result != VK_SUCCESS) {
            status = -EIO;
            goto import_failed;
        }
        if (memory_handle_type ==
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID) {
            AHardwareBuffer_acquire(hardware_buffers[index]);
            imported.hardware_buffers[index] = hardware_buffers[index];
        }
    }
    const VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
                 VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = state.queue_family_index,
    };
    VkResult result = vkCreateCommandPool(state.device, &pool_info, NULL,
                                          &imported.command_pool);
    const VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = imported.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1U,
    };
    if (result == VK_SUCCESS) {
        result = vkAllocateCommandBuffers(state.device, &command_info,
                                          &imported.command_buffer);
    }
    const VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    if (result == VK_SUCCESS) {
        result = vkCreateSemaphore(state.device, &semaphore_info, NULL,
                                   &imported.acquire_semaphore);
    }
    if (result == VK_SUCCESS) {
        result = vkCreateSemaphore(state.device, &semaphore_info, NULL,
                                   &imported.copy_semaphore);
    }
    const VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    if (result == VK_SUCCESS) {
        result = vkCreateFence(state.device, &fence_info, NULL,
                               &imported.copy_fence);
    }
    if (result != VK_SUCCESS) {
        status = -EIO;
        goto import_failed;
    }
    (void)pthread_mutex_lock(&game_frames.mutex);
    game_frames.generation = imported.generation;
    game_frames.image_count = imported.image_count;
    game_frames.width = imported.width;
    game_frames.height = imported.height;
    game_frames.format = imported.format;
    memcpy(game_frames.images, imported.images, sizeof(game_frames.images));
    memcpy(game_frames.memories, imported.memories,
           sizeof(game_frames.memories));
    memcpy(game_frames.hardware_buffers, imported.hardware_buffers,
           sizeof(game_frames.hardware_buffers));
    game_frames.ring = imported.ring;
    game_frames.command_pool = imported.command_pool;
    game_frames.command_buffer = imported.command_buffer;
    game_frames.acquire_semaphore = imported.acquire_semaphore;
    game_frames.copy_semaphore = imported.copy_semaphore;
    game_frames.copy_fence = imported.copy_fence;
    game_frames.consumed_sequence = 0U;
    game_frames.installed = true;
    status = start_game_frame_consumer_locked();
    if (status == 0) (void)pthread_cond_broadcast(&game_frames.condition);
    if (status != 0) clear_game_frame_transport_locked();
    (void)pthread_mutex_unlock(&game_frames.mutex);
    if (status == 0) {
        BVB_LOGI("E057_FRAME_TRANSPORT_IMPORTED generation=%llu images=%u "
                 "width=%u height=%u format=%d",
                 (unsigned long long)setup->generation, setup->image_count,
                 setup->width, setup->height, (int)setup->format);
        goto done;
    }
    goto done;

import_failed:
    if (imported.copy_fence != VK_NULL_HANDLE)
        vkDestroyFence(state.device, imported.copy_fence, NULL);
    if (imported.copy_semaphore != VK_NULL_HANDLE)
        vkDestroySemaphore(state.device, imported.copy_semaphore, NULL);
    if (imported.acquire_semaphore != VK_NULL_HANDLE)
        vkDestroySemaphore(state.device, imported.acquire_semaphore, NULL);
    if (imported.command_pool != VK_NULL_HANDLE)
        vkDestroyCommandPool(state.device, imported.command_pool, NULL);
    for (uint32_t index = 0U; index < setup->image_count; ++index) {
        if (imported.images[index] != VK_NULL_HANDLE)
            vkDestroyImage(state.device, imported.images[index], NULL);
        if (imported.memories[index] != VK_NULL_HANDLE)
            vkFreeMemory(state.device, imported.memories[index], NULL);
        if (imported.hardware_buffers[index] != NULL)
            AHardwareBuffer_release(imported.hardware_buffers[index]);
    }
    if (imported.ring != NULL)
        (void)munmap(imported.ring, BVB_WSI_FRAME_RING_REGION_BYTES);

done:
    close_frame_setup_descriptors(descriptors, descriptor_count);
    (void)pthread_mutex_unlock(&renderer_device_mutex);
    return status;
}

JNIEXPORT jobjectArray JNICALL
Java_io_github_huntergdavis_bvb_visiblehost_SharedRegionProvider_nativeReceiveHardwareBuffers(
    JNIEnv *env, jclass provider_class, jint socket_fd, jint image_count) {
    (void)provider_class;
    if (socket_fd < 0 || image_count < 2 ||
        image_count > BVB_WSI_FRAME_RING_MAX_SLOTS) {
        if (socket_fd >= 0) (void)close(socket_fd);
        return NULL;
    }
    jclass hardware_buffer_class =
        (*env)->FindClass(env, "android/hardware/HardwareBuffer");
    jobjectArray result = hardware_buffer_class == NULL
        ? NULL
        : (*env)->NewObjectArray(env, image_count, hardware_buffer_class,
                                 NULL);
    for (jint index = 0; result != NULL && index < image_count; ++index) {
        AHardwareBuffer *buffer = NULL;
        if (AHardwareBuffer_recvHandleFromUnixSocket(socket_fd, &buffer) != 0 ||
            buffer == NULL) {
            result = NULL;
            break;
        }
        jobject java_buffer = AHardwareBuffer_toHardwareBuffer(env, buffer);
        AHardwareBuffer_release(buffer);
        if (java_buffer == NULL) {
            result = NULL;
            break;
        }
        (*env)->SetObjectArrayElement(env, result, index, java_buffer);
        (*env)->DeleteLocalRef(env, java_buffer);
        if ((*env)->ExceptionCheck(env)) result = NULL;
    }
    (void)close(socket_fd);
    if (hardware_buffer_class != NULL)
        (*env)->DeleteLocalRef(env, hardware_buffer_class);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return NULL;
    }
    return result;
}

JNIEXPORT jint JNICALL
Java_io_github_huntergdavis_bvb_visiblehost_SharedRegionProvider_nativeInstallFrameTransport(
    JNIEnv *env, jclass provider_class, jstring token_string, jint image_count,
    jint width, jint height, jint format, jint image_usage, jint setup_flags,
    jlong generation, jlongArray allocation_array,
    jintArray memory_type_array, jobjectArray hardware_buffer_array,
    jintArray descriptor_array) {
    (void)provider_class;
    if (token_string == NULL || allocation_array == NULL ||
        memory_type_array == NULL || hardware_buffer_array == NULL ||
        descriptor_array == NULL ||
        image_count < 2 || image_count > BVB_WSI_FRAME_RING_MAX_SLOTS ||
        (*env)->GetArrayLength(env, allocation_array) != image_count ||
        (*env)->GetArrayLength(env, memory_type_array) != image_count ||
        (*env)->GetArrayLength(env, hardware_buffer_array) !=
            (((uint32_t)setup_flags &
              BVB_ACTIVITY_FRAME_FLAG_AHARDWAREBUFFER) != 0U
                 ? image_count
                 : 0) ||
        (*env)->GetArrayLength(env, descriptor_array) !=
            (((uint32_t)setup_flags &
              BVB_ACTIVITY_FRAME_FLAG_AHARDWAREBUFFER) != 0U
                 ? 1
                 : image_count + 1)) {
        return -EINVAL;
    }
    jlong allocations[BVB_WSI_FRAME_RING_MAX_SLOTS] = {0};
    jint memory_types[BVB_WSI_FRAME_RING_MAX_SLOTS] = {0};
    jint java_descriptors[BVB_WSI_FRAME_RING_MAX_SLOTS + 1U] = {
        -1, -1, -1, -1, -1,
    };
    const uint32_t java_descriptor_count =
        ((uint32_t)setup_flags &
         BVB_ACTIVITY_FRAME_FLAG_AHARDWAREBUFFER) != 0U
            ? 1U
            : (uint32_t)image_count + 1U;
    (*env)->GetIntArrayRegion(env, descriptor_array, 0,
                              (jint)java_descriptor_count,
                              java_descriptors);
    int descriptors[BVB_WSI_FRAME_RING_MAX_SLOTS + 1U] = {-1, -1, -1, -1,
                                                           -1};
    if (java_descriptor_count == 1U)
        descriptors[image_count] = java_descriptors[0];
    else
        for (uint32_t index = 0U; index < java_descriptor_count; ++index)
            descriptors[index] = java_descriptors[index];
    AHardwareBuffer *hardware_buffers[BVB_WSI_FRAME_RING_MAX_SLOTS] = {0};
    for (uint32_t index = 0U;
         index < (uint32_t)(*env)->GetArrayLength(
                     env, hardware_buffer_array);
         ++index) {
        jobject java_buffer = (*env)->GetObjectArrayElement(
            env, hardware_buffer_array, (jsize)index);
        hardware_buffers[index] = java_buffer == NULL
            ? NULL
            : AHardwareBuffer_fromHardwareBuffer(env, java_buffer);
        if (hardware_buffers[index] != NULL)
            AHardwareBuffer_acquire(hardware_buffers[index]);
        if (java_buffer != NULL) (*env)->DeleteLocalRef(env, java_buffer);
    }
    (*env)->GetLongArrayRegion(env, allocation_array, 0, image_count,
                               allocations);
    (*env)->GetIntArrayRegion(env, memory_type_array, 0, image_count,
                              memory_types);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        close_frame_setup_descriptors(descriptors,
                                      (uint32_t)image_count + 1U);
        for (uint32_t index = 0U; index < (uint32_t)image_count; ++index)
            if (hardware_buffers[index] != NULL)
                AHardwareBuffer_release(hardware_buffers[index]);
        return -EINVAL;
    }
    const char *token = (*env)->GetStringUTFChars(env, token_string, NULL);
    if (token == NULL) {
        close_frame_setup_descriptors(descriptors,
                                      (uint32_t)image_count + 1U);
        for (uint32_t index = 0U; index < (uint32_t)image_count; ++index)
            if (hardware_buffers[index] != NULL)
                AHardwareBuffer_release(hardware_buffers[index]);
        return -EINVAL;
    }
    uint8_t parsed_token[BVB_LIFECYCLE_TOKEN_SIZE];
    int status = bvb_lifecycle_token_from_hex(token, parsed_token);
    (*env)->ReleaseStringUTFChars(env, token_string, token);
    (void)pthread_mutex_lock(&lifecycle_mutex);
    const bool authorized = status == 0 && lifecycle.configured &&
                            token_matches(parsed_token, lifecycle.token);
    (void)pthread_mutex_unlock(&lifecycle_mutex);
    if (!authorized) {
        close_frame_setup_descriptors(descriptors,
                                      (uint32_t)image_count + 1U);
        for (uint32_t index = 0U; index < (uint32_t)image_count; ++index)
            if (hardware_buffers[index] != NULL)
                AHardwareBuffer_release(hardware_buffers[index]);
        return -EACCES;
    }
    struct bvb_activity_frame_setup setup = {
        .magic = BVB_ACTIVITY_FRAME_SETUP_MAGIC,
        .version = BVB_ACTIVITY_FRAME_SETUP_VERSION,
        .header_bytes = BVB_ACTIVITY_FRAME_SETUP_BYTES,
        .image_count = (uint32_t)image_count,
        .width = (uint32_t)width,
        .height = (uint32_t)height,
        .format = (uint32_t)format,
        .image_usage = (uint32_t)image_usage,
        .flags = (uint32_t)setup_flags,
        .generation = (uint64_t)generation,
    };
    for (uint32_t index = 0U; index < (uint32_t)image_count; ++index) {
        setup.allocation_sizes[index] = (uint64_t)allocations[index];
        setup.memory_type_indices[index] = (uint32_t)memory_types[index];
    }
    uint8_t validation[BVB_ACTIVITY_FRAME_SETUP_BYTES];
    if ((*env)->ExceptionCheck(env) ||
        bvb_activity_frame_setup_encode(validation, &setup) != 0) {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        close_frame_setup_descriptors(descriptors,
                                      (uint32_t)image_count + 1U);
        for (uint32_t index = 0U; index < (uint32_t)image_count; ++index)
            if (hardware_buffers[index] != NULL)
                AHardwareBuffer_release(hardware_buffers[index]);
        return -EINVAL;
    }
    status = import_game_frame_transport(&setup, descriptors,
                                         hardware_buffers);
    for (uint32_t index = 0U; index < (uint32_t)image_count; ++index)
        if (hardware_buffers[index] != NULL)
            AHardwareBuffer_release(hardware_buffers[index]);
    return status;
}

static void destroy_renderer(void) {
    if (state.device != VK_NULL_HANDLE) {
        (void)vkDeviceWaitIdle(state.device);
    }
    (void)pthread_mutex_lock(&game_frames.mutex);
    clear_game_frame_transport_locked();
    (void)pthread_mutex_unlock(&game_frames.mutex);
    (void)pthread_mutex_lock(&external_memory_mutex);
    if (state.device != VK_NULL_HANDLE) {
        if (state.external_sync_semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(state.device, state.external_sync_semaphore,
                               NULL);
        }
        if (state.external_image_semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(state.device, state.external_image_semaphore,
                               NULL);
        }
        if (state.external_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(state.device, state.external_buffer, NULL);
        }
        if (state.external_image != VK_NULL_HANDLE) {
            vkDestroyImage(state.device, state.external_image, NULL);
        }
        if (state.external_memory != VK_NULL_HANDLE) {
            vkFreeMemory(state.device, state.external_memory, NULL);
        }
        if (state.external_image_memory != VK_NULL_HANDLE) {
            vkFreeMemory(state.device, state.external_image_memory, NULL);
        }
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
    (void)pthread_mutex_unlock(&external_memory_mutex);
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

static bool create_external_memory(VkPhysicalDevice physical_device) {
    PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR query =
        (PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR)
            vkGetInstanceProcAddr(
                state.instance,
                "vkGetPhysicalDeviceExternalBufferPropertiesKHR");
    if (query == NULL) {
        query = (PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR)
            vkGetInstanceProcAddr(
                state.instance,
                "vkGetPhysicalDeviceExternalBufferProperties");
    }
    state.get_memory_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(
        state.device, "vkGetMemoryFdKHR");
    state.get_memory_fd_properties =
        (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(
            state.device, "vkGetMemoryFdPropertiesKHR");
    state.get_semaphore_fd = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(
        state.device, "vkGetSemaphoreFdKHR");
    if (query == NULL || state.get_memory_fd == NULL ||
        state.get_memory_fd_properties == NULL ||
        state.get_semaphore_fd == NULL) {
        BVB_LOGE("E036_FAIL missing_external_memory_entry_points");
        return false;
    }
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    const VkPhysicalDeviceExternalBufferInfo query_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .usage = usage,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    VkExternalBufferProperties properties = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES,
    };
    query(physical_device, &query_info, &properties);
    const VkExternalMemoryFeatureFlags required =
        VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
        VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
    if ((properties.externalMemoryProperties.externalMemoryFeatures &
         required) != required ||
        (properties.externalMemoryProperties.compatibleHandleTypes &
         VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) == 0U) {
        BVB_LOGE("E036_FAIL opaque_fd_features=%u compatible=%u",
                 properties.externalMemoryProperties.externalMemoryFeatures,
                 properties.externalMemoryProperties.compatibleHandleTypes);
        return false;
    }
    const VkExternalMemoryBufferCreateInfo external_buffer_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    const VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &external_buffer_info,
        .size = BVB_E020_REGION_BYTES,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult result = vkCreateBuffer(
        state.device, &buffer_info, NULL, &state.external_buffer);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E036_FAIL vkCreateBuffer=%d", (int)result);
        return false;
    }
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(
        state.device, state.external_buffer, &requirements);
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
    bool found = false;
    bool coherent_found = false;
    for (uint32_t index = 0U; index < memory_properties.memoryTypeCount;
         ++index) {
        const VkMemoryPropertyFlags flags =
            memory_properties.memoryTypes[index].propertyFlags;
        if ((requirements.memoryTypeBits & (UINT32_C(1) << index)) == 0U ||
            (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0U) {
            continue;
        }
        const bool coherent =
            (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0U;
        if (!found || (coherent && !coherent_found)) {
            state.external_memory_type_index = index;
            state.external_memory_property_flags = flags;
            found = true;
            coherent_found = coherent;
        }
    }
    if (!found) {
        BVB_LOGE("E036_FAIL no_host_visible_memory_type bits=%u",
                 requirements.memoryTypeBits);
        return false;
    }
    const bool dedicated_only =
        (properties.externalMemoryProperties.externalMemoryFeatures &
         VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0U;
    const VkMemoryDedicatedAllocateInfo dedicated_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .buffer = state.external_buffer,
    };
    const VkExportMemoryAllocateInfo export_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = dedicated_only ? &dedicated_info : NULL,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    const VkMemoryAllocateInfo allocation_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &export_info,
        .allocationSize = requirements.size,
        .memoryTypeIndex = state.external_memory_type_index,
    };
    result = vkAllocateMemory(
        state.device, &allocation_info, NULL, &state.external_memory);
    if (result == VK_SUCCESS) {
        result = vkBindBufferMemory(
            state.device, state.external_buffer, state.external_memory, 0U);
    }
    void *mapped = NULL;
    if (result == VK_SUCCESS) {
        result = vkMapMemory(state.device, state.external_memory, 0U,
                             VK_WHOLE_SIZE, 0U, &mapped);
    }
    if (result != VK_SUCCESS || mapped == NULL) {
        BVB_LOGE("E036_FAIL export_memory_setup=%d", (int)result);
        return false;
    }
    for (uint32_t index = 0U; index < BVB_E020_REGION_BYTES; ++index) {
        ((uint8_t *)mapped)[index] =
            (uint8_t)(index ^ (index >> 4U) ^ UINT32_C(0x5a));
    }
    if (!coherent_found) {
        const VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = state.external_memory,
            .offset = 0U,
            .size = VK_WHOLE_SIZE,
        };
        result = vkFlushMappedMemoryRanges(state.device, 1U, &range);
    }
    vkUnmapMemory(state.device, state.external_memory);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E036_FAIL vkFlushMappedMemoryRanges=%d", (int)result);
        return false;
    }
    state.external_allocation_size = requirements.size;
    BVB_LOGI("E036_EXPORT_READY allocation=%llu type=%u flags=%u bytes=%u",
             (unsigned long long)state.external_allocation_size,
             state.external_memory_type_index,
             state.external_memory_property_flags, BVB_E020_REGION_BYTES);
    return true;
}

static bool create_external_image(VkPhysicalDevice physical_device) {
    PFN_vkGetPhysicalDeviceImageFormatProperties2 query =
        (PFN_vkGetPhysicalDeviceImageFormatProperties2)vkGetInstanceProcAddr(
            state.instance, "vkGetPhysicalDeviceImageFormatProperties2");
    if (query == NULL) {
        query = (PFN_vkGetPhysicalDeviceImageFormatProperties2)
            vkGetInstanceProcAddr(
                state.instance,
                "vkGetPhysicalDeviceImageFormatProperties2KHR");
    }
    if (query == NULL) {
        BVB_LOGE("E038_FAIL missing_external_image_query");
        return false;
    }
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkPhysicalDeviceExternalImageFormatInfo external_query = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    const VkPhysicalDeviceImageFormatInfo2 image_query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &external_query,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
    };
    VkExternalImageFormatProperties external_properties = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
    };
    VkImageFormatProperties2 image_properties = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &external_properties,
    };
    const VkResult query_result = query(
        physical_device, &image_query, &image_properties);
    const VkExternalMemoryProperties *external =
        &external_properties.externalMemoryProperties;
    const VkExternalMemoryFeatureFlags required =
        VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
        VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
    if (query_result != VK_SUCCESS ||
        (external->externalMemoryFeatures & required) != required ||
        (external->compatibleHandleTypes &
         VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) == 0U) {
        BVB_LOGE("E038_FAIL image_query=%d features=%u compatible=%u",
                 (int)query_result, external->externalMemoryFeatures,
                 external->compatibleHandleTypes);
        return false;
    }
    const VkExternalMemoryImageCreateInfo external_image_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    const VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_image_info,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {BVB_E038_IMAGE_WIDTH, BVB_E038_IMAGE_HEIGHT, 1U},
        .mipLevels = 1U,
        .arrayLayers = 1U,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult result = vkCreateImage(
        state.device, &image_info, NULL, &state.external_image);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E038_FAIL vkCreateImage=%d", (int)result);
        return false;
    }
    VkMemoryRequirements requirements = {0};
    vkGetImageMemoryRequirements(
        state.device, state.external_image, &requirements);
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
    bool found = false;
    bool device_local_found = false;
    for (uint32_t index = 0U; index < memory_properties.memoryTypeCount;
         ++index) {
        if ((requirements.memoryTypeBits & (UINT32_C(1) << index)) == 0U) {
            continue;
        }
        const bool device_local =
            (memory_properties.memoryTypes[index].propertyFlags &
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U;
        if (!found || (device_local && !device_local_found)) {
            state.external_image_memory_type_index = index;
            found = true;
            device_local_found = device_local;
        }
    }
    if (!found) {
        BVB_LOGE("E038_FAIL no_image_memory_type bits=%u",
                 requirements.memoryTypeBits);
        return false;
    }
    const bool dedicated_only =
        (external->externalMemoryFeatures &
         VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0U;
    const VkMemoryDedicatedAllocateInfo dedicated_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = state.external_image,
    };
    const VkExportMemoryAllocateInfo export_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = dedicated_only ? &dedicated_info : NULL,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    const VkMemoryAllocateInfo allocation_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &export_info,
        .allocationSize = requirements.size,
        .memoryTypeIndex = state.external_image_memory_type_index,
    };
    result = vkAllocateMemory(
        state.device, &allocation_info, NULL, &state.external_image_memory);
    if (result == VK_SUCCESS) {
        result = vkBindImageMemory(
            state.device, state.external_image,
            state.external_image_memory, 0U);
    }
    if (result != VK_SUCCESS) {
        BVB_LOGE("E038_FAIL image_memory_setup=%d", (int)result);
        return false;
    }
    state.external_image_allocation_size = requirements.size;
    BVB_LOGI("E038_IMAGE_READY width=%u height=%u format=%u allocation=%llu "
             "type=%u features=%u",
             BVB_E038_IMAGE_WIDTH, BVB_E038_IMAGE_HEIGHT,
             (unsigned int)VK_FORMAT_R8G8B8A8_UNORM,
             (unsigned long long)state.external_image_allocation_size,
             state.external_image_memory_type_index,
             external->externalMemoryFeatures);
    return true;
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

static VkPipelineLayout pipeline_layout_from_bits(uint64_t bits) {
    VkPipelineLayout handle = VK_NULL_HANDLE;
    memcpy(&handle, &bits, sizeof(handle));
    return handle;
}

static int build_triangle_batch(uint8_t *batch, size_t capacity,
                                VkExtent2D extent, uint64_t sequence,
                                size_t *length) {
    const uint64_t command_buffer_id =
        bvb_handle_id(BVB_OBJECT_COMMAND_BUFFER, 1U);
    const uint64_t image_view_id =
        bvb_handle_id(BVB_OBJECT_IMAGE_VIEW, 1U);
    const uint64_t pipeline_id = bvb_handle_id(BVB_OBJECT_PIPELINE, 1U);
    const uint64_t pipeline_layout_id =
        bvb_handle_id(BVB_OBJECT_PIPELINE_LAYOUT, 1U);
    struct bvb_command_batch_builder builder;
    int result = bvb_command_batch_begin(&builder, batch, capacity,
                                         command_buffer_id, sequence);
    if (result == 0) {
        struct bvb_begin_rendering_command rendering = {
            .width = extent.width,
            .height = extent.height,
            .layer_count = 1U,
            .color_attachment_count = 1U,
            .color_attachments = {{
                .image_view_id = image_view_id,
                .image_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .store_op = VK_ATTACHMENT_STORE_OP_STORE,
            }},
        };
        const float clear[4] = {0.015F, 0.02F, 0.06F, 1.0F};
        memcpy(rendering.color_attachments[0].clear_words, clear,
               sizeof(clear));
        result = bvb_command_batch_append_begin_rendering(
            &builder, &rendering);
    }
    if (result == 0) {
        result = bvb_command_batch_append_bind_graphics_pipeline(
            &builder,
            &(const struct bvb_bind_graphics_pipeline_command){
                .pipeline_id = pipeline_id,
            });
    }
    if (result == 0) {
        result = bvb_command_batch_append_push_rotation(
            &builder,
            &(const struct bvb_push_rotation_command){
                .pipeline_layout_id = pipeline_layout_id,
                .angle_radians =
                    (float)((sequence - 1U) %
                            BVB_TRIANGLE_ROTATION_FRAMES_PER_TURN) *
                    0.0104719755F,
                .aspect_ratio = (float)extent.width / (float)extent.height,
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
    VkImageView image_view, VkPipeline pipeline,
    VkPipelineLayout pipeline_layout, VkRenderPass render_pass,
    VkFramebuffer framebuffer, VkExtent2D extent) {
    struct bvb_command_batch_info info;
    int result = bvb_command_batch_validate(batch, length, &info);
    if (result != 0 || info.command_count != 7U ||
        pipeline_layout == VK_NULL_HANDLE || render_pass == VK_NULL_HANDLE ||
        framebuffer == VK_NULL_HANDLE) {
        return result != 0 ? result : -EPROTO;
    }
    const uint64_t image_view_id =
        bvb_handle_id(BVB_OBJECT_IMAGE_VIEW, 1U);
    const uint64_t pipeline_id = bvb_handle_id(BVB_OBJECT_PIPELINE, 1U);
    const uint64_t pipeline_layout_id =
        bvb_handle_id(BVB_OBJECT_PIPELINE_LAYOUT, 1U);
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
    if (result == 0) {
        result = bvb_handle_table_insert(
            &handles, pipeline_layout_id, 0U,
            native_handle_bits(&pipeline_layout, sizeof(pipeline_layout)));
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
    for (uint32_t index = 0U; index < 7U; ++index) {
        result = bvb_command_batch_next(&iterator, &record);
        if (result != 0) {
            return -EPROTO;
        }
        if (index == 0U && record.opcode == BVB_COMMAND_BEGIN_RENDERING) {
            struct bvb_begin_rendering_command command;
            uint64_t image_view_bits = 0U;
            result = bvb_command_decode_begin_rendering(&record, &command);
            const struct bvb_vulkan_rendering_attachment *attachment =
                &command.color_attachments[0];
            if (result == 0) {
                result = bvb_handle_table_lookup(
                    &handles, attachment->image_view_id,
                    BVB_OBJECT_IMAGE_VIEW, NULL, &image_view_bits);
            }
            if (result != 0) {
                return result;
            }
            if (image_view_from_bits(image_view_bits) != image_view ||
                command.color_attachment_count != 1U ||
                attachment->image_layout !=
                    (uint32_t)VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
                attachment->load_op != (uint32_t)VK_ATTACHMENT_LOAD_OP_CLEAR ||
                attachment->store_op !=
                    (uint32_t)VK_ATTACHMENT_STORE_OP_STORE ||
                command.layer_count != 1U || command.width != extent.width ||
                command.height != extent.height) {
                return -ENOTSUP;
            }
            VkClearValue clear_value;
            memcpy(&clear_value, attachment->clear_words,
                   sizeof(attachment->clear_words));
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
        } else if (index == 2U &&
                   record.opcode == BVB_COMMAND_PUSH_ROTATION) {
            struct bvb_push_rotation_command command;
            uint64_t pipeline_layout_bits = 0U;
            result = bvb_command_decode_push_rotation(&record, &command);
            if (result == 0) {
                result = bvb_handle_table_lookup(
                    &handles, command.pipeline_layout_id,
                    BVB_OBJECT_PIPELINE_LAYOUT, NULL, &pipeline_layout_bits);
            }
            if (result != 0 ||
                pipeline_layout_from_bits(pipeline_layout_bits) !=
                    pipeline_layout ||
                command.aspect_ratio !=
                    (float)extent.width / (float)extent.height) {
                return result != 0 ? result : -ENOTSUP;
            }
            const float constants[] = {
                command.angle_radians,
                command.aspect_ratio,
            };
            vkCmdPushConstants(command_buffer, pipeline_layout,
                               VK_SHADER_STAGE_VERTEX_BIT, 0U,
                               sizeof(constants), constants);
        } else if (index == 3U && record.opcode == BVB_COMMAND_SET_VIEWPORT) {
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
        } else if (index == 4U && record.opcode == BVB_COMMAND_SET_SCISSOR) {
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
        } else if (index == 5U && record.opcode == BVB_COMMAND_DRAW) {
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
        } else if (index == 6U &&
                   record.opcode == BVB_COMMAND_END_RENDERING) {
            vkCmdEndRenderPass(command_buffer);
        } else {
            return -EPROTO;
        }
    }
    return bvb_command_batch_next(&iterator, &record) == 1 ? 0 : -EPROTO;
}

static int complete_external_batch_mode(bool claimed, int status,
                                        bool accept_next) {
    if (!claimed || visible_ingress == NULL) {
        return 0;
    }
    int result = accept_next
                     ? bvb_visible_ingress_complete_and_accept_next(
                           visible_ingress, status)
                     : bvb_visible_ingress_complete(visible_ingress, status);
    if (result != 0) {
        BVB_LOGE("E018_COMPLETE_FAIL status=%d result=%d", status, result);
    }
    return result;
}

static void complete_external_batch(bool claimed, int status) {
    (void)complete_external_batch_mode(claimed, status, false);
}

static int render_triangle_frame_unlocked(
    const uint8_t *render_batch, size_t render_batch_length,
    const VkImage *images, uint32_t image_count, VkFormat image_format,
    VkExtent2D extent, uint32_t *presented_image_index) {
    uint32_t image_index = 0U;
    VkResult result = vkAcquireNextImageKHR(
        state.device, state.swapchain, UINT64_MAX, state.acquire_semaphore,
        VK_NULL_HANDLE, &image_index);
    if ((result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) ||
        image_index >= image_count) {
        BVB_LOGE("E008_FAIL acquire=%d index=%u", (int)result, image_index);
        return -EIO;
    }

    VkImageView image_view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    bool queue_submitted = false;
    int status = 0;
    const VkImageViewCreateInfo image_view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = images[image_index],
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = image_format,
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
                               &image_view);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E016_FAIL image_view=%d", (int)result);
        status = -EIO;
        goto cleanup;
    }
    const VkFramebufferCreateInfo framebuffer_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = state.render_pass,
        .attachmentCount = 1U,
        .pAttachments = &image_view,
        .width = extent.width,
        .height = extent.height,
        .layers = 1U,
    };
    result = vkCreateFramebuffer(state.device, &framebuffer_info, NULL,
                                 &framebuffer);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E016_FAIL framebuffer=%d", (int)result);
        status = -EIO;
        goto cleanup;
    }
    const VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = state.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1U,
    };
    result = vkAllocateCommandBuffers(state.device, &command_info,
                                      &command_buffer);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E008_FAIL command_buffer=%d", (int)result);
        status = -EIO;
        goto cleanup;
    }
    const VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    result = vkBeginCommandBuffer(command_buffer, &begin_info);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E008_FAIL command_begin=%d", (int)result);
        status = -EIO;
        goto cleanup;
    }
    const VkImageSubresourceRange range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0U,
        .levelCount = 1U,
        .baseArrayLayer = 0U,
        .layerCount = 1U,
    };
    const VkImageMemoryBarrier to_render = {
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
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0U,
                         0U, NULL, 0U, NULL, 1U, &to_render);
    status = replay_triangle_batch(
        render_batch, render_batch_length, command_buffer, image_view,
        state.pipeline, state.pipeline_layout, state.render_pass, framebuffer,
        extent);
    if (status != 0) {
        BVB_LOGE("E016_FAIL batch_replay=%d", status);
        goto cleanup;
    }
    const VkImageMemoryBarrier to_present = {
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
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0U, 0U, NULL,
                         0U, NULL, 1U, &to_present);
    result = vkEndCommandBuffer(command_buffer);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E008_FAIL command_end=%d", (int)result);
        status = -EIO;
        goto cleanup;
    }
    const VkPipelineStageFlags wait_stage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1U,
        .pWaitSemaphores = &state.acquire_semaphore,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1U,
        .pCommandBuffers = &command_buffer,
        .signalSemaphoreCount = 1U,
        .pSignalSemaphores = &state.render_semaphore,
    };
    result = vkQueueSubmit(state.queue, 1U, &submit_info, VK_NULL_HANDLE);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E008_FAIL submit=%d", (int)result);
        status = -EIO;
        goto cleanup;
    }
    queue_submitted = true;
    const VkPresentInfoKHR present_info = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1U,
        .pWaitSemaphores = &state.render_semaphore,
        .swapchainCount = 1U,
        .pSwapchains = &state.swapchain,
        .pImageIndices = &image_index,
    };
    result = vkQueuePresentKHR(state.queue, &present_info);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        BVB_LOGE("E008_FAIL present=%d", (int)result);
        status = -EIO;
        goto cleanup;
    }
    result = vkQueueWaitIdle(state.queue);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E008_FAIL queue_idle=%d", (int)result);
        status = -EIO;
        goto cleanup;
    }
    queue_submitted = false;
    *presented_image_index = image_index;

cleanup:
    if (queue_submitted) {
        (void)vkQueueWaitIdle(state.queue);
    }
    if (command_buffer != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(state.device, state.command_pool, 1U,
                             &command_buffer);
    }
    if (framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(state.device, framebuffer, NULL);
    }
    if (image_view != VK_NULL_HANDLE) {
        vkDestroyImageView(state.device, image_view, NULL);
    }
    return status;
}

static int render_triangle_frame(
    const uint8_t *render_batch, size_t render_batch_length,
    const VkImage *images, uint32_t image_count, VkFormat image_format,
    VkExtent2D extent, uint32_t *presented_image_index) {
    (void)pthread_mutex_lock(&queue_mutex);
    const int status = render_triangle_frame_unlocked(
        render_batch, render_batch_length, images, image_count, image_format,
        extent, presented_image_index);
    (void)pthread_mutex_unlock(&queue_mutex);
    return status;
}

static bool create_renderer(ANativeWindow *window) {
    static const char *const instance_extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
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
        .enabledExtensionCount = 4,
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
                              VK_KHR_SWAPCHAIN_EXTENSION_NAME) ||
        !has_device_extension(physical_device,
                              VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) ||
        !has_device_extension(physical_device,
                              VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) ||
        !has_device_extension(
            physical_device,
            VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME) ||
        !has_device_extension(physical_device,
                              VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME) ||
        !has_device_extension(physical_device,
                              VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME)) {
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
         (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
          VK_IMAGE_USAGE_TRANSFER_DST_BIT)) !=
            (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
             VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
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
    BVB_LOGI("E115_SURFACE_OPACITY window_format=%d supported_alpha=0x%x "
             "selected_alpha=0x%x",
             ANativeWindow_getFormat(window),
             (unsigned int)capabilities.supportedCompositeAlpha,
             (unsigned int)composite_alpha);

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
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
    };
    const VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 6,
        .ppEnabledExtensionNames = device_extensions,
    };
    result = vkCreateDevice(physical_device, &device_info, NULL,
                            &state.device);
    if (result != VK_SUCCESS) {
        BVB_LOGE("E008_FAIL vkCreateDevice=%d", (int)result);
        return false;
    }
    vkGetDeviceQueue(state.device, queue_family_index, 0, &state.queue);
    state.physical_device = physical_device;
    state.queue_family_index = queue_family_index;
    (void)pthread_mutex_lock(&external_memory_mutex);
    const bool external_memory_ready = create_external_memory(physical_device);
    const bool external_image_ready = external_memory_ready &&
                                      create_external_image(physical_device);
    (void)pthread_mutex_unlock(&external_memory_mutex);
    if (!external_image_ready) return false;

    VkExtent2D extent = choose_extent(&capabilities, window);
    const VkSwapchainCreateInfoKHR swapchain_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = state.surface,
        .minImageCount = capabilities.minImageCount,
        .imageFormat = surface_format.format,
        .imageColorSpace = surface_format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT,
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
    memcpy(state.swapchain_images, images,
           image_count * sizeof(state.swapchain_images[0]));
    state.swapchain_image_count = image_count;
    state.swapchain_format = surface_format.format;
    state.swapchain_extent = extent;

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
    const VkPushConstantRange push_constant_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0U,
        .size = 2U * sizeof(float),
    };
    const VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1U,
        .pPushConstantRanges = &push_constant_range,
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
    (void)pthread_mutex_lock(&external_memory_mutex);
    const int external_sync_status = cache_external_sync();
    const int external_image_status = external_sync_status == 0
                                          ? cache_external_image_sync()
                                          : external_sync_status;
    (void)pthread_mutex_unlock(&external_memory_mutex);
    if (external_sync_status != 0) {
        BVB_LOGE("E037_CACHE_FAIL status=%d", external_sync_status);
        return false;
    }
    if (external_image_status != 0) {
        BVB_LOGE("E038_CACHE_FAIL status=%d", external_image_status);
        return false;
    }

    const uint32_t frame_count =
        visible_ingress == NULL ? 1U : atomic_load(&visible_frame_count);
    if (frame_count > 1U) {
        BVB_LOGI("E023_RING_BEGIN frames=%u", frame_count);
        for (uint32_t frame = 0U; frame < frame_count; ++frame) {
            const uint8_t *render_batch = NULL;
            size_t render_batch_length = 0U;
            uint64_t external_sequence = 0U;
            int batch_status = bvb_visible_ingress_wait_batch(
                visible_ingress, 10000U, &render_batch,
                &render_batch_length, &external_sequence);
            if (batch_status != 0) {
                BVB_LOGE("E023_BATCH_WAIT_FAIL frame=%u status=%d", frame,
                         batch_status);
                return false;
            }
            BVB_LOGI("E023_BATCH_CLAIM frame=%u sequence=%llu bytes=%zu",
                     frame, (unsigned long long)external_sequence,
                     render_batch_length);
            uint32_t image_index = 0U;
            batch_status = render_triangle_frame(
                render_batch, render_batch_length, images, image_count,
                surface_format.format, extent, &image_index);
            const bool accept_next = frame + 1U < frame_count;
            int complete_status = complete_external_batch_mode(
                true, batch_status, accept_next);
            if (batch_status != 0 || complete_status != 0) {
                BVB_LOGE("E023_FRAME_FAIL frame=%u sequence=%llu "
                         "render_status=%d complete_status=%d",
                         frame, (unsigned long long)external_sequence,
                         batch_status, complete_status);
                return false;
            }
            BVB_LOGI("E023_FRAME_PASS frame=%u sequence=%llu index=%u",
                     frame, (unsigned long long)external_sequence,
                     image_index);
        }
        state.window = window;
        BVB_LOGI("E023_PASS frames=%u width=%u height=%u images=%u",
                 frame_count, extent.width, extent.height, image_count);
        return true;
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
    int batch_status = build_triangle_batch(
        triangle_batch, sizeof(triangle_batch), extent, 1U,
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
        state.pipeline, state.pipeline_layout, state.render_pass,
        state.framebuffer, extent);
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
    BVB_LOGI("E016_PASS batch_bytes=%zu commands=7 backend=render_pass "
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

static bool renderer_generation_current(uint64_t generation,
                                        ANativeWindow *window) {
    (void)pthread_mutex_lock(&renderer.mutex);
    const bool current = generation == renderer.requested_generation &&
                         window == renderer.window;
    (void)pthread_mutex_unlock(&renderer.mutex);
    return current;
}

static uint64_t monotonic_nanoseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0U;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static int animate_local_heartbeat(uint64_t generation,
                                   ANativeWindow *window) {
    uint64_t sequence = 2U;
    uint64_t measurement_started_ns = monotonic_nanoseconds();
    uint32_t measured_frames = 0U;
    BVB_LOGI("E033_HEARTBEAT_BEGIN pacing=fifo_display target_hz=60_or_120");
    while (renderer_generation_current(generation, window)) {
        uint8_t triangle_batch[512];
        size_t triangle_batch_length = 0U;
        int status = build_triangle_batch(
            triangle_batch, sizeof(triangle_batch), state.swapchain_extent,
            sequence, &triangle_batch_length);
        uint32_t image_index = 0U;
        if (status == 0) {
            status = render_triangle_frame(
                triangle_batch, triangle_batch_length,
                state.swapchain_images, state.swapchain_image_count,
                state.swapchain_format, state.swapchain_extent, &image_index);
        }
        if (status != 0) {
            BVB_LOGE("E033_HEARTBEAT_FAIL sequence=%llu status=%d",
                     (unsigned long long)sequence, status);
            return status;
        }
        measured_frames += 1U;
        if (measured_frames == 120U) {
            const uint64_t measured_ns = monotonic_nanoseconds();
            if (measurement_started_ns != 0U &&
                measured_ns > measurement_started_ns) {
                const uint64_t elapsed_ns =
                    measured_ns - measurement_started_ns;
                const uint64_t fps_milli =
                    UINT64_C(120000000000000) / elapsed_ns;
                BVB_LOGI("E033_HEARTBEAT_RATE fps=%llu.%03llu index=%u",
                         (unsigned long long)(fps_milli / 1000U),
                         (unsigned long long)(fps_milli % 1000U), image_index);
            }
            measurement_started_ns = measured_ns;
            measured_frames = 0U;
        }
        sequence = sequence == UINT64_MAX ? 1U : sequence + 1U;
    }
    BVB_LOGI("E033_HEARTBEAT_END sequence=%llu",
             (unsigned long long)sequence);
    return 0;
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

        (void)pthread_mutex_lock(&renderer_device_mutex);
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
        (void)pthread_mutex_unlock(&renderer_device_mutex);

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

        if (success && current && visible_ingress == NULL &&
            !atomic_load(&retain_external_renderer)) {
            const int heartbeat_status =
                animate_local_heartbeat(generation, window);
            if (heartbeat_status != 0) {
                (void)pthread_mutex_lock(&renderer.mutex);
                const bool heartbeat_current =
                    generation == renderer.requested_generation &&
                    window == renderer.window;
                if (heartbeat_current) renderer.ready = false;
                (void)pthread_mutex_unlock(&renderer.mutex);
                if (heartbeat_current) {
                    emit_lifecycle(BVB_LIFECYCLE_EVENT_RENDERER_FAILED,
                                   width, height);
                }
                (void)pthread_mutex_lock(&renderer_device_mutex);
                destroy_renderer();
                (void)pthread_mutex_unlock(&renderer_device_mutex);
                success = false;
            }
        }

        if (success && !current) {
            (void)pthread_mutex_lock(&renderer_device_mutex);
            destroy_renderer();
            (void)pthread_mutex_unlock(&renderer_device_mutex);
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
    atomic_store(&activity_window_present, true);
    (void)pthread_mutex_lock(&game_frames.mutex);
    (void)pthread_cond_broadcast(&game_frames.condition);
    (void)pthread_mutex_unlock(&game_frames.mutex);
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
    atomic_store(&activity_window_present, false);
    (void)pthread_mutex_lock(&game_frames.mutex);
    (void)pthread_cond_broadcast(&game_frames.condition);
    (void)pthread_mutex_unlock(&game_frames.mutex);
    if (atomic_load(&retain_external_renderer) &&
        !game_frame_transport_installed()) {
        BVB_LOGI("E041_OFFSCREEN_CACHE_RETAINED");
    } else {
        int result = schedule_renderer(NULL, true);
        if (result != 0) {
            BVB_LOGE("E017_DESTROY_SCHEDULE_FAIL status=%d", result);
        }
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
    atomic_store(&activity_resumed, true);
    (void)pthread_mutex_lock(&game_frames.mutex);
    (void)pthread_cond_broadcast(&game_frames.condition);
    (void)pthread_mutex_unlock(&game_frames.mutex);
    emit_lifecycle(BVB_LIFECYCLE_EVENT_RESUMED, 0, 0);
}

static void on_pause(ANativeActivity *activity) {
    (void)activity;
    atomic_store(&activity_resumed, false);
    atomic_store(&activity_window_present, false);
    (void)pthread_mutex_lock(&game_frames.mutex);
    (void)pthread_cond_broadcast(&game_frames.condition);
    (void)pthread_mutex_unlock(&game_frames.mutex);
    emit_lifecycle(BVB_LIFECYCLE_EVENT_PAUSED, 0, 0);
}

static void on_stop(ANativeActivity *activity) {
    (void)activity;
    emit_lifecycle(BVB_LIFECYCLE_EVENT_STOPPED, 0, 0);
}

static void on_destroy(ANativeActivity *activity) {
    (void)activity;
    atomic_store(&activity_resumed, false);
    int result = schedule_renderer(NULL, true);
    if (result != 0) {
        BVB_LOGE("E017_ACTIVITY_DESTROY_SCHEDULE_FAIL status=%d", result);
    }
    emit_lifecycle(BVB_LIFECYCLE_EVENT_DESTROYED, 0, 0);
    write_native_binder_diagnostic("activity_destroyed", 0, false);
    disable_lifecycle();
}

__attribute__((visibility("default"))) void
ANativeActivity_onCreate(ANativeActivity *activity, void *saved_state,
                         size_t saved_state_size) {
    (void)saved_state;
    (void)saved_state_size;
    configure_lifecycle(activity);
    (void)pthread_once(&external_broker_once, start_external_broker);
    (void)pthread_once(&native_binder_once, start_native_binder_service);
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
    /*
     * A D3D backbuffer's alpha channel is not desktop-compositor content.
     * Advertising the NativeActivity layer as RGBA lets SurfaceFlinger blend
     * arbitrary game alpha over the Android desktop.  RGBX retains the same
     * four-byte Vulkan-compatible storage while making the layer opaque.
     */
    ANativeActivity_setWindowFormat(activity, WINDOW_FORMAT_RGBX_8888);
    ANativeActivity_setWindowFlags(
        activity, AWINDOW_FLAG_FULLSCREEN | AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
    apply_immersive_mode(activity);
    BVB_LOGI("E008_ACTIVITY_CREATED");
    emit_lifecycle(BVB_LIFECYCLE_EVENT_CREATED, 0, 0);
    emit_lifecycle(BVB_LIFECYCLE_EVENT_NATIVE_BINDER_STATUS,
                   (uint32_t)native_binder_registration_status,
                   UINT32_C(0xe040));
}
