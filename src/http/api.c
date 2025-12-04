#include "http/api.h"

#include "auth/session.h"
#include "db/database.h"

#include <ctype.h>
#include <stdint.h>
#ifdef ENABLE_TLS
#include <openssl/ssl.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define HTTP_MAX_BODY 4096

typedef struct {
    int fd;
    SSL *ssl;
} api_io_t;

static int http_write_all(api_io_t *io, const void *buf, size_t len) {
    const uint8_t *p = buf;
    size_t sent = 0;
    while (sent < len) {
        int n = 0;
#ifdef ENABLE_TLS
        if (io->ssl) {
            n = SSL_write(io->ssl, p + sent, (int)(len - sent));
        } else
#endif
        {
            n = (int)send(io->fd, p + sent, len - sent, 0);
        }
        if (n <= 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static int http_read_exact(api_io_t *io, void *buf, size_t len) {
    uint8_t *p = buf;
    size_t read_total = 0;
    while (read_total < len) {
        int n = 0;
#ifdef ENABLE_TLS
        if (io->ssl) {
            n = SSL_read(io->ssl, p + read_total, (int)(len - read_total));
        } else
#endif
        {
            n = (int)recv(io->fd, p + read_total, len - read_total, 0);
        }
        if (n <= 0) {
            return -1;
        }
        read_total += (size_t)n;
    }
    return 0;
}

static int http_send_response(api_io_t *io, const char *status_line, const char *headers, const char *body) {
    if (!status_line || !headers || !body) {
        return -1;
    }
    size_t body_len = strlen(body);
    char head_buf[1024];
    int hlen = snprintf(head_buf, sizeof(head_buf), "%sContent-Length: %zu\r\n%s\r\n", status_line, body_len, headers);
    if (hlen < 0 || (size_t)hlen >= sizeof(head_buf)) {
        return -1;
    }
    if (http_write_all(io, head_buf, (size_t)hlen) != 0) {
        return -1;
    }
    return body_len > 0 ? http_write_all(io, body, body_len) : 0;
}

static void http_build_cors(const http_request_t *req, char *out, size_t out_size) {
    const char *origin = http_get_header(req, "origin");
    const char *allow_origin = origin ? origin : "*";
    snprintf(out,
             out_size,
             "Access-Control-Allow-Origin: %s\r\n"
             "Access-Control-Allow-Credentials: true\r\n"
             "Access-Control-Allow-Headers: Content-Type, Authorization, Cookie\r\n"
             "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n",
             allow_origin);
}

static int json_extract_string(const char *json, const char *key, char *out, size_t out_size) {
    if (!json || !key || !out || out_size == 0) {
        return -1;
    }
    char pattern[64];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (plen <= 0 || (size_t)plen >= sizeof(pattern)) {
        return -1;
    }
    const char *p = strstr(json, pattern);
    if (!p) {
        return -1;
    }
    p += strlen(pattern);
    while (*p && (*p == ' ' || *p == '\t' || *p == ':')) {
        p++;
    }
    if (*p != '\"') {
        return -1;
    }
    p++;
    const char *end = strchr(p, '\"');
    if (!end) {
        return -1;
    }
    size_t len = (size_t)(end - p);
    if (len == 0 || len >= out_size) {
        return -1;
    }
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

static int handle_login(api_io_t *io, const http_request_t *req, websocket_context_t *ctx, const char *body, size_t body_len) {
    if (!ctx || !ctx->db) {
        return http_send_response(io, "HTTP/1.1 503 Service Unavailable\r\n", "Content-Type: text/plain\r\n", "service-unavailable");
    }
    if (strcasecmp(req->method, "OPTIONS") == 0) {
        char cors[256];
        http_build_cors(req, cors, sizeof(cors));
        return http_send_response(io, "HTTP/1.1 204 No Content\r\n", cors, "");
    }
    if (strcasecmp(req->method, "POST") != 0) {
        return http_send_response(io, "HTTP/1.1 405 Method Not Allowed\r\n", "Content-Type: text/plain\r\n", "method-not-allowed");
    }
    char user[128];
    char pass[128];
    char body_buf[HTTP_MAX_BODY + 1];
    size_t copy_len = body_len > HTTP_MAX_BODY ? HTTP_MAX_BODY : body_len;
    memcpy(body_buf, body, copy_len);
    body_buf[copy_len] = '\0';

    if (json_extract_string(body_buf, "username", user, sizeof(user)) != 0 ||
        json_extract_string(body_buf, "password", pass, sizeof(pass)) != 0) {
        return http_send_response(io, "HTTP/1.1 400 Bad Request\r\n", "Content-Type: text/plain\r\n", "bad-json");
    }

    char sid[SESSION_ID_LEN + 1];
    if (session_login(ctx->db, user, pass, sid, sizeof(sid)) != 0) {
        return http_send_response(io, "HTTP/1.1 401 Unauthorized\r\n", "Content-Type: text/plain\r\n", "login-failed");
    }

    char headers[512];
    char cors[256];
    http_build_cors(req, cors, sizeof(cors));
    int hlen = snprintf(headers,
                        sizeof(headers),
                        "Content-Type: application/json\r\n"
                        "Set-Cookie: SID=%s; HttpOnly; Path=/; SameSite=Lax\r\n"
                        "%s",
                        sid,
                        cors);
    if (hlen < 0 || (size_t)hlen >= sizeof(headers)) {
        return http_send_response(io, "HTTP/1.1 500 Internal Server Error\r\n", "Content-Type: text/plain\r\n", "header-too-long");
    }

    db_user_t user_row;
    int is_admin = 0;
    if (db_get_user_by_username(ctx->db, user, &user_row) == SQLITE_OK && strcasecmp(user_row.role, "admin") == 0) {
        is_admin = 1;
    }

    char resp_body[512];
    snprintf(resp_body, sizeof(resp_body), "{\"session_id\":\"%s\",\"username\":\"%s\",\"is_admin\":%s}", sid, user, is_admin ? "true" : "false");
    return http_send_response(io, "HTTP/1.1 200 OK\r\n", headers, resp_body);
}

static int handle_logout(api_io_t *io, const http_request_t *req, websocket_context_t *ctx) {
    if (!ctx || !ctx->db) {
        return http_send_response(io, "HTTP/1.1 503 Service Unavailable\r\n", "Content-Type: text/plain\r\n", "service-unavailable");
    }
    if (strcasecmp(req->method, "OPTIONS") == 0) {
        char cors[256];
        http_build_cors(req, cors, sizeof(cors));
        return http_send_response(io, "HTTP/1.1 204 No Content\r\n", cors, "");
    }
    char sid[SESSION_ID_LEN + 1];
    if (session_extract_from_headers(req, sid, sizeof(sid)) != 0) {
        return http_send_response(io, "HTTP/1.1 400 Bad Request\r\n", "Content-Type: text/plain\r\n", "session-missing");
    }
    session_logout(ctx->db, sid);
    char cors[256];
    http_build_cors(req, cors, sizeof(cors));
    char headers[512];
    int hlen = snprintf(headers,
                        sizeof(headers),
                        "Content-Type: text/plain\r\n"
                        "Set-Cookie: SID=; Max-Age=0; HttpOnly; Path=/; SameSite=Lax\r\n"
                        "%s",
                        cors);
    if (hlen < 0 || (size_t)hlen >= sizeof(headers)) {
        return http_send_response(io, "HTTP/1.1 500 Internal Server Error\r\n", "Content-Type: text/plain\r\n", "header-too-long");
    }
    return http_send_response(io, "HTTP/1.1 200 OK\r\n", headers, "logged-out");
}

static int handle_upload(api_io_t *io, const http_request_t *req, websocket_context_t *ctx, const char *body, size_t body_len) {
    if (!ctx || !ctx->db) {
        return http_send_response(io, "HTTP/1.1 503 Service Unavailable\r\n", "Content-Type: text/plain\r\n", "service-unavailable");
    }
    if (strcasecmp(req->method, "OPTIONS") == 0) {
        char cors[256];
        http_build_cors(req, cors, sizeof(cors));
        return http_send_response(io, "HTTP/1.1 204 No Content\r\n", cors, "");
    }
    if (body_len == 0) {
        return http_send_response(io, "HTTP/1.1 400 Bad Request\r\n", "Content-Type: text/plain\r\n", "empty-body");
    }

    /* Note: this is a placeholder; multipart parsing is not implemented. */
    (void)body;
    (void)ctx;

    char cors[256];
    http_build_cors(req, cors, sizeof(cors));
    return http_send_response(io, "HTTP/1.1 202 Accepted\r\n", cors, "upload-received");
}

int http_api_handle(int fd, SSL *ssl, const http_request_t *req, websocket_context_t *ctx, const char *pre_read_body, size_t pre_read_len) {
    if (!req) {
        return -1;
    }

    api_io_t io = {.fd = fd, .ssl = ssl};

    const char *cl = http_get_header(req, "content-length");
    size_t content_len = 0;
    if (cl) {
        long body_len = strtol(cl, NULL, 10);
        if (body_len > 0) {
            content_len = (size_t)body_len;
        }
    }

    /* Limit overly large bodies to keep server safe. */
    if (content_len > HTTP_MAX_BODY) {
        return http_send_response(&io, "HTTP/1.1 413 Payload Too Large\r\n", "Content-Type: text/plain\r\n", "payload-too-large");
    }

    char body_buf[HTTP_MAX_BODY];
    size_t to_copy = pre_read_len > content_len ? content_len : pre_read_len;
    if (to_copy > 0) {
        memcpy(body_buf, pre_read_body, to_copy);
    }
    size_t remaining = content_len > to_copy ? content_len - to_copy : 0;
    if (remaining > 0) {
        if (http_read_exact(&io, body_buf + to_copy, remaining) != 0) {
            return http_send_response(&io, "HTTP/1.1 400 Bad Request\r\n", "Content-Type: text/plain\r\n", "body-read-failed");
        }
    }

    if (strcmp(req->path, "/login") == 0) {
        return handle_login(&io, req, ctx, body_buf, content_len);
    }
    if (strcmp(req->path, "/logout") == 0) {
        return handle_logout(&io, req, ctx);
    }
    if (strcmp(req->path, "/upload") == 0) {
        return handle_upload(&io, req, ctx, body_buf, content_len);
    }

    return http_send_response(&io, "HTTP/1.1 404 Not Found\r\n", "Content-Type: text/plain\r\n", "not-found");
}
