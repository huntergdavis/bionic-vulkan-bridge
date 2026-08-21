#define _POSIX_C_SOURCE 200809L
#define VK_NO_PROTOTYPES

#include <bvb/first_rejection.h>
#include <bvb/handle.h>
#include <bvb/triangle_dispatch.h>

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
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
vkGetDeviceProcAddr(VkDevice device, const char *name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *name);

static PFN_vkVoidFunction resolve_device(const char *name) {
    return vkGetDeviceProcAddr(VK_NULL_HANDLE, name);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL dummy_get_device_proc_addr(
    VkDevice device, const char *name) {
    (void)device;
    (void)name;
    return NULL;
}

static int check_default(void) {
    CHECK(!bvb_first_rejection_enabled());
    CHECK(resolve_device("vkCmdDispatch") == NULL);
    struct bvb_first_rejection_snapshot snapshot = {0};
    CHECK(bvb_first_rejection_snapshot(&snapshot) == 0);
    CHECK(!snapshot.enabled);
    CHECK(!snapshot.emitted);
    CHECK(snapshot.stub_queries == 0U);
    CHECK(snapshot.null_queries == 0U);
    PFN_vkGetDeviceProcAddr typed = dummy_get_device_proc_addr;
    PFN_vkVoidFunction raw = NULL;
    _Static_assert(sizeof(typed) == sizeof(raw),
                   "Vulkan function pointer width mismatch");
    memcpy(&raw, &typed, sizeof(raw));
    CHECK(bvb_first_rejection_wrap(
              "vkGetDeviceProcAddr", BVB_DXVK_SCOPE_DEVICE, raw) == raw);
    CHECK(resolve_device("vkGetDeviceProcAddr") ==
          resolve_device("vkGetDeviceProcAddr"));
    puts("PASS: default diagnostic preserves NULL dispatch");
    return 0;
}

static int check_required(void) {
    CHECK(bvb_first_rejection_enabled());
    PFN_vkVoidFunction erased = resolve_device("vkCmdDispatch");
    CHECK(erased != NULL);
    PFN_vkCmdDispatch dispatch = NULL;
    memcpy(&dispatch, &erased, sizeof(dispatch));
    dispatch(VK_NULL_HANDLE, 1U, 2U, 3U);

    erased = resolve_device("vkCreateComputePipelines");
    CHECK(erased != NULL);
    PFN_vkCreateComputePipelines create = NULL;
    memcpy(&create, &erased, sizeof(create));
    CHECK(create(VK_NULL_HANDLE, VK_NULL_HANDLE, 0U, NULL, NULL, NULL) ==
          VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(resolve_device("wine_vkAcquireKeyedMutex") == NULL);
    CHECK(resolve_device("vkCmdDispatchBaseKHR") == NULL);

    struct bvb_first_rejection_snapshot snapshot = {0};
    CHECK(bvb_first_rejection_snapshot(&snapshot) == 0);
    CHECK(snapshot.enabled && snapshot.emitted);
    CHECK(strcmp(snapshot.category, "required_unimplemented") == 0);
    CHECK(strcmp(snapshot.entry, "vkCreateComputePipelines") == 0);
    CHECK(strcmp(snapshot.canonical, "vkCreateComputePipelines") == 0);
    CHECK(strcmp(snapshot.scope, "device") == 0);
    CHECK(strcmp(snapshot.reason, "diagnostic_stub_invoked") == 0);
    CHECK(strcmp(snapshot.shape,
                 "VkDevice_value,VkPipelineCache_value,uint32_t_value,"
                 "VkComputePipelineCreateInfo_ptr,VkAllocationCallbacks_ptr,"
                 "VkPipeline_ptr") ==
          0);
    CHECK(snapshot.result == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(snapshot.argument_count == 6U);
    CHECK(snapshot.pointer_mask == 0U);
    CHECK(snapshot.stub_queries == 2U);
    CHECK(snapshot.stub_invocations == 1U);
    CHECK(snapshot.null_queries == 0U);
    puts("PASS: first invoked required entry is reported once");
    return 0;
}

struct rejection_race {
    pthread_barrier_t start;
    PFN_vkCreateComputePipelines create_compute;
    PFN_vkCreateRenderPass create_render_pass;
    VkResult compute_result;
    VkResult render_pass_result;
};

static void *invoke_compute_stub(void *opaque) {
    struct rejection_race *race = opaque;
    const int barrier_result = pthread_barrier_wait(&race->start);
    if (barrier_result != 0 &&
        barrier_result != PTHREAD_BARRIER_SERIAL_THREAD) return NULL;
    race->compute_result = race->create_compute(
        VK_NULL_HANDLE, VK_NULL_HANDLE, 0U, NULL, NULL, NULL);
    return NULL;
}

static void *invoke_render_pass_stub(void *opaque) {
    struct rejection_race *race = opaque;
    const int barrier_result = pthread_barrier_wait(&race->start);
    if (barrier_result != 0 &&
        barrier_result != PTHREAD_BARRIER_SERIAL_THREAD) return NULL;
    race->render_pass_result = race->create_render_pass(
        VK_NULL_HANDLE, NULL, NULL, NULL);
    return NULL;
}

static int check_race(void) {
    CHECK(bvb_first_rejection_enabled());
    struct rejection_race race = {
        .compute_result = VK_ERROR_UNKNOWN,
        .render_pass_result = VK_ERROR_UNKNOWN,
    };
    PFN_vkVoidFunction erased = resolve_device("vkCreateComputePipelines");
    CHECK(erased != NULL);
    memcpy(&race.create_compute, &erased, sizeof(race.create_compute));
    erased = resolve_device("vkCreateRenderPass");
    CHECK(erased != NULL);
    memcpy(&race.create_render_pass, &erased, sizeof(race.create_render_pass));
    CHECK(pthread_barrier_init(&race.start, NULL, 2U) == 0);
    pthread_t threads[2];
    CHECK(pthread_create(&threads[0], NULL, invoke_compute_stub, &race) == 0);
    CHECK(pthread_create(&threads[1], NULL, invoke_render_pass_stub, &race) == 0);
    CHECK(pthread_join(threads[0], NULL) == 0);
    CHECK(pthread_join(threads[1], NULL) == 0);
    CHECK(pthread_barrier_destroy(&race.start) == 0);
    CHECK(race.compute_result == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(race.render_pass_result == VK_ERROR_FEATURE_NOT_PRESENT);
    struct bvb_first_rejection_snapshot snapshot = {0};
    CHECK(bvb_first_rejection_snapshot(&snapshot) == 0);
    CHECK(snapshot.enabled && snapshot.emitted);
    CHECK(strcmp(snapshot.entry, "vkCreateComputePipelines") == 0 ||
          strcmp(snapshot.entry, "vkCreateRenderPass") == 0);
    CHECK(snapshot.stub_queries == 2U);
    CHECK(snapshot.stub_invocations == 1U);
    puts("PASS: concurrent stubs select one complete winner");
    return 0;
}

static int check_void_exit(void) {
    CHECK(bvb_first_rejection_enabled());
    PFN_vkVoidFunction erased = resolve_device("vkGetDeviceQueue2");
    CHECK(erased != NULL);
    PFN_vkGetDeviceQueue2 get_queue = NULL;
    memcpy(&get_queue, &erased, sizeof(get_queue));
    VkQueue queue = (VkQueue)(uintptr_t)1U;
    const VkDeviceQueueInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2,
    };
    get_queue(VK_NULL_HANDLE, &info, &queue);
    CHECK(false);
    return 1;
}

static int check_implemented_rejection(void) {
    CHECK(bvb_first_rejection_enabled());
    PFN_vkVoidFunction erased =
        vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    CHECK(erased != NULL);
    PFN_vkCreateInstance create = NULL;
    memcpy(&create, &erased, sizeof(create));
    const VkInstanceCreateInfo invalid = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    };
    VkInstance instance = VK_NULL_HANDLE;
    CHECK(create(&invalid, NULL, &instance) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(instance == VK_NULL_HANDLE);

    struct bvb_first_rejection_snapshot snapshot = {0};
    CHECK(bvb_first_rejection_snapshot(&snapshot) == 0);
    CHECK(snapshot.enabled && snapshot.emitted);
    CHECK(strcmp(snapshot.category, "implemented_rejection") == 0);
    CHECK(strcmp(snapshot.entry, "vkCreateInstance") == 0);
    CHECK(strcmp(snapshot.canonical, "vkCreateInstance") == 0);
    CHECK(strcmp(snapshot.scope, "global") == 0);
    CHECK(strcmp(snapshot.reason, "negative_vkresult") == 0);
    CHECK(strcmp(snapshot.shape,
                 "VkInstanceCreateInfo_ptr,VkAllocationCallbacks_ptr,VkInstance_ptr") ==
          0);
    CHECK(snapshot.result == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(snapshot.argument_count == 3U);
    CHECK(snapshot.pointer_mask == UINT64_C(5));
    CHECK(snapshot.executable_invocations == 1U);
    CHECK(snapshot.implemented_rejections == 1U);
    puts("PASS: first implemented negative VkResult is reported");
    return 0;
}

static int check_command_poison(void) {
    CHECK(bvb_first_rejection_enabled());
    uint8_t batch[512] = {0};
    const uint64_t command_buffer_id =
        bvb_handle_id(BVB_OBJECT_COMMAND_BUFFER, 91U);
    const uint64_t sequence = 73U;
    VkCommandBuffer command_buffer = bvb_triangle_command_buffer_create(
        batch, sizeof(batch), command_buffer_id, sequence);
    CHECK(command_buffer != VK_NULL_HANDLE);

    PFN_vkVoidFunction erased = resolve_device("vkCmdBeginRendering");
    CHECK(erased != NULL);
    PFN_vkCmdBeginRendering begin = NULL;
    memcpy(&begin, &erased, sizeof(begin));
    const VkRenderingInfo unsupported = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {.extent = {64U, 64U}},
        .layerCount = 0U,
    };
    begin(command_buffer, &unsupported);
    CHECK(bvb_triangle_command_buffer_status(command_buffer) == -ENOTSUP);
    size_t length = 0U;
    CHECK(bvb_triangle_command_buffer_finish(command_buffer, &length) ==
          -ENOTSUP);

    struct bvb_first_rejection_snapshot snapshot = {0};
    CHECK(bvb_first_rejection_snapshot(&snapshot) == 0);
    CHECK(snapshot.enabled && snapshot.emitted);
    CHECK(strcmp(snapshot.category, "command_poison") == 0);
    CHECK(strcmp(snapshot.entry, "vkCmdBeginRendering") == 0);
    CHECK(strcmp(snapshot.reason, "unsupported_rendering_info") == 0);
    CHECK(strcmp(snapshot.shape, "VkRenderingInfo_ptr") == 0);
    CHECK(snapshot.result == -ENOTSUP);
    CHECK(snapshot.command_buffer_id == command_buffer_id);
    CHECK(snapshot.command_sequence == sequence);
    CHECK(snapshot.end_poison);
    CHECK(snapshot.executable_invocations == 1U);
    CHECK(snapshot.command_poisons == 1U);
    CHECK(snapshot.command_end_failures == 1U);
    bvb_triangle_command_buffer_destroy(command_buffer);
    puts("PASS: first void command rejection is reported at End");
    return 0;
}

int main(int argument_count, char **arguments) {
    CHECK(argument_count == 2);
    if (strcmp(arguments[1], "default") == 0) return check_default();
    if (strcmp(arguments[1], "required") == 0) return check_required();
    if (strcmp(arguments[1], "implemented") == 0)
        return check_implemented_rejection();
    if (strcmp(arguments[1], "poison") == 0) return check_command_poison();
    if (strcmp(arguments[1], "race") == 0) return check_race();
    if (strcmp(arguments[1], "void-exit") == 0) return check_void_exit();
    fprintf(stderr, "unknown mode: %s\n", arguments[1]);
    return 2;
}
