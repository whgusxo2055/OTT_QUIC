#include "auth/hash.h"
#include "auth/bcrypt.h"

#include <string.h>

#define BCRYPT_WORKFACTOR 12

void hash_password(const char *password, char *out_hex, size_t out_size) {
    if (!password || !out_hex || out_size < HASH_HEX_SIZE) {
        return;
    }

    char salt[BCRYPT_HASHSIZE];
    char hash[BCRYPT_HASHSIZE];

    if (bcrypt_gensalt(BCRYPT_WORKFACTOR, salt) != 0) {
        return;
    }
    if (bcrypt_hashpw(password, salt, hash) != 0) {
        return;
    }
    strncpy(out_hex, hash, out_size - 1);
    out_hex[out_size - 1] = '\0';
}

int verify_password(const char *password, const char *hash_hex) {
    if (!password || !hash_hex) {
        return -1;
    }
    int rc = bcrypt_checkpw(password, hash_hex);
    return rc == 0 ? 0 : -1;
}
