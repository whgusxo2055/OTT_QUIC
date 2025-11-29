#ifndef SERVER_HTTP_H
#define SERVER_HTTP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HTTP_MAX_METHOD     8
#define HTTP_MAX_PATH       128
#define HTTP_MAX_VERSION    16
#define HTTP_MAX_HEADERS    32
#define HTTP_MAX_HEADER_KEY 64
#define HTTP_MAX_HEADER_VAL 256

typedef struct {
    char name[HTTP_MAX_HEADER_KEY];
    char value[HTTP_MAX_HEADER_VAL];
} http_header_t;

typedef struct {
    char method[HTTP_MAX_METHOD];
    char path[HTTP_MAX_PATH];
    char version[HTTP_MAX_VERSION];
    http_header_t headers[HTTP_MAX_HEADERS];
    int header_count;
} http_request_t;

int http_parse_request(const char *raw, size_t len, http_request_t *req);
const char *http_get_header(const http_request_t *req, const char *name);

#ifdef __cplusplus
}
#endif

#endif // SERVER_HTTP_H
