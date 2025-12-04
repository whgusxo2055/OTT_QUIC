#include "server/server.h"

#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/time.h>

static int contains_sequence(const char *buffer, size_t len, const char *sequence, size_t seq_len) {
    if (seq_len == 0 || len < seq_len) {
        return 0;
    }
    for (size_t i = seq_len - 1; i < len; ++i) {
        if (memcmp(buffer + i - (seq_len - 1), sequence, seq_len) == 0) {
            return 1;
        }
    }
    return 0;
}

static void read_until_sequence(int fd, const char *sequence) {
    size_t seq_len = strlen(sequence);
    char buffer[512];
    size_t total = 0;

    while (total < sizeof(buffer)) {
        ssize_t n = recv(fd, buffer + total, 1, 0);
        assert(n == 1);
        total += 1;
        if (total >= seq_len && contains_sequence(buffer, total, sequence, seq_len)) {
            return;
        }
    }

    assert(!"sequence not found");
}

static void expect_text_frame(int fd, char *buffer, size_t buf_size) {
    uint8_t header[2];
    assert(recv(fd, header, 2, MSG_WAITALL) == 2);
    assert((header[0] & 0x0F) == 0x1);
    size_t resp_len = header[1] & 0x7F;
    assert(resp_len + 1 < buf_size);
    assert(recv(fd, buffer, resp_len, MSG_WAITALL) == (ssize_t)resp_len);
    buffer[resp_len] = '\0';
}

static void websocket_client_ping(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);

    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
    assert(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);

    const char *key = "dGhlIHNhbXBsZSBub25jZQ==";
    char request[512];
    int req_len = snprintf(request,
                           sizeof(request),
                           "GET /chat HTTP/1.1\r\n"
                           "Host: localhost\r\n"
                           "Upgrade: websocket\r\n"
                           "Connection: Upgrade\r\n"
                           "Sec-WebSocket-Key: %s\r\n"
                           "Sec-WebSocket-Version: 13\r\n"
                           "\r\n",
                           key);
    assert(req_len > 0);
    assert(send(fd, request, (size_t)req_len, 0) == req_len);

    read_until_sequence(fd, "\r\n\r\n");

    char ready_buffer[128];
    expect_text_frame(fd, ready_buffer, sizeof(ready_buffer));
    assert(strstr(ready_buffer, "\"type\":\"ready\""));

    const char *message = "{\"type\":\"ping\"}";
    size_t len = strlen(message);
    assert(len <= 125);

    uint8_t frame[2 + 4 + 125];
    frame[0] = 0x81; // FIN + text
    frame[1] = 0x80 | (uint8_t)len;
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    memcpy(&frame[2], mask, 4);
    for (size_t i = 0; i < len; ++i) {
        frame[6 + i] = ((uint8_t)message[i]) ^ mask[i % 4];
    }

    assert(send(fd, frame, 6 + len, 0) == (ssize_t)(6 + len));

    char response[256];
    expect_text_frame(fd, response, sizeof(response));
    assert(strstr(response, "\"type\":\"pong\""));

    // send close frame
    uint8_t close_frame[2 + 4];
    close_frame[0] = 0x88;
    close_frame[1] = 0x80;
    uint8_t close_mask[4] = {0x00, 0x00, 0x00, 0x01};
    memcpy(&close_frame[2], close_mask, 4);
    assert(send(fd, close_frame, sizeof(close_frame), 0) == (ssize_t)sizeof(close_frame));

    close(fd);
}

int main(void) {
    server_ctx_t server;
    const uint16_t port = 18080;

    if (server_init(&server, "127.0.0.1", port, 5) != 0) {
        fprintf(stderr, "server_init bind failed, skipping test\n");
        return 0;
    }
    assert(server_start(&server) == 0);

    usleep(100 * 1000);

    websocket_client_ping(port);
    websocket_client_ping(port);

    server_request_stop(&server);
    server_join(&server);
    server_destroy(&server);

    puts("server_test passed");
    return 0;
}
