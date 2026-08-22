#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define VK_NO_PROTOTYPES

#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR
#include <android/hardware_buffer.h>
typedef int (*bvb_ahb_is_supported_fn)(const AHardwareBuffer_Desc *);
typedef int (*bvb_ahb_allocate_fn)(const AHardwareBuffer_Desc *,
                                   AHardwareBuffer **);
typedef void (*bvb_ahb_release_fn)(AHardwareBuffer *);
#endif

#include <bvb/command_batch.h>
#include <bvb/handle.h>
#include <bvb/vulkan_global.h>

#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <linux/memfd.h>

enum {
    BVB_GLOBAL_OBJECT_CAPACITY = 4096,
    BVB_DESCRIPTOR_TEMPLATE_METADATA_CAPACITY = 256,
    BVB_EXPOSED_INSTANCE_EXTENSION_CAPACITY = 3,
};

static const char *const bvb_instance_extension_allowlist[
    BVB_EXPOSED_INSTANCE_EXTENSION_CAPACITY] = {
    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
};

struct bvb_device_metadata {
    uint64_t device_id;
    uint64_t non_coherent_atom_size;
    uint32_t queue_create_info_count;
    struct bvb_vulkan_device_queue_create_info
        queue_create_infos[BVB_VULKAN_MAX_DEVICE_QUEUE_CREATE_INFOS];
};

struct bvb_memory_metadata {
    uint64_t memory_id;
    uint64_t allocation_size;
    uint32_t property_flags;
};

struct bvb_memory_mirror_metadata {
    uint64_t device_id;
    uint64_t memory_id;
    uint64_t generation;
    uint64_t offset;
    uint64_t length;
    uint64_t allocation_size;
    uint64_t non_coherent_atom_size;
    uint32_t property_flags;
    VkDevice device;
    VkDeviceMemory memory;
    uint8_t *mirror;
    uint8_t *native;
    uint8_t *baseline;
};

struct bvb_buffer_metadata {
    uint64_t buffer_id;
    uint64_t device_id;
    uint64_t bound_memory_id;
    uint32_t usage;
    bool memory_bound;
};

struct bvb_semaphore_metadata {
    uint64_t semaphore_id;
    VkSemaphoreType type;
};

struct bvb_image_metadata {
    uint64_t image_id;
    uint64_t device_id;
    uint64_t bound_memory_id;
    uint32_t flags;
    uint32_t image_type;
    uint32_t format;
    uint32_t mip_levels;
    uint32_t array_layers;
};

struct bvb_swapchain_metadata {
    uint64_t swapchain_id;
    uint64_t device_id;
    uint64_t generation;
    VkDevice device;
    uint32_t image_count;
    uint64_t image_ids[BVB_WSI_FRAME_RING_MAX_SLOTS];
    VkImage images[BVB_WSI_FRAME_RING_MAX_SLOTS];
    VkDeviceMemory memories[BVB_WSI_FRAME_RING_MAX_SLOTS];
    void *hardware_buffers[BVB_WSI_FRAME_RING_MAX_SLOTS];
    uint64_t allocation_sizes[BVB_WSI_FRAME_RING_MAX_SLOTS];
    uint32_t memory_type_indices[BVB_WSI_FRAME_RING_MAX_SLOTS];
    uint32_t queue_family_index;
    VkQueue producer_queue;
    VkCommandPool producer_command_pool;
    VkCommandBuffer acquire_commands[BVB_WSI_FRAME_RING_MAX_SLOTS];
    VkCommandBuffer present_commands[BVB_WSI_FRAME_RING_MAX_SLOTS];
    VkFence present_completion;
    bool presented_once[BVB_WSI_FRAME_RING_MAX_SLOTS];
    int control_fd;
    struct bvb_wsi_frame_ring *control;
};

struct bvb_descriptor_template_metadata {
    uint64_t template_id;
    uint64_t device_id;
    uint32_t entry_count;
    struct bvb_vulkan_descriptor_update_template_entry
        entries[BVB_VULKAN_MAX_DESCRIPTOR_UPDATE_TEMPLATE_ENTRIES];
};

struct bvb_vulkan_global_context {
    void *loader;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    PFN_vkCreateInstance create_instance;
    PFN_vkDestroyInstance destroy_instance;
    PFN_vkGetDeviceProcAddr get_device_proc_addr;
#ifdef __ANDROID__
    void *android_library;
    bvb_ahb_is_supported_fn ahardwarebuffer_is_supported;
    bvb_ahb_allocate_fn ahardwarebuffer_allocate;
    bvb_ahb_release_fn ahardwarebuffer_release;
#endif
    struct bvb_vulkan_global_info info;
    VkExtensionProperties exposed_instance_extensions[
        BVB_EXPOSED_INSTANCE_EXTENSION_CAPACITY];
    struct bvb_handle_entry object_entries[BVB_GLOBAL_OBJECT_CAPACITY];
    struct bvb_handle_table objects;
    uint64_t next_instance_serial;
    uint64_t next_physical_device_serial;
    uint64_t next_device_serial;
    uint64_t next_queue_serial;
    uint64_t next_command_pool_serial;
    uint64_t next_command_buffer_serial;
    uint64_t next_buffer_serial;
    uint64_t next_memory_serial;
    uint64_t next_fence_serial;
    uint64_t next_semaphore_serial;
    uint64_t next_swapchain_serial;
    uint64_t next_image_serial;
    uint64_t next_descriptor_set_layout_serial;
    uint64_t next_descriptor_pool_serial;
    uint64_t next_descriptor_set_serial;
    uint64_t next_sampler_serial;
    uint64_t next_descriptor_update_template_serial;
    uint64_t next_pipeline_layout_serial;
    uint64_t next_image_view_serial;
    uint64_t next_pipeline_serial;
    struct bvb_device_metadata device_metadata[BVB_GLOBAL_OBJECT_CAPACITY];
    struct bvb_memory_metadata memory_metadata[BVB_GLOBAL_OBJECT_CAPACITY];
    struct bvb_memory_mirror_metadata
        memory_mirrors[BVB_VULKAN_MEMORY_MIRROR_CAPACITY];
    uint64_t memory_mirror_bytes;
    struct bvb_buffer_metadata buffer_metadata[BVB_GLOBAL_OBJECT_CAPACITY];
    struct bvb_semaphore_metadata
        semaphore_metadata[BVB_GLOBAL_OBJECT_CAPACITY];
    struct bvb_image_metadata image_metadata[BVB_GLOBAL_OBJECT_CAPACITY];
    struct bvb_swapchain_metadata
        swapchain_metadata[BVB_WSI_FRAME_RING_MAX_SLOTS];
    struct bvb_descriptor_template_metadata descriptor_template_metadata[
        BVB_DESCRIPTOR_TEMPLATE_METADATA_CAPACITY];
};

static int destroy_swapchain_metadata(
    struct bvb_vulkan_global_context *context,
    struct bvb_swapchain_metadata *metadata);
static int sync_coherent_memory_mirrors(
    struct bvb_vulkan_global_context *context, uint64_t device_id);
static struct bvb_memory_mirror_metadata *memory_mirror_slot(
    struct bvb_vulkan_global_context *context, uint64_t memory_id);
static bool buffer_usage_is_upload_only(uint32_t usage);
static bool memory_is_upload_only(
    const struct bvb_vulkan_global_context *context, uint64_t memory_id);

static void set_error(char *output, size_t output_size, const char *format, ...) {
    if (output == NULL || output_size == 0U) {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(output, output_size, format, arguments);
    va_end(arguments);
}

#ifdef __ANDROID__
static int ensure_android_hardware_buffer_api(
    struct bvb_vulkan_global_context *context) {
    if (context->android_library != NULL) return 0;
    void *library = dlopen(
        "/system/lib64/libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (library == NULL) return -ENOENT;
#define BVB_ANDROID_RESOLVE(member, type, name)                                \
    do {                                                                       \
        union {                                                                \
            void *object;                                                      \
            type function;                                                     \
        } conversion = {.object = dlsym(library, (name))};                     \
        context->member = conversion.function;                                 \
    } while (0)
    BVB_ANDROID_RESOLVE(
        ahardwarebuffer_is_supported,
        bvb_ahb_is_supported_fn,
        "AHardwareBuffer_isSupported");
    BVB_ANDROID_RESOLVE(
        ahardwarebuffer_allocate,
        bvb_ahb_allocate_fn,
        "AHardwareBuffer_allocate");
    BVB_ANDROID_RESOLVE(
        ahardwarebuffer_release, bvb_ahb_release_fn,
        "AHardwareBuffer_release");
#undef BVB_ANDROID_RESOLVE
    if (context->ahardwarebuffer_is_supported == NULL ||
        context->ahardwarebuffer_allocate == NULL ||
        context->ahardwarebuffer_release == NULL) {
        (void)dlclose(library);
        context->ahardwarebuffer_is_supported = NULL;
        context->ahardwarebuffer_allocate = NULL;
        context->ahardwarebuffer_release = NULL;
        return -ENOSYS;
    }
    context->android_library = library;
    return 0;
}
#endif

static void append_error_entry_point(
    char *output, size_t output_size, const char *name) {
    if (output == NULL || output_size == 0U || name == NULL) return;
    const size_t used = strnlen(output, output_size);
    if (used >= output_size) return;
    (void)snprintf(output + used, output_size - used, " %s", name);
}

static PFN_vkVoidFunction symbol_from_loader(void *loader, const char *name) {
    void *raw = dlsym(loader, name);
    PFN_vkVoidFunction function = NULL;
    if (raw != NULL) {
        memcpy(&function, &raw, sizeof(function));
    }
    return function;
}

static uint64_t handle_bits(const void *handle, size_t size) {
    uint64_t bits = 0U;
    if (handle != NULL && size <= sizeof(bits)) {
        memcpy(&bits, handle, size);
    }
    return bits;
}

static VkInstance instance_from_bits(uint64_t bits) {
    VkInstance instance = VK_NULL_HANDLE;
    _Static_assert(sizeof(instance) <= sizeof(bits),
                   "VkInstance exceeds bridge handle width");
    memcpy(&instance, &bits, sizeof(instance));
    return instance;
}

static VkPhysicalDevice physical_device_from_bits(uint64_t bits) {
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    _Static_assert(sizeof(physical_device) <= sizeof(bits),
                   "VkPhysicalDevice exceeds bridge handle width");
    memcpy(&physical_device, &bits, sizeof(physical_device));
    return physical_device;
}

static VkDevice device_from_bits(uint64_t bits) {
    VkDevice device = VK_NULL_HANDLE;
    _Static_assert(sizeof(device) <= sizeof(bits),
                   "VkDevice exceeds bridge handle width");
    memcpy(&device, &bits, sizeof(device));
    return device;
}

static VkCommandPool command_pool_from_bits(uint64_t bits) {
    VkCommandPool command_pool = VK_NULL_HANDLE;
    _Static_assert(sizeof(command_pool) <= sizeof(bits),
                   "VkCommandPool exceeds bridge handle width");
    memcpy(&command_pool, &bits, sizeof(command_pool));
    return command_pool;
}

static VkCommandBuffer command_buffer_from_bits(uint64_t bits) {
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    _Static_assert(sizeof(command_buffer) <= sizeof(bits),
                   "VkCommandBuffer exceeds bridge handle width");
    memcpy(&command_buffer, &bits, sizeof(command_buffer));
    return command_buffer;
}

static VkBuffer buffer_from_bits(uint64_t bits) {
    VkBuffer buffer = VK_NULL_HANDLE;
    _Static_assert(sizeof(buffer) <= sizeof(bits),
                   "VkBuffer exceeds bridge handle width");
    memcpy(&buffer, &bits, sizeof(buffer));
    return buffer;
}

static VkImage image_from_bits(uint64_t bits) {
    VkImage image = VK_NULL_HANDLE;
    _Static_assert(sizeof(image) <= sizeof(bits),
                   "VkImage exceeds bridge handle width");
    memcpy(&image, &bits, sizeof(image));
    return image;
}

static VkImageView image_view_from_bits(uint64_t bits) {
    VkImageView image_view = VK_NULL_HANDLE;
    _Static_assert(sizeof(image_view) <= sizeof(bits),
                   "VkImageView exceeds bridge handle width");
    memcpy(&image_view, &bits, sizeof(image_view));
    return image_view;
}

static VkDeviceMemory memory_from_bits(uint64_t bits) {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    _Static_assert(sizeof(memory) <= sizeof(bits),
                   "VkDeviceMemory exceeds bridge handle width");
    memcpy(&memory, &bits, sizeof(memory));
    return memory;
}

static VkFence fence_from_bits(uint64_t bits) {
    VkFence fence = VK_NULL_HANDLE;
    _Static_assert(sizeof(fence) <= sizeof(bits),
                   "VkFence exceeds bridge handle width");
    memcpy(&fence, &bits, sizeof(fence));
    return fence;
}

static VkSemaphore semaphore_from_bits(uint64_t bits) {
    VkSemaphore semaphore = VK_NULL_HANDLE;
    _Static_assert(sizeof(semaphore) <= sizeof(bits),
                   "VkSemaphore exceeds bridge handle width");
    memcpy(&semaphore, &bits, sizeof(semaphore));
    return semaphore;
}

#define BVB_DEFINE_HANDLE_FROM_BITS(function_name, handle_type)              \
    static handle_type function_name(uint64_t bits) {                        \
        handle_type handle = VK_NULL_HANDLE;                                 \
        _Static_assert(sizeof(handle) <= sizeof(bits),                       \
                       #handle_type " exceeds bridge handle width");         \
        memcpy(&handle, &bits, sizeof(handle));                              \
        return handle;                                                       \
    }

BVB_DEFINE_HANDLE_FROM_BITS(descriptor_set_layout_from_bits,
                            VkDescriptorSetLayout)
BVB_DEFINE_HANDLE_FROM_BITS(descriptor_pool_from_bits, VkDescriptorPool)
BVB_DEFINE_HANDLE_FROM_BITS(descriptor_set_from_bits, VkDescriptorSet)
BVB_DEFINE_HANDLE_FROM_BITS(sampler_from_bits, VkSampler)
BVB_DEFINE_HANDLE_FROM_BITS(descriptor_update_template_from_bits,
                            VkDescriptorUpdateTemplate)
BVB_DEFINE_HANDLE_FROM_BITS(pipeline_layout_from_bits, VkPipelineLayout)
BVB_DEFINE_HANDLE_FROM_BITS(pipeline_from_bits, VkPipeline)

#undef BVB_DEFINE_HANDLE_FROM_BITS

static void release_memory_mirror(
    struct bvb_vulkan_global_context *context,
    struct bvb_memory_mirror_metadata *mirror) {
    if (context == NULL || mirror == NULL || mirror->memory_id == 0U) return;
    PFN_vkUnmapMemory unmap = context->get_device_proc_addr == NULL
        ? NULL
        : (PFN_vkUnmapMemory)context->get_device_proc_addr(
              mirror->device, "vkUnmapMemory");
    if (unmap != NULL && mirror->native != NULL)
        unmap(mirror->device, mirror->memory);
    if (mirror->mirror != NULL)
        (void)munmap(mirror->mirror, (size_t)mirror->length);
    free(mirror->baseline);
    if (context->memory_mirror_bytes >= mirror->length)
        context->memory_mirror_bytes -= mirror->length;
    else
        context->memory_mirror_bytes = 0U;
    *mirror = (struct bvb_memory_mirror_metadata){0};
}

static void release_all_memory_mirrors(
    struct bvb_vulkan_global_context *context) {
    if (context == NULL) return;
    for (size_t index = 0U; index < BVB_VULKAN_MEMORY_MIRROR_CAPACITY;
         ++index)
        release_memory_mirror(context, &context->memory_mirrors[index]);
}

static int resolve_physical_device(
    const struct bvb_vulkan_global_context *context,
    uint64_t physical_device_id, VkInstance *instance,
    VkPhysicalDevice *physical_device) {
    if (context == NULL || instance == NULL || physical_device == NULL) {
        return -EINVAL;
    }
    uint64_t parent_id = 0U;
    uint64_t physical_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, physical_device_id, BVB_OBJECT_PHYSICAL_DEVICE,
        &parent_id, &physical_bits);
    uint64_t instance_bits = 0U;
    if (result == 0) {
        result = bvb_handle_table_lookup(
            &context->objects, parent_id, BVB_OBJECT_INSTANCE, NULL,
            &instance_bits);
    }
    if (result == 0) {
        *instance = instance_from_bits(instance_bits);
        *physical_device = physical_device_from_bits(physical_bits);
    }
    return result;
}

int bvb_vulkan_global_context_create(
    const char *loader_path, struct bvb_vulkan_global_context **output,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (loader_path == NULL || loader_path[0] != '/' || output == NULL) {
        set_error(error, error_size, "loader path must be absolute");
        return -EINVAL;
    }
    *output = NULL;
    struct bvb_vulkan_global_context *context = calloc(1, sizeof(*context));
    if (context == NULL) {
        set_error(error, error_size, "could not allocate global context");
        return -ENOMEM;
    }
    context->loader = dlopen(loader_path, RTLD_NOW | RTLD_LOCAL);
    if (context->loader == NULL) {
        set_error(error, error_size, "could not load %s: %s", loader_path,
                  dlerror());
        free(context);
        return -ENOENT;
    }
    context->get_instance_proc_addr =
        (PFN_vkGetInstanceProcAddr)symbol_from_loader(
            context->loader, "vkGetInstanceProcAddr");
    if (context->get_instance_proc_addr == NULL) {
        context->get_instance_proc_addr =
            (PFN_vkGetInstanceProcAddr)symbol_from_loader(
                context->loader, "vk_icdGetInstanceProcAddr");
    }
    if (context->get_instance_proc_addr == NULL) {
        set_error(error, error_size,
                  "loader has no Vulkan instance resolver");
        bvb_vulkan_global_context_destroy(context);
        return -ENOSYS;
    }
    context->get_device_proc_addr =
        (PFN_vkGetDeviceProcAddr)symbol_from_loader(
            context->loader, "vkGetDeviceProcAddr");
    if (context->get_device_proc_addr == NULL) {
        context->get_device_proc_addr =
            (PFN_vkGetDeviceProcAddr)context->get_instance_proc_addr(
                VK_NULL_HANDLE, "vkGetDeviceProcAddr");
    }
    PFN_vkEnumerateInstanceVersion enumerate_version =
        (PFN_vkEnumerateInstanceVersion)context->get_instance_proc_addr(
            VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    PFN_vkEnumerateInstanceExtensionProperties enumerate_extensions =
        (PFN_vkEnumerateInstanceExtensionProperties)
            context->get_instance_proc_addr(
            VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    PFN_vkEnumerateInstanceLayerProperties enumerate_layers =
        (PFN_vkEnumerateInstanceLayerProperties)context->get_instance_proc_addr(
            VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
    context->create_instance = (PFN_vkCreateInstance)
        context->get_instance_proc_addr(
        VK_NULL_HANDLE, "vkCreateInstance");
    if (enumerate_extensions == NULL || enumerate_layers == NULL ||
        context->create_instance == NULL) {
        set_error(error, error_size, "loader lacks required global functions");
        bvb_vulkan_global_context_destroy(context);
        return -ENOSYS;
    }

    context->info.loader_api_version = VK_API_VERSION_1_0;
    VkResult result = VK_SUCCESS;
    if (enumerate_version != NULL) {
        result = enumerate_version(&context->info.loader_api_version);
    }
    if (result == VK_SUCCESS) {
        result = enumerate_extensions(
            NULL, &context->info.native_extension_count, NULL);
    }
    if (result == VK_SUCCESS && context->info.native_extension_count != 0U) {
        const uint32_t available = context->info.native_extension_count;
        VkExtensionProperties *extensions =
            calloc(available, sizeof(*extensions));
        if (extensions == NULL) {
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
        } else {
            uint32_t returned = available;
            result = enumerate_extensions(NULL, &returned, extensions);
            if (result == VK_SUCCESS) {
                for (uint32_t allowed = 0U;
                     allowed < BVB_EXPOSED_INSTANCE_EXTENSION_CAPACITY;
                     ++allowed) {
                    for (uint32_t index = 0U; index < returned; ++index) {
                        if (strcmp(
                                extensions[index].extensionName,
                                bvb_instance_extension_allowlist[allowed]) ==
                            0) {
                            context->exposed_instance_extensions[
                                context->info.exposed_extension_count++] =
                                extensions[index];
                            break;
                        }
                    }
                }
            }
            free(extensions);
        }
    }
    if (result == VK_SUCCESS) {
        result = enumerate_layers(&context->info.native_layer_count, NULL);
    }
    if (result != VK_SUCCESS || context->info.loader_api_version == 0U) {
        set_error(error, error_size,
                  "global Vulkan enumeration failed: %d", (int)result);
        bvb_vulkan_global_context_destroy(context);
        return -EIO;
    }
    context->info.exposed_layer_count = 0U;
    int status = bvb_handle_table_init(
        &context->objects, context->object_entries,
        BVB_GLOBAL_OBJECT_CAPACITY);
    if (status != 0) {
        set_error(error, error_size, "instance table init failed: %d", status);
        bvb_vulkan_global_context_destroy(context);
        return status;
    }
    context->next_instance_serial = 1U;
    context->next_physical_device_serial = 1U;
    context->next_device_serial = 1U;
    context->next_queue_serial = 1U;
    context->next_command_pool_serial = 1U;
    context->next_command_buffer_serial = 1U;
    context->next_buffer_serial = 1U;
    context->next_memory_serial = 1U;
    context->next_fence_serial = 1U;
    context->next_semaphore_serial = 1U;
    context->next_swapchain_serial = 1U;
    context->next_image_serial = 1U;
    context->next_descriptor_set_layout_serial = 1U;
    context->next_descriptor_pool_serial = 1U;
    context->next_descriptor_set_serial = 1U;
    context->next_sampler_serial = 1U;
    context->next_descriptor_update_template_serial = 1U;
    context->next_pipeline_layout_serial = 1U;
    context->next_image_view_serial = 1U;
    context->next_pipeline_serial = 1U;
    *output = context;
    return 0;
}

void bvb_vulkan_global_context_destroy(
    struct bvb_vulkan_global_context *context) {
    if (context == NULL) {
        return;
    }
    release_all_memory_mirrors(context);
    for (size_t index = 0U; index < BVB_WSI_FRAME_RING_MAX_SLOTS; ++index) {
        if (context->swapchain_metadata[index].swapchain_id != 0U) {
            (void)destroy_swapchain_metadata(
                context, &context->swapchain_metadata[index]);
        }
    }
    if (context->get_device_proc_addr != NULL) {
        for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
            const struct bvb_handle_entry *entry =
                &context->object_entries[index];
            if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_DEVICE &&
                entry->native_bits != 0U) {
                const VkDevice device = device_from_bits(entry->native_bits);
                PFN_vkDestroyDevice destroy_device =
                    (PFN_vkDestroyDevice)context->get_device_proc_addr(
                        device, "vkDestroyDevice");
                if (destroy_device != NULL) {
                    destroy_device(device, NULL);
                }
            }
        }
    }
    if (context->destroy_instance != NULL) {
        for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
            const struct bvb_handle_entry *entry =
                &context->object_entries[index];
            if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_INSTANCE &&
                entry->native_bits != 0U) {
                context->destroy_instance(
                    instance_from_bits(entry->native_bits), NULL);
            }
        }
    }
    /*
     * Keep the Vulkan implementation resident until process exit. Mesa can
     * install pthread TLS destructors while servicing this connection. The
     * connection worker exits after this context is destroyed, so dlclose()
     * here would unmap those destructor callbacks before libc invokes them
     * from pthread_key_clean_all(). Device and instance objects above are
     * still destroyed at connection scope; only the loader mapping has
     * process lifetime.
     */
    context->loader = NULL;
#ifdef __ANDROID__
    if (context->android_library != NULL) {
        (void)dlclose(context->android_library);
    }
#endif
    free(context);
}

int bvb_vulkan_global_context_info(
    const struct bvb_vulkan_global_context *context,
    struct bvb_vulkan_global_info *info) {
    if (context == NULL || info == NULL) {
        return -EINVAL;
    }
    *info = context->info;
    return 0;
}

int bvb_vulkan_global_context_enumerate_instance_extensions(
    const struct bvb_vulkan_global_context *context,
    struct bvb_vulkan_extension_page *page) {
    if (context == NULL || page == NULL) {
        return -EINVAL;
    }
    *page = (struct bvb_vulkan_extension_page){
        .vulkan_result = VK_SUCCESS,
        .total_count = context->info.exposed_extension_count,
        .count = context->info.exposed_extension_count,
    };
    for (uint32_t index = 0U; index < page->count; ++index) {
        page->properties[index] =
            context->exposed_instance_extensions[index];
    }
    return 0;
}

int bvb_vulkan_global_context_create_instance(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_instance_create_request *request,
    const char *const *enabled_extensions,
    struct bvb_vulkan_instance_create_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (context == NULL || request == NULL || response == NULL) {
        return -EINVAL;
    }
    *response = (struct bvb_vulkan_instance_create_response){0};
    if (request->flags != 0U) {
        response->vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
        return 0;
    }
    if (request->enabled_layer_count != 0U) {
        response->vulkan_result = VK_ERROR_LAYER_NOT_PRESENT;
        return 0;
    }
    if (request->enabled_extension_count >
            BVB_EXPOSED_INSTANCE_EXTENSION_CAPACITY ||
        (request->enabled_extension_count != 0U &&
         enabled_extensions == NULL)) {
        response->vulkan_result = VK_ERROR_EXTENSION_NOT_PRESENT;
        return 0;
    }
    for (uint32_t index = 0U;
         index < request->enabled_extension_count; ++index) {
        bool supported = false;
        for (uint32_t exposed = 0U;
             exposed < context->info.exposed_extension_count; ++exposed) {
            if (strcmp(
                    enabled_extensions[index],
                    context->exposed_instance_extensions[exposed]
                        .extensionName) == 0) {
                supported = true;
                break;
            }
        }
        if (!supported) {
            response->vulkan_result = VK_ERROR_EXTENSION_NOT_PRESENT;
            return 0;
        }
    }
    if (context->objects.count == context->objects.capacity) {
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    const VkApplicationInfo application_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "bionic-vulkan-bridge",
        .applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .pEngineName = "bvb-global-dispatch",
        .engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .apiVersion = request->api_version,
    };
    const VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application_info,
        .enabledExtensionCount = request->enabled_extension_count,
        .ppEnabledExtensionNames = enabled_extensions,
    };
    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = context->create_instance(&create_info, NULL, &instance);
    if (result != VK_SUCCESS) {
        response->vulkan_result = result;
        return -EIO;
    }
    if (context->destroy_instance == NULL) {
        context->destroy_instance =
            (PFN_vkDestroyInstance)context->get_instance_proc_addr(
                instance, "vkDestroyInstance");
        if (context->destroy_instance == NULL) {
            set_error(error, error_size,
                      "created instance has no vkDestroyInstance");
            response->vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
            return 0;
        }
    }
    if (context->get_device_proc_addr == NULL) {
        context->get_device_proc_addr =
            (PFN_vkGetDeviceProcAddr)context->get_instance_proc_addr(
                instance, "vkGetDeviceProcAddr");
        if (context->get_device_proc_addr == NULL) {
            context->destroy_instance(instance, NULL);
            set_error(error, error_size,
                      "created instance has no vkGetDeviceProcAddr");
            response->vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
            return 0;
        }
    }
    const uint64_t native_bits = handle_bits(&instance, sizeof(instance));
    const uint64_t wire_id = bvb_handle_id(
        BVB_OBJECT_INSTANCE, context->next_instance_serial++);
    int status = bvb_handle_table_insert(
        &context->objects, wire_id, 0U, native_bits);
    if (status != 0) {
        context->destroy_instance(instance, NULL);
        set_error(error, error_size, "instance ownership failed: %d", status);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    response->vulkan_result = VK_SUCCESS;
    response->instance_id = wire_id;
    return 0;
}

int bvb_vulkan_global_context_destroy_instance(
    struct bvb_vulkan_global_context *context, uint64_t instance_id) {
    if (context == NULL || context->destroy_instance == NULL) {
        return -EINVAL;
    }
    uint64_t native_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, instance_id, BVB_OBJECT_INSTANCE, NULL,
        &native_bits);
    if (result != 0) {
        return result;
    }
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) != BVB_OBJECT_DEVICE) {
            continue;
        }
        uint64_t physical_bits = 0U;
        uint64_t physical_parent = 0U;
        result = bvb_handle_table_lookup(
            &context->objects, entry->parent_id, BVB_OBJECT_PHYSICAL_DEVICE,
            &physical_parent, &physical_bits);
        if (result == 0 && physical_parent == instance_id) {
            result = bvb_vulkan_global_context_destroy_device(
                context, entry->wire_id);
            if (result != 0) {
                return result;
            }
        }
    }
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_PHYSICAL_DEVICE &&
            entry->parent_id == instance_id) {
            const uint64_t child_id = entry->wire_id;
            result = bvb_handle_table_remove(
                &context->objects, child_id, BVB_OBJECT_PHYSICAL_DEVICE, NULL);
            if (result != 0) {
                return result;
            }
        }
    }
    result = bvb_handle_table_remove(
        &context->objects, instance_id, BVB_OBJECT_INSTANCE, &native_bits);
    if (result != 0) {
        return result;
    }
    context->destroy_instance(instance_from_bits(native_bits), NULL);
    return 0;
}

static uint64_t existing_physical_device_id(
    const struct bvb_vulkan_global_context *context, uint64_t instance_id,
    uint64_t native_bits) {
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_PHYSICAL_DEVICE &&
            entry->parent_id == instance_id &&
            entry->native_bits == native_bits) {
            return entry->wire_id;
        }
    }
    return 0U;
}

int bvb_vulkan_global_context_enumerate_physical_devices(
    struct bvb_vulkan_global_context *context, uint64_t instance_id,
    struct bvb_vulkan_physical_devices *devices,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (context == NULL || devices == NULL) {
        return -EINVAL;
    }
    *devices = (struct bvb_vulkan_physical_devices){0};
    uint64_t instance_bits = 0U;
    int status = bvb_handle_table_lookup(
        &context->objects, instance_id, BVB_OBJECT_INSTANCE, NULL,
        &instance_bits);
    if (status != 0) {
        return status;
    }
    const VkInstance instance = instance_from_bits(instance_bits);
    PFN_vkEnumeratePhysicalDevices enumerate_devices =
        (PFN_vkEnumeratePhysicalDevices)context->get_instance_proc_addr(
            instance, "vkEnumeratePhysicalDevices");
    if (enumerate_devices == NULL) {
        set_error(error, error_size,
                  "created instance has no vkEnumeratePhysicalDevices");
        return -ENOSYS;
    }
    uint32_t count = 0U;
    VkResult result = enumerate_devices(instance, &count, NULL);
    if (result != VK_SUCCESS) {
        devices->vulkan_result = result;
        return -EIO;
    }
    if (count > BVB_VULKAN_MAX_PHYSICAL_DEVICES) {
        set_error(error, error_size,
                  "physical-device count exceeds bridge bound: %u", count);
        return -EOVERFLOW;
    }
    VkPhysicalDevice native_devices[BVB_VULKAN_MAX_PHYSICAL_DEVICES] = {0};
    uint32_t returned = count;
    if (count != 0U) {
        result = enumerate_devices(instance, &returned, native_devices);
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            devices->vulkan_result = result;
            return 0;
        }
        if (returned > count) {
            return -EPROTO;
        }
    }
    devices->vulkan_result = result;
    devices->count = returned;
    for (uint32_t index = 0U; index < returned; ++index) {
        const uint64_t native_bits =
            handle_bits(&native_devices[index], sizeof(native_devices[index]));
        uint64_t wire_id = existing_physical_device_id(
            context, instance_id, native_bits);
        if (wire_id == 0U) {
            wire_id = bvb_handle_id(
                BVB_OBJECT_PHYSICAL_DEVICE,
                context->next_physical_device_serial++);
            status = bvb_handle_table_insert(
                &context->objects, wire_id, instance_id, native_bits);
            if (status != 0) {
                set_error(error, error_size,
                          "physical-device ownership failed: %d", status);
                return status;
            }
        }
        devices->ids[index] = wire_id;
    }
    return 0;
}

int bvb_vulkan_global_context_get_physical_device_properties(
    const struct bvb_vulkan_global_context *context,
    uint64_t physical_device_id, VkPhysicalDeviceProperties *properties,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (properties == NULL) {
        return -EINVAL;
    }
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    int result = resolve_physical_device(
        context, physical_device_id, &instance, &physical_device);
    if (result != 0) {
        set_error(error, error_size, "unknown physical-device handle");
        return result;
    }
    PFN_vkGetPhysicalDeviceProperties get_properties =
        (PFN_vkGetPhysicalDeviceProperties)context->get_instance_proc_addr(
            instance, "vkGetPhysicalDeviceProperties");
    if (get_properties == NULL) {
        set_error(error, error_size,
                  "instance has no vkGetPhysicalDeviceProperties");
        return -ENOSYS;
    }
    memset(properties, 0, sizeof(*properties));
    get_properties(physical_device, properties);
    if (memchr(properties->deviceName, '\0',
               sizeof(properties->deviceName)) == NULL) {
        set_error(error, error_size, "physical-device name is unterminated");
        return -EPROTO;
    }
    return 0;
}

int bvb_vulkan_global_context_get_queue_family_properties(
    const struct bvb_vulkan_global_context *context,
    uint64_t physical_device_id,
    VkQueueFamilyProperties properties[BVB_VULKAN_MAX_QUEUE_FAMILIES],
    uint32_t *count, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (properties == NULL || count == NULL) {
        return -EINVAL;
    }
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    int result = resolve_physical_device(
        context, physical_device_id, &instance, &physical_device);
    if (result != 0) {
        set_error(error, error_size, "unknown physical-device handle");
        return result;
    }
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_properties =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
            context->get_instance_proc_addr(
                instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (get_properties == NULL) {
        set_error(error, error_size,
                  "instance has no vkGetPhysicalDeviceQueueFamilyProperties");
        return -ENOSYS;
    }
    uint32_t available = 0U;
    get_properties(physical_device, &available, NULL);
    if (available > BVB_VULKAN_MAX_QUEUE_FAMILIES) {
        set_error(error, error_size,
                  "queue-family count exceeds bridge bound: %u", available);
        return -EOVERFLOW;
    }
    memset(properties, 0, sizeof(*properties) * BVB_VULKAN_MAX_QUEUE_FAMILIES);
    uint32_t returned = available;
    if (available != 0U) {
        get_properties(physical_device, &returned, properties);
    }
    if (returned > available) {
        set_error(error, error_size, "queue-family count changed unexpectedly");
        return -EPROTO;
    }
    *count = returned;
    return 0;
}

int bvb_vulkan_global_context_get_memory_properties(
    const struct bvb_vulkan_global_context *context,
    uint64_t physical_device_id,
    VkPhysicalDeviceMemoryProperties *properties,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (properties == NULL) {
        return -EINVAL;
    }
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    int result = resolve_physical_device(
        context, physical_device_id, &instance, &physical_device);
    if (result != 0) {
        set_error(error, error_size, "unknown physical-device handle");
        return result;
    }
    PFN_vkGetPhysicalDeviceMemoryProperties get_properties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)
            context->get_instance_proc_addr(
                instance, "vkGetPhysicalDeviceMemoryProperties");
    if (get_properties == NULL) {
        set_error(error, error_size,
                  "instance has no vkGetPhysicalDeviceMemoryProperties");
        return -ENOSYS;
    }
    memset(properties, 0, sizeof(*properties));
    get_properties(physical_device, properties);
    if (properties->memoryTypeCount > VK_MAX_MEMORY_TYPES ||
        properties->memoryHeapCount > VK_MAX_MEMORY_HEAPS) {
        set_error(error, error_size, "invalid physical-device memory counts");
        return -EPROTO;
    }
    return 0;
}

int bvb_vulkan_global_context_enumerate_device_extensions(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_device_extension_query *query,
    struct bvb_vulkan_extension_page *page,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (query == NULL || page == NULL ||
        query->max_count > BVB_VULKAN_EXTENSION_PAGE_CAPACITY) {
        return -EINVAL;
    }
    *page = (struct bvb_vulkan_extension_page){0};
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    int status = resolve_physical_device(
        context, query->physical_device_id, &instance, &physical_device);
    if (status != 0) {
        set_error(error, error_size, "unknown physical-device handle");
        return status;
    }
    PFN_vkEnumerateDeviceExtensionProperties enumerate =
        (PFN_vkEnumerateDeviceExtensionProperties)
            context->get_instance_proc_addr(
                instance, "vkEnumerateDeviceExtensionProperties");
    if (enumerate == NULL) {
        set_error(error, error_size,
                  "instance has no vkEnumerateDeviceExtensionProperties");
        return -ENOSYS;
    }
    uint32_t available = 0U;
    VkResult result = enumerate(physical_device, NULL, &available, NULL);
    page->vulkan_result = result;
    page->total_count = available;
    page->first = query->first;
    if (result != VK_SUCCESS || query->max_count == 0U) {
        return 0;
    }
    if (available > BVB_VULKAN_MAX_DEVICE_EXTENSIONS) {
        set_error(error, error_size,
                  "device-extension count exceeds bridge bound: %u",
                  available);
        return -EOVERFLOW;
    }
    if (query->first > available) {
        set_error(error, error_size,
                  "device-extension page starts beyond available records");
        return -ERANGE;
    }
    VkExtensionProperties *all = NULL;
    if (available != 0U) {
        all = calloc(available, sizeof(*all));
        if (all == NULL) {
            return -ENOMEM;
        }
    }
    uint32_t returned = available;
    if (available != 0U) {
        result = enumerate(physical_device, NULL, &returned, all);
    }
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) ||
        returned > available || query->first > returned) {
        free(all);
        page->vulkan_result = result;
        set_error(error, error_size,
                  "device-extension list changed unexpectedly: %d",
                  (int)result);
        return result == VK_SUCCESS ? -EPROTO : 0;
    }
    if (getenv("BVB_ICD_DIAGNOSTICS") != NULL) {
        fprintf(stderr,
                "BVB_ICD_DEVICE_EXTENSIONS result=%d count=%u\n",
                (int)result, returned);
        for (uint32_t index = 0U; index < returned; ++index) {
            fprintf(stderr,
                    "BVB_ICD_DEVICE_EXTENSION index=%u name=%s spec=%u\n",
                    index, all[index].extensionName,
                    all[index].specVersion);
        }
    }
    page->vulkan_result = result;
    page->total_count = returned;
    const uint32_t remaining = returned - query->first;
    page->count = query->max_count < remaining ? query->max_count : remaining;
    if (page->count != 0U) {
        memcpy(page->properties, all + query->first,
               page->count * sizeof(*page->properties));
    }
    free(all);
    return 0;
}

int bvb_vulkan_global_context_get_physical_device_features(
    const struct bvb_vulkan_global_context *context,
    uint64_t physical_device_id, VkPhysicalDeviceFeatures *features,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (features == NULL) {
        return -EINVAL;
    }
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    int result = resolve_physical_device(
        context, physical_device_id, &instance, &physical_device);
    if (result != 0) {
        set_error(error, error_size, "unknown physical-device handle");
        return result;
    }
    PFN_vkGetPhysicalDeviceFeatures get_features =
        (PFN_vkGetPhysicalDeviceFeatures)context->get_instance_proc_addr(
            instance, "vkGetPhysicalDeviceFeatures");
    if (get_features == NULL) {
        set_error(error, error_size,
                  "instance has no vkGetPhysicalDeviceFeatures");
        return -ENOSYS;
    }
    memset(features, 0, sizeof(*features));
    get_features(physical_device, features);
    return 0;
}

int bvb_vulkan_global_context_get_core_features(
    const struct bvb_vulkan_global_context *context,
    uint64_t physical_device_id,
    struct bvb_vulkan_core_features *features,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (features == NULL) {
        return -EINVAL;
    }
    *features = (struct bvb_vulkan_core_features){0};
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    int result = resolve_physical_device(
        context, physical_device_id, &instance, &physical_device);
    if (result != 0) {
        set_error(error, error_size, "unknown physical-device handle");
        return result;
    }
    PFN_vkGetPhysicalDeviceFeatures2 get_features2 =
        (PFN_vkGetPhysicalDeviceFeatures2)context->get_instance_proc_addr(
            instance, "vkGetPhysicalDeviceFeatures2");
    if (get_features2 == NULL) {
        get_features2 = (PFN_vkGetPhysicalDeviceFeatures2)
            context->get_instance_proc_addr(
                instance, "vkGetPhysicalDeviceFeatures2KHR");
    }
    if (get_features2 == NULL) {
        set_error(error, error_size,
                  "instance has no vkGetPhysicalDeviceFeatures2");
        return -ENOSYS;
    }
    VkPhysicalDeviceVulkan11Features vulkan11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
    };
    VkPhysicalDeviceVulkan12Features vulkan12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
    };
    VkPhysicalDeviceVulkan13Features vulkan13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
    };
    VkPhysicalDeviceDepthClipEnableFeaturesEXT depth_clip = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT,
    };
    VkPhysicalDeviceRobustness2FeaturesEXT robustness2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
    };
    VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5 = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR,
    };
    VkPhysicalDeviceMaintenance6FeaturesKHR maintenance6 = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR,
    };
    vulkan11.pNext = &vulkan12;
    vulkan12.pNext = &vulkan13;
    vulkan13.pNext = &depth_clip;
    depth_clip.pNext = &robustness2;
    robustness2.pNext = &maintenance5;
    maintenance5.pNext = &maintenance6;
    VkPhysicalDeviceFeatures2 base = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &vulkan11,
    };
    get_features2(physical_device, &base);
    features->shader_draw_parameters =
        vulkan11.shaderDrawParameters == VK_TRUE ? 1U : 0U;
    features->buffer_device_address =
        vulkan12.bufferDeviceAddress == VK_TRUE ? 1U : 0U;
    features->descriptor_indexing =
        vulkan12.descriptorIndexing == VK_TRUE ? 1U : 0U;
    features->descriptor_binding_sampled_image_update_after_bind =
        vulkan12.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE
            ? 1U
            : 0U;
    features->descriptor_binding_update_unused_while_pending =
        vulkan12.descriptorBindingUpdateUnusedWhilePending == VK_TRUE ? 1U
                                                                      : 0U;
    features->descriptor_binding_partially_bound =
        vulkan12.descriptorBindingPartiallyBound == VK_TRUE ? 1U : 0U;
    features->host_query_reset =
        vulkan12.hostQueryReset == VK_TRUE ? 1U : 0U;
    features->runtime_descriptor_array =
        vulkan12.runtimeDescriptorArray == VK_TRUE ? 1U : 0U;
    features->sampler_mirror_clamp_to_edge =
        vulkan12.samplerMirrorClampToEdge == VK_TRUE ? 1U : 0U;
    features->scalar_block_layout =
        vulkan12.scalarBlockLayout == VK_TRUE ? 1U : 0U;
    features->timeline_semaphore =
        vulkan12.timelineSemaphore == VK_TRUE ? 1U : 0U;
    features->uniform_buffer_standard_layout =
        vulkan12.uniformBufferStandardLayout == VK_TRUE ? 1U : 0U;
    features->vulkan_memory_model =
        vulkan12.vulkanMemoryModel == VK_TRUE ? 1U : 0U;
    features->compute_full_subgroups =
        vulkan13.computeFullSubgroups == VK_TRUE ? 1U : 0U;
    features->dynamic_rendering =
        vulkan13.dynamicRendering == VK_TRUE ? 1U : 0U;
    features->maintenance4 =
        vulkan13.maintenance4 == VK_TRUE ? 1U : 0U;
    features->shader_demote_to_helper_invocation =
        vulkan13.shaderDemoteToHelperInvocation == VK_TRUE ? 1U : 0U;
    features->shader_zero_initialize_workgroup_memory =
        vulkan13.shaderZeroInitializeWorkgroupMemory == VK_TRUE ? 1U : 0U;
    features->subgroup_size_control =
        vulkan13.subgroupSizeControl == VK_TRUE ? 1U : 0U;
    features->synchronization2 =
        vulkan13.synchronization2 == VK_TRUE ? 1U : 0U;
    features->depth_clip_enable =
        depth_clip.depthClipEnable == VK_TRUE ? 1U : 0U;
    features->robust_buffer_access2 =
        robustness2.robustBufferAccess2 == VK_TRUE ? 1U : 0U;
    features->null_descriptor =
        robustness2.nullDescriptor == VK_TRUE ? 1U : 0U;
    features->maintenance5 =
        maintenance5.maintenance5 == VK_TRUE ? 1U : 0U;
    features->maintenance6 =
        maintenance6.maintenance6 == VK_TRUE ? 1U : 0U;
    return 0;
}

int bvb_vulkan_global_context_get_format_properties(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_format_query *query,
    struct bvb_vulkan_format_properties *properties,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (query == NULL || properties == NULL) {
        return -EINVAL;
    }
    *properties = (struct bvb_vulkan_format_properties){0};
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    int result = resolve_physical_device(
        context, query->physical_device_id, &instance, &physical_device);
    if (result != 0) {
        set_error(error, error_size, "unknown physical-device handle");
        return result;
    }
    PFN_vkGetPhysicalDeviceFormatProperties get_properties =
        (PFN_vkGetPhysicalDeviceFormatProperties)
            context->get_instance_proc_addr(
                instance, "vkGetPhysicalDeviceFormatProperties");
    if (get_properties == NULL) {
        set_error(error, error_size,
                  "instance has no vkGetPhysicalDeviceFormatProperties");
        return -ENOSYS;
    }
    VkFormatProperties native = {0};
    get_properties(physical_device, (VkFormat)query->format, &native);
    properties->linear_tiling_features = native.linearTilingFeatures;
    properties->optimal_tiling_features = native.optimalTilingFeatures;
    properties->buffer_features = native.bufferFeatures;
    return 0;
}

int bvb_vulkan_global_context_get_format_properties_3(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_format_query *query,
    struct bvb_vulkan_format_properties_3 *properties,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (query == NULL || properties == NULL) return -EINVAL;
    *properties = (struct bvb_vulkan_format_properties_3){0};
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    int result = resolve_physical_device(
        context, query->physical_device_id, &instance, &physical_device);
    if (result != 0) {
        set_error(error, error_size, "unknown physical-device handle");
        return result;
    }
    PFN_vkGetPhysicalDeviceFormatProperties2 get_properties_2 =
        (PFN_vkGetPhysicalDeviceFormatProperties2)
            context->get_instance_proc_addr(
                instance, "vkGetPhysicalDeviceFormatProperties2");
    if (get_properties_2 == NULL) {
        set_error(error, error_size,
                  "instance has no vkGetPhysicalDeviceFormatProperties2");
        return -ENOSYS;
    }
    VkFormatProperties3 native_3 = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3,
    };
    VkFormatProperties2 native_2 = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &native_3,
    };
    get_properties_2(physical_device, (VkFormat)query->format, &native_2);
    properties->linear_tiling_features = native_3.linearTilingFeatures;
    properties->optimal_tiling_features = native_3.optimalTilingFeatures;
    properties->buffer_features = native_3.bufferFeatures;
    return 0;
}

int bvb_vulkan_global_context_get_image_format_properties(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_image_format_query *query,
    struct bvb_vulkan_image_format_properties *properties,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (query == NULL || properties == NULL) {
        return -EINVAL;
    }
    *properties = (struct bvb_vulkan_image_format_properties){0};
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    int result = resolve_physical_device(
        context, query->physical_device_id, &instance, &physical_device);
    if (result != 0) {
        set_error(error, error_size, "unknown physical-device handle");
        return result;
    }
    PFN_vkGetPhysicalDeviceImageFormatProperties get_properties =
        (PFN_vkGetPhysicalDeviceImageFormatProperties)
            context->get_instance_proc_addr(
                instance, "vkGetPhysicalDeviceImageFormatProperties");
    if (get_properties == NULL) {
        set_error(
            error, error_size,
            "instance has no vkGetPhysicalDeviceImageFormatProperties");
        return -ENOSYS;
    }
    VkImageFormatProperties native = {0};
    const VkResult vulkan_result = get_properties(
        physical_device, (VkFormat)query->format, (VkImageType)query->type,
        (VkImageTiling)query->tiling, (VkImageUsageFlags)query->usage,
        (VkImageCreateFlags)query->flags, &native);
    properties->vulkan_result = vulkan_result;
    if (vulkan_result == VK_SUCCESS) {
        properties->max_extent_width = native.maxExtent.width;
        properties->max_extent_height = native.maxExtent.height;
        properties->max_extent_depth = native.maxExtent.depth;
        properties->max_mip_levels = native.maxMipLevels;
        properties->max_array_layers = native.maxArrayLayers;
        properties->sample_counts = native.sampleCounts;
        properties->max_resource_size = native.maxResourceSize;
    }
    return 0;
}

int bvb_vulkan_global_context_get_external_buffer_properties(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_external_buffer_query *query,
    struct bvb_vulkan_external_buffer_properties *properties,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (query == NULL || properties == NULL) {
        return -EINVAL;
    }
    *properties = (struct bvb_vulkan_external_buffer_properties){0};
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    int result = resolve_physical_device(
        context, query->physical_device_id, &instance, &physical_device);
    if (result != 0) {
        set_error(error, error_size, "unknown physical-device handle");
        return result;
    }
    PFN_vkGetPhysicalDeviceExternalBufferProperties get_properties =
        (PFN_vkGetPhysicalDeviceExternalBufferProperties)
            context->get_instance_proc_addr(
                instance, "vkGetPhysicalDeviceExternalBufferProperties");
    if (get_properties == NULL) {
        get_properties = (PFN_vkGetPhysicalDeviceExternalBufferProperties)
            context->get_instance_proc_addr(
                instance,
                "vkGetPhysicalDeviceExternalBufferPropertiesKHR");
    }
    if (get_properties == NULL) {
        set_error(error, error_size,
                  "instance has no external-buffer capability query");
        return -ENOSYS;
    }
    const VkPhysicalDeviceExternalBufferInfo info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .flags = (VkBufferCreateFlags)query->flags,
        .usage = (VkBufferUsageFlags)query->usage,
        .handleType = (VkExternalMemoryHandleTypeFlagBits)query->handle_type,
    };
    VkExternalBufferProperties native = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES,
    };
    get_properties(physical_device, &info, &native);
    properties->external_memory_features =
        native.externalMemoryProperties.externalMemoryFeatures;
    properties->export_from_imported_handle_types =
        native.externalMemoryProperties.exportFromImportedHandleTypes;
    properties->compatible_handle_types =
        native.externalMemoryProperties.compatibleHandleTypes;
    return 0;
}

int bvb_vulkan_global_context_get_external_semaphore_properties(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_external_semaphore_query *query,
    struct bvb_vulkan_external_semaphore_properties *properties,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (query == NULL || properties == NULL) {
        return -EINVAL;
    }
    *properties = (struct bvb_vulkan_external_semaphore_properties){0};
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    int result = resolve_physical_device(
        context, query->physical_device_id, &instance, &physical_device);
    if (result != 0) {
        set_error(error, error_size, "unknown physical-device handle");
        return result;
    }
    PFN_vkGetPhysicalDeviceExternalSemaphoreProperties get_properties =
        (PFN_vkGetPhysicalDeviceExternalSemaphoreProperties)
            context->get_instance_proc_addr(
                instance,
                "vkGetPhysicalDeviceExternalSemaphoreProperties");
    if (get_properties == NULL) {
        get_properties =
            (PFN_vkGetPhysicalDeviceExternalSemaphoreProperties)
                context->get_instance_proc_addr(
                    instance,
                    "vkGetPhysicalDeviceExternalSemaphorePropertiesKHR");
    }
    if (get_properties == NULL) {
        set_error(error, error_size,
                  "instance has no external-semaphore capability query");
        return -ENOSYS;
    }
    const VkPhysicalDeviceExternalSemaphoreInfo info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
        .handleType =
            (VkExternalSemaphoreHandleTypeFlagBits)query->handle_type,
    };
    VkExternalSemaphoreProperties native = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
    };
    get_properties(physical_device, &info, &native);
    properties->export_from_imported_handle_types =
        native.exportFromImportedHandleTypes;
    properties->compatible_handle_types = native.compatibleHandleTypes;
    properties->external_semaphore_features =
        native.externalSemaphoreFeatures;
    return 0;
}

static struct bvb_device_metadata *device_metadata_slot(
    struct bvb_vulkan_global_context *context, uint64_t device_id) {
    struct bvb_device_metadata *empty = NULL;
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        struct bvb_device_metadata *metadata = &context->device_metadata[index];
        if (metadata->device_id == device_id) {
            return metadata;
        }
        if (metadata->device_id == 0U && empty == NULL) {
            empty = metadata;
        }
    }
    return empty;
}

static bool buffer_usage_is_upload_only(uint32_t usage) {
    const uint32_t gpu_read_only =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    return usage != 0U && (usage & ~gpu_read_only) == 0U;
}

static bool memory_is_upload_only(
    const struct bvb_vulkan_global_context *context, uint64_t memory_id) {
    bool found = false;
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_buffer_metadata *buffer =
            &context->buffer_metadata[index];
        if (buffer->bound_memory_id == memory_id) {
            if (!buffer_usage_is_upload_only(buffer->usage)) return false;
            found = true;
        }
        if (context->image_metadata[index].bound_memory_id == memory_id)
            return false;
    }
    return found;
}

int bvb_vulkan_global_context_create_device_packed(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_device_create_packed_request *request,
    const char *const *enabled_extensions,
    struct bvb_vulkan_device_create_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (context == NULL || request == NULL || response == NULL) {
        return -EINVAL;
    }
    *response = (struct bvb_vulkan_device_create_response){0};
    if (request->flags != 0U || request->queue_create_info_count == 0U ||
        request->queue_create_info_count >
            BVB_VULKAN_MAX_DEVICE_QUEUE_CREATE_INFOS ||
        request->queue_priority_count == 0U ||
        request->queue_priority_count >
            BVB_VULKAN_MAX_DEVICE_QUEUE_PRIORITIES ||
        request->enabled_layer_count != 0U ||
        (request->enabled_extension_count != 0U &&
         enabled_extensions == NULL) ||
        request->enabled_extension_count >
            BVB_VULKAN_MAX_DEVICE_CREATE_EXTENSIONS ||
        (request->enabled_feature_structs &
         ~BVB_VULKAN_DEVICE_FEATURE_STRUCT_MASK) != 0U) {
        response->vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
        return 0;
    }
    float queue_priorities[BVB_VULKAN_MAX_DEVICE_QUEUE_PRIORITIES] = {0};
    for (uint32_t index = 0U; index < request->queue_priority_count; ++index) {
        memcpy(&queue_priorities[index], &request->queue_priority_bits[index],
               sizeof(queue_priorities[index]));
        if (!(queue_priorities[index] >= 0.0F &&
              queue_priorities[index] <= 1.0F)) {
            response->vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
            return 0;
        }
    }
    if (context->objects.count == context->objects.capacity) {
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    int status = resolve_physical_device(
        context, request->physical_device_id, &instance, &physical_device);
    if (status != 0) {
        return status;
    }
    VkPhysicalDeviceProperties physical_properties = {0};
    status = bvb_vulkan_global_context_get_physical_device_properties(
        context, request->physical_device_id, &physical_properties,
        error, error_size);
    if (status != 0 || physical_properties.limits.nonCoherentAtomSize == 0U) {
        return status != 0 ? status : -EPROTO;
    }
    VkQueueFamilyProperties queue_properties[BVB_VULKAN_MAX_QUEUE_FAMILIES];
    uint32_t queue_family_count = 0U;
    status = bvb_vulkan_global_context_get_queue_family_properties(
        context, request->physical_device_id, queue_properties,
        &queue_family_count, error, error_size);
    if (status != 0) {
        return status;
    }
    VkDeviceQueueCreateInfo
        queue_infos[BVB_VULKAN_MAX_DEVICE_QUEUE_CREATE_INFOS] = {0};
    for (uint32_t index = 0U;
         index < request->queue_create_info_count; ++index) {
        const struct bvb_vulkan_device_queue_create_info *wire_info =
            &request->queue_create_infos[index];
        if (wire_info->flags != 0U || wire_info->queue_count == 0U ||
            wire_info->queue_family_index >= queue_family_count ||
            queue_properties[wire_info->queue_family_index].queueCount <
                wire_info->queue_count ||
            wire_info->first_priority >= request->queue_priority_count ||
            wire_info->queue_count >
                request->queue_priority_count - wire_info->first_priority) {
            response->vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
            return 0;
        }
        for (uint32_t prior = 0U; prior < index; ++prior) {
            if (request->queue_create_infos[prior].queue_family_index ==
                wire_info->queue_family_index) {
                response->vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
                return 0;
            }
        }
        queue_infos[index] = (VkDeviceQueueCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .flags = wire_info->flags,
            .queueFamilyIndex = wire_info->queue_family_index,
            .queueCount = wire_info->queue_count,
            .pQueuePriorities =
                &queue_priorities[wire_info->first_priority],
        };
    }
    PFN_vkCreateDevice create_device =
        (PFN_vkCreateDevice)context->get_instance_proc_addr(
            instance, "vkCreateDevice");
    if (create_device == NULL) {
        set_error(error, error_size, "instance has no vkCreateDevice");
        return -ENOSYS;
    }
    VkPhysicalDeviceVulkan11Features vulkan11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .shaderDrawParameters =
            (VkBool32)request->enabled_features.shader_draw_parameters,
    };
    VkPhysicalDeviceVulkan12Features vulkan12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .samplerMirrorClampToEdge =
            (VkBool32)request->enabled_features.sampler_mirror_clamp_to_edge,
        .descriptorIndexing =
            (VkBool32)request->enabled_features.descriptor_indexing,
        .descriptorBindingSampledImageUpdateAfterBind =
            (VkBool32)request->enabled_features
                .descriptor_binding_sampled_image_update_after_bind,
        .descriptorBindingUpdateUnusedWhilePending =
            (VkBool32)request->enabled_features
                .descriptor_binding_update_unused_while_pending,
        .descriptorBindingPartiallyBound =
            (VkBool32)request->enabled_features
                .descriptor_binding_partially_bound,
        .runtimeDescriptorArray =
            (VkBool32)request->enabled_features.runtime_descriptor_array,
        .scalarBlockLayout =
            (VkBool32)request->enabled_features.scalar_block_layout,
        .uniformBufferStandardLayout =
            (VkBool32)request->enabled_features
                .uniform_buffer_standard_layout,
        .hostQueryReset =
            (VkBool32)request->enabled_features.host_query_reset,
        .timelineSemaphore =
            (VkBool32)request->enabled_features.timeline_semaphore,
        .bufferDeviceAddress =
            (VkBool32)request->enabled_features.buffer_device_address,
        .vulkanMemoryModel =
            (VkBool32)request->enabled_features.vulkan_memory_model,
    };
    VkPhysicalDeviceVulkan13Features vulkan13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .shaderDemoteToHelperInvocation =
            (VkBool32)request->enabled_features
                .shader_demote_to_helper_invocation,
        .subgroupSizeControl =
            (VkBool32)request->enabled_features.subgroup_size_control,
        .computeFullSubgroups =
            (VkBool32)request->enabled_features.compute_full_subgroups,
        .synchronization2 =
            (VkBool32)request->enabled_features.synchronization2,
        .shaderZeroInitializeWorkgroupMemory =
            (VkBool32)request->enabled_features
                .shader_zero_initialize_workgroup_memory,
        .dynamicRendering =
            (VkBool32)request->enabled_features.dynamic_rendering,
        .maintenance4 =
            (VkBool32)request->enabled_features.maintenance4,
    };
    VkPhysicalDeviceDepthClipEnableFeaturesEXT depth_clip = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT,
        .depthClipEnable =
            (VkBool32)request->enabled_features.depth_clip_enable,
    };
    VkPhysicalDeviceRobustness2FeaturesEXT robustness2 = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
        .robustBufferAccess2 =
            (VkBool32)request->enabled_features.robust_buffer_access2,
        .nullDescriptor =
            (VkBool32)request->enabled_features.null_descriptor,
    };
    VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5 = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR,
        .maintenance5 =
            (VkBool32)request->enabled_features.maintenance5,
    };
    VkPhysicalDeviceMaintenance6FeaturesKHR maintenance6 = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR,
        .maintenance6 =
            (VkBool32)request->enabled_features.maintenance6,
    };
    void *feature_chain = NULL;
    void **feature_tail = &feature_chain;
#define BVB_APPEND_DEVICE_FEATURE(bit, structure) \
    do { \
        if ((request->enabled_feature_structs & (bit)) != 0U) { \
            *feature_tail = &(structure); \
            feature_tail = &(structure).pNext; \
        } \
    } while (0)
    BVB_APPEND_DEVICE_FEATURE(
        BVB_VULKAN_DEVICE_FEATURE_VULKAN_11, vulkan11);
    BVB_APPEND_DEVICE_FEATURE(
        BVB_VULKAN_DEVICE_FEATURE_VULKAN_12, vulkan12);
    BVB_APPEND_DEVICE_FEATURE(
        BVB_VULKAN_DEVICE_FEATURE_VULKAN_13, vulkan13);
    BVB_APPEND_DEVICE_FEATURE(
        BVB_VULKAN_DEVICE_FEATURE_DEPTH_CLIP_ENABLE, depth_clip);
    BVB_APPEND_DEVICE_FEATURE(
        BVB_VULKAN_DEVICE_FEATURE_ROBUSTNESS_2, robustness2);
    BVB_APPEND_DEVICE_FEATURE(
        BVB_VULKAN_DEVICE_FEATURE_MAINTENANCE_5, maintenance5);
    BVB_APPEND_DEVICE_FEATURE(
        BVB_VULKAN_DEVICE_FEATURE_MAINTENANCE_6, maintenance6);
#undef BVB_APPEND_DEVICE_FEATURE
    _Static_assert(sizeof(VkPhysicalDeviceFeatures) ==
                       BVB_VULKAN_BASE_FEATURES_SIZE,
                   "VkPhysicalDeviceFeatures wire size changed");
    VkPhysicalDeviceFeatures base_features = {0};
    memcpy(&base_features, request->enabled_base_features.values,
           BVB_VULKAN_BASE_FEATURES_SIZE);
    const VkDeviceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = feature_chain,
        .queueCreateInfoCount = request->queue_create_info_count,
        .pQueueCreateInfos = queue_infos,
        .enabledExtensionCount = request->enabled_extension_count,
        .ppEnabledExtensionNames = enabled_extensions,
        .pEnabledFeatures =
            (request->enabled_feature_structs &
             BVB_VULKAN_DEVICE_FEATURE_BASE) == 0U
                ? NULL
                : &base_features,
    };
    VkDevice device = VK_NULL_HANDLE;
    VkResult result = create_device(
        physical_device, &create_info, NULL, &device);
    response->vulkan_result = result;
    if (result != VK_SUCCESS) {
        return 0;
    }
    PFN_vkDestroyDevice destroy_device =
        (PFN_vkDestroyDevice)context->get_device_proc_addr(
            device, "vkDestroyDevice");
    if (destroy_device == NULL) {
        set_error(error, error_size, "created device has no vkDestroyDevice");
        response->vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
        return 0;
    }
    const uint64_t native_bits = handle_bits(&device, sizeof(device));
    const uint64_t wire_id = bvb_handle_id(
        BVB_OBJECT_DEVICE, context->next_device_serial++);
    struct bvb_device_metadata *metadata = device_metadata_slot(context, 0U);
    if (metadata == NULL) {
        destroy_device(device, NULL);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    status = bvb_handle_table_insert(
        &context->objects, wire_id, request->physical_device_id, native_bits);
    if (status != 0) {
        destroy_device(device, NULL);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    *metadata = (struct bvb_device_metadata){
        .device_id = wire_id,
        .non_coherent_atom_size =
            physical_properties.limits.nonCoherentAtomSize,
        .queue_create_info_count = request->queue_create_info_count,
    };
    memcpy(metadata->queue_create_infos, request->queue_create_infos,
           request->queue_create_info_count *
               sizeof(metadata->queue_create_infos[0]));
    response->device_id = wire_id;
    return 0;
}

int bvb_vulkan_global_context_create_device(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_device_create_request *request,
    const char *const *enabled_extensions,
    struct bvb_vulkan_device_create_response *response,
    char *error, size_t error_size) {
    if (request == NULL) {
        return -EINVAL;
    }
    const struct bvb_vulkan_device_create_packed_request packed = {
        .physical_device_id = request->physical_device_id,
        .flags = request->flags,
        .queue_create_info_count = 1U,
        .queue_priority_count = 1U,
        .enabled_layer_count = request->enabled_layer_count,
        .enabled_extension_count = request->enabled_extension_count,
        .queue_create_infos = {{
            .queue_family_index = request->queue_family_index,
            .queue_count = request->queue_count,
        }},
        .queue_priority_bits = {request->queue_priority_bits},
    };
    return bvb_vulkan_global_context_create_device_packed(
        context, &packed, enabled_extensions, response, error, error_size);
}

int bvb_vulkan_global_context_destroy_device(
    struct bvb_vulkan_global_context *context, uint64_t device_id) {
    if (context == NULL || context->get_device_proc_addr == NULL) {
        return -EINVAL;
    }
    uint64_t device_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, device_id, BVB_OBJECT_DEVICE, NULL, &device_bits);
    if (result != 0) {
        return result;
    }
    const VkDevice device = device_from_bits(device_bits);
    PFN_vkDestroyDevice destroy_device =
        (PFN_vkDestroyDevice)context->get_device_proc_addr(
            device, "vkDestroyDevice");
    if (destroy_device == NULL) {
        return -ENOSYS;
    }
    for (size_t index = 0U; index < BVB_VULKAN_MEMORY_MIRROR_CAPACITY;
         ++index)
        if (context->memory_mirrors[index].memory_id != 0U &&
            context->memory_mirrors[index].device_id == device_id)
            return -EBUSY;
    for (size_t index = 0U; index < BVB_WSI_FRAME_RING_MAX_SLOTS; ++index) {
        struct bvb_swapchain_metadata *swapchain =
            &context->swapchain_metadata[index];
        if (swapchain->swapchain_id != 0U &&
            swapchain->device_id == device_id) {
            result = destroy_swapchain_metadata(context, swapchain);
            if (result != 0) return result;
        }
    }
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_PIPELINE &&
            entry->parent_id == device_id) {
            result = bvb_vulkan_global_context_destroy_pipeline(
                context, entry->wire_id, NULL, 0U);
            if (result != 0) return result;
        }
    }
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_PIPELINE_LAYOUT &&
            entry->parent_id == device_id) {
            result = bvb_vulkan_global_context_destroy_pipeline_layout(
                context, entry->wire_id, NULL, 0U);
            if (result != 0) return result;
        }
    }
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_DESCRIPTOR_POOL &&
            entry->parent_id == device_id) {
            result = bvb_vulkan_global_context_destroy_descriptor_pool(
                context, entry->wire_id, NULL, 0U);
            if (result != 0) return result;
        }
    }
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_SAMPLER &&
            entry->parent_id == device_id) {
            result = bvb_vulkan_global_context_destroy_sampler(
                context, entry->wire_id, NULL, 0U);
            if (result != 0) return result;
        }
    }
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) ==
                BVB_OBJECT_DESCRIPTOR_UPDATE_TEMPLATE &&
            entry->parent_id == device_id) {
            result =
                bvb_vulkan_global_context_destroy_descriptor_update_template(
                    context, entry->wire_id, NULL, 0U);
            if (result != 0) return result;
        }
    }
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) ==
                BVB_OBJECT_DESCRIPTOR_SET_LAYOUT &&
            entry->parent_id == device_id) {
            result = bvb_vulkan_global_context_destroy_descriptor_set_layout(
                context, entry->wire_id, NULL, 0U);
            if (result != 0) return result;
        }
    }
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_COMMAND_POOL &&
            entry->parent_id == device_id) {
            result = bvb_vulkan_global_context_destroy_command_pool(
                context, entry->wire_id, NULL, 0U);
            if (result != 0) {
                return result;
            }
        }
    }
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_IMAGE_VIEW) {
            uint64_t image_device_id = 0U;
            uint64_t image_bits = 0U;
            const int owner_result = bvb_handle_table_lookup(
                &context->objects, entry->parent_id, BVB_OBJECT_IMAGE,
                &image_device_id, &image_bits);
            if (owner_result == 0 && image_device_id == device_id) {
                result = bvb_vulkan_global_context_destroy_image_view(
                    context, entry->wire_id, NULL, 0U);
                if (result != 0) return result;
            }
        }
    }
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_IMAGE &&
            entry->parent_id == device_id) {
            result = bvb_vulkan_global_context_destroy_image(
                context, entry->wire_id, NULL, 0U);
            if (result != 0) return result;
        }
    }
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_BUFFER &&
            entry->parent_id == device_id) {
            result = bvb_vulkan_global_context_destroy_buffer(
                context, entry->wire_id, NULL, 0U);
            if (result != 0) return result;
        }
    }
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_DEVICE_MEMORY &&
            entry->parent_id == device_id) {
            result = bvb_vulkan_global_context_free_memory(
                context, entry->wire_id, NULL, 0U);
            if (result != 0) return result;
        }
    }
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_FENCE &&
            entry->parent_id == device_id) {
            result = bvb_vulkan_global_context_destroy_fence(
                context, entry->wire_id, NULL, 0U);
            if (result != 0) return result;
        }
    }
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_SEMAPHORE &&
            entry->parent_id == device_id) {
            result = bvb_vulkan_global_context_destroy_semaphore(
                context, entry->wire_id, NULL, 0U);
            if (result != 0) return result;
        }
    }
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_QUEUE &&
            entry->parent_id == device_id) {
            result = bvb_handle_table_remove(
                &context->objects, entry->wire_id, BVB_OBJECT_QUEUE, NULL);
            if (result != 0) {
                return result;
            }
        }
    }
    result = bvb_handle_table_remove(
        &context->objects, device_id, BVB_OBJECT_DEVICE, &device_bits);
    if (result != 0) {
        return result;
    }
    struct bvb_device_metadata *metadata =
        device_metadata_slot(context, device_id);
    if (metadata != NULL && metadata->device_id == device_id) {
        *metadata = (struct bvb_device_metadata){0};
    }
    destroy_device(device, NULL);
    return 0;
}

static uint64_t existing_queue_id(
    const struct bvb_vulkan_global_context *context, uint64_t device_id,
    uint64_t native_bits) {
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_QUEUE &&
            entry->parent_id == device_id && entry->native_bits == native_bits) {
            return entry->wire_id;
        }
    }
    return 0U;
}

static uint32_t device_metadata_queue_count(
    const struct bvb_device_metadata *metadata, uint32_t queue_family_index) {
    if (metadata == NULL) {
        return 0U;
    }
    for (uint32_t index = 0U;
         index < metadata->queue_create_info_count; ++index) {
        if (metadata->queue_create_infos[index].queue_family_index ==
            queue_family_index) {
            return metadata->queue_create_infos[index].queue_count;
        }
    }
    return 0U;
}

int bvb_vulkan_global_context_get_device_queue(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_device_queue_request *request,
    uint64_t *queue_id, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (context == NULL || request == NULL || queue_id == NULL) {
        return -EINVAL;
    }
    *queue_id = 0U;
    uint64_t device_bits = 0U;
    int status = bvb_handle_table_lookup(
        &context->objects, request->device_id, BVB_OBJECT_DEVICE, NULL,
        &device_bits);
    struct bvb_device_metadata *metadata =
        device_metadata_slot(context, request->device_id);
    if (status != 0 || metadata == NULL ||
        metadata->device_id != request->device_id) {
        set_error(error, error_size, "unknown device handle");
        return status != 0 ? status : -ENOENT;
    }
    const uint32_t created_queue_count = device_metadata_queue_count(
        metadata, request->queue_family_index);
    if (created_queue_count == 0U ||
        request->queue_index >= created_queue_count) {
        set_error(error, error_size, "queue index was not created");
        return -ERANGE;
    }
    const VkDevice device = device_from_bits(device_bits);
    PFN_vkGetDeviceQueue get_queue =
        (PFN_vkGetDeviceQueue)context->get_device_proc_addr(
            device, "vkGetDeviceQueue");
    if (get_queue == NULL) {
        set_error(error, error_size, "device has no vkGetDeviceQueue");
        return -ENOSYS;
    }
    VkQueue queue = VK_NULL_HANDLE;
    get_queue(device, request->queue_family_index, request->queue_index, &queue);
    if (queue == VK_NULL_HANDLE) {
        set_error(error, error_size, "vkGetDeviceQueue returned null");
        return -EIO;
    }
    const uint64_t native_bits = handle_bits(&queue, sizeof(queue));
    uint64_t wire_id = existing_queue_id(
        context, request->device_id, native_bits);
    if (wire_id == 0U) {
        wire_id = bvb_handle_id(
            BVB_OBJECT_QUEUE, context->next_queue_serial++);
        status = bvb_handle_table_insert(
            &context->objects, wire_id, request->device_id, native_bits);
        if (status != 0) {
            return status;
        }
    }
    *queue_id = wire_id;
    return 0;
}

static int resolve_queue(
    const struct bvb_vulkan_global_context *context, uint64_t queue_id,
    VkDevice *device, VkQueue *queue) {
    if (context == NULL || device == NULL || queue == NULL) {
        return -EINVAL;
    }
    uint64_t device_id = 0U;
    uint64_t queue_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, queue_id, BVB_OBJECT_QUEUE, &device_id, &queue_bits);
    uint64_t device_bits = 0U;
    if (result == 0) {
        result = bvb_handle_table_lookup(
            &context->objects, device_id, BVB_OBJECT_DEVICE, NULL,
            &device_bits);
    }
    if (result == 0) {
        *device = device_from_bits(device_bits);
        *queue = VK_NULL_HANDLE;
        _Static_assert(sizeof(*queue) <= sizeof(queue_bits),
                       "VkQueue exceeds bridge handle width");
        memcpy(queue, &queue_bits, sizeof(*queue));
    }
    return result;
}

int bvb_vulkan_global_context_queue_submit_empty(
    const struct bvb_vulkan_global_context *context, uint64_t queue_id,
    int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (vulkan_result == NULL) {
        return -EINVAL;
    }
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    int result = resolve_queue(context, queue_id, &device, &queue);
    if (result != 0) {
        set_error(error, error_size, "unknown queue handle");
        return result;
    }
    PFN_vkQueueSubmit submit =
        (PFN_vkQueueSubmit)context->get_device_proc_addr(
            device, "vkQueueSubmit");
    if (submit == NULL) {
        set_error(error, error_size, "device has no vkQueueSubmit");
        return -ENOSYS;
    }
    *vulkan_result = submit(queue, 0U, NULL, VK_NULL_HANDLE);
    return 0;
}

int bvb_vulkan_global_context_queue_wait_idle(
    const struct bvb_vulkan_global_context *context, uint64_t queue_id,
    int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (vulkan_result == NULL) {
        return -EINVAL;
    }
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    int result = resolve_queue(context, queue_id, &device, &queue);
    if (result != 0) {
        set_error(error, error_size, "unknown queue handle");
        return result;
    }
    PFN_vkQueueWaitIdle wait_idle =
        (PFN_vkQueueWaitIdle)context->get_device_proc_addr(
            device, "vkQueueWaitIdle");
    if (wait_idle == NULL) {
        set_error(error, error_size, "device has no vkQueueWaitIdle");
        return -ENOSYS;
    }
    *vulkan_result = wait_idle(queue);
    return 0;
}

int bvb_vulkan_global_context_device_wait_idle(
    const struct bvb_vulkan_global_context *context, uint64_t device_id,
    int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (context == NULL || vulkan_result == NULL) {
        return -EINVAL;
    }
    uint64_t device_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, device_id, BVB_OBJECT_DEVICE, NULL, &device_bits);
    if (result != 0) {
        set_error(error, error_size, "unknown device handle");
        return result;
    }
    const VkDevice device = device_from_bits(device_bits);
    PFN_vkDeviceWaitIdle wait_idle =
        (PFN_vkDeviceWaitIdle)context->get_device_proc_addr(
            device, "vkDeviceWaitIdle");
    if (wait_idle == NULL) {
        set_error(error, error_size, "device has no vkDeviceWaitIdle");
        return -ENOSYS;
    }
    *vulkan_result = wait_idle(device);
    return 0;
}

static int resolve_command_pool(
    const struct bvb_vulkan_global_context *context, uint64_t command_pool_id,
    uint64_t *device_id, VkDevice *device, VkCommandPool *command_pool) {
    if (context == NULL || device_id == NULL || device == NULL ||
        command_pool == NULL) {
        return -EINVAL;
    }
    uint64_t pool_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, command_pool_id, BVB_OBJECT_COMMAND_POOL,
        device_id, &pool_bits);
    uint64_t device_bits = 0U;
    if (result == 0) {
        result = bvb_handle_table_lookup(
            &context->objects, *device_id, BVB_OBJECT_DEVICE, NULL,
            &device_bits);
    }
    if (result == 0) {
        *device = device_from_bits(device_bits);
        *command_pool = command_pool_from_bits(pool_bits);
    }
    return result;
}

static int resolve_command_buffer(
    const struct bvb_vulkan_global_context *context,
    uint64_t command_buffer_id, uint64_t *device_id,
    VkDevice *device, VkCommandPool *command_pool,
    VkCommandBuffer *command_buffer) {
    if (context == NULL || device_id == NULL || device == NULL ||
        command_pool == NULL || command_buffer == NULL) {
        return -EINVAL;
    }
    uint64_t command_pool_id = 0U;
    uint64_t command_buffer_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, command_buffer_id, BVB_OBJECT_COMMAND_BUFFER,
        &command_pool_id, &command_buffer_bits);
    if (result == 0) {
        result = resolve_command_pool(
            context, command_pool_id, device_id, device, command_pool);
    }
    if (result == 0) {
        *command_buffer = command_buffer_from_bits(command_buffer_bits);
    }
    return result;
}

bool bvb_vulkan_global_context_command_buffer_is_live(
    const struct bvb_vulkan_global_context *context,
    uint64_t command_buffer_id) {
    uint64_t device_id = 0U;
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    return resolve_command_buffer(
               context, command_buffer_id, &device_id, &device,
               &command_pool, &command_buffer) == 0;
}

int bvb_vulkan_global_context_create_command_pool(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_command_pool_create_request *request,
    struct bvb_vulkan_command_pool_create_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (context == NULL || request == NULL || response == NULL) {
        return -EINVAL;
    }
    *response = (struct bvb_vulkan_command_pool_create_response){0};
    const uint32_t supported_flags =
        VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if ((request->flags & ~supported_flags) != 0U) {
        response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return 0;
    }
    uint64_t device_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, request->device_id, BVB_OBJECT_DEVICE, NULL,
        &device_bits);
    struct bvb_device_metadata *metadata =
        device_metadata_slot(context, request->device_id);
    if (result != 0 || metadata == NULL ||
        metadata->device_id != request->device_id) {
        set_error(error, error_size, "unknown device handle");
        return result != 0 ? result : -ENOENT;
    }
    if (device_metadata_queue_count(
            metadata, request->queue_family_index) == 0U) {
        response->vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
        return 0;
    }
    const VkDevice device = device_from_bits(device_bits);
    PFN_vkCreateCommandPool create_command_pool =
        (PFN_vkCreateCommandPool)context->get_device_proc_addr(
            device, "vkCreateCommandPool");
    PFN_vkDestroyCommandPool destroy_command_pool =
        (PFN_vkDestroyCommandPool)context->get_device_proc_addr(
            device, "vkDestroyCommandPool");
    if (create_command_pool == NULL || destroy_command_pool == NULL) {
        set_error(error, error_size, "device lacks command-pool lifecycle");
        return -ENOSYS;
    }
    const VkCommandPoolCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = request->flags,
        .queueFamilyIndex = request->queue_family_index,
    };
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkResult vulkan_result = create_command_pool(
        device, &create_info, NULL, &command_pool);
    response->vulkan_result = vulkan_result;
    if (vulkan_result != VK_SUCCESS) {
        return 0;
    }
    const uint64_t wire_id = bvb_handle_id(
        BVB_OBJECT_COMMAND_POOL, context->next_command_pool_serial++);
    result = bvb_handle_table_insert(
        &context->objects, wire_id, request->device_id,
        handle_bits(&command_pool, sizeof(command_pool)));
    if (result != 0) {
        destroy_command_pool(device, command_pool, NULL);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    response->command_pool_id = wire_id;
    return 0;
}

int bvb_vulkan_global_context_destroy_command_pool(
    struct bvb_vulkan_global_context *context, uint64_t command_pool_id,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    uint64_t device_id = 0U;
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    int result = resolve_command_pool(
        context, command_pool_id, &device_id, &device, &command_pool);
    if (result != 0) {
        set_error(error, error_size, "unknown command-pool handle");
        return result;
    }
    PFN_vkDestroyCommandPool destroy_command_pool =
        (PFN_vkDestroyCommandPool)context->get_device_proc_addr(
            device, "vkDestroyCommandPool");
    if (destroy_command_pool == NULL) {
        return -ENOSYS;
    }
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_COMMAND_BUFFER &&
            entry->parent_id == command_pool_id) {
            result = bvb_handle_table_remove(
                &context->objects, entry->wire_id,
                BVB_OBJECT_COMMAND_BUFFER, NULL);
            if (result != 0) {
                return result;
            }
        }
    }
    result = bvb_handle_table_remove(
        &context->objects, command_pool_id, BVB_OBJECT_COMMAND_POOL, NULL);
    if (result != 0) {
        return result;
    }
    destroy_command_pool(device, command_pool, NULL);
    return 0;
}

int bvb_vulkan_global_context_reset_command_pool(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_command_pool_reset_request *request,
    int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (request == NULL || vulkan_result == NULL ||
        (request->flags & ~VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT) != 0U) {
        return -EINVAL;
    }
    uint64_t device_id = 0U;
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    int result = resolve_command_pool(
        context, request->command_pool_id, &device_id, &device, &command_pool);
    if (result != 0) {
        set_error(error, error_size, "unknown command-pool handle");
        return result;
    }
    PFN_vkResetCommandPool reset_command_pool =
        (PFN_vkResetCommandPool)context->get_device_proc_addr(
            device, "vkResetCommandPool");
    if (reset_command_pool == NULL) {
        return -ENOSYS;
    }
    *vulkan_result = reset_command_pool(
        device, command_pool, request->flags);
    return 0;
}

int bvb_vulkan_global_context_allocate_command_buffer(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_command_buffer_allocate_request *request,
    struct bvb_vulkan_command_buffer_allocate_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (context == NULL || request == NULL || response == NULL) {
        return -EINVAL;
    }
    *response = (struct bvb_vulkan_command_buffer_allocate_response){0};
    if (request->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY ||
        request->count != 1U) {
        response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return 0;
    }
    uint64_t device_id = 0U;
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    int result = resolve_command_pool(
        context, request->command_pool_id, &device_id, &device, &command_pool);
    if (result != 0) {
        set_error(error, error_size, "unknown command-pool handle");
        return result;
    }
    PFN_vkAllocateCommandBuffers allocate =
        (PFN_vkAllocateCommandBuffers)context->get_device_proc_addr(
            device, "vkAllocateCommandBuffers");
    PFN_vkFreeCommandBuffers free_command_buffers =
        (PFN_vkFreeCommandBuffers)context->get_device_proc_addr(
            device, "vkFreeCommandBuffers");
    if (allocate == NULL || free_command_buffers == NULL) {
        set_error(error, error_size,
                  "device lacks command-buffer allocation lifecycle");
        return -ENOSYS;
    }
    const VkCommandBufferAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1U,
    };
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkResult vulkan_result = allocate(device, &allocate_info, &command_buffer);
    response->vulkan_result = vulkan_result;
    if (vulkan_result != VK_SUCCESS) {
        return 0;
    }
    const uint64_t wire_id = bvb_handle_id(
        BVB_OBJECT_COMMAND_BUFFER, context->next_command_buffer_serial++);
    result = bvb_handle_table_insert(
        &context->objects, wire_id, request->command_pool_id,
        handle_bits(&command_buffer, sizeof(command_buffer)));
    if (result != 0) {
        free_command_buffers(device, command_pool, 1U, &command_buffer);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    response->command_buffer_id = wire_id;
    return 0;
}

int bvb_vulkan_global_context_free_command_buffer(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_command_buffer_free_request *request,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (context == NULL || request == NULL) {
        return -EINVAL;
    }
    uint64_t parent_pool_id = 0U;
    uint64_t command_buffer_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, request->command_buffer_id,
        BVB_OBJECT_COMMAND_BUFFER, &parent_pool_id, &command_buffer_bits);
    if (result != 0 || parent_pool_id != request->command_pool_id) {
        set_error(error, error_size, "command buffer does not belong to pool");
        return result != 0 ? result : -EPROTO;
    }
    uint64_t device_id = 0U;
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    result = resolve_command_pool(
        context, request->command_pool_id, &device_id, &device, &command_pool);
    if (result != 0) {
        return result;
    }
    PFN_vkFreeCommandBuffers free_command_buffers =
        (PFN_vkFreeCommandBuffers)context->get_device_proc_addr(
            device, "vkFreeCommandBuffers");
    if (free_command_buffers == NULL) {
        return -ENOSYS;
    }
    const VkCommandBuffer command_buffer =
        command_buffer_from_bits(command_buffer_bits);
    free_command_buffers(device, command_pool, 1U, &command_buffer);
    return bvb_handle_table_remove(
        &context->objects, request->command_buffer_id,
        BVB_OBJECT_COMMAND_BUFFER, NULL);
}

int bvb_vulkan_global_context_begin_command_buffer(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_command_buffer_begin_request *request,
    int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (request == NULL || vulkan_result == NULL ||
        (request->flags & ~VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) != 0U) {
        return -EINVAL;
    }
    uint64_t device_id = 0U;
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    int result = resolve_command_buffer(
        context, request->command_buffer_id, &device_id, &device,
        &command_pool, &command_buffer);
    if (result != 0) {
        set_error(error, error_size, "unknown command-buffer handle");
        return result;
    }
    PFN_vkBeginCommandBuffer begin =
        (PFN_vkBeginCommandBuffer)context->get_device_proc_addr(
            device, "vkBeginCommandBuffer");
    if (begin == NULL) {
        return -ENOSYS;
    }
    const VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = request->flags,
    };
    *vulkan_result = begin(command_buffer, &begin_info);
    return 0;
}

int bvb_vulkan_global_context_end_command_buffer(
    const struct bvb_vulkan_global_context *context,
    uint64_t command_buffer_id, int32_t *vulkan_result,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (vulkan_result == NULL) {
        return -EINVAL;
    }
    uint64_t device_id = 0U;
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    int result = resolve_command_buffer(
        context, command_buffer_id, &device_id, &device,
        &command_pool, &command_buffer);
    if (result != 0) {
        set_error(error, error_size, "unknown command-buffer handle");
        return result;
    }
    PFN_vkEndCommandBuffer end =
        (PFN_vkEndCommandBuffer)context->get_device_proc_addr(
            device, "vkEndCommandBuffer");
    if (end == NULL) {
        return -ENOSYS;
    }
    *vulkan_result = end(command_buffer);
    return 0;
}

int bvb_vulkan_global_context_queue_submit_command(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_queue_submit_command_request *request,
    int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (request == NULL || vulkan_result == NULL) {
        return -EINVAL;
    }
    VkDevice queue_device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    int result = resolve_queue(
        context, request->queue_id, &queue_device, &queue);
    if (result != 0) {
        set_error(error, error_size, "unknown queue handle");
        return result;
    }
    uint64_t queue_device_id = 0U;
    uint64_t queue_bits = 0U;
    result = bvb_handle_table_lookup(
        &context->objects, request->queue_id, BVB_OBJECT_QUEUE,
        &queue_device_id, &queue_bits);
    uint64_t command_device_id = 0U;
    VkDevice command_device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    if (result == 0) {
        result = resolve_command_buffer(
            context, request->command_buffer_id, &command_device_id,
            &command_device, &command_pool, &command_buffer);
    }
    if (result != 0 || command_device_id != queue_device_id ||
        command_device != queue_device) {
        set_error(error, error_size,
                  "queue and command buffer have different devices");
        return result != 0 ? result : -EPROTO;
    }
    result = sync_coherent_memory_mirrors(context, queue_device_id);
    if (result != 0) return result;
    PFN_vkQueueSubmit submit =
        (PFN_vkQueueSubmit)context->get_device_proc_addr(
            queue_device, "vkQueueSubmit");
    if (submit == NULL) {
        return -ENOSYS;
    }
    const VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1U,
        .pCommandBuffers = &command_buffer,
    };
    *vulkan_result = submit(queue, 1U, &submit_info, VK_NULL_HANDLE);
    return 0;
}

static int resolve_device_child(
    const struct bvb_vulkan_global_context *context, uint64_t object_id,
    enum bvb_object_type type, uint64_t *device_id, VkDevice *device,
    uint64_t *native_bits) {
    if (context == NULL || device_id == NULL || device == NULL ||
        native_bits == NULL) return -EINVAL;
    int result = bvb_handle_table_lookup(
        &context->objects, object_id, type, device_id, native_bits);
    uint64_t device_bits = 0U;
    if (result == 0) {
        result = bvb_handle_table_lookup(
            &context->objects, *device_id, BVB_OBJECT_DEVICE, NULL,
            &device_bits);
    }
    if (result == 0) *device = device_from_bits(device_bits);
    return result;
}

static struct bvb_memory_metadata *memory_metadata_slot(
    struct bvb_vulkan_global_context *context, uint64_t memory_id) {
    struct bvb_memory_metadata *empty = NULL;
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        struct bvb_memory_metadata *metadata = &context->memory_metadata[index];
        if (metadata->memory_id == memory_id) return metadata;
        if (metadata->memory_id == 0U && empty == NULL) empty = metadata;
    }
    return empty;
}

static bool core_descriptor_type_supported(uint32_t descriptor_type) {
    switch ((VkDescriptorType)descriptor_type) {
    case VK_DESCRIPTOR_TYPE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
        return true;
    default:
        return false;
    }
}

int bvb_vulkan_global_context_create_descriptor_set_layout(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_descriptor_set_layout_create_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || response == NULL) return -EINVAL;
    *response = (struct bvb_vulkan_object_create_response){0};
    const VkDescriptorSetLayoutCreateFlags allowed_layout_flags =
        VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    const VkDescriptorBindingFlags allowed_binding_flags =
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT |
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    if ((request->flags & ~allowed_layout_flags) != 0U ||
        request->binding_count > BVB_VULKAN_MAX_DESCRIPTOR_LAYOUT_BINDINGS ||
        request->has_binding_flags > 1U) {
        response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return 0;
    }
    VkDescriptorSetLayoutBinding
        bindings[BVB_VULKAN_MAX_DESCRIPTOR_LAYOUT_BINDINGS] = {0};
    VkDescriptorBindingFlags
        binding_flags[BVB_VULKAN_MAX_DESCRIPTOR_LAYOUT_BINDINGS] = {0};
    for (uint32_t index = 0U; index < request->binding_count; ++index) {
        const struct bvb_vulkan_descriptor_layout_binding *wire =
            &request->bindings[index];
        if (!core_descriptor_type_supported(wire->descriptor_type) ||
            wire->descriptor_count == 0U ||
            wire->descriptor_count > (1U << 20) ||
            wire->stage_flags == 0U ||
            (wire->binding_flags & ~allowed_binding_flags) != 0U ||
            (!request->has_binding_flags && wire->binding_flags != 0U)) {
            response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
            return 0;
        }
        bindings[index] = (VkDescriptorSetLayoutBinding){
            .binding = wire->binding,
            .descriptorType = (VkDescriptorType)wire->descriptor_type,
            .descriptorCount = wire->descriptor_count,
            .stageFlags = wire->stage_flags,
        };
        binding_flags[index] = wire->binding_flags;
    }
    uint64_t device_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, request->device_id, BVB_OBJECT_DEVICE, NULL,
        &device_bits);
    if (result != 0) {
        set_error(error, error_size,
                  "descriptor layout device lookup failed: %d", result);
        return result;
    }
    const VkDevice device = device_from_bits(device_bits);
    PFN_vkCreateDescriptorSetLayout create_layout =
        (PFN_vkCreateDescriptorSetLayout)context->get_device_proc_addr(
            device, "vkCreateDescriptorSetLayout");
    PFN_vkDestroyDescriptorSetLayout destroy_layout =
        (PFN_vkDestroyDescriptorSetLayout)context->get_device_proc_addr(
            device, "vkDestroyDescriptorSetLayout");
    if (create_layout == NULL || destroy_layout == NULL) return -ENOSYS;
    const VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info = {
        .sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = request->binding_count,
        .pBindingFlags = binding_flags,
    };
    const VkDescriptorSetLayoutCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = request->has_binding_flags ? &flags_info : NULL,
        .flags = request->flags,
        .bindingCount = request->binding_count,
        .pBindings = request->binding_count == 0U ? NULL : bindings,
    };
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    response->vulkan_result =
        create_layout(device, &create_info, NULL, &layout);
    if (response->vulkan_result != VK_SUCCESS) return 0;
    const uint64_t wire_id = bvb_handle_id(
        BVB_OBJECT_DESCRIPTOR_SET_LAYOUT,
        context->next_descriptor_set_layout_serial++);
    result = bvb_handle_table_insert(
        &context->objects, wire_id, request->device_id,
        handle_bits(&layout, sizeof(layout)));
    if (result != 0) {
        destroy_layout(device, layout, NULL);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    response->object_id = wire_id;
    return 0;
}

int bvb_vulkan_global_context_destroy_descriptor_set_layout(
    struct bvb_vulkan_global_context *context, uint64_t layout_id,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    uint64_t device_id = 0U, layout_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, layout_id, BVB_OBJECT_DESCRIPTOR_SET_LAYOUT, &device_id,
        &device, &layout_bits);
    if (result != 0) return result;
    PFN_vkDestroyDescriptorSetLayout destroy_layout =
        (PFN_vkDestroyDescriptorSetLayout)context->get_device_proc_addr(
            device, "vkDestroyDescriptorSetLayout");
    if (destroy_layout == NULL) return -ENOSYS;
    result = bvb_handle_table_remove(
        &context->objects, layout_id, BVB_OBJECT_DESCRIPTOR_SET_LAYOUT, NULL);
    if (result == 0) {
        destroy_layout(
            device, descriptor_set_layout_from_bits(layout_bits), NULL);
    }
    return result;
}

static struct bvb_descriptor_template_metadata *descriptor_template_slot(
    struct bvb_vulkan_global_context *context, uint64_t template_id) {
    struct bvb_descriptor_template_metadata *empty = NULL;
    for (size_t index = 0U;
         index < BVB_DESCRIPTOR_TEMPLATE_METADATA_CAPACITY; ++index) {
        struct bvb_descriptor_template_metadata *metadata =
            &context->descriptor_template_metadata[index];
        if (metadata->template_id == template_id) return metadata;
        if (metadata->template_id == 0U && empty == NULL) empty = metadata;
    }
    return empty;
}

static size_t descriptor_template_data_size(uint32_t descriptor_type) {
    switch ((VkDescriptorType)descriptor_type) {
    case VK_DESCRIPTOR_TYPE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
        return sizeof(VkDescriptorImageInfo);
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        return sizeof(VkBufferView);
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
        return sizeof(VkDescriptorBufferInfo);
    default:
        return 0U;
    }
}

int bvb_vulkan_global_context_create_descriptor_update_template(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_descriptor_update_template_create_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || response == NULL) return -EINVAL;
    *response = (struct bvb_vulkan_object_create_response){0};
    if (request->flags != 0U || request->entry_count == 0U ||
        request->entry_count >
            BVB_VULKAN_MAX_DESCRIPTOR_UPDATE_TEMPLATE_ENTRIES ||
        request->template_type !=
            VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET ||
        request->set != 0U || request->pipeline_layout_id != 0U ||
        request->pipeline_bind_point != 0U) {
        response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return 0;
    }
    uint64_t device_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, request->device_id, BVB_OBJECT_DEVICE, NULL,
        &device_bits);
    uint64_t layout_device_id = 0U, layout_bits = 0U;
    if (result == 0) {
        result = bvb_handle_table_lookup(
            &context->objects, request->descriptor_set_layout_id,
            BVB_OBJECT_DESCRIPTOR_SET_LAYOUT, &layout_device_id,
            &layout_bits);
    }
    if (result != 0 || layout_device_id != request->device_id) {
        set_error(error, error_size,
                  "descriptor template layout lineage failed: %d", result);
        return result != 0 ? result : -EPROTO;
    }
    VkDescriptorUpdateTemplateEntry
        entries[BVB_VULKAN_MAX_DESCRIPTOR_UPDATE_TEMPLATE_ENTRIES] = {0};
    for (uint32_t index = 0U; index < request->entry_count; ++index) {
        const struct bvb_vulkan_descriptor_update_template_entry *wire =
            &request->entries[index];
        const size_t value_size =
            descriptor_template_data_size(wire->descriptor_type);
        if (!core_descriptor_type_supported(wire->descriptor_type) ||
            value_size == 0U ||
            wire->descriptor_count == 0U ||
            wire->descriptor_count > (1U << 20) || wire->stride == 0U ||
            wire->offset >=
                BVB_VULKAN_MAX_DESCRIPTOR_UPDATE_TEMPLATE_DATA_SIZE ||
            wire->offset >
                BVB_VULKAN_MAX_DESCRIPTOR_UPDATE_TEMPLATE_DATA_SIZE -
                    value_size ||
            wire->stride >
                BVB_VULKAN_MAX_DESCRIPTOR_UPDATE_TEMPLATE_DATA_SIZE ||
            (uint64_t)(wire->descriptor_count - 1U) >
                (BVB_VULKAN_MAX_DESCRIPTOR_UPDATE_TEMPLATE_DATA_SIZE -
                 value_size - wire->offset) / wire->stride) {
            response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
            return 0;
        }
        entries[index] = (VkDescriptorUpdateTemplateEntry){
            .dstBinding = wire->dst_binding,
            .dstArrayElement = wire->dst_array_element,
            .descriptorCount = wire->descriptor_count,
            .descriptorType = (VkDescriptorType)wire->descriptor_type,
            .offset = (size_t)wire->offset,
            .stride = (size_t)wire->stride,
        };
    }
    const VkDevice device = device_from_bits(device_bits);
    PFN_vkCreateDescriptorUpdateTemplate create_template =
        (PFN_vkCreateDescriptorUpdateTemplate)context->get_device_proc_addr(
            device, "vkCreateDescriptorUpdateTemplate");
    PFN_vkDestroyDescriptorUpdateTemplate destroy_template =
        (PFN_vkDestroyDescriptorUpdateTemplate)context->get_device_proc_addr(
            device, "vkDestroyDescriptorUpdateTemplate");
    if (create_template == NULL || destroy_template == NULL) return -ENOSYS;
    const VkDescriptorUpdateTemplateCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
        .flags = request->flags,
        .descriptorUpdateEntryCount = request->entry_count,
        .pDescriptorUpdateEntries = entries,
        .templateType = (VkDescriptorUpdateTemplateType)request->template_type,
        .descriptorSetLayout =
            descriptor_set_layout_from_bits(layout_bits),
        .pipelineBindPoint =
            (VkPipelineBindPoint)request->pipeline_bind_point,
        .pipelineLayout = VK_NULL_HANDLE,
        .set = request->set,
    };
    VkDescriptorUpdateTemplate descriptor_template = VK_NULL_HANDLE;
    response->vulkan_result = create_template(
        device, &create_info, NULL, &descriptor_template);
    if (response->vulkan_result != VK_SUCCESS) return 0;
    const uint64_t wire_id = bvb_handle_id(
        BVB_OBJECT_DESCRIPTOR_UPDATE_TEMPLATE,
        context->next_descriptor_update_template_serial++);
    result = bvb_handle_table_insert(
        &context->objects, wire_id, request->device_id,
        handle_bits(&descriptor_template, sizeof(descriptor_template)));
    if (result != 0) {
        destroy_template(device, descriptor_template, NULL);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    struct bvb_descriptor_template_metadata *metadata =
        descriptor_template_slot(context, 0U);
    if (metadata == NULL) {
        (void)bvb_handle_table_remove(
            &context->objects, wire_id,
            BVB_OBJECT_DESCRIPTOR_UPDATE_TEMPLATE, NULL);
        destroy_template(device, descriptor_template, NULL);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    *metadata = (struct bvb_descriptor_template_metadata){
        .template_id = wire_id,
        .device_id = request->device_id,
        .entry_count = request->entry_count,
    };
    memcpy(metadata->entries, request->entries,
           request->entry_count * sizeof(request->entries[0]));
    response->object_id = wire_id;
    return 0;
}

int bvb_vulkan_global_context_destroy_descriptor_update_template(
    struct bvb_vulkan_global_context *context, uint64_t template_id,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL) return -EINVAL;
    uint64_t device_id = 0U, template_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, template_id, BVB_OBJECT_DESCRIPTOR_UPDATE_TEMPLATE,
        &device_id, &device, &template_bits);
    if (result != 0) return result;
    PFN_vkDestroyDescriptorUpdateTemplate destroy_template =
        (PFN_vkDestroyDescriptorUpdateTemplate)context->get_device_proc_addr(
            device, "vkDestroyDescriptorUpdateTemplate");
    if (destroy_template == NULL) return -ENOSYS;
    result = bvb_handle_table_remove(
        &context->objects, template_id,
        BVB_OBJECT_DESCRIPTOR_UPDATE_TEMPLATE, NULL);
    if (result == 0) {
        struct bvb_descriptor_template_metadata *metadata =
            descriptor_template_slot(context, template_id);
        if (metadata != NULL && metadata->template_id == template_id)
            *metadata = (struct bvb_descriptor_template_metadata){0};
        destroy_template(device,
            descriptor_update_template_from_bits(template_bits), NULL);
    }
    return result;
}

static int descriptor_template_resolve_image_view(
    const struct bvb_vulkan_global_context *context, uint64_t image_view_id,
    uint64_t expected_device_id, VkImageView *image_view) {
    if (image_view_id == 0U) {
        *image_view = VK_NULL_HANDLE;
        return 0;
    }
    uint64_t image_id = 0U, view_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, image_view_id, BVB_OBJECT_IMAGE_VIEW,
        &image_id, &view_bits);
    uint64_t image_device_id = 0U, image_bits = 0U;
    if (result == 0) {
        result = bvb_handle_table_lookup(
            &context->objects, image_id, BVB_OBJECT_IMAGE,
            &image_device_id, &image_bits);
    }
    if (result != 0 || image_device_id != expected_device_id)
        return result != 0 ? result : -EPROTO;
    *image_view = image_view_from_bits(view_bits);
    return 0;
}

int bvb_vulkan_global_context_update_descriptor_set_with_template(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_descriptor_template_update_request *request,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL) return -EINVAL;
    uint64_t device_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, request->device_id, BVB_OBJECT_DEVICE, NULL,
        &device_bits);
    uint64_t pool_id = 0U, set_bits = 0U, pool_device_id = 0U;
    uint64_t pool_bits = 0U;
    if (result == 0) {
        result = bvb_handle_table_lookup(
            &context->objects, request->descriptor_set_id,
            BVB_OBJECT_DESCRIPTOR_SET, &pool_id, &set_bits);
    }
    if (result == 0) {
        result = bvb_handle_table_lookup(
            &context->objects, pool_id, BVB_OBJECT_DESCRIPTOR_POOL,
            &pool_device_id, &pool_bits);
    }
    uint64_t template_device_id = 0U, template_bits = 0U;
    if (result == 0) {
        result = bvb_handle_table_lookup(
            &context->objects, request->descriptor_update_template_id,
            BVB_OBJECT_DESCRIPTOR_UPDATE_TEMPLATE, &template_device_id,
            &template_bits);
    }
    struct bvb_descriptor_template_metadata *metadata =
        descriptor_template_slot(
            context, request->descriptor_update_template_id);
    if (result != 0 || pool_device_id != request->device_id ||
        template_device_id != request->device_id || metadata == NULL ||
        metadata->template_id != request->descriptor_update_template_id ||
        metadata->device_id != request->device_id) {
        set_error(error, error_size,
                  "descriptor template update lineage failed: lookup=%d "
                  "device=%#llx set=%#llx pool=%#llx pool-device=%#llx "
                  "template=%#llx template-device=%#llx metadata=%#llx",
                  result, (unsigned long long)request->device_id,
                  (unsigned long long)request->descriptor_set_id,
                  (unsigned long long)pool_id,
                  (unsigned long long)pool_device_id,
                  (unsigned long long)
                      request->descriptor_update_template_id,
                  (unsigned long long)template_device_id,
                  (unsigned long long)
                      (metadata == NULL ? 0U : metadata->template_id));
        return result != 0 ? result : -EPROTO;
    }
    uint32_t expected_values = 0U;
    size_t data_size = 0U;
    for (uint32_t entry_index = 0U;
         entry_index < metadata->entry_count; ++entry_index) {
        const struct bvb_vulkan_descriptor_update_template_entry *entry =
            &metadata->entries[entry_index];
        if (entry->descriptor_count >
            BVB_VULKAN_MAX_DESCRIPTOR_TEMPLATE_VALUES - expected_values)
            return -E2BIG;
        expected_values += entry->descriptor_count;
        const size_t value_size =
            descriptor_template_data_size(entry->descriptor_type);
        const uint64_t end = entry->offset +
            (uint64_t)(entry->descriptor_count - 1U) * entry->stride +
            value_size;
        if (value_size == 0U || end > SIZE_MAX ||
            end > BVB_VULKAN_MAX_DESCRIPTOR_UPDATE_TEMPLATE_DATA_SIZE)
            return -EPROTO;
        if ((size_t)end > data_size) data_size = (size_t)end;
    }
    if (request->value_count != expected_values) return -EPROTO;
    uint8_t *data = calloc(1U, data_size);
    if (data == NULL) return -ENOMEM;
    uint32_t value_index = 0U;
    for (uint32_t entry_index = 0U; result == 0 &&
         entry_index < metadata->entry_count; ++entry_index) {
        const struct bvb_vulkan_descriptor_update_template_entry *entry =
            &metadata->entries[entry_index];
        for (uint32_t descriptor = 0U;
             result == 0 && descriptor < entry->descriptor_count;
             ++descriptor, ++value_index) {
            const struct bvb_vulkan_descriptor_template_value *value =
                &request->values[value_index];
            uint8_t *destination = data + entry->offset +
                (uint64_t)descriptor * entry->stride;
            if (value->descriptor_type != entry->descriptor_type) {
                result = -EPROTO;
                break;
            }
            switch ((VkDescriptorType)entry->descriptor_type) {
            case VK_DESCRIPTOR_TYPE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
                VkDescriptorImageInfo info = {
                    .imageLayout = (VkImageLayout)value->image_layout,
                };
                const uint64_t sampler_id = entry->descriptor_type ==
                        VK_DESCRIPTOR_TYPE_SAMPLER
                    ? value->object_id : value->auxiliary_object_id;
                uint64_t sampler_bits = 0U, sampler_device_id = 0U;
                if (sampler_id != 0U) {
                    result = bvb_handle_table_lookup(
                        &context->objects, sampler_id,
                        BVB_OBJECT_SAMPLER, &sampler_device_id,
                        &sampler_bits);
                    if (result == 0 &&
                        sampler_device_id != request->device_id)
                        result = -EPROTO;
                    info.sampler = sampler_from_bits(sampler_bits);
                }
                if (entry->descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLER) {
                    if (value->auxiliary_object_id != 0U)
                        result = -EPROTO;
                } else if (result == 0) {
                    result = descriptor_template_resolve_image_view(
                        context, value->object_id, request->device_id,
                        &info.imageView);
                }
                if (result == 0) memcpy(destination, &info, sizeof(info));
                break;
            }
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
                if (value->object_id != 0U) result = -ENOTSUP;
                const VkBufferView view = VK_NULL_HANDLE;
                if (result == 0) memcpy(destination, &view, sizeof(view));
                break;
            }
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
                uint64_t buffer_bits = 0U, buffer_device_id = 0U;
                if (value->object_id != 0U) {
                    result = bvb_handle_table_lookup(
                        &context->objects, value->object_id,
                        BVB_OBJECT_BUFFER, &buffer_device_id, &buffer_bits);
                    if (result == 0 &&
                        buffer_device_id != request->device_id)
                        result = -EPROTO;
                }
                const VkDescriptorBufferInfo info = {
                    .buffer = buffer_from_bits(buffer_bits),
                    .offset = value->offset,
                    .range = value->range,
                };
                if (result == 0) memcpy(destination, &info, sizeof(info));
                break;
            }
            default:
                result = -ENOTSUP;
                break;
            }
        }
    }
    if (result == 0) {
        const VkDevice device = device_from_bits(device_bits);
        PFN_vkUpdateDescriptorSetWithTemplate update =
            (PFN_vkUpdateDescriptorSetWithTemplate)
                context->get_device_proc_addr(
                    device, "vkUpdateDescriptorSetWithTemplate");
        if (update == NULL) result = -ENOSYS;
        else update(device, descriptor_set_from_bits(set_bits),
                    descriptor_update_template_from_bits(template_bits), data);
    }
    free(data);
    if (result != 0)
        set_error(error, error_size,
                  "descriptor template value translation failed: %d",
                  result);
    return result;
}

int bvb_vulkan_global_context_create_descriptor_pool(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_descriptor_pool_create_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || response == NULL) return -EINVAL;
    *response = (struct bvb_vulkan_object_create_response){0};
    if ((request->flags & ~VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT) !=
            0U ||
        request->max_sets == 0U || request->max_sets > (1U << 20) ||
        request->pool_size_count == 0U ||
        request->pool_size_count > BVB_VULKAN_MAX_DESCRIPTOR_POOL_SIZES) {
        response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return 0;
    }
    VkDescriptorPoolSize sizes[BVB_VULKAN_MAX_DESCRIPTOR_POOL_SIZES] = {0};
    for (uint32_t index = 0U; index < request->pool_size_count; ++index) {
        if (!core_descriptor_type_supported(
                request->pool_sizes[index].descriptor_type) ||
            request->pool_sizes[index].descriptor_count == 0U ||
            request->pool_sizes[index].descriptor_count > (1U << 20)) {
            response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
            return 0;
        }
        sizes[index] = (VkDescriptorPoolSize){
            .type = (VkDescriptorType)
                request->pool_sizes[index].descriptor_type,
            .descriptorCount = request->pool_sizes[index].descriptor_count,
        };
    }
    uint64_t device_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, request->device_id, BVB_OBJECT_DEVICE, NULL,
        &device_bits);
    if (result != 0) {
        set_error(error, error_size,
                  "descriptor pool device lookup failed: %d", result);
        return result;
    }
    const VkDevice device = device_from_bits(device_bits);
    PFN_vkCreateDescriptorPool create_pool =
        (PFN_vkCreateDescriptorPool)context->get_device_proc_addr(
            device, "vkCreateDescriptorPool");
    PFN_vkDestroyDescriptorPool destroy_pool =
        (PFN_vkDestroyDescriptorPool)context->get_device_proc_addr(
            device, "vkDestroyDescriptorPool");
    if (create_pool == NULL || destroy_pool == NULL) return -ENOSYS;
    const VkDescriptorPoolCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = request->flags,
        .maxSets = request->max_sets,
        .poolSizeCount = request->pool_size_count,
        .pPoolSizes = sizes,
    };
    VkDescriptorPool pool = VK_NULL_HANDLE;
    response->vulkan_result = create_pool(device, &create_info, NULL, &pool);
    if (response->vulkan_result != VK_SUCCESS) return 0;
    const uint64_t wire_id = bvb_handle_id(
        BVB_OBJECT_DESCRIPTOR_POOL,
        context->next_descriptor_pool_serial++);
    result = bvb_handle_table_insert(
        &context->objects, wire_id, request->device_id,
        handle_bits(&pool, sizeof(pool)));
    if (result != 0) {
        destroy_pool(device, pool, NULL);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    response->object_id = wire_id;
    return 0;
}

int bvb_vulkan_global_context_destroy_descriptor_pool(
    struct bvb_vulkan_global_context *context, uint64_t pool_id,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    uint64_t device_id = 0U, pool_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, pool_id, BVB_OBJECT_DESCRIPTOR_POOL, &device_id,
        &device, &pool_bits);
    if (result != 0) return result;
    PFN_vkDestroyDescriptorPool destroy_pool =
        (PFN_vkDestroyDescriptorPool)context->get_device_proc_addr(
            device, "vkDestroyDescriptorPool");
    if (destroy_pool == NULL) return -ENOSYS;
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_DESCRIPTOR_SET &&
            entry->parent_id == pool_id) {
            result = bvb_handle_table_remove(
                &context->objects, entry->wire_id,
                BVB_OBJECT_DESCRIPTOR_SET, NULL);
            if (result != 0) return result;
        }
    }
    result = bvb_handle_table_remove(
        &context->objects, pool_id, BVB_OBJECT_DESCRIPTOR_POOL, NULL);
    if (result == 0) {
        destroy_pool(device, descriptor_pool_from_bits(pool_bits), NULL);
    }
    return result;
}

int bvb_vulkan_global_context_allocate_descriptor_sets(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_descriptor_set_allocate_request *request,
    struct bvb_vulkan_descriptor_set_allocate_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || response == NULL) return -EINVAL;
    *response = (struct bvb_vulkan_descriptor_set_allocate_response){0};
    if (request->descriptor_set_count == 0U ||
        request->descriptor_set_count >
            BVB_VULKAN_MAX_DESCRIPTOR_SETS_PER_ALLOCATE ||
        request->descriptor_set_count >
            context->objects.capacity - context->objects.count) {
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    uint64_t device_id = 0U, pool_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, request->descriptor_pool_id,
        BVB_OBJECT_DESCRIPTOR_POOL, &device_id, &pool_bits);
    uint64_t device_bits = 0U;
    if (result == 0) {
        result = bvb_handle_table_lookup(
            &context->objects, device_id, BVB_OBJECT_DEVICE, NULL,
            &device_bits);
    }
    if (result != 0) return result;
    VkDescriptorSetLayout
        layouts[BVB_VULKAN_MAX_DESCRIPTOR_SETS_PER_ALLOCATE] = {0};
    for (uint32_t index = 0U; index < request->descriptor_set_count; ++index) {
        uint64_t layout_device_id = 0U, layout_bits = 0U;
        result = bvb_handle_table_lookup(
            &context->objects, request->set_layout_ids[index],
            BVB_OBJECT_DESCRIPTOR_SET_LAYOUT, &layout_device_id,
            &layout_bits);
        if (result != 0 || layout_device_id != device_id) {
            return result != 0 ? result : -EPROTO;
        }
        layouts[index] = descriptor_set_layout_from_bits(layout_bits);
    }
    const VkDevice device = device_from_bits(device_bits);
    PFN_vkAllocateDescriptorSets allocate_sets =
        (PFN_vkAllocateDescriptorSets)context->get_device_proc_addr(
            device, "vkAllocateDescriptorSets");
    if (allocate_sets == NULL) return -ENOSYS;
    const VkDescriptorSetAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool_from_bits(pool_bits),
        .descriptorSetCount = request->descriptor_set_count,
        .pSetLayouts = layouts,
    };
    VkDescriptorSet sets[BVB_VULKAN_MAX_DESCRIPTOR_SETS_PER_ALLOCATE] = {0};
    response->vulkan_result =
        allocate_sets(device, &allocate_info, sets);
    if (response->vulkan_result != VK_SUCCESS) return 0;
    response->descriptor_set_count = request->descriptor_set_count;
    for (uint32_t index = 0U; index < request->descriptor_set_count; ++index) {
        const uint64_t wire_id = bvb_handle_id(
            BVB_OBJECT_DESCRIPTOR_SET,
            context->next_descriptor_set_serial++);
        result = bvb_handle_table_insert(
            &context->objects, wire_id, request->descriptor_pool_id,
            handle_bits(&sets[index], sizeof(sets[index])));
        if (result != 0) {
            response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
            response->descriptor_set_count = 0U;
            return 0;
        }
        response->descriptor_set_ids[index] = wire_id;
    }
    return 0;
}

static float wire_float(uint32_t bits) {
    float value = 0.0F;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

int bvb_vulkan_global_context_create_sampler(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_sampler_create_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || response == NULL) return -EINVAL;
    *response = (struct bvb_vulkan_object_create_response){0};
    if (request->flags != 0U || request->mag_filter > VK_FILTER_LINEAR ||
        request->min_filter > VK_FILTER_LINEAR ||
        request->mipmap_mode > VK_SAMPLER_MIPMAP_MODE_LINEAR ||
        request->address_mode_u > VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE ||
        request->address_mode_v > VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE ||
        request->address_mode_w > VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE ||
        request->anisotropy_enable > 1U || request->compare_enable > 1U ||
        request->compare_op > VK_COMPARE_OP_ALWAYS ||
        request->border_color > VK_BORDER_COLOR_INT_OPAQUE_WHITE ||
        request->unnormalized_coordinates > 1U) {
        response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return 0;
    }
    uint64_t device_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, request->device_id, BVB_OBJECT_DEVICE, NULL,
        &device_bits);
    if (result != 0) return result;
    const VkDevice device = device_from_bits(device_bits);
    PFN_vkCreateSampler create_sampler =
        (PFN_vkCreateSampler)context->get_device_proc_addr(
            device, "vkCreateSampler");
    PFN_vkDestroySampler destroy_sampler =
        (PFN_vkDestroySampler)context->get_device_proc_addr(
            device, "vkDestroySampler");
    if (create_sampler == NULL || destroy_sampler == NULL) return -ENOSYS;
    const VkSamplerCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .flags = request->flags,
        .magFilter = (VkFilter)request->mag_filter,
        .minFilter = (VkFilter)request->min_filter,
        .mipmapMode = (VkSamplerMipmapMode)request->mipmap_mode,
        .addressModeU = (VkSamplerAddressMode)request->address_mode_u,
        .addressModeV = (VkSamplerAddressMode)request->address_mode_v,
        .addressModeW = (VkSamplerAddressMode)request->address_mode_w,
        .mipLodBias = wire_float(request->mip_lod_bias_bits),
        .anisotropyEnable = request->anisotropy_enable,
        .maxAnisotropy = wire_float(request->max_anisotropy_bits),
        .compareEnable = request->compare_enable,
        .compareOp = (VkCompareOp)request->compare_op,
        .minLod = wire_float(request->min_lod_bits),
        .maxLod = wire_float(request->max_lod_bits),
        .borderColor = (VkBorderColor)request->border_color,
        .unnormalizedCoordinates = request->unnormalized_coordinates,
    };
    VkSampler sampler = VK_NULL_HANDLE;
    response->vulkan_result =
        create_sampler(device, &create_info, NULL, &sampler);
    if (response->vulkan_result != VK_SUCCESS) return 0;
    const uint64_t wire_id = bvb_handle_id(
        BVB_OBJECT_SAMPLER, context->next_sampler_serial++);
    result = bvb_handle_table_insert(
        &context->objects, wire_id, request->device_id,
        handle_bits(&sampler, sizeof(sampler)));
    if (result != 0) {
        destroy_sampler(device, sampler, NULL);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    response->object_id = wire_id;
    return 0;
}

int bvb_vulkan_global_context_destroy_sampler(
    struct bvb_vulkan_global_context *context, uint64_t sampler_id,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    uint64_t device_id = 0U, sampler_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, sampler_id, BVB_OBJECT_SAMPLER, &device_id, &device,
        &sampler_bits);
    if (result != 0) return result;
    PFN_vkDestroySampler destroy_sampler =
        (PFN_vkDestroySampler)context->get_device_proc_addr(
            device, "vkDestroySampler");
    if (destroy_sampler == NULL) return -ENOSYS;
    result = bvb_handle_table_remove(
        &context->objects, sampler_id, BVB_OBJECT_SAMPLER, NULL);
    if (result == 0) {
        destroy_sampler(device, sampler_from_bits(sampler_bits), NULL);
    }
    return result;
}

int bvb_vulkan_global_context_update_descriptors(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_descriptor_update_request *request,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || request->write_count == 0U ||
        request->write_count > BVB_VULKAN_MAX_DESCRIPTOR_WRITES ||
        request->sampler_count == 0U ||
        request->sampler_count > BVB_VULKAN_MAX_DESCRIPTOR_SAMPLERS) {
        return -EINVAL;
    }
    uint64_t device_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, request->device_id, BVB_OBJECT_DEVICE, NULL,
        &device_bits);
    if (result != 0) {
        set_error(error, error_size,
                  "descriptor update device lookup failed: %d", result);
        return result;
    }
    VkDescriptorImageInfo
        image_infos[BVB_VULKAN_MAX_DESCRIPTOR_SAMPLERS] = {0};
    for (uint32_t index = 0U; index < request->sampler_count; ++index) {
        uint64_t sampler_device_id = 0U, sampler_bits = 0U;
        result = bvb_handle_table_lookup(
            &context->objects, request->sampler_ids[index],
            BVB_OBJECT_SAMPLER, &sampler_device_id, &sampler_bits);
        if (result != 0 || sampler_device_id != request->device_id) {
            set_error(error, error_size,
                      "descriptor update sampler[%u] lineage failed: "
                      "lookup=%d parent=%#llx expected=%#llx",
                      index, result, (unsigned long long)sampler_device_id,
                      (unsigned long long)request->device_id);
            return result != 0 ? result : -EPROTO;
        }
        image_infos[index] = (VkDescriptorImageInfo){
            .sampler = sampler_from_bits(sampler_bits),
            .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
    }
    VkWriteDescriptorSet writes[BVB_VULKAN_MAX_DESCRIPTOR_WRITES] = {0};
    for (uint32_t index = 0U; index < request->write_count; ++index) {
        const struct bvb_vulkan_descriptor_write *wire =
            &request->writes[index];
        if (wire->descriptor_type != VK_DESCRIPTOR_TYPE_SAMPLER ||
            wire->descriptor_count == 0U ||
            wire->first_sampler >= request->sampler_count ||
            wire->descriptor_count >
                request->sampler_count - wire->first_sampler) {
            return -EPROTO;
        }
        uint64_t pool_id = 0U, set_bits = 0U;
        result = bvb_handle_table_lookup(
            &context->objects, wire->descriptor_set_id,
            BVB_OBJECT_DESCRIPTOR_SET, &pool_id, &set_bits);
        if (result != 0) {
            set_error(error, error_size,
                      "descriptor update set[%u] lookup failed: %d",
                      index, result);
            return result;
        }
        uint64_t set_device_id = 0U, pool_bits = 0U;
        result = bvb_handle_table_lookup(
            &context->objects, pool_id, BVB_OBJECT_DESCRIPTOR_POOL,
            &set_device_id, &pool_bits);
        if (result != 0 || set_device_id != request->device_id) {
            set_error(error, error_size,
                      "descriptor update set[%u] pool lineage failed: "
                      "lookup=%d pool=%#llx parent=%#llx expected=%#llx",
                      index, result, (unsigned long long)pool_id,
                      (unsigned long long)set_device_id,
                      (unsigned long long)request->device_id);
            return result != 0 ? result : -EPROTO;
        }
        writes[index] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = descriptor_set_from_bits(set_bits),
            .dstBinding = wire->dst_binding,
            .dstArrayElement = wire->dst_array_element,
            .descriptorCount = wire->descriptor_count,
            .descriptorType = (VkDescriptorType)wire->descriptor_type,
            .pImageInfo = &image_infos[wire->first_sampler],
        };
    }
    const VkDevice device = device_from_bits(device_bits);
    PFN_vkUpdateDescriptorSets update =
        (PFN_vkUpdateDescriptorSets)context->get_device_proc_addr(
            device, "vkUpdateDescriptorSets");
    if (update == NULL) return -ENOSYS;
    update(device, request->write_count, writes, 0U, NULL);
    return 0;
}

int bvb_vulkan_global_context_create_pipeline_layout(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_pipeline_layout_create_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || response == NULL) return -EINVAL;
    *response = (struct bvb_vulkan_object_create_response){0};
    const VkPipelineLayoutCreateFlags allowed_flags =
        VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT;
    const VkShaderStageFlags allowed_stages =
        VK_SHADER_STAGE_VERTEX_BIT |
        VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
        VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT |
        VK_SHADER_STAGE_GEOMETRY_BIT |
        VK_SHADER_STAGE_FRAGMENT_BIT |
        VK_SHADER_STAGE_COMPUTE_BIT;
    if ((request->flags & ~allowed_flags) != 0U ||
        request->set_layout_count > BVB_VULKAN_MAX_PIPELINE_SET_LAYOUTS ||
        request->push_constant_range_count >
            BVB_VULKAN_MAX_PIPELINE_PUSH_CONSTANT_RANGES) {
        response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return 0;
    }
    uint64_t device_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, request->device_id, BVB_OBJECT_DEVICE, NULL,
        &device_bits);
    if (result != 0) {
        set_error(error, error_size,
                  "pipeline layout device lookup failed: %d", result);
        return result;
    }
    VkDescriptorSetLayout
        set_layouts[BVB_VULKAN_MAX_PIPELINE_SET_LAYOUTS] = {0};
    for (uint32_t index = 0U; index < request->set_layout_count; ++index) {
        const uint64_t layout_id = request->set_layout_ids[index];
        if (layout_id == 0U) {
            if ((request->flags &
                 VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT) == 0U) {
                response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
                return 0;
            }
            continue;
        }
        uint64_t layout_device_id = 0U, layout_bits = 0U;
        result = bvb_handle_table_lookup(
            &context->objects, layout_id, BVB_OBJECT_DESCRIPTOR_SET_LAYOUT,
            &layout_device_id, &layout_bits);
        if (result != 0 || layout_device_id != request->device_id) {
            set_error(error, error_size,
                      "pipeline layout set[%u] lineage failed: %d", index,
                      result);
            return result != 0 ? result : -EPROTO;
        }
        set_layouts[index] = descriptor_set_layout_from_bits(layout_bits);
    }
    VkPushConstantRange
        ranges[BVB_VULKAN_MAX_PIPELINE_PUSH_CONSTANT_RANGES] = {0};
    VkShaderStageFlags used_stages = 0U;
    for (uint32_t index = 0U;
         index < request->push_constant_range_count; ++index) {
        const struct bvb_vulkan_pipeline_push_constant_range *wire =
            &request->push_constant_ranges[index];
        if (wire->stage_flags == 0U ||
            (wire->stage_flags & ~allowed_stages) != 0U ||
            (wire->stage_flags & used_stages) != 0U || wire->size == 0U ||
            (wire->offset & 3U) != 0U || (wire->size & 3U) != 0U ||
            wire->offset > 256U || wire->size > 256U - wire->offset) {
            response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
            return 0;
        }
        ranges[index] = (VkPushConstantRange){
            .stageFlags = wire->stage_flags,
            .offset = wire->offset,
            .size = wire->size,
        };
        used_stages |= wire->stage_flags;
    }
    const VkDevice device = device_from_bits(device_bits);
    PFN_vkCreatePipelineLayout create_pipeline_layout =
        (PFN_vkCreatePipelineLayout)context->get_device_proc_addr(
            device, "vkCreatePipelineLayout");
    PFN_vkDestroyPipelineLayout destroy_pipeline_layout =
        (PFN_vkDestroyPipelineLayout)context->get_device_proc_addr(
            device, "vkDestroyPipelineLayout");
    if (create_pipeline_layout == NULL || destroy_pipeline_layout == NULL) {
        return -ENOSYS;
    }
    const VkPipelineLayoutCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .flags = request->flags,
        .setLayoutCount = request->set_layout_count,
        .pSetLayouts = request->set_layout_count == 0U ? NULL : set_layouts,
        .pushConstantRangeCount = request->push_constant_range_count,
        .pPushConstantRanges = request->push_constant_range_count == 0U
            ? NULL : ranges,
    };
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    response->vulkan_result = create_pipeline_layout(
        device, &create_info, NULL, &pipeline_layout);
    if (response->vulkan_result != VK_SUCCESS) return 0;
    const uint64_t wire_id = bvb_handle_id(
        BVB_OBJECT_PIPELINE_LAYOUT, context->next_pipeline_layout_serial++);
    result = bvb_handle_table_insert(
        &context->objects, wire_id, request->device_id,
        handle_bits(&pipeline_layout, sizeof(pipeline_layout)));
    if (result != 0) {
        destroy_pipeline_layout(device, pipeline_layout, NULL);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    response->object_id = wire_id;
    return 0;
}

int bvb_vulkan_global_context_destroy_pipeline_layout(
    struct bvb_vulkan_global_context *context, uint64_t pipeline_layout_id,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    uint64_t device_id = 0U, pipeline_layout_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, pipeline_layout_id, BVB_OBJECT_PIPELINE_LAYOUT, &device_id,
        &device, &pipeline_layout_bits);
    if (result != 0) return result;
    PFN_vkDestroyPipelineLayout destroy_pipeline_layout =
        (PFN_vkDestroyPipelineLayout)context->get_device_proc_addr(
            device, "vkDestroyPipelineLayout");
    if (destroy_pipeline_layout == NULL) return -ENOSYS;
    result = bvb_handle_table_remove(
        &context->objects, pipeline_layout_id, BVB_OBJECT_PIPELINE_LAYOUT,
        NULL);
    if (result == 0) {
        destroy_pipeline_layout(
            device, pipeline_layout_from_bits(pipeline_layout_bits), NULL);
    }
    return result;
}

static bool graphics_dynamic_states_match_dxvk_null_fragment(
    const struct bvb_vulkan_graphics_pipeline_create_request *request) {
    static const uint32_t expected[] = {
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP,
    };
    return request->dynamic_state_count ==
            (uint32_t)(sizeof(expected) / sizeof(expected[0])) &&
        memcmp(request->dynamic_states, expected, sizeof(expected)) == 0;
}

int bvb_vulkan_global_context_create_graphics_pipeline(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_graphics_pipeline_create_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || response == NULL) return -EINVAL;
    *response = (struct bvb_vulkan_object_create_response){0};
    if (request->flags_2 != VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR ||
        request->library_flags !=
            VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT ||
        request->shader_stage != VK_SHADER_STAGE_FRAGMENT_BIT ||
        request->shader_word_count < 5U ||
        request->shader_word_count >
            BVB_VULKAN_MAX_GRAPHICS_PIPELINE_SHADER_WORDS ||
        request->shader_words[0] != UINT32_C(0x07230203) ||
        !graphics_dynamic_states_match_dxvk_null_fragment(request)) {
        response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return 0;
    }
    uint64_t device_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, request->device_id, BVB_OBJECT_DEVICE, NULL,
        &device_bits);
    if (result != 0) return result;
    uint64_t layout_device_id = 0U, layout_bits = 0U;
    result = bvb_handle_table_lookup(
        &context->objects, request->pipeline_layout_id,
        BVB_OBJECT_PIPELINE_LAYOUT, &layout_device_id, &layout_bits);
    if (result != 0 || layout_device_id != request->device_id) {
        set_error(error, error_size,
                  "graphics pipeline layout lineage failed: %d", result);
        return result != 0 ? result : -EPROTO;
    }
    const VkDevice device = device_from_bits(device_bits);
    PFN_vkCreateGraphicsPipelines create_graphics_pipelines =
        (PFN_vkCreateGraphicsPipelines)context->get_device_proc_addr(
            device, "vkCreateGraphicsPipelines");
    PFN_vkDestroyPipeline destroy_pipeline =
        (PFN_vkDestroyPipeline)context->get_device_proc_addr(
            device, "vkDestroyPipeline");
    if (create_graphics_pipelines == NULL || destroy_pipeline == NULL) {
        return -ENOSYS;
    }
    const VkPipelineRenderingCreateInfo rendering_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
    };
    const VkPipelineCreateFlags2CreateInfo flags_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
        .pNext = &rendering_info,
        .flags = request->flags_2,
    };
    const VkGraphicsPipelineLibraryCreateInfoEXT library_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
        .pNext = &flags_info,
        .flags = request->library_flags,
    };
    const VkShaderModuleCreateInfo module_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = request->shader_word_count * sizeof(uint32_t),
        .pCode = request->shader_words,
    };
    const VkPipelineShaderStageCreateInfo stage_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .pNext = &module_info,
        .stage = request->shader_stage,
        .pName = "main",
    };
    const VkPipelineDepthStencilStateCreateInfo depth_stencil_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
    };
    const VkPipelineDynamicStateCreateInfo dynamic_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = request->dynamic_state_count,
        .pDynamicStates = request->dynamic_states,
    };
    const VkGraphicsPipelineCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &library_info,
        .stageCount = 1U,
        .pStages = &stage_info,
        .pDepthStencilState = &depth_stencil_info,
        .pDynamicState = &dynamic_info,
        .layout = pipeline_layout_from_bits(layout_bits),
        .basePipelineIndex = -1,
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    response->vulkan_result = create_graphics_pipelines(
        device, VK_NULL_HANDLE, 1U, &create_info, NULL, &pipeline);
    if (response->vulkan_result != VK_SUCCESS) return 0;
    const uint64_t wire_id = bvb_handle_id(
        BVB_OBJECT_PIPELINE, context->next_pipeline_serial++);
    result = bvb_handle_table_insert(
        &context->objects, wire_id, request->device_id,
        handle_bits(&pipeline, sizeof(pipeline)));
    if (result != 0) {
        destroy_pipeline(device, pipeline, NULL);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    response->object_id = wire_id;
    return 0;
}

static int general_graphics_blob_resolve(
    uint8_t *mapping, uint32_t mapping_size, const void *encoded,
    uint32_t count, size_t element_size, size_t alignment, void **output) {
    if (output == NULL || element_size == 0U || alignment == 0U ||
        (alignment & (alignment - 1U)) != 0U) {
        return -EINVAL;
    }
    *output = NULL;
    const uintptr_t offset = (uintptr_t)encoded;
    if (offset == 0U) return count == 0U ? 0 : -EPROTO;
    if (offset < BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_BLOB_HEADER_SIZE ||
        (offset & (alignment - 1U)) != 0U || offset > mapping_size ||
        count > SIZE_MAX / element_size) {
        return -EPROTO;
    }
    const size_t bytes = (size_t)count * element_size;
    if (bytes > (size_t)mapping_size - offset) return -EPROTO;
    *output = mapping + offset;
    return 0;
}

static bool general_graphics_blob_header_matches(
    const uint8_t *mapping, uint32_t mapping_size) {
    return mapping_size >=
            BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_BLOB_HEADER_SIZE &&
        bvb_wire_get_u32(mapping) ==
            BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_BLOB_MAGIC &&
        bvb_wire_get_u32(mapping + 4U) ==
            BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_BLOB_VERSION &&
        bvb_wire_get_u32(mapping + 8U) == mapping_size &&
        bvb_wire_get_u32(mapping + 16U) ==
            sizeof(VkGraphicsPipelineCreateInfo) &&
        bvb_wire_get_u32(mapping + 20U) ==
            sizeof(VkPipelineShaderStageCreateInfo) &&
        bvb_wire_get_u32(mapping + 24U) == sizeof(VkShaderModuleCreateInfo) &&
        bvb_wire_get_u32(mapping + 28U) == sizeof(VkSpecializationInfo) &&
        bvb_wire_get_u32(mapping + 32U) ==
            sizeof(VkPipelineVertexInputStateCreateInfo) &&
        bvb_wire_get_u32(mapping + 36U) ==
            sizeof(VkPipelineInputAssemblyStateCreateInfo) &&
        bvb_wire_get_u32(mapping + 40U) ==
            sizeof(VkPipelineViewportStateCreateInfo) &&
        bvb_wire_get_u32(mapping + 44U) ==
            sizeof(VkPipelineRasterizationStateCreateInfo) &&
        bvb_wire_get_u32(mapping + 48U) ==
            sizeof(VkPipelineMultisampleStateCreateInfo) &&
        bvb_wire_get_u32(mapping + 52U) ==
            sizeof(VkPipelineDepthStencilStateCreateInfo) &&
        bvb_wire_get_u32(mapping + 56U) ==
            sizeof(VkPipelineColorBlendStateCreateInfo) &&
        bvb_wire_get_u32(mapping + 60U) ==
            sizeof(VkPipelineDynamicStateCreateInfo) &&
        bvb_wire_get_u32(mapping + 64U) ==
            sizeof(VkPipelineRenderingCreateInfo) &&
        bvb_wire_get_u32(mapping + 68U) ==
            sizeof(VkPipelineTessellationStateCreateInfo) &&
        bvb_wire_get_u32(mapping + 72U) == sizeof(VkViewport) &&
        bvb_wire_get_u32(mapping + 76U) == sizeof(VkRect2D) &&
        bvb_wire_get_u32(mapping + 80U) ==
            sizeof(VkPipelineRasterizationDepthClipStateCreateInfoEXT) &&
        bvb_wire_get_u32(mapping + 84U) == 0U;
}

int bvb_vulkan_global_context_create_general_graphics_pipeline(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_builtin_graphics_pipeline_create_request *request,
    int blob_fd, struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || response == NULL || blob_fd < 0 ||
        request->schema !=
            BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_BLOB_VERSION ||
        request->blob_bytes <
            BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_BLOB_HEADER_SIZE ||
        request->blob_bytes >
            BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_BLOB_MAX_SIZE ||
        (request->blob_bytes & 7U) != 0U) {
        return -EINVAL;
    }
    *response = (struct bvb_vulkan_object_create_response){0};
    struct stat metadata;
    const int required_seals =
        F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_WRITE | F_SEAL_SEAL;
    const int seals = fcntl(blob_fd, F_GET_SEALS);
    if (fstat(blob_fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_size != (off_t)request->blob_bytes || seals < 0 ||
        (seals & required_seals) != required_seals) {
        set_error(error, error_size,
                  "general graphics blob size/seals are invalid");
        return -EPROTO;
    }
    uint8_t *mapping = mmap(
        NULL, request->blob_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE,
        blob_fd, 0);
    if (mapping == MAP_FAILED) return -errno;
    int result = general_graphics_blob_header_matches(
        mapping, request->blob_bytes) ? 0 : -EPROTO;
    VkGraphicsPipelineCreateInfo *info = NULL;
    if (result == 0) {
        const uint32_t root_offset = bvb_wire_get_u32(mapping + 12U);
        result = general_graphics_blob_resolve(
            mapping, request->blob_bytes,
            (const void *)(uintptr_t)root_offset, 1U, sizeof(*info), 8U,
            (void **)&info);
    }
    if (result == 0 &&
        (info->sType != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO ||
         info->flags != 0U || info->stageCount == 0U ||
         info->stageCount >
             BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_STAGES ||
         info->layout == VK_NULL_HANDLE || info->renderPass != VK_NULL_HANDLE ||
         info->subpass != 0U ||
         info->basePipelineHandle != VK_NULL_HANDLE ||
         info->basePipelineIndex != -1)) {
        result = -EPROTO;
    }
    VkPipelineRenderingCreateInfo *rendering = NULL;
    VkPipelineShaderStageCreateInfo *stages = NULL;
    if (result == 0)
        result = general_graphics_blob_resolve(
            mapping, request->blob_bytes, info->pNext, 1U,
            sizeof(*rendering), 8U, (void **)&rendering);
    if (result == 0)
        result = general_graphics_blob_resolve(
            mapping, request->blob_bytes, info->pStages, info->stageCount,
            sizeof(*stages), 8U, (void **)&stages);
    if (result == 0 &&
        (rendering->sType !=
             VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO ||
         rendering->pNext != NULL ||
         rendering->colorAttachmentCount >
             BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_COLOR_ATTACHMENTS)) {
        result = -EPROTO;
    }
    VkFormat *color_formats = NULL;
    if (result == 0)
        result = general_graphics_blob_resolve(
            mapping, request->blob_bytes,
            rendering->pColorAttachmentFormats,
            rendering->colorAttachmentCount, sizeof(VkFormat), 4U,
            (void **)&color_formats);
    if (result == 0) {
        rendering->pColorAttachmentFormats = color_formats;
        info->pNext = rendering;
        info->pStages = stages;
    }
    for (uint32_t index = 0U;
         result == 0 && index < info->stageCount; ++index) {
        VkPipelineShaderStageCreateInfo *stage = &stages[index];
        VkShaderModuleCreateInfo *module = NULL;
        char *name = NULL;
        if (stage->sType !=
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO ||
            stage->flags != 0U || stage->module != VK_NULL_HANDLE) {
            result = -EPROTO;
            break;
        }
        result = general_graphics_blob_resolve(
            mapping, request->blob_bytes, stage->pNext, 1U,
            sizeof(*module), 8U, (void **)&module);
        if (result == 0)
            result = general_graphics_blob_resolve(
                mapping, request->blob_bytes, stage->pName, 5U, 1U, 1U,
                (void **)&name);
        uint32_t *code = NULL;
        if (result == 0 &&
            (module->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO ||
             module->pNext != NULL || module->flags != 0U ||
             module->codeSize < 5U * sizeof(uint32_t) ||
             (module->codeSize & 3U) != 0U ||
             module->codeSize >
                 BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_BLOB_MAX_SIZE ||
             memcmp(name, "main", 5U) != 0)) {
            result = -EPROTO;
        }
        if (result == 0)
            result = general_graphics_blob_resolve(
                mapping, request->blob_bytes, module->pCode,
                (uint32_t)(module->codeSize / sizeof(uint32_t)),
                sizeof(uint32_t), 4U, (void **)&code);
        if (result == 0 && code[0] != UINT32_C(0x07230203)) result = -EPROTO;
        VkSpecializationInfo *specialization = NULL;
        if (result == 0 && stage->pSpecializationInfo != NULL) {
            result = general_graphics_blob_resolve(
                mapping, request->blob_bytes, stage->pSpecializationInfo, 1U,
                sizeof(*specialization), 8U, (void **)&specialization);
            if (result == 0 &&
                (specialization->mapEntryCount >
                     BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_SPEC_ENTRIES ||
                 specialization->dataSize >
                     BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_SPEC_BYTES))
                result = -EPROTO;
            VkSpecializationMapEntry *entries = NULL;
            void *data = NULL;
            if (result == 0)
                result = general_graphics_blob_resolve(
                    mapping, request->blob_bytes,
                    specialization->pMapEntries,
                    specialization->mapEntryCount,
                    sizeof(VkSpecializationMapEntry), 8U,
                    (void **)&entries);
            if (result == 0)
                result = general_graphics_blob_resolve(
                    mapping, request->blob_bytes, specialization->pData,
                    (uint32_t)specialization->dataSize, 1U, 1U, &data);
            for (uint32_t entry = 0U;
                 result == 0 && entry < specialization->mapEntryCount;
                 ++entry) {
                if (entries[entry].offset > specialization->dataSize ||
                    entries[entry].size > specialization->dataSize -
                        entries[entry].offset)
                    result = -EPROTO;
            }
            if (result == 0) {
                specialization->pMapEntries = entries;
                specialization->pData = data;
            }
        }
        if (result == 0) {
            module->pCode = code;
            stage->pNext = module;
            stage->pName = name;
            stage->pSpecializationInfo = specialization;
        }
    }
#define BVB_RESOLVE_STATE(field, type, stype, required) do { \
    type *resolved = NULL; \
    if (result == 0 && ((required) || info->field != NULL)) \
        result = general_graphics_blob_resolve( \
            mapping, request->blob_bytes, info->field, 1U, \
            sizeof(type), 8U, (void **)&resolved); \
    if (result == 0 && resolved != NULL && \
        (resolved->sType != (stype) || resolved->pNext != NULL || \
         resolved->flags != 0U)) result = -EPROTO; \
    if (result == 0) info->field = resolved; \
} while (0)
    BVB_RESOLVE_STATE(
        pVertexInputState, VkPipelineVertexInputStateCreateInfo,
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, true);
    if (result == 0) {
        VkPipelineVertexInputStateCreateInfo *vertex =
            (VkPipelineVertexInputStateCreateInfo *)info->pVertexInputState;
        if (vertex->vertexBindingDescriptionCount >
                BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_VERTEX_BINDINGS ||
            vertex->vertexAttributeDescriptionCount >
                BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_VERTEX_ATTRIBUTES)
            result = -EPROTO;
        VkVertexInputBindingDescription *bindings = NULL;
        VkVertexInputAttributeDescription *attributes = NULL;
        if (result == 0)
            result = general_graphics_blob_resolve(
                mapping, request->blob_bytes,
                vertex->pVertexBindingDescriptions,
                vertex->vertexBindingDescriptionCount,
                sizeof(*bindings), 4U, (void **)&bindings);
        if (result == 0)
            result = general_graphics_blob_resolve(
                mapping, request->blob_bytes,
                vertex->pVertexAttributeDescriptions,
                vertex->vertexAttributeDescriptionCount,
                sizeof(*attributes), 4U, (void **)&attributes);
        if (result == 0) {
            vertex->pVertexBindingDescriptions = bindings;
            vertex->pVertexAttributeDescriptions = attributes;
        }
    }
    BVB_RESOLVE_STATE(
        pInputAssemblyState, VkPipelineInputAssemblyStateCreateInfo,
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, true);
    BVB_RESOLVE_STATE(
        pTessellationState, VkPipelineTessellationStateCreateInfo,
        VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO, false);
    BVB_RESOLVE_STATE(
        pViewportState, VkPipelineViewportStateCreateInfo,
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, true);
    if (result == 0) {
        VkPipelineViewportStateCreateInfo *viewport =
            (VkPipelineViewportStateCreateInfo *)info->pViewportState;
        if (viewport->viewportCount >
                BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_VIEWPORTS ||
            viewport->scissorCount >
                BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_VIEWPORTS)
            result = -EPROTO;
        VkViewport *viewports = NULL;
        VkRect2D *scissors = NULL;
        if (result == 0)
            result = general_graphics_blob_resolve(
                mapping, request->blob_bytes, viewport->pViewports,
                viewport->viewportCount, sizeof(*viewports), 4U,
                (void **)&viewports);
        if (result == 0)
            result = general_graphics_blob_resolve(
                mapping, request->blob_bytes, viewport->pScissors,
                viewport->scissorCount, sizeof(*scissors), 4U,
                (void **)&scissors);
        if (result == 0) {
            viewport->pViewports = viewports;
            viewport->pScissors = scissors;
        }
    }
    VkPipelineRasterizationStateCreateInfo *rasterization = NULL;
    if (result == 0)
        result = general_graphics_blob_resolve(
            mapping, request->blob_bytes, info->pRasterizationState, 1U,
            sizeof(*rasterization), 8U, (void **)&rasterization);
    if (result == 0 &&
        (rasterization->sType !=
             VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO ||
         rasterization->flags != 0U))
        result = -EPROTO;
    VkPipelineRasterizationDepthClipStateCreateInfoEXT *depth_clip = NULL;
    if (result == 0 && rasterization->pNext != NULL)
        result = general_graphics_blob_resolve(
            mapping, request->blob_bytes, rasterization->pNext, 1U,
            sizeof(*depth_clip), 8U, (void **)&depth_clip);
    if (result == 0 && depth_clip != NULL &&
        (depth_clip->sType !=
             VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT ||
         depth_clip->pNext != NULL || depth_clip->flags != 0U ||
         depth_clip->depthClipEnable > VK_TRUE))
        result = -EPROTO;
    if (result == 0) {
        rasterization->pNext = depth_clip;
        info->pRasterizationState = rasterization;
    }
    BVB_RESOLVE_STATE(
        pMultisampleState, VkPipelineMultisampleStateCreateInfo,
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, true);
    if (result == 0) {
        VkPipelineMultisampleStateCreateInfo *multisample =
            (VkPipelineMultisampleStateCreateInfo *)info->pMultisampleState;
        const uint32_t mask_count =
            ((uint32_t)multisample->rasterizationSamples + 31U) / 32U;
        VkSampleMask *masks = NULL;
        if (multisample->pSampleMask != NULL)
            result = general_graphics_blob_resolve(
                mapping, request->blob_bytes, multisample->pSampleMask,
                mask_count, sizeof(*masks), 4U, (void **)&masks);
        if (result == 0) multisample->pSampleMask = masks;
    }
    BVB_RESOLVE_STATE(
        pDepthStencilState, VkPipelineDepthStencilStateCreateInfo,
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, false);
    BVB_RESOLVE_STATE(
        pColorBlendState, VkPipelineColorBlendStateCreateInfo,
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, true);
    if (result == 0) {
        VkPipelineColorBlendStateCreateInfo *blend =
            (VkPipelineColorBlendStateCreateInfo *)info->pColorBlendState;
        if (blend->attachmentCount >
            BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_COLOR_ATTACHMENTS)
            result = -EPROTO;
        VkPipelineColorBlendAttachmentState *attachments = NULL;
        if (result == 0)
            result = general_graphics_blob_resolve(
                mapping, request->blob_bytes, blend->pAttachments,
                blend->attachmentCount, sizeof(*attachments), 4U,
                (void **)&attachments);
        if (result == 0) blend->pAttachments = attachments;
    }
    BVB_RESOLVE_STATE(
        pDynamicState, VkPipelineDynamicStateCreateInfo,
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, true);
    if (result == 0) {
        VkPipelineDynamicStateCreateInfo *dynamic =
            (VkPipelineDynamicStateCreateInfo *)info->pDynamicState;
        if (dynamic->dynamicStateCount == 0U ||
            dynamic->dynamicStateCount >
                BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_MAX_DYNAMIC_STATES)
            result = -EPROTO;
        VkDynamicState *states = NULL;
        if (result == 0)
            result = general_graphics_blob_resolve(
                mapping, request->blob_bytes, dynamic->pDynamicStates,
                dynamic->dynamicStateCount, sizeof(*states), 4U,
                (void **)&states);
        if (result == 0) dynamic->pDynamicStates = states;
    }
#undef BVB_RESOLVE_STATE
    uint64_t device_bits = 0U;
    if (result == 0)
        result = bvb_handle_table_lookup(
            &context->objects, request->device_id, BVB_OBJECT_DEVICE,
            NULL, &device_bits);
    uint64_t layout_device_id = 0U, layout_bits = 0U;
    uint64_t layout_id = 0U;
    if (result == 0)
        memcpy(&layout_id, &info->layout, sizeof(info->layout));
    if (result == 0 && layout_id != request->pipeline_layout_id)
        result = -EPROTO;
    if (result == 0)
        result = bvb_handle_table_lookup(
            &context->objects, layout_id, BVB_OBJECT_PIPELINE_LAYOUT,
            &layout_device_id, &layout_bits);
    if (result == 0 && layout_device_id != request->device_id)
        result = -EPROTO;
    if (result != 0) {
        (void)munmap(mapping, request->blob_bytes);
        set_error(error, error_size,
                  "general graphics blob validation failed: %d", result);
        return result;
    }
    const VkDevice device = device_from_bits(device_bits);
    info->layout = pipeline_layout_from_bits(layout_bits);
    PFN_vkCreateGraphicsPipelines create_graphics_pipelines =
        (PFN_vkCreateGraphicsPipelines)context->get_device_proc_addr(
            device, "vkCreateGraphicsPipelines");
    PFN_vkDestroyPipeline destroy_pipeline =
        (PFN_vkDestroyPipeline)context->get_device_proc_addr(
            device, "vkDestroyPipeline");
    if (create_graphics_pipelines == NULL || destroy_pipeline == NULL) {
        (void)munmap(mapping, request->blob_bytes);
        return -ENOSYS;
    }
    VkPipeline pipeline = VK_NULL_HANDLE;
    response->vulkan_result = create_graphics_pipelines(
        device, VK_NULL_HANDLE, 1U, info, NULL, &pipeline);
    (void)munmap(mapping, request->blob_bytes);
    if (response->vulkan_result != VK_SUCCESS) return 0;
    const uint64_t wire_id = bvb_handle_id(
        BVB_OBJECT_PIPELINE, context->next_pipeline_serial++);
    result = bvb_handle_table_insert(
        &context->objects, wire_id, request->device_id,
        handle_bits(&pipeline, sizeof(pipeline)));
    if (result != 0) {
        destroy_pipeline(device, pipeline, NULL);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    response->object_id = wire_id;
    return 0;
}

int bvb_vulkan_global_context_create_builtin_graphics_pipeline(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_builtin_graphics_pipeline_create_request *request,
    int blob_fd, struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size) {
    if (request != NULL && request->schema ==
            BVB_VULKAN_GENERAL_GRAPHICS_PIPELINE_BLOB_VERSION) {
        return bvb_vulkan_global_context_create_general_graphics_pipeline(
            context, request, blob_fd, response, error, error_size);
    }
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || response == NULL || blob_fd < 0)
        return -EINVAL;
    *response = (struct bvb_vulkan_object_create_response){0};
    struct stat metadata;
    const int required_seals =
        F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_WRITE | F_SEAL_SEAL;
    const int seals = fcntl(blob_fd, F_GET_SEALS);
    if (fstat(blob_fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_size != (off_t)request->blob_bytes || seals < 0 ||
        (seals & required_seals) != required_seals) {
        set_error(error, error_size,
                  "built-in graphics blob size/seals are invalid");
        return -EPROTO;
    }
    const uint8_t *mapping = mmap(
        NULL, request->blob_bytes, PROT_READ, MAP_PRIVATE, blob_fd, 0);
    if (mapping == MAP_FAILED) return -errno;
    struct bvb_vulkan_builtin_graphics_pipeline_blob_view blob = {0};
    int result = bvb_protocol_decode_vulkan_builtin_graphics_pipeline_blob(
        mapping, request->blob_bytes, &blob);
    if (result != 0) {
        (void)munmap((void *)mapping, request->blob_bytes);
        set_error(error, error_size,
                  "built-in graphics blob is not canonical: %d", result);
        return result;
    }
    uint64_t device_bits = 0U;
    result = bvb_handle_table_lookup(
        &context->objects, request->device_id, BVB_OBJECT_DEVICE, NULL,
        &device_bits);
    uint64_t layout_device_id = 0U, layout_bits = 0U;
    if (result == 0) {
        result = bvb_handle_table_lookup(
            &context->objects, request->pipeline_layout_id,
            BVB_OBJECT_PIPELINE_LAYOUT, &layout_device_id, &layout_bits);
    }
    if (result != 0 || layout_device_id != request->device_id) {
        (void)munmap((void *)mapping, request->blob_bytes);
        set_error(error, error_size,
                  "built-in graphics layout lineage failed: %d", result);
        return result != 0 ? result : -EPROTO;
    }
    const VkDevice device = device_from_bits(device_bits);
    PFN_vkCreateGraphicsPipelines create_graphics_pipelines =
        (PFN_vkCreateGraphicsPipelines)context->get_device_proc_addr(
            device, "vkCreateGraphicsPipelines");
    PFN_vkDestroyPipeline destroy_pipeline =
        (PFN_vkDestroyPipeline)context->get_device_proc_addr(
            device, "vkDestroyPipeline");
    if (create_graphics_pipelines == NULL || destroy_pipeline == NULL) {
        (void)munmap((void *)mapping, request->blob_bytes);
        return -ENOSYS;
    }
    VkSpecializationMapEntry specialization_entries[
        BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_ENTRY_COUNT];
    for (uint32_t index = 0U;
         index < BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_ENTRY_COUNT;
         ++index) {
        const uint8_t *entry = blob.specialization_entries +
            index * BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_ENTRY_SIZE;
        specialization_entries[index] = (VkSpecializationMapEntry){
            .constantID = bvb_wire_get_u32(entry),
            .offset = bvb_wire_get_u32(entry + 4U),
            .size = (size_t)bvb_wire_get_u64(entry + 8U),
        };
    }
    const VkSpecializationInfo specialization = {
        .mapEntryCount =
            BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_ENTRY_COUNT,
        .pMapEntries = specialization_entries,
        .dataSize = BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_SPEC_DATA_SIZE,
        .pData = blob.specialization_data,
    };
    const VkShaderModuleCreateInfo modules[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize =
                BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_VERTEX_CODE_SIZE,
            .pCode = blob.vertex_words,
        },
        {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize =
                BVB_VULKAN_BUILTIN_GRAPHICS_PIPELINE_FRAGMENT_CODE_SIZE,
            .pCode = blob.fragment_words,
        },
    };
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = &modules[0],
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = &modules[1],
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pName = "main",
            .pSpecializationInfo = &specialization,
        },
    };
    const VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
    const VkPipelineRenderingCreateInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1U,
        .pColorAttachmentFormats = &color_format,
    };
    const VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    const VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    const VkPipelineViewportStateCreateInfo viewport = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
    };
    const VkPipelineRasterizationStateCreateInfo rasterization = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0F,
    };
    const VkSampleMask sample_mask = 1U;
    const VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        .minSampleShading = 1.0F,
        .pSampleMask = &sample_mask,
    };
    const VkPipelineColorBlendAttachmentState blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
            VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
            VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1U,
        .pAttachments = &blend_attachment,
    };
    const VkDynamicState dynamic_states[2] = {
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,
        VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
    };
    const VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2U,
        .pDynamicStates = dynamic_states,
    };
    const VkGraphicsPipelineCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .stageCount = 2U,
        .pStages = stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = pipeline_layout_from_bits(layout_bits),
        .basePipelineIndex = -1,
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    response->vulkan_result = create_graphics_pipelines(
        device, VK_NULL_HANDLE, 1U, &create_info, NULL, &pipeline);
    (void)munmap((void *)mapping, request->blob_bytes);
    if (response->vulkan_result != VK_SUCCESS) return 0;
    const uint64_t wire_id = bvb_handle_id(
        BVB_OBJECT_PIPELINE, context->next_pipeline_serial++);
    result = bvb_handle_table_insert(
        &context->objects, wire_id, request->device_id,
        handle_bits(&pipeline, sizeof(pipeline)));
    if (result != 0) {
        destroy_pipeline(device, pipeline, NULL);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    response->object_id = wire_id;
    return 0;
}

int bvb_vulkan_global_context_destroy_pipeline(
    struct bvb_vulkan_global_context *context, uint64_t pipeline_id,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    uint64_t device_id = 0U, pipeline_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, pipeline_id, BVB_OBJECT_PIPELINE, &device_id,
        &device, &pipeline_bits);
    if (result != 0) return result;
    PFN_vkDestroyPipeline destroy_pipeline =
        (PFN_vkDestroyPipeline)context->get_device_proc_addr(
            device, "vkDestroyPipeline");
    if (destroy_pipeline == NULL) return -ENOSYS;
    result = bvb_handle_table_remove(
        &context->objects, pipeline_id, BVB_OBJECT_PIPELINE, NULL);
    if (result == 0) {
        destroy_pipeline(device, pipeline_from_bits(pipeline_bits), NULL);
    }
    return result;
}

static struct bvb_image_metadata *image_metadata_slot(
    struct bvb_vulkan_global_context *context, uint64_t image_id) {
    struct bvb_image_metadata *empty = NULL;
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        struct bvb_image_metadata *metadata = &context->image_metadata[index];
        if (metadata->image_id == image_id && image_id != 0U) return metadata;
        if (metadata->image_id == 0U && empty == NULL) empty = metadata;
    }
    return empty;
}

static struct bvb_buffer_metadata *buffer_metadata_slot(
    struct bvb_vulkan_global_context *context, uint64_t buffer_id) {
    struct bvb_buffer_metadata *empty = NULL;
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        struct bvb_buffer_metadata *metadata =
            &context->buffer_metadata[index];
        if (metadata->buffer_id == buffer_id && buffer_id != 0U)
            return metadata;
        if (metadata->buffer_id == 0U && empty == NULL) empty = metadata;
    }
    return empty;
}

static const struct bvb_buffer_metadata *buffer_metadata_find(
    const struct bvb_vulkan_global_context *context, uint64_t buffer_id) {
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_buffer_metadata *metadata =
            &context->buffer_metadata[index];
        if (metadata->buffer_id == buffer_id && buffer_id != 0U)
            return metadata;
    }
    return NULL;
}

int bvb_vulkan_global_context_create_buffer(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_buffer_create_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || response == NULL) return -EINVAL;
    *response = (struct bvb_vulkan_object_create_response){0};
    if (request->size == 0U ||
        request->size > BVB_VULKAN_MAX_MEMORY_ALLOCATION_SIZE ||
        request->flags != 0U ||
        (request->usage != VK_BUFFER_USAGE_TRANSFER_DST_BIT &&
         request->usage != VK_BUFFER_USAGE_TRANSFER_SRC_BIT &&
         (((request->usage &
            (VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
             VK_BUFFER_USAGE_TRANSFER_DST_BIT |
             VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) !=
           (VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) ||
          (request->usage &
           ~(VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
             VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
             VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
             VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
             VK_BUFFER_USAGE_TRANSFER_DST_BIT |
             VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
             VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT |
             VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT)) !=
              0U))) {
        response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return 0;
    }
    uint64_t device_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, request->device_id, BVB_OBJECT_DEVICE, NULL,
        &device_bits);
    if (result != 0) return result;
    const VkDevice device = device_from_bits(device_bits);
    PFN_vkCreateBuffer create_buffer =
        (PFN_vkCreateBuffer)context->get_device_proc_addr(device,
                                                           "vkCreateBuffer");
    PFN_vkDestroyBuffer destroy_buffer =
        (PFN_vkDestroyBuffer)context->get_device_proc_addr(
            device, "vkDestroyBuffer");
    if (create_buffer == NULL || destroy_buffer == NULL) return -ENOSYS;
    const VkBufferCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = request->size,
        .usage = request->usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buffer = VK_NULL_HANDLE;
    response->vulkan_result = create_buffer(
        device, &create_info, NULL, &buffer);
    if (response->vulkan_result != VK_SUCCESS) return 0;
    struct bvb_buffer_metadata *metadata = buffer_metadata_slot(context, 0U);
    if (metadata == NULL) {
        destroy_buffer(device, buffer, NULL);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    const uint64_t wire_id = bvb_handle_id(
        BVB_OBJECT_BUFFER, context->next_buffer_serial++);
    result = bvb_handle_table_insert(
        &context->objects, wire_id, request->device_id,
        handle_bits(&buffer, sizeof(buffer)));
    if (result != 0) {
        destroy_buffer(device, buffer, NULL);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    *metadata = (struct bvb_buffer_metadata){
        .buffer_id = wire_id,
        .device_id = request->device_id,
        .usage = request->usage,
    };
    response->object_id = wire_id;
    return 0;
}

int bvb_vulkan_global_context_destroy_buffer(
    struct bvb_vulkan_global_context *context, uint64_t buffer_id,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    uint64_t device_id = 0U, buffer_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, buffer_id, BVB_OBJECT_BUFFER, &device_id, &device,
        &buffer_bits);
    if (result != 0) return result;
    PFN_vkDestroyBuffer destroy_buffer =
        (PFN_vkDestroyBuffer)context->get_device_proc_addr(
            device, "vkDestroyBuffer");
    if (destroy_buffer == NULL) return -ENOSYS;
    result = bvb_handle_table_remove(
        &context->objects, buffer_id, BVB_OBJECT_BUFFER, NULL);
    if (result == 0) {
        struct bvb_buffer_metadata *metadata =
            buffer_metadata_slot(context, buffer_id);
        if (metadata != NULL && metadata->buffer_id == buffer_id)
            *metadata = (struct bvb_buffer_metadata){0};
        destroy_buffer(device, buffer_from_bits(buffer_bits), NULL);
    }
    return result;
}

int bvb_vulkan_global_context_get_buffer_requirements(
    const struct bvb_vulkan_global_context *context, uint64_t buffer_id,
    struct bvb_vulkan_buffer_requirements *requirements,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (requirements == NULL) return -EINVAL;
    uint64_t device_id = 0U, buffer_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, buffer_id, BVB_OBJECT_BUFFER, &device_id, &device,
        &buffer_bits);
    if (result != 0) return result;
    PFN_vkGetBufferMemoryRequirements get_requirements =
        (PFN_vkGetBufferMemoryRequirements)context->get_device_proc_addr(
            device, "vkGetBufferMemoryRequirements");
    if (get_requirements == NULL) return -ENOSYS;
    VkMemoryRequirements native = {0};
    get_requirements(device, buffer_from_bits(buffer_bits), &native);
    *requirements = (struct bvb_vulkan_buffer_requirements){
        .size = native.size,
        .alignment = native.alignment,
        .memory_type_bits = native.memoryTypeBits,
    };
    return native.size != 0U && native.alignment != 0U &&
                   native.memoryTypeBits != 0U
               ? 0 : -EPROTO;
}

int bvb_vulkan_global_context_get_device_buffer_requirements(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_device_buffer_requirements_request *request,
    struct bvb_vulkan_device_buffer_requirements_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || response == NULL)
        return -EINVAL;
    *response = (struct bvb_vulkan_device_buffer_requirements_response){0};
    uint64_t device_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, request->device_id, BVB_OBJECT_DEVICE, NULL,
        &device_bits);
    if (result != 0) return result;
    const VkDevice device = device_from_bits(device_bits);
    PFN_vkGetDeviceBufferMemoryRequirements get_requirements =
        (PFN_vkGetDeviceBufferMemoryRequirements)
            context->get_device_proc_addr(
                device, "vkGetDeviceBufferMemoryRequirements");
    if (get_requirements == NULL) return -ENOSYS;
    const VkBufferCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .flags = request->flags,
        .size = request->size,
        .usage = request->usage,
        .sharingMode = (VkSharingMode)request->sharing_mode,
        .queueFamilyIndexCount = request->queue_family_index_count,
        .pQueueFamilyIndices = request->queue_family_index_count != 0U
                                   ? request->queue_family_indices
                                   : NULL,
    };
    const VkDeviceBufferMemoryRequirements info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS,
        .pCreateInfo = &create_info,
    };
    VkMemoryDedicatedRequirements dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
    };
    VkMemoryRequirements2 native = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = &dedicated,
    };
    get_requirements(device, &info, &native);
    response->memory = (struct bvb_vulkan_buffer_requirements){
        .size = native.memoryRequirements.size,
        .alignment = native.memoryRequirements.alignment,
        .memory_type_bits = native.memoryRequirements.memoryTypeBits,
    };
    response->prefers_dedicated_allocation =
        dedicated.prefersDedicatedAllocation == VK_TRUE ? 1U : 0U;
    response->requires_dedicated_allocation =
        dedicated.requiresDedicatedAllocation == VK_TRUE ? 1U : 0U;
    return response->memory.size != 0U && response->memory.alignment != 0U &&
                   response->memory.memory_type_bits != 0U &&
                   (dedicated.prefersDedicatedAllocation == VK_FALSE ||
                    dedicated.prefersDedicatedAllocation == VK_TRUE) &&
                   (dedicated.requiresDedicatedAllocation == VK_FALSE ||
                    dedicated.requiresDedicatedAllocation == VK_TRUE)
               ? 0 : -EPROTO;
}

int bvb_vulkan_global_context_get_buffer_requirements_2(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_buffer_requirements_2_request *request,
    struct bvb_vulkan_buffer_requirements_2_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || response == NULL)
        return -EINVAL;
    *response = (struct bvb_vulkan_buffer_requirements_2_response){0};
    uint8_t validation[BVB_VULKAN_BUFFER_REQUIREMENTS_2_REQUEST_SIZE];
    if (bvb_protocol_encode_vulkan_buffer_requirements_2_request(
            validation, request) != 0) {
        return -EINVAL;
    }
    uint64_t device_id = 0U;
    uint64_t buffer_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, request->buffer_id, BVB_OBJECT_BUFFER, &device_id, &device,
        &buffer_bits);
    if (result != 0) return result;
    PFN_vkGetBufferMemoryRequirements2 get_requirements =
        (PFN_vkGetBufferMemoryRequirements2)context->get_device_proc_addr(
            device, "vkGetBufferMemoryRequirements2");
    if (get_requirements == NULL) return -ENOSYS;
    const VkBufferMemoryRequirementsInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
        .buffer = buffer_from_bits(buffer_bits),
    };
    VkMemoryDedicatedRequirements dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
    };
    VkMemoryRequirements2 native = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = (request->pnext_flags &
                  BVB_VULKAN_BUFFER_REQUIREMENTS_2_PNEXT_DEDICATED) != 0U
            ? &dedicated : NULL,
    };
    get_requirements(device, &info, &native);
    if (native.memoryRequirements.size == 0U ||
        native.memoryRequirements.alignment == 0U ||
        native.memoryRequirements.memoryTypeBits == 0U ||
        dedicated.prefersDedicatedAllocation > VK_TRUE ||
        dedicated.requiresDedicatedAllocation > VK_TRUE) {
        return -EPROTO;
    }
    *response = (struct bvb_vulkan_buffer_requirements_2_response){
        .size = native.memoryRequirements.size,
        .alignment = native.memoryRequirements.alignment,
        .memory_type_bits = native.memoryRequirements.memoryTypeBits,
        .pnext_flags = request->pnext_flags,
        .prefers_dedicated =
            (uint32_t)dedicated.prefersDedicatedAllocation,
        .requires_dedicated =
            (uint32_t)dedicated.requiresDedicatedAllocation,
    };
    return 0;
}

int bvb_vulkan_global_context_get_buffer_device_address(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_buffer_device_address_request *request,
    struct bvb_vulkan_buffer_device_address_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || response == NULL)
        return -EINVAL;
    *response = (struct bvb_vulkan_buffer_device_address_response){0};
    uint8_t validation[BVB_VULKAN_BUFFER_DEVICE_ADDRESS_REQUEST_SIZE];
    if (bvb_protocol_encode_vulkan_buffer_device_address_request(
            validation, request) != 0) {
        return -EINVAL;
    }
    uint64_t device_id = 0U;
    uint64_t buffer_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, request->buffer_id, BVB_OBJECT_BUFFER, &device_id, &device,
        &buffer_bits);
    if (result != 0) return result;
    const struct bvb_buffer_metadata *metadata = buffer_metadata_find(
        context, request->buffer_id);
    if (metadata == NULL || metadata->device_id != device_id ||
        !metadata->memory_bound ||
        (metadata->usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) == 0U) {
        return -EINVAL;
    }
    PFN_vkGetBufferDeviceAddress get_address =
        (PFN_vkGetBufferDeviceAddress)context->get_device_proc_addr(
            device, "vkGetBufferDeviceAddress");
    if (get_address == NULL) return -ENOSYS;
    const VkBufferDeviceAddressInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = buffer_from_bits(buffer_bits),
    };
    response->device_address = (uint64_t)get_address(device, &info);
    return response->device_address != 0U ? 0 : -EPROTO;
}

int bvb_vulkan_global_context_allocate_memory_extended(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_memory_allocate_extended_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || response == NULL) return -EINVAL;
    *response = (struct bvb_vulkan_object_create_response){0};
    uint8_t validation[BVB_VULKAN_MEMORY_ALLOCATE_EXTENDED_REQUEST_SIZE];
    if (bvb_protocol_encode_vulkan_memory_allocate_extended_request(
            validation, request) != 0) {
        response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return 0;
    }
    uint64_t physical_id = 0U, device_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, request->device_id, BVB_OBJECT_DEVICE,
        &physical_id, &device_bits);
    if (result != 0) return result;
    VkPhysicalDeviceMemoryProperties properties;
    result = bvb_vulkan_global_context_get_memory_properties(
        context, physical_id, &properties, error, error_size);
    if (result != 0) return result;
    if (request->memory_type_index >= properties.memoryTypeCount) {
        response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return 0;
    }
    const VkDevice device = device_from_bits(device_bits);
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
    };
    if ((request->pnext_flags &
         BVB_VULKAN_MEMORY_ALLOCATE_PNEXT_DEDICATED_IMAGE) != 0U) {
        uint64_t image_device_id = 0U;
        uint64_t image_bits = 0U;
        VkDevice image_device = VK_NULL_HANDLE;
        result = resolve_device_child(
            context, request->dedicated_image_id, BVB_OBJECT_IMAGE,
            &image_device_id, &image_device, &image_bits);
        if (result != 0 || image_device_id != request->device_id ||
            image_device != device) {
            return result != 0 ? result : -EPROTO;
        }
        dedicated.image = image_from_bits(image_bits);
    }
    VkMemoryAllocateFlagsInfo flags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = request->allocation_flags,
        .deviceMask = request->device_mask,
    };
    const void *pnext = NULL;
    if ((request->pnext_flags &
         BVB_VULKAN_MEMORY_ALLOCATE_PNEXT_DEDICATED_IMAGE) != 0U) {
        dedicated.pNext = pnext;
        pnext = &dedicated;
    }
    if ((request->pnext_flags &
         BVB_VULKAN_MEMORY_ALLOCATE_PNEXT_FLAGS) != 0U) {
        flags.pNext = pnext;
        pnext = &flags;
    }
    PFN_vkAllocateMemory allocate =
        (PFN_vkAllocateMemory)context->get_device_proc_addr(
            device, "vkAllocateMemory");
    PFN_vkFreeMemory free_memory =
        (PFN_vkFreeMemory)context->get_device_proc_addr(device,
                                                        "vkFreeMemory");
    if (allocate == NULL || free_memory == NULL) return -ENOSYS;
    const VkMemoryAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = pnext,
        .allocationSize = request->allocation_size,
        .memoryTypeIndex = request->memory_type_index,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    response->vulkan_result = allocate(device, &allocate_info, NULL, &memory);
    if (response->vulkan_result != VK_SUCCESS) return 0;
    struct bvb_memory_metadata *metadata = memory_metadata_slot(context, 0U);
    if (metadata == NULL) {
        free_memory(device, memory, NULL);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    const uint64_t wire_id = bvb_handle_id(
        BVB_OBJECT_DEVICE_MEMORY, context->next_memory_serial++);
    result = bvb_handle_table_insert(
        &context->objects, wire_id, request->device_id,
        handle_bits(&memory, sizeof(memory)));
    if (result != 0) {
        free_memory(device, memory, NULL);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    *metadata = (struct bvb_memory_metadata){
        .memory_id = wire_id,
        .allocation_size = request->allocation_size,
        .property_flags =
            properties.memoryTypes[request->memory_type_index].propertyFlags,
    };
    response->object_id = wire_id;
    return 0;
}

int bvb_vulkan_global_context_allocate_memory(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_memory_allocate_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size) {
    if (request == NULL) return -EINVAL;
    const struct bvb_vulkan_memory_allocate_extended_request extended = {
        .device_id = request->device_id,
        .allocation_size = request->allocation_size,
        .memory_type_index = request->memory_type_index,
    };
    return bvb_vulkan_global_context_allocate_memory_extended(
        context, &extended, response, error, error_size);
}

int bvb_vulkan_global_context_free_memory(
    struct bvb_vulkan_global_context *context, uint64_t memory_id,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    uint64_t device_id = 0U, memory_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, memory_id, BVB_OBJECT_DEVICE_MEMORY, &device_id, &device,
        &memory_bits);
    if (result != 0) return result;
    struct bvb_memory_mirror_metadata *mirror =
        memory_mirror_slot(context, memory_id);
    if (mirror != NULL && mirror->memory_id == memory_id) {
        set_error(error, error_size,
                  "device memory cannot be freed while mirror is mapped");
        return -EBUSY;
    }
    PFN_vkFreeMemory free_memory =
        (PFN_vkFreeMemory)context->get_device_proc_addr(device,
                                                        "vkFreeMemory");
    if (free_memory == NULL) return -ENOSYS;
    result = bvb_handle_table_remove(
        &context->objects, memory_id, BVB_OBJECT_DEVICE_MEMORY, NULL);
    if (result != 0) return result;
    struct bvb_memory_metadata *metadata =
        memory_metadata_slot(context, memory_id);
    if (metadata != NULL && metadata->memory_id == memory_id)
        *metadata = (struct bvb_memory_metadata){0};
    free_memory(device, memory_from_bits(memory_bits), NULL);
    return 0;
}

int bvb_vulkan_global_context_bind_buffer_memory(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_buffer_bind_request *request,
    int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (request == NULL || vulkan_result == NULL) return -EINVAL;
    uint64_t buffer_device_id = 0U, buffer_bits = 0U;
    uint64_t memory_device_id = 0U, memory_bits = 0U;
    VkDevice buffer_device = VK_NULL_HANDLE, memory_device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, request->buffer_id, BVB_OBJECT_BUFFER, &buffer_device_id,
        &buffer_device, &buffer_bits);
    if (result == 0)
        result = resolve_device_child(
            context, request->memory_id, BVB_OBJECT_DEVICE_MEMORY,
            &memory_device_id, &memory_device, &memory_bits);
    if (result != 0 || buffer_device_id != memory_device_id ||
        buffer_device != memory_device) return result != 0 ? result : -EPROTO;
    struct bvb_buffer_metadata *metadata = buffer_metadata_slot(
        context, request->buffer_id);
    if (metadata == NULL || metadata->device_id != buffer_device_id)
        return -EPROTO;
    struct bvb_memory_mirror_metadata *mirror =
        memory_mirror_slot(context, request->memory_id);
    if (mirror != NULL && mirror->memory_id == request->memory_id &&
        !buffer_usage_is_upload_only(metadata->usage)) {
        *vulkan_result = VK_ERROR_MEMORY_MAP_FAILED;
        return 0;
    }
    PFN_vkBindBufferMemory bind =
        (PFN_vkBindBufferMemory)context->get_device_proc_addr(
            buffer_device, "vkBindBufferMemory");
    if (bind == NULL) return -ENOSYS;
    *vulkan_result = bind(
        buffer_device, buffer_from_bits(buffer_bits),
        memory_from_bits(memory_bits), request->offset);
    if (*vulkan_result == VK_SUCCESS) {
        metadata->memory_bound = true;
        metadata->bound_memory_id = request->memory_id;
    }
    return 0;
}

int bvb_vulkan_global_context_create_image(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_image_create_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || response == NULL) return -EINVAL;
    *response = (struct bvb_vulkan_object_create_response){0};
    uint8_t validation[BVB_VULKAN_IMAGE_CREATE_REQUEST_SIZE];
    if (bvb_protocol_encode_vulkan_image_create_request(
            validation, request) != 0) {
        response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return 0;
    }
    uint64_t device_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, request->device_id, BVB_OBJECT_DEVICE, NULL,
        &device_bits);
    if (result != 0) return result;
    const VkDevice device = device_from_bits(device_bits);
    PFN_vkCreateImage create_image =
        (PFN_vkCreateImage)context->get_device_proc_addr(device,
                                                         "vkCreateImage");
    PFN_vkDestroyImage destroy_image =
        (PFN_vkDestroyImage)context->get_device_proc_addr(device,
                                                          "vkDestroyImage");
    if (create_image == NULL || destroy_image == NULL) return -ENOSYS;

    VkImageStencilUsageCreateInfo stencil_usage = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO,
        .stencilUsage = request->stencil_usage,
    };
    VkFormat view_formats[BVB_VULKAN_IMAGE_MAX_VIEW_FORMATS] = {0};
    for (uint32_t index = 0U; index < request->view_format_count; ++index) {
        view_formats[index] = (VkFormat)request->view_formats[index];
    }
    VkImageFormatListCreateInfo format_list = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .viewFormatCount = request->view_format_count,
        .pViewFormats = view_formats,
    };
    const void *pnext = NULL;
    if ((request->pnext_flags &
         BVB_VULKAN_IMAGE_CREATE_PNEXT_STENCIL_USAGE) != 0U) {
        stencil_usage.pNext = pnext;
        pnext = &stencil_usage;
    }
    if ((request->pnext_flags &
         BVB_VULKAN_IMAGE_CREATE_PNEXT_FORMAT_LIST) != 0U) {
        format_list.pNext = pnext;
        pnext = &format_list;
    }
    const VkImageCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = pnext,
        .flags = request->flags,
        .imageType = (VkImageType)request->image_type,
        .format = (VkFormat)request->format,
        .extent = {
            request->extent_width,
            request->extent_height,
            request->extent_depth,
        },
        .mipLevels = request->mip_levels,
        .arrayLayers = request->array_layers,
        .samples = (VkSampleCountFlagBits)request->samples,
        .tiling = (VkImageTiling)request->tiling,
        .usage = request->usage,
        .sharingMode = (VkSharingMode)request->sharing_mode,
        .queueFamilyIndexCount = request->queue_family_index_count,
        .pQueueFamilyIndices = request->queue_family_index_count == 0U
            ? NULL : request->queue_family_indices,
        .initialLayout = (VkImageLayout)request->initial_layout,
    };
    VkImage image = VK_NULL_HANDLE;
    response->vulkan_result = create_image(device, &create_info, NULL, &image);
    if (response->vulkan_result != VK_SUCCESS) return 0;
    struct bvb_image_metadata *metadata = image_metadata_slot(context, 0U);
    if (metadata == NULL) {
        destroy_image(device, image, NULL);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    const uint64_t wire_id = bvb_handle_id(
        BVB_OBJECT_IMAGE, context->next_image_serial++);
    result = bvb_handle_table_insert(
        &context->objects, wire_id, request->device_id,
        handle_bits(&image, sizeof(image)));
    if (result != 0) {
        destroy_image(device, image, NULL);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    *metadata = (struct bvb_image_metadata){
        .image_id = wire_id,
        .device_id = request->device_id,
        .flags = request->flags,
        .image_type = request->image_type,
        .format = request->format,
        .mip_levels = request->mip_levels,
        .array_layers = request->array_layers,
    };
    response->object_id = wire_id;
    return 0;
}

int bvb_vulkan_global_context_destroy_image(
    struct bvb_vulkan_global_context *context, uint64_t image_id,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    uint64_t device_id = 0U;
    uint64_t image_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, image_id, BVB_OBJECT_IMAGE, &device_id, &device,
        &image_bits);
    if (result != 0) return result;
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        const struct bvb_handle_entry *entry = &context->object_entries[index];
        if (bvb_handle_type(entry->wire_id) == BVB_OBJECT_IMAGE_VIEW &&
            entry->parent_id == image_id) {
            set_error(error, error_size, "image still owns an image view");
            return -EBUSY;
        }
    }
    PFN_vkDestroyImage destroy_image =
        (PFN_vkDestroyImage)context->get_device_proc_addr(device,
                                                          "vkDestroyImage");
    if (destroy_image == NULL) return -ENOSYS;
    result = bvb_handle_table_remove(
        &context->objects, image_id, BVB_OBJECT_IMAGE, NULL);
    if (result != 0) return result;
    struct bvb_image_metadata *metadata = image_metadata_slot(context, image_id);
    if (metadata != NULL && metadata->image_id == image_id) {
        *metadata = (struct bvb_image_metadata){0};
    }
    destroy_image(device, image_from_bits(image_bits), NULL);
    return 0;
}

int bvb_vulkan_global_context_get_image_requirements(
    const struct bvb_vulkan_global_context *context, uint64_t image_id,
    struct bvb_vulkan_image_requirements *requirements,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (requirements == NULL) return -EINVAL;
    uint64_t device_id = 0U;
    uint64_t image_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, image_id, BVB_OBJECT_IMAGE, &device_id, &device,
        &image_bits);
    if (result != 0) return result;
    PFN_vkGetImageMemoryRequirements get_requirements =
        (PFN_vkGetImageMemoryRequirements)context->get_device_proc_addr(
            device, "vkGetImageMemoryRequirements");
    if (get_requirements == NULL) return -ENOSYS;
    VkMemoryRequirements native = {0};
    get_requirements(device, image_from_bits(image_bits), &native);
    *requirements = (struct bvb_vulkan_image_requirements){
        .size = native.size,
        .alignment = native.alignment,
        .memory_type_bits = native.memoryTypeBits,
    };
    return native.size != 0U && native.alignment != 0U &&
                   native.memoryTypeBits != 0U
               ? 0 : -EPROTO;
}

int bvb_vulkan_global_context_get_image_requirements_2(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_image_requirements_2_request *request,
    struct bvb_vulkan_image_requirements_2_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || response == NULL) return -EINVAL;
    *response = (struct bvb_vulkan_image_requirements_2_response){0};
    uint8_t validation[BVB_VULKAN_IMAGE_REQUIREMENTS_2_REQUEST_SIZE];
    if (bvb_protocol_encode_vulkan_image_requirements_2_request(
            validation, request) != 0) {
        return -EINVAL;
    }
    uint64_t device_id = 0U;
    uint64_t image_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, request->image_id, BVB_OBJECT_IMAGE, &device_id, &device,
        &image_bits);
    if (result != 0) return result;
    PFN_vkGetImageMemoryRequirements2 get_requirements =
        (PFN_vkGetImageMemoryRequirements2)context->get_device_proc_addr(
            device, "vkGetImageMemoryRequirements2");
    if (get_requirements == NULL) return -ENOSYS;
    const VkImageMemoryRequirementsInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = image_from_bits(image_bits),
    };
    VkMemoryDedicatedRequirements dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
    };
    VkMemoryRequirements2 native = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = (request->pnext_flags &
                  BVB_VULKAN_IMAGE_REQUIREMENTS_2_PNEXT_DEDICATED) != 0U
            ? &dedicated : NULL,
    };
    get_requirements(device, &info, &native);
    if (native.memoryRequirements.size == 0U ||
        native.memoryRequirements.alignment == 0U ||
        native.memoryRequirements.memoryTypeBits == 0U ||
        dedicated.prefersDedicatedAllocation > VK_TRUE ||
        dedicated.requiresDedicatedAllocation > VK_TRUE) {
        return -EPROTO;
    }
    *response = (struct bvb_vulkan_image_requirements_2_response){
        .size = native.memoryRequirements.size,
        .alignment = native.memoryRequirements.alignment,
        .memory_type_bits = native.memoryRequirements.memoryTypeBits,
        .pnext_flags = request->pnext_flags,
        .prefers_dedicated =
            (uint32_t)dedicated.prefersDedicatedAllocation,
        .requires_dedicated =
            (uint32_t)dedicated.requiresDedicatedAllocation,
    };
    return 0;
}

int bvb_vulkan_global_context_bind_image_memory(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_image_bind_request *request,
    int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (request == NULL || vulkan_result == NULL) return -EINVAL;
    uint64_t image_device_id = 0U;
    uint64_t memory_device_id = 0U;
    uint64_t image_bits = 0U;
    uint64_t memory_bits = 0U;
    VkDevice image_device = VK_NULL_HANDLE;
    VkDevice memory_device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, request->image_id, BVB_OBJECT_IMAGE, &image_device_id,
        &image_device, &image_bits);
    if (result == 0) {
        result = resolve_device_child(
            context, request->memory_id, BVB_OBJECT_DEVICE_MEMORY,
            &memory_device_id, &memory_device, &memory_bits);
    }
    if (result != 0 || image_device_id != memory_device_id ||
        image_device != memory_device) {
        return result != 0 ? result : -EPROTO;
    }
    struct bvb_image_metadata *metadata = image_metadata_slot(
        context, request->image_id);
    if (metadata == NULL || metadata->device_id != image_device_id)
        return -EPROTO;
    struct bvb_memory_mirror_metadata *mirror =
        memory_mirror_slot(context, request->memory_id);
    if (mirror != NULL && mirror->memory_id == request->memory_id) {
        *vulkan_result = VK_ERROR_MEMORY_MAP_FAILED;
        return 0;
    }
    PFN_vkBindImageMemory bind =
        (PFN_vkBindImageMemory)context->get_device_proc_addr(
            image_device, "vkBindImageMemory");
    if (bind == NULL) return -ENOSYS;
    *vulkan_result = bind(
        image_device, image_from_bits(image_bits),
        memory_from_bits(memory_bits), request->offset);
    if (*vulkan_result == VK_SUCCESS)
        metadata->bound_memory_id = request->memory_id;
    return 0;
}

static bool image_view_range_supported(
    const struct bvb_image_metadata *image,
    const struct bvb_vulkan_image_view_create_request *request) {
    if (image == NULL || request->base_mip_level >= image->mip_levels ||
        (request->level_count != VK_REMAINING_MIP_LEVELS &&
         request->level_count > image->mip_levels - request->base_mip_level) ||
        request->base_array_layer >= image->array_layers ||
        (request->layer_count != VK_REMAINING_ARRAY_LAYERS &&
         request->layer_count > image->array_layers -
             request->base_array_layer) ||
        (request->format != image->format &&
         (image->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) == 0U)) {
        return false;
    }
    if (image->image_type == VK_IMAGE_TYPE_1D) {
        return request->view_type == VK_IMAGE_VIEW_TYPE_1D ||
               request->view_type == VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    }
    if (image->image_type == VK_IMAGE_TYPE_2D) {
        if (request->view_type == VK_IMAGE_VIEW_TYPE_2D ||
            request->view_type == VK_IMAGE_VIEW_TYPE_2D_ARRAY) return true;
        if (request->view_type == VK_IMAGE_VIEW_TYPE_CUBE ||
            request->view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) {
            return (image->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0U;
        }
        return false;
    }
    return request->view_type == VK_IMAGE_VIEW_TYPE_3D ||
           (((image->flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT) != 0U) &&
            (request->view_type == VK_IMAGE_VIEW_TYPE_2D ||
             request->view_type == VK_IMAGE_VIEW_TYPE_2D_ARRAY));
}

int bvb_vulkan_global_context_create_image_view(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_image_view_create_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || response == NULL) return -EINVAL;
    *response = (struct bvb_vulkan_object_create_response){0};
    uint8_t validation[BVB_VULKAN_IMAGE_VIEW_CREATE_REQUEST_SIZE];
    if (bvb_protocol_encode_vulkan_image_view_create_request(
            validation, request) != 0) {
        response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return 0;
    }
    uint64_t image_device_id = 0U;
    uint64_t image_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, request->image_id, BVB_OBJECT_IMAGE, &image_device_id,
        &device, &image_bits);
    if (result != 0) return result;
    if (image_device_id != request->device_id) return -EPROTO;
    struct bvb_image_metadata *image = image_metadata_slot(
        context, request->image_id);
    if (image == NULL || image->image_id != request->image_id ||
        !image_view_range_supported(image, request)) {
        response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return 0;
    }
    PFN_vkCreateImageView create_image_view =
        (PFN_vkCreateImageView)context->get_device_proc_addr(
            device, "vkCreateImageView");
    PFN_vkDestroyImageView destroy_image_view =
        (PFN_vkDestroyImageView)context->get_device_proc_addr(
            device, "vkDestroyImageView");
    if (create_image_view == NULL || destroy_image_view == NULL) return -ENOSYS;
    const VkImageViewUsageCreateInfo usage = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
        .usage = request->usage,
    };
    const VkImageViewCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = (request->pnext_flags &
                  BVB_VULKAN_IMAGE_VIEW_CREATE_PNEXT_USAGE) != 0U
            ? &usage : NULL,
        .flags = request->flags,
        .image = image_from_bits(image_bits),
        .viewType = (VkImageViewType)request->view_type,
        .format = (VkFormat)request->format,
        .components = {
            (VkComponentSwizzle)request->component_r,
            (VkComponentSwizzle)request->component_g,
            (VkComponentSwizzle)request->component_b,
            (VkComponentSwizzle)request->component_a,
        },
        .subresourceRange = {
            .aspectMask = request->aspect_mask,
            .baseMipLevel = request->base_mip_level,
            .levelCount = request->level_count,
            .baseArrayLayer = request->base_array_layer,
            .layerCount = request->layer_count,
        },
    };
    VkImageView image_view = VK_NULL_HANDLE;
    response->vulkan_result = create_image_view(
        device, &create_info, NULL, &image_view);
    if (response->vulkan_result != VK_SUCCESS) return 0;
    const uint64_t wire_id = bvb_handle_id(
        BVB_OBJECT_IMAGE_VIEW, context->next_image_view_serial++);
    result = bvb_handle_table_insert(
        &context->objects, wire_id, request->image_id,
        handle_bits(&image_view, sizeof(image_view)));
    if (result != 0) {
        destroy_image_view(device, image_view, NULL);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    response->object_id = wire_id;
    return 0;
}

int bvb_vulkan_global_context_destroy_image_view(
    struct bvb_vulkan_global_context *context, uint64_t image_view_id,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL) return -EINVAL;
    uint64_t image_id = 0U;
    uint64_t image_view_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, image_view_id, BVB_OBJECT_IMAGE_VIEW,
        &image_id, &image_view_bits);
    uint64_t device_id = 0U;
    uint64_t image_bits = 0U;
    if (result == 0) {
        result = bvb_handle_table_lookup(
            &context->objects, image_id, BVB_OBJECT_IMAGE,
            &device_id, &image_bits);
    }
    uint64_t device_bits = 0U;
    if (result == 0) {
        result = bvb_handle_table_lookup(
            &context->objects, device_id, BVB_OBJECT_DEVICE,
            NULL, &device_bits);
    }
    if (result != 0) return result;
    const VkDevice device = device_from_bits(device_bits);
    PFN_vkDestroyImageView destroy_image_view =
        (PFN_vkDestroyImageView)context->get_device_proc_addr(
            device, "vkDestroyImageView");
    if (destroy_image_view == NULL) return -ENOSYS;
    result = bvb_handle_table_remove(
        &context->objects, image_view_id, BVB_OBJECT_IMAGE_VIEW, NULL);
    if (result == 0) {
        destroy_image_view(device, image_view_from_bits(image_view_bits), NULL);
    }
    return result;
}

int bvb_vulkan_global_context_command_buffer_fill(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_command_buffer_fill_request *request,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (request == NULL || request->size == 0U ||
        (request->offset & 3U) != 0U || (request->size & 3U) != 0U)
        return -EINVAL;
    uint64_t command_device_id = 0U, buffer_device_id = 0U, buffer_bits = 0U;
    VkDevice command_device = VK_NULL_HANDLE, buffer_device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    int result = resolve_command_buffer(
        context, request->command_buffer_id, &command_device_id,
        &command_device, &command_pool, &command_buffer);
    if (result == 0)
        result = resolve_device_child(
            context, request->buffer_id, BVB_OBJECT_BUFFER, &buffer_device_id,
            &buffer_device, &buffer_bits);
    if (result != 0 || command_device_id != buffer_device_id ||
        command_device != buffer_device) return result != 0 ? result : -EPROTO;
    PFN_vkCmdFillBuffer fill =
        (PFN_vkCmdFillBuffer)context->get_device_proc_addr(
            command_device, "vkCmdFillBuffer");
    PFN_vkCmdPipelineBarrier barrier =
        (PFN_vkCmdPipelineBarrier)context->get_device_proc_addr(
            command_device, "vkCmdPipelineBarrier");
    if (fill == NULL || barrier == NULL) return -ENOSYS;
    fill(command_buffer, buffer_from_bits(buffer_bits), request->offset,
         request->size, request->data);
    const VkBufferMemoryBarrier memory_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer_from_bits(buffer_bits),
        .offset = request->offset,
        .size = request->size,
    };
    barrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0U, 0U, NULL, 1U,
            &memory_barrier, 0U, NULL);
    return 0;
}

int bvb_vulkan_global_context_command_buffer_image_barrier(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_command_buffer_image_barrier_request *request,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || request->image_count == 0U ||
        request->image_count > BVB_VULKAN_INIT_IMAGE_MAX_BARRIERS)
        return -EINVAL;
    uint64_t command_device_id = 0U;
    VkDevice command_device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    int result = resolve_command_buffer(
        context, request->command_buffer_id, &command_device_id,
        &command_device, &command_pool, &command_buffer);
    VkImageMemoryBarrier2 image_barriers[
        BVB_VULKAN_INIT_IMAGE_MAX_BARRIERS];
    memset(image_barriers, 0, sizeof(image_barriers));
    for (uint32_t index = 0U; result == 0 && index < request->image_count;
         ++index) {
        uint64_t image_device_id = 0U, image_bits = 0U;
        VkDevice image_device = VK_NULL_HANDLE;
        result = resolve_device_child(
            context, request->image_ids[index], BVB_OBJECT_IMAGE,
            &image_device_id, &image_device, &image_bits);
        if (result == 0 &&
            (image_device_id != command_device_id ||
             image_device != command_device))
            result = -EPROTO;
        if (result == 0) {
            image_barriers[index] = (VkImageMemoryBarrier2){
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                .srcAccessMask = VK_ACCESS_2_NONE,
                .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image_from_bits(image_bits),
                .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0U,
                    .levelCount = 1U,
                    .baseArrayLayer = 0U,
                    .layerCount = 1U,
                },
            };
        }
    }
    if (result != 0) return result;
    PFN_vkCmdPipelineBarrier2 barrier =
        (PFN_vkCmdPipelineBarrier2)context->get_device_proc_addr(
            command_device, "vkCmdPipelineBarrier2");
    if (barrier == NULL) return -ENOSYS;
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = request->image_count,
        .pImageMemoryBarriers = image_barriers,
    };
    barrier(command_buffer, &dependency);
    return 0;
}

int bvb_vulkan_global_context_command_buffer_clear_color_image(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_command_buffer_clear_color_image_request *request,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL) return -EINVAL;
    uint64_t command_device_id = 0U, image_device_id = 0U, image_bits = 0U;
    VkDevice command_device = VK_NULL_HANDLE, image_device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    int result = resolve_command_buffer(
        context, request->command_buffer_id, &command_device_id,
        &command_device, &command_pool, &command_buffer);
    if (result == 0)
        result = resolve_device_child(
            context, request->image_id, BVB_OBJECT_IMAGE, &image_device_id,
            &image_device, &image_bits);
    if (result != 0 || command_device_id != image_device_id ||
        command_device != image_device)
        return result != 0 ? result : -EPROTO;
    PFN_vkCmdClearColorImage clear =
        (PFN_vkCmdClearColorImage)context->get_device_proc_addr(
            command_device, "vkCmdClearColorImage");
    if (clear == NULL) return -ENOSYS;
    const VkClearColorValue color = {0};
    const VkImageSubresourceRange range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0U,
        .levelCount = 1U,
        .baseArrayLayer = 0U,
        .layerCount = 1U,
    };
    clear(command_buffer, image_from_bits(image_bits),
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1U, &range);
    return 0;
}

int bvb_vulkan_global_context_command_buffer_bind_descriptor_sets(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_bind_descriptor_sets_request *request,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL ||
        request->pipeline_bind_point > VK_PIPELINE_BIND_POINT_COMPUTE ||
        request->descriptor_set_count == 0U ||
        request->descriptor_set_count > BVB_VULKAN_MAX_BOUND_DESCRIPTOR_SETS ||
        request->dynamic_offset_count > BVB_VULKAN_MAX_DYNAMIC_OFFSETS) {
        return -EINVAL;
    }
    uint64_t device_id = 0U;
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    int result = resolve_command_buffer(
        context, request->command_buffer_id, &device_id, &device,
        &command_pool, &command_buffer);
    uint64_t layout_device_id = 0U, layout_bits = 0U;
    if (result == 0) {
        result = bvb_handle_table_lookup(
            &context->objects, request->pipeline_layout_id,
            BVB_OBJECT_PIPELINE_LAYOUT, &layout_device_id, &layout_bits);
    }
    if (result == 0 && layout_device_id != device_id) result = -EPROTO;
    VkDescriptorSet sets[BVB_VULKAN_MAX_BOUND_DESCRIPTOR_SETS] = {0};
    for (uint32_t index = 0U;
         result == 0 && index < request->descriptor_set_count; ++index) {
        uint64_t pool_id = 0U, set_bits = 0U, pool_device_id = 0U;
        uint64_t pool_bits = 0U;
        result = bvb_handle_table_lookup(
            &context->objects, request->descriptor_set_ids[index],
            BVB_OBJECT_DESCRIPTOR_SET, &pool_id, &set_bits);
        if (result == 0) {
            result = bvb_handle_table_lookup(
                &context->objects, pool_id, BVB_OBJECT_DESCRIPTOR_POOL,
                &pool_device_id, &pool_bits);
        }
        if (result == 0 && pool_device_id != device_id) result = -EPROTO;
        sets[index] = descriptor_set_from_bits(set_bits);
    }
    if (result == 0) {
        PFN_vkCmdBindDescriptorSets bind =
            (PFN_vkCmdBindDescriptorSets)context->get_device_proc_addr(
                device, "vkCmdBindDescriptorSets");
        if (bind == NULL) result = -ENOSYS;
        else bind(command_buffer,
                  (VkPipelineBindPoint)request->pipeline_bind_point,
                  pipeline_layout_from_bits(layout_bits), request->first_set,
                  request->descriptor_set_count, sets,
                  request->dynamic_offset_count,
                  request->dynamic_offset_count == 0U
                      ? NULL : request->dynamic_offsets);
    }
    if (result != 0)
        set_error(error, error_size,
                  "bind descriptor sets lineage or native call failed: %d",
                  result);
    return result;
}

static int replay_command_stream_image_barrier_2(
    const struct bvb_vulkan_global_context *context,
    uint64_t command_buffer_id,
    const struct bvb_vulkan_image_barrier_2_command *command) {
    uint64_t command_device_id = 0U;
    VkDevice command_device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    int result = resolve_command_buffer(
        context, command_buffer_id, &command_device_id, &command_device,
        &command_pool, &command_buffer);
    VkImageMemoryBarrier2 barriers[BVB_COMMAND_VULKAN_MAX_IMAGE_BARRIERS];
    memset(barriers, 0, sizeof(barriers));
    for (uint32_t index = 0U;
         result == 0 && index < command->image_count; ++index) {
        uint64_t image_device_id = 0U;
        uint64_t image_bits = 0U;
        VkDevice image_device = VK_NULL_HANDLE;
        result = resolve_device_child(
            context, command->images[index].image_id, BVB_OBJECT_IMAGE,
            &image_device_id, &image_device, &image_bits);
        if (result == 0 &&
            (image_device_id != command_device_id ||
             image_device != command_device)) {
            result = -EPROTO;
        }
        if (result == 0) {
            const struct bvb_vulkan_image_barrier_2 *source =
                &command->images[index];
            barriers[index] = (VkImageMemoryBarrier2){
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = source->source_stage_mask,
                .srcAccessMask = source->source_access_mask,
                .dstStageMask = source->destination_stage_mask,
                .dstAccessMask = source->destination_access_mask,
                .oldLayout = (VkImageLayout)source->old_layout,
                .newLayout = (VkImageLayout)source->new_layout,
                .srcQueueFamilyIndex = source->source_queue_family_index,
                .dstQueueFamilyIndex = source->destination_queue_family_index,
                .image = image_from_bits(image_bits),
                .subresourceRange = {
                    .aspectMask = source->range.aspect_mask,
                    .baseMipLevel = source->range.base_mip_level,
                    .levelCount = source->range.level_count,
                    .baseArrayLayer = source->range.base_array_layer,
                    .layerCount = source->range.layer_count,
                },
            };
        }
    }
    if (result != 0) return result;
    PFN_vkCmdPipelineBarrier2 barrier =
        (PFN_vkCmdPipelineBarrier2)context->get_device_proc_addr(
            command_device, "vkCmdPipelineBarrier2");
    if (barrier == NULL) return -ENOSYS;
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .dependencyFlags = command->dependency_flags,
        .imageMemoryBarrierCount = command->image_count,
        .pImageMemoryBarriers = barriers,
    };
    barrier(command_buffer, &dependency);
    return 0;
}

static int replay_command_stream_clear_color_image_general(
    const struct bvb_vulkan_global_context *context,
    uint64_t command_buffer_id,
    const struct bvb_vulkan_clear_color_image_general_command *command) {
    uint64_t command_device_id = 0U;
    VkDevice command_device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    int result = resolve_command_buffer(
        context, command_buffer_id, &command_device_id, &command_device,
        &command_pool, &command_buffer);
    uint64_t image_device_id = 0U;
    uint64_t image_bits = 0U;
    VkDevice image_device = VK_NULL_HANDLE;
    if (result == 0) {
        result = resolve_device_child(
            context, command->image_id, BVB_OBJECT_IMAGE, &image_device_id,
            &image_device, &image_bits);
    }
    if (result == 0 &&
        (image_device_id != command_device_id || image_device != command_device)) {
        result = -EPROTO;
    }
    if (result != 0) return result;
    PFN_vkCmdClearColorImage clear =
        (PFN_vkCmdClearColorImage)context->get_device_proc_addr(
            command_device, "vkCmdClearColorImage");
    if (clear == NULL) return -ENOSYS;
    VkClearColorValue color;
    memcpy(color.uint32, command->color_words, sizeof(command->color_words));
    VkImageSubresourceRange ranges[BVB_COMMAND_VULKAN_MAX_CLEAR_RANGES];
    memset(ranges, 0, sizeof(ranges));
    for (uint32_t index = 0U; index < command->range_count; ++index) {
        ranges[index] = (VkImageSubresourceRange){
            .aspectMask = command->ranges[index].aspect_mask,
            .baseMipLevel = command->ranges[index].base_mip_level,
            .levelCount = command->ranges[index].level_count,
            .baseArrayLayer = command->ranges[index].base_array_layer,
            .layerCount = command->ranges[index].layer_count,
        };
    }
    clear(command_buffer, image_from_bits(image_bits),
          (VkImageLayout)command->image_layout, &color,
          command->range_count, ranges);
    return 0;
}

int bvb_vulkan_global_context_validate_queue_submit_2(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_queue_submit_2_request *request,
    uint64_t *device_id, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || device_id == NULL ||
        request->flags != 0U ||
        request->wait_count > BVB_VULKAN_MAX_SEMAPHORES_PER_WAIT ||
        request->command_count > BVB_VULKAN_MAX_COMMAND_BUFFERS_PER_SUBMIT ||
        request->signal_count > BVB_VULKAN_MAX_SEMAPHORES_PER_WAIT) {
        return -EINVAL;
    }
    VkDevice queue_device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    int result = resolve_queue(
        context, request->queue_id, &queue_device, &queue);
    uint64_t queue_device_id = 0U;
    uint64_t queue_bits = 0U;
    if (result == 0) {
        result = bvb_handle_table_lookup(
            &context->objects, request->queue_id, BVB_OBJECT_QUEUE,
            &queue_device_id, &queue_bits);
    }
    for (uint32_t index = 0U; result == 0 && index < request->wait_count;
         ++index) {
        uint64_t parent_id = 0U;
        uint64_t semaphore_bits = 0U;
        result = bvb_handle_table_lookup(
            &context->objects, request->waits[index].semaphore_id,
            BVB_OBJECT_SEMAPHORE, &parent_id, &semaphore_bits);
        if (result == 0 &&
            (parent_id != queue_device_id ||
             request->waits[index].device_index != 0U)) {
            result = -EPROTO;
        }
    }
    for (uint32_t index = 0U; result == 0 && index < request->command_count;
         ++index) {
        uint64_t command_device_id = 0U;
        VkDevice command_device = VK_NULL_HANDLE;
        VkCommandPool command_pool = VK_NULL_HANDLE;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        result = resolve_command_buffer(
            context, request->commands[index].command_buffer_id,
            &command_device_id, &command_device, &command_pool,
            &command_buffer);
        if (result == 0 &&
            (command_device_id != queue_device_id ||
             command_device != queue_device ||
             request->commands[index].device_mask != 0U)) {
            result = -EPROTO;
        }
    }
    for (uint32_t index = 0U; result == 0 && index < request->signal_count;
         ++index) {
        uint64_t parent_id = 0U;
        uint64_t semaphore_bits = 0U;
        result = bvb_handle_table_lookup(
            &context->objects, request->signals[index].semaphore_id,
            BVB_OBJECT_SEMAPHORE, &parent_id, &semaphore_bits);
        if (result == 0 &&
            (parent_id != queue_device_id ||
             request->signals[index].device_index != 0U)) {
            result = -EPROTO;
        }
    }
    if (result == 0 && request->fence_id != 0U) {
        uint64_t fence_device_id = 0U;
        uint64_t fence_bits = 0U;
        VkDevice fence_device = VK_NULL_HANDLE;
        result = resolve_device_child(
            context, request->fence_id, BVB_OBJECT_FENCE,
            &fence_device_id, &fence_device, &fence_bits);
        if (result == 0 &&
            (fence_device_id != queue_device_id ||
             fence_device != queue_device)) {
            result = -EPROTO;
        }
    }
    if (result != 0) {
        set_error(error, error_size,
                  "submit2 references unknown or cross-device objects");
        return result;
    }
    *device_id = queue_device_id;
    return 0;
}

static int command_stream_child_matches_device(
    const struct bvb_vulkan_global_context *context, uint64_t child_id,
    enum bvb_object_type type, uint64_t expected_device_id) {
    uint64_t child_device_id = 0U;
    uint64_t child_bits = 0U;
    VkDevice child_device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, child_id, type, &child_device_id, &child_device,
        &child_bits);
    return result != 0 ? result
                       : child_device_id == expected_device_id ? 0 : -EPROTO;
}

static int command_stream_descriptor_set_matches_device(
    const struct bvb_vulkan_global_context *context, uint64_t set_id,
    uint64_t expected_device_id) {
    uint64_t pool_id = 0U, set_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, set_id, BVB_OBJECT_DESCRIPTOR_SET,
        &pool_id, &set_bits);
    uint64_t pool_device_id = 0U, pool_bits = 0U;
    if (result == 0)
        result = bvb_handle_table_lookup(
            &context->objects, pool_id, BVB_OBJECT_DESCRIPTOR_POOL,
            &pool_device_id, &pool_bits);
    return result != 0 ? result
                       : pool_device_id == expected_device_id ? 0 : -EPROTO;
}

static int command_stream_image_view_matches_device(
    const struct bvb_vulkan_global_context *context, uint64_t image_view_id,
    uint64_t expected_device_id) {
    VkImageView image_view = VK_NULL_HANDLE;
    return descriptor_template_resolve_image_view(
        context, image_view_id, expected_device_id, &image_view);
}

static int command_stream_transfer_opcode(uint16_t opcode) {
    return opcode >= BVB_COMMAND_VULKAN_COPY_BUFFER_2 &&
           opcode <= BVB_COMMAND_VULKAN_RESOLVE_IMAGE_2;
}

static enum bvb_object_type command_stream_transfer_source_type(
    uint16_t opcode) {
    return opcode == BVB_COMMAND_VULKAN_COPY_BUFFER_2 ||
                   opcode == BVB_COMMAND_VULKAN_COPY_BUFFER_TO_IMAGE_2
               ? BVB_OBJECT_BUFFER : BVB_OBJECT_IMAGE;
}

static enum bvb_object_type command_stream_transfer_destination_type(
    uint16_t opcode) {
    return opcode == BVB_COMMAND_VULKAN_COPY_BUFFER_2 ||
                   opcode == BVB_COMMAND_VULKAN_COPY_IMAGE_TO_BUFFER_2
               ? BVB_OBJECT_BUFFER : BVB_OBJECT_IMAGE;
}

static VkImageSubresourceLayers command_stream_transfer_layers(
    const struct bvb_vulkan_image_subresource_layers *layers) {
    return (VkImageSubresourceLayers){
        .aspectMask = (VkImageAspectFlags)layers->aspect_mask,
        .mipLevel = layers->mip_level,
        .baseArrayLayer = layers->base_array_layer,
        .layerCount = layers->layer_count,
    };
}

static VkOffset3D command_stream_transfer_offset(
    const struct bvb_vulkan_offset_3d *offset) {
    return (VkOffset3D){.x = offset->x, .y = offset->y, .z = offset->z};
}

static VkExtent3D command_stream_transfer_extent(
    const struct bvb_vulkan_extent_3d *extent) {
    return (VkExtent3D){.width = extent->width, .height = extent->height,
                        .depth = extent->depth};
}

static int validate_transfer_command_record(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_command_record *record, uint64_t expected_device_id) {
    struct bvb_vulkan_transfer_command command;
    int result = bvb_command_decode_vulkan_transfer(record, &command);
    if (result == 0)
        result = command_stream_child_matches_device(
            context, command.source_id,
            command_stream_transfer_source_type(record->opcode),
            expected_device_id);
    if (result == 0)
        result = command_stream_child_matches_device(
            context, command.destination_id,
            command_stream_transfer_destination_type(record->opcode),
            expected_device_id);
    return result;
}

static int validate_render_command_record(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_command_record *record, uint64_t expected_device_id,
    bool *rendering) {
    if (record == NULL || rendering == NULL) return -EINVAL;
    if (record->opcode == BVB_COMMAND_BEGIN_RENDERING) {
        struct bvb_begin_rendering_command command;
        int result = bvb_command_decode_begin_rendering(record, &command);
        if (result == 0 && *rendering) result = -EPROTO;
        if (result == 0 &&
            command.image_layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
            command.image_layout != VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL)
            result = -EPROTO;
        if (result == 0)
            result = command_stream_image_view_matches_device(
                context, command.color_image_view_id, expected_device_id);
        if (result == 0) *rendering = true;
        return result;
    }
    if (record->opcode == BVB_COMMAND_END_RENDERING) {
        if (record->payload_length != 0U || !*rendering) return -EPROTO;
        *rendering = false;
        return 0;
    }
    if (record->opcode == BVB_COMMAND_BIND_GRAPHICS_PIPELINE) {
        struct bvb_bind_graphics_pipeline_command command;
        int result = bvb_command_decode_bind_graphics_pipeline(
            record, &command);
        return result != 0 ? result : command_stream_child_matches_device(
            context, command.pipeline_id, BVB_OBJECT_PIPELINE,
            expected_device_id);
    }
    if (record->opcode == BVB_COMMAND_VULKAN_PUSH_CONSTANTS) {
        struct bvb_vulkan_push_constants_command command;
        int result = bvb_command_decode_vulkan_push_constants(
            record, &command);
        return result != 0 ? result : command_stream_child_matches_device(
            context, command.pipeline_layout_id,
            BVB_OBJECT_PIPELINE_LAYOUT, expected_device_id);
    }
    if (command_stream_transfer_opcode(record->opcode))
        return validate_transfer_command_record(
            context, record, expected_device_id);
    if (record->opcode == BVB_COMMAND_SET_VIEWPORT) {
        struct bvb_set_viewport_command command;
        return bvb_command_decode_set_viewport(record, &command);
    }
    if (record->opcode == BVB_COMMAND_SET_SCISSOR) {
        struct bvb_set_scissor_command command;
        return bvb_command_decode_set_scissor(record, &command);
    }
    if (record->opcode == BVB_COMMAND_DRAW) {
        struct bvb_draw_command command;
        if (!*rendering) return -EPROTO;
        return bvb_command_decode_draw(record, &command);
    }
    return -EPROTO;
}

static int replay_render_command_record(
    const struct bvb_vulkan_global_context *context,
    uint64_t command_buffer_id, const struct bvb_command_record *record) {
    uint64_t device_id = 0U;
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    int result = resolve_command_buffer(
        context, command_buffer_id, &device_id, &device,
        &command_pool, &command_buffer);
    if (result != 0) return result;
    if (record->opcode == BVB_COMMAND_BEGIN_RENDERING) {
        struct bvb_begin_rendering_command command;
        VkImageView image_view = VK_NULL_HANDLE;
        result = bvb_command_decode_begin_rendering(record, &command);
        if (result == 0)
            result = descriptor_template_resolve_image_view(
                context, command.color_image_view_id, device_id, &image_view);
        PFN_vkCmdBeginRendering begin = result == 0
            ? (PFN_vkCmdBeginRendering)context->get_device_proc_addr(
                  device, "vkCmdBeginRendering") : NULL;
        if (result == 0 && begin == NULL)
            begin = (PFN_vkCmdBeginRendering)context->get_device_proc_addr(
                device, "vkCmdBeginRenderingKHR");
        if (result != 0 || begin == NULL) return result != 0 ? result : -ENOSYS;
        const VkRenderingAttachmentInfo color = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = image_view,
            .imageLayout = (VkImageLayout)command.image_layout,
            .resolveMode = VK_RESOLVE_MODE_NONE,
            .loadOp = (VkAttachmentLoadOp)command.load_op,
            .storeOp = (VkAttachmentStoreOp)command.store_op,
            .clearValue = {.color.float32 = {
                command.clear_color[0], command.clear_color[1],
                command.clear_color[2], command.clear_color[3]}},
        };
        const VkRenderingInfo rendering = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = {.offset = {0, 0},
                           .extent = {command.width, command.height}},
            .layerCount = command.layer_count,
            .colorAttachmentCount = 1U,
            .pColorAttachments = &color,
        };
        begin(command_buffer, &rendering);
        return 0;
    }
    if (record->opcode == BVB_COMMAND_END_RENDERING) {
        PFN_vkCmdEndRendering end =
            (PFN_vkCmdEndRendering)context->get_device_proc_addr(
                device, "vkCmdEndRendering");
        if (end == NULL)
            end = (PFN_vkCmdEndRendering)context->get_device_proc_addr(
                device, "vkCmdEndRenderingKHR");
        if (end == NULL) return -ENOSYS;
        end(command_buffer);
        return 0;
    }
    if (record->opcode == BVB_COMMAND_BIND_GRAPHICS_PIPELINE) {
        struct bvb_bind_graphics_pipeline_command command;
        uint64_t pipeline_device_id = 0U, pipeline_bits = 0U;
        VkDevice pipeline_device = VK_NULL_HANDLE;
        result = bvb_command_decode_bind_graphics_pipeline(record, &command);
        if (result == 0)
            result = resolve_device_child(
                context, command.pipeline_id, BVB_OBJECT_PIPELINE,
                &pipeline_device_id, &pipeline_device, &pipeline_bits);
        if (result == 0 &&
            (pipeline_device_id != device_id || pipeline_device != device))
            result = -EPROTO;
        PFN_vkCmdBindPipeline bind = result == 0
            ? (PFN_vkCmdBindPipeline)context->get_device_proc_addr(
                  device, "vkCmdBindPipeline") : NULL;
        if (result != 0 || bind == NULL) return result != 0 ? result : -ENOSYS;
        bind(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
             pipeline_from_bits(pipeline_bits));
        return 0;
    }
    if (record->opcode == BVB_COMMAND_VULKAN_PUSH_CONSTANTS) {
        struct bvb_vulkan_push_constants_command command;
        uint64_t layout_device_id = 0U, layout_bits = 0U;
        VkDevice layout_device = VK_NULL_HANDLE;
        result = bvb_command_decode_vulkan_push_constants(record, &command);
        if (result == 0)
            result = resolve_device_child(
                context, command.pipeline_layout_id,
                BVB_OBJECT_PIPELINE_LAYOUT, &layout_device_id,
                &layout_device, &layout_bits);
        if (result == 0 &&
            (layout_device_id != device_id || layout_device != device))
            result = -EPROTO;
        PFN_vkCmdPushConstants push = result == 0
            ? (PFN_vkCmdPushConstants)context->get_device_proc_addr(
                  device, "vkCmdPushConstants") : NULL;
        if (result != 0 || push == NULL) return result != 0 ? result : -ENOSYS;
        push(command_buffer, pipeline_layout_from_bits(layout_bits),
             (VkShaderStageFlags)command.stage_flags, command.offset,
             command.size, command.data);
        return 0;
    }
    if (record->opcode == BVB_COMMAND_SET_VIEWPORT) {
        struct bvb_set_viewport_command command;
        result = bvb_command_decode_set_viewport(record, &command);
        PFN_vkCmdSetViewportWithCount set = result == 0
            ? (PFN_vkCmdSetViewportWithCount)context->get_device_proc_addr(
                  device, "vkCmdSetViewportWithCount") : NULL;
        if (set == NULL && result == 0)
            set = (PFN_vkCmdSetViewportWithCount)
                context->get_device_proc_addr(device,
                                              "vkCmdSetViewportWithCountEXT");
        if (result != 0 || set == NULL) return result != 0 ? result : -ENOSYS;
        const VkViewport viewport = {
            .x = command.x, .y = command.y,
            .width = command.width, .height = command.height,
            .minDepth = command.minimum_depth,
            .maxDepth = command.maximum_depth,
        };
        set(command_buffer, 1U, &viewport);
        return 0;
    }
    if (record->opcode == BVB_COMMAND_SET_SCISSOR) {
        struct bvb_set_scissor_command command;
        result = bvb_command_decode_set_scissor(record, &command);
        PFN_vkCmdSetScissorWithCount set = result == 0
            ? (PFN_vkCmdSetScissorWithCount)context->get_device_proc_addr(
                  device, "vkCmdSetScissorWithCount") : NULL;
        if (set == NULL && result == 0)
            set = (PFN_vkCmdSetScissorWithCount)
                context->get_device_proc_addr(device,
                                              "vkCmdSetScissorWithCountEXT");
        if (result != 0 || set == NULL) return result != 0 ? result : -ENOSYS;
        const VkRect2D scissor = {
            .offset = {command.x, command.y},
            .extent = {command.width, command.height},
        };
        set(command_buffer, 1U, &scissor);
        return 0;
    }
    if (record->opcode == BVB_COMMAND_DRAW) {
        struct bvb_draw_command command;
        result = bvb_command_decode_draw(record, &command);
        PFN_vkCmdDraw draw = result == 0
            ? (PFN_vkCmdDraw)context->get_device_proc_addr(
                  device, "vkCmdDraw") : NULL;
        if (result != 0 || draw == NULL) return result != 0 ? result : -ENOSYS;
        draw(command_buffer, command.vertex_count, command.instance_count,
             command.first_vertex, command.first_instance);
        return 0;
    }
    if (command_stream_transfer_opcode(record->opcode)) {
        struct bvb_vulkan_transfer_command command;
        uint64_t source_device_id = 0U, source_bits = 0U;
        uint64_t destination_device_id = 0U, destination_bits = 0U;
        VkDevice source_device = VK_NULL_HANDLE;
        VkDevice destination_device = VK_NULL_HANDLE;
        result = bvb_command_decode_vulkan_transfer(record, &command);
        if (result == 0)
            result = resolve_device_child(
                context, command.source_id,
                command_stream_transfer_source_type(record->opcode),
                &source_device_id, &source_device, &source_bits);
        if (result == 0)
            result = resolve_device_child(
                context, command.destination_id,
                command_stream_transfer_destination_type(record->opcode),
                &destination_device_id, &destination_device,
                &destination_bits);
        if (result == 0 &&
            (source_device_id != device_id ||
             destination_device_id != device_id || source_device != device ||
             destination_device != device)) result = -EPROTO;
        if (result != 0) return result;
        if (record->opcode == BVB_COMMAND_VULKAN_COPY_BUFFER_2) {
            VkBufferCopy2 regions[BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS];
            for (uint32_t index = 0U; index < command.region_count; ++index) {
                regions[index] = (VkBufferCopy2){
                    .sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
                    .srcOffset = command.regions[index].source_buffer_offset,
                    .dstOffset =
                        command.regions[index].destination_buffer_offset,
                    .size = command.regions[index].size,
                };
            }
            PFN_vkCmdCopyBuffer2 call =
                (PFN_vkCmdCopyBuffer2)context->get_device_proc_addr(
                    device, "vkCmdCopyBuffer2");
            if (call == NULL) return -ENOSYS;
            call(command_buffer, &(const VkCopyBufferInfo2){
                .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
                .srcBuffer = buffer_from_bits(source_bits),
                .dstBuffer = buffer_from_bits(destination_bits),
                .regionCount = command.region_count, .pRegions = regions});
            return 0;
        }
        if (record->opcode ==
            BVB_COMMAND_VULKAN_COPY_BUFFER_TO_IMAGE_2) {
            VkBufferImageCopy2
                regions[BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS];
            for (uint32_t index = 0U; index < command.region_count; ++index) {
                const struct bvb_vulkan_transfer_region *wire =
                    &command.regions[index];
                regions[index] = (VkBufferImageCopy2){
                    .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
                    .bufferOffset = wire->source_buffer_offset,
                    .bufferRowLength = wire->buffer_row_length,
                    .bufferImageHeight = wire->buffer_image_height,
                    .imageSubresource = command_stream_transfer_layers(
                        &wire->destination_layers),
                    .imageOffset = command_stream_transfer_offset(
                        &wire->destination_offsets[0]),
                    .imageExtent = command_stream_transfer_extent(
                        &wire->extent),
                };
            }
            PFN_vkCmdCopyBufferToImage2 call =
                (PFN_vkCmdCopyBufferToImage2)
                    context->get_device_proc_addr(
                        device, "vkCmdCopyBufferToImage2");
            if (call == NULL) return -ENOSYS;
            call(command_buffer, &(const VkCopyBufferToImageInfo2){
                .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
                .srcBuffer = buffer_from_bits(source_bits),
                .dstImage = image_from_bits(destination_bits),
                .dstImageLayout = (VkImageLayout)command.destination_layout,
                .regionCount = command.region_count, .pRegions = regions});
            return 0;
        }
        if (record->opcode ==
            BVB_COMMAND_VULKAN_COPY_IMAGE_TO_BUFFER_2) {
            VkBufferImageCopy2
                regions[BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS];
            for (uint32_t index = 0U; index < command.region_count; ++index) {
                const struct bvb_vulkan_transfer_region *wire =
                    &command.regions[index];
                regions[index] = (VkBufferImageCopy2){
                    .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
                    .bufferOffset = wire->destination_buffer_offset,
                    .bufferRowLength = wire->buffer_row_length,
                    .bufferImageHeight = wire->buffer_image_height,
                    .imageSubresource = command_stream_transfer_layers(
                        &wire->source_layers),
                    .imageOffset = command_stream_transfer_offset(
                        &wire->source_offsets[0]),
                    .imageExtent = command_stream_transfer_extent(
                        &wire->extent),
                };
            }
            PFN_vkCmdCopyImageToBuffer2 call =
                (PFN_vkCmdCopyImageToBuffer2)
                    context->get_device_proc_addr(
                        device, "vkCmdCopyImageToBuffer2");
            if (call == NULL) return -ENOSYS;
            call(command_buffer, &(const VkCopyImageToBufferInfo2){
                .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
                .srcImage = image_from_bits(source_bits),
                .srcImageLayout = (VkImageLayout)command.source_layout,
                .dstBuffer = buffer_from_bits(destination_bits),
                .regionCount = command.region_count, .pRegions = regions});
            return 0;
        }
        if (record->opcode == BVB_COMMAND_VULKAN_COPY_IMAGE_2) {
            VkImageCopy2 regions[BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS];
            for (uint32_t index = 0U; index < command.region_count; ++index) {
                const struct bvb_vulkan_transfer_region *wire =
                    &command.regions[index];
                regions[index] = (VkImageCopy2){
                    .sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
                    .srcSubresource = command_stream_transfer_layers(
                        &wire->source_layers),
                    .srcOffset = command_stream_transfer_offset(
                        &wire->source_offsets[0]),
                    .dstSubresource = command_stream_transfer_layers(
                        &wire->destination_layers),
                    .dstOffset = command_stream_transfer_offset(
                        &wire->destination_offsets[0]),
                    .extent = command_stream_transfer_extent(&wire->extent),
                };
            }
            PFN_vkCmdCopyImage2 call =
                (PFN_vkCmdCopyImage2)context->get_device_proc_addr(
                    device, "vkCmdCopyImage2");
            if (call == NULL) return -ENOSYS;
            call(command_buffer, &(const VkCopyImageInfo2){
                .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2,
                .srcImage = image_from_bits(source_bits),
                .srcImageLayout = (VkImageLayout)command.source_layout,
                .dstImage = image_from_bits(destination_bits),
                .dstImageLayout = (VkImageLayout)command.destination_layout,
                .regionCount = command.region_count, .pRegions = regions});
            return 0;
        }
        if (record->opcode == BVB_COMMAND_VULKAN_BLIT_IMAGE_2) {
            VkImageBlit2 regions[BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS];
            for (uint32_t index = 0U; index < command.region_count; ++index) {
                const struct bvb_vulkan_transfer_region *wire =
                    &command.regions[index];
                regions[index] = (VkImageBlit2){
                    .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
                    .srcSubresource = command_stream_transfer_layers(
                        &wire->source_layers),
                    .srcOffsets = {
                        command_stream_transfer_offset(
                            &wire->source_offsets[0]),
                        command_stream_transfer_offset(
                            &wire->source_offsets[1])},
                    .dstSubresource = command_stream_transfer_layers(
                        &wire->destination_layers),
                    .dstOffsets = {
                        command_stream_transfer_offset(
                            &wire->destination_offsets[0]),
                        command_stream_transfer_offset(
                            &wire->destination_offsets[1])},
                };
            }
            PFN_vkCmdBlitImage2 call =
                (PFN_vkCmdBlitImage2)context->get_device_proc_addr(
                    device, "vkCmdBlitImage2");
            if (call == NULL) return -ENOSYS;
            call(command_buffer, &(const VkBlitImageInfo2){
                .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
                .srcImage = image_from_bits(source_bits),
                .srcImageLayout = (VkImageLayout)command.source_layout,
                .dstImage = image_from_bits(destination_bits),
                .dstImageLayout = (VkImageLayout)command.destination_layout,
                .regionCount = command.region_count, .pRegions = regions,
                .filter = (VkFilter)command.filter});
            return 0;
        }
        VkImageResolve2 regions[BVB_COMMAND_VULKAN_MAX_TRANSFER_REGIONS];
        for (uint32_t index = 0U; index < command.region_count; ++index) {
            const struct bvb_vulkan_transfer_region *wire =
                &command.regions[index];
            regions[index] = (VkImageResolve2){
                .sType = VK_STRUCTURE_TYPE_IMAGE_RESOLVE_2,
                .srcSubresource = command_stream_transfer_layers(
                    &wire->source_layers),
                .srcOffset = command_stream_transfer_offset(
                    &wire->source_offsets[0]),
                .dstSubresource = command_stream_transfer_layers(
                    &wire->destination_layers),
                .dstOffset = command_stream_transfer_offset(
                    &wire->destination_offsets[0]),
                .extent = command_stream_transfer_extent(&wire->extent),
            };
        }
        PFN_vkCmdResolveImage2 call =
            (PFN_vkCmdResolveImage2)context->get_device_proc_addr(
                device, "vkCmdResolveImage2");
        if (call == NULL) return -ENOSYS;
        call(command_buffer, &(const VkResolveImageInfo2){
            .sType = VK_STRUCTURE_TYPE_RESOLVE_IMAGE_INFO_2,
            .srcImage = image_from_bits(source_bits),
            .srcImageLayout = (VkImageLayout)command.source_layout,
            .dstImage = image_from_bits(destination_bits),
            .dstImageLayout = (VkImageLayout)command.destination_layout,
            .regionCount = command.region_count, .pRegions = regions});
        return 0;
    }
    return -EPROTO;
}

int bvb_vulkan_global_context_execute_immediate_record(
    const struct bvb_vulkan_global_context *context,
    const uint8_t *batch, size_t batch_length,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    struct bvb_command_batch_info info;
    int result = context == NULL || batch == NULL ? -EINVAL :
        bvb_command_batch_validate(batch, batch_length, &info);
    if (result == 0 && info.command_count != 1U) result = -EPROTO;
    uint64_t device_id = 0U;
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    if (result == 0)
        result = resolve_command_buffer(context, info.command_buffer_id,
            &device_id, &device, &pool, &command_buffer);
    struct bvb_command_batch_iterator iterator;
    if (result == 0)
        result = bvb_command_batch_iterator_init(&iterator, batch, batch_length);
    struct bvb_command_record record;
    if (result == 0) result = bvb_command_batch_next(&iterator, &record);
    bool rendering = record.opcode != BVB_COMMAND_BEGIN_RENDERING;
    if (result == 0)
        result = validate_render_command_record(
            context, &record, device_id, &rendering);
    if (result == 0)
        result = replay_render_command_record(
            context, info.command_buffer_id, &record);
    if (result != 0)
        set_error(error, error_size,
                  "immediate render command rejected: %d", result);
    return result;
}

static int command_stream_image_barrier_range_supported(
    const struct bvb_vulkan_image_subresource_range *range) {
    const VkImageAspectFlags supported_aspects =
        VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT |
        VK_IMAGE_ASPECT_STENCIL_BIT | VK_IMAGE_ASPECT_METADATA_BIT |
        VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT |
        VK_IMAGE_ASPECT_PLANE_2_BIT |
        VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT |
        VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT |
        VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT |
        VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT;
    return range != NULL && range->aspect_mask != 0U &&
           (range->aspect_mask & ~supported_aspects) == 0U &&
           range->level_count != 0U && range->layer_count != 0U;
}

static int command_stream_clear_color_range_supported(
    const struct bvb_vulkan_image_subresource_range *range) {
    return command_stream_image_barrier_range_supported(range) &&
           range->aspect_mask == VK_IMAGE_ASPECT_COLOR_BIT;
}

static int command_stream_image_layout_supported(uint32_t layout) {
    switch ((VkImageLayout)layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
        case VK_IMAGE_LAYOUT_GENERAL:
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        case VK_IMAGE_LAYOUT_PREINITIALIZED:
        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
        case VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR:
        case VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ:
        case VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT:
        case VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR:
        case VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT:
        case VK_IMAGE_LAYOUT_ZERO_INITIALIZED_EXT:
            return 1;
        default:
            return 0;
    }
}

int bvb_vulkan_global_context_validate_command_stream(
    const struct bvb_vulkan_global_context *context,
    const uint8_t *batch, size_t batch_length, uint64_t expected_device_id,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || batch == NULL ||
        bvb_handle_expect(expected_device_id, BVB_OBJECT_DEVICE) != 0) {
        return -EINVAL;
    }
    struct bvb_command_batch_info info;
    int result = bvb_command_batch_validate(batch, batch_length, &info);
    uint64_t command_device_id = 0U;
    VkDevice command_device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    if (result == 0) {
        result = resolve_command_buffer(
            context, info.command_buffer_id, &command_device_id,
            &command_device, &command_pool, &command_buffer);
    }
    if (result == 0 && command_device_id != expected_device_id) {
        result = -EPROTO;
    }
    struct bvb_command_batch_iterator iterator;
    if (result == 0) {
        result = info.command_count >= 2U
                     ? bvb_command_batch_iterator_init(
                           &iterator, batch, batch_length)
                     : -EPROTO;
    }
    bool rendering = false;
    for (uint32_t index = 0U; result == 0 && index < info.command_count;
         ++index) {
        struct bvb_command_record record;
        result = bvb_command_batch_next(&iterator, &record);
        if (result != 0) break;
        if (index == 0U) {
            struct bvb_vulkan_begin_command begin;
            result = bvb_command_decode_vulkan_begin(&record, &begin);
            continue;
        }
        if (index + 1U == info.command_count) {
            result = record.opcode == BVB_COMMAND_VULKAN_END &&
                             record.payload_length == 0U
                         ? 0 : -EPROTO;
            continue;
        }
        if (record.opcode == BVB_COMMAND_FILL_BUFFER) {
            struct bvb_fill_buffer_command fill;
            result = bvb_command_decode_fill_buffer(&record, &fill);
            if (result == 0) {
                result = command_stream_child_matches_device(
                    context, fill.buffer_id, BVB_OBJECT_BUFFER,
                    expected_device_id);
            }
        } else if (record.opcode ==
                   BVB_COMMAND_VULKAN_IMAGE_BARRIER_2) {
            struct bvb_vulkan_image_barrier_2_command barrier;
            result = bvb_command_decode_vulkan_image_barrier_2(
                &record, &barrier);
            if (result == 0 && barrier.dependency_flags != 0U) {
                result = -EPROTO;
            }
            for (uint32_t image = 0U;
                 result == 0 && image < barrier.image_count; ++image) {
                const struct bvb_vulkan_image_barrier_2 *entry =
                    &barrier.images[image];
                if (!command_stream_image_layout_supported(entry->old_layout) ||
                    !command_stream_image_layout_supported(entry->new_layout) ||
                    entry->new_layout == VK_IMAGE_LAYOUT_UNDEFINED ||
                    entry->new_layout == VK_IMAGE_LAYOUT_PREINITIALIZED ||
                    !command_stream_image_barrier_range_supported(
                        &entry->range)) {
                    result = -EPROTO;
                    break;
                }
                result = command_stream_child_matches_device(
                    context, entry->image_id, BVB_OBJECT_IMAGE,
                    expected_device_id);
            }
        } else if (record.opcode ==
                   BVB_COMMAND_VULKAN_CLEAR_COLOR_IMAGE_GENERAL) {
            struct bvb_vulkan_clear_color_image_general_command clear;
            result = bvb_command_decode_vulkan_clear_color_image_general(
                &record, &clear);
            if (result == 0 &&
                clear.image_layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
                clear.image_layout != VK_IMAGE_LAYOUT_GENERAL) {
                result = -EPROTO;
            }
            for (uint32_t range = 0U;
                 result == 0 && range < clear.range_count; ++range) {
                if (!command_stream_clear_color_range_supported(
                        &clear.ranges[range])) {
                    result = -EPROTO;
                }
            }
            if (result == 0) {
                result = command_stream_child_matches_device(
                    context, clear.image_id, BVB_OBJECT_IMAGE,
                    expected_device_id);
            }
        } else if (record.opcode ==
                   BVB_COMMAND_VULKAN_CLEAR_COLOR_IMAGE) {
            struct bvb_vulkan_clear_color_image_command clear;
            result = bvb_command_decode_vulkan_clear_color_image(
                &record, &clear);
            if (result == 0) {
                result = command_stream_child_matches_device(
                    context, clear.image_id, BVB_OBJECT_IMAGE,
                    expected_device_id);
            }
        } else if (record.opcode ==
                   BVB_COMMAND_VULKAN_INIT_IMAGE_BARRIER) {
            struct bvb_vulkan_init_image_barrier_command barrier;
            result = bvb_command_decode_vulkan_init_image_barrier(
                &record, &barrier);
            for (uint32_t image = 0U;
                 result == 0 && image < barrier.image_count; ++image) {
                result = command_stream_child_matches_device(
                    context, barrier.image_ids[image], BVB_OBJECT_IMAGE,
                    expected_device_id);
            }
        } else if (record.opcode ==
                   BVB_COMMAND_VULKAN_BIND_DESCRIPTOR_SETS) {
            struct bvb_vulkan_bind_descriptor_sets_command bind;
            result = bvb_command_decode_vulkan_bind_descriptor_sets(
                &record, &bind);
            if (result == 0)
                result = command_stream_child_matches_device(
                    context, bind.pipeline_layout_id,
                    BVB_OBJECT_PIPELINE_LAYOUT, expected_device_id);
            for (uint32_t set = 0U;
                 result == 0 && set < bind.descriptor_set_count; ++set) {
                result = command_stream_descriptor_set_matches_device(
                    context, bind.descriptor_set_ids[set],
                    expected_device_id);
            }
        } else if (record.opcode >= BVB_COMMAND_BEGIN_RENDERING &&
                   record.opcode <= BVB_COMMAND_END_RENDERING) {
            result = validate_render_command_record(
                context, &record, expected_device_id, &rendering);
        } else if (record.opcode == BVB_COMMAND_VULKAN_PUSH_CONSTANTS) {
            result = validate_render_command_record(
                context, &record, expected_device_id, &rendering);
        } else if (command_stream_transfer_opcode(record.opcode)) {
            result = validate_render_command_record(
                context, &record, expected_device_id, &rendering);
        } else {
            result = -EPROTO;
        }
    }
    if (result == 0 && rendering) result = -EPROTO;
    if (result == 0 && bvb_command_batch_next(
                           &iterator,
                           &(struct bvb_command_record){0}) != 1) {
        result = -EPROTO;
    }
    if (result != 0) {
        set_error(error, error_size,
                  "invalid or cross-device shared command stream");
    }
    return result;
}

int bvb_vulkan_global_context_replay_command_stream(
    const struct bvb_vulkan_global_context *context,
    const uint8_t *batch, size_t batch_length, uint64_t expected_device_id,
    char *error, size_t error_size) {
    int result = bvb_vulkan_global_context_validate_command_stream(
        context, batch, batch_length, expected_device_id, error, error_size);
    if (result != 0) return result;
    struct bvb_command_batch_info info;
    result = bvb_command_batch_validate(batch, batch_length, &info);
    struct bvb_command_batch_iterator iterator;
    if (result == 0) {
        result = bvb_command_batch_iterator_init(
            &iterator, batch, batch_length);
    }
    for (uint32_t index = 0U; result == 0 && index < info.command_count;
         ++index) {
        struct bvb_command_record record;
        result = bvb_command_batch_next(&iterator, &record);
        if (result != 0) break;
        if (record.opcode == BVB_COMMAND_VULKAN_BEGIN) {
            struct bvb_vulkan_begin_command begin;
            result = bvb_command_decode_vulkan_begin(&record, &begin);
            int32_t vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
            if (result == 0) {
                result = bvb_vulkan_global_context_begin_command_buffer(
                    context,
                    &(const struct bvb_vulkan_command_buffer_begin_request){
                        .command_buffer_id = info.command_buffer_id,
                        .flags = begin.flags,
                    },
                    &vulkan_result, error, error_size);
            }
            if (result == 0 && vulkan_result != VK_SUCCESS) result = -EIO;
        } else if (record.opcode == BVB_COMMAND_FILL_BUFFER) {
            struct bvb_fill_buffer_command fill;
            result = bvb_command_decode_fill_buffer(&record, &fill);
            if (result == 0) {
                result = bvb_vulkan_global_context_command_buffer_fill(
                    context,
                    &(const struct bvb_vulkan_command_buffer_fill_request){
                        .command_buffer_id = info.command_buffer_id,
                        .buffer_id = fill.buffer_id,
                        .offset = fill.offset,
                        .size = fill.size,
                        .data = fill.data,
                    },
                    error, error_size);
            }
        } else if (record.opcode ==
                   BVB_COMMAND_VULKAN_IMAGE_BARRIER_2) {
            struct bvb_vulkan_image_barrier_2_command barrier;
            result = bvb_command_decode_vulkan_image_barrier_2(
                &record, &barrier);
            if (result == 0) {
                result = replay_command_stream_image_barrier_2(
                    context, info.command_buffer_id, &barrier);
            }
        } else if (record.opcode ==
                   BVB_COMMAND_VULKAN_CLEAR_COLOR_IMAGE_GENERAL) {
            struct bvb_vulkan_clear_color_image_general_command clear;
            result = bvb_command_decode_vulkan_clear_color_image_general(
                &record, &clear);
            if (result == 0) {
                result = replay_command_stream_clear_color_image_general(
                    context, info.command_buffer_id, &clear);
            }
        } else if (record.opcode ==
                   BVB_COMMAND_VULKAN_CLEAR_COLOR_IMAGE) {
            struct bvb_vulkan_clear_color_image_command clear;
            result = bvb_command_decode_vulkan_clear_color_image(
                &record, &clear);
            if (result == 0) {
                result =
                    bvb_vulkan_global_context_command_buffer_clear_color_image(
                        context,
                        &(const struct
                          bvb_vulkan_command_buffer_clear_color_image_request){
                            .command_buffer_id = info.command_buffer_id,
                            .image_id = clear.image_id,
                        },
                        error, error_size);
            }
        } else if (record.opcode ==
                   BVB_COMMAND_VULKAN_INIT_IMAGE_BARRIER) {
            struct bvb_vulkan_init_image_barrier_command barrier;
            result = bvb_command_decode_vulkan_init_image_barrier(
                &record, &barrier);
            struct bvb_vulkan_command_buffer_image_barrier_request request = {
                .command_buffer_id = info.command_buffer_id,
                .image_count = barrier.image_count,
            };
            memcpy(request.image_ids, barrier.image_ids,
                   barrier.image_count * sizeof(barrier.image_ids[0]));
            if (result == 0) {
                result =
                    bvb_vulkan_global_context_command_buffer_image_barrier(
                        context, &request, error, error_size);
            }
        } else if (record.opcode ==
                   BVB_COMMAND_VULKAN_BIND_DESCRIPTOR_SETS) {
            struct bvb_vulkan_bind_descriptor_sets_command bind;
            result = bvb_command_decode_vulkan_bind_descriptor_sets(
                &record, &bind);
            const struct bvb_vulkan_bind_descriptor_sets_request request = {
                .command_buffer_id = info.command_buffer_id,
                .pipeline_layout_id = bind.pipeline_layout_id,
                .pipeline_bind_point = bind.pipeline_bind_point,
                .first_set = bind.first_set,
                .descriptor_set_count = bind.descriptor_set_count,
                .dynamic_offset_count = bind.dynamic_offset_count,
            };
            struct bvb_vulkan_bind_descriptor_sets_request mutable_request =
                request;
            memcpy(mutable_request.descriptor_set_ids,
                   bind.descriptor_set_ids,
                   bind.descriptor_set_count * sizeof(bind.descriptor_set_ids[0]));
            memcpy(mutable_request.dynamic_offsets, bind.dynamic_offsets,
                   bind.dynamic_offset_count * sizeof(bind.dynamic_offsets[0]));
            if (result == 0)
                result =
                    bvb_vulkan_global_context_command_buffer_bind_descriptor_sets(
                        context, &mutable_request, error, error_size);
        } else if ((record.opcode >= BVB_COMMAND_BEGIN_RENDERING &&
                    record.opcode <= BVB_COMMAND_END_RENDERING) ||
                   record.opcode == BVB_COMMAND_VULKAN_PUSH_CONSTANTS ||
                   command_stream_transfer_opcode(record.opcode)) {
            result = replay_render_command_record(
                context, info.command_buffer_id, &record);
        } else if (record.opcode == BVB_COMMAND_VULKAN_END) {
            int32_t vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
            result = bvb_vulkan_global_context_end_command_buffer(
                context, info.command_buffer_id, &vulkan_result,
                error, error_size);
            if (result == 0 && vulkan_result != VK_SUCCESS) result = -EIO;
        } else {
            result = -EPROTO;
        }
    }
    if (result != 0 && error != NULL && error_size != 0U && error[0] == '\0') {
        set_error(error, error_size, "native shared command replay failed");
    }
    return result;
}

int bvb_vulkan_global_context_verify_memory_fill(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_memory_verify_fill_request *request,
    struct bvb_vulkan_memory_verify_fill_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || response == NULL ||
        request->size == 0U || (request->size & 3U) != 0U) return -EINVAL;
    *response = (struct bvb_vulkan_memory_verify_fill_response){0};
    struct bvb_memory_metadata *metadata = memory_metadata_slot(
        (struct bvb_vulkan_global_context *)context, request->memory_id);
    if (metadata == NULL || metadata->memory_id != request->memory_id ||
        request->offset > metadata->allocation_size ||
        request->size > metadata->allocation_size - request->offset ||
        (metadata->property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0U)
        return -ERANGE;
    uint64_t device_id = 0U, memory_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, request->memory_id, BVB_OBJECT_DEVICE_MEMORY, &device_id,
        &device, &memory_bits);
    if (result != 0) return result;
    PFN_vkMapMemory map = (PFN_vkMapMemory)context->get_device_proc_addr(
        device, "vkMapMemory");
    PFN_vkUnmapMemory unmap =
        (PFN_vkUnmapMemory)context->get_device_proc_addr(device,
                                                         "vkUnmapMemory");
    if (map == NULL || unmap == NULL) return -ENOSYS;
    void *mapped = NULL;
    response->vulkan_result = map(
        device, memory_from_bits(memory_bits), 0U, VK_WHOLE_SIZE, 0U, &mapped);
    if (response->vulkan_result != VK_SUCCESS || mapped == NULL) return 0;
    if ((metadata->property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0U) {
        PFN_vkInvalidateMappedMemoryRanges invalidate =
            (PFN_vkInvalidateMappedMemoryRanges)context->get_device_proc_addr(
                device, "vkInvalidateMappedMemoryRanges");
        const VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = memory_from_bits(memory_bits),
            .offset = 0U,
            .size = VK_WHOLE_SIZE,
        };
        if (invalidate == NULL)
            response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
        else
            response->vulkan_result = invalidate(device, 1U, &range);
    }
    if (response->vulkan_result == VK_SUCCESS) {
        const uint32_t *words = (const uint32_t *)(
            (const uint8_t *)mapped + request->offset);
        for (uint64_t index = 0U; index < request->size / 4U; ++index)
            if (words[index] != request->expected_word)
                ++response->mismatched_words;
    }
    unmap(device, memory_from_bits(memory_bits));
    return 0;
}

static int resolve_host_visible_memory(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_memory_io_request *request,
    const struct bvb_memory_metadata **metadata, VkDevice *device,
    VkDeviceMemory *memory) {
    if (context == NULL || request == NULL || metadata == NULL ||
        device == NULL || memory == NULL || request->length == 0U ||
        request->length > BVB_VULKAN_MEMORY_IO_MAX_BYTES) {
        return -EINVAL;
    }
    struct bvb_memory_metadata *found = memory_metadata_slot(
        (struct bvb_vulkan_global_context *)context, request->memory_id);
    if (found == NULL || found->memory_id != request->memory_id ||
        request->offset > found->allocation_size ||
        request->length > found->allocation_size - request->offset ||
        (found->property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0U) {
        return -ERANGE;
    }
    uint64_t device_id = 0U;
    uint64_t memory_bits = 0U;
    int result = resolve_device_child(
        context, request->memory_id, BVB_OBJECT_DEVICE_MEMORY, &device_id,
        device, &memory_bits);
    if (result == 0) {
        *metadata = found;
        *memory = memory_from_bits(memory_bits);
    }
    return result;
}

int bvb_vulkan_global_context_write_memory(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_memory_io_request *request, const uint8_t *data,
    int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (data == NULL || vulkan_result == NULL) return -EINVAL;
    *vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    const struct bvb_memory_metadata *metadata = NULL;
    VkDevice device = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    int result = resolve_host_visible_memory(
        context, request, &metadata, &device, &memory);
    if (result != 0) return result;
    PFN_vkMapMemory map = (PFN_vkMapMemory)context->get_device_proc_addr(
        device, "vkMapMemory");
    PFN_vkUnmapMemory unmap =
        (PFN_vkUnmapMemory)context->get_device_proc_addr(device,
                                                         "vkUnmapMemory");
    if (map == NULL || unmap == NULL) return -ENOSYS;
    void *mapped = NULL;
    *vulkan_result = map(device, memory, 0U, VK_WHOLE_SIZE, 0U, &mapped);
    if (*vulkan_result != VK_SUCCESS || mapped == NULL) return 0;
    memcpy((uint8_t *)mapped + request->offset, data, request->length);
    if ((metadata->property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ==
        0U) {
        PFN_vkFlushMappedMemoryRanges flush =
            (PFN_vkFlushMappedMemoryRanges)context->get_device_proc_addr(
                device, "vkFlushMappedMemoryRanges");
        const VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = memory,
            .offset = 0U,
            .size = VK_WHOLE_SIZE,
        };
        *vulkan_result = flush == NULL
                             ? VK_ERROR_FEATURE_NOT_PRESENT
                             : flush(device, 1U, &range);
    }
    unmap(device, memory);
    return 0;
}

int bvb_vulkan_global_context_read_memory(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_memory_io_request *request, uint8_t *data,
    uint32_t capacity, uint32_t *length, int32_t *vulkan_result,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (data == NULL || length == NULL || vulkan_result == NULL ||
        request == NULL || capacity < request->length) return -EINVAL;
    *length = 0U;
    *vulkan_result = VK_ERROR_INITIALIZATION_FAILED;
    const struct bvb_memory_metadata *metadata = NULL;
    VkDevice device = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    int result = resolve_host_visible_memory(
        context, request, &metadata, &device, &memory);
    if (result != 0) return result;
    PFN_vkMapMemory map = (PFN_vkMapMemory)context->get_device_proc_addr(
        device, "vkMapMemory");
    PFN_vkUnmapMemory unmap =
        (PFN_vkUnmapMemory)context->get_device_proc_addr(device,
                                                         "vkUnmapMemory");
    if (map == NULL || unmap == NULL) return -ENOSYS;
    void *mapped = NULL;
    *vulkan_result = map(device, memory, 0U, VK_WHOLE_SIZE, 0U, &mapped);
    if (*vulkan_result != VK_SUCCESS || mapped == NULL) return 0;
    if ((metadata->property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ==
        0U) {
        PFN_vkInvalidateMappedMemoryRanges invalidate =
            (PFN_vkInvalidateMappedMemoryRanges)
                context->get_device_proc_addr(
                    device, "vkInvalidateMappedMemoryRanges");
        const VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = memory,
            .offset = 0U,
            .size = VK_WHOLE_SIZE,
        };
        *vulkan_result = invalidate == NULL
                             ? VK_ERROR_FEATURE_NOT_PRESENT
                             : invalidate(device, 1U, &range);
    }
    if (*vulkan_result == VK_SUCCESS) {
        memcpy(data, (const uint8_t *)mapped + request->offset,
               request->length);
        *length = request->length;
    }
    unmap(device, memory);
    return 0;
}

static struct bvb_memory_mirror_metadata *memory_mirror_slot(
    struct bvb_vulkan_global_context *context, uint64_t memory_id) {
    struct bvb_memory_mirror_metadata *empty = NULL;
    for (size_t index = 0U; index < BVB_VULKAN_MEMORY_MIRROR_CAPACITY;
         ++index) {
        struct bvb_memory_mirror_metadata *mirror =
            &context->memory_mirrors[index];
        if (mirror->memory_id == memory_id && memory_id != 0U) return mirror;
        if (mirror->memory_id == 0U && empty == NULL) empty = mirror;
    }
    return empty;
}

static struct bvb_memory_mirror_metadata *resolve_memory_mirror(
    struct bvb_vulkan_global_context *context, uint64_t device_id,
    uint64_t memory_id, uint64_t generation) {
    struct bvb_memory_mirror_metadata *mirror =
        memory_mirror_slot(context, memory_id);
    return mirror != NULL && mirror->memory_id == memory_id &&
                   mirror->device_id == device_id &&
                   mirror->generation == generation
               ? mirror
               : NULL;
}

static VkResult maintain_native_memory_range(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_memory_mirror_metadata *mirror, uint64_t offset,
    uint64_t size, bool invalidate) {
    const uint64_t atom = mirror->non_coherent_atom_size;
    if (atom == 0U || size == 0U || offset > mirror->allocation_size ||
        size > mirror->allocation_size - offset)
        return VK_ERROR_MEMORY_MAP_FAILED;
    const uint64_t aligned_offset = offset - offset % atom;
    uint64_t aligned_end = offset + size;
    const uint64_t remainder = aligned_end % atom;
    if (remainder != 0U && aligned_end != mirror->allocation_size) {
        const uint64_t padding = atom - remainder;
        if (padding > mirror->allocation_size - aligned_end)
            aligned_end = mirror->allocation_size;
        else
            aligned_end += padding;
    }
    const char *name = invalidate ? "vkInvalidateMappedMemoryRanges"
                                  : "vkFlushMappedMemoryRanges";
    PFN_vkVoidFunction erased = context->get_device_proc_addr(
        mirror->device, name);
    const VkMappedMemoryRange range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = mirror->memory,
        .offset = aligned_offset,
        .size = aligned_end - aligned_offset,
    };
    if (invalidate) {
        PFN_vkInvalidateMappedMemoryRanges maintain = NULL;
        memcpy(&maintain, &erased, sizeof(maintain));
        return maintain == NULL
                   ? VK_ERROR_FEATURE_NOT_PRESENT
                   : maintain(mirror->device, 1U, &range);
    }
    PFN_vkFlushMappedMemoryRanges maintain = NULL;
    memcpy(&maintain, &erased, sizeof(maintain));
    return maintain == NULL ? VK_ERROR_FEATURE_NOT_PRESENT
                            : maintain(mirror->device, 1U, &range);
}

static void upload_host_diverged_range(
    struct bvb_memory_mirror_metadata *mirror, size_t first, size_t length) {
    const size_t end = first + length;
    size_t cursor = first;
    while (cursor < end) {
        while (cursor < end &&
               mirror->mirror[cursor] == mirror->baseline[cursor])
            ++cursor;
        const size_t dirty_first = cursor;
        while (cursor < end &&
               mirror->mirror[cursor] != mirror->baseline[cursor])
            ++cursor;
        if (cursor != dirty_first) {
            const size_t dirty_length = cursor - dirty_first;
            memcpy(mirror->native + mirror->offset + dirty_first,
                   mirror->mirror + dirty_first, dirty_length);
            memcpy(mirror->baseline + dirty_first,
                   mirror->mirror + dirty_first, dirty_length);
        }
    }
}

int bvb_vulkan_global_context_setup_memory_mirror(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_memory_mirror_setup_request *request,
    int mirror_fd, int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || mirror_fd < 0 ||
        vulkan_result == NULL) return -EINVAL;
    *vulkan_result = VK_ERROR_MEMORY_MAP_FAILED;
    uint8_t validation[BVB_VULKAN_MEMORY_MIRROR_SETUP_SIZE];
    if (bvb_protocol_encode_vulkan_memory_mirror_setup_request(
            validation, request) != 0) return -EINVAL;
    struct stat metadata;
    const int seals = fcntl(mirror_fd, F_GET_SEALS);
    const int required_seals = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    if (fstat(mirror_fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_size < 0 ||
        (uint64_t)metadata.st_size != request->length || seals < 0 ||
        (seals & required_seals) != required_seals ||
        (seals & F_SEAL_WRITE) != 0) {
        set_error(error, error_size,
                  "memory mirror fd size or immutable-capacity seals invalid");
        return -EINVAL;
    }
    uint64_t memory_device_id = 0U, memory_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, request->memory_id, BVB_OBJECT_DEVICE_MEMORY,
        &memory_device_id, &device, &memory_bits);
    uint64_t device_bits = 0U;
    if (result == 0)
        result = bvb_handle_table_lookup(
            &context->objects, request->device_id, BVB_OBJECT_DEVICE,
            NULL, &device_bits);
    struct bvb_memory_metadata *native_metadata =
        memory_metadata_slot(context, request->memory_id);
    struct bvb_device_metadata *native_device_metadata =
        device_metadata_slot(context, request->device_id);
    if (result != 0 || memory_device_id != request->device_id ||
        device_from_bits(device_bits) != device || native_metadata == NULL ||
        native_metadata->memory_id != request->memory_id ||
        native_device_metadata == NULL ||
        native_device_metadata->device_id != request->device_id ||
        native_device_metadata->non_coherent_atom_size == 0U) {
        set_error(error, error_size,
                  "memory mirror references unknown or cross-device memory");
        return result != 0 ? result : -EPROTO;
    }
    if (!memory_is_upload_only(context, request->memory_id)) {
        set_error(error, error_size,
                  "memory mirror is not bound only to GPU-read-only buffers");
        *vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return 0;
    }
    if ((native_metadata->property_flags &
         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0U ||
        request->offset > native_metadata->allocation_size ||
        request->length > native_metadata->allocation_size - request->offset) {
        set_error(error, error_size,
                  "memory mirror range is not host-visible allocation data");
        return -ERANGE;
    }
    struct bvb_memory_mirror_metadata *slot =
        memory_mirror_slot(context, request->memory_id);
    if (slot == NULL || slot->memory_id != 0U ||
        context->memory_mirror_bytes >
            BVB_VULKAN_MEMORY_MIRROR_TOTAL_BYTES - request->length) {
        set_error(error, error_size,
                  "memory mirror slot or total-byte cap exhausted");
        return slot != NULL && slot->memory_id != 0U ? -EBUSY : -ENOSPC;
    }
    void *shared = mmap(NULL, (size_t)request->length,
                        PROT_READ | PROT_WRITE, MAP_SHARED, mirror_fd, 0);
    if (shared == MAP_FAILED) return -errno;
    uint8_t *baseline = malloc((size_t)request->length);
    if (baseline == NULL) {
        (void)munmap(shared, (size_t)request->length);
        return -ENOMEM;
    }
    PFN_vkMapMemory map = (PFN_vkMapMemory)context->get_device_proc_addr(
        device, "vkMapMemory");
    PFN_vkUnmapMemory unmap =
        (PFN_vkUnmapMemory)context->get_device_proc_addr(
            device, "vkUnmapMemory");
    if (map == NULL || unmap == NULL) {
        free(baseline);
        (void)munmap(shared, (size_t)request->length);
        return -ENOSYS;
    }
    void *native = NULL;
    *vulkan_result = map(device, memory_from_bits(memory_bits), 0U,
                          VK_WHOLE_SIZE, 0U, &native);
    if (*vulkan_result != VK_SUCCESS || native == NULL) {
        free(baseline);
        (void)munmap(shared, (size_t)request->length);
        return 0;
    }
    const struct bvb_memory_mirror_metadata candidate = {
        .device_id = request->device_id,
        .memory_id = request->memory_id,
        .generation = request->generation,
        .offset = request->offset,
        .length = request->length,
        .allocation_size = native_metadata->allocation_size,
        .non_coherent_atom_size =
            native_device_metadata->non_coherent_atom_size,
        .property_flags = native_metadata->property_flags,
        .device = device,
        .memory = memory_from_bits(memory_bits),
        .mirror = shared,
        .native = native,
        .baseline = baseline,
    };
    if ((candidate.property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) !=
        0U)
        memcpy(shared, (const uint8_t *)native + request->offset,
               (size_t)request->length);
    memcpy(baseline, shared, (size_t)request->length);
    atomic_thread_fence(memory_order_release);
    *slot = candidate;
    context->memory_mirror_bytes += request->length;
    return 0;
}

static int memory_mirror_range_operation(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_memory_mirror_range_request *request,
    bool invalidate, int32_t *vulkan_result, char *error,
    size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || vulkan_result == NULL)
        return -EINVAL;
    *vulkan_result = VK_ERROR_MEMORY_MAP_FAILED;
    uint8_t validation[BVB_VULKAN_MEMORY_MIRROR_RANGE_SIZE];
    if (bvb_protocol_encode_vulkan_memory_mirror_range_request(
            validation, request) != 0) return -EINVAL;
    struct bvb_memory_mirror_metadata *mirror = resolve_memory_mirror(
        context, request->device_id, request->memory_id,
        request->generation);
    if (mirror == NULL) {
        set_error(error, error_size, "stale or cross-device memory mirror");
        return -ESTALE;
    }
    if (request->offset < mirror->offset ||
        request->offset > mirror->offset + mirror->length ||
        request->size > mirror->offset + mirror->length - request->offset) {
        set_error(error, error_size, "memory mirror range is out of bounds");
        return -ERANGE;
    }
    const size_t mirror_offset = (size_t)(request->offset - mirror->offset);
    atomic_thread_fence(memory_order_acquire);
    if (invalidate &&
        (mirror->property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0U)
        *vulkan_result = maintain_native_memory_range(
            context, mirror, request->offset, request->size, true);
    else
        *vulkan_result = VK_SUCCESS;
    if (*vulkan_result != VK_SUCCESS) return 0;
    if (invalidate) {
        memcpy(mirror->mirror + mirror_offset,
               mirror->native + request->offset, (size_t)request->size);
        memcpy(mirror->baseline + mirror_offset,
               mirror->native + request->offset, (size_t)request->size);
        atomic_thread_fence(memory_order_release);
    } else {
        memcpy(mirror->native + request->offset,
               mirror->mirror + mirror_offset, (size_t)request->size);
        memcpy(mirror->baseline + mirror_offset,
               mirror->mirror + mirror_offset, (size_t)request->size);
        if ((mirror->property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ==
            0U)
            *vulkan_result = maintain_native_memory_range(
                context, mirror, request->offset, request->size, false);
    }
    return 0;
}

int bvb_vulkan_global_context_flush_memory_mirror(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_memory_mirror_range_request *request,
    int32_t *vulkan_result, char *error, size_t error_size) {
    return memory_mirror_range_operation(
        context, request, false, vulkan_result, error, error_size);
}

int bvb_vulkan_global_context_invalidate_memory_mirror(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_memory_mirror_range_request *request,
    int32_t *vulkan_result, char *error, size_t error_size) {
    return memory_mirror_range_operation(
        context, request, true, vulkan_result, error, error_size);
}

int bvb_vulkan_global_context_unmap_memory_mirror(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_memory_mirror_unmap_request *request,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL) return -EINVAL;
    uint8_t validation[BVB_VULKAN_MEMORY_MIRROR_UNMAP_SIZE];
    if (bvb_protocol_encode_vulkan_memory_mirror_unmap_request(
            validation, request) != 0) return -EINVAL;
    struct bvb_memory_mirror_metadata *mirror = resolve_memory_mirror(
        context, request->device_id, request->memory_id,
        request->generation);
    if (mirror == NULL) {
        set_error(error, error_size, "stale or cross-device memory mirror");
        return -ESTALE;
    }
    if ((mirror->property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0U) {
        atomic_thread_fence(memory_order_acquire);
        upload_host_diverged_range(mirror, 0U, (size_t)mirror->length);
    }
    release_memory_mirror(context, mirror);
    return 0;
}

static int sync_coherent_memory_mirrors(
    struct bvb_vulkan_global_context *context, uint64_t device_id) {
    atomic_thread_fence(memory_order_acquire);
    for (size_t index = 0U; index < BVB_VULKAN_MEMORY_MIRROR_CAPACITY;
         ++index) {
        struct bvb_memory_mirror_metadata *mirror =
            &context->memory_mirrors[index];
        if (mirror->memory_id != 0U && mirror->device_id == device_id &&
            (mirror->property_flags &
             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0U) {
            upload_host_diverged_range(mirror, 0U, (size_t)mirror->length);
        }
    }
    return 0;
}

int bvb_vulkan_global_context_create_fence(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_fence_create_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || response == NULL) return -EINVAL;
    *response = (struct bvb_vulkan_object_create_response){0};
    if ((request->flags & ~VK_FENCE_CREATE_SIGNALED_BIT) != 0U) {
        response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return 0;
    }
    uint64_t device_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, request->device_id, BVB_OBJECT_DEVICE, NULL,
        &device_bits);
    if (result != 0) return result;
    const VkDevice device = device_from_bits(device_bits);
    PFN_vkCreateFence create_fence =
        (PFN_vkCreateFence)context->get_device_proc_addr(device,
                                                         "vkCreateFence");
    PFN_vkDestroyFence destroy_fence =
        (PFN_vkDestroyFence)context->get_device_proc_addr(device,
                                                          "vkDestroyFence");
    if (create_fence == NULL || destroy_fence == NULL) return -ENOSYS;
    const VkFenceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = request->flags,
    };
    VkFence fence = VK_NULL_HANDLE;
    response->vulkan_result = create_fence(device, &create_info, NULL, &fence);
    if (response->vulkan_result != VK_SUCCESS) return 0;
    const uint64_t wire_id = bvb_handle_id(
        BVB_OBJECT_FENCE, context->next_fence_serial++);
    result = bvb_handle_table_insert(
        &context->objects, wire_id, request->device_id,
        handle_bits(&fence, sizeof(fence)));
    if (result != 0) {
        destroy_fence(device, fence, NULL);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    response->object_id = wire_id;
    return 0;
}

int bvb_vulkan_global_context_destroy_fence(
    struct bvb_vulkan_global_context *context, uint64_t fence_id,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    uint64_t device_id = 0U, fence_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, fence_id, BVB_OBJECT_FENCE, &device_id, &device, &fence_bits);
    if (result != 0) return result;
    PFN_vkDestroyFence destroy_fence =
        (PFN_vkDestroyFence)context->get_device_proc_addr(device,
                                                          "vkDestroyFence");
    if (destroy_fence == NULL) return -ENOSYS;
    result = bvb_handle_table_remove(
        &context->objects, fence_id, BVB_OBJECT_FENCE, NULL);
    if (result == 0) destroy_fence(device, fence_from_bits(fence_bits), NULL);
    return result;
}

int bvb_vulkan_global_context_get_fence_status(
    const struct bvb_vulkan_global_context *context, uint64_t fence_id,
    int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (vulkan_result == NULL) return -EINVAL;
    uint64_t device_id = 0U, fence_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, fence_id, BVB_OBJECT_FENCE, &device_id, &device, &fence_bits);
    if (result != 0) return result;
    PFN_vkGetFenceStatus get_status =
        (PFN_vkGetFenceStatus)context->get_device_proc_addr(
            device, "vkGetFenceStatus");
    if (get_status == NULL) return -ENOSYS;
    *vulkan_result = get_status(device, fence_from_bits(fence_bits));
    return 0;
}

int bvb_vulkan_global_context_wait_fence(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_fence_wait_request *request,
    int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (request == NULL || vulkan_result == NULL || request->wait_all > 1U)
        return -EINVAL;
    uint64_t device_id = 0U, fence_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, request->fence_id, BVB_OBJECT_FENCE, &device_id, &device,
        &fence_bits);
    if (result != 0) return result;
    PFN_vkWaitForFences wait =
        (PFN_vkWaitForFences)context->get_device_proc_addr(
            device, "vkWaitForFences");
    if (wait == NULL) return -ENOSYS;
    const VkFence fence = fence_from_bits(fence_bits);
    *vulkan_result = wait(device, 1U, &fence,
                          request->wait_all != 0U ? VK_TRUE : VK_FALSE,
                          request->timeout);
    return 0;
}

int bvb_vulkan_global_context_reset_fence(
    const struct bvb_vulkan_global_context *context, uint64_t fence_id,
    int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (vulkan_result == NULL) return -EINVAL;
    uint64_t device_id = 0U, fence_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, fence_id, BVB_OBJECT_FENCE, &device_id, &device, &fence_bits);
    if (result != 0) return result;
    PFN_vkResetFences reset =
        (PFN_vkResetFences)context->get_device_proc_addr(device,
                                                         "vkResetFences");
    if (reset == NULL) return -ENOSYS;
    const VkFence fence = fence_from_bits(fence_bits);
    *vulkan_result = reset(device, 1U, &fence);
    return 0;
}

static struct bvb_semaphore_metadata *semaphore_metadata_slot(
    struct bvb_vulkan_global_context *context, uint64_t semaphore_id) {
    struct bvb_semaphore_metadata *empty = NULL;
    for (size_t index = 0U; index < BVB_GLOBAL_OBJECT_CAPACITY; ++index) {
        struct bvb_semaphore_metadata *metadata =
            &context->semaphore_metadata[index];
        if (metadata->semaphore_id == semaphore_id && semaphore_id != 0U)
            return metadata;
        if (metadata->semaphore_id == 0U && empty == NULL) empty = metadata;
    }
    return empty;
}

int bvb_vulkan_global_context_create_semaphore(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_semaphore_create_request *request,
    struct bvb_vulkan_object_create_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || response == NULL) return -EINVAL;
    *response = (struct bvb_vulkan_object_create_response){0};
    if (request->flags != 0U ||
        (request->semaphore_type != VK_SEMAPHORE_TYPE_BINARY &&
         request->semaphore_type != VK_SEMAPHORE_TYPE_TIMELINE) ||
        (request->semaphore_type == VK_SEMAPHORE_TYPE_BINARY &&
         request->initial_value != 0U)) {
        response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return 0;
    }
    struct bvb_semaphore_metadata *metadata =
        semaphore_metadata_slot(context, 0U);
    if (metadata == NULL) {
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    uint64_t device_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, request->device_id, BVB_OBJECT_DEVICE, NULL,
        &device_bits);
    if (result != 0) return result;
    const VkDevice device = device_from_bits(device_bits);
    PFN_vkCreateSemaphore create =
        (PFN_vkCreateSemaphore)context->get_device_proc_addr(
            device, "vkCreateSemaphore");
    PFN_vkDestroySemaphore destroy =
        (PFN_vkDestroySemaphore)context->get_device_proc_addr(
            device, "vkDestroySemaphore");
    if (create == NULL || destroy == NULL) return -ENOSYS;
    const VkSemaphoreTypeCreateInfo type_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = (VkSemaphoreType)request->semaphore_type,
        .initialValue = request->initial_value,
    };
    const VkSemaphoreCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = request->semaphore_type == VK_SEMAPHORE_TYPE_TIMELINE
                     ? &type_info : NULL,
        .flags = request->flags,
    };
    VkSemaphore semaphore = VK_NULL_HANDLE;
    response->vulkan_result = create(device, &create_info, NULL, &semaphore);
    if (response->vulkan_result != VK_SUCCESS) return 0;
    const uint64_t wire_id = bvb_handle_id(
        BVB_OBJECT_SEMAPHORE, context->next_semaphore_serial++);
    result = bvb_handle_table_insert(
        &context->objects, wire_id, request->device_id,
        handle_bits(&semaphore, sizeof(semaphore)));
    if (result != 0) {
        destroy(device, semaphore, NULL);
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    *metadata = (struct bvb_semaphore_metadata){
        .semaphore_id = wire_id,
        .type = (VkSemaphoreType)request->semaphore_type,
    };
    response->object_id = wire_id;
    return 0;
}

int bvb_vulkan_global_context_destroy_semaphore(
    struct bvb_vulkan_global_context *context, uint64_t semaphore_id,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    uint64_t device_id = 0U, semaphore_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, semaphore_id, BVB_OBJECT_SEMAPHORE, &device_id, &device,
        &semaphore_bits);
    if (result != 0) return result;
    PFN_vkDestroySemaphore destroy =
        (PFN_vkDestroySemaphore)context->get_device_proc_addr(
            device, "vkDestroySemaphore");
    if (destroy == NULL) return -ENOSYS;
    result = bvb_handle_table_remove(
        &context->objects, semaphore_id, BVB_OBJECT_SEMAPHORE, NULL);
    if (result == 0) {
        destroy(device, semaphore_from_bits(semaphore_bits), NULL);
        struct bvb_semaphore_metadata *metadata =
            semaphore_metadata_slot(context, semaphore_id);
        if (metadata != NULL && metadata->semaphore_id == semaphore_id)
            *metadata = (struct bvb_semaphore_metadata){0};
    }
    return result;
}

int bvb_vulkan_global_context_get_semaphore_counter(
    const struct bvb_vulkan_global_context *context, uint64_t semaphore_id,
    struct bvb_vulkan_semaphore_counter_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (response == NULL) return -EINVAL;
    *response = (struct bvb_vulkan_semaphore_counter_response){
        .vulkan_result = VK_ERROR_INITIALIZATION_FAILED,
    };
    uint64_t device_id = 0U, semaphore_bits = 0U;
    VkDevice device = VK_NULL_HANDLE;
    int result = resolve_device_child(
        context, semaphore_id, BVB_OBJECT_SEMAPHORE, &device_id, &device,
        &semaphore_bits);
    if (result != 0) return result;
    PFN_vkGetSemaphoreCounterValue get_counter =
        (PFN_vkGetSemaphoreCounterValue)context->get_device_proc_addr(
            device, "vkGetSemaphoreCounterValue");
    if (get_counter == NULL) return -ENOSYS;
    response->vulkan_result = get_counter(
        device, semaphore_from_bits(semaphore_bits), &response->value);
    return 0;
}

int bvb_vulkan_global_context_wait_semaphores(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_semaphore_wait_request *request,
    int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || vulkan_result == NULL ||
        request->semaphore_count == 0U ||
        request->semaphore_count > BVB_VULKAN_MAX_SEMAPHORES_PER_WAIT ||
        (request->flags & ~VK_SEMAPHORE_WAIT_ANY_BIT) != 0U) return -EINVAL;
    uint64_t device_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, request->device_id, BVB_OBJECT_DEVICE, NULL,
        &device_bits);
    if (result != 0) return result;
    const VkDevice device = device_from_bits(device_bits);
    VkSemaphore semaphores[BVB_VULKAN_MAX_SEMAPHORES_PER_WAIT];
    uint64_t values[BVB_VULKAN_MAX_SEMAPHORES_PER_WAIT];
    for (uint32_t index = 0U; index < request->semaphore_count; ++index) {
        uint64_t parent_id = 0U, semaphore_bits = 0U;
        result = bvb_handle_table_lookup(
            &context->objects, request->semaphores[index].semaphore_id,
            BVB_OBJECT_SEMAPHORE, &parent_id, &semaphore_bits);
        if (result != 0 || parent_id != request->device_id) return -EPROTO;
        semaphores[index] = semaphore_from_bits(semaphore_bits);
        values[index] = request->semaphores[index].value;
    }
    PFN_vkWaitSemaphores wait =
        (PFN_vkWaitSemaphores)context->get_device_proc_addr(
            device, "vkWaitSemaphores");
    if (wait == NULL) return -ENOSYS;
    const VkSemaphoreWaitInfo wait_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .flags = request->flags,
        .semaphoreCount = request->semaphore_count,
        .pSemaphores = semaphores,
        .pValues = values,
    };
    *vulkan_result = wait(device, &wait_info, request->timeout);
    return 0;
}

int bvb_vulkan_global_context_signal_semaphore(
    const struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_semaphore_signal_request *request,
    int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || vulkan_result == NULL)
        return -EINVAL;
    uint64_t parent_id = 0U, semaphore_bits = 0U;
    int result = bvb_handle_table_lookup(
        &context->objects, request->semaphore_id, BVB_OBJECT_SEMAPHORE,
        &parent_id, &semaphore_bits);
    if (result != 0 || parent_id != request->device_id) return -EPROTO;
    uint64_t device_bits = 0U;
    result = bvb_handle_table_lookup(
        &context->objects, request->device_id, BVB_OBJECT_DEVICE, NULL,
        &device_bits);
    if (result != 0) return result;
    const VkDevice device = device_from_bits(device_bits);
    PFN_vkSignalSemaphore signal =
        (PFN_vkSignalSemaphore)context->get_device_proc_addr(
            device, "vkSignalSemaphore");
    if (signal == NULL) return -ENOSYS;
    const VkSemaphoreSignalInfo signal_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
        .semaphore = semaphore_from_bits(semaphore_bits),
        .value = request->value,
    };
    *vulkan_result = signal(device, &signal_info);
    return 0;
}

int bvb_vulkan_global_context_queue_submit_2(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_queue_submit_2_request *request,
    int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || vulkan_result == NULL ||
        request->flags != 0U ||
        request->wait_count > BVB_VULKAN_MAX_SEMAPHORES_PER_WAIT ||
        request->command_count > BVB_VULKAN_MAX_COMMAND_BUFFERS_PER_SUBMIT ||
        request->signal_count > BVB_VULKAN_MAX_SEMAPHORES_PER_WAIT)
        return -EINVAL;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    int result = resolve_queue(context, request->queue_id, &device, &queue);
    uint64_t device_id = 0U, queue_bits = 0U;
    if (result == 0)
        result = bvb_handle_table_lookup(
            &context->objects, request->queue_id, BVB_OBJECT_QUEUE,
            &device_id, &queue_bits);
    VkSemaphoreSubmitInfo waits[BVB_VULKAN_MAX_SEMAPHORES_PER_WAIT];
    VkCommandBufferSubmitInfo
        commands[BVB_VULKAN_MAX_COMMAND_BUFFERS_PER_SUBMIT];
    VkSemaphoreSubmitInfo signals[BVB_VULKAN_MAX_SEMAPHORES_PER_WAIT];
    for (uint32_t index = 0U; result == 0 && index < request->wait_count;
         ++index) {
        uint64_t parent_id = 0U, semaphore_bits = 0U;
        result = bvb_handle_table_lookup(
            &context->objects, request->waits[index].semaphore_id,
            BVB_OBJECT_SEMAPHORE, &parent_id, &semaphore_bits);
        if (result == 0 &&
            (parent_id != device_id || request->waits[index].device_index != 0U))
            result = -EPROTO;
        if (result == 0)
            waits[index] = (VkSemaphoreSubmitInfo){
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .semaphore = semaphore_from_bits(semaphore_bits),
                .value = request->waits[index].value,
                .stageMask = request->waits[index].stage_mask,
                .deviceIndex = request->waits[index].device_index,
            };
    }
    for (uint32_t index = 0U; result == 0 && index < request->command_count;
         ++index) {
        uint64_t command_device_id = 0U;
        VkDevice command_device = VK_NULL_HANDLE;
        VkCommandPool command_pool = VK_NULL_HANDLE;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        result = resolve_command_buffer(
            context, request->commands[index].command_buffer_id,
            &command_device_id, &command_device, &command_pool,
            &command_buffer);
        if (result == 0 &&
            (command_device_id != device_id || command_device != device ||
             request->commands[index].device_mask != 0U))
            result = -EPROTO;
        if (result == 0)
            commands[index] = (VkCommandBufferSubmitInfo){
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .commandBuffer = command_buffer,
                .deviceMask = request->commands[index].device_mask,
            };
    }
    for (uint32_t index = 0U; result == 0 && index < request->signal_count;
         ++index) {
        uint64_t parent_id = 0U, semaphore_bits = 0U;
        result = bvb_handle_table_lookup(
            &context->objects, request->signals[index].semaphore_id,
            BVB_OBJECT_SEMAPHORE, &parent_id, &semaphore_bits);
        if (result == 0 &&
            (parent_id != device_id ||
             request->signals[index].device_index != 0U))
            result = -EPROTO;
        if (result == 0)
            signals[index] = (VkSemaphoreSubmitInfo){
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .semaphore = semaphore_from_bits(semaphore_bits),
                .value = request->signals[index].value,
                .stageMask = request->signals[index].stage_mask,
                .deviceIndex = request->signals[index].device_index,
            };
    }
    VkFence fence = VK_NULL_HANDLE;
    if (result == 0 && request->fence_id != 0U) {
        uint64_t fence_device_id = 0U, fence_bits = 0U;
        VkDevice fence_device = VK_NULL_HANDLE;
        result = resolve_device_child(
            context, request->fence_id, BVB_OBJECT_FENCE, &fence_device_id,
            &fence_device, &fence_bits);
        if (result == 0 &&
            (fence_device_id != device_id || fence_device != device))
            result = -EPROTO;
        if (result == 0) fence = fence_from_bits(fence_bits);
    }
    if (result != 0) {
        set_error(error, error_size,
                  "submit2 references objects from different devices");
        return result;
    }
    result = sync_coherent_memory_mirrors(context, device_id);
    if (result != 0) return result;
    PFN_vkQueueSubmit2 submit =
        (PFN_vkQueueSubmit2)context->get_device_proc_addr(
            device, "vkQueueSubmit2");
    if (submit == NULL)
        submit = (PFN_vkQueueSubmit2)context->get_device_proc_addr(
            device, "vkQueueSubmit2KHR");
    if (submit == NULL) return -ENOSYS;
    const VkSubmitInfo2 submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .flags = request->flags,
        .waitSemaphoreInfoCount = request->wait_count,
        .pWaitSemaphoreInfos = request->wait_count != 0U ? waits : NULL,
        .commandBufferInfoCount = request->command_count,
        .pCommandBufferInfos = request->command_count != 0U ? commands : NULL,
        .signalSemaphoreInfoCount = request->signal_count,
        .pSignalSemaphoreInfos = request->signal_count != 0U ? signals : NULL,
    };
    *vulkan_result = submit(queue, 1U, &submit_info, fence);
    return 0;
}

int bvb_vulkan_global_context_queue_submit_command_fence(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_queue_submit_command_fence_request *request,
    int32_t *vulkan_result, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || vulkan_result == NULL)
        return -EINVAL;
    VkDevice queue_device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    int result = resolve_queue(context, request->queue_id, &queue_device,
                               &queue);
    uint64_t queue_device_id = 0U, queue_bits = 0U;
    if (result == 0)
        result = bvb_handle_table_lookup(
            &context->objects, request->queue_id, BVB_OBJECT_QUEUE,
            &queue_device_id, &queue_bits);
    uint64_t command_device_id = 0U;
    VkDevice command_device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    if (result == 0)
        result = resolve_command_buffer(
            context, request->command_buffer_id, &command_device_id,
            &command_device, &command_pool, &command_buffer);
    uint64_t fence_device_id = 0U, fence_bits = 0U;
    VkDevice fence_device = VK_NULL_HANDLE;
    if (result == 0)
        result = resolve_device_child(
            context, request->fence_id, BVB_OBJECT_FENCE, &fence_device_id,
            &fence_device, &fence_bits);
    if (result != 0 || command_device_id != queue_device_id ||
        fence_device_id != queue_device_id || command_device != queue_device ||
        fence_device != queue_device) {
        set_error(error, error_size,
                  "queue, command buffer, and fence have different devices");
        return result != 0 ? result : -EPROTO;
    }
    result = sync_coherent_memory_mirrors(context, queue_device_id);
    if (result != 0) return result;
    PFN_vkQueueSubmit submit =
        (PFN_vkQueueSubmit)context->get_device_proc_addr(queue_device,
                                                         "vkQueueSubmit");
    if (submit == NULL) return -ENOSYS;
    const VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1U,
        .pCommandBuffers = &command_buffer,
    };
    *vulkan_result = submit(queue, 1U, &submit_info,
                            fence_from_bits(fence_bits));
    return 0;
}

static struct bvb_swapchain_metadata *swapchain_metadata_slot(
    struct bvb_vulkan_global_context *context, uint64_t swapchain_id) {
    struct bvb_swapchain_metadata *empty = NULL;
    for (size_t index = 0U; index < BVB_WSI_FRAME_RING_MAX_SLOTS; ++index) {
        struct bvb_swapchain_metadata *metadata =
            &context->swapchain_metadata[index];
        if (metadata->swapchain_id == swapchain_id && swapchain_id != 0U) {
            return metadata;
        }
        if (metadata->swapchain_id == 0U && empty == NULL) empty = metadata;
    }
    return empty;
}

static int destroy_swapchain_metadata(
    struct bvb_vulkan_global_context *context,
    struct bvb_swapchain_metadata *metadata) {
    if (context == NULL || metadata == NULL) return -EINVAL;
    PFN_vkDestroyImage destroy_image = NULL;
    PFN_vkFreeMemory free_memory = NULL;
    PFN_vkDeviceWaitIdle wait_idle = NULL;
    PFN_vkDestroyFence destroy_fence = NULL;
    PFN_vkDestroyCommandPool destroy_command_pool = NULL;
    if (metadata->device != VK_NULL_HANDLE &&
        context->get_device_proc_addr != NULL) {
        destroy_image = (PFN_vkDestroyImage)context->get_device_proc_addr(
            metadata->device, "vkDestroyImage");
        free_memory = (PFN_vkFreeMemory)context->get_device_proc_addr(
            metadata->device, "vkFreeMemory");
        wait_idle = (PFN_vkDeviceWaitIdle)context->get_device_proc_addr(
            metadata->device, "vkDeviceWaitIdle");
        destroy_fence = (PFN_vkDestroyFence)context->get_device_proc_addr(
            metadata->device, "vkDestroyFence");
        destroy_command_pool =
            (PFN_vkDestroyCommandPool)context->get_device_proc_addr(
                metadata->device, "vkDestroyCommandPool");
    }
    int status = 0;
    if (metadata->control != NULL && metadata->control != MAP_FAILED) {
        const int failed =
            bvb_wsi_frame_ring_fail_producer(metadata->control, -EPIPE);
        if (failed != 0 && status == 0) status = failed;
    }
    if (wait_idle != NULL) {
        const VkResult waited = wait_idle(metadata->device);
        if (waited != VK_SUCCESS && status == 0) status = -EIO;
    }
    if (metadata->present_completion != VK_NULL_HANDLE &&
        destroy_fence != NULL) {
        destroy_fence(metadata->device, metadata->present_completion, NULL);
    }
    if (metadata->producer_command_pool != VK_NULL_HANDLE &&
        destroy_command_pool != NULL) {
        destroy_command_pool(metadata->device,
                             metadata->producer_command_pool, NULL);
    }
    for (uint32_t index = 0U; index < metadata->image_count; ++index) {
        if (metadata->image_ids[index] != 0U) {
            struct bvb_image_metadata *image_metadata =
                image_metadata_slot(context, metadata->image_ids[index]);
            if (image_metadata != NULL &&
                image_metadata->image_id == metadata->image_ids[index]) {
                *image_metadata = (struct bvb_image_metadata){0};
            }
            const int removed = bvb_handle_table_remove(
                &context->objects, metadata->image_ids[index],
                BVB_OBJECT_IMAGE, NULL);
            if (removed != 0 && status == 0) status = removed;
        }
        if (metadata->images[index] != VK_NULL_HANDLE &&
            destroy_image != NULL) {
            destroy_image(metadata->device, metadata->images[index], NULL);
        }
        if (metadata->memories[index] != VK_NULL_HANDLE &&
            free_memory != NULL) {
            free_memory(metadata->device, metadata->memories[index], NULL);
        }
#ifdef __ANDROID__
        if (metadata->hardware_buffers[index] != NULL &&
            context->ahardwarebuffer_release != NULL) {
            context->ahardwarebuffer_release(
                (AHardwareBuffer *)metadata->hardware_buffers[index]);
        }
#endif
    }
    if (metadata->swapchain_id != 0U) {
        const int removed = bvb_handle_table_remove(
            &context->objects, metadata->swapchain_id,
            BVB_OBJECT_SWAPCHAIN, NULL);
        if (removed != 0 && status == 0) status = removed;
    }
    if (metadata->control != NULL && metadata->control != MAP_FAILED) {
        if (munmap(metadata->control, BVB_WSI_FRAME_RING_REGION_BYTES) != 0 &&
            status == 0) status = -errno;
    }
    if (metadata->control_fd >= 0 && close(metadata->control_fd) != 0 &&
        status == 0) status = -errno;
    *metadata = (struct bvb_swapchain_metadata){.control_fd = -1};
    return status;
}

static int create_swapchain_control(
    struct bvb_swapchain_metadata *metadata, uint32_t image_count,
    uint64_t generation) {
    metadata->control_fd = (int)syscall(
        SYS_memfd_create, "bvb-wsi-frame-ring",
        MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (metadata->control_fd < 0) return -errno;
    if (ftruncate(metadata->control_fd, BVB_WSI_FRAME_RING_REGION_BYTES) != 0) {
        return -errno;
    }
    metadata->control = mmap(
        NULL, BVB_WSI_FRAME_RING_REGION_BYTES, PROT_READ | PROT_WRITE,
        MAP_SHARED, metadata->control_fd, 0U);
    if (metadata->control == MAP_FAILED) return -errno;
    return bvb_wsi_frame_ring_initialize(
        metadata->control, BVB_WSI_FRAME_RING_REGION_BYTES, image_count,
        generation);
}

static int choose_swapchain_memory_type(
    const VkPhysicalDeviceMemoryProperties *properties,
    uint32_t memory_type_bits, uint32_t *memory_type_index) {
    if (properties == NULL || memory_type_index == NULL) return -EINVAL;
    bool found = false;
    bool device_local_found = false;
    for (uint32_t index = 0U; index < properties->memoryTypeCount; ++index) {
        if ((memory_type_bits & (UINT32_C(1) << index)) == 0U) continue;
        const bool device_local =
            (properties->memoryTypes[index].propertyFlags &
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U;
        if (!found || (device_local && !device_local_found)) {
            *memory_type_index = index;
            found = true;
            device_local_found = device_local;
        }
    }
    return found ? 0 : -ENODEV;
}

static int create_swapchain_producer_resources(
    struct bvb_vulkan_global_context *context,
    struct bvb_swapchain_metadata *metadata, char *error,
    size_t error_size) {
    struct bvb_device_metadata *device_metadata =
        device_metadata_slot(context, metadata->device_id);
    if (device_metadata == NULL ||
        device_metadata->device_id != metadata->device_id ||
        device_metadata->queue_create_info_count == 0U ||
        device_metadata->queue_create_infos[0].queue_count == 0U) {
        set_error(error, error_size, "swapchain device has no producer queue");
        return -ENODEV;
    }
    metadata->queue_family_index =
        device_metadata->queue_create_infos[0].queue_family_index;
    PFN_vkGetDeviceQueue get_queue =
        (PFN_vkGetDeviceQueue)context->get_device_proc_addr(
            metadata->device, "vkGetDeviceQueue");
    PFN_vkCreateCommandPool create_pool =
        (PFN_vkCreateCommandPool)context->get_device_proc_addr(
            metadata->device, "vkCreateCommandPool");
    PFN_vkAllocateCommandBuffers allocate_commands =
        (PFN_vkAllocateCommandBuffers)context->get_device_proc_addr(
            metadata->device, "vkAllocateCommandBuffers");
    PFN_vkCreateFence create_fence =
        (PFN_vkCreateFence)context->get_device_proc_addr(
            metadata->device, "vkCreateFence");
    if (get_queue == NULL || create_pool == NULL ||
        allocate_commands == NULL || create_fence == NULL) {
        set_error(error, error_size,
                  "device lacks producer synchronization entry points");
        return -ENOSYS;
    }
    get_queue(metadata->device, metadata->queue_family_index, 0U,
              &metadata->producer_queue);
    if (metadata->producer_queue == VK_NULL_HANDLE) {
        set_error(error, error_size, "producer queue resolution failed");
        return -ENODEV;
    }
    const VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
                 VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = metadata->queue_family_index,
    };
    VkResult result = create_pool(metadata->device, &pool_info, NULL,
                                  &metadata->producer_command_pool);
    if (result != VK_SUCCESS) {
        set_error(error, error_size, "producer command-pool create: %d",
                  (int)result);
        return 0;
    }
    VkCommandBuffer commands[2U * BVB_WSI_FRAME_RING_MAX_SLOTS] = {0};
    const VkCommandBufferAllocateInfo allocation_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = metadata->producer_command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 2U * metadata->image_count,
    };
    result = allocate_commands(metadata->device, &allocation_info, commands);
    if (result == VK_SUCCESS) {
        for (uint32_t index = 0U; index < metadata->image_count; ++index) {
            metadata->acquire_commands[index] = commands[index];
            metadata->present_commands[index] =
                commands[metadata->image_count + index];
        }
    }
    const VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    if (result == VK_SUCCESS) {
        result = create_fence(metadata->device, &fence_info, NULL,
                              &metadata->present_completion);
    }
    if (result != VK_SUCCESS) {
        set_error(error, error_size, "producer synchronization create: %d",
                  (int)result);
        return 0;
    }
    return 0;
}

int bvb_vulkan_global_context_prepare_swapchain(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_swapchain_prepare_request *request,
    struct bvb_vulkan_swapchain_prepare_response *response,
    int descriptors[BVB_WSI_FRAME_RING_MAX_SLOTS + 1U],
    size_t *descriptor_count,
    void *hardware_buffers[BVB_WSI_FRAME_RING_MAX_SLOTS],
    size_t *hardware_buffer_count, char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || response == NULL ||
        descriptors == NULL || descriptor_count == NULL) return -EINVAL;
    if (hardware_buffers == NULL || hardware_buffer_count == NULL)
        return -EINVAL;
    *response = (struct bvb_vulkan_swapchain_prepare_response){0};
    *descriptor_count = 0U;
    *hardware_buffer_count = 0U;
    for (size_t index = 0U; index < BVB_WSI_FRAME_RING_MAX_SLOTS + 1U;
         ++index) descriptors[index] = -1;
    for (size_t index = 0U; index < BVB_WSI_FRAME_RING_MAX_SLOTS; ++index)
        hardware_buffers[index] = NULL;
    uint8_t request_validation[BVB_VULKAN_SWAPCHAIN_PREPARE_REQUEST_SIZE];
    if (bvb_protocol_encode_vulkan_swapchain_prepare_request(
            request_validation, request) != 0 ||
        (request->format != VK_FORMAT_R8G8B8A8_UNORM &&
         request->format != VK_FORMAT_B8G8R8A8_UNORM)) {
        return -EINVAL;
    }
    if (context->objects.capacity - context->objects.count <
        (size_t)request->min_image_count + 1U) {
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    struct bvb_swapchain_metadata *metadata =
        swapchain_metadata_slot(context, 0U);
    if (metadata == NULL) {
        response->vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
        return 0;
    }
    *metadata = (struct bvb_swapchain_metadata){
        .device_id = request->device_id,
        .generation = request->generation,
        .image_count = request->min_image_count,
        .control_fd = -1,
        .control = MAP_FAILED,
    };
    uint64_t physical_id = 0U;
    uint64_t device_bits = 0U;
    int status = bvb_handle_table_lookup(
        &context->objects, request->device_id, BVB_OBJECT_DEVICE,
        &physical_id, &device_bits);
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    if (status == 0) {
        status = resolve_physical_device(
            context, physical_id, &instance, &physical_device);
    }
    if (status != 0) {
        set_error(error, error_size, "unknown swapchain device");
        *metadata = (struct bvb_swapchain_metadata){.control_fd = -1};
        return status;
    }
    metadata->device = device_from_bits(device_bits);
    PFN_vkGetPhysicalDeviceImageFormatProperties2 query =
        (PFN_vkGetPhysicalDeviceImageFormatProperties2)
            context->get_instance_proc_addr(
                instance, "vkGetPhysicalDeviceImageFormatProperties2");
    if (query == NULL) {
        query = (PFN_vkGetPhysicalDeviceImageFormatProperties2)
            context->get_instance_proc_addr(
                instance, "vkGetPhysicalDeviceImageFormatProperties2KHR");
    }
    PFN_vkCreateImage create_image =
        (PFN_vkCreateImage)context->get_device_proc_addr(
            metadata->device, "vkCreateImage");
    PFN_vkDestroyImage destroy_image =
        (PFN_vkDestroyImage)context->get_device_proc_addr(
            metadata->device, "vkDestroyImage");
    PFN_vkGetImageMemoryRequirements get_requirements =
        (PFN_vkGetImageMemoryRequirements)context->get_device_proc_addr(
            metadata->device, "vkGetImageMemoryRequirements");
    PFN_vkAllocateMemory allocate_memory =
        (PFN_vkAllocateMemory)context->get_device_proc_addr(
            metadata->device, "vkAllocateMemory");
    PFN_vkFreeMemory free_memory =
        (PFN_vkFreeMemory)context->get_device_proc_addr(
            metadata->device, "vkFreeMemory");
    PFN_vkBindImageMemory bind_image =
        (PFN_vkBindImageMemory)context->get_device_proc_addr(
            metadata->device, "vkBindImageMemory");
#ifdef __ANDROID__
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID get_hardware_buffer_properties =
        (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)
            context->get_device_proc_addr(
                metadata->device,
                "vkGetAndroidHardwareBufferPropertiesANDROID");
#else
    PFN_vkGetMemoryFdKHR get_memory_fd =
        (PFN_vkGetMemoryFdKHR)context->get_device_proc_addr(
            metadata->device, "vkGetMemoryFdKHR");
#endif
    if (query == NULL || create_image == NULL || destroy_image == NULL ||
        get_requirements == NULL || allocate_memory == NULL ||
        free_memory == NULL || bind_image == NULL
#ifdef __ANDROID__
        || get_hardware_buffer_properties == NULL
#else
        || get_memory_fd == NULL
#endif
    ) {
        set_error(error, error_size,
                  "device lacks external-image ring entry points:");
        if (query == NULL)
            append_error_entry_point(
                error, error_size,
                "vkGetPhysicalDeviceImageFormatProperties2[KHR]");
        if (create_image == NULL)
            append_error_entry_point(error, error_size, "vkCreateImage");
        if (destroy_image == NULL)
            append_error_entry_point(error, error_size, "vkDestroyImage");
        if (get_requirements == NULL)
            append_error_entry_point(
                error, error_size, "vkGetImageMemoryRequirements");
        if (allocate_memory == NULL)
            append_error_entry_point(error, error_size, "vkAllocateMemory");
        if (free_memory == NULL)
            append_error_entry_point(error, error_size, "vkFreeMemory");
        if (bind_image == NULL)
            append_error_entry_point(error, error_size, "vkBindImageMemory");
#ifdef __ANDROID__
        if (get_hardware_buffer_properties == NULL)
            append_error_entry_point(
                error, error_size,
                "vkGetAndroidHardwareBufferPropertiesANDROID");
#else
        if (get_memory_fd == NULL)
            append_error_entry_point(error, error_size, "vkGetMemoryFdKHR");
#endif
        *metadata = (struct bvb_swapchain_metadata){.control_fd = -1};
        return -ENOSYS;
    }
    const VkImageUsageFlags transport_usage =
#ifdef __ANDROID__
        (VkImageUsageFlags)request->image_usage;
#else
        (VkImageUsageFlags)request->image_usage |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
#endif
#ifdef __ANDROID__
    const VkExternalMemoryHandleTypeFlagBits transport_handle_type =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    status = ensure_android_hardware_buffer_api(context);
    if (status != 0 || request->format != VK_FORMAT_R8G8B8A8_UNORM) {
        response->vulkan_result = status == 0
            ? VK_ERROR_FORMAT_NOT_SUPPORTED
            : VK_ERROR_INITIALIZATION_FAILED;
        *metadata = (struct bvb_swapchain_metadata){.control_fd = -1};
        return status == 0 ? 0 : status;
    }
    const AHardwareBuffer_Desc hardware_buffer_description = {
        .width = request->width,
        .height = request->height,
        .layers = 1U,
        .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT,
    };
    if (context->ahardwarebuffer_is_supported(
            &hardware_buffer_description) == 0) {
        response->vulkan_result = VK_ERROR_FORMAT_NOT_SUPPORTED;
        *metadata = (struct bvb_swapchain_metadata){.control_fd = -1};
        return 0;
    }
#else
    const VkExternalMemoryHandleTypeFlagBits transport_handle_type =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
#endif
    const VkPhysicalDeviceExternalImageFormatInfo external_query = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .handleType = transport_handle_type,
    };
    const VkPhysicalDeviceImageFormatInfo2 format_query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &external_query,
        .format = (VkFormat)request->format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = transport_usage,
    };
    VkExternalImageFormatProperties external_properties = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
    };
    VkImageFormatProperties2 format_properties = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &external_properties,
    };
    VkResult vulkan_result = query(
        physical_device, &format_query, &format_properties);
    const VkExternalMemoryFeatureFlags required_features =
#ifdef __ANDROID__
        VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
#else
        VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
        VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
#endif
    if (vulkan_result != VK_SUCCESS ||
        request->width > format_properties.imageFormatProperties.maxExtent.width ||
        request->height > format_properties.imageFormatProperties.maxExtent.height ||
        (external_properties.externalMemoryProperties.externalMemoryFeatures &
         required_features) != required_features ||
        (external_properties.externalMemoryProperties.compatibleHandleTypes &
         transport_handle_type) == 0U) {
        response->vulkan_result = vulkan_result != VK_SUCCESS
            ? vulkan_result : VK_ERROR_FORMAT_NOT_SUPPORTED;
        *metadata = (struct bvb_swapchain_metadata){.control_fd = -1};
        return 0;
    }
    VkPhysicalDeviceMemoryProperties memory_properties = {0};
    status = bvb_vulkan_global_context_get_memory_properties(
        context, physical_id, &memory_properties, error, error_size);
    if (status != 0) {
        *metadata = (struct bvb_swapchain_metadata){.control_fd = -1};
        return status;
    }
    status = create_swapchain_control(
        metadata, request->min_image_count, request->generation);
    if (status != 0) {
        set_error(error, error_size, "frame-ring control allocation failed");
        (void)destroy_swapchain_metadata(context, metadata);
        return status;
    }
    status = create_swapchain_producer_resources(
        context, metadata, error, error_size);
    if (status != 0) {
        (void)destroy_swapchain_metadata(context, metadata);
        return status;
    }
    const VkExternalMemoryImageCreateInfo external_image_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = transport_handle_type,
    };
    const VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_image_info,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = (VkFormat)request->format,
        .extent = {request->width, request->height, 1U},
        .mipLevels = 1U,
        .arrayLayers = 1U,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = transport_usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const char *failure_stage = "none";
    uint32_t failure_index = 0U;
    uint32_t failure_image_memory_bits = 0U;
    uint32_t failure_hardware_memory_bits = 0U;
    uint64_t failure_image_size = 0U;
    uint64_t failure_allocation_size = 0U;
    for (uint32_t index = 0U; index < metadata->image_count; ++index) {
        failure_index = index;
#ifdef __ANDROID__
        AHardwareBuffer *hardware_buffer = NULL;
        if (context->ahardwarebuffer_allocate(
                &hardware_buffer_description, &hardware_buffer) != 0 ||
            hardware_buffer == NULL) {
            failure_stage = "ahardwarebuffer_allocate";
            vulkan_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
            break;
        }
        metadata->hardware_buffers[index] = hardware_buffer;
#endif
        vulkan_result = create_image(
            metadata->device, &image_info, NULL, &metadata->images[index]);
        if (vulkan_result != VK_SUCCESS) {
            failure_stage = "vkCreateImage";
            break;
        }
        VkMemoryRequirements requirements = {0};
        get_requirements(
            metadata->device, metadata->images[index], &requirements);
        failure_image_memory_bits = requirements.memoryTypeBits;
        failure_image_size = requirements.size;
#ifdef __ANDROID__
        VkAndroidHardwareBufferPropertiesANDROID hardware_properties = {
            .sType =
                VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
        };
        vulkan_result = get_hardware_buffer_properties(
            metadata->device, hardware_buffer, &hardware_properties);
        failure_hardware_memory_bits = hardware_properties.memoryTypeBits;
        failure_allocation_size = hardware_properties.allocationSize;
        const uint32_t compatible_memory_types =
            requirements.memoryTypeBits & hardware_properties.memoryTypeBits;
        status = vulkan_result == VK_SUCCESS
            ? choose_swapchain_memory_type(
                  &memory_properties, compatible_memory_types,
                  &metadata->memory_type_indices[index])
            : -ENOTSUP;
        metadata->allocation_sizes[index] =
            hardware_properties.allocationSize;
#else
        status = choose_swapchain_memory_type(
            &memory_properties, requirements.memoryTypeBits,
            &metadata->memory_type_indices[index]);
        metadata->allocation_sizes[index] = requirements.size;
#endif
        const bool invalid_allocation_size =
#ifdef __ANDROID__
            metadata->allocation_sizes[index] == 0U;
#else
            requirements.size == 0U;
#endif
        if (vulkan_result != VK_SUCCESS || status != 0 ||
            invalid_allocation_size) {
            failure_stage = vulkan_result != VK_SUCCESS
                ? "vkGetAndroidHardwareBufferPropertiesANDROID"
                : status != 0 ? "choose_memory_type"
                              : "memory_requirements";
            vulkan_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
            break;
        }
#ifdef __ANDROID__
        const VkImportAndroidHardwareBufferInfoANDROID import_info = {
            .sType =
                VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
            .buffer = hardware_buffer,
        };
        const VkMemoryDedicatedAllocateInfo dedicated_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .pNext = &import_info,
            .image = metadata->images[index],
        };
#else
        const VkMemoryDedicatedAllocateInfo dedicated_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .image = metadata->images[index],
        };
        const VkExportMemoryAllocateInfo export_info = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
            .pNext = &dedicated_info,
            .handleTypes = transport_handle_type,
        };
#endif
        const VkMemoryAllocateInfo allocation_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
#ifdef __ANDROID__
            .pNext = &dedicated_info,
#else
            .pNext = &export_info,
#endif
            .allocationSize = metadata->allocation_sizes[index],
            .memoryTypeIndex = metadata->memory_type_indices[index],
        };
        vulkan_result = allocate_memory(
            metadata->device, &allocation_info, NULL,
            &metadata->memories[index]);
        if (vulkan_result == VK_SUCCESS) {
            vulkan_result = bind_image(
                metadata->device, metadata->images[index],
                metadata->memories[index], 0U);
            if (vulkan_result != VK_SUCCESS)
                failure_stage = "vkBindImageMemory";
        } else {
            failure_stage = "vkAllocateMemory";
        }
#ifndef __ANDROID__
        const VkMemoryGetFdInfoKHR fd_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
            .memory = metadata->memories[index],
            .handleType = transport_handle_type,
        };
        if (vulkan_result == VK_SUCCESS) {
            vulkan_result = get_memory_fd(
                metadata->device, &fd_info, &descriptors[index]);
        }
        if (vulkan_result == VK_SUCCESS && descriptors[index] < 0) {
            vulkan_result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }
#endif
        if (vulkan_result != VK_SUCCESS) break;
        metadata->image_ids[index] = bvb_handle_id(
            BVB_OBJECT_IMAGE, context->next_image_serial++);
        status = bvb_handle_table_insert(
            &context->objects, metadata->image_ids[index], request->device_id,
            handle_bits(&metadata->images[index],
                        sizeof(metadata->images[index])));
        if (status != 0) {
            vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
            break;
        }
        struct bvb_image_metadata *image_metadata =
            image_metadata_slot(context, 0U);
        if (image_metadata == NULL) {
            (void)bvb_handle_table_remove(
                &context->objects, metadata->image_ids[index],
                BVB_OBJECT_IMAGE, NULL);
            metadata->image_ids[index] = 0U;
            status = -ENOSPC;
            vulkan_result = VK_ERROR_TOO_MANY_OBJECTS;
            break;
        }
        *image_metadata = (struct bvb_image_metadata){
            .image_id = metadata->image_ids[index],
            .device_id = request->device_id,
            .flags = image_info.flags,
            .image_type = image_info.imageType,
            .format = image_info.format,
            .mip_levels = image_info.mipLevels,
            .array_layers = image_info.arrayLayers,
        };
    }
    if (vulkan_result == VK_SUCCESS) {
#ifdef __ANDROID__
        descriptors[0] = dup(metadata->control_fd);
        if (descriptors[0] < 0) status = -errno;
#else
        descriptors[metadata->image_count] = dup(metadata->control_fd);
        if (descriptors[metadata->image_count] < 0) status = -errno;
#endif
    }
    if (vulkan_result == VK_SUCCESS && status == 0) {
        metadata->swapchain_id = bvb_handle_id(
            BVB_OBJECT_SWAPCHAIN, context->next_swapchain_serial++);
        status = bvb_handle_table_insert(
            &context->objects, metadata->swapchain_id, request->device_id,
            (uint64_t)(uintptr_t)metadata);
    }
    if (vulkan_result != VK_SUCCESS || status != 0) {
        for (size_t index = 0U;
             index < BVB_WSI_FRAME_RING_MAX_SLOTS + 1U; ++index) {
            if (descriptors[index] >= 0) {
                (void)close(descriptors[index]);
                descriptors[index] = -1;
            }
        }
        (void)destroy_swapchain_metadata(context, metadata);
        if (status != 0 && vulkan_result == VK_SUCCESS) {
            set_error(error, error_size, "swapchain ownership failed: %d",
                      status);
            return status;
        }
        set_error(error, error_size,
                  "swapchain image stage=%s index=%u vulkan=%d status=%d "
                  "image_size=%llu allocation_size=%llu image_bits=0x%x "
                  "hardware_bits=0x%x",
                  failure_stage, failure_index, (int)vulkan_result, status,
                  (unsigned long long)failure_image_size,
                  (unsigned long long)failure_allocation_size,
                  failure_image_memory_bits, failure_hardware_memory_bits);
        response->vulkan_result = vulkan_result;
        return 0;
    }
    response->vulkan_result = VK_SUCCESS;
    response->image_count = metadata->image_count;
    response->swapchain_id = metadata->swapchain_id;
    response->generation = metadata->generation;
    response->control_region_bytes = BVB_WSI_FRAME_RING_REGION_BYTES;
#ifdef __ANDROID__
    response->flags = BVB_VULKAN_SWAPCHAIN_PREPARE_FLAG_AHARDWAREBUFFER;
#endif
    for (uint32_t index = 0U; index < metadata->image_count; ++index) {
        response->images[index] = (struct bvb_vulkan_swapchain_image_record){
            .image_id = metadata->image_ids[index],
            .allocation_size = metadata->allocation_sizes[index],
            .memory_type_index = metadata->memory_type_indices[index],
        };
    }
#ifdef __ANDROID__
    for (uint32_t index = 0U; index < metadata->image_count; ++index)
        hardware_buffers[index] = metadata->hardware_buffers[index];
    *hardware_buffer_count = metadata->image_count;
    *descriptor_count = 1U;
#else
    *descriptor_count = (size_t)metadata->image_count + 1U;
#endif
    return 0;
}

static int resolve_binary_semaphore(
    struct bvb_vulkan_global_context *context, uint64_t semaphore_id,
    uint64_t device_id, VkSemaphore *semaphore) {
    if (semaphore == NULL || semaphore_id == 0U) return -EINVAL;
    struct bvb_semaphore_metadata *metadata =
        semaphore_metadata_slot(context, semaphore_id);
    if (metadata == NULL || metadata->semaphore_id != semaphore_id ||
        metadata->type != VK_SEMAPHORE_TYPE_BINARY) return -ENOTSUP;
    uint64_t parent_id = 0U;
    uint64_t native_bits = 0U;
    const int status = bvb_handle_table_lookup(
        &context->objects, semaphore_id, BVB_OBJECT_SEMAPHORE,
        &parent_id, &native_bits);
    if (status != 0 || parent_id != device_id)
        return status != 0 ? status : -EPROTO;
    *semaphore = semaphore_from_bits(native_bits);
    return 0;
}

static int resolve_fence(
    struct bvb_vulkan_global_context *context, uint64_t fence_id,
    uint64_t device_id, VkFence *fence) {
    if (fence == NULL || fence_id == 0U) return -EINVAL;
    uint64_t parent_id = 0U;
    uint64_t native_bits = 0U;
    const int status = bvb_handle_table_lookup(
        &context->objects, fence_id, BVB_OBJECT_FENCE,
        &parent_id, &native_bits);
    if (status != 0 || parent_id != device_id)
        return status != 0 ? status : -EPROTO;
    *fence = fence_from_bits(native_bits);
    return 0;
}

static VkResult ring_acquire_result(int status) {
    if (status == 0) return VK_SUCCESS;
    if (status == -EAGAIN) return VK_NOT_READY;
    if (status == -ETIMEDOUT) return VK_TIMEOUT;
    return VK_ERROR_SURFACE_LOST_KHR;
}

static int acquire_ring_slot(struct bvb_wsi_frame_ring *ring,
                             uint64_t timeout_ns, uint32_t *slot) {
    if (timeout_ns == 0U)
        return bvb_wsi_frame_ring_acquire(ring, 0U, slot);
    if (timeout_ns == UINT64_MAX) {
        int status = 0;
        do {
            status = bvb_wsi_frame_ring_acquire(ring, UINT32_MAX, slot);
        } while (status == -ETIMEDOUT);
        return status;
    }
    uint64_t timeout_ms = timeout_ns / UINT64_C(1000000);
    if (timeout_ns % UINT64_C(1000000) != 0U) ++timeout_ms;
    if (timeout_ms == 0U) timeout_ms = 1U;
    if (timeout_ms > UINT32_MAX) timeout_ms = UINT32_MAX;
    return bvb_wsi_frame_ring_acquire(ring, (uint32_t)timeout_ms, slot);
}

static VkResult record_swapchain_barrier(
    struct bvb_vulkan_global_context *context,
    struct bvb_swapchain_metadata *metadata, uint32_t image_index,
    bool acquire_from_external) {
    PFN_vkResetCommandBuffer reset =
        (PFN_vkResetCommandBuffer)context->get_device_proc_addr(
            metadata->device, "vkResetCommandBuffer");
    PFN_vkBeginCommandBuffer begin =
        (PFN_vkBeginCommandBuffer)context->get_device_proc_addr(
            metadata->device, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer end =
        (PFN_vkEndCommandBuffer)context->get_device_proc_addr(
            metadata->device, "vkEndCommandBuffer");
    PFN_vkCmdPipelineBarrier barrier =
        (PFN_vkCmdPipelineBarrier)context->get_device_proc_addr(
            metadata->device, "vkCmdPipelineBarrier");
    if (reset == NULL || begin == NULL || end == NULL || barrier == NULL)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkCommandBuffer command = acquire_from_external
        ? metadata->acquire_commands[image_index]
        : metadata->present_commands[image_index];
    VkResult result = reset(command, 0U);
    const VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (result == VK_SUCCESS) result = begin(command, &begin_info);
    const bool needs_barrier =
        !acquire_from_external || metadata->presented_once[image_index];
    if (result == VK_SUCCESS && needs_barrier) {
        const VkImageMemoryBarrier image_barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = acquire_from_external
                ? VK_ACCESS_MEMORY_READ_BIT : VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = acquire_from_external
                ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : 0U,
            .oldLayout = acquire_from_external
                ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .newLayout = acquire_from_external
                ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = acquire_from_external
                ? VK_QUEUE_FAMILY_EXTERNAL : metadata->queue_family_index,
            .dstQueueFamilyIndex = acquire_from_external
                ? metadata->queue_family_index : VK_QUEUE_FAMILY_EXTERNAL,
            .image = metadata->images[image_index],
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0U,
                .levelCount = 1U,
                .baseArrayLayer = 0U,
                .layerCount = 1U,
            },
        };
        barrier(command,
                acquire_from_external ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                      : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                acquire_from_external ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
                                      : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                0U, 0U, NULL, 0U, NULL, 1U, &image_barrier);
    }
    if (result == VK_SUCCESS) result = end(command);
    return result;
}

int bvb_vulkan_global_context_acquire_swapchain_image(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_swapchain_acquire_request *request,
    struct bvb_vulkan_swapchain_acquire_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || response == NULL) return -EINVAL;
    *response = (struct bvb_vulkan_swapchain_acquire_response){
        .vulkan_result = VK_ERROR_INITIALIZATION_FAILED,
    };
    struct bvb_swapchain_metadata *metadata =
        swapchain_metadata_slot(context, request->swapchain_id);
    if (metadata == NULL || metadata->swapchain_id != request->swapchain_id ||
        metadata->device_id != request->device_id) return -ENOENT;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    int status = request->semaphore_id == 0U ? 0 :
        resolve_binary_semaphore(context, request->semaphore_id,
                                 request->device_id, &semaphore);
    if (status == 0 && request->fence_id != 0U)
        status = resolve_fence(context, request->fence_id,
                               request->device_id, &fence);
    if (status != 0) {
        response->vulkan_result = status == -ENOTSUP
            ? VK_ERROR_FEATURE_NOT_PRESENT : VK_ERROR_INITIALIZATION_FAILED;
        return 0;
    }
    uint32_t image_index = 0U;
    status = acquire_ring_slot(metadata->control, request->timeout_ns,
                               &image_index);
    response->vulkan_result = ring_acquire_result(status);
    if (status != 0) return 0;
    VkResult result = record_swapchain_barrier(
        context, metadata, image_index, true);
    PFN_vkQueueSubmit submit =
        (PFN_vkQueueSubmit)context->get_device_proc_addr(
            metadata->device, "vkQueueSubmit");
    if (result == VK_SUCCESS && submit == NULL)
        result = VK_ERROR_FEATURE_NOT_PRESENT;
    const VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1U,
        .pCommandBuffers = &metadata->acquire_commands[image_index],
        .signalSemaphoreCount = semaphore == VK_NULL_HANDLE ? 0U : 1U,
        .pSignalSemaphores = semaphore == VK_NULL_HANDLE ? NULL : &semaphore,
    };
    if (result == VK_SUCCESS)
        result = submit(metadata->producer_queue, 1U, &submit_info, fence);
    if (result != VK_SUCCESS) {
        (void)bvb_wsi_frame_ring_fail_producer(metadata->control, -EIO);
        set_error(error, error_size, "swapchain acquire submit failed: %d",
                  (int)result);
        response->vulkan_result = result;
        return 0;
    }
    response->vulkan_result = VK_SUCCESS;
    response->image_index = image_index;
    return 0;
}

int bvb_vulkan_global_context_present_swapchain_image(
    struct bvb_vulkan_global_context *context,
    const struct bvb_vulkan_swapchain_present_request *request,
    struct bvb_vulkan_swapchain_present_response *response,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || request == NULL || response == NULL) return -EINVAL;
    *response = (struct bvb_vulkan_swapchain_present_response){
        .vulkan_result = VK_ERROR_INITIALIZATION_FAILED,
    };
    struct bvb_swapchain_metadata *metadata =
        swapchain_metadata_slot(context, request->swapchain_id);
    if (metadata == NULL || metadata->swapchain_id != request->swapchain_id ||
        request->image_index >= metadata->image_count || request->flags != 0U)
        return -ENOENT;
    VkDevice queue_device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    int status = resolve_queue(context, request->queue_id, &queue_device,
                               &queue);
    uint64_t queue_device_id = 0U;
    uint64_t queue_bits = 0U;
    if (status == 0)
        status = bvb_handle_table_lookup(
            &context->objects, request->queue_id, BVB_OBJECT_QUEUE,
            &queue_device_id, &queue_bits);
    if (status != 0 || queue_device_id != metadata->device_id ||
        queue_device != metadata->device || queue != metadata->producer_queue) {
        response->vulkan_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return 0;
    }
    VkSemaphore waits[BVB_VULKAN_MAX_PRESENT_WAIT_SEMAPHORES];
    VkPipelineStageFlags wait_stages[
        BVB_VULKAN_MAX_PRESENT_WAIT_SEMAPHORES];
    for (uint32_t index = 0U; index < request->wait_semaphore_count; ++index) {
        status = resolve_binary_semaphore(
            context, request->wait_semaphore_ids[index],
            metadata->device_id, &waits[index]);
        if (status != 0) {
            response->vulkan_result = status == -ENOTSUP
                ? VK_ERROR_FEATURE_NOT_PRESENT
                : VK_ERROR_INITIALIZATION_FAILED;
            return 0;
        }
        wait_stages[index] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
    VkResult result = record_swapchain_barrier(
        context, metadata, request->image_index, false);
    PFN_vkResetFences reset =
        (PFN_vkResetFences)context->get_device_proc_addr(
            metadata->device, "vkResetFences");
    PFN_vkQueueSubmit submit =
        (PFN_vkQueueSubmit)context->get_device_proc_addr(
            metadata->device, "vkQueueSubmit");
    PFN_vkWaitForFences wait =
        (PFN_vkWaitForFences)context->get_device_proc_addr(
            metadata->device, "vkWaitForFences");
    if (reset == NULL || submit == NULL || wait == NULL)
        result = VK_ERROR_FEATURE_NOT_PRESENT;
    if (result == VK_SUCCESS)
        result = reset(metadata->device, 1U,
                       &metadata->present_completion);
    const VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = request->wait_semaphore_count,
        .pWaitSemaphores = request->wait_semaphore_count == 0U
            ? NULL : waits,
        .pWaitDstStageMask = request->wait_semaphore_count == 0U
            ? NULL : wait_stages,
        .commandBufferCount = 1U,
        .pCommandBuffers = &metadata->present_commands[request->image_index],
    };
    if (result == VK_SUCCESS)
        result = submit(queue, 1U, &submit_info,
                        metadata->present_completion);
    if (result == VK_SUCCESS)
        result = wait(metadata->device, 1U, &metadata->present_completion,
                      VK_TRUE, UINT64_MAX);
    uint32_t sequence = 0U;
    if (result == VK_SUCCESS) {
        status = bvb_wsi_frame_ring_present(
            metadata->control, request->image_index, &sequence);
        if (status != 0) result = VK_ERROR_SURFACE_LOST_KHR;
    }
    if (result != VK_SUCCESS) {
        (void)bvb_wsi_frame_ring_fail_producer(metadata->control, -EIO);
        set_error(error, error_size, "swapchain present failed: %d",
                  (int)result);
        response->vulkan_result = result;
        return 0;
    }
    metadata->presented_once[request->image_index] = true;
    response->vulkan_result = VK_SUCCESS;
    response->sequence = sequence;
    return 0;
}

int bvb_vulkan_global_context_destroy_swapchain(
    struct bvb_vulkan_global_context *context, uint64_t swapchain_id,
    char *error, size_t error_size) {
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL ||
        bvb_handle_expect(swapchain_id, BVB_OBJECT_SWAPCHAIN) != 0) {
        return -EINVAL;
    }
    struct bvb_swapchain_metadata *metadata =
        swapchain_metadata_slot(context, swapchain_id);
    if (metadata == NULL || metadata->swapchain_id != swapchain_id) {
        return -ENOENT;
    }
    const int status = destroy_swapchain_metadata(context, metadata);
    if (status != 0) {
        set_error(error, error_size, "swapchain teardown failed: %d", status);
    }
    return status;
}
