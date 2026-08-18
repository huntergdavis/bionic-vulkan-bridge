#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include <vulkan/vulkan.h>

static PFN_vkGetInstanceProcAddr object_as_gipa(void *object) {
    union {
        void *object;
        PFN_vkGetInstanceProcAddr function;
    } conversion = {.object = object};
    return conversion.function;
}

static PFN_vkGetDeviceProcAddr object_as_gdpa(void *object) {
    union {
        void *object;
        PFN_vkGetDeviceProcAddr function;
    } conversion = {.object = object};
    return conversion.function;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s FAKE_VULKAN\n", argv[0]);
        return 2;
    }
    void *loader = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (loader == NULL) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }

    PFN_vkGetInstanceProcAddr gipa =
        object_as_gipa(dlsym(loader, "vkGetInstanceProcAddr"));
    if (gipa == NULL || gipa(VK_NULL_HANDLE, "vkCreateInstance") == NULL ||
        gipa(VK_NULL_HANDLE, "vkDefinitelyMissing") != NULL ||
        gipa(VK_NULL_HANDLE, "vkGetInstanceProcAddr") !=
            (PFN_vkVoidFunction)gipa) {
        fputs("instance dispatch was not transparent\n", stderr);
        return 1;
    }

    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(
        VK_NULL_HANDLE, "vkGetDeviceProcAddr");
    if (gdpa == NULL || gdpa(VK_NULL_HANDLE, "vkCreateBuffer") == NULL ||
        gdpa(VK_NULL_HANDLE, "vkDefinitelyMissing") != NULL ||
        gdpa(VK_NULL_HANDLE, "vkGetDeviceProcAddr") !=
            (PFN_vkVoidFunction)gdpa) {
        fputs("device dispatch was not transparent\n", stderr);
        return 1;
    }

    PFN_vkGetDeviceProcAddr direct_gdpa =
        object_as_gdpa(dlsym(loader, "vkGetDeviceProcAddr"));
    if (direct_gdpa == NULL ||
        direct_gdpa(VK_NULL_HANDLE, "vkCreateBuffer") == NULL) {
        fputs("direct device dispatch was not transparent\n", stderr);
        return 1;
    }

    (void)dlclose(loader);
    puts("PASS: Vulkan resolution passthrough");
    return 0;
}
