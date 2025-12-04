#ifndef SERVER_QUIC_TLS_H
#define SERVER_QUIC_TLS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t initial_secret[32];
    uint8_t handshake_secret[32];
    uint8_t application_secret[32];
} quic_tls_keys_t;

typedef struct {
    const char *cert_path;
    const char *key_path;
} quic_tls_context_t;

typedef struct {
    uint64_t connection_id;
    int handshake_complete;
    quic_tls_keys_t keys;
} quic_tls_session_t;

int quic_tls_context_init(quic_tls_context_t *ctx, const char *cert_path, const char *key_path);
void quic_tls_context_free(quic_tls_context_t *ctx);

int quic_tls_session_init(quic_tls_session_t *session, quic_tls_context_t *ctx, uint64_t connection_id);
int quic_tls_process_crypto(quic_tls_session_t *session, const uint8_t *crypto_data, size_t crypto_len);
int quic_tls_get_1rtt_keys(const quic_tls_session_t *session, quic_tls_keys_t *out_keys);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_QUIC_TLS_H */
