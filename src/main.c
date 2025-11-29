#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static volatile sig_atomic_t keep_running = 1;

static void handle_signal(int signo) {
    (void)signo;
    keep_running = 0;
}

int main(void) {
    puts("OTT_QUIC server bootstrap running.");
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    while (keep_running) {
        sleep(1);
    }

    puts("OTT_QUIC server shutting down.");
    return 0;
}
