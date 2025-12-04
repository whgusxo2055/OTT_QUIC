#ifndef HTTP_API_H
#define HTTP_API_H

#include <stddef.h>
#ifdef ENABLE_TLS
#include <openssl/ssl.h>
#else
typedef struct ssl_st SSL;
#endif

#include "server/http.h"
#include "server/websocket.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Handle REST-style HTTP requests (login/logout/upload/admin) separate from WebSocket upgrade path. */
int http_api_handle(int fd, SSL *ssl, const http_request_t *req, websocket_context_t *ctx, const char *pre_read_body, size_t pre_read_len);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_API_H */
