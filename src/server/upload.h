#ifndef SERVER_UPLOAD_H
#define SERVER_UPLOAD_H

#include "db/database.h"

int handle_upload_request(int fd, const char *headers, const char *body, size_t body_len, db_context_t *db);

#endif // SERVER_UPLOAD_H
