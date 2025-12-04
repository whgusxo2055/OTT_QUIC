#include "server/server.h"
#include "server/websocket.h"
#include "server/quic.h"
#include "db/database.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
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
    db_context_t db;

    int quic_initialized = 0;
    int quic_started = 0;
    int ws_initialized = 0;
    int server_initialized = 0;
    int server_started = 0;
    int db_initialized = 0;
    int exit_code = 0;

    if (db_init(&db, "data/ott.db") != SQLITE_OK) {
        fputs("Failed to open DB.\n", stderr);
        exit_code = 1;
        goto cleanup;
    }
    if (db_initialize_schema(&db) != SQLITE_OK) {
        fputs("Failed to init DB schema.\n", stderr);
        exit_code = 1;
        goto cleanup;
    }
    db_initialized = 1;

    if (quic_engine_init(&quic_engine, 9443, quic_default_handler, NULL) != 0) {
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

    websocket_context_init(&ws_context, &quic_engine, &db);
    ws_initialized = 1;

    if (server_init(&server, "0.0.0.0", 8443, 5) != 0) {
        fputs("Failed to initialize server context.\n", stderr);
        exit_code = 1;
        goto cleanup;
    }
    server_initialized = 1;

    const char *tls_cert = getenv("TLS_CERT_PATH");
    const char *tls_key = getenv("TLS_KEY_PATH");
    if (tls_cert && tls_key) {
        if (server_enable_tls(&server, tls_cert, tls_key) != 0) {
            fputs("Warning: failed to enable TLS with provided cert/key. Continuing without TLS.\n", stderr);
        } else {
            puts("TLS enabled for HTTPS/WSS.");
        }
    } else {
        puts("TLS_CERT_PATH/TLS_KEY_PATH not set; running without TLS (dev only).");
    }

    server_set_websocket_context(&server, &ws_context);

    if (server_start(&server) != 0) {
        fputs("Failed to start TCP/WebSocket server.\n", stderr);
        exit_code = 1;
        goto cleanup;
    }
    server_started = 1;

    puts("Server is running on TCP:0.0.0.0:8443 (HTTPS/WSS) and UDP:0.0.0.0:9443 (QUIC).");

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
    if (db_initialized) {
        db_close(&db);
    }

    puts("OTT_QUIC server shutting down.");
    return exit_code;
}
