#define _GNU_SOURCE

#include <bvb/activity_frame_transport.h>
#include <bvb/protocol.h>

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int validate(const struct bvb_activity_frame_setup *setup) {
    if (setup == NULL || setup->magic != BVB_ACTIVITY_FRAME_SETUP_MAGIC ||
        setup->version != BVB_ACTIVITY_FRAME_SETUP_VERSION ||
        setup->header_bytes != BVB_ACTIVITY_FRAME_SETUP_BYTES ||
        setup->image_count < 2U ||
        setup->image_count > BVB_WSI_FRAME_RING_MAX_SLOTS ||
        setup->width == 0U || setup->height == 0U || setup->format == 0U ||
        setup->image_usage == 0U || setup->flags != 0U ||
        setup->generation == 0U) {
        return -EINVAL;
    }
    for (uint32_t index = 0U; index < BVB_WSI_FRAME_RING_MAX_SLOTS; ++index) {
        if ((index < setup->image_count) !=
            (setup->allocation_sizes[index] != 0U)) return -EINVAL;
    }
    for (size_t index = 0U;
         index < sizeof(setup->reserved) / sizeof(setup->reserved[0]);
         ++index) {
        if (setup->reserved[index] != 0U) return -EINVAL;
    }
    return 0;
}

int bvb_activity_frame_setup_encode(
    uint8_t output[BVB_ACTIVITY_FRAME_SETUP_BYTES],
    const struct bvb_activity_frame_setup *setup) {
    if (output == NULL || validate(setup) != 0) return -EINVAL;
    memset(output, 0, BVB_ACTIVITY_FRAME_SETUP_BYTES);
    bvb_wire_put_u32(output, setup->magic);
    bvb_wire_put_u16(output + 4, setup->version);
    bvb_wire_put_u16(output + 6, setup->header_bytes);
    bvb_wire_put_u32(output + 8, setup->image_count);
    bvb_wire_put_u32(output + 12, setup->width);
    bvb_wire_put_u32(output + 16, setup->height);
    bvb_wire_put_u32(output + 20, setup->format);
    bvb_wire_put_u32(output + 24, setup->image_usage);
    bvb_wire_put_u32(output + 28, setup->flags);
    bvb_wire_put_u64(output + 32, setup->generation);
    for (uint32_t index = 0U; index < BVB_WSI_FRAME_RING_MAX_SLOTS; ++index) {
        bvb_wire_put_u64(output + 40 + index * 8U,
                         setup->allocation_sizes[index]);
        bvb_wire_put_u32(output + 72 + index * 4U,
                         setup->memory_type_indices[index]);
    }
    return 0;
}

int bvb_activity_frame_setup_decode(
    const uint8_t input[BVB_ACTIVITY_FRAME_SETUP_BYTES],
    struct bvb_activity_frame_setup *setup) {
    if (input == NULL || setup == NULL) return -EINVAL;
    struct bvb_activity_frame_setup decoded = {
        .magic = bvb_wire_get_u32(input),
        .version = bvb_wire_get_u16(input + 4),
        .header_bytes = bvb_wire_get_u16(input + 6),
        .image_count = bvb_wire_get_u32(input + 8),
        .width = bvb_wire_get_u32(input + 12),
        .height = bvb_wire_get_u32(input + 16),
        .format = bvb_wire_get_u32(input + 20),
        .image_usage = bvb_wire_get_u32(input + 24),
        .flags = bvb_wire_get_u32(input + 28),
        .generation = bvb_wire_get_u64(input + 32),
    };
    for (uint32_t index = 0U; index < BVB_WSI_FRAME_RING_MAX_SLOTS; ++index) {
        decoded.allocation_sizes[index] =
            bvb_wire_get_u64(input + 40 + index * 8U);
        decoded.memory_type_indices[index] =
            bvb_wire_get_u32(input + 72 + index * 4U);
    }
    for (uint32_t index = 0U; index < 10U; ++index) {
        decoded.reserved[index] = bvb_wire_get_u32(input + 88 + index * 4U);
    }
    if (validate(&decoded) != 0) return -EPROTO;
    *setup = decoded;
    return 0;
}

int bvb_activity_frame_setup_send(
    const char *abstract_socket_name,
    const struct bvb_activity_frame_setup *setup, const int *descriptors,
    size_t descriptor_count) {
    if (abstract_socket_name == NULL || abstract_socket_name[0] == '\0' ||
        setup == NULL || descriptors == NULL ||
        descriptor_count != (size_t)setup->image_count + 1U ||
        descriptor_count > BVB_WSI_FRAME_RING_MAX_SLOTS + 1U) {
        return -EINVAL;
    }
    const size_t name_length = strlen(abstract_socket_name);
    if (name_length > sizeof(((struct sockaddr_un *)0)->sun_path) - 2U) {
        return -ENAMETOOLONG;
    }
    for (size_t index = 0U; index < descriptor_count; ++index) {
        if (descriptors[index] < 0) return -EBADF;
    }
    uint8_t wire[BVB_ACTIVITY_FRAME_SETUP_BYTES];
    int status = bvb_activity_frame_setup_encode(wire, setup);
    int channel = -1;
    if (status == 0) {
        channel = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        status = channel < 0 ? -errno : 0;
    }
    const struct sockaddr_un address = {
        .sun_family = AF_UNIX,
    };
    struct sockaddr_un destination = address;
    memcpy(destination.sun_path + 1, abstract_socket_name, name_length);
    const socklen_t destination_length =
        (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1U + name_length);
    if (status == 0 &&
        connect(channel, (const struct sockaddr *)&destination,
                destination_length) != 0) {
        status = -errno;
    }
    struct ucred peer = {0};
    socklen_t peer_size = sizeof(peer);
    if (status == 0 &&
        (getsockopt(channel, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) != 0 ||
         peer_size != sizeof(peer) || peer.uid != geteuid())) {
        status = errno != 0 ? -errno : -EACCES;
    }
    struct iovec vector = {.iov_base = wire, .iov_len = sizeof(wire)};
    uint8_t control[CMSG_SPACE(
        sizeof(int) * (BVB_WSI_FRAME_RING_MAX_SLOTS + 1U))] = {0};
    struct msghdr message = {
        .msg_iov = &vector,
        .msg_iovlen = 1U,
        .msg_control = control,
        .msg_controllen = CMSG_SPACE(sizeof(int) * descriptor_count),
    };
    if (status == 0) {
        struct cmsghdr *rights = CMSG_FIRSTHDR(&message);
        rights->cmsg_level = SOL_SOCKET;
        rights->cmsg_type = SCM_RIGHTS;
        rights->cmsg_len = CMSG_LEN(sizeof(int) * descriptor_count);
        memcpy(CMSG_DATA(rights), descriptors,
               sizeof(int) * descriptor_count);
        ssize_t count;
        do {
            count = sendmsg(channel, &message, MSG_NOSIGNAL);
        } while (count < 0 && errno == EINTR);
        if (count < 0) {
            status = -errno;
        } else if (count != (ssize_t)sizeof(wire)) {
            status = -EIO;
        }
    }
    if (channel >= 0) (void)close(channel);
    return status;
}
