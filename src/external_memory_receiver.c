#define _GNU_SOURCE

#include <bvb/protocol.h>
#include <bvb/transport.h>
#include <bvb/vulkan_selftest.h>

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

static int send_response(int socket_fd,
                         const struct bvb_protocol_packet *request,
                         int status) {
    struct bvb_protocol_packet response;
    memset(&response, 0, sizeof(response));
    response.header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_RESPONSE,
        .opcode = request->header.opcode,
        .request_id = request->header.request_id,
        .status = status,
    };
    return bvb_transport_send(socket_fd, &response);
}

int main(int argc, char **argv) {
    const char *socket_name = NULL;
    const char *loader_path = BVB_DEFAULT_LOADER;
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--socket") == 0 && index + 1 < argc &&
            argv[index + 1][0] != '\0') {
            socket_name = argv[++index];
        } else if (strcmp(argv[index], "--loader") == 0 &&
                   index + 1 < argc && argv[index + 1][0] == '/') {
            loader_path = argv[++index];
        } else {
            fprintf(stderr,
                    "usage: %s --socket ABSTRACT_NAME "
                    "[--loader ABSOLUTE_PATH]\n",
                    argv[0]);
            return 2;
        }
    }
    if (socket_name == NULL) {
        fprintf(stderr,
                "usage: %s --socket ABSTRACT_NAME "
                "[--loader ABSOLUTE_PATH]\n",
                argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    int listener = bvb_transport_listen_abstract(
        (const uint8_t *)socket_name, strlen(socket_name));
    if (listener < 0) {
        fprintf(stderr, "listen failed: %s (%d)\n", strerror(-listener),
                listener);
        return 3;
    }
    printf("bvb-external-memory-receiver: ready socket=%s\n", socket_name);
    int connection = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    if (connection < 0) {
        fprintf(stderr, "accept failed: %s\n", strerror(errno));
        (void)close(listener);
        return 3;
    }

    const int64_t started_ns = monotonic_ns();
    uid_t peer_uid = (uid_t)-1;
    pid_t peer_pid = 0;
    int result = bvb_transport_peer_credentials(connection, &peer_uid,
                                                &peer_pid);
    if (result == 0 && peer_uid != getuid()) result = -EACCES;
    struct bvb_protocol_packet request;
    memset(&request, 0, sizeof(request));
    int external_fds[2] = {-1, -1};
    size_t external_fd_count = 0U;
    if (result == 0) {
        result = bvb_transport_receive_fds(
            connection, &request, external_fds, 2U, &external_fd_count);
    }
    struct bvb_external_memory_import_request import_request = {0};
    struct bvb_external_sync_import_request sync_request = {0};
    struct bvb_external_image_import_request image_request = {0};
    const bool external_image =
        request.header.opcode == BVB_OPCODE_EXTERNAL_IMAGE_IMPORT_TEST;
    const bool synchronized = external_image ||
        request.header.opcode == BVB_OPCODE_EXTERNAL_SYNC_IMPORT_TEST;
    if (result == 0 &&
        (request.header.version != BVB_PROTOCOL_VERSION ||
         request.header.kind != BVB_PROTOCOL_REQUEST ||
         (request.header.opcode != BVB_OPCODE_EXTERNAL_MEMORY_IMPORT_TEST &&
          request.header.opcode != BVB_OPCODE_EXTERNAL_SYNC_IMPORT_TEST &&
          request.header.opcode != BVB_OPCODE_EXTERNAL_IMAGE_IMPORT_TEST) ||
         request.header.payload_length !=
             (external_image ? BVB_EXTERNAL_IMAGE_IMPORT_REQUEST_SIZE
              : synchronized ? BVB_EXTERNAL_SYNC_IMPORT_REQUEST_SIZE
                           : BVB_EXTERNAL_MEMORY_IMPORT_REQUEST_SIZE) ||
         external_fd_count != (synchronized ? 2U : 1U) ||
         request.header.status != 0)) {
        result = -EPROTO;
    }
    if (result == 0 && external_image) {
        result = bvb_protocol_decode_external_image_import_request(
            request.payload, &image_request);
    } else if (result == 0 && synchronized) {
        result = bvb_protocol_decode_external_sync_import_request(
            request.payload, &sync_request);
    } else if (result == 0) {
        result = bvb_protocol_decode_external_memory_import_request(
            request.payload, &import_request);
    }

    char error[512] = {0};
    struct bvb_vulkan_batch_context *context = NULL;
    struct bvb_vulkan_external_memory_result import_result = {0};
    struct bvb_vulkan_external_sync_result sync_result = {0};
    struct bvb_vulkan_external_image_result image_result = {0};
    if (result == 0) {
        result = bvb_vulkan_batch_context_create(
            loader_path, &context, error, sizeof(error));
    }
    if (result == 0) {
        if (external_image) {
            result = bvb_vulkan_batch_context_import_external_image_fds(
                context, external_fds[0], external_fds[1],
                image_request.allocation_size,
                image_request.memory_type_index, image_request.width,
                image_request.height, image_request.format,
                image_request.expected_color, &image_result, error,
                sizeof(error));
            external_fds[0] = -1;
            external_fds[1] = -1;
        } else if (synchronized) {
            result = bvb_vulkan_batch_context_import_external_sync_fds(
                context, external_fds[0], external_fds[1],
                sync_request.allocation_size,
                sync_request.memory_type_index, sync_request.buffer_bytes,
                sync_request.expected_fill_word, &sync_result, error,
                sizeof(error));
            external_fds[0] = -1;
            external_fds[1] = -1;
        } else {
            result = bvb_vulkan_batch_context_import_external_memory_fd(
                context, external_fds[0], import_request.allocation_size,
                import_request.memory_type_index, import_request.buffer_bytes,
                &import_result, error, sizeof(error));
            external_fds[0] = -1;
        }
    }
    for (size_t index = 0U; index < 2U; ++index) {
        if (external_fds[index] >= 0) (void)close(external_fds[index]);
    }
    bvb_vulkan_batch_context_destroy(context);

    const int response_result = send_response(connection, &request, result);
    (void)close(connection);
    (void)close(listener);
    if (result != 0 || response_result != 0) {
        const int failure = result != 0 ? result : response_result;
        fprintf(stderr, "external-memory receive/import failed: %s (%d)%s%s\n",
                strerror(-failure), failure, error[0] == '\0' ? "" : ": ",
                error);
        return 4;
    }
    const int64_t finished_ns = monotonic_ns();
    if (external_image) {
        printf("{\"schema_version\":1,\"gate\":\"E038\","
               "\"result\":\"pass\","
               "\"transport\":\"binder_then_scm_rights_opaque_image_plus_sync_fd\","
               "\"loader_path\":\"%s\",\"peer_uid\":%lu,"
               "\"peer_pid\":%ld,\"descriptor_count\":2,"
               "\"allocation_size\":%" PRIu64 ","
               "\"memory_type_index\":%" PRIu32 ","
               "\"width\":%" PRIu32 ",\"height\":%" PRIu32 ","
               "\"format\":%" PRIu32 ","
               "\"expected_color\":%" PRIu32 ","
               "\"mismatched_pixels\":%" PRIu32 ","
               "\"readback_memory_property_flags\":%" PRIu32 ","
               "\"gpu_wait_elapsed_ns\":%" PRIu64 ","
               "\"receive_import_ns\":%" PRId64 "}\n",
               loader_path, (unsigned long)peer_uid, (long)peer_pid,
               image_request.allocation_size,
               image_request.memory_type_index, image_result.width,
               image_result.height, image_result.format,
               image_result.expected_color, image_result.mismatched_pixels,
               image_result.readback_memory_property_flags,
               image_result.gpu_wait_elapsed_ns,
               finished_ns >= started_ns ? finished_ns - started_ns : -1);
    } else if (synchronized) {
        printf("{\"schema_version\":1,\"gate\":\"E037\","
               "\"result\":\"pass\","
               "\"transport\":\"binder_then_scm_rights_opaque_memory_plus_sync_fd\","
               "\"loader_path\":\"%s\",\"peer_uid\":%lu,"
               "\"peer_pid\":%ld,\"descriptor_count\":2,"
               "\"allocation_size\":%" PRIu64 ","
               "\"memory_type_index\":%" PRIu32 ","
               "\"memory_property_flags\":%" PRIu32 ","
               "\"buffer_bytes\":%" PRIu32 ","
               "\"expected_fill_word\":%" PRIu32 ","
               "\"mismatched_words\":%" PRIu32 ","
               "\"gpu_wait_elapsed_ns\":%" PRIu64 ","
               "\"receive_import_ns\":%" PRId64 "}\n",
               loader_path, (unsigned long)peer_uid, (long)peer_pid,
               sync_request.allocation_size, sync_request.memory_type_index,
               sync_result.memory_property_flags, sync_result.buffer_bytes,
               sync_result.expected_fill_word, sync_result.mismatched_words,
               sync_result.gpu_wait_elapsed_ns,
               finished_ns >= started_ns ? finished_ns - started_ns : -1);
    } else {
        printf("{\"schema_version\":1,\"gate\":\"E036\","
               "\"result\":\"pass\","
               "\"transport\":\"binder_then_scm_rights_opaque_fd\","
               "\"loader_path\":\"%s\",\"peer_uid\":%lu,"
               "\"peer_pid\":%ld,\"allocation_size\":%" PRIu64 ","
               "\"memory_type_index\":%" PRIu32 ","
               "\"memory_property_flags\":%" PRIu32 ","
               "\"buffer_bytes\":%" PRIu32 ","
               "\"mismatched_bytes\":%" PRIu32 ","
               "\"receive_import_ns\":%" PRId64 "}\n",
               loader_path, (unsigned long)peer_uid, (long)peer_pid,
               import_request.allocation_size,
               import_result.memory_type_index,
               import_result.memory_property_flags,
               import_result.buffer_bytes, import_result.mismatched_bytes,
               finished_ns >= started_ns ? finished_ns - started_ns : -1);
    }
    return 0;
}
