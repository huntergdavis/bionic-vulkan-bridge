#include <string.h>

#include <vulkan/vulkan.h>

static void VKAPI_PTR fake_noop(void) {}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *name) {
    (void)device;
    if (name != NULL && strcmp(name, "vkGetDeviceProcAddr") == 0) {
        return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    }
    if (name != NULL && strcmp(name, "vkCreateBuffer") == 0) {
        return fake_noop;
    }
    return NULL;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *name) {
    (void)instance;
    if (name != NULL && strcmp(name, "vkGetInstanceProcAddr") == 0) {
        return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    }
    if (name != NULL && strcmp(name, "vkGetDeviceProcAddr") == 0) {
        return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    }
    if (name != NULL && strcmp(name, "vkCreateInstance") == 0) {
        return fake_noop;
    }
    return NULL;
}
