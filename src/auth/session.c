#include "auth/session.h"

#include "auth/hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <unistd.h>

static int read_random(uint8_t *buf, size_t len) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) {
        return -1;
    }
    size_t r = fread(buf, 1, len, f);
    fclose(f);
    return r == len ? 0 : -1;
}

int session_generate_id(char *out, size_t out_size) {
    if (!out || out_size < SESSION_ID_LEN + 1) {
        return -1;
    }
    uint8_t bytes[SESSION_ID_LEN / 2];
    if (read_random(bytes, sizeof(bytes)) != 0) {
        return -1;
    }
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        out[i * 2] = hex[(bytes[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[bytes[i] & 0xF];
    }
    out[SESSION_ID_LEN] = '\0';
    return 0;
}

int session_login(db_context_t *db, const char *username, const char *password, char *out_session, size_t out_size) {
    if (!db || !username || !password || !out_session) {
        return -1;
    }

    db_user_t user;
    int rc = db_get_user_by_username(db, username, &user);
    if (rc != SQLITE_OK) {
        return -1;
    }
    if (verify_password(password, user.password_hash) != 0) {
        return -1;
    }

    if (session_generate_id(out_session, out_size) != 0) {
        return -1;
    }

    if (db_create_session(db, user.id, out_session, SESSION_TTL_SECONDS) != SQLITE_OK) {
        return -1;
    }

    return 0;
}

int session_validate_and_extend(db_context_t *db, const char *session_id, int ttl_seconds, int *out_user_id) {
    if (!db || !session_id) {
        return -1;
    }

    db_session_t session;
    int rc = db_get_session(db, session_id, &session);
    if (rc != SQLITE_OK) {
        return -1;
    }

    if (db_delete_expired_sessions(db) != SQLITE_OK) {
        return -1;
    }

    /* Extend expiry */
    db_delete_session(db, session_id);
    if (db_create_session(db, session.user_id, session_id, ttl_seconds) != SQLITE_OK) {
        return -1;
    }

    if (out_user_id) {
        *out_user_id = session.user_id;
    }

    return 0;
}

int session_logout(db_context_t *db, const char *session_id) {
    if (!db || !session_id) {
        return -1;
    }
    return db_delete_session(db, session_id);
}

static int extract_bearer(const char *auth, char *out, size_t out_size) {
    const char *prefix = "Bearer ";
    size_t prefix_len = strlen(prefix);
    if (strncasecmp(auth, prefix, prefix_len) != 0) {
        return -1;
    }
    const char *token = auth + prefix_len;
    size_t len = strlen(token);
    if (len >= out_size) {
        return -1;
    }
    strncpy(out, token, out_size - 1);
    out[out_size - 1] = '\0';
    return 0;
}

static int extract_cookie(const char *cookie, char *out, size_t out_size) {
    const char *p = cookie;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ';') {
            p++;
        }
        if (strncasecmp(p, "SID=", 4) == 0) {
            p += 4;
            const char *end = strchr(p, ';');
            size_t len = end ? (size_t)(end - p) : strlen(p);
            if (len >= out_size) {
                return -1;
            }
            memcpy(out, p, len);
            out[len] = '\0';
            return 0;
        }
        const char *next = strchr(p, ';');
        if (!next) {
            break;
        }
        p = next + 1;
    }
    return -1;
}

int session_extract_from_headers(const http_request_t *req, char *out_session, size_t out_size) {
    if (!req || !out_session || out_size == 0) {
        return -1;
    }
    const char *auth = http_get_header(req, "authorization");
    if (auth && extract_bearer(auth, out_session, out_size) == 0) {
        return 0;
    }
    const char *cookie = http_get_header(req, "cookie");
    if (cookie && extract_cookie(cookie, out_session, out_size) == 0) {
        return 0;
    }
    return -1;
}
