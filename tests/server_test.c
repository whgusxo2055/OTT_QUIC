#include "server/server.h"

#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void send_and_expect_echo(uint16_t port, const char *payload) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);

    size_t len = strlen(payload);
    assert(send(fd, payload, len, 0) == (ssize_t)len);

    char buffer[128] = {0};
    ssize_t received = recv(fd, buffer, sizeof(buffer), 0);
    assert(received == (ssize_t)len);
    assert(strncmp(buffer, payload, len) == 0);

    close(fd);
}

int main(void) {
    server_ctx_t server;
    const uint16_t port = 18080;

    assert(server_init(&server, "127.0.0.1", port, 5) == 0);
    assert(server_start(&server) == 0);

    usleep(100 * 1000);

    send_and_expect_echo(port, "hello");
    send_and_expect_echo(port, "world");

    server_request_stop(&server);
    server_join(&server);
    server_destroy(&server);

    puts("server_test passed");
    return 0;
}
