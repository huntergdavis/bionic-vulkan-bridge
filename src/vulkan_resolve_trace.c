#define _GNU_SOURCE

#include <dlfcn.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <vulkan/vulkan.h>

#define BVB_TRACE_ENV "BVB_VULKAN_TRACE_FILE"
#define BVB_TRACE_NAME_LIMIT 511U

typedef void *(*bvb_dlsym_function)(void *, const char *);

static _Atomic(bvb_dlsym_function) bvb_real_dlsym;
static _Atomic(PFN_vkGetInstanceProcAddr) bvb_real_gipa;
static _Atomic(PFN_vkGetDeviceProcAddr) bvb_real_gdpa;
static _Atomic uint64_t bvb_trace_sequence;

static size_t bvb_append_u64(char *output, uint64_t value) {
    char reversed[20];
    size_t length = 0;
    do {
        reversed[length++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    for (size_t index = 0; index < length; ++index) {
        output[index] = reversed[length - index - 1U];
    }
    return length;
}

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

    /*
     * One append-only write keeps records from separate Wine-side processes
     * intact. Format v1 is:
     * version, pid, tid, process-local sequence, stage, resolved, name.
     */
    char record[1U + 1U + 20U + 1U + 20U + 1U + 20U + 1U + 1U + 1U +
                1U + 1U + BVB_TRACE_NAME_LIMIT + 1U];
    size_t offset = 0;
    record[offset++] = '1';
    record[offset++] = '\t';
    offset += bvb_append_u64(record + offset, (uint64_t)getpid());
    record[offset++] = '\t';
    offset += bvb_append_u64(record + offset, (uint64_t)syscall(SYS_gettid));
    record[offset++] = '\t';
    uint64_t sequence = atomic_fetch_add_explicit(
                            &bvb_trace_sequence, 1U, memory_order_relaxed) +
                        1U;
    offset += bvb_append_u64(record + offset, sequence);
    record[offset++] = '\t';
    record[offset++] = stage;
    record[offset++] = '\t';
    record[offset++] = resolved ? '1' : '0';
    record[offset++] = '\t';
    memcpy(record + offset, name, name_length);
    offset += name_length;
    record[offset++] = '\n';
    ssize_t written = write(descriptor, record, offset);
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
