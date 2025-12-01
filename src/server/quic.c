#include "server/quic.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static uint64_t host_to_be64(uint64_t value) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint64_t)htonl((uint32_t)(value >> 32)) | ((uint64_t)htonl((uint32_t)(value & 0xFFFFFFFF)) << 32));
#else
    return value;
#endif
}

static uint64_t be64_to_host(uint64_t value) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint64_t)ntohl((uint32_t)(value >> 32)) | ((uint64_t)ntohl((uint32_t)(value & 0xFFFFFFFF)) << 32));
#else
    return value;
#endif
}

static quic_connection_entry_t *quic_engine_find_entry_locked(quic_engine_t *engine, uint64_t connection_id);
static int quic_engine_add_connection_locked(quic_engine_t *engine, uint64_t connection_id, const struct sockaddr_in *addr);
static void quic_engine_cleanup_connections_locked(quic_engine_t *engine, time_t now);
static int quic_engine_process_packet(quic_engine_t *engine,
                                      const quic_packet_t *packet,
                                      const struct sockaddr_in *addr,
                                      int *handshake_needed);
static void quic_engine_send_handshake(quic_engine_t *engine, const struct sockaddr_in *addr, uint64_t connection_id);
static void quic_engine_send_close(quic_engine_t *engine, const struct sockaddr_in *addr, uint64_t connection_id);
static void *quic_engine_loop(void *arg);

static quic_connection_entry_t *quic_engine_find_entry_locked(quic_engine_t *engine, uint64_t connection_id) {
    for (int i = 0; i < QUIC_MAX_CONNECTIONS; ++i) {
        if (engine->connections[i].in_use && engine->connections[i].connection_id == connection_id) {
            return &engine->connections[i];
        }
    }
    return NULL;
}

static int quic_engine_add_connection_locked(quic_engine_t *engine, uint64_t connection_id, const struct sockaddr_in *addr) {
    for (int i = 0; i < QUIC_MAX_CONNECTIONS; ++i) {
        if (!engine->connections[i].in_use) {
            engine->connections[i].in_use = 1;
            engine->connections[i].connection_id = connection_id;
            engine->connections[i].addr = *addr;
            engine->connections[i].last_seen = time(NULL);
            engine->connections[i].state = QUIC_CONN_STATE_CONNECTING;
            return 0;
        }
    }
    return -1;
}

static int quic_engine_process_packet(quic_engine_t *engine,
                                      const quic_packet_t *packet,
                                      const struct sockaddr_in *addr,
                                      int *handshake_needed) {
    if (!engine || !packet || !addr) {
        return -1;
    }

    time_t now = time(NULL);
    pthread_mutex_lock(&engine->lock);
    quic_engine_cleanup_connections_locked(engine, now);

    quic_connection_entry_t *entry = quic_engine_find_entry_locked(engine, packet->connection_id);
    int created = 0;
    if (!entry) {
        if (!(packet->flags & QUIC_FLAG_INITIAL)) {
            pthread_mutex_unlock(&engine->lock);
            return -1;
        }
        if (quic_engine_add_connection_locked(engine, packet->connection_id, addr) != 0) {
            pthread_mutex_unlock(&engine->lock);
            return -1;
        }
        entry = quic_engine_find_entry_locked(engine, packet->connection_id);
        created = 1;
        if (handshake_needed) {
            *handshake_needed = 1;
        }
    } else {
        entry->addr = *addr;
        entry->last_seen = now;
    }

    if (packet->flags & QUIC_FLAG_CLOSE) {
        entry->state = QUIC_CONN_STATE_CLOSED;
        entry->in_use = 0;
        pthread_mutex_unlock(&engine->lock);
        return 0;
    }

    if (entry->state == QUIC_CONN_STATE_CONNECTING && (packet->flags & QUIC_FLAG_HANDSHAKE)) {
        entry->state = QUIC_CONN_STATE_CONNECTED;
    }

    int should_deliver = (entry->state == QUIC_CONN_STATE_CONNECTED) ||
                         (packet->flags & (QUIC_FLAG_INITIAL | QUIC_FLAG_HANDSHAKE));

    (void)created;
    pthread_mutex_unlock(&engine->lock);
    return should_deliver ? 0 : -1;
}

static void quic_engine_send_handshake(quic_engine_t *engine, const struct sockaddr_in *addr, uint64_t connection_id) {
    quic_packet_t response = {
        .flags = QUIC_FLAG_HANDSHAKE | QUIC_FLAG_ACK,
        .connection_id = connection_id,
        .packet_number = 1,
        .stream_id = 0,
        .offset = 0,
        .length = 0,
        .payload = NULL,
    };
    quic_engine_send(engine, &response, addr);
}

static void quic_engine_send_close(quic_engine_t *engine, const struct sockaddr_in *addr, uint64_t connection_id) {
    quic_packet_t response = {
        .flags = QUIC_FLAG_CLOSE,
        .connection_id = connection_id,
        .packet_number = 0,
        .stream_id = 0,
        .offset = 0,
        .length = 0,
        .payload = NULL,
    };
    quic_engine_send(engine, &response, addr);
}

int quic_packet_serialize(const quic_packet_t *packet, uint8_t *buffer, size_t buffer_len, size_t *out_len) {
    if (!packet || !buffer) {
        return -1;
    }

    if (packet->length > QUIC_MAX_PAYLOAD) {
        return -1;
    }

    size_t required = QUIC_HEADER_SIZE + packet->length;
    if (buffer_len < required) {
        return -1;
    }

    buffer[0] = packet->flags;

    uint64_t conn_be = host_to_be64(packet->connection_id);
    memcpy(buffer + 1, &conn_be, sizeof(conn_be));

    uint32_t tmp;
    tmp = htonl(packet->packet_number);
    memcpy(buffer + 9, &tmp, sizeof(tmp));
    tmp = htonl(packet->stream_id);
    memcpy(buffer + 13, &tmp, sizeof(tmp));
    tmp = htonl(packet->offset);
    memcpy(buffer + 17, &tmp, sizeof(tmp));
    tmp = htonl(packet->length);
    memcpy(buffer + 21, &tmp, sizeof(tmp));

    if (packet->length > 0 && packet->payload) {
        memcpy(buffer + QUIC_HEADER_SIZE, packet->payload, packet->length);
    }

    if (out_len) {
        *out_len = required;
    }

    return 0;
}

int quic_packet_deserialize(quic_packet_t *packet, const uint8_t *buffer, size_t buffer_len) {
    if (!packet || !buffer || buffer_len < QUIC_HEADER_SIZE) {
        return -1;
    }

    memset(packet, 0, sizeof(*packet));

    packet->flags = buffer[0];

    uint64_t conn_be;
    memcpy(&conn_be, buffer + 1, sizeof(conn_be));
    packet->connection_id = be64_to_host(conn_be);

    uint32_t tmp;
    memcpy(&tmp, buffer + 9, sizeof(tmp));
    packet->packet_number = ntohl(tmp);
    memcpy(&tmp, buffer + 13, sizeof(tmp));
    packet->stream_id = ntohl(tmp);
    memcpy(&tmp, buffer + 17, sizeof(tmp));
    packet->offset = ntohl(tmp);
    memcpy(&tmp, buffer + 21, sizeof(tmp));
    packet->length = ntohl(tmp);

    size_t required = QUIC_HEADER_SIZE + packet->length;
    if (packet->length > QUIC_MAX_PAYLOAD || buffer_len < required) {
        return -1;
    }

    packet->payload = buffer + QUIC_HEADER_SIZE;
    return 0;
}

int quic_engine_init(quic_engine_t *engine, uint16_t port, quic_packet_handler handler, void *user_data) {
    if (!engine) {
        return -1;
    }

    memset(engine, 0, sizeof(*engine));
    engine->sockfd = -1;
    engine->port = port;
    engine->handler = handler;
    engine->user_data = user_data;
    pthread_mutex_init(&engine->lock, NULL);

    engine->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (engine->sockfd < 0) {
        perror("socket");
        pthread_mutex_destroy(&engine->lock);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(engine->sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(engine->sockfd);
        engine->sockfd = -1;
        pthread_mutex_destroy(&engine->lock);
        return -1;
    }

    engine->running = 1;
    return 0;
}

int quic_engine_start(quic_engine_t *engine) {
    if (!engine || engine->sockfd < 0) {
        return -1;
    }

    int rc = pthread_create(&engine->thread, NULL, quic_engine_loop, engine);
    if (rc != 0) {
        errno = rc;
        perror("pthread_create");
        return -1;
    }

    return 0;
}

void quic_engine_stop(quic_engine_t *engine) {
    if (!engine) {
        return;
    }

    pthread_mutex_lock(&engine->lock);
    engine->running = 0;
    pthread_mutex_unlock(&engine->lock);

    if (engine->sockfd >= 0) {
        shutdown(engine->sockfd, SHUT_RDWR);
        close(engine->sockfd);
        engine->sockfd = -1;
    }
}

void quic_engine_join(quic_engine_t *engine) {
    if (!engine) {
        return;
    }
    pthread_join(engine->thread, NULL);
}

void quic_engine_destroy(quic_engine_t *engine) {
    if (!engine) {
        return;
    }

    if (engine->sockfd >= 0) {
        close(engine->sockfd);
        engine->sockfd = -1;
    }

    pthread_mutex_destroy(&engine->lock);
    memset(engine->connections, 0, sizeof(engine->connections));
}

int quic_engine_send(const quic_engine_t *engine, const quic_packet_t *packet, const struct sockaddr_in *addr) {
    if (!engine || !packet || !addr) {
        return -1;
    }

    uint8_t buffer[QUIC_MAX_PACKET_SIZE];
    size_t len = 0;
    if (quic_packet_serialize(packet, buffer, sizeof(buffer), &len) != 0) {
        return -1;
    }

    ssize_t sent = sendto(engine->sockfd, buffer, len, 0, (const struct sockaddr *)addr, sizeof(*addr));
    if (sent < 0 || (size_t)sent != len) {
        return -1;
    }

    return 0;
}

int quic_engine_get_connection(quic_engine_t *engine, uint64_t connection_id, struct sockaddr_in *addr_out) {
    if (!engine || !addr_out) {
        return -1;
    }

    int found = -1;
    time_t now = time(NULL);
    pthread_mutex_lock(&engine->lock);
    quic_engine_cleanup_connections_locked(engine, now);
    quic_connection_entry_t *entry = quic_engine_find_entry_locked(engine, connection_id);
    if (entry && entry->state == QUIC_CONN_STATE_CONNECTED) {
        *addr_out = entry->addr;
        entry->last_seen = now;
        found = 0;
    }
    pthread_mutex_unlock(&engine->lock);
    return found;
}

int quic_engine_send_to_connection(quic_engine_t *engine, const quic_packet_t *packet) {
    if (!engine || !packet) {
        return -1;
    }

    struct sockaddr_in addr;
    if (quic_engine_get_connection(engine, packet->connection_id, &addr) != 0) {
        return -1;
    }

    return quic_engine_send(engine, packet, &addr);
}

int quic_engine_close_connection(quic_engine_t *engine, uint64_t connection_id) {
    if (!engine) {
        return -1;
    }

    struct sockaddr_in addr;
    int found = -1;
    pthread_mutex_lock(&engine->lock);
    quic_connection_entry_t *entry = quic_engine_find_entry_locked(engine, connection_id);
    if (entry && entry->state != QUIC_CONN_STATE_CLOSED) {
        addr = entry->addr;
        entry->state = QUIC_CONN_STATE_CLOSED;
        entry->in_use = 0;
        found = 0;
    }
    pthread_mutex_unlock(&engine->lock);

    if (found == 0) {
        quic_engine_send_close(engine, &addr, connection_id);
    }
    return found;
}

static void quic_engine_cleanup_connections_locked(quic_engine_t *engine, time_t now) {
    for (int i = 0; i < QUIC_MAX_CONNECTIONS; ++i) {
        if (engine->connections[i].in_use &&
            (now - engine->connections[i].last_seen) > QUIC_CONNECTION_TIMEOUT) {
            engine->connections[i].in_use = 0;
            engine->connections[i].state = QUIC_CONN_STATE_CLOSED;
        }
    }
}

static void *quic_engine_loop(void *arg) {
    quic_engine_t *engine = (quic_engine_t *)arg;
    uint8_t buffer[QUIC_MAX_PACKET_SIZE];

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        ssize_t received = recvfrom(engine->sockfd, buffer, sizeof(buffer), 0, (struct sockaddr *)&client_addr, &addr_len);
        if (received < 0) {
            pthread_mutex_lock(&engine->lock);
            int running = engine->running;
            pthread_mutex_unlock(&engine->lock);
            if (!running) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            perror("recvfrom");
            continue;
        }

        quic_packet_t packet;
        if (quic_packet_deserialize(&packet, buffer, (size_t)received) != 0) {
            continue;
        }

        int handshake_needed = 0;
        if (quic_engine_process_packet(engine, &packet, &client_addr, &handshake_needed) != 0) {
            continue;
        }

        if (handshake_needed) {
            quic_engine_send_handshake(engine, &client_addr, packet.connection_id);
        }

        if (engine->handler) {
            engine->handler(&packet, &client_addr, engine->user_data);
        }
    }

    return NULL;
}
