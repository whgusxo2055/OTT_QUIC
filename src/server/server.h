#ifndef SERVER_SERVER_H
#define SERVER_SERVER_H

#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct server_ctx {
    int listen_fd;
    pthread_t accept_thread;
    pthread_mutex_t lock;
    int running;
    int client_count;
    int max_clients;
    uint16_t port;
    char bind_ip[INET_ADDRSTRLEN];
} server_ctx_t;

int server_init(server_ctx_t *ctx, const char *bind_ip, uint16_t port, int max_clients);
int server_start(server_ctx_t *ctx);
void server_request_stop(server_ctx_t *ctx);
void server_join(server_ctx_t *ctx);
void server_destroy(server_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif // SERVER_SERVER_H
