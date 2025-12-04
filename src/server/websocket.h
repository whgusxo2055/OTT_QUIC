#ifndef SERVER_WEBSOCKET_H
#define SERVER_WEBSOCKET_H

#include "server/quic.h"
#include "db/database.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct websocket_context {
    quic_engine_t *quic_engine;
    db_context_t *db; /* optional */
    pthread_mutex_t lock;
    uint32_t next_packet_number;
    uint64_t segment_sent_ok;
    uint64_t segment_sent_fail;
} websocket_context_t;

void websocket_context_init(websocket_context_t *ctx, quic_engine_t *engine, db_context_t *db);
void websocket_context_destroy(websocket_context_t *ctx);

int websocket_handle_client(int client_fd, websocket_context_t *ctx);
int websocket_calculate_accept_key(const char *client_key, char *out, size_t out_size);
void websocket_apply_mask(uint8_t *data, size_t len, const uint8_t mask[4]);

#ifdef __cplusplus
}
#endif

#endif // SERVER_WEBSOCKET_H
