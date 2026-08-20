#include <bvb/lifecycle.h>
#include <bvb/native_binder.h>
#include <bvb/protocol.h>
#include <bvb/vulkan_selftest.h>

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BVB_DEFAULT_LOADER "/system/lib64/libvulkan.so"

static int64_t monotonic_ns(void) {
    struct timespec timestamp;
    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) return -1;
    return (int64_t)timestamp.tv_sec * INT64_C(1000000000) +
           timestamp.tv_nsec;
}

static int send_exact(int descriptor, const uint8_t *bytes, size_t length) {
    size_t offset = 0U;
    while (offset < length) {
        ssize_t sent = send(descriptor, bytes + offset, length - offset,
                            MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return sent == 0 ? -EPIPE : -errno;
        offset += (size_t)sent;
    }
    return 0;
}

static int receive_exact(int descriptor, uint8_t *bytes, size_t length) {
    size_t offset = 0U;
    while (offset < length) {
        ssize_t count = recv(descriptor, bytes + offset, length - offset, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return count == 0 ? -EPIPE : -errno;
        offset += (size_t)count;
    }
    return 0;
}

static void *client_binder_on_create(void *argument) {
    return argument;
}

static void client_binder_on_destroy(void *user_data) {
    (void)user_data;
}

static binder_status_t client_binder_on_transact(
    AIBinder *binder, transaction_code_t code, const AParcel *input,
    AParcel *output) {
    (void)binder;
    (void)code;
    (void)input;
    (void)output;
    return STATUS_UNKNOWN_TRANSACTION;
}

static void print_failure(const char *stage, int native_status,
                          binder_status_t binder_status) {
    printf("{\"schema_version\":1,\"gate\":\"E040\","
           "\"result\":\"fail\",\"stage\":\"%s\","
           "\"native_status\":%d,\"binder_status\":%d,"
           "\"java_calls\":0}\n",
           stage, native_status, binder_status);
}

int main(int argc, char **argv) {
    const char *token_hex = NULL;
    const char *loader_path = BVB_DEFAULT_LOADER;
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--token") == 0 && index + 1 < argc) {
            token_hex = argv[++index];
        } else if (strcmp(argv[index], "--loader") == 0 &&
                   index + 1 < argc && argv[index + 1][0] == '/') {
            loader_path = argv[++index];
        } else {
            fprintf(stderr,
                    "usage: %s --token 64_HEX [--loader ABSOLUTE_PATH]\n",
                    argv[0]);
            return 2;
        }
    }
    uint8_t token[BVB_LIFECYCLE_TOKEN_SIZE];
    if (token_hex == NULL ||
        bvb_lifecycle_token_from_hex(token_hex, token) != 0) {
        fprintf(stderr, "exactly 64 hexadecimal token characters are required\n");
        return 2;
    }

    const int64_t started_ns = monotonic_ns();
    AIBinder_Class *binder_class = AIBinder_Class_define(
        BVB_NATIVE_BINDER_DESCRIPTOR, client_binder_on_create,
        client_binder_on_destroy, client_binder_on_transact);
    if (binder_class == NULL) {
        print_failure("define_class", -EIO, STATUS_UNKNOWN_ERROR);
        return 3;
    }
    AIBinder *binder = AServiceManager_checkService(
        BVB_NATIVE_BINDER_INSTANCE);
    if (binder == NULL) {
        print_failure("service_lookup", -ENOENT, STATUS_NAME_NOT_FOUND);
        return 3;
    }
    if (!AIBinder_associateClass(binder, binder_class)) {
        print_failure("associate_class", -EPROTO, STATUS_BAD_TYPE);
        AIBinder_decStrong(binder);
        return 3;
    }

    AParcel *input = NULL;
    AParcel *output = NULL;
    binder_status_t binder_status = AIBinder_prepareTransaction(
        binder, &input);
    for (size_t index = 0U;
         index < BVB_NATIVE_BINDER_TOKEN_WORDS && binder_status == STATUS_OK;
         ++index) {
        const uint32_t word =
            (uint32_t)token[index * 4U] |
            (uint32_t)token[index * 4U + 1U] << 8U |
            (uint32_t)token[index * 4U + 2U] << 16U |
            (uint32_t)token[index * 4U + 3U] << 24U;
        binder_status = AParcel_writeInt32(input, (int32_t)word);
    }
    if (binder_status == STATUS_OK) {
        binder_status = AIBinder_transact(
            binder, BVB_NATIVE_BINDER_TRANSACTION_OPEN, &input, &output, 0);
    } else if (input != NULL) {
        AParcel_delete(input);
        input = NULL;
    }
    int32_t native_status = -EIO;
    if (binder_status == STATUS_OK) {
        binder_status = AParcel_readInt32(output, &native_status);
    }
    if (binder_status != STATUS_OK || native_status != 0) {
        print_failure("open_channel", native_status, binder_status);
        if (output != NULL) AParcel_delete(output);
        AIBinder_decStrong(binder);
        return 4;
    }

    int64_t allocation_size = 0;
    int32_t memory_type_index = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t expected_color = 0;
    int memory_fd = -1;
    int semaphore_fd = -1;
    int channel_fd = -1;
#define BVB_READ_PARCEL(call)                                                   \
    do {                                                                        \
        if (binder_status == STATUS_OK) binder_status = (call);                 \
    } while (0)
    BVB_READ_PARCEL(AParcel_readInt64(output, &allocation_size));
    BVB_READ_PARCEL(AParcel_readInt32(output, &memory_type_index));
    BVB_READ_PARCEL(AParcel_readInt32(output, &width));
    BVB_READ_PARCEL(AParcel_readInt32(output, &height));
    BVB_READ_PARCEL(AParcel_readInt32(output, &expected_color));
    BVB_READ_PARCEL(AParcel_readParcelFileDescriptor(output, &memory_fd));
    BVB_READ_PARCEL(AParcel_readParcelFileDescriptor(output, &semaphore_fd));
    BVB_READ_PARCEL(AParcel_readParcelFileDescriptor(output, &channel_fd));
#undef BVB_READ_PARCEL
    AParcel_delete(output);
    AIBinder_decStrong(binder);
    if (binder_status != STATUS_OK || allocation_size <= 0 || width <= 0 ||
        height <= 0 || memory_fd < 0 || semaphore_fd < 0 || channel_fd < 0) {
        print_failure("read_channel", -EPROTO, binder_status);
        if (memory_fd >= 0) (void)close(memory_fd);
        if (semaphore_fd >= 0) (void)close(semaphore_fd);
        if (channel_fd >= 0) (void)close(channel_fd);
        return 4;
    }

    char error[512] = {0};
    struct bvb_vulkan_batch_context *context = NULL;
    struct bvb_vulkan_external_image_result image = {0};
    int result = bvb_vulkan_batch_context_create(
        loader_path, &context, error, sizeof(error));
    if (result == 0) {
        result = bvb_vulkan_batch_context_import_external_image_fds(
            context, memory_fd, semaphore_fd, (uint64_t)allocation_size,
            (uint32_t)memory_type_index, (uint32_t)width, (uint32_t)height,
            UINT32_C(37), (uint32_t)expected_color, &image, error,
            sizeof(error));
        memory_fd = -1;
        semaphore_fd = -1;
    }
    bvb_vulkan_batch_context_destroy(context);
    if (memory_fd >= 0) (void)close(memory_fd);
    if (semaphore_fd >= 0) (void)close(semaphore_fd);
    uint8_t acknowledgement = UINT8_C(0xa5);
    uint8_t channel_response[8] = {0};
    if (result == 0) {
        result = send_exact(channel_fd, &acknowledgement,
                            sizeof(acknowledgement));
    }
    if (result == 0) {
        result = receive_exact(channel_fd, channel_response,
                               sizeof(channel_response));
    }
    (void)close(channel_fd);
    if (result == 0 &&
        (bvb_wire_get_i32(channel_response) != 0 ||
         bvb_wire_get_u32(channel_response + 4) != UINT32_C(0xe040c0de))) {
        result = -EPROTO;
    }
    if (result != 0) {
        fprintf(stderr, "native Binder image channel failed: %s (%d)%s%s\n",
                strerror(-result), result, error[0] == '\0' ? "" : ": ",
                error);
        print_failure("gpu_or_channel", result, STATUS_OK);
        return 4;
    }
    const int64_t finished_ns = monotonic_ns();
    printf("{\"schema_version\":1,\"gate\":\"E040\","
           "\"result\":\"pass\","
           "\"transport\":\"native_binder_setup_then_connected_socket\","
           "\"client_binder_calls_setup\":1,"
           "\"binder_calls_steady_state\":0,\"java_calls\":0,"
           "\"channel_acknowledged\":true,\"descriptor_count\":3,"
           "\"allocation_size\":%" PRId64 ","
           "\"memory_type_index\":%d,\"width\":%" PRIu32 ","
           "\"height\":%" PRIu32 ",\"format\":%" PRIu32 ","
           "\"expected_color\":%" PRIu32 ","
           "\"mismatched_pixels\":%" PRIu32 ","
           "\"gpu_wait_elapsed_ns\":%" PRIu64 ","
           "\"total_elapsed_ns\":%" PRId64 "}\n",
           allocation_size, memory_type_index, image.width, image.height,
           image.format, image.expected_color, image.mismatched_pixels,
           image.gpu_wait_elapsed_ns,
           finished_ns >= started_ns ? finished_ns - started_ns : -1);
    return 0;
}
