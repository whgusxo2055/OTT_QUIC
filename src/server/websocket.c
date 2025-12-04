#include "server/websocket.h"

#include "server/http.h"
#include "http/api.h"
#ifdef ENABLE_TLS
#include <openssl/ssl.h>
#endif
#include "auth/session.h"
#include "db/database.h"
#include "utils/thumbnail.h"
#include "server/upload.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_MAX_HTTP_BUFFER 8192
#define WS_MAX_FRAME_PAYLOAD (64 * 1024)
#define STREAM_MAX_CHUNK (1024 * 1024)
#define VIDEO_BASE_PATH "data/videos"

typedef struct {
    int fd;
    SSL *ssl; /* optional */
} ws_io_t;

typedef struct {
    uint8_t opcode;
    uint8_t fin;
    uint64_t payload_len;
    uint8_t *payload;
} ws_frame_t;

typedef enum {
    WS_CMD_UNKNOWN = 0,
    WS_CMD_PING,
    WS_CMD_QUIC_SEND,
    WS_CMD_LIST_VIDEOS,
    WS_CMD_VIDEO_DETAIL,
    WS_CMD_STREAM_START,
    WS_CMD_STREAM_CHUNK,
    WS_CMD_WATCH_GET,
    WS_CMD_WATCH_UPDATE,
    WS_CMD_STREAM_SEEK,
    WS_CMD_STREAM_STOP,
    WS_CMD_WS_INIT,
    WS_CMD_WS_SEGMENT,
    WS_CMD_LIST_CONTINUE
} ws_command_type;

typedef struct {
    ws_command_type type;
    uint64_t connection_id;
    uint32_t stream_id;
    uint32_t offset;
    uint8_t payload[QUIC_MAX_PAYLOAD];
    size_t payload_len;
    int video_id;
    uint32_t chunk_length;
    uint32_t length;
    int position;
    uint32_t seek_offset;
    int segment_index;
} ws_command_t;

static int io_read(ws_io_t *io, void *buf, size_t len);
static int io_write(ws_io_t *io, const void *buf, size_t len);
static int read_http_request(ws_io_t *io, char *buffer, size_t buf_size, size_t *out_len);
static int send_http_error(ws_io_t *io, const char *status_line);
static int perform_handshake(ws_io_t *io, http_request_t *request);
static int validate_websocket_request(const http_request_t *req);
static int buffer_contains_clrf(const char *buf, size_t len);
static int ws_read_frame(ws_io_t *io, ws_frame_t *frame);
static void ws_free_frame(ws_frame_t *frame);
static int ws_send_frame(ws_io_t *io, uint8_t opcode, const uint8_t *payload, size_t payload_len);
static void sha1_compute(const uint8_t *data, size_t len, uint8_t out[20]);
static int base64_encode(const uint8_t *data, size_t len, char *out, size_t out_size);
static int read_exact(ws_io_t *io, void *buf, size_t len);
static int write_all(ws_io_t *io, const void *buf, size_t len);
static int header_contains_token(const char *value, const char *token);
static const char *json_find_value(const char *json, const char *key);
static int json_extract_string_field(const char *json, const char *key, char *out, size_t out_size);
static int json_extract_uint64_field(const char *json, const char *key, uint64_t *out);
static int json_extract_uint32_field(const char *json, const char *key, uint32_t *out);
static int json_extract_int_field(const char *json, const char *key, int *out);
static int parse_command(const char *text, ws_command_t *cmd);
static int hex_to_bytes(const char *hex, uint8_t *out, size_t max_len, size_t *out_len);
static int handle_text_frame(ws_io_t *io, websocket_context_t *ctx, const ws_frame_t *frame, int user_id);
static int send_json_response(ws_io_t *io, const char *type, const char *status, const char *message);
static int send_video_chunk(websocket_context_t *ctx,
                            uint64_t connection_id,
                            uint32_t stream_id,
                            const char *file_path,
                            uint32_t offset,
                            uint32_t length,
                            uint32_t *next_packet_number);
static int send_ws_file(ws_io_t *io, const char *path, const char magic[4], uint32_t index) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    long size = ftell(fp);
    if (size <= 0) {
        fclose(fp);
        return -1;
    }
    rewind(fp);

    size_t total = (size_t)size + 8;
    uint8_t *buf = malloc(total);
    if (!buf) {
        fclose(fp);
        return -1;
    }
    memcpy(buf, magic, 4);
    buf[4] = (uint8_t)((index >> 24) & 0xFF);
    buf[5] = (uint8_t)((index >> 16) & 0xFF);
    buf[6] = (uint8_t)((index >> 8) & 0xFF);
    buf[7] = (uint8_t)(index & 0xFF);

    size_t n = fread(buf + 8, 1, (size_t)size, fp);
    fclose(fp);
    if (n != (size_t)size) {
        free(buf);
        return -1;
    }

    int rc = ws_send_frame(io, 0x2, buf, total);
    free(buf);
    return rc;
}

void websocket_context_init(websocket_context_t *ctx, quic_engine_t *engine, db_context_t *db) {
    if (!ctx) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->quic_engine = engine;
    ctx->db = db;
    pthread_mutex_init(&ctx->lock, NULL);
    ctx->next_packet_number = 1;
    ctx->segment_sent_ok = 0;
    ctx->segment_sent_fail = 0;
}

void websocket_context_destroy(websocket_context_t *ctx) {
    if (!ctx) {
        return;
    }
    pthread_mutex_destroy(&ctx->lock);
    memset(ctx, 0, sizeof(*ctx));
}

static int send_video_chunk(websocket_context_t *ctx,
                            uint64_t connection_id,
                            uint32_t stream_id,
                            const char *file_path,
                            uint32_t offset,
                            uint32_t length,
                            uint32_t *next_packet_number) {
    if (!ctx || !ctx->quic_engine || !file_path || !next_packet_number) {
        return -1;
    }

    struct stat st;
    if (stat(file_path, &st) != 0) {
        return -1;
    }
    if (offset >= (uint32_t)st.st_size) {
        return -1;
    }

    FILE *fp = fopen(file_path, "rb");
    if (!fp) {
        return -1;
    }
    if (fseek(fp, (long)offset, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }

    uint32_t remaining = length;
    uint32_t sent_bytes = 0;
    uint8_t buffer[QUIC_MAX_PAYLOAD];

    while (remaining > 0 && sent_bytes + offset < (uint32_t)st.st_size) {
        size_t to_read = remaining;
        if (to_read > QUIC_MAX_PAYLOAD) {
            to_read = QUIC_MAX_PAYLOAD;
        }
        size_t n = fread(buffer, 1, to_read, fp);
        if (n == 0) {
            break;
        }
        quic_packet_t pkt = {
            .flags = QUIC_FLAG_DATA,
            .connection_id = connection_id,
            .packet_number = (*next_packet_number)++,
            .stream_id = stream_id,
            .offset = offset + sent_bytes,
            .length = (uint32_t)n,
            .payload = buffer,
        };
        if (quic_engine_send_to_connection(ctx->quic_engine, &pkt) != 0) {
            fclose(fp);
            return -1;
        }
        sent_bytes += (uint32_t)n;
        if (sent_bytes + offset >= (uint32_t)st.st_size) {
            break;
        }
        if (remaining >= (uint32_t)n) {
            remaining -= (uint32_t)n;
        } else {
            remaining = 0;
        }
    }

    fclose(fp);
    return 0;
}

int websocket_handle_client(int client_fd, SSL *ssl, websocket_context_t *ctx) {
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    ws_io_t io = {.fd = client_fd, .ssl = ssl};

    char buffer[WS_MAX_HTTP_BUFFER];
    size_t received = 0;
    if (read_http_request(&io, buffer, sizeof(buffer), &received) != 0) {
        send_http_error(&io, "HTTP/1.1 400 Bad Request\r\n\r\n");
        close(client_fd);
        return -1;
    }

    http_request_t request;
    if (http_parse_request(buffer, received, &request) != 0) {
        send_http_error(&io, "HTTP/1.1 400 Bad Request\r\n\r\n");
        close(client_fd);
        return -1;
    }

    if (validate_websocket_request(&request) != 0) {
        const char *sep = strstr(buffer, "\r\n\r\n");
        size_t header_size = sep ? (size_t)(sep - buffer + 4) : received;
        size_t already = received > header_size ? received - header_size : 0;
        if (ctx && ctx->db) {
            int rc = http_api_handle(client_fd, io.ssl, &request, ctx, buffer + header_size, already);
            close(client_fd);
            return rc;
        }
        send_http_error(&io, "HTTP/1.1 400 Bad Request\r\n\r\n");
        close(client_fd);
        return -1;
    }

    int current_user_id = 0;
    /* optional: extend session if provided */
    if (ctx && ctx->db) {
        char sid[SESSION_ID_LEN + 1];
        if (session_extract_from_headers(&request, sid, sizeof(sid)) == 0) {
            session_validate_and_extend(ctx->db, sid, SESSION_TTL_SECONDS, &current_user_id);
        }
    }

    if (perform_handshake(&io, &request) != 0) {
        send_http_error(&io, "HTTP/1.1 500 Internal Server Error\r\n\r\n");
        close(client_fd);
        return -1;
    }

    if (send_json_response(&io, "ready", "ok", "websocket-ready") != 0) {
        close(client_fd);
        return -1;
    }

    while (1) {
        ws_frame_t frame = {0};
        if (ws_read_frame(&io, &frame) != 0) {
            break;
        }

        if (frame.opcode == 0x1) {
            if (handle_text_frame(&io, ctx, &frame, current_user_id) != 0 && frame.payload) {
                ws_send_frame(&io, 0x1, frame.payload, (size_t)frame.payload_len);
            }
        } else if (frame.opcode == 0x2 || frame.opcode == 0x0) {
            ws_send_frame(&io, frame.opcode ? frame.opcode : 0x1, frame.payload, (size_t)frame.payload_len);
        } else if (frame.opcode == 0x8) {
            ws_send_frame(&io, 0x8, frame.payload, (size_t)frame.payload_len);
            ws_free_frame(&frame);
            break;
        } else if (frame.opcode == 0x9) {
            ws_send_frame(&io, 0xA, frame.payload, (size_t)frame.payload_len);
        } else {
            ws_send_frame(&io, 0x8, NULL, 0);
            ws_free_frame(&frame);
            break;
        }

        ws_free_frame(&frame);
    }

    close(client_fd);
    return 0;
}

static int read_http_request(ws_io_t *io, char *buffer, size_t buf_size, size_t *out_len) {
    size_t total = 0;

    while (total < buf_size) {
        int n = io_read(io, buffer + total, buf_size - total);
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

static int send_http_error(ws_io_t *io, const char *status_line) {
    return write_all(io, status_line, strlen(status_line));
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

static int perform_handshake(ws_io_t *io, http_request_t *request) {
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

    return write_all(io, response, (size_t)len);
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

static int ws_read_frame(ws_io_t *io, ws_frame_t *frame) {
    uint8_t header[2];
    if (read_exact(io, header, sizeof(header)) != 0) {
        return -1;
    }

    frame->fin = (header[0] & 0x80) != 0;
    frame->opcode = header[0] & 0x0F;
    uint8_t mask_flag = (header[1] & 0x80) != 0;
    uint64_t payload_len = header[1] & 0x7F;

    if (payload_len == 126) {
        uint8_t extended[2];
        if (read_exact(io, extended, sizeof(extended)) != 0) {
            return -1;
        }
        payload_len = (extended[0] << 8) | extended[1];
    } else if (payload_len == 127) {
        uint8_t extended[8];
        if (read_exact(io, extended, sizeof(extended)) != 0) {
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
        if (read_exact(io, mask_key, sizeof(mask_key)) != 0) {
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
        if (read_exact(io, frame->payload, (size_t)payload_len) != 0) {
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

static int ws_send_frame(ws_io_t *io, uint8_t opcode, const uint8_t *payload, size_t payload_len) {
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

    if (write_all(io, header, header_len) != 0) {
        return -1;
    }

    if (payload_len > 0 && write_all(io, payload, payload_len) != 0) {
        return -1;
    }

    return 0;
}

static int io_read(ws_io_t *io, void *buf, size_t len) {
    if (!io || len == 0) {
        return -1;
    }
#ifdef ENABLE_TLS
    if (io->ssl) {
        int n = SSL_read(io->ssl, buf, (int)len);
        return n <= 0 ? -1 : n;
    }
#endif
    ssize_t n = recv(io->fd, buf, len, 0);
    return n <= 0 ? -1 : (int)n;
}

static int io_write(ws_io_t *io, const void *buf, size_t len) {
    if (!io || len == 0) {
        return -1;
    }
#ifdef ENABLE_TLS
    if (io->ssl) {
        int n = SSL_write(io->ssl, buf, (int)len);
        return n <= 0 ? -1 : n;
    }
#endif
    ssize_t n = send(io->fd, buf, len, MSG_NOSIGNAL);
    return n <= 0 ? -1 : (int)n;
}

static int read_exact(ws_io_t *io, void *buf, size_t len) {
    uint8_t *ptr = buf;
    size_t total = 0;
    while (total < len) {
        int n = io_read(io, ptr + total, len - total);
        if (n <= 0) {
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

static int write_all(ws_io_t *io, const void *buf, size_t len) {
    const uint8_t *ptr = buf;
    size_t total = 0;
    while (total < len) {
        int n = io_write(io, ptr + total, len - total);
        if (n <= 0) {
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

static int send_json_response(ws_io_t *io, const char *type, const char *status, const char *message) {
    char payload[256];
    const char *safe_message = message ? message : "";
    int len = snprintf(payload,
                       sizeof(payload),
                       "{\"type\":\"%s\",\"status\":\"%s\",\"message\":\"%s\"}",
                       type ? type : "event",
                       status ? status : "ok",
                       safe_message);
    if (len <= 0) {
        return -1;
    }
    if ((size_t)len >= sizeof(payload)) {
        len = (int)sizeof(payload) - 1;
        payload[len] = '\0';
    }
    return ws_send_frame(io, 0x1, (const uint8_t *)payload, (size_t)len);
}

static const char *json_find_value(const char *json, const char *key) {
    if (!json || !key) {
        return NULL;
    }
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *pos = strstr(json, pattern);
    if (!pos) {
        return NULL;
    }
    pos = strchr(pos + strlen(pattern), ':');
    if (!pos) {
        return NULL;
    }
    pos++;
    while (*pos && isspace((unsigned char)*pos)) {
        pos++;
    }
    return pos;
}

static int json_extract_string_field(const char *json, const char *key, char *out, size_t out_size) {
    if (!json || !key || !out || out_size == 0) {
        return -1;
    }
    const char *value = json_find_value(json, key);
    if (!value || *value != '\"') {
        return -1;
    }
    value++;
    const char *end = strchr(value, '\"');
    if (!end) {
        return -1;
    }
    size_t copy_len = (size_t)(end - value);
    if (copy_len >= out_size) {
        copy_len = out_size - 1;
    }
    memcpy(out, value, copy_len);
    out[copy_len] = '\0';
    return 0;
}

static int json_extract_uint64_field(const char *json, const char *key, uint64_t *out) {
    if (!json || !key || !out) {
        return -1;
    }
    const char *value = json_find_value(json, key);
    if (!value) {
        return -1;
    }
    char *endptr = NULL;
    unsigned long long parsed = strtoull(value, &endptr, 10);
    if (value == endptr) {
        return -1;
    }
    *out = (uint64_t)parsed;
    return 0;
}

static int json_extract_uint32_field(const char *json, const char *key, uint32_t *out) {
    uint64_t tmp = 0;
    if (json_extract_uint64_field(json, key, &tmp) != 0) {
        return -1;
    }
    if (tmp > UINT32_MAX) {
        return -1;
    }
    *out = (uint32_t)tmp;
    return 0;
}

static int json_extract_int_field(const char *json, const char *key, int *out) {
    if (!json || !key || !out) {
        return -1;
    }
    const char *value = json_find_value(json, key);
    if (!value) {
        return -1;
    }
    char *endptr = NULL;
    long parsed = strtol(value, &endptr, 10);
    if (value == endptr) {
        return -1;
    }
    *out = (int)parsed;
    return 0;
}

static int hex_to_bytes(const char *hex, uint8_t *out, size_t max_len, size_t *out_len) {
    if (!hex || !out) {
        return -1;
    }
    size_t hex_len = strlen(hex);
    if (hex_len == 0) {
        *out_len = 0;
        return 0;
    }
    if (hex_len % 2 != 0) {
        return -1;
    }
    size_t bytes_needed = hex_len / 2;
    if (bytes_needed > max_len) {
        return -1;
    }
    for (size_t i = 0; i < bytes_needed; ++i) {
        char byte_str[3] = {hex[2 * i], hex[2 * i + 1], '\0'};
        char *endptr = NULL;
        long value = strtol(byte_str, &endptr, 16);
        if (endptr == byte_str) {
            return -1;
        }
        out[i] = (uint8_t)value;
    }
    if (out_len) {
        *out_len = bytes_needed;
    }
    return 0;
}

static int parse_command(const char *text, ws_command_t *cmd) {
    if (!text || !cmd) {
        return -1;
    }

    memset(cmd, 0, sizeof(*cmd));
    char type[32];
    if (json_extract_string_field(text, "type", type, sizeof(type)) != 0) {
        return -1;
    }

    if (strcmp(type, "ping") == 0) {
        cmd->type = WS_CMD_PING;
        return 0;
    }

    if (strcmp(type, "quic_send") == 0) {
        cmd->type = WS_CMD_QUIC_SEND;
        if (json_extract_uint64_field(text, "connection_id", &cmd->connection_id) != 0) {
            return -1;
        }
        if (json_extract_uint32_field(text, "stream_id", &cmd->stream_id) != 0) {
            cmd->stream_id = 1;
        }
    if (json_extract_uint32_field(text, "offset", &cmd->offset) != 0) {
        cmd->offset = 0;
    }
    char payload_hex[QUIC_MAX_PAYLOAD * 2 + 1];
    if (json_extract_string_field(text, "payload_hex", payload_hex, sizeof(payload_hex)) == 0) {
            if (hex_to_bytes(payload_hex, cmd->payload, sizeof(cmd->payload), &cmd->payload_len) != 0) {
                return -1;
            }
        } else {
            cmd->payload_len = 0;
        }
        return 0;
    }

    if (strcmp(type, "list_videos") == 0) {
        cmd->type = WS_CMD_LIST_VIDEOS;
        return 0;
    }

    if (strcmp(type, "list_continue") == 0) {
        cmd->type = WS_CMD_LIST_CONTINUE;
        return 0;
    }

    if (strcmp(type, "video_detail") == 0) {
        cmd->type = WS_CMD_VIDEO_DETAIL;
        if (json_extract_int_field(text, "video_id", &cmd->video_id) != 0) {
            return -1;
        }
        return 0;
    }

    if (strcmp(type, "watch_get") == 0) {
        cmd->type = WS_CMD_WATCH_GET;
        if (json_extract_int_field(text, "video_id", &cmd->video_id) != 0) {
            return -1;
        }
        return 0;
    }

    if (strcmp(type, "watch_update") == 0) {
        cmd->type = WS_CMD_WATCH_UPDATE;
        if (json_extract_int_field(text, "video_id", &cmd->video_id) != 0) {
            return -1;
        }
        if (json_extract_int_field(text, "position", &cmd->position) != 0) {
            return -1;
        }
        return 0;
    }

    if (strcmp(type, "stream_start") == 0) {
        cmd->type = WS_CMD_STREAM_START;
        if (json_extract_int_field(text, "video_id", &cmd->video_id) != 0) {
            return -1;
        }
        return 0;
    }

    if (strcmp(type, "stream_chunk") == 0) {
        cmd->type = WS_CMD_STREAM_CHUNK;
        if (json_extract_int_field(text, "video_id", &cmd->video_id) != 0) {
            return -1;
        }
        if (json_extract_uint32_field(text, "offset", &cmd->offset) != 0) {
            return -1;
        }
        if (json_extract_uint32_field(text, "length", &cmd->length) != 0) {
            return -1;
        }
        if (json_extract_uint64_field(text, "connection_id", &cmd->connection_id) != 0) {
            return -1;
        }
        if (json_extract_uint32_field(text, "stream_id", &cmd->stream_id) != 0) {
            cmd->stream_id = 1;
        }
        return 0;
    }

    if (strcmp(type, "ws_init") == 0) {
        cmd->type = WS_CMD_WS_INIT;
        if (json_extract_int_field(text, "video_id", &cmd->video_id) != 0) {
            return -1;
        }
        return 0;
    }

    if (strcmp(type, "ws_segment") == 0) {
        cmd->type = WS_CMD_WS_SEGMENT;
        if (json_extract_int_field(text, "video_id", &cmd->video_id) != 0) {
            return -1;
        }
        if (json_extract_int_field(text, "segment", &cmd->segment_index) != 0) {
            return -1;
        }
        return 0;
    }

    return -1;
}

static int handle_text_frame(ws_io_t *io, websocket_context_t *ctx, const ws_frame_t *frame, int user_id) {
    if (!frame->payload || frame->payload_len == 0) {
        return send_json_response(io, "error", "bad_request", "empty-payload");
    }

    char *text = malloc((size_t)frame->payload_len + 1);
    if (!text) {
        return send_json_response(io, "error", "internal_error", "alloc-failed");
    }
    memcpy(text, frame->payload, (size_t)frame->payload_len);
    text[frame->payload_len] = '\0';

    ws_command_t cmd;
    if (parse_command(text, &cmd) != 0) {
        free(text);
        return send_json_response(io, "error", "bad_request", "unknown-command");
    }
    free(text);

    if (cmd.type == WS_CMD_PING) {
        return send_json_response(io, "pong", "ok", "alive");
    }

    if (cmd.type == WS_CMD_QUIC_SEND) {
        if (!ctx || !ctx->quic_engine) {
            return send_json_response(io, "error", "unavailable", "quic-engine-missing");
        }

        quic_connection_state_t state;
        if (quic_engine_get_connection_state(ctx->quic_engine, cmd.connection_id, &state) != 0) {
            return send_json_response(io, "error", "connection-not-found", "quic-connection-not-found");
        }
        if (state != QUIC_CONN_STATE_CONNECTED) {
            const char *state_str = "unknown";
            if (state == QUIC_CONN_STATE_CONNECTING) {
                state_str = "connecting";
            } else if (state == QUIC_CONN_STATE_CLOSED) {
                state_str = "closed";
            } else if (state == QUIC_CONN_STATE_IDLE) {
                state_str = "idle";
            }
            char detail[128];
            snprintf(detail, sizeof(detail), "quic-connection-not-ready(%s)", state_str);
            return send_json_response(io, "error", "connection-not-ready", detail);
        }

        quic_packet_t packet = {
            .flags = QUIC_FLAG_DATA,
            .connection_id = cmd.connection_id,
            .stream_id = cmd.stream_id,
            .offset = cmd.offset,
            .length = (uint32_t)cmd.payload_len,
            .payload = cmd.payload,
        };

        pthread_mutex_lock(&ctx->lock);
        packet.packet_number = ctx->next_packet_number++;
        pthread_mutex_unlock(&ctx->lock);

        if (quic_engine_send_to_connection(ctx->quic_engine, &packet) != 0) {
            return send_json_response(io, "error", "quic_send_failed", "connection-not-found");
        }

        char detail[128];
        snprintf(detail,
                 sizeof(detail),
                 "sent-pn-%u",
                 packet.packet_number);
        return send_json_response(io, "quic_send", "ok", detail);
    }

    if (cmd.type == WS_CMD_LIST_VIDEOS) {
        if (!ctx || !ctx->db) {
            return send_json_response(io, "error", "unavailable", "db-missing");
        }
        sqlite3_stmt *stmt = NULL;
        const char *sql = "SELECT id, title, IFNULL(description,''), IFNULL(thumbnail_path,''), IFNULL(duration,0) "
                          "FROM videos ORDER BY id DESC LIMIT 20;";
        if (sqlite3_prepare_v2(ctx->db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) {
            return send_json_response(io, "error", "db_error", "list-failed");
        }
        char buf[4096];
        size_t pos = 0;
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "{\"type\":\"videos\",\"items\":[");
        int first = 1;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (!first) {
                buf[pos++] = ',';
            }
            first = 0;
            int id = sqlite3_column_int(stmt, 0);
            const unsigned char *title = sqlite3_column_text(stmt, 1);
            const unsigned char *desc = sqlite3_column_text(stmt, 2);
            const unsigned char *thumb = sqlite3_column_text(stmt, 3);
            int duration = sqlite3_column_int(stmt, 4);
            pos += (size_t)snprintf(buf + pos,
                                    sizeof(buf) - pos,
                                    "{\"id\":%d,\"title\":\"%s\",\"description\":\"%s\",\"thumbnail_path\":\"%s\",\"duration\":%d}",
                                    id,
                                    title ? (const char *)title : "",
                                    desc ? (const char *)desc : "",
                                    thumb ? (const char *)thumb : "",
                                    duration);
            if (pos >= sizeof(buf) - 1) {
                break;
            }
        }
        sqlite3_finalize(stmt);
        if (pos >= sizeof(buf) - 2) {
            return send_json_response(io, "error", "internal_error", "response-too-large");
        }
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "]}");
        return ws_send_frame(io, 0x1, (const uint8_t *)buf, pos);
    }

    if (cmd.type == WS_CMD_LIST_CONTINUE) {
        if (!ctx || !ctx->db) {
            return send_json_response(io, "error", "unavailable", "db-missing");
        }
        if (user_id <= 0) {
            return send_json_response(io, "error", "unauthorized", "login-required");
        }
        sqlite3_stmt *stmt = NULL;
        const char *sql = "SELECT v.id, v.title, IFNULL(v.description,''), IFNULL(v.thumbnail_path,''), IFNULL(v.duration,0), w.last_position "
                          "FROM watch_history w JOIN videos v ON w.video_id = v.id "
                          "WHERE w.user_id = ? AND w.last_position > 10 ORDER BY w.updated_at DESC LIMIT 10;";
        if (sqlite3_prepare_v2(ctx->db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) {
            return send_json_response(io, "error", "db_error", "list-continue-failed");
        }
        sqlite3_bind_int(stmt, 1, user_id);
        
        char buf[4096];
        size_t pos = 0;
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "{\"type\":\"continue_videos\",\"items\":[");
        int first = 1;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (!first) {
                buf[pos++] = ',';
            }
            first = 0;
            int id = sqlite3_column_int(stmt, 0);
            const unsigned char *title = sqlite3_column_text(stmt, 1);
            const unsigned char *desc = sqlite3_column_text(stmt, 2);
            const unsigned char *thumb = sqlite3_column_text(stmt, 3);
            int duration = sqlite3_column_int(stmt, 4);
            int position = sqlite3_column_int(stmt, 5);
            pos += (size_t)snprintf(buf + pos,
                                    sizeof(buf) - pos,
                                    "{\"id\":%d,\"title\":\"%s\",\"description\":\"%s\",\"thumbnail_path\":\"%s\",\"duration\":%d,\"position\":%d}",
                                    id,
                                    title ? (const char *)title : "",
                                    desc ? (const char *)desc : "",
                                    thumb ? (const char *)thumb : "",
                                    duration,
                                    position);
            if (pos >= sizeof(buf) - 1) {
                break;
            }
        }
        sqlite3_finalize(stmt);
        if (pos >= sizeof(buf) - 2) {
            return send_json_response(io, "error", "internal_error", "response-too-large");
        }
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "]}");
        return ws_send_frame(io, 0x1, (const uint8_t *)buf, pos);
    }

    if (cmd.type == WS_CMD_VIDEO_DETAIL) {
        if (!ctx || !ctx->db) {
            return send_json_response(io, "error", "unavailable", "db-missing");
        }
        const char *sql =
            "SELECT id, title, IFNULL(description,''), file_path, IFNULL(thumbnail_path,''), "
            "IFNULL(duration,0), IFNULL(upload_date,'') FROM videos WHERE id = ?;";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(ctx->db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) {
            return send_json_response(io, "error", "db_error", "detail-prepare-failed");
        }
        sqlite3_bind_int(stmt, 1, cmd.video_id);
        int rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            return send_json_response(io, "error", "not_found", "video-not-found");
        }
        const unsigned char *title = sqlite3_column_text(stmt, 1);
        const unsigned char *desc = sqlite3_column_text(stmt, 2);
        const unsigned char *file = sqlite3_column_text(stmt, 3);
        const unsigned char *thumb = sqlite3_column_text(stmt, 4);
        int duration = sqlite3_column_int(stmt, 5);
        const unsigned char *upload = sqlite3_column_text(stmt, 6);
        char buf[1024];
        int len = snprintf(buf,
                           sizeof(buf),
                           "{\"type\":\"video_detail\",\"id\":%d,\"title\":\"%s\",\"description\":\"%s\","
                           "\"file_path\":\"%s\",\"thumbnail_path\":\"%s\",\"duration\":%d,\"upload_date\":\"%s\"}",
                           cmd.video_id,
                           title ? (const char *)title : "",
                           desc ? (const char *)desc : "",
                           file ? (const char *)file : "",
                           thumb ? (const char *)thumb : "",
                           duration,
                           upload ? (const char *)upload : "");
        sqlite3_finalize(stmt);
        if (len <= 0 || len >= (int)sizeof(buf)) {
            return send_json_response(io, "error", "internal_error", "response-too-large");
        }
        return ws_send_frame(io, 0x1, (const uint8_t *)buf, (size_t)len);
    }

    if (cmd.type == WS_CMD_STREAM_START) {
        if (!ctx || !ctx->db) {
            return send_json_response(io, "error", "unavailable", "db-missing");
        }
        db_video_t video;
        if (db_get_video_by_id(ctx->db, cmd.video_id, &video) != SQLITE_OK) {
            return send_json_response(io, "error", "not_found", "video-not-found");
        }
        char full_path[512];
        if (video.file_path[0] == '/') {
            strncpy(full_path, video.file_path, sizeof(full_path) - 1);
        } else {
            snprintf(full_path, sizeof(full_path), "%s/%s", VIDEO_BASE_PATH, video.file_path);
        }
        struct stat st;
        if (stat(full_path, &st) != 0) {
            return send_json_response(io, "error", "not_found", "file-missing");
        }
        double duration_sec = 0.0;
        video_probe_duration(full_path, &duration_sec);
        char resp[256];
        int len = snprintf(resp,
                           sizeof(resp),
                           "{\"type\":\"stream_start\",\"status\":\"ok\",\"id\":%d,"
                           "\"total_bytes\":%lld,\"chunk_size\":%u,\"duration\":%.2f,"
                           "\"connection_id\":%llu,\"stream_id\":%u}",
                           cmd.video_id,
                           (long long)st.st_size,
                           cmd.chunk_length,
                           duration_sec,
                           (unsigned long long)cmd.connection_id,
                           cmd.stream_id);
        if (len <= 0 || len >= (int)sizeof(resp)) {
            return send_json_response(io, "error", "internal_error", "response-too-large");
        }
        return ws_send_frame(io, 0x1, (const uint8_t *)resp, (size_t)len);
    }

    if (cmd.type == WS_CMD_STREAM_CHUNK) {
        if (!ctx || !ctx->db) {
            return send_json_response(io, "error", "unavailable", "db-missing");
        }
        db_video_t video;
        if (db_get_video_by_id(ctx->db, cmd.video_id, &video) != SQLITE_OK) {
            return send_json_response(io, "error", "not_found", "video-not-found");
        }
        char full_path[512];
        if (video.file_path[0] == '/') {
            strncpy(full_path, video.file_path, sizeof(full_path) - 1);
        } else {
            snprintf(full_path, sizeof(full_path), "%s/%s", VIDEO_BASE_PATH, video.file_path);
        }
        uint32_t next_pn;
        pthread_mutex_lock(&ctx->lock);
        next_pn = ctx->next_packet_number;
        pthread_mutex_unlock(&ctx->lock);
        if (send_video_chunk(ctx,
                             cmd.connection_id,
                             cmd.stream_id,
                             full_path,
                             cmd.offset,
                             cmd.length,
                             &next_pn) != 0) {
            return send_json_response(io, "error", "stream_failed", "chunk-send-failed");
        }
        pthread_mutex_lock(&ctx->lock);
        ctx->next_packet_number = next_pn;
        pthread_mutex_unlock(&ctx->lock);

        char resp[128];
        int len = snprintf(resp,
                           sizeof(resp),
                           "{\"type\":\"stream_chunk\",\"status\":\"ok\",\"offset\":%u,\"length\":%u}",
                           cmd.offset,
                           cmd.length);
        if (len <= 0 || len >= (int)sizeof(resp)) {
            return send_json_response(io, "error", "internal_error", "response-too-large");
        }
        return ws_send_frame(io, 0x1, (const uint8_t *)resp, (size_t)len);
    }

    if (cmd.type == WS_CMD_WS_INIT) {
        char path[512];
        snprintf(path, sizeof(path), "data/segments/%d/init-stream0.m4s", cmd.video_id);
        if (send_ws_file(io, path, "INIT", 0) != 0) {
            fprintf(stderr, "[ws] init segment missing video=%d path=%s\n", cmd.video_id, path);
            return send_json_response(io, "ws_segment", "error", "init-missing");
        }
        
        // Try to read segment_info.json first
        char info_path[512];
        snprintf(info_path, sizeof(info_path), "data/segments/%d/segment_info.json", cmd.video_id);
        FILE *fp = fopen(info_path, "r");
        if (fp) {
            // Read the entire segment_info.json
            fseek(fp, 0, SEEK_END);
            long file_size = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            
            if (file_size > 0 && file_size < 1024 * 1024) { // Max 1MB
                char *file_content = malloc(file_size + 1);
                if (file_content) {
                    size_t read_len = fread(file_content, 1, file_size, fp);
                    fclose(fp);
                    file_content[read_len] = '\0';
                    
                    // Find where JSON content starts (after opening brace and whitespace)
                    char *content_start = file_content;
                    while (*content_start && (*content_start == ' ' || *content_start == '\n' || *content_start == '\r' || *content_start == '\t')) {
                        content_start++;
                    }
                    if (*content_start == '{') {
                        content_start++;
                        while (*content_start && (*content_start == ' ' || *content_start == '\n' || *content_start == '\r' || *content_start == '\t')) {
                            content_start++;
                        }
                    }
                    
                    // Build response with header + content
                    size_t content_len = strlen(content_start);
                    size_t resp_size = 64 + content_len;
                    char *resp = malloc(resp_size);
                    if (resp) {
                        int len = snprintf(resp, resp_size, "{\"type\":\"ws_init\",\"status\":\"ok\",%s", content_start);
                        if (len > 0 && (size_t)len < resp_size) {
                            int rc = ws_send_frame(io, 0x1, (const uint8_t *)resp, (size_t)len);
                            free(resp);
                            free(file_content);
                            return rc;
                        }
                        free(resp);
                    }
                    free(file_content);
                }
            } else {
                fclose(fp);
            }
        }
        
        // Fallback: count segments manually if segment_info.json doesn't exist
        int duration = 0;
        int total_segments = 0;
        if (ctx && ctx->db) {
            db_video_t video;
            if (db_get_video_by_id(ctx->db, cmd.video_id, &video) == SQLITE_OK) {
                duration = video.duration;
            }
        }
        
        // Count segment files
        for (int i = 0; i < 1000; i++) {
            char seg_path[256];
            snprintf(seg_path, sizeof(seg_path), "data/segments/%d/chunk-stream0-%05d.m4s", cmd.video_id, i);
            struct stat st;
            if (stat(seg_path, &st) != 0) {
                total_segments = i;
                break;
            }
        }
        
        char resp[256];
        int len = snprintf(resp, sizeof(resp),
            "{\"type\":\"ws_init\",\"status\":\"ok\",\"duration\":%d,\"total_segments\":%d}",
            duration, total_segments);
        if (len > 0 && len < (int)sizeof(resp)) {
            return ws_send_frame(io, 0x1, (const uint8_t *)resp, (size_t)len);
        }
        return send_json_response(io, "ws_init", "ok", "init-sent");
    }

    if (cmd.type == WS_CMD_WS_SEGMENT) {
        char path[512];
        snprintf(path, sizeof(path), "data/segments/%d/chunk-stream0-%05d.m4s", cmd.video_id, cmd.segment_index);
        int retries = 0;
        int send_rc = -1;
        while (retries < 2) {
            send_rc = send_ws_file(io, path, "SEGM", (uint32_t)cmd.segment_index);
            if (send_rc == 0) {
                break;
            }
            retries++;
        }
        if (send_rc != 0) {
            fprintf(stderr, "[ws] segment missing/fail video=%d seg=%d path=%s retries=%d\n", cmd.video_id, cmd.segment_index, path, retries);
            if (ctx) {
                pthread_mutex_lock(&ctx->lock);
                ctx->segment_sent_fail++;
                pthread_mutex_unlock(&ctx->lock);
            }
            char payload[160];
            int len = snprintf(payload,
                               sizeof(payload),
                               "{\"type\":\"ws_segment\",\"status\":\"error\",\"segment\":%d,\"message\":\"segment-missing\"}",
                               cmd.segment_index);
            if (len > 0 && len < (int)sizeof(payload)) {
                return ws_send_frame(io, 0x1, (const uint8_t *)payload, (size_t)len);
            }
            return send_json_response(io, "ws_segment", "error", "segment-missing");
        }
        if (ctx) {
            pthread_mutex_lock(&ctx->lock);
            ctx->segment_sent_ok++;
            pthread_mutex_unlock(&ctx->lock);
        }
        return send_json_response(io, "ws_segment", "ok", "segment-sent");
    }

    if (cmd.type == WS_CMD_WATCH_GET) {
        if (!ctx || !ctx->db) {
            return send_json_response(io, "error", "unavailable", "db-missing");
        }
        db_watch_history_t hist;
        if (user_id <= 0) {
            return send_json_response(io, "error", "unauthorized", "login-required");
        }
        if (db_get_watch_history(ctx->db, user_id, cmd.video_id, &hist) != SQLITE_OK) {
            return send_json_response(io, "watch_get", "not_found", "history-missing");
        }
        char resp[256];
        int len = snprintf(resp,
                           sizeof(resp),
                           "{\"type\":\"watch_get\",\"status\":\"ok\",\"user_id\":%d,\"video_id\":%d,\"position\":%d}",
                           user_id,
                           cmd.video_id,
                           hist.last_position);
        if (len <= 0 || len >= (int)sizeof(resp)) {
            return send_json_response(io, "error", "internal_error", "response-too-large");
        }
        return ws_send_frame(io, 0x1, (const uint8_t *)resp, (size_t)len);
    }

    if (cmd.type == WS_CMD_WATCH_UPDATE) {
        if (!ctx || !ctx->db) {
            return send_json_response(io, "error", "unavailable", "db-missing");
        }
        if (user_id <= 0) {
            return send_json_response(io, "error", "unauthorized", "login-required");
        }
        if (db_upsert_watch_history(ctx->db, user_id, cmd.video_id, cmd.position) != SQLITE_OK) {
            return send_json_response(io, "error", "db_error", "watch-update-failed");
        }
        char resp[128];
        int len = snprintf(resp,
                           sizeof(resp),
                           "{\"type\":\"watch_update\",\"status\":\"ok\",\"user_id\":%d,\"position\":%d}",
                           user_id,
                           cmd.position);
        if (len <= 0 || len >= (int)sizeof(resp)) {
            return send_json_response(io, "error", "internal_error", "response-too-large");
        }
        return ws_send_frame(io, 0x1, (const uint8_t *)resp, (size_t)len);
    }

    return send_json_response(io, "error", "bad_request", "unsupported");
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
