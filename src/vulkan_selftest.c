#define _POSIX_C_SOURCE 200809L
#define VK_NO_PROTOTYPES

#include <bvb/command_batch.h>
#include <bvb/vulkan_selftest.h>

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
#include <unistd.h>

enum {
    BVB_SELFTEST_BUFFER_BYTES = 4096,
};

static const uint32_t BVB_SELFTEST_FILL_WORD = UINT32_C(0xa5c3f00d);

struct bvb_vulkan_objects {
    void *loader;
    VkInstance instance;
    VkDevice device;
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkCommandPool command_pool;
    void *mapped_memory;
    PFN_vkDestroyInstance destroy_instance;
    PFN_vkDestroyDevice destroy_device;
    PFN_vkDestroyBuffer destroy_buffer;
    PFN_vkFreeMemory free_memory;
    PFN_vkDestroyCommandPool destroy_command_pool;
    PFN_vkUnmapMemory unmap_memory;
};

struct bvb_vulkan_batch_context {
    struct bvb_vulkan_objects objects;
    struct bvb_vulkan_selftest_result base_result;
    VkPhysicalDevice physical_device;
    VkPhysicalDeviceMemoryProperties memory_properties;
    VkQueue queue;
    VkCommandBuffer command_buffer;
    struct bvb_handle_entry handle_entries[8];
    struct bvb_handle_table handles;
    PFN_vkResetCommandPool reset_command_pool;
    PFN_vkBeginCommandBuffer begin_command_buffer;
    PFN_vkEndCommandBuffer end_command_buffer;
    PFN_vkCmdFillBuffer fill_buffer;
    PFN_vkCmdPipelineBarrier pipeline_barrier;
    PFN_vkQueueSubmit queue_submit;
    PFN_vkQueueWaitIdle queue_wait_idle;
    PFN_vkInvalidateMappedMemoryRanges invalidate_mapped_memory_ranges;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    PFN_vkCreateDevice create_device;
    PFN_vkGetDeviceProcAddr get_device_proc_addr;
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

static PFN_vkVoidFunction symbol_from_loader(void *loader, const char *name) {
    void *raw_symbol = dlsym(loader, name);
    PFN_vkVoidFunction function = NULL;
    if (raw_symbol != NULL) {
        memcpy(&function, &raw_symbol, sizeof(function));
    }
    return function;
}

static uint64_t instance_extension_flag(const char *name) {
    if (strcmp(name, "VK_KHR_surface") == 0) {
        return BVB_INSTANCE_KHR_SURFACE;
    }
    if (strcmp(name, "VK_KHR_android_surface") == 0) {
        return BVB_INSTANCE_KHR_ANDROID_SURFACE;
    }
    if (strcmp(name, "VK_EXT_headless_surface") == 0) {
        return BVB_INSTANCE_EXT_HEADLESS_SURFACE;
    }
    if (strcmp(name, "VK_KHR_get_physical_device_properties2") == 0) {
        return BVB_INSTANCE_KHR_GET_PROPERTIES_2;
    }
    if (strcmp(name, "VK_KHR_external_memory_capabilities") == 0) {
        return BVB_INSTANCE_KHR_EXTERNAL_MEMORY_CAPS;
    }
    if (strcmp(name, "VK_KHR_external_semaphore_capabilities") == 0) {
        return BVB_INSTANCE_KHR_EXTERNAL_SEMAPHORE_CAPS;
    }
    return 0;
}

static uint64_t device_extension_flag(const char *name) {
    if (strcmp(name, "VK_KHR_swapchain") == 0) {
        return BVB_DEVICE_KHR_SWAPCHAIN;
    }
    if (strcmp(name, "VK_KHR_external_memory") == 0) {
        return BVB_DEVICE_KHR_EXTERNAL_MEMORY;
    }
    if (strcmp(name, "VK_KHR_external_memory_fd") == 0) {
        return BVB_DEVICE_KHR_EXTERNAL_MEMORY_FD;
    }
    if (strcmp(name,
               "VK_ANDROID_external_memory_android_hardware_buffer") == 0) {
        return BVB_DEVICE_ANDROID_HARDWARE_BUFFER;
    }
    if (strcmp(name, "VK_KHR_external_semaphore") == 0) {
        return BVB_DEVICE_KHR_EXTERNAL_SEMAPHORE;
    }
    if (strcmp(name, "VK_KHR_external_semaphore_fd") == 0) {
        return BVB_DEVICE_KHR_EXTERNAL_SEMAPHORE_FD;
    }
    if (strcmp(name, "VK_KHR_timeline_semaphore") == 0) {
        return BVB_DEVICE_KHR_TIMELINE_SEMAPHORE;
    }
    if (strcmp(name, "VK_KHR_external_fence_fd") == 0) {
        return BVB_DEVICE_KHR_EXTERNAL_FENCE_FD;
    }
    if (strcmp(name, "VK_KHR_dynamic_rendering") == 0) {
        return BVB_DEVICE_KHR_DYNAMIC_RENDERING;
    }
    return 0;
}

static int monotonic_ns(uint64_t *output) {
    struct timespec timestamp;
    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
        return -errno;
    }
    *output = (uint64_t)timestamp.tv_sec * UINT64_C(1000000000) +
              (uint64_t)timestamp.tv_nsec;
    return 0;
}

static void cleanup(struct bvb_vulkan_objects *objects) {
    if (objects->device != VK_NULL_HANDLE) {
        if (objects->mapped_memory != NULL && objects->memory != VK_NULL_HANDLE &&
            objects->unmap_memory != NULL) {
            objects->unmap_memory(objects->device, objects->memory);
        }
        if (objects->command_pool != VK_NULL_HANDLE &&
            objects->destroy_command_pool != NULL) {
            objects->destroy_command_pool(objects->device,
                                          objects->command_pool, NULL);
        }
        if (objects->buffer != VK_NULL_HANDLE &&
            objects->destroy_buffer != NULL) {
            objects->destroy_buffer(objects->device, objects->buffer, NULL);
        }
        if (objects->memory != VK_NULL_HANDLE && objects->free_memory != NULL) {
            objects->free_memory(objects->device, objects->memory, NULL);
        }
        if (objects->destroy_device != NULL) {
            objects->destroy_device(objects->device, NULL);
        }
    }
    if (objects->instance != VK_NULL_HANDLE &&
        objects->destroy_instance != NULL) {
        objects->destroy_instance(objects->instance, NULL);
    }
    if (objects->loader != NULL) {
        (void)dlclose(objects->loader);
    }
}

struct bvb_external_resources {
    VkDevice device;
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *mapped;
    bool owns_device;
    PFN_vkDestroyDevice destroy_device;
    PFN_vkDestroyBuffer destroy_buffer;
    PFN_vkFreeMemory free_memory;
    PFN_vkUnmapMemory unmap_memory;
};

static void cleanup_external_resources(
    struct bvb_external_resources *resources) {
    if (resources == NULL || resources->device == VK_NULL_HANDLE) return;
    if (resources->mapped != NULL && resources->memory != VK_NULL_HANDLE &&
        resources->unmap_memory != NULL) {
        resources->unmap_memory(resources->device, resources->memory);
    }
    if (resources->buffer != VK_NULL_HANDLE &&
        resources->destroy_buffer != NULL) {
        resources->destroy_buffer(resources->device, resources->buffer, NULL);
    }
    if (resources->memory != VK_NULL_HANDLE && resources->free_memory != NULL) {
        resources->free_memory(resources->device, resources->memory, NULL);
    }
    if (resources->owns_device && resources->destroy_device != NULL) {
        resources->destroy_device(resources->device, NULL);
    }
    memset(resources, 0, sizeof(*resources));
}

static uint64_t vulkan_handle_bits(const void *handle, size_t handle_size) {
    uint64_t bits = 0U;
    if (handle != NULL && handle_size <= sizeof(bits)) {
        memcpy(&bits, handle, handle_size);
    }
    return bits;
}

static VkBuffer buffer_from_bits(uint64_t bits) {
    VkBuffer buffer = VK_NULL_HANDLE;
    _Static_assert(sizeof(buffer) <= sizeof(bits), "VkBuffer exceeds wire bits");
    memcpy(&buffer, &bits, sizeof(buffer));
    return buffer;
}

static int replay_transfer_batch(
    const uint8_t *batch, size_t batch_length,
    struct bvb_vulkan_batch_context *context, char *error, size_t error_size) {
    struct bvb_command_batch_info info;
    int result = bvb_command_batch_validate(batch, batch_length, &info);
    if (result != 0) {
        set_error(error, error_size, "command batch validation failed: %d",
                  result);
        return result;
    }

    const uint64_t buffer_id = bvb_handle_id(BVB_OBJECT_BUFFER, 1U);

    uint64_t command_buffer_bits = 0U;
    result = bvb_handle_table_lookup(&context->handles, info.command_buffer_id,
                                     BVB_OBJECT_COMMAND_BUFFER, NULL,
                                     &command_buffer_bits);
    if (result != 0 ||
        command_buffer_bits !=
            vulkan_handle_bits(&context->command_buffer,
                               sizeof(context->command_buffer))) {
        set_error(error, error_size, "command-buffer ownership mismatch");
        return -EPROTO;
    }

    struct bvb_command_batch_iterator iterator;
    result = bvb_command_batch_iterator_init(&iterator, batch, batch_length);
    if (result != 0) {
        set_error(error, error_size, "could not iterate command batch: %d",
                  result);
        return result;
    }
    bool fill_seen = false;
    bool barrier_seen = false;
    struct bvb_command_record record;
    while ((result = bvb_command_batch_next(&iterator, &record)) == 0) {
        if (record.opcode == BVB_COMMAND_FILL_BUFFER && !fill_seen &&
            !barrier_seen) {
            struct bvb_fill_buffer_command command;
            result = bvb_command_decode_fill_buffer(&record, &command);
            if (result != 0 || command.buffer_id != buffer_id ||
                command.offset > BVB_SELFTEST_BUFFER_BYTES ||
                command.size > BVB_SELFTEST_BUFFER_BYTES - command.offset) {
                set_error(error, error_size, "invalid fill-buffer command");
                return -EPROTO;
            }
            uint64_t native_bits = 0U;
            result = bvb_handle_table_lookup(&context->handles,
                                             command.buffer_id,
                                             BVB_OBJECT_BUFFER, NULL,
                                             &native_bits);
            if (result != 0) {
                set_error(error, error_size, "unknown fill-buffer handle");
                return result;
            }
            context->fill_buffer(context->command_buffer,
                                 buffer_from_bits(native_bits), command.offset,
                                 command.size, command.data);
            fill_seen = true;
        } else if (record.opcode == BVB_COMMAND_BUFFER_HOST_READ_BARRIER &&
                   fill_seen && !barrier_seen) {
            struct bvb_buffer_host_read_barrier_command command;
            result = bvb_command_decode_buffer_host_read_barrier(&record,
                                                                  &command);
            if (result != 0 || command.buffer_id != buffer_id ||
                command.offset > BVB_SELFTEST_BUFFER_BYTES ||
                command.size > BVB_SELFTEST_BUFFER_BYTES - command.offset) {
                set_error(error, error_size,
                          "invalid buffer host-read barrier command");
                return -EPROTO;
            }
            uint64_t native_bits = 0U;
            result = bvb_handle_table_lookup(&context->handles,
                                             command.buffer_id,
                                             BVB_OBJECT_BUFFER, NULL,
                                             &native_bits);
            if (result != 0) {
                set_error(error, error_size, "unknown barrier buffer handle");
                return result;
            }
            const VkBufferMemoryBarrier barrier = {
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = buffer_from_bits(native_bits),
                .offset = command.offset,
                .size = command.size,
            };
            context->pipeline_barrier(
                context->command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &barrier, 0, NULL);
            barrier_seen = true;
        } else {
            set_error(error, error_size,
                      "unsupported or out-of-order command opcode: %u",
                      (unsigned int)record.opcode);
            return -EPROTO;
        }
    }
    if (result != 1 || !fill_seen || !barrier_seen) {
        set_error(error, error_size, "incomplete transfer command batch");
        return -EPROTO;
    }
    return 0;
}

int bvb_vulkan_batch_context_create(
    const char *loader_path, struct bvb_vulkan_batch_context **context_output,
    char *error, size_t error_size) {
    struct bvb_vulkan_objects objects;
    struct bvb_vulkan_selftest_result initialized_result;
    struct bvb_vulkan_selftest_result *output = &initialized_result;
    memset(&objects, 0, sizeof(objects));
    if (error != NULL && error_size > 0U) {
        error[0] = '\0';
    }
    if (loader_path == NULL || loader_path[0] != '/' ||
        context_output == NULL) {
        set_error(error, error_size, "loader path must be absolute");
        return -EINVAL;
    }
    *context_output = NULL;
    memset(output, 0, sizeof(*output));
    output->buffer_bytes = BVB_SELFTEST_BUFFER_BYTES;
    output->fill_word = BVB_SELFTEST_FILL_WORD;

    int status = 0;
    objects.loader = dlopen(loader_path, RTLD_NOW | RTLD_LOCAL);
    if (objects.loader == NULL) {
        set_error(error, error_size, "could not load %s: %s", loader_path,
                  dlerror());
        return -ENOENT;
    }

    PFN_vkGetInstanceProcAddr gipa =
        (PFN_vkGetInstanceProcAddr)symbol_from_loader(objects.loader,
                                                      "vkGetInstanceProcAddr");
    if (gipa == NULL) {
        set_error(error, error_size, "loader has no vkGetInstanceProcAddr");
        status = -ENOSYS;
        goto done;
    }
    PFN_vkEnumerateInstanceExtensionProperties enumerate_instance_extensions =
        (PFN_vkEnumerateInstanceExtensionProperties)gipa(
            VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    PFN_vkCreateInstance create_instance =
        (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (enumerate_instance_extensions == NULL || create_instance == NULL) {
        set_error(error, error_size, "loader is missing global entry points");
        status = -ENOSYS;
        goto done;
    }

    VkResult vk_result = enumerate_instance_extensions(
        NULL, &output->instance_extension_count, NULL);
    if (vk_result != VK_SUCCESS || output->instance_extension_count > 256U) {
        set_error(error, error_size,
                  "instance extension enumeration failed: %d", (int)vk_result);
        status = -EIO;
        goto done;
    }
    VkExtensionProperties *instance_extensions = calloc(
        output->instance_extension_count, sizeof(*instance_extensions));
    if (instance_extensions == NULL && output->instance_extension_count > 0U) {
        set_error(error, error_size,
                  "could not allocate instance extension list");
        status = -ENOMEM;
        goto done;
    }
    uint32_t instance_extension_count = output->instance_extension_count;
    vk_result = enumerate_instance_extensions(NULL, &instance_extension_count,
                                              instance_extensions);
    if (vk_result != VK_SUCCESS) {
        free(instance_extensions);
        set_error(error, error_size,
                  "instance extension list retrieval failed: %d",
                  (int)vk_result);
        status = -EIO;
        goto done;
    }
    output->instance_extension_count = instance_extension_count;
    for (uint32_t index = 0; index < instance_extension_count; ++index) {
        output->instance_extension_flags |=
            instance_extension_flag(instance_extensions[index].extensionName);
    }
    free(instance_extensions);

    const VkApplicationInfo application_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "bvb-vulkan-selftest",
        .applicationVersion = VK_MAKE_API_VERSION(0, 0, 4, 0),
        .pEngineName = "none",
        .apiVersion = VK_API_VERSION_1_0,
    };
    static const char *const external_instance_extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
    };
    const bool enable_external_memory_capabilities =
        (output->instance_extension_flags &
         BVB_INSTANCE_KHR_EXTERNAL_MEMORY_CAPS) != 0U;
    const VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application_info,
        .enabledExtensionCount = enable_external_memory_capabilities ? 1U : 0U,
        .ppEnabledExtensionNames = enable_external_memory_capabilities
                                       ? external_instance_extensions
                                       : NULL,
    };
    vk_result = create_instance(&instance_info, NULL, &objects.instance);
    if (vk_result != VK_SUCCESS) {
        set_error(error, error_size, "vkCreateInstance failed: %d",
                  (int)vk_result);
        status = -EIO;
        goto done;
    }

#define LOAD_INSTANCE(name)                                                     \
    PFN_##name name = (PFN_##name)gipa(objects.instance, #name)
    LOAD_INSTANCE(vkDestroyInstance);
    LOAD_INSTANCE(vkEnumeratePhysicalDevices);
    LOAD_INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD_INSTANCE(vkGetPhysicalDeviceMemoryProperties);
    LOAD_INSTANCE(vkEnumerateDeviceExtensionProperties);
    LOAD_INSTANCE(vkCreateDevice);
    LOAD_INSTANCE(vkGetDeviceProcAddr);
#undef LOAD_INSTANCE
    objects.destroy_instance = vkDestroyInstance;
    if (vkDestroyInstance == NULL || vkEnumeratePhysicalDevices == NULL ||
        vkGetPhysicalDeviceQueueFamilyProperties == NULL ||
        vkGetPhysicalDeviceMemoryProperties == NULL ||
        vkEnumerateDeviceExtensionProperties == NULL || vkCreateDevice == NULL ||
        vkGetDeviceProcAddr == NULL) {
        set_error(error, error_size, "loader is missing instance entry points");
        status = -ENOSYS;
        goto done;
    }

    uint32_t physical_device_count = 0;
    vk_result = vkEnumeratePhysicalDevices(objects.instance,
                                           &physical_device_count, NULL);
    if (vk_result != VK_SUCCESS || physical_device_count == 0U) {
        set_error(error, error_size, "no physical Vulkan device: %d",
                  (int)vk_result);
        status = -ENODEV;
        goto done;
    }
    VkPhysicalDevice *physical_devices =
        calloc(physical_device_count, sizeof(*physical_devices));
    if (physical_devices == NULL) {
        set_error(error, error_size, "could not allocate device list");
        status = -ENOMEM;
        goto done;
    }
    vk_result = vkEnumeratePhysicalDevices(objects.instance,
                                           &physical_device_count,
                                           physical_devices);
    if (vk_result != VK_SUCCESS && vk_result != VK_INCOMPLETE) {
        free(physical_devices);
        set_error(error, error_size, "device list retrieval failed: %d",
                  (int)vk_result);
        status = -EIO;
        goto done;
    }
    VkPhysicalDevice physical_device = physical_devices[0];
    free(physical_devices);

    vk_result = vkEnumerateDeviceExtensionProperties(
        physical_device, NULL, &output->device_extension_count, NULL);
    if (vk_result != VK_SUCCESS || output->device_extension_count > 1024U) {
        set_error(error, error_size, "device extension enumeration failed: %d",
                  (int)vk_result);
        status = -EIO;
        goto done;
    }
    VkExtensionProperties *device_extensions = calloc(
        output->device_extension_count, sizeof(*device_extensions));
    if (device_extensions == NULL && output->device_extension_count > 0U) {
        set_error(error, error_size, "could not allocate device extension list");
        status = -ENOMEM;
        goto done;
    }
    uint32_t device_extension_count = output->device_extension_count;
    vk_result = vkEnumerateDeviceExtensionProperties(
        physical_device, NULL, &device_extension_count, device_extensions);
    if (vk_result != VK_SUCCESS) {
        free(device_extensions);
        set_error(error, error_size,
                  "device extension list retrieval failed: %d",
                  (int)vk_result);
        status = -EIO;
        goto done;
    }
    output->device_extension_count = device_extension_count;
    for (uint32_t index = 0; index < device_extension_count; ++index) {
        output->device_extension_flags |=
            device_extension_flag(device_extensions[index].extensionName);
    }
    free(device_extensions);

    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device,
                                             &queue_family_count, NULL);
    if (queue_family_count == 0U || queue_family_count > 256U) {
        set_error(error, error_size, "no usable queue family");
        status = -ENODEV;
        goto done;
    }
    VkQueueFamilyProperties *queue_families =
        calloc(queue_family_count, sizeof(*queue_families));
    if (queue_families == NULL) {
        set_error(error, error_size, "could not allocate queue-family list");
        status = -ENOMEM;
        goto done;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device,
                                             &queue_family_count,
                                             queue_families);
    bool queue_found = false;
    const VkQueueFlags transfer_capable = VK_QUEUE_GRAPHICS_BIT |
                                           VK_QUEUE_COMPUTE_BIT |
                                           VK_QUEUE_TRANSFER_BIT;
    for (uint32_t index = 0; index < queue_family_count; ++index) {
        if (queue_families[index].queueCount > 0U &&
            (queue_families[index].queueFlags & transfer_capable) != 0U) {
            output->queue_family_index = index;
            output->queue_flags = queue_families[index].queueFlags;
            queue_found = true;
            break;
        }
    }
    free(queue_families);
    if (!queue_found) {
        set_error(error, error_size, "no transfer-compatible queue family");
        status = -ENOTSUP;
        goto done;
    }

    const float queue_priority = 1.0F;
    const VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = output->queue_family_index,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };
    static const char *const external_device_extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    };
    const bool enable_external_memory_fd =
        (output->device_extension_flags &
         (BVB_DEVICE_KHR_EXTERNAL_MEMORY |
          BVB_DEVICE_KHR_EXTERNAL_MEMORY_FD)) ==
        (BVB_DEVICE_KHR_EXTERNAL_MEMORY |
         BVB_DEVICE_KHR_EXTERNAL_MEMORY_FD);
    const VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = enable_external_memory_fd ? 2U : 0U,
        .ppEnabledExtensionNames = enable_external_memory_fd
                                       ? external_device_extensions
                                       : NULL,
    };
    vk_result = vkCreateDevice(physical_device, &device_info, NULL,
                               &objects.device);
    if (vk_result != VK_SUCCESS) {
        set_error(error, error_size, "vkCreateDevice failed: %d",
                  (int)vk_result);
        status = -EIO;
        goto done;
    }

#define LOAD_DEVICE(name)                                                       \
    PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(objects.device, #name)
    LOAD_DEVICE(vkDestroyDevice);
    LOAD_DEVICE(vkGetDeviceQueue);
    LOAD_DEVICE(vkCreateBuffer);
    LOAD_DEVICE(vkDestroyBuffer);
    LOAD_DEVICE(vkGetBufferMemoryRequirements);
    LOAD_DEVICE(vkAllocateMemory);
    LOAD_DEVICE(vkFreeMemory);
    LOAD_DEVICE(vkBindBufferMemory);
    LOAD_DEVICE(vkCreateCommandPool);
    LOAD_DEVICE(vkDestroyCommandPool);
    LOAD_DEVICE(vkResetCommandPool);
    LOAD_DEVICE(vkAllocateCommandBuffers);
    LOAD_DEVICE(vkBeginCommandBuffer);
    LOAD_DEVICE(vkEndCommandBuffer);
    LOAD_DEVICE(vkCmdFillBuffer);
    LOAD_DEVICE(vkCmdPipelineBarrier);
    LOAD_DEVICE(vkQueueSubmit);
    LOAD_DEVICE(vkQueueWaitIdle);
    LOAD_DEVICE(vkMapMemory);
    LOAD_DEVICE(vkUnmapMemory);
    LOAD_DEVICE(vkInvalidateMappedMemoryRanges);
#undef LOAD_DEVICE
    objects.destroy_device = vkDestroyDevice;
    objects.destroy_buffer = vkDestroyBuffer;
    objects.free_memory = vkFreeMemory;
    objects.destroy_command_pool = vkDestroyCommandPool;
    objects.unmap_memory = vkUnmapMemory;
    if (vkDestroyDevice == NULL || vkGetDeviceQueue == NULL ||
        vkCreateBuffer == NULL || vkDestroyBuffer == NULL ||
        vkGetBufferMemoryRequirements == NULL || vkAllocateMemory == NULL ||
        vkFreeMemory == NULL || vkBindBufferMemory == NULL ||
        vkCreateCommandPool == NULL || vkDestroyCommandPool == NULL ||
        vkResetCommandPool == NULL || vkAllocateCommandBuffers == NULL ||
        vkBeginCommandBuffer == NULL ||
        vkEndCommandBuffer == NULL || vkCmdFillBuffer == NULL ||
        vkCmdPipelineBarrier == NULL || vkQueueSubmit == NULL ||
        vkQueueWaitIdle == NULL || vkMapMemory == NULL ||
        vkUnmapMemory == NULL || vkInvalidateMappedMemoryRanges == NULL) {
        set_error(error, error_size, "driver is missing device entry points");
        status = -ENOSYS;
        goto done;
    }

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(objects.device, output->queue_family_index, 0, &queue);
    if (queue == VK_NULL_HANDLE) {
        set_error(error, error_size, "vkGetDeviceQueue returned null");
        status = -EIO;
        goto done;
    }
    const VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = BVB_SELFTEST_BUFFER_BYTES,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    vk_result = vkCreateBuffer(objects.device, &buffer_info, NULL,
                               &objects.buffer);
    if (vk_result != VK_SUCCESS) {
        set_error(error, error_size, "vkCreateBuffer failed: %d",
                  (int)vk_result);
        status = -EIO;
        goto done;
    }

    VkMemoryRequirements memory_requirements;
    vkGetBufferMemoryRequirements(objects.device, objects.buffer,
                                  &memory_requirements);
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
    bool memory_found = false;
    bool coherent_memory_found = false;
    for (uint32_t index = 0; index < memory_properties.memoryTypeCount;
         ++index) {
        VkMemoryPropertyFlags flags =
            memory_properties.memoryTypes[index].propertyFlags;
        if ((memory_requirements.memoryTypeBits & (UINT32_C(1) << index)) == 0U ||
            (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0U) {
            continue;
        }
        bool coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0U;
        if (!memory_found || (coherent && !coherent_memory_found)) {
            output->memory_type_index = index;
            output->memory_property_flags = flags;
            memory_found = true;
            coherent_memory_found = coherent;
        }
    }
    if (!memory_found) {
        set_error(error, error_size, "no host-visible buffer memory type");
        status = -ENOTSUP;
        goto done;
    }
    const VkMemoryAllocateInfo allocation_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memory_requirements.size,
        .memoryTypeIndex = output->memory_type_index,
    };
    vk_result = vkAllocateMemory(objects.device, &allocation_info, NULL,
                                 &objects.memory);
    if (vk_result != VK_SUCCESS) {
        set_error(error, error_size, "vkAllocateMemory failed: %d",
                  (int)vk_result);
        status = -ENOMEM;
        goto done;
    }
    vk_result = vkBindBufferMemory(objects.device, objects.buffer,
                                   objects.memory, 0);
    if (vk_result != VK_SUCCESS) {
        set_error(error, error_size, "vkBindBufferMemory failed: %d",
                  (int)vk_result);
        status = -EIO;
        goto done;
    }

    const VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = output->queue_family_index,
    };
    vk_result = vkCreateCommandPool(objects.device, &pool_info, NULL,
                                    &objects.command_pool);
    if (vk_result != VK_SUCCESS) {
        set_error(error, error_size, "vkCreateCommandPool failed: %d",
                  (int)vk_result);
        status = -EIO;
        goto done;
    }
    const VkCommandBufferAllocateInfo command_buffer_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = objects.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    vk_result = vkAllocateCommandBuffers(objects.device, &command_buffer_info,
                                         &command_buffer);
    if (vk_result != VK_SUCCESS) {
        set_error(error, error_size, "vkAllocateCommandBuffers failed: %d",
                  (int)vk_result);
        status = -EIO;
        goto done;
    }

    vk_result = vkMapMemory(objects.device, objects.memory, 0,
                            BVB_SELFTEST_BUFFER_BYTES, 0,
                            &objects.mapped_memory);
    if (vk_result != VK_SUCCESS || objects.mapped_memory == NULL) {
        set_error(error, error_size, "vkMapMemory failed: %d", (int)vk_result);
        status = -EIO;
        goto done;
    }

    struct bvb_vulkan_batch_context *context = calloc(1, sizeof(*context));
    if (context == NULL) {
        set_error(error, error_size, "could not allocate Vulkan context");
        status = -ENOMEM;
        goto done;
    }
    context->objects = objects;
    memset(&objects, 0, sizeof(objects));
    context->base_result = *output;
    context->physical_device = physical_device;
    context->memory_properties = memory_properties;
    context->queue = queue;
    context->command_buffer = command_buffer;
    context->reset_command_pool = vkResetCommandPool;
    context->begin_command_buffer = vkBeginCommandBuffer;
    context->end_command_buffer = vkEndCommandBuffer;
    context->fill_buffer = vkCmdFillBuffer;
    context->pipeline_barrier = vkCmdPipelineBarrier;
    context->queue_submit = vkQueueSubmit;
    context->queue_wait_idle = vkQueueWaitIdle;
    context->invalidate_mapped_memory_ranges = vkInvalidateMappedMemoryRanges;
    context->get_instance_proc_addr = gipa;
    context->create_device = vkCreateDevice;
    context->get_device_proc_addr = vkGetDeviceProcAddr;

    status = bvb_handle_table_init(&context->handles,
                                   context->handle_entries,
                                   sizeof(context->handle_entries) /
                                       sizeof(context->handle_entries[0]));
    const uint64_t command_buffer_id =
        bvb_handle_id(BVB_OBJECT_COMMAND_BUFFER, 1U);
    const uint64_t buffer_id = bvb_handle_id(BVB_OBJECT_BUFFER, 1U);
    if (status == 0) {
        status = bvb_handle_table_insert(
            &context->handles, command_buffer_id, 0U,
            vulkan_handle_bits(&context->command_buffer,
                               sizeof(context->command_buffer)));
    }
    if (status == 0) {
        status = bvb_handle_table_insert(
            &context->handles, buffer_id, 0U,
            vulkan_handle_bits(&context->objects.buffer,
                               sizeof(context->objects.buffer)));
    }
    if (status != 0) {
        set_error(error, error_size, "could not establish handle ownership: %d",
                  status);
        bvb_vulkan_batch_context_destroy(context);
        return status;
    }
    *context_output = context;
    return 0;

done:
    cleanup(&objects);
    return status;
}

int bvb_vulkan_batch_context_execute(
    struct bvb_vulkan_batch_context *context, const uint8_t *batch,
    size_t batch_length, struct bvb_vulkan_selftest_result *output,
    char *error, size_t error_size) {
    if (error != NULL && error_size > 0U) {
        error[0] = '\0';
    }
    if (context == NULL || output == NULL ||
        (batch == NULL && batch_length != 0U) ||
        (batch != NULL && batch_length == 0U)) {
        set_error(error, error_size, "invalid Vulkan batch execution");
        return -EINVAL;
    }
    *output = context->base_result;
    output->mismatched_words = 0U;
    output->submit_wait_elapsed_ns = 0U;

    VkResult vk_result = context->reset_command_pool(
        context->objects.device, context->objects.command_pool, 0);
    if (vk_result != VK_SUCCESS) {
        set_error(error, error_size, "vkResetCommandPool failed: %d",
                  (int)vk_result);
        return -EIO;
    }
    const VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vk_result = context->begin_command_buffer(context->command_buffer,
                                               &begin_info);
    if (vk_result != VK_SUCCESS) {
        set_error(error, error_size, "vkBeginCommandBuffer failed: %d",
                  (int)vk_result);
        return -EIO;
    }
    if (batch != NULL) {
        int status = replay_transfer_batch(batch, batch_length, context, error,
                                           error_size);
        if (status != 0) {
            return status;
        }
    } else {
        context->fill_buffer(context->command_buffer, context->objects.buffer,
                             0, BVB_SELFTEST_BUFFER_BYTES,
                             BVB_SELFTEST_FILL_WORD);
        const VkBufferMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = context->objects.buffer,
            .offset = 0,
            .size = BVB_SELFTEST_BUFFER_BYTES,
        };
        context->pipeline_barrier(
            context->command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &barrier, 0, NULL);
    }
    vk_result = context->end_command_buffer(context->command_buffer);
    if (vk_result != VK_SUCCESS) {
        set_error(error, error_size, "vkEndCommandBuffer failed: %d",
                  (int)vk_result);
        return -EIO;
    }
    const VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &context->command_buffer,
    };
    uint64_t start_ns = 0;
    uint64_t end_ns = 0;
    int status = monotonic_ns(&start_ns);
    if (status != 0) {
        set_error(error, error_size, "could not read monotonic clock");
        return status;
    }
    vk_result = context->queue_submit(context->queue, 1, &submit_info,
                                      VK_NULL_HANDLE);
    if (vk_result == VK_SUCCESS) {
        vk_result = context->queue_wait_idle(context->queue);
    }
    if (vk_result != VK_SUCCESS) {
        set_error(error, error_size, "GPU submission failed: %d",
                  (int)vk_result);
        return -EIO;
    }
    status = monotonic_ns(&end_ns);
    if (status != 0) {
        set_error(error, error_size, "could not read monotonic clock");
        return status;
    }
    output->submit_wait_elapsed_ns = end_ns - start_ns;

    if ((output->memory_property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ==
        0U) {
        const VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = context->objects.memory,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        };
        vk_result = context->invalidate_mapped_memory_ranges(
            context->objects.device, 1, &range);
        if (vk_result != VK_SUCCESS) {
            set_error(error, error_size,
                      "vkInvalidateMappedMemoryRanges failed: %d",
                      (int)vk_result);
            return -EIO;
        }
    }
    const uint32_t *words = context->objects.mapped_memory;
    for (uint32_t index = 0;
         index < BVB_SELFTEST_BUFFER_BYTES / sizeof(uint32_t); ++index) {
        if (words[index] != BVB_SELFTEST_FILL_WORD) {
            ++output->mismatched_words;
        }
    }
    if (output->mismatched_words != 0U) {
        set_error(error, error_size, "GPU fill verification found %u mismatches",
                  output->mismatched_words);
        return -EIO;
    }
    return 0;
}

int bvb_vulkan_batch_context_external_memory_test(
    struct bvb_vulkan_batch_context *context,
    struct bvb_vulkan_external_memory_result *output,
    char *error, size_t error_size) {
    if (error != NULL && error_size > 0U) error[0] = '\0';
    if (context == NULL || output == NULL) return -EINVAL;
    memset(output, 0, sizeof(*output));
    output->buffer_bytes = BVB_SELFTEST_BUFFER_BYTES;
    if ((context->base_result.instance_extension_flags &
         BVB_INSTANCE_KHR_EXTERNAL_MEMORY_CAPS) == 0U ||
        (context->base_result.device_extension_flags &
         (BVB_DEVICE_KHR_EXTERNAL_MEMORY |
          BVB_DEVICE_KHR_EXTERNAL_MEMORY_FD)) !=
            (BVB_DEVICE_KHR_EXTERNAL_MEMORY |
             BVB_DEVICE_KHR_EXTERNAL_MEMORY_FD)) {
        set_error(error, error_size,
                  "external-memory FD extensions are unavailable");
        return -ENOTSUP;
    }

    PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR query =
        (PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR)
            context->get_instance_proc_addr(
                context->objects.instance,
                "vkGetPhysicalDeviceExternalBufferPropertiesKHR");
    if (query == NULL) {
        query = (PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR)
            context->get_instance_proc_addr(
                context->objects.instance,
                "vkGetPhysicalDeviceExternalBufferProperties");
    }
    if (query == NULL) {
        set_error(error, error_size,
                  "driver has no external-buffer-properties query");
        return -ENOSYS;
    }
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    const VkPhysicalDeviceExternalBufferInfo external_query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .usage = usage,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    VkExternalBufferProperties properties = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES,
    };
    query(context->physical_device, &external_query, &properties);
    const VkExternalMemoryProperties *external =
        &properties.externalMemoryProperties;
    output->external_memory_features = external->externalMemoryFeatures;
    output->compatible_handle_types = external->compatibleHandleTypes;
    output->export_from_imported_handle_types =
        external->exportFromImportedHandleTypes;
    const VkExternalMemoryFeatureFlags required_features =
        VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
        VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
    if ((external->externalMemoryFeatures & required_features) !=
            required_features ||
        (external->compatibleHandleTypes &
         VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) == 0U) {
        set_error(error, error_size,
                  "opaque FD buffers are not exportable and importable");
        return -ENOTSUP;
    }

    struct bvb_external_resources source = {
        .device = context->objects.device,
    };
    struct bvb_external_resources destination = {0};
    int exported_fd = -1;
    int status = 0;
    const float queue_priority = 1.0F;
    const VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = context->base_result.queue_family_index,
        .queueCount = 1U,
        .pQueuePriorities = &queue_priority,
    };
    static const char *const extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    };
    const VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1U,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 2U,
        .ppEnabledExtensionNames = extensions,
    };
    VkResult vk_result = context->create_device(
        context->physical_device, &device_info, NULL, &destination.device);
    if (vk_result != VK_SUCCESS) {
        set_error(error, error_size,
                  "second vkCreateDevice failed: %d", (int)vk_result);
        return -EIO;
    }
    destination.owns_device = true;

#define LOAD_EXTERNAL(resources, name)                                         \
    PFN_##name resources##_##name = (PFN_##name)                              \
        context->get_device_proc_addr((resources).device, #name)
    LOAD_EXTERNAL(source, vkDestroyBuffer);
    LOAD_EXTERNAL(source, vkGetBufferMemoryRequirements);
    LOAD_EXTERNAL(source, vkAllocateMemory);
    LOAD_EXTERNAL(source, vkFreeMemory);
    LOAD_EXTERNAL(source, vkBindBufferMemory);
    LOAD_EXTERNAL(source, vkMapMemory);
    LOAD_EXTERNAL(source, vkUnmapMemory);
    LOAD_EXTERNAL(source, vkFlushMappedMemoryRanges);
    LOAD_EXTERNAL(source, vkGetMemoryFdKHR);
    LOAD_EXTERNAL(destination, vkDestroyDevice);
    LOAD_EXTERNAL(destination, vkCreateBuffer);
    LOAD_EXTERNAL(destination, vkDestroyBuffer);
    LOAD_EXTERNAL(destination, vkGetBufferMemoryRequirements);
    LOAD_EXTERNAL(destination, vkAllocateMemory);
    LOAD_EXTERNAL(destination, vkFreeMemory);
    LOAD_EXTERNAL(destination, vkBindBufferMemory);
    LOAD_EXTERNAL(destination, vkMapMemory);
    LOAD_EXTERNAL(destination, vkUnmapMemory);
    LOAD_EXTERNAL(destination, vkInvalidateMappedMemoryRanges);
    PFN_vkCreateBuffer source_vkCreateBuffer =
        (PFN_vkCreateBuffer)context->get_device_proc_addr(
            source.device, "vkCreateBuffer");
#undef LOAD_EXTERNAL
    source.destroy_buffer = source_vkDestroyBuffer;
    source.free_memory = source_vkFreeMemory;
    source.unmap_memory = source_vkUnmapMemory;
    destination.destroy_device = destination_vkDestroyDevice;
    destination.destroy_buffer = destination_vkDestroyBuffer;
    destination.free_memory = destination_vkFreeMemory;
    destination.unmap_memory = destination_vkUnmapMemory;
    if (source_vkCreateBuffer == NULL ||
        source_vkGetBufferMemoryRequirements == NULL ||
        source_vkAllocateMemory == NULL || source_vkFreeMemory == NULL ||
        source_vkBindBufferMemory == NULL || source_vkMapMemory == NULL ||
        source_vkUnmapMemory == NULL ||
        source_vkFlushMappedMemoryRanges == NULL ||
        source_vkGetMemoryFdKHR == NULL ||
        destination_vkDestroyDevice == NULL ||
        destination_vkCreateBuffer == NULL ||
        destination_vkDestroyBuffer == NULL ||
        destination_vkGetBufferMemoryRequirements == NULL ||
        destination_vkAllocateMemory == NULL ||
        destination_vkFreeMemory == NULL ||
        destination_vkBindBufferMemory == NULL ||
        destination_vkMapMemory == NULL ||
        destination_vkUnmapMemory == NULL ||
        destination_vkInvalidateMappedMemoryRanges == NULL) {
        set_error(error, error_size,
                  "driver is missing external-memory entry points");
        status = -ENOSYS;
        goto done;
    }

    const VkExternalMemoryBufferCreateInfo external_buffer_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    const VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &external_buffer_info,
        .size = BVB_SELFTEST_BUFFER_BYTES,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    vk_result = source_vkCreateBuffer(
        source.device, &buffer_info, NULL, &source.buffer);
    if (vk_result == VK_SUCCESS) {
        vk_result = destination_vkCreateBuffer(
            destination.device, &buffer_info, NULL, &destination.buffer);
    }
    if (vk_result != VK_SUCCESS) {
        set_error(error, error_size,
                  "external vkCreateBuffer failed: %d", (int)vk_result);
        status = -EIO;
        goto done;
    }
    VkMemoryRequirements source_requirements = {0};
    VkMemoryRequirements destination_requirements = {0};
    source_vkGetBufferMemoryRequirements(
        source.device, source.buffer, &source_requirements);
    destination_vkGetBufferMemoryRequirements(
        destination.device, destination.buffer, &destination_requirements);
    if (source_requirements.size != destination_requirements.size ||
        source_requirements.size < BVB_SELFTEST_BUFFER_BYTES) {
        set_error(error, error_size,
                  "external buffer memory requirements differ");
        status = -EPROTO;
        goto done;
    }
    const uint32_t compatible_memory_types =
        source_requirements.memoryTypeBits &
        destination_requirements.memoryTypeBits;
    bool memory_found = false;
    bool coherent_found = false;
    for (uint32_t index = 0U;
         index < context->memory_properties.memoryTypeCount; ++index) {
        const VkMemoryPropertyFlags flags =
            context->memory_properties.memoryTypes[index].propertyFlags;
        if ((compatible_memory_types & (UINT32_C(1) << index)) == 0U ||
            (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0U) {
            continue;
        }
        const bool coherent =
            (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0U;
        if (!memory_found || (coherent && !coherent_found)) {
            output->memory_type_index = index;
            output->memory_property_flags = flags;
            memory_found = true;
            coherent_found = coherent;
        }
    }
    if (!memory_found) {
        set_error(error, error_size,
                  "no shared host-visible external memory type");
        status = -ENOTSUP;
        goto done;
    }

    const bool dedicated_only =
        (external->externalMemoryFeatures &
         VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0U;
    const VkMemoryDedicatedAllocateInfo source_dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .buffer = source.buffer,
    };
    const VkExportMemoryAllocateInfo export_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = dedicated_only ? &source_dedicated : NULL,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    const VkMemoryAllocateInfo source_allocation_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &export_info,
        .allocationSize = source_requirements.size,
        .memoryTypeIndex = output->memory_type_index,
    };
    vk_result = source_vkAllocateMemory(
        source.device, &source_allocation_info, NULL, &source.memory);
    if (vk_result == VK_SUCCESS) {
        vk_result = source_vkBindBufferMemory(
            source.device, source.buffer, source.memory, 0U);
    }
    if (vk_result == VK_SUCCESS) {
        vk_result = source_vkMapMemory(
            source.device, source.memory, 0U, VK_WHOLE_SIZE, 0U,
            &source.mapped);
    }
    if (vk_result != VK_SUCCESS || source.mapped == NULL) {
        set_error(error, error_size,
                  "export memory setup failed: %d", (int)vk_result);
        status = -EIO;
        goto done;
    }
    for (uint32_t index = 0U; index < BVB_SELFTEST_BUFFER_BYTES; ++index) {
        ((uint8_t *)source.mapped)[index] =
            (uint8_t)(index ^ (index >> 4U) ^ UINT32_C(0x5a));
    }
    if (!coherent_found) {
        const VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = source.memory,
            .offset = 0U,
            .size = VK_WHOLE_SIZE,
        };
        vk_result = source_vkFlushMappedMemoryRanges(
            source.device, 1U, &range);
        if (vk_result != VK_SUCCESS) {
            set_error(error, error_size,
                      "external-memory flush failed: %d", (int)vk_result);
            status = -EIO;
            goto done;
        }
    }
    source_vkUnmapMemory(source.device, source.memory);
    source.mapped = NULL;

    const VkMemoryGetFdInfoKHR fd_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = source.memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    vk_result = source_vkGetMemoryFdKHR(
        source.device, &fd_info, &exported_fd);
    if (vk_result != VK_SUCCESS || exported_fd < 0) {
        set_error(error, error_size,
                  "vkGetMemoryFdKHR failed: %d", (int)vk_result);
        status = -EIO;
        goto done;
    }

    const VkMemoryDedicatedAllocateInfo destination_dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .buffer = destination.buffer,
    };
    const VkImportMemoryFdInfoKHR import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext = dedicated_only ? &destination_dedicated : NULL,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
        .fd = exported_fd,
    };
    const VkMemoryAllocateInfo destination_allocation_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import_info,
        .allocationSize = source_requirements.size,
        .memoryTypeIndex = output->memory_type_index,
    };
    vk_result = destination_vkAllocateMemory(
        destination.device, &destination_allocation_info, NULL,
        &destination.memory);
    if (vk_result == VK_SUCCESS) exported_fd = -1;
    if (vk_result == VK_SUCCESS) {
        vk_result = destination_vkBindBufferMemory(
            destination.device, destination.buffer, destination.memory, 0U);
    }
    if (vk_result == VK_SUCCESS) {
        vk_result = destination_vkMapMemory(
            destination.device, destination.memory, 0U, VK_WHOLE_SIZE, 0U,
            &destination.mapped);
    }
    if (vk_result != VK_SUCCESS || destination.mapped == NULL) {
        set_error(error, error_size,
                  "import memory setup failed: %d", (int)vk_result);
        status = -EIO;
        goto done;
    }
    if (!coherent_found) {
        const VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = destination.memory,
            .offset = 0U,
            .size = VK_WHOLE_SIZE,
        };
        vk_result = destination_vkInvalidateMappedMemoryRanges(
            destination.device, 1U, &range);
        if (vk_result != VK_SUCCESS) {
            set_error(error, error_size,
                      "external-memory invalidate failed: %d",
                      (int)vk_result);
            status = -EIO;
            goto done;
        }
    }
    for (uint32_t index = 0U; index < BVB_SELFTEST_BUFFER_BYTES; ++index) {
        const uint8_t expected =
            (uint8_t)(index ^ (index >> 4U) ^ UINT32_C(0x5a));
        if (((const uint8_t *)destination.mapped)[index] != expected) {
            ++output->mismatched_bytes;
        }
    }
    if (output->mismatched_bytes != 0U) {
        set_error(error, error_size,
                  "external-memory import found %u mismatched bytes",
                  output->mismatched_bytes);
        status = -EIO;
    }

done:
    if (exported_fd >= 0) (void)close(exported_fd);
    cleanup_external_resources(&destination);
    cleanup_external_resources(&source);
    return status;
}

int bvb_vulkan_batch_context_import_external_memory_fd(
    struct bvb_vulkan_batch_context *context, int external_fd,
    uint64_t allocation_size, uint32_t memory_type_index,
    uint32_t buffer_bytes, struct bvb_vulkan_external_memory_result *output,
    char *error, size_t error_size) {
    if (error != NULL && error_size > 0U) error[0] = '\0';
    if (context == NULL || external_fd < 0 || allocation_size == 0U ||
        buffer_bytes == 0U || buffer_bytes > BVB_SELFTEST_BUFFER_BYTES ||
        buffer_bytes > allocation_size || output == NULL) {
        if (external_fd >= 0) (void)close(external_fd);
        return -EINVAL;
    }
    memset(output, 0, sizeof(*output));
    output->buffer_bytes = buffer_bytes;
    output->memory_type_index = memory_type_index;
    if ((context->base_result.instance_extension_flags &
         BVB_INSTANCE_KHR_EXTERNAL_MEMORY_CAPS) == 0U ||
        (context->base_result.device_extension_flags &
         (BVB_DEVICE_KHR_EXTERNAL_MEMORY |
          BVB_DEVICE_KHR_EXTERNAL_MEMORY_FD)) !=
            (BVB_DEVICE_KHR_EXTERNAL_MEMORY |
             BVB_DEVICE_KHR_EXTERNAL_MEMORY_FD)) {
        set_error(error, error_size,
                  "external-memory FD extensions are unavailable");
        (void)close(external_fd);
        return -ENOTSUP;
    }

    PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR query =
        (PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR)
            context->get_instance_proc_addr(
                context->objects.instance,
                "vkGetPhysicalDeviceExternalBufferPropertiesKHR");
    if (query == NULL) {
        query = (PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR)
            context->get_instance_proc_addr(
                context->objects.instance,
                "vkGetPhysicalDeviceExternalBufferProperties");
    }
    if (query == NULL) {
        set_error(error, error_size,
                  "driver has no external-buffer-properties query");
        (void)close(external_fd);
        return -ENOSYS;
    }
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    const VkPhysicalDeviceExternalBufferInfo external_query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .usage = usage,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    VkExternalBufferProperties properties = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES,
    };
    query(context->physical_device, &external_query, &properties);
    const VkExternalMemoryProperties *external =
        &properties.externalMemoryProperties;
    output->external_memory_features = external->externalMemoryFeatures;
    output->compatible_handle_types = external->compatibleHandleTypes;
    output->export_from_imported_handle_types =
        external->exportFromImportedHandleTypes;
    if ((external->externalMemoryFeatures &
         VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0U ||
        (external->compatibleHandleTypes &
         VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) == 0U) {
        set_error(error, error_size, "opaque FD buffers are not importable");
        (void)close(external_fd);
        return -ENOTSUP;
    }

    struct bvb_external_resources imported = {
        .device = context->objects.device,
    };
#define LOAD_IMPORT(name)                                                       \
    PFN_##name imported_##name = (PFN_##name)                                  \
        context->get_device_proc_addr(imported.device, #name)
    LOAD_IMPORT(vkCreateBuffer);
    LOAD_IMPORT(vkDestroyBuffer);
    LOAD_IMPORT(vkGetBufferMemoryRequirements);
    LOAD_IMPORT(vkAllocateMemory);
    LOAD_IMPORT(vkFreeMemory);
    LOAD_IMPORT(vkBindBufferMemory);
    LOAD_IMPORT(vkMapMemory);
    LOAD_IMPORT(vkUnmapMemory);
    LOAD_IMPORT(vkInvalidateMappedMemoryRanges);
#undef LOAD_IMPORT
    imported.destroy_buffer = imported_vkDestroyBuffer;
    imported.free_memory = imported_vkFreeMemory;
    imported.unmap_memory = imported_vkUnmapMemory;
    if (imported_vkCreateBuffer == NULL ||
        imported_vkDestroyBuffer == NULL ||
        imported_vkGetBufferMemoryRequirements == NULL ||
        imported_vkAllocateMemory == NULL || imported_vkFreeMemory == NULL ||
        imported_vkBindBufferMemory == NULL || imported_vkMapMemory == NULL ||
        imported_vkUnmapMemory == NULL ||
        imported_vkInvalidateMappedMemoryRanges == NULL) {
        set_error(error, error_size,
                  "driver is missing external-memory import entry points");
        (void)close(external_fd);
        return -ENOSYS;
    }

    int status = 0;
    VkResult vk_result = VK_SUCCESS;
    const VkExternalMemoryBufferCreateInfo external_buffer_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    const VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &external_buffer_info,
        .size = buffer_bytes,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    vk_result = imported_vkCreateBuffer(
        imported.device, &buffer_info, NULL, &imported.buffer);
    if (vk_result != VK_SUCCESS) {
        set_error(error, error_size,
                  "external import vkCreateBuffer failed: %d",
                  (int)vk_result);
        status = -EIO;
        goto done;
    }
    VkMemoryRequirements requirements = {0};
    imported_vkGetBufferMemoryRequirements(
        imported.device, imported.buffer, &requirements);
    if (requirements.size > allocation_size ||
        memory_type_index >= context->memory_properties.memoryTypeCount ||
        (requirements.memoryTypeBits &
         (UINT32_C(1) << memory_type_index)) == 0U) {
        set_error(error, error_size,
                  "export metadata is incompatible with import buffer");
        status = -EPROTO;
        goto done;
    }
    output->memory_property_flags = context->memory_properties
                                        .memoryTypes[memory_type_index]
                                        .propertyFlags;
    if ((output->memory_property_flags &
         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0U) {
        set_error(error, error_size,
                  "export memory type is not host visible");
        status = -ENOTSUP;
        goto done;
    }

    const bool dedicated_only =
        (external->externalMemoryFeatures &
         VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0U;
    const VkMemoryDedicatedAllocateInfo dedicated_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .buffer = imported.buffer,
    };
    const VkImportMemoryFdInfoKHR import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext = dedicated_only ? &dedicated_info : NULL,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
        .fd = external_fd,
    };
    const VkMemoryAllocateInfo allocation_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import_info,
        .allocationSize = allocation_size,
        .memoryTypeIndex = memory_type_index,
    };
    vk_result = imported_vkAllocateMemory(
        imported.device, &allocation_info, NULL, &imported.memory);
    if (vk_result == VK_SUCCESS) external_fd = -1;
    if (vk_result == VK_SUCCESS) {
        vk_result = imported_vkBindBufferMemory(
            imported.device, imported.buffer, imported.memory, 0U);
    }
    if (vk_result == VK_SUCCESS) {
        vk_result = imported_vkMapMemory(
            imported.device, imported.memory, 0U, VK_WHOLE_SIZE, 0U,
            &imported.mapped);
    }
    if (vk_result != VK_SUCCESS || imported.mapped == NULL) {
        set_error(error, error_size,
                  "external memory import setup failed: %d",
                  (int)vk_result);
        status = -EIO;
        goto done;
    }
    if ((output->memory_property_flags &
         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0U) {
        const VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = imported.memory,
            .offset = 0U,
            .size = VK_WHOLE_SIZE,
        };
        vk_result = imported_vkInvalidateMappedMemoryRanges(
            imported.device, 1U, &range);
        if (vk_result != VK_SUCCESS) {
            set_error(error, error_size,
                      "external-memory invalidate failed: %d",
                      (int)vk_result);
            status = -EIO;
            goto done;
        }
    }
    for (uint32_t index = 0U; index < buffer_bytes; ++index) {
        const uint8_t expected =
            (uint8_t)(index ^ (index >> 4U) ^ UINT32_C(0x5a));
        if (((const uint8_t *)imported.mapped)[index] != expected) {
            ++output->mismatched_bytes;
        }
    }
    if (output->mismatched_bytes != 0U) {
        set_error(error, error_size,
                  "external-memory import found %u mismatched bytes",
                  output->mismatched_bytes);
        status = -EIO;
    }

done:
    if (external_fd >= 0) (void)close(external_fd);
    cleanup_external_resources(&imported);
    return status;
}

void bvb_vulkan_batch_context_destroy(
    struct bvb_vulkan_batch_context *context) {
    if (context != NULL) {
        cleanup(&context->objects);
        free(context);
    }
}

static int run_selftest(const char *loader_path, const uint8_t *batch,
                        size_t batch_length,
                        struct bvb_vulkan_selftest_result *output, char *error,
                        size_t error_size) {
    struct bvb_vulkan_batch_context *context = NULL;
    int status = bvb_vulkan_batch_context_create(
        loader_path, &context, error, error_size);
    if (status == 0) {
        status = bvb_vulkan_batch_context_execute(
            context, batch, batch_length, output, error, error_size);
    }
    bvb_vulkan_batch_context_destroy(context);
    return status;
}

int bvb_vulkan_run_selftest(const char *loader_path,
                            struct bvb_vulkan_selftest_result *output,
                            char *error, size_t error_size) {
    return run_selftest(loader_path, NULL, 0U, output, error, error_size);
}

int bvb_vulkan_run_batched_selftest(
    const char *loader_path, const uint8_t *batch, size_t batch_length,
    struct bvb_vulkan_selftest_result *output, char *error, size_t error_size) {
    return run_selftest(loader_path, batch, batch_length, output, error,
                        error_size);
}
