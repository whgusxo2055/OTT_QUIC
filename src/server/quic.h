#ifndef SERVER_QUIC_H
#define SERVER_QUIC_H

#include <netinet/in.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QUIC_MAX_PAYLOAD        (16 * 1024)
#define QUIC_HEADER_SIZE        25
#define QUIC_MAX_PACKET_SIZE    (QUIC_HEADER_SIZE + QUIC_MAX_PAYLOAD)
#define QUIC_MAX_CONNECTIONS    32
#define QUIC_CONNECTION_TIMEOUT 30

#define QUIC_FLAG_INITIAL   0x01
#define QUIC_FLAG_HANDSHAKE 0x02
#define QUIC_FLAG_DATA      0x04
#define QUIC_FLAG_ACK       0x08
#define QUIC_FLAG_CLOSE     0x10

typedef enum {
    QUIC_CONN_STATE_IDLE = 0,
    QUIC_CONN_STATE_CONNECTING,
    QUIC_CONN_STATE_CONNECTED,
    QUIC_CONN_STATE_CLOSED
} quic_connection_state_t;

typedef struct {
    uint8_t flags;
    uint64_t connection_id;
    uint32_t packet_number;
    uint32_t stream_id;
    uint32_t offset;
    uint32_t length;
    const uint8_t *payload; /* lifetime tied to owning buffer */
} quic_packet_t;

struct quic_engine;
typedef void (*quic_packet_handler)(const quic_packet_t *packet, const struct sockaddr_in *addr, void *user_data);

typedef struct {
    uint64_t connection_id;
    struct sockaddr_in addr;
    time_t last_seen;
    quic_connection_state_t state;
    int in_use;
} quic_connection_entry_t;

typedef struct quic_engine {
    int sockfd;
    uint16_t port;
    pthread_t thread;
    pthread_mutex_t lock;
    int running;
    quic_packet_handler handler;
    void *user_data;
    quic_connection_entry_t connections[QUIC_MAX_CONNECTIONS];
} quic_engine_t;

int quic_packet_serialize(const quic_packet_t *packet, uint8_t *buffer, size_t buffer_len, size_t *out_len);
int quic_packet_deserialize(quic_packet_t *packet, const uint8_t *buffer, size_t buffer_len);

int quic_engine_init(quic_engine_t *engine, uint16_t port, quic_packet_handler handler, void *user_data);
int quic_engine_start(quic_engine_t *engine);
void quic_engine_stop(quic_engine_t *engine);
void quic_engine_join(quic_engine_t *engine);
void quic_engine_destroy(quic_engine_t *engine);
int quic_engine_send(const quic_engine_t *engine, const quic_packet_t *packet, const struct sockaddr_in *addr);
int quic_engine_send_to_connection(quic_engine_t *engine, const quic_packet_t *packet);
int quic_engine_get_connection(quic_engine_t *engine, uint64_t connection_id, struct sockaddr_in *addr_out);
int quic_engine_close_connection(quic_engine_t *engine, uint64_t connection_id);

#ifdef __cplusplus
}
#endif

#endif // SERVER_QUIC_H
