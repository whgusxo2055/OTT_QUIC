#include "server/server.h"
#include "server/websocket.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#ifdef ENABLE_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define SERVER_BACKLOG 8

typedef struct {
    server_ctx_t *ctx;
    int client_fd;
    struct sockaddr_in addr;
} client_task_t;

static void *client_worker(void *arg);
static void *accept_loop(void *arg);

static void server_close_socket(int *fd) {
    if (*fd >= 0) {
        shutdown(*fd, SHUT_RDWR);
        close(*fd);
        *fd = -1;
    }
}

int server_init(server_ctx_t *ctx, const char *bind_ip, uint16_t port, int max_clients) {
    if (!ctx || !bind_ip || max_clients <= 0) {
        return -1;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->listen_fd = -1;
    ctx->max_clients = max_clients;
    ctx->port = port;
    strncpy(ctx->bind_ip, bind_ip, sizeof(ctx->bind_ip) - 1);
    pthread_mutex_init(&ctx->lock, NULL);
    pthread_cond_init(&ctx->clients_cv, NULL);
    ctx->ws_context = NULL;
    ctx->ssl_ctx = NULL;

    ctx->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->listen_fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    if (setsockopt(ctx->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        server_close_socket(&ctx->listen_fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid bind IP: %s\n", bind_ip);
        server_close_socket(&ctx->listen_fd);
        return -1;
    }

    if (bind(ctx->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        server_close_socket(&ctx->listen_fd);
        return -1;
    }

    if (listen(ctx->listen_fd, SERVER_BACKLOG) < 0) {
        perror("listen");
        server_close_socket(&ctx->listen_fd);
        return -1;
    }

    ctx->running = 1;
    return 0;
}

int server_enable_tls(server_ctx_t *ctx, const char *cert_path, const char *key_path) {
    if (!ctx || !cert_path || !key_path) {
        return -1;
    }
#ifdef ENABLE_TLS
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
    ctx->ssl_ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx->ssl_ctx) {
        ERR_print_errors_fp(stderr);
        return -1;
    }
    /* Allow TLS1.2+ so older clients can connect while keeping TLS1/1.1 disabled. */
    if (SSL_CTX_set_min_proto_version(ctx->ssl_ctx, TLS1_2_VERSION) != 1) {
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx->ssl_ctx);
        ctx->ssl_ctx = NULL;
        return -1;
    }
    if (SSL_CTX_use_certificate_file(ctx->ssl_ctx, cert_path, SSL_FILETYPE_PEM) != 1) {
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx->ssl_ctx);
        ctx->ssl_ctx = NULL;
        return -1;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx->ssl_ctx, key_path, SSL_FILETYPE_PEM) != 1) {
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx->ssl_ctx);
        ctx->ssl_ctx = NULL;
        return -1;
    }
    if (SSL_CTX_check_private_key(ctx->ssl_ctx) != 1) {
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx->ssl_ctx);
        ctx->ssl_ctx = NULL;
        return -1;
    }
    return 0;
#else
    (void)cert_path;
    (void)key_path;
    return -1;
#endif
}

int server_start(server_ctx_t *ctx) {
    if (!ctx || ctx->listen_fd < 0) {
        return -1;
    }

    int rc = pthread_create(&ctx->accept_thread, NULL, accept_loop, ctx);
    if (rc != 0) {
        errno = rc;
        perror("pthread_create");
        return -1;
    }

    return 0;
}

void server_request_stop(server_ctx_t *ctx) {
    if (!ctx) {
        return;
    }

    pthread_mutex_lock(&ctx->lock);
    ctx->running = 0;
    pthread_mutex_unlock(&ctx->lock);
    server_close_socket(&ctx->listen_fd);
}

void server_join(server_ctx_t *ctx) {
    if (!ctx) {
        return;
    }

    pthread_join(ctx->accept_thread, NULL);

    pthread_mutex_lock(&ctx->lock);
    while (ctx->client_count > 0) {
        pthread_cond_wait(&ctx->clients_cv, &ctx->lock);
    }
    pthread_mutex_unlock(&ctx->lock);
}

void server_destroy(server_ctx_t *ctx) {
    if (!ctx) {
        return;
    }

    server_close_socket(&ctx->listen_fd);
    pthread_mutex_destroy(&ctx->lock);
    pthread_cond_destroy(&ctx->clients_cv);
#ifdef ENABLE_TLS
    if (ctx->ssl_ctx) {
        SSL_CTX_free(ctx->ssl_ctx);
        ctx->ssl_ctx = NULL;
    }
#endif
    memset(ctx, 0, sizeof(*ctx));
}

void server_set_websocket_context(server_ctx_t *ctx, websocket_context_t *ws_ctx) {
    if (!ctx) {
        return;
    }
    ctx->ws_context = ws_ctx;
}

static void *accept_loop(void *arg) {
    server_ctx_t *ctx = (server_ctx_t *)arg;

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(ctx->listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            pthread_mutex_lock(&ctx->lock);
            int should_continue = ctx->running;
            pthread_mutex_unlock(&ctx->lock);
            if (!should_continue) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            continue;
        }

        pthread_mutex_lock(&ctx->lock);
        int can_accept = ctx->client_count < ctx->max_clients;
        if (can_accept) {
            ctx->client_count++;
        }
        pthread_mutex_unlock(&ctx->lock);

        if (!can_accept) {
            const char *msg = "Server busy, try again later\n";
            send(client_fd, msg, strlen(msg), MSG_NOSIGNAL);
            close(client_fd);
            continue;
        }

        client_task_t *task = malloc(sizeof(*task));
        if (!task) {
            close(client_fd);
            pthread_mutex_lock(&ctx->lock);
            ctx->client_count--;
            pthread_mutex_unlock(&ctx->lock);
            continue;
        }

        task->ctx = ctx;
        task->client_fd = client_fd;
        task->addr = client_addr;

        pthread_t thread_id;
        int rc = pthread_create(&thread_id, NULL, client_worker, task);
        if (rc != 0) {
            errno = rc;
            perror("pthread_create");
            close(client_fd);
            free(task);
            pthread_mutex_lock(&ctx->lock);
            ctx->client_count--;
            pthread_mutex_unlock(&ctx->lock);
            continue;
        }

        pthread_detach(thread_id);
    }

    return NULL;
}

static void *client_worker(void *arg) {
    client_task_t *task = (client_task_t *)arg;
    server_ctx_t *ctx = task->ctx;
    int client_fd = task->client_fd;
    SSL *ssl = NULL;
#ifdef ENABLE_TLS
    if (ctx->ssl_ctx) {
        ssl = SSL_new(ctx->ssl_ctx);
        if (!ssl) {
            close(client_fd);
            free(task);
            goto done;
        }
        SSL_set_fd(ssl, client_fd);
        if (SSL_accept(ssl) != 1) {
            // SSL 핸드셰이크 실패 (평문 HTTP 요청이 HTTPS 포트로 온 경우 등)
            // 에러를 stderr에 출력하지 않고 조용히 처리 (정상적인 거부 케이스)
            int ssl_err = SSL_get_error(ssl, -1);
            if (ssl_err != SSL_ERROR_WANT_READ && ssl_err != SSL_ERROR_WANT_WRITE) {
                // 실제 에러인 경우에만 로그 출력
                fprintf(stderr, "[TLS] SSL_accept failed for client (possibly plain HTTP on HTTPS port)\n");
            }
            SSL_free(ssl);
            ssl = NULL;
            close(client_fd);
            free(task);
            goto done;
        }
    }
#else
#endif
    free(task);

    websocket_handle_client(client_fd, ssl, ctx->ws_context);

#ifdef ENABLE_TLS
    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
#endif
    pthread_mutex_lock(&ctx->lock);
    if (ctx->client_count > 0) {
        ctx->client_count--;
    }
    pthread_cond_signal(&ctx->clients_cv);
    pthread_mutex_unlock(&ctx->lock);

#ifdef ENABLE_TLS
done:
#endif
    return NULL;
}
