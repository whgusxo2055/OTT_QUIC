#ifndef AUTH_SESSION_H
#define AUTH_SESSION_H

#include "db/database.h"
#include "server/http.h"

#include <stddef.h>

#define SESSION_ID_LEN 64
#define SESSION_TTL_SECONDS 1800

int session_generate_id(char *out, size_t out_size);
int session_login(db_context_t *db, const char *username, const char *password, char *out_session, size_t out_size);
int session_validate_and_extend(db_context_t *db, const char *session_id, int ttl_seconds, int *out_user_id);
int session_logout(db_context_t *db, const char *session_id);

int session_extract_from_headers(const http_request_t *req, char *out_session, size_t out_size);

#endif // AUTH_SESSION_H
