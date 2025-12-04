#ifndef SERVER_SERVER_H
#define SERVER_SERVER_H

#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>

#ifdef ENABLE_TLS
#include <openssl/ssl.h>
#else
typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct websocket_context;
typedef struct websocket_context websocket_context_t;

typedef struct server_ctx {
    int listen_fd;
    pthread_t accept_thread;
    pthread_mutex_t lock;
    pthread_cond_t clients_cv;
    int running;
    int client_count;
    int max_clients;
    uint16_t port;
    char bind_ip[INET_ADDRSTRLEN];
    websocket_context_t *ws_context;
    SSL_CTX *ssl_ctx;
} server_ctx_t;

int server_init(server_ctx_t *ctx, const char *bind_ip, uint16_t port, int max_clients);
int server_enable_tls(server_ctx_t *ctx, const char *cert_path, const char *key_path);
int server_start(server_ctx_t *ctx);
void server_request_stop(server_ctx_t *ctx);
void server_join(server_ctx_t *ctx);
void server_destroy(server_ctx_t *ctx);
void server_set_websocket_context(server_ctx_t *ctx, websocket_context_t *ws_ctx);

#ifdef __cplusplus
}
#endif

#endif // SERVER_SERVER_H
