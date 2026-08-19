#include <bvb/vulkan_discovery.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>

struct bvb_wire_writer {
    uint8_t *output;
    size_t capacity;
    size_t offset;
    int status;
};

struct bvb_wire_reader {
    const uint8_t *input;
    size_t length;
    size_t offset;
    int status;
};

static void bvb_writer_put_bytes(struct bvb_wire_writer *writer,
                                 const void *bytes, size_t length) {
    if (writer->status != 0) {
        return;
    }
    if (length > writer->capacity - writer->offset) {
        writer->status = -EMSGSIZE;
        return;
    }
    memcpy(writer->output + writer->offset, bytes, length);
    writer->offset += length;
}

static void bvb_writer_put_u32(struct bvb_wire_writer *writer,
                               uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t)value,
        (uint8_t)(value >> 8U),
        (uint8_t)(value >> 16U),
        (uint8_t)(value >> 24U),
    };
    bvb_writer_put_bytes(writer, bytes, sizeof(bytes));
}

static void bvb_writer_put_u64(struct bvb_wire_writer *writer,
                               uint64_t value) {
    uint8_t bytes[8];
    for (uint32_t index = 0U; index < 8U; ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8U));
    }
    bvb_writer_put_bytes(writer, bytes, sizeof(bytes));
}

static void bvb_writer_put_float(struct bvb_wire_writer *writer, float value) {
    uint32_t bits = 0U;
    _Static_assert(sizeof(bits) == sizeof(value), "float must be 32-bit");
    memcpy(&bits, &value, sizeof(bits));
    bvb_writer_put_u32(writer, bits);
}

static void bvb_reader_get_bytes(struct bvb_wire_reader *reader, void *output,
                                 size_t length) {
    if (reader->status != 0) {
        return;
    }
    if (length > reader->length - reader->offset) {
        reader->status = -EMSGSIZE;
        return;
    }
    memcpy(output, reader->input + reader->offset, length);
    reader->offset += length;
}

static uint32_t bvb_reader_get_u32(struct bvb_wire_reader *reader) {
    uint8_t bytes[4] = {0};
    bvb_reader_get_bytes(reader, bytes, sizeof(bytes));
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8U |
           (uint32_t)bytes[2] << 16U | (uint32_t)bytes[3] << 24U;
}

static uint64_t bvb_reader_get_u64(struct bvb_wire_reader *reader) {
    uint8_t bytes[8] = {0};
    bvb_reader_get_bytes(reader, bytes, sizeof(bytes));
    uint64_t value = 0U;
    for (uint32_t index = 0U; index < 8U; ++index) {
        value |= (uint64_t)bytes[index] << (index * 8U);
    }
    return value;
}

static float bvb_reader_get_float(struct bvb_wire_reader *reader) {
    const uint32_t bits = bvb_reader_get_u32(reader);
    float value = 0.0F;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

#include "bvb_vulkan_discovery_wire.inc"

static int writer_finish(const struct bvb_wire_writer *writer,
                         uint32_t *output_length) {
    if (writer->status != 0) {
        return writer->status;
    }
    if (writer->offset > UINT32_MAX || output_length == NULL) {
        return -EOVERFLOW;
    }
    *output_length = (uint32_t)writer->offset;
    return 0;
}

static int reader_finish(const struct bvb_wire_reader *reader) {
    if (reader->status != 0) {
        return reader->status;
    }
    return reader->offset == reader->length ? 0 : -EPROTO;
}

int bvb_vulkan_encode_physical_device_features(
    uint8_t output[BVB_VULKAN_DISCOVERY_MAX_PAYLOAD],
    const VkPhysicalDeviceFeatures *features, uint32_t *output_length) {
    if (output == NULL || features == NULL || output_length == NULL) {
        return -EINVAL;
    }
    memset(output, 0, BVB_VULKAN_DISCOVERY_MAX_PAYLOAD);
    struct bvb_wire_writer writer = {
        .output = output,
        .capacity = BVB_VULKAN_DISCOVERY_MAX_PAYLOAD,
    };
    bvb_wire_encode_VkPhysicalDeviceFeatures(&writer, features);
    return writer_finish(&writer, output_length);
}

int bvb_vulkan_decode_physical_device_features(
    const uint8_t *input, uint32_t input_length,
    VkPhysicalDeviceFeatures *features) {
    if (input == NULL || features == NULL) {
        return -EINVAL;
    }
    memset(features, 0, sizeof(*features));
    struct bvb_wire_reader reader = {.input = input, .length = input_length};
    bvb_wire_decode_VkPhysicalDeviceFeatures(&reader, features);
    return reader_finish(&reader);
}

int bvb_vulkan_encode_physical_device_properties(
    uint8_t output[BVB_VULKAN_DISCOVERY_MAX_PAYLOAD],
    const VkPhysicalDeviceProperties *properties, uint32_t *output_length) {
    if (output == NULL || properties == NULL || output_length == NULL) {
        return -EINVAL;
    }
    memset(output, 0, BVB_VULKAN_DISCOVERY_MAX_PAYLOAD);
    struct bvb_wire_writer writer = {
        .output = output,
        .capacity = BVB_VULKAN_DISCOVERY_MAX_PAYLOAD,
    };
    bvb_wire_encode_VkPhysicalDeviceProperties(&writer, properties);
    return writer_finish(&writer, output_length);
}

int bvb_vulkan_decode_physical_device_properties(
    const uint8_t *input, uint32_t input_length,
    VkPhysicalDeviceProperties *properties) {
    if (input == NULL || properties == NULL) {
        return -EINVAL;
    }
    memset(properties, 0, sizeof(*properties));
    struct bvb_wire_reader reader = {.input = input, .length = input_length};
    bvb_wire_decode_VkPhysicalDeviceProperties(&reader, properties);
    return reader_finish(&reader);
}

int bvb_vulkan_encode_queue_family_properties(
    uint8_t output[BVB_VULKAN_DISCOVERY_MAX_PAYLOAD],
    const VkQueueFamilyProperties *properties, uint32_t count,
    uint32_t *output_length) {
    if (output == NULL || output_length == NULL ||
        count > BVB_VULKAN_MAX_QUEUE_FAMILIES ||
        (count != 0U && properties == NULL)) {
        return -EINVAL;
    }
    memset(output, 0, BVB_VULKAN_DISCOVERY_MAX_PAYLOAD);
    struct bvb_wire_writer writer = {
        .output = output,
        .capacity = BVB_VULKAN_DISCOVERY_MAX_PAYLOAD,
    };
    bvb_writer_put_u32(&writer, count);
    for (uint32_t index = 0U; index < count; ++index) {
        bvb_wire_encode_VkQueueFamilyProperties(&writer, &properties[index]);
    }
    return writer_finish(&writer, output_length);
}

int bvb_vulkan_decode_queue_family_properties(
    const uint8_t *input, uint32_t input_length,
    VkQueueFamilyProperties properties[BVB_VULKAN_MAX_QUEUE_FAMILIES],
    uint32_t *count) {
    if (input == NULL || properties == NULL || count == NULL) {
        return -EINVAL;
    }
    memset(properties, 0,
           sizeof(*properties) * BVB_VULKAN_MAX_QUEUE_FAMILIES);
    struct bvb_wire_reader reader = {.input = input, .length = input_length};
    const uint32_t decoded_count = bvb_reader_get_u32(&reader);
    if (decoded_count > BVB_VULKAN_MAX_QUEUE_FAMILIES) {
        return -EPROTO;
    }
    for (uint32_t index = 0U; index < decoded_count; ++index) {
        bvb_wire_decode_VkQueueFamilyProperties(&reader, &properties[index]);
    }
    const int result = reader_finish(&reader);
    if (result == 0) {
        *count = decoded_count;
    }
    return result;
}

int bvb_vulkan_encode_memory_properties(
    uint8_t output[BVB_VULKAN_DISCOVERY_MAX_PAYLOAD],
    const VkPhysicalDeviceMemoryProperties *properties,
    uint32_t *output_length) {
    if (output == NULL || properties == NULL || output_length == NULL ||
        properties->memoryTypeCount > VK_MAX_MEMORY_TYPES ||
        properties->memoryHeapCount > VK_MAX_MEMORY_HEAPS) {
        return -EINVAL;
    }
    memset(output, 0, BVB_VULKAN_DISCOVERY_MAX_PAYLOAD);
    struct bvb_wire_writer writer = {
        .output = output,
        .capacity = BVB_VULKAN_DISCOVERY_MAX_PAYLOAD,
    };
    bvb_wire_encode_VkPhysicalDeviceMemoryProperties(&writer, properties);
    return writer_finish(&writer, output_length);
}

int bvb_vulkan_decode_memory_properties(
    const uint8_t *input, uint32_t input_length,
    VkPhysicalDeviceMemoryProperties *properties) {
    if (input == NULL || properties == NULL) {
        return -EINVAL;
    }
    memset(properties, 0, sizeof(*properties));
    struct bvb_wire_reader reader = {.input = input, .length = input_length};
    bvb_wire_decode_VkPhysicalDeviceMemoryProperties(&reader, properties);
    const int result = reader_finish(&reader);
    if (result != 0) {
        return result;
    }
    return properties->memoryTypeCount <= VK_MAX_MEMORY_TYPES &&
                   properties->memoryHeapCount <= VK_MAX_MEMORY_HEAPS
               ? 0
               : -EPROTO;
}

int bvb_vulkan_encode_extension_page(
    uint8_t output[BVB_VULKAN_DISCOVERY_MAX_PAYLOAD],
    const struct bvb_vulkan_extension_page *page, uint32_t *output_length) {
    if (output == NULL || page == NULL || output_length == NULL ||
        page->count > BVB_VULKAN_EXTENSION_PAGE_CAPACITY ||
        page->first > page->total_count ||
        page->count > page->total_count - page->first) {
        return -EINVAL;
    }
    memset(output, 0, BVB_VULKAN_DISCOVERY_MAX_PAYLOAD);
    struct bvb_wire_writer writer = {
        .output = output,
        .capacity = BVB_VULKAN_DISCOVERY_MAX_PAYLOAD,
    };
    bvb_writer_put_u32(&writer, (uint32_t)page->vulkan_result);
    bvb_writer_put_u32(&writer, page->total_count);
    bvb_writer_put_u32(&writer, page->first);
    bvb_writer_put_u32(&writer, page->count);
    for (uint32_t index = 0U; index < page->count; ++index) {
        bvb_wire_encode_VkExtensionProperties(&writer,
                                               &page->properties[index]);
    }
    return writer_finish(&writer, output_length);
}

int bvb_vulkan_decode_extension_page(
    const uint8_t *input, uint32_t input_length,
    struct bvb_vulkan_extension_page *page) {
    if (input == NULL || page == NULL) {
        return -EINVAL;
    }
    memset(page, 0, sizeof(*page));
    struct bvb_wire_reader reader = {.input = input, .length = input_length};
    page->vulkan_result = (int32_t)bvb_reader_get_u32(&reader);
    page->total_count = bvb_reader_get_u32(&reader);
    page->first = bvb_reader_get_u32(&reader);
    page->count = bvb_reader_get_u32(&reader);
    if (page->count > BVB_VULKAN_EXTENSION_PAGE_CAPACITY ||
        page->first > page->total_count ||
        page->count > page->total_count - page->first) {
        return -EPROTO;
    }
    for (uint32_t index = 0U; index < page->count; ++index) {
        bvb_wire_decode_VkExtensionProperties(&reader,
                                               &page->properties[index]);
    }
    return reader_finish(&reader);
}
