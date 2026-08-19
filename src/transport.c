#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <bvb/transport.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int runtime_directory(const char *socket_path, char output[PATH_MAX]) {
    if (socket_path == NULL || socket_path[0] != '/') {
        return -EINVAL;
    }
    size_t length = strlen(socket_path);
    if (length == 0U || length >= PATH_MAX ||
        length >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        return -ENAMETOOLONG;
    }
    const char *separator = strrchr(socket_path, '/');
    if (separator == NULL || separator == socket_path || separator[1] == '\0') {
        return -EINVAL;
    }
    size_t directory_length = (size_t)(separator - socket_path);
    memcpy(output, socket_path, directory_length);
    output[directory_length] = '\0';
    return 0;
}

static int validate_runtime_directory(const char *path, uid_t expected_uid) {
    struct stat status;
    if (lstat(path, &status) != 0) {
        return -errno;
    }
    if (!S_ISDIR(status.st_mode)) {
        return -ENOTDIR;
    }
    if (status.st_uid != expected_uid || (status.st_mode & 0777U) != 0700U) {
        return -EPERM;
    }
    return 0;
}

int bvb_transport_listen(const char *socket_path, uid_t expected_uid) {
    char directory[PATH_MAX];
    int result = runtime_directory(socket_path, directory);
    if (result != 0) {
        return result;
    }
    if (mkdir(directory, 0700) != 0 && errno != EEXIST) {
        return -errno;
    }
    result = validate_runtime_directory(directory, expected_uid);
    if (result != 0) {
        return result;
    }

    struct stat existing;
    if (lstat(socket_path, &existing) == 0) {
        return -EADDRINUSE;
    }
    if (errno != ENOENT) {
        return -errno;
    }

    int socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket_fd < 0) {
        return -errno;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    size_t path_length = strlen(socket_path);
    memcpy(address.sun_path, socket_path, path_length + 1U);

    mode_t old_mask = umask(0077);
    int bind_result = bind(socket_fd, (const struct sockaddr *)&address,
                           (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                                       path_length + 1U));
    int bind_error = errno;
    (void)umask(old_mask);
    if (bind_result != 0) {
        (void)close(socket_fd);
        return -bind_error;
    }
    if (chmod(socket_path, 0600) != 0 || listen(socket_fd, 4) != 0) {
        int saved_error = errno;
        (void)close(socket_fd);
        (void)unlink(socket_path);
        return -saved_error;
    }
    return socket_fd;
}

int bvb_transport_connect(const char *socket_path, uid_t expected_uid) {
    char directory[PATH_MAX];
    int result = runtime_directory(socket_path, directory);
    if (result != 0) {
        return result;
    }
    result = validate_runtime_directory(directory, expected_uid);
    if (result != 0) {
        return result;
    }

    struct stat status;
    if (lstat(socket_path, &status) != 0) {
        return -errno;
    }
    if (!S_ISSOCK(status.st_mode) || status.st_uid != expected_uid ||
        (status.st_mode & 0777U) != 0600U) {
        return -EPERM;
    }

    int socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket_fd < 0) {
        return -errno;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    size_t path_length = strlen(socket_path);
    memcpy(address.sun_path, socket_path, path_length + 1U);
    if (connect(socket_fd, (const struct sockaddr *)&address,
                (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                            path_length + 1U)) != 0) {
        int saved_error = errno;
        (void)close(socket_fd);
        return -saved_error;
    }
    pid_t peer_pid = 0;
    result = bvb_transport_authenticate(socket_fd, expected_uid, &peer_pid);
    if (result != 0) {
        (void)close(socket_fd);
        return result;
    }
    return socket_fd;
}

static int abstract_address(const uint8_t *name, size_t name_length,
                            struct sockaddr_un *address,
                            socklen_t *address_length) {
    if (name == NULL || name_length == 0U || address == NULL ||
        address_length == NULL ||
        name_length > sizeof(address->sun_path) - 1U) {
        return -EINVAL;
    }
    memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    memcpy(address->sun_path + 1, name, name_length);
    *address_length =
        (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1U + name_length);
    return 0;
}

int bvb_transport_listen_abstract(const uint8_t *name, size_t name_length) {
    struct sockaddr_un address;
    socklen_t address_length = 0;
    int result =
        abstract_address(name, name_length, &address, &address_length);
    if (result != 0) {
        return result;
    }
    int socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket_fd < 0) {
        return -errno;
    }
    if (bind(socket_fd, (const struct sockaddr *)&address, address_length) !=
            0 ||
        listen(socket_fd, 4) != 0) {
        int saved_error = errno;
        (void)close(socket_fd);
        return -saved_error;
    }
    return socket_fd;
}

int bvb_transport_connect_abstract(const uint8_t *name, size_t name_length) {
    struct sockaddr_un address;
    socklen_t address_length = 0;
    int result =
        abstract_address(name, name_length, &address, &address_length);
    if (result != 0) {
        return result;
    }
    int socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket_fd < 0) {
        return -errno;
    }
    if (connect(socket_fd, (const struct sockaddr *)&address, address_length) !=
        0) {
        int saved_error = errno;
        (void)close(socket_fd);
        return -saved_error;
    }
    return socket_fd;
}

int bvb_transport_peer_credentials(int socket_fd, uid_t *peer_uid,
                                   pid_t *peer_pid) {
    if (socket_fd < 0 || peer_uid == NULL || peer_pid == NULL) {
        return -EINVAL;
    }
    struct ucred credentials;
    socklen_t length = sizeof(credentials);
    if (getsockopt(socket_fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) !=
        0) {
        return -errno;
    }
    if (length != sizeof(credentials) || credentials.pid <= 0) {
        return -EACCES;
    }
    *peer_uid = credentials.uid;
    *peer_pid = credentials.pid;
    return 0;
}

int bvb_transport_authenticate(int socket_fd, uid_t expected_uid,
                               pid_t *peer_pid) {
    if (socket_fd < 0 || peer_pid == NULL) {
        return -EINVAL;
    }
    uid_t peer_uid = 0;
    int result =
        bvb_transport_peer_credentials(socket_fd, &peer_uid, peer_pid);
    if (result != 0) {
        return result;
    }
    if (peer_uid != expected_uid) {
        return -EACCES;
    }
    return 0;
}

static int receive_exact(int socket_fd, uint8_t *output, size_t length,
                         int clean_eof_allowed) {
    size_t offset = 0;
    while (offset < length) {
        ssize_t received = recv(socket_fd, output + offset, length - offset, 0);
        if (received > 0) {
            offset += (size_t)received;
            continue;
        }
        if (received == 0) {
            return clean_eof_allowed != 0 && offset == 0U ? BVB_TRANSPORT_EOF
                                                          : -EPROTO;
        }
        if (errno != EINTR) {
            return -errno;
        }
    }
    return 0;
}

int bvb_transport_receive_fd(int socket_fd, struct bvb_protocol_packet *packet,
                             int *received_fd) {
    if (socket_fd < 0 || packet == NULL || received_fd == NULL) {
        return -EINVAL;
    }
    *received_fd = -1;
    uint8_t wire_header[BVB_PROTOCOL_HEADER_SIZE];
    union {
        struct cmsghdr alignment;
        uint8_t bytes[CMSG_SPACE(sizeof(int) * 2U)];
    } control;
    memset(&control, 0, sizeof(control));
    struct iovec vector = {
        .iov_base = wire_header,
        .iov_len = sizeof(wire_header),
    };
    struct msghdr message = {
        .msg_iov = &vector,
        .msg_iovlen = 1,
        .msg_control = control.bytes,
        .msg_controllen = sizeof(control.bytes),
    };
    ssize_t received;
    do {
        received = recvmsg(socket_fd, &message, MSG_CMSG_CLOEXEC);
    } while (received < 0 && errno == EINTR);
    if (received == 0) {
        return BVB_TRANSPORT_EOF;
    }
    if (received < 0) {
        return -errno;
    }

    int descriptor = -1;
    int ancillary_invalid =
        (message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) != 0;
    for (struct cmsghdr *header = CMSG_FIRSTHDR(&message); header != NULL;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET ||
            header->cmsg_type != SCM_RIGHTS ||
            header->cmsg_len < CMSG_LEN(0U)) {
            ancillary_invalid = 1;
            continue;
        }
        size_t descriptor_bytes = header->cmsg_len - CMSG_LEN(0U);
        size_t descriptor_count = descriptor_bytes / sizeof(int);
        if (descriptor_bytes % sizeof(int) != 0U || descriptor_count != 1U ||
            descriptor >= 0) {
            ancillary_invalid = 1;
            for (size_t index = 0; index < descriptor_count; ++index) {
                int extra = -1;
                memcpy(&extra, (uint8_t *)CMSG_DATA(header) +
                                   index * sizeof(extra),
                       sizeof(extra));
                if (extra >= 0) {
                    (void)close(extra);
                }
            }
            continue;
        }
        memcpy(&descriptor, CMSG_DATA(header), sizeof(descriptor));
    }
    if (ancillary_invalid) {
        if (descriptor >= 0) {
            (void)close(descriptor);
        }
        return -EPROTO;
    }

    size_t header_bytes = (size_t)received;
    if (header_bytes > sizeof(wire_header)) {
        if (descriptor >= 0) {
            (void)close(descriptor);
        }
        return -EPROTO;
    }
    int result = 0;
    if (header_bytes < sizeof(wire_header)) {
        result = receive_exact(socket_fd, wire_header + header_bytes,
                               sizeof(wire_header) - header_bytes, 0);
    }
    if (result == 0) {
        result = bvb_protocol_decode_header(wire_header, &packet->header);
    }
    if (result == 0) {
        result = receive_exact(socket_fd, packet->payload,
                               packet->header.payload_length, 0);
    }
    if (result != 0) {
        if (descriptor >= 0) {
            (void)close(descriptor);
        }
        return result;
    }
    *received_fd = descriptor;
    return 0;
}

int bvb_transport_receive(int socket_fd, struct bvb_protocol_packet *packet) {
    int descriptor = -1;
    int result = bvb_transport_receive_fd(socket_fd, packet, &descriptor);
    if (result != 0) {
        return result;
    }
    if (descriptor >= 0) {
        (void)close(descriptor);
        return -EPROTO;
    }
    return 0;
}

static int send_exact(int socket_fd, const uint8_t *input, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        ssize_t sent = send(socket_fd, input + offset, length - offset,
                            MSG_NOSIGNAL);
        if (sent > 0) {
            offset += (size_t)sent;
            continue;
        }
        if (sent == 0) {
            return -EPIPE;
        }
        if (errno != EINTR) {
            return -errno;
        }
    }
    return 0;
}

int bvb_transport_send(int socket_fd,
                       const struct bvb_protocol_packet *packet) {
    if (socket_fd < 0 || packet == NULL) {
        return -EINVAL;
    }
    uint8_t wire[BVB_PROTOCOL_HEADER_SIZE + BVB_PROTOCOL_MAX_PAYLOAD];
    int result = bvb_protocol_encode_header(wire, &packet->header);
    if (result != 0) {
        return result;
    }
    if (packet->header.payload_length > 0U) {
        memcpy(wire + BVB_PROTOCOL_HEADER_SIZE, packet->payload,
               packet->header.payload_length);
    }
    return send_exact(socket_fd, wire,
                      BVB_PROTOCOL_HEADER_SIZE + packet->header.payload_length);
}

int bvb_transport_send_fd(int socket_fd,
                          const struct bvb_protocol_packet *packet,
                          int passed_fd) {
    if (socket_fd < 0 || packet == NULL || passed_fd < 0 ||
        fcntl(passed_fd, F_GETFD) < 0) {
        return -EINVAL;
    }
    uint8_t wire[BVB_PROTOCOL_HEADER_SIZE + BVB_PROTOCOL_MAX_PAYLOAD];
    int result = bvb_protocol_encode_header(wire, &packet->header);
    if (result != 0) {
        return result;
    }
    if (packet->header.payload_length > 0U) {
        memcpy(wire + BVB_PROTOCOL_HEADER_SIZE, packet->payload,
               packet->header.payload_length);
    }
    size_t wire_length =
        BVB_PROTOCOL_HEADER_SIZE + packet->header.payload_length;
    struct iovec vector = {
        .iov_base = wire,
        .iov_len = wire_length,
    };
    union {
        struct cmsghdr alignment;
        uint8_t bytes[CMSG_SPACE(sizeof(int))];
    } control;
    memset(&control, 0, sizeof(control));
    struct msghdr message = {
        .msg_iov = &vector,
        .msg_iovlen = 1,
        .msg_control = control.bytes,
        .msg_controllen = sizeof(control.bytes),
    };
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    if (header == NULL) {
        return -EINVAL;
    }
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header), &passed_fd, sizeof(passed_fd));
    ssize_t sent;
    do {
        sent = sendmsg(socket_fd, &message, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0) {
        return -errno;
    }
    if (sent == 0) {
        return -EPIPE;
    }
    if ((size_t)sent < wire_length) {
        return send_exact(socket_fd, wire + (size_t)sent,
                          wire_length - (size_t)sent);
    }
    return (size_t)sent == wire_length ? 0 : -EPROTO;
}
