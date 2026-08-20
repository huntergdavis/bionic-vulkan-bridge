#define _GNU_SOURCE

#include <bvb/protocol.h>
#include <bvb/vulkan_selftest.h>

#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define BVB_DEFAULT_LOADER "/system/lib64/libvulkan.so"

enum {
    BVB_TOKEN_HEX_BYTES = 64,
    BVB_REQUEST_BYTES = 65,
    BVB_RESPONSE_BYTES = 24,
};

static const char BVB_BROKER_SOCKET[] = "bvb-visible-external-memory";

static int64_t monotonic_ns(void) {
    struct timespec timestamp;
    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) return -1;
    return (int64_t)timestamp.tv_sec * INT64_C(1000000000) +
           timestamp.tv_nsec;
}

static int send_exact(int socket_fd, const uint8_t *bytes, size_t length) {
    size_t offset = 0U;
    while (offset < length) {
        ssize_t sent = send(socket_fd, bytes + offset, length - offset,
                            MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return sent == 0 ? -EPIPE : -errno;
        offset += (size_t)sent;
    }
    return 0;
}

static int receive_exact(int socket_fd, uint8_t *bytes, size_t length) {
    size_t offset = 0U;
    while (offset < length) {
        ssize_t count = recv(socket_fd, bytes + offset, length - offset, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return count == 0 ? -EPIPE : -errno;
        offset += (size_t)count;
    }
    return 0;
}

static int connect_broker(void) {
    int socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket_fd < 0) return -errno;
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    const size_t name_length = strlen(BVB_BROKER_SOCKET);
    memcpy(address.sun_path + 1, BVB_BROKER_SOCKET, name_length);
    const socklen_t address_size =
        (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1U + name_length);
    if (connect(socket_fd, (const struct sockaddr *)&address,
                address_size) != 0) {
        const int result = -errno;
        (void)close(socket_fd);
        return result;
    }
    return socket_fd;
}

static int receive_response(int socket_fd, uint8_t response[BVB_RESPONSE_BYTES],
                            int descriptors[2], size_t *descriptor_count) {
    struct iovec vector = {
        .iov_base = response,
        .iov_len = BVB_RESPONSE_BYTES,
    };
    uint8_t control[CMSG_SPACE(sizeof(int) * 2U)] = {0};
    struct msghdr message = {
        .msg_iov = &vector,
        .msg_iovlen = 1U,
        .msg_control = control,
        .msg_controllen = sizeof(control),
    };
    ssize_t count;
    do {
        count = recvmsg(socket_fd, &message, MSG_CMSG_CLOEXEC);
    } while (count < 0 && errno == EINTR);
    if (count < 0) return -errno;
    if (count != BVB_RESPONSE_BYTES ||
        (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
        return -EPROTO;
    }
    *descriptor_count = 0U;
    for (struct cmsghdr *rights = CMSG_FIRSTHDR(&message); rights != NULL;
         rights = CMSG_NXTHDR(&message, rights)) {
        if (rights->cmsg_level != SOL_SOCKET ||
            rights->cmsg_type != SCM_RIGHTS ||
            rights->cmsg_len != CMSG_LEN(sizeof(int) * 2U)) {
            continue;
        }
        memcpy(descriptors, CMSG_DATA(rights), sizeof(int) * 2U);
        *descriptor_count = 2U;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *token = NULL;
    const char *loader_path = BVB_DEFAULT_LOADER;
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--token") == 0 && index + 1 < argc) {
            token = argv[++index];
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
    if (token == NULL || strlen(token) != BVB_TOKEN_HEX_BYTES) {
        fprintf(stderr, "exactly 64 token characters are required\n");
        return 2;
    }
    uint8_t request[BVB_REQUEST_BYTES];
    memcpy(request, token, BVB_TOKEN_HEX_BYTES);
    request[BVB_TOKEN_HEX_BYTES] = 'P';
    const int64_t started_ns = monotonic_ns();
    int socket_fd = connect_broker();
    if (socket_fd < 0) {
        fprintf(stderr, "direct broker connect failed: %s (%d)\n",
                strerror(-socket_fd), socket_fd);
        return 3;
    }
    struct ucred peer = {0};
    socklen_t peer_size = sizeof(peer);
    if (getsockopt(socket_fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) != 0 ||
        peer_size != sizeof(peer)) {
        fprintf(stderr, "direct broker peer credentials unavailable\n");
        (void)close(socket_fd);
        return 3;
    }
    int result = send_exact(socket_fd, request, sizeof(request));
    uint8_t response[BVB_RESPONSE_BYTES] = {0};
    int descriptors[2] = {-1, -1};
    size_t descriptor_count = 0U;
    if (result == 0) {
        result = receive_response(socket_fd, response, descriptors,
                                  &descriptor_count);
    }
    const int broker_status = result == 0 ? bvb_wire_get_i32(response) : result;
    if (broker_status != 0 || descriptor_count != 2U) {
        printf("{\"schema_version\":1,\"gate\":\"E039\","
               "\"result\":\"fail\",\"native_status\":%d,"
               "\"descriptor_count\":%zu,\"peer_uid\":%u}\n",
               broker_status, descriptor_count, (unsigned int)peer.uid);
        for (size_t index = 0U; index < 2U; ++index) {
            if (descriptors[index] >= 0) (void)close(descriptors[index]);
        }
        (void)close(socket_fd);
        return 4;
    }
    const uint64_t allocation_size = bvb_wire_get_u64(response + 4);
    const uint32_t memory_type_index = bvb_wire_get_u32(response + 12);
    const uint32_t width = bvb_wire_get_u32(response + 16);
    const uint32_t height = bvb_wire_get_u32(response + 20);
    char error[512] = {0};
    struct bvb_vulkan_batch_context *context = NULL;
    struct bvb_vulkan_external_image_result image = {0};
    result = bvb_vulkan_batch_context_create(
        loader_path, &context, error, sizeof(error));
    if (result == 0) {
        result = bvb_vulkan_batch_context_import_external_image_fds(
            context, descriptors[0], descriptors[1], allocation_size,
            memory_type_index, width, height,
            UINT32_C(37), UINT32_C(0xffff00ff), &image, error,
            sizeof(error));
        descriptors[0] = -1;
        descriptors[1] = -1;
    }
    bvb_vulkan_batch_context_destroy(context);
    for (size_t index = 0U; index < 2U; ++index) {
        if (descriptors[index] >= 0) (void)close(descriptors[index]);
    }
    uint8_t acknowledgement = UINT8_C(0xa5);
    uint8_t channel_response[8] = {0};
    if (result == 0) {
        result = send_exact(socket_fd, &acknowledgement,
                            sizeof(acknowledgement));
    }
    if (result == 0) {
        result = receive_exact(socket_fd, channel_response,
                               sizeof(channel_response));
    }
    (void)close(socket_fd);
    if (result == 0 &&
        (bvb_wire_get_i32(channel_response) != 0 ||
         bvb_wire_get_u32(channel_response + 4) != UINT32_C(0xe039c0de))) {
        result = -EPROTO;
    }
    if (result != 0) {
        fprintf(stderr, "direct external-image channel failed: %s (%d)%s%s\n",
                strerror(-result), result, error[0] == '\0' ? "" : ": ",
                error);
        return 4;
    }
    const int64_t finished_ns = monotonic_ns();
    printf("{\"schema_version\":1,\"gate\":\"E039\","
           "\"result\":\"pass\","
           "\"transport\":\"direct_native_capability_socket_scm_rights\","
           "\"binder_calls\":0,\"java_calls\":0,"
           "\"channel_acknowledged\":true,\"peer_uid\":%u,"
           "\"peer_pid\":%d,\"descriptor_count\":2,"
           "\"allocation_size\":%" PRIu64 ","
           "\"memory_type_index\":%" PRIu32 ","
           "\"width\":%" PRIu32 ",\"height\":%" PRIu32 ","
           "\"format\":%" PRIu32 ","
           "\"expected_color\":%" PRIu32 ","
           "\"mismatched_pixels\":%" PRIu32 ","
           "\"gpu_wait_elapsed_ns\":%" PRIu64 ","
           "\"channel_round_trip_ns\":%" PRId64 "}\n",
           (unsigned int)peer.uid, peer.pid, allocation_size,
           memory_type_index, image.width, image.height, image.format,
           image.expected_color, image.mismatched_pixels,
           image.gpu_wait_elapsed_ns,
           finished_ns >= started_ns ? finished_ns - started_ns : -1);
    return 0;
}
