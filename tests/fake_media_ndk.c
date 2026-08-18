#include <stdint.h>
#include <stdlib.h>

#if defined(_WIN32)
#define BVB_EXPORT __declspec(dllexport)
#else
#define BVB_EXPORT __attribute__((visibility("default")))
#endif

struct AImageReader {
    uint32_t marker;
    uint64_t usage;
};

struct AImage {
    uint8_t data[64 * 64 * 4];
};

struct ANativeWindow {
    uint32_t marker;
};

static struct ANativeWindow fake_window = {UINT32_C(0xbabef00d)};

BVB_EXPORT int32_t AImageReader_newWithUsage(
    int32_t width, int32_t height, int32_t format, uint64_t usage,
    int32_t max_images, struct AImageReader **reader) {
    if (reader == NULL || width != 64 || height != 64 || format != 1 ||
        (usage != (UINT64_C(1) << 8) && usage != UINT64_C(3)) ||
        max_images != 3) {
        return -22;
    }
    *reader = calloc(1, sizeof(**reader));
    if (*reader == NULL) {
        return -12;
    }
    (*reader)->marker = UINT32_C(0xa11ce123);
    (*reader)->usage = usage;
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

BVB_EXPORT int32_t AImageReader_acquireNextImage(
    struct AImageReader *reader, struct AImage **image) {
    if (reader == NULL || reader->marker != UINT32_C(0xa11ce123) ||
        reader->usage != UINT64_C(3) || image == NULL) {
        return -22;
    }
    *image = malloc(sizeof(**image));
    if (*image == NULL) {
        return -12;
    }
    for (size_t offset = 0; offset < sizeof((*image)->data); offset += 4U) {
        (*image)->data[offset] = 255U;
        (*image)->data[offset + 1U] = 0U;
        (*image)->data[offset + 2U] = 255U;
        (*image)->data[offset + 3U] = 255U;
    }
    return 0;
}

BVB_EXPORT void AImage_delete(struct AImage *image) {
    free(image);
}

BVB_EXPORT int32_t AImage_getNumberOfPlanes(
    const struct AImage *image, int32_t *plane_count) {
    if (image == NULL || plane_count == NULL) {
        return -22;
    }
    *plane_count = 1;
    return 0;
}

BVB_EXPORT int32_t AImage_getPlanePixelStride(
    const struct AImage *image, int plane, int32_t *pixel_stride) {
    if (image == NULL || plane != 0 || pixel_stride == NULL) {
        return -22;
    }
    *pixel_stride = 4;
    return 0;
}

BVB_EXPORT int32_t AImage_getPlaneRowStride(
    const struct AImage *image, int plane, int32_t *row_stride) {
    if (image == NULL || plane != 0 || row_stride == NULL) {
        return -22;
    }
    *row_stride = 64 * 4;
    return 0;
}

BVB_EXPORT int32_t AImage_getPlaneData(
    const struct AImage *image, int plane, uint8_t **data, int *data_length) {
    if (image == NULL || plane != 0 || data == NULL || data_length == NULL) {
        return -22;
    }
    *data = (uint8_t *)image->data;
    *data_length = (int)sizeof(image->data);
    return 0;
}
