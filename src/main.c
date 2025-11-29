#include "server/server.h"

#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static volatile sig_atomic_t keep_running = 1;

static void handle_signal(int signo) {
    (void)signo;
    keep_running = 0;
}

int main(void) {
    puts("OTT_QUIC TCP server starting.");

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    server_ctx_t server;
    if (server_init(&server, "0.0.0.0", 8080, 5) != 0) {
        fputs("Failed to initialize server context.\n", stderr);
        return 1;
    }

    if (server_start(&server) != 0) {
        fputs("Failed to start server.\n", stderr);
        server_destroy(&server);
        return 1;
    }

    puts("Server is running on 0.0.0.0:8080.");

    while (keep_running) {
        sleep(1);
    }

    puts("Shutdown signal received. Stopping server...");
    server_request_stop(&server);
    server_join(&server);
    server_destroy(&server);

    puts("OTT_QUIC server shutting down.");
    return 0;
}
