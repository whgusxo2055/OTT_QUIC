#include "server/server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define SERVER_BACKLOG 8
#define SERVER_BUFFER_SIZE 1024

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
}

void server_destroy(server_ctx_t *ctx) {
    if (!ctx) {
        return;
    }

    server_close_socket(&ctx->listen_fd);
    pthread_mutex_destroy(&ctx->lock);
    memset(ctx, 0, sizeof(*ctx));
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
    free(task);

    char buffer[SERVER_BUFFER_SIZE];
    ssize_t bytes;

    while ((bytes = recv(client_fd, buffer, sizeof(buffer), 0)) > 0) {
        ssize_t sent = 0;
        while (sent < bytes) {
            ssize_t n = send(client_fd, buffer + sent, (size_t)(bytes - sent), MSG_NOSIGNAL);
            if (n <= 0) {
                bytes = -1;
                break;
            }
            sent += n;
        }
        if (bytes < 0) {
            break;
        }
    }

    close(client_fd);

    pthread_mutex_lock(&ctx->lock);
    if (ctx->client_count > 0) {
        ctx->client_count--;
    }
    pthread_mutex_unlock(&ctx->lock);

    return NULL;
}
