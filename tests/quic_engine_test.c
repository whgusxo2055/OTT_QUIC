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
    uint8_t stream_buf[QUIC_MAX_PAYLOAD];
    size_t stream_len;
    uint32_t stream_offset;
    quic_connection_state_t last_state;
    struct sockaddr_in last_state_addr;
    int state_called;
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

static void stream_handler(uint64_t connection_id, uint32_t stream_id, uint32_t offset, const uint8_t *data, size_t len, void *user_data) {
    (void)connection_id;
    (void)stream_id;
    handler_state_t *state = (handler_state_t *)user_data;
    pthread_mutex_lock(&state->lock);
    memcpy(state->stream_buf, data, len);
    state->stream_len = len;
    state->stream_offset = offset;
    pthread_mutex_unlock(&state->lock);
}

static void state_handler(uint64_t connection_id, quic_connection_state_t state, const struct sockaddr_in *addr, void *user_data) {
    (void)connection_id;
    handler_state_t *st = (handler_state_t *)user_data;
    pthread_mutex_lock(&st->lock);
    st->last_state = state;
    if (addr) {
        st->last_state_addr = *addr;
    }
    st->state_called = 1;
    pthread_mutex_unlock(&st->lock);
}

static int wait_for_packet(handler_state_t *state) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1;

    pthread_mutex_lock(&state->lock);
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
    const uint16_t candidate_ports[] = {20443, 21443, 22443, 23443, 24443};
    uint16_t port = candidate_ports[0];
    int init_ok = 0;
    for (size_t i = 0; i < sizeof(candidate_ports) / sizeof(candidate_ports[0]); ++i) {
        port = candidate_ports[i];
        if (quic_engine_init(&engine, port, packet_handler, &state) == 0) {
            init_ok = 1;
            break;
        }
    }
    if (!init_ok) {
        fprintf(stderr, "quic_engine_init bind failed on candidate ports, skipping test\n");
        return 0;
    }
    quic_engine_set_stream_data_handler(&engine, stream_handler, &state);
    quic_engine_set_state_handler(&engine, state_handler, &state);
    assert(quic_engine_start(&engine) == 0);

    int client_fd1 = socket(AF_INET, SOCK_DGRAM, 0);
    assert(client_fd1 >= 0);
    int client_fd2 = socket(AF_INET, SOCK_DGRAM, 0);
    assert(client_fd2 >= 0);
    int client_fd3 = socket(AF_INET, SOCK_DGRAM, 0);
    assert(client_fd3 >= 0);
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    assert(setsockopt(client_fd1, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
    assert(setsockopt(client_fd2, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
    assert(setsockopt(client_fd3, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    /* 로컬 포트가 다른 소켓으로 bind하여 migration 테스트 준비 */
    struct sockaddr_in bind1 = {.sin_family = AF_INET, .sin_port = 0};
    inet_pton(AF_INET, "127.0.0.1", &bind1.sin_addr);
    assert(bind(client_fd1, (struct sockaddr *)&bind1, sizeof(bind1)) == 0);
    struct sockaddr_in bind3 = {.sin_family = AF_INET, .sin_port = 0};
    inet_pton(AF_INET, "127.0.0.1", &bind3.sin_addr);
    assert(bind(client_fd3, (struct sockaddr *)&bind3, sizeof(bind3)) == 0);

    uint8_t payload[] = {0x10, 0x20, 0x30};
    const uint64_t conn_id1 = 0xABCDEFULL;
    const uint64_t conn_id2 = 0x123456ULL;

    uint8_t buffer[QUIC_MAX_PACKET_SIZE];
    size_t len = 0;
    quic_packet_t initial_packet1 = {
        .flags = QUIC_FLAG_INITIAL,
        .connection_id = conn_id1,
        .packet_number = 0,
        .stream_id = 0,
        .offset = 0,
        .length = 0,
        .payload = NULL,
    };
    assert(quic_packet_serialize(&initial_packet1, buffer, sizeof(buffer), &len) == 0);
    assert(sendto(client_fd1, buffer, len, 0, (struct sockaddr *)&addr, sizeof(addr)) == (ssize_t)len);

    uint8_t recv_buf[QUIC_MAX_PACKET_SIZE];
    ssize_t recv_len = recvfrom(client_fd1, recv_buf, sizeof(recv_buf), 0, NULL, NULL);
    assert(recv_len > 0);
    quic_packet_t handshake_resp;
    assert(quic_packet_deserialize(&handshake_resp, recv_buf, (size_t)recv_len) == 0);
    assert(handshake_resp.flags & QUIC_FLAG_HANDSHAKE);
    assert(handshake_resp.connection_id == conn_id1);

    quic_packet_t handshake_packet1 = {
        .flags = QUIC_FLAG_HANDSHAKE,
        .connection_id = conn_id1,
        .packet_number = 1,
        .stream_id = 0,
        .offset = 0,
        .length = 0,
        .payload = NULL,
    };
    assert(quic_packet_serialize(&handshake_packet1, buffer, sizeof(buffer), &len) == 0);
    assert(sendto(client_fd1, buffer, len, 0, (struct sockaddr *)&addr, sizeof(addr)) == (ssize_t)len);

    quic_packet_t data_packet1 = {
        .flags = QUIC_FLAG_DATA,
        .connection_id = conn_id1,
        .packet_number = 7,
        .stream_id = 2,
        .offset = 0,
        .length = sizeof(payload),
        .payload = payload,
    };
    assert(quic_packet_serialize(&data_packet1, buffer, sizeof(buffer), &len) == 0);
    assert(sendto(client_fd1, buffer, len, 0, (struct sockaddr *)&addr, sizeof(addr)) == (ssize_t)len);

    ssize_t ack_len = recvfrom(client_fd1, recv_buf, sizeof(recv_buf), 0, NULL, NULL);
    assert(ack_len > 0);
    quic_packet_t ack_pkt;
    assert(quic_packet_deserialize(&ack_pkt, recv_buf, (size_t)ack_len) == 0);
    assert(ack_pkt.flags & QUIC_FLAG_ACK);

    assert(wait_for_packet(&state) == 0);
    assert(state.packet.connection_id == data_packet1.connection_id);
    assert(state.packet.packet_number == data_packet1.packet_number);
    assert(state.stream_len == sizeof(payload));
    assert(state.stream_offset == 0);
    assert(memcmp(state.stream_buf, payload, sizeof(payload)) == 0);

    struct sockaddr_in stored_addr;
    assert(quic_engine_get_connection(&engine, conn_id1, &stored_addr) == 0);
    struct sockaddr_in local_addr;
    socklen_t local_len = sizeof(local_addr);
    assert(getsockname(client_fd1, (struct sockaddr *)&local_addr, &local_len) == 0);
    assert(ntohs(stored_addr.sin_port) == ntohs(local_addr.sin_port));

    /* 두 번째 연결: 별도 소켓/연결 ID */
    quic_packet_t initial_packet2 = {
        .flags = QUIC_FLAG_INITIAL,
        .connection_id = conn_id2,
        .packet_number = 0,
        .stream_id = 0,
        .offset = 0,
        .length = 0,
        .payload = NULL,
    };
    assert(quic_packet_serialize(&initial_packet2, buffer, sizeof(buffer), &len) == 0);
    assert(sendto(client_fd2, buffer, len, 0, (struct sockaddr *)&addr, sizeof(addr)) == (ssize_t)len);

    recv_len = recvfrom(client_fd2, recv_buf, sizeof(recv_buf), 0, NULL, NULL);
    assert(recv_len > 0);
    assert(quic_packet_deserialize(&handshake_resp, recv_buf, (size_t)recv_len) == 0);
    assert(handshake_resp.connection_id == conn_id2);

    quic_packet_t handshake_packet2 = {
        .flags = QUIC_FLAG_HANDSHAKE,
        .connection_id = conn_id2,
        .packet_number = 1,
        .stream_id = 0,
        .offset = 0,
        .length = 0,
        .payload = NULL,
    };
    assert(quic_packet_serialize(&handshake_packet2, buffer, sizeof(buffer), &len) == 0);
    assert(sendto(client_fd2, buffer, len, 0, (struct sockaddr *)&addr, sizeof(addr)) == (ssize_t)len);

    quic_packet_t data_packet2 = {
        .flags = QUIC_FLAG_DATA,
        .connection_id = conn_id2,
        .packet_number = 3,
        .stream_id = 1,
        .offset = 0,
        .length = sizeof(payload),
        .payload = payload,
    };
    assert(quic_packet_serialize(&data_packet2, buffer, sizeof(buffer), &len) == 0);
    assert(sendto(client_fd2, buffer, len, 0, (struct sockaddr *)&addr, sizeof(addr)) == (ssize_t)len);

    pthread_mutex_lock(&state.lock);
    state.received = 0;
    pthread_mutex_unlock(&state.lock);

    assert(wait_for_packet(&state) == 0);
    assert(state.packet.connection_id == data_packet2.connection_id);
    assert(state.packet.packet_number == data_packet2.packet_number);
    assert(state.stream_len == sizeof(payload));

    /* 서버->클라이언트 데이터 전송 및 재전송 검증 */
    quic_metrics_t before_metrics;
    quic_engine_get_metrics(&engine, &before_metrics);

    quic_packet_t server_data = {
        .flags = QUIC_FLAG_DATA,
        .connection_id = conn_id2,
        .packet_number = 100,
        .stream_id = 1,
        .offset = 0,
        .length = sizeof(payload),
        .payload = payload,
    };
    assert(quic_engine_send_to_connection(&engine, &server_data) == 0);

    sleep(2);
    quic_packet_t server_data_rx;
    memset(&server_data_rx, 0, sizeof(server_data_rx));
    for (int attempt = 0; attempt < 4; ++attempt) {
        recv_len = recvfrom(client_fd2, recv_buf, sizeof(recv_buf), 0, NULL, NULL);
        assert(recv_len > 0);
        assert(quic_packet_deserialize(&server_data_rx, recv_buf, (size_t)recv_len) == 0);
        if (server_data_rx.flags & QUIC_FLAG_DATA) {
            break;
        }
    }
    assert(server_data_rx.flags & QUIC_FLAG_DATA);
    assert(server_data_rx.packet_number == server_data.packet_number);

    quic_packet_t server_ack = {
        .flags = QUIC_FLAG_ACK,
        .connection_id = conn_id2,
        .packet_number = server_data_rx.packet_number,
        .stream_id = server_data.stream_id,
        .offset = server_data.offset,
        .length = 0,
        .payload = NULL,
    };
    assert(quic_packet_serialize(&server_ack, buffer, sizeof(buffer), &len) == 0);
    assert(sendto(client_fd2, buffer, len, 0, (struct sockaddr *)&addr, sizeof(addr)) == (ssize_t)len);

    quic_metrics_t after_metrics;
    quic_engine_get_metrics(&engine, &after_metrics);
    assert(after_metrics.packets_sent - before_metrics.packets_sent >= 2);

    /* 주소 migration: conn_id1을 다른 소켓에서 전송 */
    struct sockaddr_in src_addr1;
    socklen_t sa_len1 = sizeof(src_addr1);
    assert(getsockname(client_fd1, (struct sockaddr *)&src_addr1, &sa_len1) == 0);
    struct sockaddr_in src_addr3;
    socklen_t sa_len3 = sizeof(src_addr3);
    assert(getsockname(client_fd3, (struct sockaddr *)&src_addr3, &sa_len3) == 0);
    assert(ntohs(src_addr1.sin_port) != ntohs(src_addr3.sin_port));

    quic_packet_t migrate_packet = {
        .flags = QUIC_FLAG_DATA,
        .connection_id = conn_id1,
        .packet_number = 20,
        .stream_id = 1,
        .offset = 0,
        .length = sizeof(payload),
        .payload = payload,
    };
    assert(quic_packet_serialize(&migrate_packet, buffer, sizeof(buffer), &len) == 0);
    assert(sendto(client_fd3, buffer, len, 0, (struct sockaddr *)&addr, sizeof(addr)) == (ssize_t)len);
    pthread_mutex_lock(&state.lock);
    state.received = 0;
    state.state_called = 0;
    pthread_mutex_unlock(&state.lock);
    assert(wait_for_packet(&state) == 0);

    quic_metrics_t after_mig;
    quic_engine_get_metrics(&engine, &after_mig);
    assert(after_mig.connections_migrated >= 1);
    pthread_mutex_lock(&state.lock);
    int state_called = state.state_called;
    struct sockaddr_in new_addr = state.last_state_addr;
    pthread_mutex_unlock(&state.lock);
    assert(state_called);
    assert(ntohs(new_addr.sin_port) == ntohs(src_addr3.sin_port));

    /* 타임아웃 정리 검증 */
    pthread_mutex_lock(&engine.lock);
    quic_connection_entry_t *entry = NULL;
    for (int i = 0; i < QUIC_MAX_CONNECTIONS; ++i) {
        if (engine.connections[i].in_use && engine.connections[i].connection_id == conn_id1) {
            entry = &engine.connections[i];
            break;
        }
    }
    assert(entry);
    entry->last_seen = time(NULL) - (QUIC_CONNECTION_TIMEOUT + 1);
    pthread_mutex_unlock(&engine.lock);

    assert(quic_engine_get_connection(&engine, conn_id1, &stored_addr) == -1);
    assert(quic_engine_get_connection(&engine, conn_id2, &stored_addr) == 0);

    assert(quic_engine_close_connection(&engine, conn_id2) == 0);
    assert(quic_engine_get_connection(&engine, conn_id2, &stored_addr) == -1);

    close(client_fd1);
    close(client_fd2);
    close(client_fd3);
    quic_engine_stop(&engine);
    quic_engine_join(&engine);
    quic_engine_destroy(&engine);

    pthread_mutex_destroy(&state.lock);
    pthread_cond_destroy(&state.cond);

    puts("quic_engine_test passed");
    return 0;
}
