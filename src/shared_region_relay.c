#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <bvb/protocol.h>
#include <bvb/transport.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum {
    BVB_E021_REGION_BYTES = 4096,
    BVB_E021_REQUEST_ID = 0xE021,
};

static int64_t elapsed_ns(const struct timespec *started,
                          const struct timespec *finished) {
    return (int64_t)(finished->tv_sec - started->tv_sec) * 1000000000LL +
           (int64_t)finished->tv_nsec - (int64_t)started->tv_nsec;
}

static int validate_request(const struct bvb_protocol_packet *request) {
    if (request->header.version != BVB_PROTOCOL_VERSION ||
        request->header.kind != BVB_PROTOCOL_REQUEST ||
        request->header.opcode != BVB_OPCODE_HELLO ||
        request->header.request_id != BVB_E021_REQUEST_ID ||
        request->header.payload_length != BVB_HELLO_REQUEST_SIZE ||
        request->header.status != 0) {
        return -EPROTO;
    }
    struct bvb_hello_request hello;
    int result = bvb_protocol_decode_hello_request(request->payload, &hello);
    if (result != 0 || hello.minimum_version != BVB_PROTOCOL_VERSION ||
        hello.maximum_version != BVB_PROTOCOL_VERSION ||
        hello.client_flags != 0U) {
        return -EPROTO;
    }
    return 0;
}

static int validate_region(int descriptor, int *seals_out) {
    struct stat metadata;
    if (fstat(descriptor, &metadata) != 0) {
        return -errno;
    }
    if (!S_ISREG(metadata.st_mode) ||
        metadata.st_size != BVB_E021_REGION_BYTES) {
        return -EPROTO;
    }
    int seals = fcntl(descriptor, F_GET_SEALS);
    if (seals < 0) {
        return -errno;
    }
    const int required_seals = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    if ((seals & required_seals) != required_seals ||
        (seals & F_SEAL_WRITE) != 0) {
        return -EPROTO;
    }
    int descriptor_flags = fcntl(descriptor, F_GETFL);
    if (descriptor_flags < 0) {
        return -errno;
    }
    if ((descriptor_flags & O_ACCMODE) != O_RDWR) {
        return -EACCES;
    }
    uint8_t *mapping = mmap(NULL, BVB_E021_REGION_BYTES,
                            PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
    if (mapping == MAP_FAILED) {
        return -errno;
    }
    static const char marker[] =
        "BVB_E020_SHARED_REGION binder_parcel_fd=PASS\n";
    int result = memcmp(mapping, marker, sizeof(marker) - 1U) == 0
                     ? 0
                     : -EBADMSG;
    if (result == 0) {
        mapping[BVB_E021_REGION_BYTES - 1U] = 0x21U;
        if (mapping[BVB_E021_REGION_BYTES - 1U] != 0x21U) {
            result = -EIO;
        }
    }
    if (munmap(mapping, BVB_E021_REGION_BYTES) != 0 && result == 0) {
        result = -errno;
    }
    if (result == 0) {
        *seals_out = seals;
    }
    return result;
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
    if (argc != 3 || strcmp(argv[1], "--socket") != 0 || argv[2][0] == '\0') {
        fprintf(stderr, "usage: %s --socket ABSTRACT_NAME\n", argv[0]);
        return EXIT_FAILURE;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    const uint8_t *socket_name = (const uint8_t *)argv[2];
    size_t socket_name_length = strlen(argv[2]);
    int listener =
        bvb_transport_listen_abstract(socket_name, socket_name_length);
    if (listener < 0) {
        fprintf(stderr, "listen failed: %s (%d)\n", strerror(-listener),
                listener);
        return EXIT_FAILURE;
    }
    printf("bvb-shared-region-relay: ready socket=%s\n", argv[2]);

    int connection = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    if (connection < 0) {
        fprintf(stderr, "accept failed: %s\n", strerror(errno));
        (void)close(listener);
        return EXIT_FAILURE;
    }
    uid_t peer_uid = (uid_t)-1;
    pid_t peer_pid = 0;
    int result = bvb_transport_peer_credentials(connection, &peer_uid,
                                                &peer_pid);
    if (result == 0 && peer_uid != getuid()) {
        result = -EACCES;
    }
    struct timespec started;
    struct timespec finished;
    (void)clock_gettime(CLOCK_MONOTONIC, &started);
    struct bvb_protocol_packet request;
    memset(&request, 0, sizeof(request));
    int region_fd = -1;
    if (result == 0) {
        result = bvb_transport_receive_fd(connection, &request, &region_fd);
    }
    if (result == 0) {
        result = validate_request(&request);
    }
    int seals = 0;
    if (result == 0) {
        result = validate_region(region_fd, &seals);
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &finished);
    int response_result = region_fd >= 0
                              ? send_response(connection, &request, result)
                              : 0;
    if (region_fd >= 0) {
        (void)close(region_fd);
    }
    (void)close(connection);
    (void)close(listener);
    if (result != 0 || response_result != 0) {
        int failure = result != 0 ? result : response_result;
        fprintf(stderr, "relay validation failed: %s (%d)\n",
                strerror(-failure), failure);
        return EXIT_FAILURE;
    }
    printf("{\"result\":\"pass\","
           "\"transport\":\"binder_then_same_uid_scm_rights\","
           "\"peer_uid\":%lu,\"peer_pid\":%ld,"
           "\"region_bytes\":%u,\"seals\":%d,"
           "\"writable_mapping\":true,"
           "\"receive_validate_ns\":%" PRId64 "}\n",
           (unsigned long)peer_uid, (long)peer_pid, BVB_E021_REGION_BYTES,
           seals, elapsed_ns(&started, &finished));
    return EXIT_SUCCESS;
}
