#include <stdint.h>
#include <stdlib.h>

#if defined(_WIN32)
#define BVB_EXPORT __declspec(dllexport)
#else
#define BVB_EXPORT __attribute__((visibility("default")))
#endif

struct AImageReader {
    uint32_t marker;
};

struct ANativeWindow {
    uint32_t marker;
};

static struct ANativeWindow fake_window = {UINT32_C(0xbabef00d)};

BVB_EXPORT int32_t AImageReader_newWithUsage(
    int32_t width, int32_t height, int32_t format, uint64_t usage,
    int32_t max_images, struct AImageReader **reader) {
    if (reader == NULL || width != 64 || height != 64 || format != 1 ||
        usage != (UINT64_C(1) << 8) || max_images != 3) {
        return -22;
    }
    *reader = calloc(1, sizeof(**reader));
    if (*reader == NULL) {
        return -12;
    }
    (*reader)->marker = UINT32_C(0xa11ce123);
    return 0;
}

BVB_EXPORT int32_t AImageReader_getWindow(
    struct AImageReader *reader, struct ANativeWindow **window) {
    if (reader == NULL || reader->marker != UINT32_C(0xa11ce123) ||
        window == NULL) {
        return -22;
    }
    *window = &fake_window;
    return 0;
}

BVB_EXPORT void AImageReader_delete(struct AImageReader *reader) {
    free(reader);
}
