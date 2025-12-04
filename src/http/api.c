#include "http/api.h"

#include "auth/session.h"
#include "auth/hash.h"
#include "db/database.h"
#include "server/upload.h"

#include <stdint.h>
#ifdef ENABLE_TLS
#include <openssl/ssl.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
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
    const char *nickname = (user_row.nickname[0] != '\0') ? user_row.nickname : user;

    char resp_body[512];
    snprintf(resp_body, sizeof(resp_body), "{\"session_id\":\"%s\",\"username\":\"%s\",\"nickname\":\"%s\",\"is_admin\":%s}", sid, user, nickname, is_admin ? "true" : "false");
    return http_send_response(io, "HTTP/1.1 200 OK\r\n", headers, resp_body);
}

static int handle_signup(api_io_t *io, const http_request_t *req, websocket_context_t *ctx, const char *body, size_t body_len) {
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

    char username[DB_MAX_USERNAME];
    char password[128];
    char nickname[DB_MAX_NICKNAME];
    char body_buf[HTTP_MAX_BODY + 1];
    size_t copy_len = body_len > HTTP_MAX_BODY ? HTTP_MAX_BODY : body_len;
    memcpy(body_buf, body, copy_len);
    body_buf[copy_len] = '\0';

    if (json_extract_string(body_buf, "username", username, sizeof(username)) != 0 ||
        json_extract_string(body_buf, "password", password, sizeof(password)) != 0 ||
        json_extract_string(body_buf, "nickname", nickname, sizeof(nickname)) != 0) {
        return http_send_response(io, "HTTP/1.1 400 Bad Request\r\n", "Content-Type: text/plain\r\n", "bad-json");
    }

    db_user_t existing;
    if (db_get_user_by_username(ctx->db, username, &existing) == SQLITE_OK) {
        return http_send_response(io, "HTTP/1.1 409 Conflict\r\n", "Content-Type: text/plain\r\n", "user-exists");
    }

    char hash[HASH_HEX_SIZE];
    hash_password(password, hash, sizeof(hash));
    if (hash[0] == '\0') {
        return http_send_response(io, "HTTP/1.1 500 Internal Server Error\r\n", "Content-Type: text/plain\r\n", "hash-failed");
    }

    int new_user_id = 0;
    if (db_create_user(ctx->db, username, nickname, hash, "user", &new_user_id) != SQLITE_OK) {
        return http_send_response(io, "HTTP/1.1 500 Internal Server Error\r\n", "Content-Type: text/plain\r\n", "create-user-failed");
    }

    char sid[SESSION_ID_LEN + 1];
    if (session_generate_id(sid, sizeof(sid)) != 0 ||
        db_create_session(ctx->db, new_user_id, sid, SESSION_TTL_SECONDS) != SQLITE_OK) {
        return http_send_response(io, "HTTP/1.1 500 Internal Server Error\r\n", "Content-Type: text/plain\r\n", "session-failed");
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

    char resp_body[512];
    snprintf(resp_body, sizeof(resp_body), "{\"session_id\":\"%s\",\"username\":\"%s\",\"nickname\":\"%s\",\"is_admin\":false}", sid, username, nickname);
    return http_send_response(io, "HTTP/1.1 200 OK\r\n", headers, resp_body);
}

static int is_admin_user(db_context_t *db, const http_request_t *req) {
    if (!db || !req) {
        return 0;
    }
    char sid[SESSION_ID_LEN + 1];
    if (session_extract_from_headers(req, sid, sizeof(sid)) != 0) {
        return 0;
    }
    int user_id = 0;
    if (session_validate_and_extend(db, sid, SESSION_TTL_SECONDS, &user_id) != 0 || user_id <= 0) {
        return 0;
    }
    db_user_t user;
    if (db_get_user_by_id(db, user_id, &user) != SQLITE_OK) {
        return 0;
    }
    return strcasecmp(user.role, "admin") == 0;
}

static int json_extract_int_field(const char *json, const char *key, int *out_val) {
    if (!json || !key || !out_val) return -1;
    const char *p = strstr(json, key);
    if (!p) return -1;
    p += strlen(key);
    while (*p && (*p == ' ' || *p == '\"' || *p == ':' || *p == '=')) {
        if (*p == ':' || *p == '=') { p++; break; }
        p++;
    }
    while (*p == ' ' || *p == '\"') p++;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return -1;
    *out_val = (int)v;
    return 0;
}

static int handle_admin_delete(api_io_t *io, const http_request_t *req, websocket_context_t *ctx, const char *body, size_t body_len) {
    (void)body_len;
    if (!ctx || !ctx->db) {
        return http_send_response(io, "HTTP/1.1 503 Service Unavailable\r\n", "Content-Type: text/plain\r\n", "service-unavailable");
    }
    if (!is_admin_user(ctx->db, req)) {
        return http_send_response(io, "HTTP/1.1 403 Forbidden\r\n", "Content-Type: text/plain\r\n", "admin-required");
    }
    int video_id = 0;
    if (json_extract_int_field(body, "video_id", &video_id) != 0 || video_id <= 0) {
        return http_send_response(io, "HTTP/1.1 400 Bad Request\r\n", "Content-Type: text/plain\r\n", "invalid-video-id");
    }
    db_video_t v;
    if (db_get_video_by_id(ctx->db, video_id, &v) != SQLITE_OK) {
        return http_send_response(io, "HTTP/1.1 404 Not Found\r\n", "Content-Type: text/plain\r\n", "video-not-found");
    }
    db_delete_video_by_id(ctx->db, video_id);
    if (v.file_path[0]) unlink(v.file_path);
    if (v.thumbnail_path[0]) unlink(v.thumbnail_path);
    if (v.segment_path[0]) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", v.segment_path);
        system(cmd);
    }
    return http_send_response(io, "HTTP/1.1 200 OK\r\n", "Content-Type: text/plain\r\n", "deleted");
}

static int handle_admin_update(api_io_t *io, const http_request_t *req, websocket_context_t *ctx, const char *body, size_t body_len) {
    (void)body_len;
    if (!ctx || !ctx->db) {
        return http_send_response(io, "HTTP/1.1 503 Service Unavailable\r\n", "Content-Type: text/plain\r\n", "service-unavailable");
    }
    if (!is_admin_user(ctx->db, req)) {
        return http_send_response(io, "HTTP/1.1 403 Forbidden\r\n", "Content-Type: text/plain\r\n", "admin-required");
    }
    int video_id = 0;
    if (json_extract_int_field(body, "video_id", &video_id) != 0 || video_id <= 0) {
        return http_send_response(io, "HTTP/1.1 400 Bad Request\r\n", "Content-Type: text/plain\r\n", "invalid-video-id");
    }
    char new_title[DB_MAX_TITLE] = {0};
    char new_desc[DB_MAX_DESCRIPTION] = {0};
    json_extract_string(body, "title", new_title, sizeof(new_title));
    json_extract_string(body, "description", new_desc, sizeof(new_desc));
    if (db_update_video_metadata(ctx->db, video_id, new_title, new_desc) != SQLITE_OK) {
        return http_send_response(io, "HTTP/1.1 500 Internal Server Error\r\n", "Content-Type: text/plain\r\n", "update-failed");
    }
    return http_send_response(io, "HTTP/1.1 200 OK\r\n", "Content-Type: text/plain\r\n", "updated");
}

static int handle_admin_list(api_io_t *io, const http_request_t *req, websocket_context_t *ctx) {
    if (!ctx || !ctx->db) {
        return http_send_response(io, "HTTP/1.1 503 Service Unavailable\r\n", "Content-Type: text/plain\r\n", "service-unavailable");
    }
    if (!is_admin_user(ctx->db, req)) {
        return http_send_response(io, "HTTP/1.1 403 Forbidden\r\n", "Content-Type: text/plain\r\n", "admin-required");
    }
    const char *sql = "SELECT id, title, IFNULL(description,''), file_path, IFNULL(thumbnail_path,'') FROM videos ORDER BY id DESC;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(ctx->db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return http_send_response(io, "HTTP/1.1 500 Internal Server Error\r\n", "Content-Type: text/plain\r\n", "db-error");
    }
    char buf[8192];
    size_t pos = 0;
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "{\"items\":[");
    int first = 1;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!first) buf[pos++] = ',';
        first = 0;
        int id = sqlite3_column_int(stmt, 0);
        const unsigned char *title = sqlite3_column_text(stmt, 1);
        const unsigned char *desc = sqlite3_column_text(stmt, 2);
        const unsigned char *file = sqlite3_column_text(stmt, 3);
        const unsigned char *thumb = sqlite3_column_text(stmt, 4);
        pos += (size_t)snprintf(buf + pos,
                                sizeof(buf) - pos,
                                "{\"id\":%d,\"title\":\"%s\",\"description\":\"%s\",\"file_path\":\"%s\",\"thumbnail_path\":\"%s\"}",
                                id,
                                title ? (const char *)title : "",
                                desc ? (const char *)desc : "",
                                file ? (const char *)file : "",
                                thumb ? (const char *)thumb : "");
        if (pos >= sizeof(buf) - 1) break;
    }
    sqlite3_finalize(stmt);
    if (pos >= sizeof(buf) - 2) {
        return http_send_response(io, "HTTP/1.1 500 Internal Server Error\r\n", "Content-Type: text/plain\r\n", "response-too-large");
    }
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "]}");
    char headers[128];
    int hlen = snprintf(headers, sizeof(headers), "Content-Type: application/json\r\n");
    if (hlen <= 0 || (size_t)hlen >= sizeof(headers)) {
        return http_send_response(io, "HTTP/1.1 500 Internal Server Error\r\n", "Content-Type: text/plain\r\n", "header-too-long");
    }
    return http_send_response(io, "HTTP/1.1 200 OK\r\n", headers, buf);
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

    const char *ct = http_get_header(req, "content-type");
    if (!ct) {
        return http_send_response(io, "HTTP/1.1 400 Bad Request\r\n", "Content-Type: text/plain\r\n", "missing-content-type");
    }
#ifdef ENABLE_TLS
    if (io->ssl) {
        int fd = SSL_get_fd(io->ssl);
        handle_upload_request(fd, io->ssl, ct, body, body_len, ctx->db);
        return 0;
    }
#endif
    handle_upload_request(io->fd, NULL, ct, body, body_len, ctx->db);
    return 0;
}

static const char *guess_mime(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) {
        return "application/octet-stream";
    }
    if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0) {
        return "text/html; charset=utf-8";
    }
    if (strcasecmp(dot, ".js") == 0) {
        return "application/javascript; charset=utf-8";
    }
    if (strcasecmp(dot, ".css") == 0) {
        return "text/css; charset=utf-8";
    }
    if (strcasecmp(dot, ".json") == 0) {
        return "application/json; charset=utf-8";
    }
    if (strcasecmp(dot, ".png") == 0) {
        return "image/png";
    }
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcasecmp(dot, ".mp4") == 0) {
        return "video/mp4";
    }
    if (strcasecmp(dot, ".m3u8") == 0) {
        return "application/vnd.apple.mpegurl";
    }
    if (strcasecmp(dot, ".svg") == 0) {
        return "image/svg+xml";
    }
    return "application/octet-stream";
}

static int http_send_file(api_io_t *io, const char *status_line, const char *headers, const uint8_t *body, size_t body_len, int send_body) {
    if (!status_line || !headers) {
        return -1;
    }
    char head_buf[1024];
    int hlen = snprintf(head_buf, sizeof(head_buf), "%sContent-Length: %zu\r\n%s\r\n", status_line, body_len, headers);
    if (hlen < 0 || (size_t)hlen >= sizeof(head_buf)) {
        return -1;
    }
    if (http_write_all(io, head_buf, (size_t)hlen) != 0) {
        return -1;
    }
    if (send_body && body_len > 0) {
        return http_write_all(io, body, body_len);
    }
    return 0;
}

static int handle_static_file(api_io_t *io, const http_request_t *req) {
    if (strcasecmp(req->method, "GET") != 0 && strcasecmp(req->method, "HEAD") != 0) {
        return http_send_response(io, "HTTP/1.1 405 Method Not Allowed\r\n", "Content-Type: text/plain\r\n", "method-not-allowed");
    }
    const char *raw_path = req->path;
    if (!raw_path || raw_path[0] != '/') {
        return http_send_response(io, "HTTP/1.1 400 Bad Request\r\n", "Content-Type: text/plain\r\n", "bad-path");
    }
    char path[256];
    size_t pidx = 0;
    while (raw_path[pidx] && raw_path[pidx] != '?' && raw_path[pidx] != '#'
           && pidx + 1 < sizeof(path)) {
        path[pidx] = raw_path[pidx];
        pidx++;
    }
    path[pidx] = '\0';

    if (strstr(path, "..")) {
        return http_send_response(io, "HTTP/1.1 400 Bad Request\r\n", "Content-Type: text/plain\r\n", "invalid-path");
    }

    char rel[256];
    rel[0] = '\0';
    const char *subpath = NULL;
    const char *base_dir = "web";
    if (strcmp(path, "/") == 0) {
        subpath = "index.html";
    } else if (strncmp(path, "/data/", 6) == 0) {
        base_dir = "";
        subpath = path + 1; /* keep data/... relative to repo root */
    } else {
        subpath = path + 1;
        size_t rel_len = strlen(subpath);
        if (rel_len > 0 && subpath[rel_len - 1] == '/') {
            /* only auto-index for web content */
            int rlen = snprintf(rel, sizeof(rel), "%s%s", subpath, "index.html");
            if (rlen < 0 || (size_t)rlen >= sizeof(rel)) {
                return http_send_response(io, "HTTP/1.1 414 URI Too Long\r\n", "Content-Type: text/plain\r\n", "path-too-long");
            }
            subpath = rel;
        }
    }

    if (!subpath || subpath[0] == '\0') {
        return http_send_response(io, "HTTP/1.1 400 Bad Request\r\n", "Content-Type: text/plain\r\n", "bad-path");
    }

    if (subpath != rel) {
        strncpy(rel, subpath, sizeof(rel) - 1);
        rel[sizeof(rel) - 1] = '\0';
    }

    char fs_path[PATH_MAX];
    int plen = snprintf(fs_path, sizeof(fs_path), "%s%s%s", base_dir, base_dir[0] ? "/" : "", rel);
    if (plen < 0 || (size_t)plen >= sizeof(fs_path)) {
        return http_send_response(io, "HTTP/1.1 414 URI Too Long\r\n", "Content-Type: text/plain\r\n", "path-too-long");
    }

    struct stat st;
    if (stat(fs_path, &st) != 0 || S_ISDIR(st.st_mode)) {
        if (strcmp(rel, "favicon.ico") == 0) {
            return http_send_file(io,
                                  "HTTP/1.1 204 No Content\r\n",
                                  "Content-Type: image/x-icon\r\n",
                                  NULL,
                                  0,
                                  0);
        }
        return http_send_response(io, "HTTP/1.1 404 Not Found\r\n", "Content-Type: text/plain\r\n", "not-found");
    }
    if (st.st_size > 10 * 1024 * 1024) {
        return http_send_response(io, "HTTP/1.1 413 Payload Too Large\r\n", "Content-Type: text/plain\r\n", "file-too-large");
    }

    FILE *fp = fopen(fs_path, "rb");
    if (!fp) {
        return http_send_response(io, "HTTP/1.1 500 Internal Server Error\r\n", "Content-Type: text/plain\r\n", "open-failed");
    }
    uint8_t *buf = malloc((size_t)st.st_size);
    if (!buf) {
        fclose(fp);
        return http_send_response(io, "HTTP/1.1 500 Internal Server Error\r\n", "Content-Type: text/plain\r\n", "oom");
    }
    size_t n = fread(buf, 1, (size_t)st.st_size, fp);
    fclose(fp);
    if (n != (size_t)st.st_size) {
        free(buf);
        return http_send_response(io, "HTTP/1.1 500 Internal Server Error\r\n", "Content-Type: text/plain\r\n", "read-failed");
    }

    const char *mime = guess_mime(fs_path);
    char headers[256];
    int hlen = snprintf(headers, sizeof(headers), "Content-Type: %s\r\n", mime);
    if (hlen < 0 || (size_t)hlen >= sizeof(headers)) {
        free(buf);
        return http_send_response(io, "HTTP/1.1 500 Internal Server Error\r\n", "Content-Type: text/plain\r\n", "header-too-long");
    }

    int rc = http_send_file(io, "HTTP/1.1 200 OK\r\n", headers, buf, (size_t)n, strcasecmp(req->method, "HEAD") != 0);
    free(buf);
    return rc;
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

    size_t max_body = HTTP_MAX_BODY;
    int is_upload = strcmp(req->path, "/upload") == 0;
    if (is_upload) {
        max_body = 200 * 1024 * 1024; /* 200MB upload limit */
    }

    if (content_len > max_body) {
        return http_send_response(&io, "HTTP/1.1 413 Payload Too Large\r\n", "Content-Type: text/plain\r\n", "payload-too-large");
    }

    char stack_buf[HTTP_MAX_BODY];
    char *body_buf = stack_buf;
    if (is_upload && content_len > sizeof(stack_buf)) {
        body_buf = malloc(content_len);
        if (!body_buf) {
            return http_send_response(&io, "HTTP/1.1 500 Internal Server Error\r\n", "Content-Type: text/plain\r\n", "oom");
        }
    }

    size_t to_copy = pre_read_len > content_len ? content_len : pre_read_len;
    if (to_copy > 0) {
        memcpy(body_buf, pre_read_body, to_copy);
    }
    size_t remaining = content_len > to_copy ? content_len - to_copy : 0;
    if (remaining > 0) {
        if (http_read_exact(&io, body_buf + to_copy, remaining) != 0) {
            if (is_upload && body_buf != stack_buf) free(body_buf);
            return http_send_response(&io, "HTTP/1.1 400 Bad Request\r\n", "Content-Type: text/plain\r\n", "body-read-failed");
        }
    }

    int rc = 0;
    if (strcmp(req->path, "/login") == 0) {
        rc = handle_login(&io, req, ctx, body_buf, content_len);
    } else if (strcmp(req->path, "/signup") == 0) {
        rc = handle_signup(&io, req, ctx, body_buf, content_len);
    } else if (strcmp(req->path, "/logout") == 0) {
        rc = handle_logout(&io, req, ctx);
    } else if (strcmp(req->path, "/upload") == 0) {
        rc = handle_upload(&io, req, ctx, body_buf, content_len);
    } else if (strcmp(req->path, "/admin/video/delete") == 0) {
        rc = handle_admin_delete(&io, req, ctx, body_buf, content_len);
    } else if (strcmp(req->path, "/admin/video/update") == 0) {
        rc = handle_admin_update(&io, req, ctx, body_buf, content_len);
    } else if (strcmp(req->path, "/admin/video/list") == 0) {
        rc = handle_admin_list(&io, req, ctx);
    } else {
        rc = handle_static_file(&io, req);
    }

    if (is_upload && body_buf != stack_buf) {
        free(body_buf);
    }
    return rc;
}
