#define _GNU_SOURCE

#include <dlfcn.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <vulkan/vulkan.h>

#define BVB_TRACE_ENV "BVB_VULKAN_TRACE_FILE"
#define BVB_TRACE_NAME_LIMIT 511U

typedef void *(*bvb_dlsym_function)(void *, const char *);

static _Atomic(bvb_dlsym_function) bvb_real_dlsym;
static _Atomic(PFN_vkGetInstanceProcAddr) bvb_real_gipa;
static _Atomic(PFN_vkGetDeviceProcAddr) bvb_real_gdpa;

static void *bvb_gipa_as_object(PFN_vkGetInstanceProcAddr function) {
    union {
        PFN_vkGetInstanceProcAddr function;
        void *object;
    } conversion = {.function = function};
    return conversion.object;
}

static void *bvb_gdpa_as_object(PFN_vkGetDeviceProcAddr function) {
    union {
        PFN_vkGetDeviceProcAddr function;
        void *object;
    } conversion = {.function = function};
    return conversion.object;
}

static bvb_dlsym_function bvb_object_as_dlsym(void *object) {
    union {
        void *object;
        bvb_dlsym_function function;
    } conversion = {.object = object};
    return conversion.function;
}

static bvb_dlsym_function bvb_find_real_dlsym(void) {
    bvb_dlsym_function function =
        atomic_load_explicit(&bvb_real_dlsym, memory_order_acquire);
    if (function != NULL) {
        return function;
    }

    static const char *const versions[] = {
        "GLIBC_2.34",
        "GLIBC_2.17",
        "GLIBC_2.2.5",
    };
    void *symbol = NULL;
    for (size_t index = 0; index < sizeof(versions) / sizeof(versions[0]);
         ++index) {
        symbol = dlvsym(RTLD_NEXT, "dlsym", versions[index]);
        if (symbol != NULL) {
            break;
        }
    }
    if (symbol == NULL) {
        return NULL;
    }

    function = bvb_object_as_dlsym(symbol);
    bvb_dlsym_function expected = NULL;
    if (!atomic_compare_exchange_strong_explicit(
            &bvb_real_dlsym, &expected, function, memory_order_release,
            memory_order_acquire)) {
        function = expected;
    }
    return function;
}

static void bvb_trace_record(char stage, bool resolved, const char *name) {
    const char *path = getenv(BVB_TRACE_ENV);
    if (path == NULL || path[0] != '/' || name == NULL) {
        return;
    }

    size_t name_length = strnlen(name, BVB_TRACE_NAME_LIMIT + 1U);
    if (name_length == 0U || name_length > BVB_TRACE_NAME_LIMIT) {
        return;
    }

    int descriptor = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC |
                                    O_NOFOLLOW,
                          S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
        return;
    }

    struct stat metadata;
    if (fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode)) {
        (void)close(descriptor);
        return;
    }
    (void)fchmod(descriptor, S_IRUSR | S_IWUSR);

    char record[4U + BVB_TRACE_NAME_LIMIT + 1U];
    record[0] = stage;
    record[1] = '\t';
    record[2] = resolved ? '1' : '0';
    record[3] = '\t';
    memcpy(record + 4U, name, name_length);
    record[4U + name_length] = '\n';
    ssize_t written = write(descriptor, record, 5U + name_length);
    (void)written;
    (void)close(descriptor);
}

static PFN_vkVoidFunction VKAPI_PTR bvb_trace_gdpa(VkDevice device,
                                                   const char *name);

static PFN_vkVoidFunction VKAPI_PTR bvb_trace_gipa(VkInstance instance,
                                                   const char *name) {
    PFN_vkGetInstanceProcAddr real =
        atomic_load_explicit(&bvb_real_gipa, memory_order_acquire);
    if (real == NULL) {
        return NULL;
    }

    PFN_vkVoidFunction result = real(instance, name);
    bvb_trace_record('I', result != NULL, name);
    if (result == NULL || name == NULL) {
        return result;
    }
    if (strcmp(name, "vkGetInstanceProcAddr") == 0) {
        return (PFN_vkVoidFunction)bvb_trace_gipa;
    }
    if (strcmp(name, "vkGetDeviceProcAddr") == 0) {
        atomic_store_explicit(&bvb_real_gdpa, (PFN_vkGetDeviceProcAddr)result,
                              memory_order_release);
        return (PFN_vkVoidFunction)bvb_trace_gdpa;
    }
    return result;
}

static PFN_vkVoidFunction VKAPI_PTR bvb_trace_gdpa(VkDevice device,
                                                   const char *name) {
    PFN_vkGetDeviceProcAddr real =
        atomic_load_explicit(&bvb_real_gdpa, memory_order_acquire);
    if (real == NULL) {
        return NULL;
    }

    PFN_vkVoidFunction result = real(device, name);
    bvb_trace_record('D', result != NULL, name);
    if (result != NULL && name != NULL &&
        strcmp(name, "vkGetDeviceProcAddr") == 0) {
        return (PFN_vkVoidFunction)bvb_trace_gdpa;
    }
    return result;
}

__attribute__((visibility("default"))) void *dlsym(void *handle,
                                                    const char *name) {
    bvb_dlsym_function real = bvb_find_real_dlsym();
    if (real == NULL) {
        return NULL;
    }

    void *result = real(handle, name);
    if (strcmp(name, "vkGetInstanceProcAddr") == 0) {
        bvb_trace_record('L', result != NULL, name);
        if (result != NULL) {
            union {
                void *object;
                PFN_vkGetInstanceProcAddr function;
            } conversion = {.object = result};
            atomic_store_explicit(&bvb_real_gipa, conversion.function,
                                  memory_order_release);
            return bvb_gipa_as_object(bvb_trace_gipa);
        }
    } else if (strcmp(name, "vkGetDeviceProcAddr") == 0) {
        bvb_trace_record('L', result != NULL, name);
        if (result != NULL) {
            union {
                void *object;
                PFN_vkGetDeviceProcAddr function;
            } conversion = {.object = result};
            atomic_store_explicit(&bvb_real_gdpa, conversion.function,
                                  memory_order_release);
            return bvb_gdpa_as_object(bvb_trace_gdpa);
        }
    }
    return result;
}
