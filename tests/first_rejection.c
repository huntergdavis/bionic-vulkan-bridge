#define VK_NO_PROTOTYPES

#include <bvb/first_rejection.h>
#include <bvb/handle.h>
#include <bvb/triangle_dispatch.h>

#include <errno.h>
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

static int check_default(void) {
    CHECK(!bvb_first_rejection_enabled());
    CHECK(resolve_device("vkCmdDispatch") == NULL);
    struct bvb_first_rejection_snapshot snapshot = {0};
    CHECK(bvb_first_rejection_snapshot(&snapshot) == 0);
    CHECK(!snapshot.enabled);
    CHECK(!snapshot.emitted);
    CHECK(snapshot.stub_queries == 0U);
    CHECK(snapshot.null_queries == 0U);
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

    struct bvb_first_rejection_snapshot snapshot = {0};
    CHECK(bvb_first_rejection_snapshot(&snapshot) == 0);
    CHECK(snapshot.enabled && snapshot.emitted);
    CHECK(strcmp(snapshot.category, "required_unimplemented") == 0);
    CHECK(strcmp(snapshot.entry, "vkCmdDispatch") == 0);
    CHECK(strcmp(snapshot.canonical, "vkCmdDispatch") == 0);
    CHECK(strcmp(snapshot.scope, "device") == 0);
    CHECK(strcmp(snapshot.reason, "diagnostic_stub_invoked") == 0);
    CHECK(strcmp(snapshot.shape,
                 "VkCommandBuffer_value,uint32_t_value,uint32_t_value,uint32_t_value") ==
          0);
    CHECK(snapshot.result == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(snapshot.argument_count == 4U);
    CHECK(snapshot.pointer_mask == 0U);
    CHECK(snapshot.stub_queries == 2U);
    CHECK(snapshot.stub_invocations == 2U);
    CHECK(snapshot.null_queries == 1U);
    puts("PASS: first invoked required entry is reported once");
    return 0;
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
    fprintf(stderr, "unknown mode: %s\n", arguments[1]);
    return 2;
}
