#ifndef SERVER_UPLOAD_H
#define SERVER_UPLOAD_H

#include "db/database.h"
#ifdef ENABLE_TLS
#include <openssl/ssl.h>
#else
typedef struct ssl_st SSL;
#endif

int handle_upload_request(int fd, SSL *ssl, const char *content_type, const char *body, size_t body_len, db_context_t *db);

#endif // SERVER_UPLOAD_H
