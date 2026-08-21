#define _GNU_SOURCE

#include <bvb/protocol.h>
#include <bvb/vulkan_selftest.h>

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
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
static const char BVB_DATAGRAM_BROKER_SOCKET[] =
    "bvb-visible-external-memory-dgram";

static socklen_t abstract_address(struct sockaddr_un *address,
                                  const char *name) {
    memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    const size_t name_length = strlen(name);
    memcpy(address->sun_path + 1, name, name_length);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1U +
                       name_length);
}

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

static int connect_broker(const char *broker_socket) {
    if (broker_socket == NULL || broker_socket[0] == '\0' ||
        strlen(broker_socket) >= sizeof(((struct sockaddr_un *)0)->sun_path) -
                                     1U)
        return -EINVAL;
    int socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket_fd < 0) return -errno;
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    const size_t name_length = strlen(broker_socket);
    memcpy(address.sun_path + 1, broker_socket, name_length);
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

static int open_datagram_broker(struct sockaddr_un *broker_address,
                                socklen_t *broker_address_size,
                                const char *broker_socket) {
    if (broker_address == NULL || broker_address_size == NULL ||
        broker_socket == NULL || broker_socket[0] == '\0' ||
        strlen(broker_socket) >= sizeof(broker_address->sun_path) - 1U)
        return -EINVAL;
    int socket_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (socket_fd < 0) return -errno;
    int enabled = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_PASSCRED, &enabled,
                   sizeof(enabled)) != 0) {
        const int result = -errno;
        (void)close(socket_fd);
        return result;
    }
    char client_name[80];
    const int name_length = snprintf(
        client_name, sizeof(client_name), "bvb-e039-%ld-%" PRId64,
        (long)getpid(), monotonic_ns());
    if (name_length <= 0 || (size_t)name_length >= sizeof(client_name) ||
        (size_t)name_length >= sizeof(((struct sockaddr_un *)0)->sun_path) -
                                   1U) {
        (void)close(socket_fd);
        return -ENAMETOOLONG;
    }
    struct sockaddr_un client_address;
    const socklen_t client_address_size =
        abstract_address(&client_address, client_name);
    if (bind(socket_fd, (const struct sockaddr *)&client_address,
             client_address_size) != 0) {
        const int result = -errno;
        (void)close(socket_fd);
        return result;
    }
    *broker_address_size = abstract_address(
        broker_address, broker_socket);
    return socket_fd;
}

static int send_datagram(int socket_fd, const struct sockaddr_un *address,
                         socklen_t address_size, const uint8_t *bytes,
                         size_t length) {
    ssize_t sent;
    do {
        sent = sendto(socket_fd, bytes, length, MSG_NOSIGNAL,
                      (const struct sockaddr *)address, address_size);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0) return -errno;
    return sent == (ssize_t)length ? 0 : -EIO;
}

static int receive_response(int socket_fd, uint8_t response[BVB_RESPONSE_BYTES],
                            int descriptors[2], size_t *descriptor_count,
                            struct ucred *credentials) {
    struct iovec vector = {
        .iov_base = response,
        .iov_len = BVB_RESPONSE_BYTES,
    };
    uint8_t control[CMSG_SPACE(sizeof(int) * 2U) +
                    CMSG_SPACE(sizeof(struct ucred))] = {0};
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
            (rights->cmsg_type != SCM_RIGHTS &&
             rights->cmsg_type != SCM_CREDENTIALS)) {
            continue;
        }
        if (rights->cmsg_type == SCM_RIGHTS &&
            rights->cmsg_len == CMSG_LEN(sizeof(int) * 2U)) {
            memcpy(descriptors, CMSG_DATA(rights), sizeof(int) * 2U);
            *descriptor_count = 2U;
        } else if (rights->cmsg_type == SCM_CREDENTIALS &&
                   rights->cmsg_len == CMSG_LEN(sizeof(*credentials))) {
            memcpy(credentials, CMSG_DATA(rights), sizeof(*credentials));
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *token = NULL;
    const char *loader_path = BVB_DEFAULT_LOADER;
    const char *broker_socket = BVB_BROKER_SOCKET;
    const char *datagram_broker_socket = BVB_DATAGRAM_BROKER_SOCKET;
    bool datagram = false;
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--token") == 0 && index + 1 < argc) {
            token = argv[++index];
        } else if (strcmp(argv[index], "--loader") == 0 &&
                   index + 1 < argc && argv[index + 1][0] == '/') {
            loader_path = argv[++index];
        } else if (strcmp(argv[index], "--datagram") == 0) {
            datagram = true;
        } else if (strcmp(argv[index], "--socket") == 0 &&
                   index + 1 < argc) {
            broker_socket = argv[++index];
        } else if (strcmp(argv[index], "--datagram-socket") == 0 &&
                   index + 1 < argc) {
            datagram_broker_socket = argv[++index];
        } else {
            fprintf(stderr,
                    "usage: %s --token 64_HEX [--loader ABSOLUTE_PATH] "
                    "[--socket NAME] [--datagram] "
                    "[--datagram-socket NAME]\n",
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
    request[BVB_TOKEN_HEX_BYTES] = datagram ? 'D' : 'P';
    const int64_t started_ns = monotonic_ns();
    struct sockaddr_un broker_address;
    socklen_t broker_address_size = 0;
    int socket_fd = datagram
        ? open_datagram_broker(&broker_address, &broker_address_size,
                               datagram_broker_socket)
        : connect_broker(broker_socket);
    if (socket_fd < 0) {
        fprintf(stderr, "direct broker open failed: %s (%d)\n",
                strerror(-socket_fd), socket_fd);
        return 3;
    }
    struct ucred peer = {
        .pid = -1,
        .uid = (uid_t)-1,
        .gid = (gid_t)-1,
    };
    socklen_t peer_size = sizeof(peer);
    if (!datagram &&
        (getsockopt(socket_fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) != 0 ||
         peer_size != sizeof(peer))) {
        fprintf(stderr, "direct broker peer credentials unavailable\n");
        (void)close(socket_fd);
        return 3;
    }
    int result = datagram
        ? send_datagram(socket_fd, &broker_address, broker_address_size,
                        request, sizeof(request))
        : send_exact(socket_fd, request, sizeof(request));
    uint8_t response[BVB_RESPONSE_BYTES] = {0};
    int descriptors[2] = {-1, -1};
    size_t descriptor_count = 0U;
    if (result == 0) {
        result = receive_response(socket_fd, response, descriptors,
                                  &descriptor_count, &peer);
    }
    const int broker_status = result == 0 ? bvb_wire_get_i32(response) : result;
    if (broker_status != 0 || descriptor_count != 2U) {
        printf("{\"schema_version\":1,\"gate\":\"E039\","
               "\"result\":\"fail\",\"native_status\":%d,"
               "\"transport_status\":%d,\"response_received\":%s,"
               "\"broker_status\":%d,"
               "\"descriptor_count\":%zu,"
               "\"peer_credentials_available\":%s,"
               "\"peer_uid\":%d}\n",
               broker_status, result, result == 0 ? "true" : "false",
               result == 0 ? bvb_wire_get_i32(response) : 0,
               descriptor_count,
               peer.pid >= 0 ? "true" : "false", (int)peer.uid);
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
        result = datagram
            ? send_datagram(socket_fd, &broker_address, broker_address_size,
                            &acknowledgement, sizeof(acknowledgement))
            : send_exact(socket_fd, &acknowledgement,
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
           "\"transport\":\"%s\","
           "\"binder_calls\":0,\"java_calls\":0,"
           "\"channel_acknowledged\":true,"
           "\"peer_credentials_available\":%s,\"peer_uid\":%d,"
           "\"peer_pid\":%d,\"descriptor_count\":2,"
           "\"allocation_size\":%" PRIu64 ","
           "\"memory_type_index\":%" PRIu32 ","
           "\"width\":%" PRIu32 ",\"height\":%" PRIu32 ","
           "\"format\":%" PRIu32 ","
           "\"expected_color\":%" PRIu32 ","
           "\"mismatched_pixels\":%" PRIu32 ","
           "\"gpu_wait_elapsed_ns\":%" PRIu64 ","
           "\"channel_round_trip_ns\":%" PRId64 "}\n",
           datagram ? "direct_native_capability_datagram_scm_rights"
                    : "direct_native_capability_socket_scm_rights",
           peer.pid >= 0 ? "true" : "false", (int)peer.uid, peer.pid,
           allocation_size,
           memory_type_index, image.width, image.height, image.format,
           image.expected_color, image.mismatched_pixels,
           image.gpu_wait_elapsed_ns,
           finished_ns >= started_ns ? finished_ns - started_ns : -1);
    return 0;
}
