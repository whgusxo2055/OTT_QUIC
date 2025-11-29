#include "auth/hash.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

void hash_password(const char *password, char *out_hex, size_t out_size) {
    if (!out_hex || out_size < HASH_HEX_SIZE) {
        return;
    }

    const uint64_t fnv_prime = 1099511628211ULL;
    uint64_t hash = 1469598103934665603ULL ^ 0x9e3779b97f4a7c15ULL;

    if (password) {
        size_t len = strlen(password);
        for (size_t i = 0; i < len; ++i) {
            hash ^= (uint64_t)(unsigned char)password[i];
            hash *= fnv_prime;
            hash ^= (uint64_t)i + 0x100000001b3ULL;
        }
    }

    snprintf(out_hex, out_size, "%016llx", (unsigned long long)hash);
}
