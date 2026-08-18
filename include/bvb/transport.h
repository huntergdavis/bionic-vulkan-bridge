#ifndef BVB_TRANSPORT_H
#define BVB_TRANSPORT_H

#include <bvb/protocol.h>

#include <sys/types.h>

enum {
    BVB_TRANSPORT_EOF = 1,
};

/*
 * The listener creates the final runtime directory at mode 0700 and a socket
 * at mode 0600. Its parent must already exist. Existing socket paths are never
 * removed or replaced.
 */
int bvb_transport_listen(const char *socket_path, uid_t expected_uid);
int bvb_transport_connect(const char *socket_path, uid_t expected_uid);
int bvb_transport_authenticate(int socket_fd, uid_t expected_uid,
                               pid_t *peer_pid);
int bvb_transport_receive(int socket_fd, struct bvb_protocol_packet *packet);
int bvb_transport_send(int socket_fd,
                       const struct bvb_protocol_packet *packet);

#endif

