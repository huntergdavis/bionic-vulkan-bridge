#define VK_NO_PROTOTYPES

#include <bvb/global_dispatch.h>
#include <bvb/handle.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,          \
                    __LINE__, #expression);                                     \
            return 1;                                                           \
        }                                                                       \
    } while (0)

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *name);

typedef VkResult(VKAPI_PTR *bvb_create_platform_surface_fn)(
    VkInstance instance, const void *create_info,
    const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface);

#define RESOLVE_INSTANCE(instance, name, output)                                \
    do {                                                                        \
        PFN_vkVoidFunction erased = vkGetInstanceProcAddr((instance), (name));  \
        CHECK(erased != NULL);                                                  \
        _Static_assert(sizeof(output) == sizeof(erased),                        \
                       "Vulkan function pointer width mismatch");              \
        memcpy(&(output), &erased, sizeof(output));                             \
    } while (0)

#define RESOLVE_DEVICE(device, name, output)                                    \
    do {                                                                        \
        PFN_vkVoidFunction erased = vkGetDeviceProcAddr((device), (name));      \
        CHECK(erased != NULL);                                                  \
        _Static_assert(sizeof(output) == sizeof(erased),                        \
                       "Vulkan function pointer width mismatch");              \
        memcpy(&(output), &erased, sizeof(output));                             \
    } while (0)

static int create_instance(VkInstance *instance,
                           PFN_vkDestroyInstance *destroy_instance) {
    PFN_vkCreateInstance create = NULL;
    RESOLVE_INSTANCE(VK_NULL_HANDLE, "vkCreateInstance", create);
    const VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .apiVersion = VK_API_VERSION_1_1,
    };
    const VkInstanceCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
    };
    CHECK(create(&info, NULL, instance) == VK_SUCCESS);
    CHECK(*instance != VK_NULL_HANDLE);
    RESOLVE_INSTANCE(*instance, "vkDestroyInstance", *destroy_instance);
    return 0;
}

static int check_wsi_negative(void) {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkDestroyInstance destroy_instance = NULL;
    CHECK(create_instance(&instance, &destroy_instance) == 0);
    bvb_create_platform_surface_fn create_surface = NULL;
    RESOLVE_INSTANCE(instance, "vkCreateXlibSurfaceKHR", create_surface);
    VkSurfaceKHR surface = (VkSurfaceKHR)(uintptr_t)1U;
    CHECK(create_surface(instance, NULL, NULL, &surface) ==
          VK_ERROR_INITIALIZATION_FAILED);
    CHECK(surface == VK_NULL_HANDLE);
    destroy_instance(instance, NULL);
    puts("PASS: E079a protected WSI negative VkResult recorded");
    return 0;
}

static int check_real_command_poison(void) {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkDestroyInstance destroy_instance = NULL;
    CHECK(create_instance(&instance, &destroy_instance) == 0);
    PFN_vkEnumeratePhysicalDevices enumerate = NULL;
    PFN_vkCreateDevice create_device = NULL;
    RESOLVE_INSTANCE(instance, "vkEnumeratePhysicalDevices", enumerate);
    RESOLVE_INSTANCE(instance, "vkCreateDevice", create_device);
    uint32_t physical_count = 1U;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    CHECK(enumerate(instance, &physical_count, &physical) == VK_SUCCESS);
    CHECK(physical_count == 1U && physical != VK_NULL_HANDLE);

    const float priority = 1.0F;
    const VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0U,
        .queueCount = 1U,
        .pQueuePriorities = &priority,
    };
    const VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1U,
        .pQueueCreateInfos = &queue_info,
    };
    VkDevice device = VK_NULL_HANDLE;
    CHECK(create_device(physical, &device_info, NULL, &device) == VK_SUCCESS);
    CHECK(device != VK_NULL_HANDLE);

    PFN_vkDestroyDevice destroy_device = NULL;
    PFN_vkCreateCommandPool create_pool = NULL;
    PFN_vkDestroyCommandPool destroy_pool = NULL;
    PFN_vkAllocateCommandBuffers allocate = NULL;
    PFN_vkFreeCommandBuffers free_buffers = NULL;
    PFN_vkBeginCommandBuffer begin = NULL;
    PFN_vkEndCommandBuffer end = NULL;
    PFN_vkCmdDispatch dispatch = NULL;
    RESOLVE_DEVICE(device, "vkDestroyDevice", destroy_device);
    RESOLVE_DEVICE(device, "vkCreateCommandPool", create_pool);
    RESOLVE_DEVICE(device, "vkDestroyCommandPool", destroy_pool);
    RESOLVE_DEVICE(device, "vkAllocateCommandBuffers", allocate);
    RESOLVE_DEVICE(device, "vkFreeCommandBuffers", free_buffers);
    RESOLVE_DEVICE(device, "vkBeginCommandBuffer", begin);
    RESOLVE_DEVICE(device, "vkEndCommandBuffer", end);
    RESOLVE_DEVICE(device, "vkCmdDispatch", dispatch);

    const VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = 0U,
    };
    VkCommandPool pool = VK_NULL_HANDLE;
    CHECK(create_pool(device, &pool_info, NULL, &pool) == VK_SUCCESS);
    const VkCommandBufferAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1U,
    };
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    CHECK(allocate(device, &allocate_info, &command_buffer) == VK_SUCCESS);
    CHECK(command_buffer != VK_NULL_HANDLE);
    const uint64_t command_id = bvb_command_buffer_proxy_id(command_buffer);
    CHECK(bvb_handle_type(command_id) == BVB_OBJECT_COMMAND_BUFFER);
    const VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    CHECK(begin(command_buffer, &begin_info) == VK_SUCCESS);
    dispatch(command_buffer, 1U, 1U, 1U);
    CHECK(end(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);

    free_buffers(device, pool, 1U, &command_buffer);
    destroy_pool(device, pool, NULL);
    destroy_device(device, NULL);
    destroy_instance(instance, NULL);
    printf("PASS: E079a real command poison command_buffer=%llu\n",
           (unsigned long long)command_id);
    return 0;
}

int main(int argument_count, char **arguments) {
    CHECK(argument_count == 2);
    if (strcmp(arguments[1], "command") == 0)
        return check_real_command_poison();
    if (strcmp(arguments[1], "wsi") == 0) return check_wsi_negative();
    fprintf(stderr, "unknown mode: %s\n", arguments[1]);
    return 2;
}
