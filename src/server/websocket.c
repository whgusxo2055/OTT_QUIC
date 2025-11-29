#include "server/websocket.h"

#include "server/http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_MAX_HTTP_BUFFER 8192
#define WS_MAX_FRAME_PAYLOAD (64 * 1024)

typedef struct {
    uint8_t opcode;
    uint8_t fin;
    uint64_t payload_len;
    uint8_t *payload;
} ws_frame_t;

static int read_http_request(int fd, char *buffer, size_t buf_size, size_t *out_len);
static int send_http_error(int fd, const char *status_line);
static int perform_handshake(int fd, http_request_t *request);
static int validate_websocket_request(const http_request_t *req);
static int buffer_contains_clrf(const char *buf, size_t len);
static int ws_read_frame(int fd, ws_frame_t *frame);
static void ws_free_frame(ws_frame_t *frame);
static int ws_send_frame(int fd, uint8_t opcode, const uint8_t *payload, size_t payload_len);
static void sha1_compute(const uint8_t *data, size_t len, uint8_t out[20]);
static int base64_encode(const uint8_t *data, size_t len, char *out, size_t out_size);
static int read_exact(int fd, void *buf, size_t len);
static int write_all(int fd, const void *buf, size_t len);
static int header_contains_token(const char *value, const char *token);

int websocket_handle_client(int client_fd) {
    char buffer[WS_MAX_HTTP_BUFFER];
    size_t received = 0;
    if (read_http_request(client_fd, buffer, sizeof(buffer), &received) != 0) {
        send_http_error(client_fd, "HTTP/1.1 400 Bad Request\r\n\r\n");
        close(client_fd);
        return -1;
    }

    http_request_t request;
    if (http_parse_request(buffer, received, &request) != 0) {
        send_http_error(client_fd, "HTTP/1.1 400 Bad Request\r\n\r\n");
        close(client_fd);
        return -1;
    }

    if (validate_websocket_request(&request) != 0) {
        send_http_error(client_fd, "HTTP/1.1 400 Bad Request\r\n\r\n");
        close(client_fd);
        return -1;
    }

    if (perform_handshake(client_fd, &request) != 0) {
        send_http_error(client_fd, "HTTP/1.1 500 Internal Server Error\r\n\r\n");
        close(client_fd);
        return -1;
    }

    while (1) {
        ws_frame_t frame = {0};
        if (ws_read_frame(client_fd, &frame) != 0) {
            break;
        }

        if (frame.opcode == 0x1 || frame.opcode == 0x2 || frame.opcode == 0x0) {
            ws_send_frame(client_fd, frame.opcode ? frame.opcode : 0x1, frame.payload, (size_t)frame.payload_len);
        } else if (frame.opcode == 0x8) {
            ws_send_frame(client_fd, 0x8, frame.payload, (size_t)frame.payload_len);
            ws_free_frame(&frame);
            break;
        } else if (frame.opcode == 0x9) {
            ws_send_frame(client_fd, 0xA, frame.payload, (size_t)frame.payload_len);
        } else {
            ws_send_frame(client_fd, 0x8, NULL, 0);
            ws_free_frame(&frame);
            break;
        }

        ws_free_frame(&frame);
    }

    close(client_fd);
    return 0;
}

static int read_http_request(int fd, char *buffer, size_t buf_size, size_t *out_len) {
    size_t total = 0;

    while (total < buf_size) {
        ssize_t n = recv(fd, buffer + total, buf_size - total, 0);
        if (n <= 0) {
            return -1;
        }
        total += (size_t)n;
        if (buffer_contains_clrf(buffer, total)) {
            if (out_len) {
                *out_len = total;
            }
            return 0;
        }
    }

    return -1;
}

static int buffer_contains_clrf(const char *buf, size_t len) {
    if (len < 4) {
        return 0;
    }
    for (size_t i = 3; i < len; ++i) {
        if (buf[i - 3] == '\r' && buf[i - 2] == '\n' && buf[i - 1] == '\r' && buf[i] == '\n') {
            return 1;
        }
    }
    return 0;
}

static int send_http_error(int fd, const char *status_line) {
    return write_all(fd, status_line, strlen(status_line));
}

static int header_contains_token(const char *value, const char *token) {
    if (!value || !token) {
        return 0;
    }

    char buffer[HTTP_MAX_HEADER_VAL];
    strncpy(buffer, value, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    char *saveptr = NULL;
    char *part = strtok_r(buffer, ",", &saveptr);
    while (part) {
        while (*part == ' ' || *part == '\t') {
            part++;
        }
        if (strcasecmp(part, token) == 0) {
            return 1;
        }
        part = strtok_r(NULL, ",", &saveptr);
    }
    return 0;
}

static int validate_websocket_request(const http_request_t *req) {
    if (strcasecmp(req->method, "GET") != 0) {
        return -1;
    }

    const char *upgrade = http_get_header(req, "upgrade");
    if (!upgrade || strcasecmp(upgrade, "websocket") != 0) {
        return -1;
    }

    const char *connection = http_get_header(req, "connection");
    if (!connection || !header_contains_token(connection, "Upgrade")) {
        return -1;
    }

    const char *version = http_get_header(req, "sec-websocket-version");
    if (!version || strcmp(version, "13") != 0) {
        return -1;
    }

    const char *key = http_get_header(req, "sec-websocket-key");
    if (!key) {
        return -1;
    }

    return 0;
}

static int perform_handshake(int fd, http_request_t *request) {
    const char *client_key = http_get_header(request, "sec-websocket-key");
    if (!client_key) {
        return -1;
    }

    char accept_value[64];
    if (websocket_calculate_accept_key(client_key, accept_value, sizeof(accept_value)) != 0) {
        return -1;
    }

    char response[256];
    int len = snprintf(response,
                       sizeof(response),
                       "HTTP/1.1 101 Switching Protocols\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Accept: %s\r\n"
                       "\r\n",
                       accept_value);
    if (len <= 0 || (size_t)len >= sizeof(response)) {
        return -1;
    }

    return write_all(fd, response, (size_t)len);
}

int websocket_calculate_accept_key(const char *client_key, char *out, size_t out_size) {
    if (!client_key || !out) {
        return -1;
    }

    size_t key_len = strlen(client_key);
    char combined[128];
    if (key_len + strlen(WS_GUID) >= sizeof(combined)) {
        return -1;
    }

    strcpy(combined, client_key);
    strcat(combined, WS_GUID);

    uint8_t hash[20];
    sha1_compute((const uint8_t *)combined, strlen(combined), hash);

    if (base64_encode(hash, sizeof(hash), out, out_size) != 0) {
        return -1;
    }

    return 0;
}

void websocket_apply_mask(uint8_t *data, size_t len, const uint8_t mask[4]) {
    if (!data || !mask) {
        return;
    }
    for (size_t i = 0; i < len; ++i) {
        data[i] ^= mask[i % 4];
    }
}

static int ws_read_frame(int fd, ws_frame_t *frame) {
    uint8_t header[2];
    if (read_exact(fd, header, sizeof(header)) != 0) {
        return -1;
    }

    frame->fin = (header[0] & 0x80) != 0;
    frame->opcode = header[0] & 0x0F;
    uint8_t mask_flag = (header[1] & 0x80) != 0;
    uint64_t payload_len = header[1] & 0x7F;

    if (payload_len == 126) {
        uint8_t extended[2];
        if (read_exact(fd, extended, sizeof(extended)) != 0) {
            return -1;
        }
        payload_len = (extended[0] << 8) | extended[1];
    } else if (payload_len == 127) {
        uint8_t extended[8];
        if (read_exact(fd, extended, sizeof(extended)) != 0) {
            return -1;
        }
        payload_len = 0;
        for (int i = 0; i < 8; ++i) {
            payload_len = (payload_len << 8) | extended[i];
        }
    }

    if (payload_len > WS_MAX_FRAME_PAYLOAD) {
        return -1;
    }

    uint8_t mask_key[4] = {0};
    if (mask_flag) {
        if (read_exact(fd, mask_key, sizeof(mask_key)) != 0) {
            return -1;
        }
    }

    frame->payload_len = payload_len;
    frame->payload = NULL;
    if (payload_len > 0) {
        frame->payload = malloc((size_t)payload_len);
        if (!frame->payload) {
            return -1;
        }
        if (read_exact(fd, frame->payload, (size_t)payload_len) != 0) {
            free(frame->payload);
            frame->payload = NULL;
            return -1;
        }
        if (mask_flag) {
            websocket_apply_mask(frame->payload, (size_t)payload_len, mask_key);
        }
    }

    return 0;
}

static void ws_free_frame(ws_frame_t *frame) {
    if (frame && frame->payload) {
        free(frame->payload);
        frame->payload = NULL;
    }
}

static int ws_send_frame(int fd, uint8_t opcode, const uint8_t *payload, size_t payload_len) {
    uint8_t header[10];
    size_t header_len = 0;

    header[0] = 0x80 | (opcode & 0x0F);
    if (payload_len <= 125) {
        header[1] = (uint8_t)payload_len;
        header_len = 2;
    } else if (payload_len <= 0xFFFF) {
        header[1] = 126;
        header[2] = (uint8_t)((payload_len >> 8) & 0xFF);
        header[3] = (uint8_t)(payload_len & 0xFF);
        header_len = 4;
    } else {
        header[1] = 127;
        for (int i = 0; i < 8; ++i) {
            header[2 + i] = (uint8_t)((payload_len >> (56 - 8 * i)) & 0xFF);
        }
        header_len = 10;
    }

    if (write_all(fd, header, header_len) != 0) {
        return -1;
    }

    if (payload_len > 0 && write_all(fd, payload, payload_len) != 0) {
        return -1;
    }

    return 0;
}

static int read_exact(int fd, void *buf, size_t len) {
    uint8_t *ptr = buf;
    size_t total = 0;
    while (total < len) {
        ssize_t n = recv(fd, ptr + total, len - total, 0);
        if (n <= 0) {
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

static int write_all(int fd, const void *buf, size_t len) {
    const uint8_t *ptr = buf;
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(fd, ptr + total, len - total, MSG_NOSIGNAL);
        if (n <= 0) {
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

typedef struct {
    uint32_t state[5];
    uint64_t bitlen;
    uint8_t data[64];
    size_t datalen;
} sha1_ctx;

static void sha1_transform(sha1_ctx *ctx, const uint8_t data[64]);

static void sha1_init(sha1_ctx *ctx) {
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->bitlen = 0;
    ctx->datalen = 0;
}

static uint32_t leftrotate(uint32_t value, uint32_t bits) {
    return (value << bits) | (value >> (32 - bits));
}

static void sha1_update(sha1_ctx *ctx, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64) {
            sha1_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void sha1_final(sha1_ctx *ctx, uint8_t hash[20]) {
    ctx->bitlen += ctx->datalen * 8;

    ctx->data[ctx->datalen++] = 0x80;
    if (ctx->datalen > 56) {
        while (ctx->datalen < 64) {
            ctx->data[ctx->datalen++] = 0x00;
        }
        sha1_transform(ctx, ctx->data);
        ctx->datalen = 0;
    }

    while (ctx->datalen < 56) {
        ctx->data[ctx->datalen++] = 0x00;
    }

    for (int i = 7; i >= 0; --i) {
        ctx->data[ctx->datalen++] = (uint8_t)((ctx->bitlen >> (i * 8)) & 0xFF);
    }

    sha1_transform(ctx, ctx->data);

    for (int i = 0; i < 5; ++i) {
        hash[i * 4 + 0] = (uint8_t)((ctx->state[i] >> 24) & 0xFF);
        hash[i * 4 + 1] = (uint8_t)((ctx->state[i] >> 16) & 0xFF);
        hash[i * 4 + 2] = (uint8_t)((ctx->state[i] >> 8) & 0xFF);
        hash[i * 4 + 3] = (uint8_t)(ctx->state[i] & 0xFF);
    }
}

static void sha1_transform(sha1_ctx *ctx, const uint8_t data[64]) {
    uint32_t w[80];

    for (int i = 0; i < 16; ++i) {
        w[i] = (data[i * 4] << 24) | (data[i * 4 + 1] << 16) | (data[i * 4 + 2] << 8) | data[i * 4 + 3];
    }

    for (int i = 16; i < 80; ++i) {
        w[i] = leftrotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];

    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }

        uint32_t temp = leftrotate(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = leftrotate(b, 30);
        b = a;
        a = temp;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
}

static void sha1_compute(const uint8_t *data, size_t len, uint8_t out[20]) {
    sha1_ctx ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, out);
}

static int base64_encode(const uint8_t *data, size_t len, char *out, size_t out_size) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    size_t encoded_len = ((len + 2) / 3) * 4;
    if (out_size < encoded_len + 1) {
        return -1;
    }

    size_t j = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t octet_a = i < len ? data[i] : 0;
        uint32_t octet_b = (i + 1) < len ? data[i + 1] : 0;
        uint32_t octet_c = (i + 2) < len ? data[i + 2] : 0;

        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        out[j++] = table[(triple >> 18) & 0x3F];
        out[j++] = table[(triple >> 12) & 0x3F];
        out[j++] = (i + 1) < len ? table[(triple >> 6) & 0x3F] : '=';
        out[j++] = (i + 2) < len ? table[triple & 0x3F] : '=';
    }
    out[j] = '\0';

    return 0;
}
