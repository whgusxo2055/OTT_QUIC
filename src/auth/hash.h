#ifndef AUTH_HASH_H
#define AUTH_HASH_H

#include <stddef.h>

#define HASH_HEX_SIZE 61 /* bcrypt 60 chars + null */

#ifdef __cplusplus
extern "C" {
#endif

void hash_password(const char *password, char *out_hex, size_t out_size);
int verify_password(const char *password, const char *hash_hex);

#ifdef __cplusplus
}
#endif

#endif // AUTH_HASH_H
