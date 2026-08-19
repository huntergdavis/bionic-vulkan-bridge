#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <bvb/protocol.h>
#include <bvb/transport.h>
#include <bvb/visible_batch.h>
#include <bvb/visible_ingress.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

enum {
    BVB_VISIBLE_SOCKET_TIMEOUT_SECONDS = 10,
    BVB_VISIBLE_ABSTRACT_NAME_MAX = 107,
};

enum bvb_visible_ingress_transport {
    BVB_VISIBLE_INGRESS_ABSTRACT = 1,
    BVB_VISIBLE_INGRESS_LOOPBACK = 2,
};

struct bvb_visible_ingress {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t worker;
    int listener_fd;
    int client_fd;
    bool stop;
    bool accepting;
    bool batch_ready;
    bool batch_claimed;
    bool batch_completed;
    bool completion_pending_response;
    bool brokered_region_ready;
    int batch_status;
    const uint8_t *batch;
    size_t batch_length;
    uint64_t sequence;
    enum bvb_visible_ingress_transport transport;
    uint16_t loopback_port;
    size_t socket_name_length;
    uint8_t socket_name[BVB_VISIBLE_ABSTRACT_NAME_MAX];
    uint8_t token[BVB_LIFECYCLE_TOKEN_SIZE];
    struct bvb_visible_batch_region brokered_region;
};

static bool token_is_nonzero(
    const uint8_t token[BVB_LIFECYCLE_TOKEN_SIZE]) {
    uint8_t combined = 0U;
    for (size_t index = 0U; index < BVB_LIFECYCLE_TOKEN_SIZE; ++index) {
        combined |= token[index];
    }
    return combined != 0U;
}

static int request_status(const struct bvb_protocol_packet *request,
                          uint16_t opcode, uint32_t payload_length) {
    if (request->header.version != BVB_PROTOCOL_VERSION ||
        request->header.kind != BVB_PROTOCOL_REQUEST ||
        request->header.opcode != opcode || request->header.status != 0 ||
        request->header.payload_length != payload_length) {
        return -EPROTO;
    }
    return 0;
}

static int send_response(int socket_fd,
                         const struct bvb_protocol_packet *request,
                         int status) {
    if (status > 0) {
        status = -EPROTO;
    }
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

static int configure_client_socket(int socket_fd) {
    const struct timeval timeout = {
        .tv_sec = BVB_VISIBLE_SOCKET_TIMEOUT_SECONDS,
    };
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) != 0 ||
        setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                   sizeof(timeout)) != 0) {
        return -errno;
    }
    return 0;
}

static int submit_batch(struct bvb_visible_ingress *ingress,
                        const uint8_t *batch, size_t batch_length,
                        uint64_t sequence) {
    (void)pthread_mutex_lock(&ingress->mutex);
    if (ingress->stop) {
        (void)pthread_mutex_unlock(&ingress->mutex);
        return -ECANCELED;
    }
    if (!ingress->accepting) {
        (void)pthread_mutex_unlock(&ingress->mutex);
        return -EAGAIN;
    }
    if (ingress->batch_ready) {
        (void)pthread_mutex_unlock(&ingress->mutex);
        return -EBUSY;
    }
    ingress->batch = batch;
    ingress->batch_length = batch_length;
    ingress->sequence = sequence;
    ingress->batch_ready = true;
    ingress->batch_claimed = false;
    ingress->batch_completed = false;
    ingress->batch_status = 0;
    (void)pthread_cond_broadcast(&ingress->condition);
    while (!ingress->batch_completed && !ingress->stop) {
        (void)pthread_cond_wait(&ingress->condition, &ingress->mutex);
    }
    int result = ingress->stop ? -ECANCELED : ingress->batch_status;
    ingress->batch = NULL;
    ingress->batch_length = 0U;
    ingress->sequence = 0U;
    ingress->batch_ready = false;
    ingress->batch_claimed = false;
    ingress->batch_completed = false;
    ingress->batch_status = 0;
    (void)pthread_cond_broadcast(&ingress->condition);
    (void)pthread_mutex_unlock(&ingress->mutex);
    return result;
}

static void process_shared_connection(struct bvb_visible_ingress *ingress,
                                      int socket_fd) {
    struct bvb_visible_batch_region region;
    int result = bvb_visible_batch_region_init(&region, ingress->token);
    if (result != 0) {
        return;
    }

    struct bvb_protocol_packet request;
    memset(&request, 0, sizeof(request));
    int memory_fd = -1;
    result = bvb_transport_receive_fd(socket_fd, &request, &memory_fd);
    bool request_received = result == 0;
    if (result == 0) {
        result = request_status(&request, BVB_OPCODE_VISIBLE_BATCH_SETUP,
                                BVB_VISIBLE_BATCH_SETUP_SIZE);
    }
    if (result == 0) {
        result = bvb_visible_batch_region_setup(
            &region, request.payload, request.header.payload_length,
            memory_fd);
    }
    if (memory_fd >= 0) {
        (void)close(memory_fd);
    }
    if (result != 0) {
        if (request_received &&
            request.header.opcode == BVB_OPCODE_VISIBLE_BATCH_SETUP) {
            (void)send_response(socket_fd, &request, result);
        }
        bvb_visible_batch_region_destroy(&region);
        return;
    }
    result = send_response(socket_fd, &request, 0);
    if (result != 0) {
        bvb_visible_batch_region_destroy(&region);
        return;
    }

    memset(&request, 0, sizeof(request));
    result = bvb_transport_receive(socket_fd, &request);
    request_received = result == 0;
    if (result == 0) {
        result = request_status(&request, BVB_OPCODE_VISIBLE_BATCH_EXECUTE,
                                BVB_VISIBLE_BATCH_EXECUTE_SIZE);
    }
    const uint8_t *batch = NULL;
    size_t batch_length = 0U;
    uint64_t sequence = 0U;
    if (result == 0) {
        result = bvb_visible_batch_region_execute(
            &region, request.payload, request.header.payload_length, &batch,
            &batch_length, &sequence);
    }
    if (result == 0) {
        result = submit_batch(ingress, batch, batch_length, sequence);
    }
    if (request_received &&
        request.header.opcode == BVB_OPCODE_VISIBLE_BATCH_EXECUTE) {
        (void)send_response(socket_fd, &request, result);
    }
    bvb_visible_batch_region_destroy(&region);
}

static void process_inline_connection(struct bvb_visible_ingress *ingress,
                                      int socket_fd) {
    for (;;) {
        struct bvb_protocol_packet request;
        memset(&request, 0, sizeof(request));
        int result = bvb_transport_receive(socket_fd, &request);
        if (result != 0) {
            return;
        }
        const uint8_t *batch = NULL;
        size_t batch_length = 0U;
        uint64_t sequence = 0U;
        if (request.header.opcode == BVB_OPCODE_VISIBLE_BATCH_INLINE) {
            if (request.header.version != BVB_PROTOCOL_VERSION ||
                request.header.kind != BVB_PROTOCOL_REQUEST ||
                request.header.status != 0) {
                result = -EPROTO;
            }
        } else if (request.header.opcode ==
                   BVB_OPCODE_VISIBLE_BATCH_EXECUTE) {
            result = request_status(&request,
                                    BVB_OPCODE_VISIBLE_BATCH_EXECUTE,
                                    BVB_VISIBLE_BATCH_EXECUTE_SIZE);
        } else {
            result = -EPROTO;
        }
        if (result == 0 &&
            request.header.opcode == BVB_OPCODE_VISIBLE_BATCH_INLINE) {
            result = bvb_visible_batch_inline_decode(
                ingress->token, request.payload,
                request.header.payload_length, &batch, &batch_length,
                &sequence);
        } else if (result == 0) {
            (void)pthread_mutex_lock(&ingress->mutex);
            result = ingress->brokered_region_ready
                         ? bvb_visible_batch_region_execute(
                               &ingress->brokered_region, request.payload,
                               request.header.payload_length, &batch,
                               &batch_length, &sequence)
                         : -ENXIO;
            (void)pthread_mutex_unlock(&ingress->mutex);
        }
        if (result == 0) {
            result = submit_batch(ingress, batch, batch_length, sequence);
        }
        const bool supported =
            request.header.opcode == BVB_OPCODE_VISIBLE_BATCH_INLINE ||
            request.header.opcode == BVB_OPCODE_VISIBLE_BATCH_EXECUTE;
        const int response_result =
            supported ? send_response(socket_fd, &request, result) : -EPROTO;
        (void)pthread_mutex_lock(&ingress->mutex);
        ingress->completion_pending_response = false;
        (void)pthread_cond_broadcast(&ingress->condition);
        (void)pthread_mutex_unlock(&ingress->mutex);
        if (!supported || response_result != 0 || result != 0) {
            return;
        }
    }
}

static void *worker_main(void *opaque) {
    struct bvb_visible_ingress *ingress = opaque;
    for (;;) {
        int socket_fd;
        do {
            socket_fd = accept4(ingress->listener_fd, NULL, NULL,
                                SOCK_CLOEXEC);
        } while (socket_fd < 0 && errno == EINTR);
        if (socket_fd < 0) {
            (void)pthread_mutex_lock(&ingress->mutex);
            bool stopping = ingress->stop;
            (void)pthread_mutex_unlock(&ingress->mutex);
            if (stopping) {
                break;
            }
            continue;
        }

        (void)pthread_mutex_lock(&ingress->mutex);
        if (ingress->stop) {
            (void)pthread_mutex_unlock(&ingress->mutex);
            (void)close(socket_fd);
            break;
        }
        ingress->client_fd = socket_fd;
        (void)pthread_mutex_unlock(&ingress->mutex);

        if (configure_client_socket(socket_fd) == 0) {
            if (ingress->transport == BVB_VISIBLE_INGRESS_LOOPBACK) {
                process_inline_connection(ingress, socket_fd);
            } else {
                process_shared_connection(ingress, socket_fd);
            }
        }

        (void)pthread_mutex_lock(&ingress->mutex);
        ingress->client_fd = -1;
        ingress->completion_pending_response = false;
        bool stopping = ingress->stop;
        (void)pthread_cond_broadcast(&ingress->condition);
        (void)pthread_mutex_unlock(&ingress->mutex);
        (void)close(socket_fd);
        if (stopping) {
            break;
        }
    }
    return NULL;
}

int bvb_visible_ingress_create(
    struct bvb_visible_ingress **output, const uint8_t *socket_name,
    size_t socket_name_length,
    const uint8_t token[BVB_LIFECYCLE_TOKEN_SIZE]) {
    if (output == NULL || socket_name == NULL || socket_name_length == 0U ||
        socket_name_length > BVB_VISIBLE_ABSTRACT_NAME_MAX || token == NULL ||
        !token_is_nonzero(token)) {
        return -EINVAL;
    }
    *output = NULL;
    struct bvb_visible_ingress *ingress = calloc(1U, sizeof(*ingress));
    if (ingress == NULL) {
        return -ENOMEM;
    }
    ingress->listener_fd = -1;
    ingress->client_fd = -1;
    ingress->accepting = true;
    ingress->transport = BVB_VISIBLE_INGRESS_ABSTRACT;
    ingress->socket_name_length = socket_name_length;
    memcpy(ingress->socket_name, socket_name, socket_name_length);
    memcpy(ingress->token, token, sizeof(ingress->token));
    int result = pthread_mutex_init(&ingress->mutex, NULL);
    if (result != 0) {
        free(ingress);
        return -result;
    }
    result = pthread_cond_init(&ingress->condition, NULL);
    if (result != 0) {
        (void)pthread_mutex_destroy(&ingress->mutex);
        free(ingress);
        return -result;
    }
    ingress->listener_fd =
        bvb_transport_listen_abstract(socket_name, socket_name_length);
    if (ingress->listener_fd < 0) {
        result = ingress->listener_fd;
        (void)pthread_cond_destroy(&ingress->condition);
        (void)pthread_mutex_destroy(&ingress->mutex);
        free(ingress);
        return result;
    }
    result = pthread_create(&ingress->worker, NULL, worker_main, ingress);
    if (result != 0) {
        (void)close(ingress->listener_fd);
        (void)pthread_cond_destroy(&ingress->condition);
        (void)pthread_mutex_destroy(&ingress->mutex);
        free(ingress);
        return -result;
    }
    *output = ingress;
    return 0;
}

int bvb_visible_ingress_create_loopback(
    struct bvb_visible_ingress **output, uint16_t requested_port,
    uint16_t *bound_port,
    const uint8_t token[BVB_LIFECYCLE_TOKEN_SIZE]) {
    if (output == NULL || bound_port == NULL || token == NULL ||
        !token_is_nonzero(token)) {
        return -EINVAL;
    }
    *output = NULL;
    *bound_port = 0U;
    struct bvb_visible_ingress *ingress = calloc(1U, sizeof(*ingress));
    if (ingress == NULL) {
        return -ENOMEM;
    }
    ingress->listener_fd = -1;
    ingress->client_fd = -1;
    ingress->accepting = true;
    ingress->transport = BVB_VISIBLE_INGRESS_LOOPBACK;
    memcpy(ingress->token, token, sizeof(ingress->token));
    int result = pthread_mutex_init(&ingress->mutex, NULL);
    if (result != 0) {
        free(ingress);
        return -result;
    }
    result = pthread_cond_init(&ingress->condition, NULL);
    if (result != 0) {
        (void)pthread_mutex_destroy(&ingress->mutex);
        free(ingress);
        return -result;
    }
    ingress->listener_fd =
        bvb_transport_listen_loopback(requested_port, &ingress->loopback_port);
    if (ingress->listener_fd < 0) {
        result = ingress->listener_fd;
        (void)pthread_cond_destroy(&ingress->condition);
        (void)pthread_mutex_destroy(&ingress->mutex);
        free(ingress);
        return result;
    }
    result = pthread_create(&ingress->worker, NULL, worker_main, ingress);
    if (result != 0) {
        (void)close(ingress->listener_fd);
        (void)pthread_cond_destroy(&ingress->condition);
        (void)pthread_mutex_destroy(&ingress->mutex);
        free(ingress);
        return -result;
    }
    *bound_port = ingress->loopback_port;
    *output = ingress;
    return 0;
}

int bvb_visible_ingress_install_region(struct bvb_visible_ingress *ingress,
                                       int memory_fd, size_t region_bytes,
                                       uint64_t generation) {
    if (ingress == NULL || memory_fd < 0 || region_bytes == 0U ||
        generation == 0U) {
        return -EINVAL;
    }
    if (ingress->transport != BVB_VISIBLE_INGRESS_LOOPBACK) {
        return -EOPNOTSUPP;
    }
    struct bvb_visible_batch_region candidate;
    int result = bvb_visible_batch_region_init(&candidate, ingress->token);
    if (result != 0) {
        return result;
    }
    struct bvb_visible_batch_setup setup = {
        .shared = {
            .region_bytes = region_bytes,
            .generation = generation,
        },
    };
    memcpy(setup.token, ingress->token, sizeof(setup.token));
    uint8_t payload[BVB_VISIBLE_BATCH_SETUP_SIZE];
    result = bvb_protocol_encode_visible_batch_setup(payload, &setup);
    if (result == 0) {
        result = bvb_visible_batch_region_setup(
            &candidate, payload, sizeof(payload), memory_fd);
    }
    if (result != 0) {
        bvb_visible_batch_region_destroy(&candidate);
        return result;
    }

    (void)pthread_mutex_lock(&ingress->mutex);
    if (ingress->stop) {
        result = -ECANCELED;
    } else if (ingress->brokered_region_ready) {
        result = -EALREADY;
    } else {
        ingress->brokered_region = candidate;
        memset(&candidate, 0, sizeof(candidate));
        ingress->brokered_region_ready = true;
    }
    (void)pthread_mutex_unlock(&ingress->mutex);
    bvb_visible_batch_region_destroy(&candidate);
    return result;
}

static int realtime_deadline(uint32_t timeout_ms, struct timespec *deadline) {
    if (clock_gettime(CLOCK_REALTIME, deadline) != 0) {
        return -errno;
    }
    deadline->tv_sec += (time_t)(timeout_ms / 1000U);
    deadline->tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec += 1;
        deadline->tv_nsec -= 1000000000L;
    }
    return 0;
}

int bvb_visible_ingress_wait_batch(struct bvb_visible_ingress *ingress,
                                   uint32_t timeout_ms,
                                   const uint8_t **batch,
                                   size_t *batch_length, uint64_t *sequence) {
    if (ingress == NULL || timeout_ms == 0U || batch == NULL ||
        batch_length == NULL || sequence == NULL) {
        return -EINVAL;
    }
    struct timespec deadline;
    int result = realtime_deadline(timeout_ms, &deadline);
    if (result != 0) {
        return result;
    }
    (void)pthread_mutex_lock(&ingress->mutex);
    ingress->accepting = true;
    while ((!ingress->batch_ready || ingress->batch_claimed) &&
           !ingress->stop && result == 0) {
        result = pthread_cond_timedwait(&ingress->condition, &ingress->mutex,
                                        &deadline);
    }
    if (result == ETIMEDOUT) {
        ingress->accepting = false;
        (void)pthread_mutex_unlock(&ingress->mutex);
        return -ETIMEDOUT;
    }
    if (result != 0 || ingress->stop) {
        (void)pthread_mutex_unlock(&ingress->mutex);
        return result != 0 ? -result : -ECANCELED;
    }
    ingress->accepting = false;
    ingress->batch_claimed = true;
    *batch = ingress->batch;
    *batch_length = ingress->batch_length;
    *sequence = ingress->sequence;
    (void)pthread_mutex_unlock(&ingress->mutex);
    return 0;
}

int bvb_visible_ingress_complete(struct bvb_visible_ingress *ingress,
                                 int status) {
    if (ingress == NULL || status > 0) {
        return -EINVAL;
    }
    (void)pthread_mutex_lock(&ingress->mutex);
    if (!ingress->batch_ready || !ingress->batch_claimed ||
        ingress->batch_completed) {
        (void)pthread_mutex_unlock(&ingress->mutex);
        return -EINVAL;
    }
    ingress->batch_status = status;
    ingress->batch_completed = true;
    ingress->completion_pending_response = true;
    (void)pthread_cond_broadcast(&ingress->condition);
    (void)pthread_mutex_unlock(&ingress->mutex);
    return 0;
}

void bvb_visible_ingress_destroy(struct bvb_visible_ingress *ingress) {
    if (ingress == NULL) {
        return;
    }
    (void)pthread_mutex_lock(&ingress->mutex);
    if (ingress->completion_pending_response) {
        struct timespec deadline;
        if (realtime_deadline(1000U, &deadline) == 0) {
            while (ingress->completion_pending_response) {
                int wait_status = pthread_cond_timedwait(
                    &ingress->condition, &ingress->mutex, &deadline);
                if (wait_status != 0) {
                    break;
                }
            }
        }
    }
    ingress->stop = true;
    int client_fd = ingress->client_fd;
    if (client_fd >= 0) {
        (void)shutdown(client_fd, SHUT_RDWR);
    }
    (void)pthread_cond_broadcast(&ingress->condition);
    (void)pthread_mutex_unlock(&ingress->mutex);
    int wake_fd = ingress->transport == BVB_VISIBLE_INGRESS_LOOPBACK
                      ? bvb_transport_connect_loopback(ingress->loopback_port)
                      : bvb_transport_connect_abstract(
                            ingress->socket_name,
                            ingress->socket_name_length);
    if (wake_fd >= 0) {
        (void)close(wake_fd);
    }
    (void)pthread_join(ingress->worker, NULL);
    (void)close(ingress->listener_fd);
    bvb_visible_batch_region_destroy(&ingress->brokered_region);
    (void)pthread_cond_destroy(&ingress->condition);
    (void)pthread_mutex_destroy(&ingress->mutex);
    memset(ingress, 0, sizeof(*ingress));
    free(ingress);
}
