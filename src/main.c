#include "server/server.h"
#include "server/websocket.h"
#include "server/quic.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t keep_running = 1;

static void handle_signal(int signo) {
    (void)signo;
    keep_running = 0;
}

static void quic_default_handler(const quic_packet_t *packet, const struct sockaddr_in *addr, void *user_data) {
    (void)packet;
    (void)addr;
    (void)user_data;
    /* Placeholder: QUIC packets can be routed to WebSocket clients in later sprints. */
}

int main(void) {
    puts("OTT_QUIC TCP server starting.");

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    quic_engine_t quic_engine;
    websocket_context_t ws_context;
    server_ctx_t server;

    int quic_initialized = 0;
    int quic_started = 0;
    int ws_initialized = 0;
    int server_initialized = 0;
    int server_started = 0;
    int exit_code = 0;

    if (quic_engine_init(&quic_engine, 8443, quic_default_handler, NULL) != 0) {
        fputs("Failed to initialize QUIC engine.\n", stderr);
        exit_code = 1;
        goto cleanup;
    }
    quic_initialized = 1;

    if (quic_engine_start(&quic_engine) != 0) {
        fputs("Failed to start QUIC engine.\n", stderr);
        exit_code = 1;
        goto cleanup;
    }
    quic_started = 1;

    websocket_context_init(&ws_context, &quic_engine);
    ws_initialized = 1;

    if (server_init(&server, "0.0.0.0", 8080, 5) != 0) {
        fputs("Failed to initialize server context.\n", stderr);
        exit_code = 1;
        goto cleanup;
    }
    server_initialized = 1;
    server_set_websocket_context(&server, &ws_context);

    if (server_start(&server) != 0) {
        fputs("Failed to start TCP/WebSocket server.\n", stderr);
        exit_code = 1;
        goto cleanup;
    }
    server_started = 1;

    puts("Server is running on TCP:0.0.0.0:8080 and UDP:0.0.0.0:8443.");

    while (keep_running) {
        sleep(1);
    }

    puts("Shutdown signal received. Stopping server...");

cleanup:
    if (server_started) {
        server_request_stop(&server);
        server_join(&server);
    }
    if (server_initialized) {
        server_destroy(&server);
    }
    if (ws_initialized) {
        websocket_context_destroy(&ws_context);
    }
    if (quic_started) {
        quic_engine_stop(&quic_engine);
        quic_engine_join(&quic_engine);
    }
    if (quic_initialized) {
        quic_engine_destroy(&quic_engine);
    }

    puts("OTT_QUIC server shutting down.");
    return exit_code;
}
