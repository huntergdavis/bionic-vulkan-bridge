#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <bvb/transport.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #expression);                                               \
            return EXIT_FAILURE;                                                \
        }                                                                       \
    } while (0)

static int hello_packet(struct bvb_protocol_packet *packet,
                        uint32_t request_id) {
    memset(packet, 0, sizeof(*packet));
    packet->header = (struct bvb_protocol_header){
        .version = BVB_PROTOCOL_VERSION,
        .kind = BVB_PROTOCOL_REQUEST,
        .opcode = BVB_OPCODE_HELLO,
        .request_id = request_id,
        .payload_length = BVB_HELLO_REQUEST_SIZE,
    };
    const struct bvb_hello_request request = {
        .minimum_version = 1U,
        .maximum_version = 1U,
    };
    return bvb_protocol_encode_hello_request(packet->payload, &request);
}

static int test_abstract_descriptor_transport(int memory) {
    uint8_t name[64];
    int length = snprintf((char *)name, sizeof(name), "bvb-test-%ld",
                          (long)getpid());
    CHECK(length > 0 && (size_t)length < sizeof(name));
    CHECK(bvb_transport_listen_abstract(NULL, 1U) == -EINVAL);
    CHECK(bvb_transport_listen_abstract(name, 0U) == -EINVAL);
    uint8_t oversized[sizeof(((struct sockaddr_un *)0)->sun_path)] = {0};
    CHECK(bvb_transport_listen_abstract(oversized, sizeof(oversized)) ==
          -EINVAL);

    int listener =
        bvb_transport_listen_abstract(name, (size_t)length);
    CHECK(listener >= 0);
    int client = bvb_transport_connect_abstract(name, (size_t)length);
    CHECK(client >= 0);
    int server = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    CHECK(server >= 0);
    uid_t peer_uid = (uid_t)-1;
    pid_t peer_pid = 0;
    CHECK(bvb_transport_peer_credentials(server, &peer_uid, &peer_pid) == 0);
    CHECK(peer_uid == getuid());
    CHECK(peer_pid == getpid());
    CHECK(bvb_transport_authenticate(client, getuid(), &peer_pid) == 0);
    CHECK(peer_pid == getpid());

    struct bvb_protocol_packet sent;
    CHECK(hello_packet(&sent, 10U) == 0);
    CHECK(bvb_transport_send_fd(client, &sent, memory) == 0);
    struct bvb_protocol_packet received;
    int received_fd = -1;
    CHECK(bvb_transport_receive_fd(server, &received, &received_fd) == 0);
    CHECK(received.header.request_id == 10U);
    CHECK(received_fd >= 0);
    char readback[13] = {0};
    CHECK(pread(received_fd, readback, sizeof(readback), 0) ==
          (ssize_t)sizeof(readback));
    CHECK(memcmp(readback, "shared-batch", sizeof(readback)) == 0);

    CHECK(close(received_fd) == 0);
    CHECK(close(server) == 0);
    CHECK(close(client) == 0);
    CHECK(close(listener) == 0);
    return 0;
}

int main(void) {
    int sockets[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    int memory =
        (int)syscall(SYS_memfd_create, "bvb-transport-test", MFD_CLOEXEC);
    CHECK(memory >= 0);
    CHECK(ftruncate(memory, 4096) == 0);
    const char marker[] = "shared-batch";
    CHECK(pwrite(memory, marker, sizeof(marker), 0) == (ssize_t)sizeof(marker));

    struct bvb_protocol_packet sent;
    CHECK(hello_packet(&sent, 7U) == 0);
    CHECK(bvb_transport_send_fd(sockets[0], &sent, memory) == 0);
    struct bvb_protocol_packet received;
    int received_fd = -1;
    CHECK(bvb_transport_receive_fd(sockets[1], &received, &received_fd) == 0);
    CHECK(received.header.request_id == 7U);
    CHECK(received_fd >= 0);
    CHECK((fcntl(received_fd, F_GETFD) & FD_CLOEXEC) != 0);
    char readback[sizeof(marker)] = {0};
    CHECK(pread(received_fd, readback, sizeof(readback), 0) ==
          (ssize_t)sizeof(readback));
    CHECK(memcmp(readback, marker, sizeof(marker)) == 0);
    CHECK(close(received_fd) == 0);

    CHECK(hello_packet(&sent, 8U) == 0);
    CHECK(bvb_transport_send(sockets[0], &sent) == 0);
    received_fd = 99;
    CHECK(bvb_transport_receive_fd(sockets[1], &received, &received_fd) == 0);
    CHECK(received.header.request_id == 8U);
    CHECK(received_fd == -1);

    CHECK(hello_packet(&sent, 9U) == 0);
    CHECK(bvb_transport_send_fd(sockets[0], &sent, memory) == 0);
    CHECK(bvb_transport_receive(sockets[1], &received) == -EPROTO);

    CHECK(test_abstract_descriptor_transport(memory) == 0);

    CHECK(close(memory) == 0);
    CHECK(close(sockets[0]) == 0);
    CHECK(close(sockets[1]) == 0);
    puts("PASS: filesystem, abstract, and descriptor Unix transport");
    return EXIT_SUCCESS;
}
