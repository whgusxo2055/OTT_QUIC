#ifndef AUTH_HASH_H
#define AUTH_HASH_H

#include <stddef.h>

#define HASH_HEX_SIZE 17

#ifdef __cplusplus
extern "C" {
#endif

void hash_password(const char *password, char *out_hex, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif // AUTH_HASH_H
