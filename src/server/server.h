#ifndef SERVER_SERVER_H
#define SERVER_SERVER_H

#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct websocket_context;
typedef struct websocket_context websocket_context_t;

typedef struct server_ctx {
    int listen_fd;
    pthread_t accept_thread;
    pthread_mutex_t lock;
    int running;
    int client_count;
    int max_clients;
    uint16_t port;
    char bind_ip[INET_ADDRSTRLEN];
    websocket_context_t *ws_context;
} server_ctx_t;

int server_init(server_ctx_t *ctx, const char *bind_ip, uint16_t port, int max_clients);
int server_start(server_ctx_t *ctx);
void server_request_stop(server_ctx_t *ctx);
void server_join(server_ctx_t *ctx);
void server_destroy(server_ctx_t *ctx);
void server_set_websocket_context(server_ctx_t *ctx, websocket_context_t *ws_ctx);

#ifdef __cplusplus
}
#endif

#endif // SERVER_SERVER_H
