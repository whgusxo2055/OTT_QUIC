#include "server/quic.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int received;
    quic_packet_t packet;
    uint8_t payload_copy[QUIC_MAX_PAYLOAD];
} handler_state_t;

static void packet_handler(const quic_packet_t *packet, const struct sockaddr_in *addr, void *user_data) {
    (void)addr;
    handler_state_t *state = (handler_state_t *)user_data;
    if (!(packet->flags & QUIC_FLAG_DATA)) {
        return;
    }
    pthread_mutex_lock(&state->lock);
    state->packet = *packet;
    if (packet->length > 0 && packet->payload) {
        memcpy(state->payload_copy, packet->payload, packet->length);
        state->packet.payload = state->payload_copy;
    }
    state->received = 1;
    pthread_cond_signal(&state->cond);
    pthread_mutex_unlock(&state->lock);
}

static int wait_for_packet(handler_state_t *state) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1;

    pthread_mutex_lock(&state->lock);
    state->received = 0;
    while (!state->received) {
        int rc = pthread_cond_timedwait(&state->cond, &state->lock, &ts);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&state->lock);
            return -1;
        }
    }
    pthread_mutex_unlock(&state->lock);
    return 0;
}

int main(void) {
    handler_state_t state;
    memset(&state, 0, sizeof(state));
    pthread_mutex_init(&state.lock, NULL);
    pthread_cond_init(&state.cond, NULL);

    quic_engine_t engine;
    const uint16_t port = 18443;
    assert(quic_engine_init(&engine, port, packet_handler, &state) == 0);
    assert(quic_engine_start(&engine) == 0);

    int client_fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(client_fd >= 0);
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    assert(setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    uint8_t payload[] = {0x10, 0x20, 0x30};
    const uint64_t conn_id = 0xABCDEFULL;

    uint8_t buffer[QUIC_MAX_PACKET_SIZE];
    size_t len = 0;
    quic_packet_t initial_packet = {
        .flags = QUIC_FLAG_INITIAL,
        .connection_id = conn_id,
        .packet_number = 0,
        .stream_id = 0,
        .offset = 0,
        .length = 0,
        .payload = NULL,
    };
    assert(quic_packet_serialize(&initial_packet, buffer, sizeof(buffer), &len) == 0);
    assert(sendto(client_fd, buffer, len, 0, (struct sockaddr *)&addr, sizeof(addr)) == (ssize_t)len);

    uint8_t recv_buf[QUIC_MAX_PACKET_SIZE];
    ssize_t recv_len = recvfrom(client_fd, recv_buf, sizeof(recv_buf), 0, NULL, NULL);
    assert(recv_len > 0);
    quic_packet_t handshake_resp;
    assert(quic_packet_deserialize(&handshake_resp, recv_buf, (size_t)recv_len) == 0);
    assert(handshake_resp.flags & QUIC_FLAG_HANDSHAKE);
    assert(handshake_resp.connection_id == conn_id);

    quic_packet_t handshake_packet = {
        .flags = QUIC_FLAG_HANDSHAKE,
        .connection_id = conn_id,
        .packet_number = 1,
        .stream_id = 0,
        .offset = 0,
        .length = 0,
        .payload = NULL,
    };
    assert(quic_packet_serialize(&handshake_packet, buffer, sizeof(buffer), &len) == 0);
    assert(sendto(client_fd, buffer, len, 0, (struct sockaddr *)&addr, sizeof(addr)) == (ssize_t)len);

    quic_packet_t data_packet = {
        .flags = QUIC_FLAG_DATA,
        .connection_id = conn_id,
        .packet_number = 7,
        .stream_id = 2,
        .offset = 0,
        .length = sizeof(payload),
        .payload = payload,
    };
    assert(quic_packet_serialize(&data_packet, buffer, sizeof(buffer), &len) == 0);
    assert(sendto(client_fd, buffer, len, 0, (struct sockaddr *)&addr, sizeof(addr)) == (ssize_t)len);

    assert(wait_for_packet(&state) == 0);
    assert(state.packet.connection_id == data_packet.connection_id);
    assert(state.packet.packet_number == data_packet.packet_number);

    struct sockaddr_in stored_addr;
    assert(quic_engine_get_connection(&engine, conn_id, &stored_addr) == 0);
    struct sockaddr_in local_addr;
    socklen_t local_len = sizeof(local_addr);
    assert(getsockname(client_fd, (struct sockaddr *)&local_addr, &local_len) == 0);
    assert(ntohs(stored_addr.sin_port) == ntohs(local_addr.sin_port));

    assert(quic_engine_close_connection(&engine, conn_id) == 0);
    assert(quic_engine_get_connection(&engine, conn_id, &stored_addr) == -1);

    close(client_fd);
    quic_engine_stop(&engine);
    quic_engine_join(&engine);
    quic_engine_destroy(&engine);

    pthread_mutex_destroy(&state.lock);
    pthread_cond_destroy(&state.cond);

    puts("quic_engine_test passed");
    return 0;
}
