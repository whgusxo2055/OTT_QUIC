#include "server/http.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void trim_leading(char **str) {
    while (**str == ' ' || **str == '\t') {
        (*str)++;
    }
}

int http_parse_request(const char *raw, size_t len, http_request_t *req) {
    if (!raw || !req || len == 0) {
        return -1;
    }

    memset(req, 0, sizeof(*req));

    const char *separator = NULL;
    for (size_t i = 3; i < len; ++i) {
        if (raw[i - 3] == '\r' && raw[i - 2] == '\n' && raw[i - 1] == '\r' && raw[i] == '\n') {
            separator = &raw[i - 3];
            break;
        }
    }

    if (!separator) {
        return -1;
    }

    size_t header_len = (size_t)(separator - raw);
    char *buffer = malloc(header_len + 1);
    if (!buffer) {
        return -1;
    }

    size_t j = 0;
    for (size_t i = 0; i < header_len; ++i) {
        if (raw[i] == '\r') {
            continue;
        }
        buffer[j++] = raw[i];
    }
    buffer[j] = '\0';

    char *saveptr = NULL;
    char *line = strtok_r(buffer, "\n", &saveptr);
    if (!line) {
        free(buffer);
        return -1;
    }

    if (sscanf(line, "%7s %127s %15s", req->method, req->path, req->version) != 3) {
        free(buffer);
        return -1;
    }

    int header_index = 0;
    while ((line = strtok_r(NULL, "\n", &saveptr)) != NULL && header_index < HTTP_MAX_HEADERS) {
        if (*line == '\0') {
            continue;
        }
        char *colon = strchr(line, ':');
        if (!colon) {
            continue;
        }
        *colon = '\0';
        char *key = line;
        char *value = colon + 1;
        trim_leading(&value);

        strncpy(req->headers[header_index].name, key, HTTP_MAX_HEADER_KEY - 1);
        for (char *p = req->headers[header_index].name; *p; ++p) {
            *p = (char)tolower(*p);
        }
        strncpy(req->headers[header_index].value, value, HTTP_MAX_HEADER_VAL - 1);
        header_index++;
    }

    req->header_count = header_index;
    free(buffer);
    return 0;
}

const char *http_get_header(const http_request_t *req, const char *name) {
    if (!req || !name) {
        return NULL;
    }

    for (int i = 0; i < req->header_count; ++i) {
        if (strcasecmp(req->headers[i].name, name) == 0) {
            return req->headers[i].value;
        }
    }

    return NULL;
}
