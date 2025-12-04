#include "server/quic_tls.h"

#include <stdint.h>
#include <string.h>

/* Placeholder QUIC <-> TLS bridge.
 * This stub prepares deterministic secrets to allow early wiring of CRYPTO handling
 * without pulling OpenSSL during initial development. Replace with real OpenSSL 3.2
 * integration (SSL_CTX/SSL objects + BIO handoff) when TLS is wired. */

int quic_tls_context_init(quic_tls_context_t *ctx, const char *cert_path, const char *key_path) {
    if (!ctx) {
        return -1;
    }
    ctx->cert_path = cert_path;
    ctx->key_path = key_path;
    return 0;
}

void quic_tls_context_free(quic_tls_context_t *ctx) {
    (void)ctx;
}

int quic_tls_session_init(quic_tls_session_t *session, quic_tls_context_t *ctx, uint64_t connection_id) {
    if (!session || !ctx) {
        return -1;
    }
    memset(session, 0, sizeof(*session));
    session->connection_id = connection_id;
    /* For now, synthesize fixed-length pseudo-secrets; replace with HKDF output. */
    for (size_t i = 0; i < sizeof(session->keys.initial_secret); ++i) {
        session->keys.initial_secret[i] = (uint8_t)(connection_id + i);
    }
    return 0;
}

int quic_tls_process_crypto(quic_tls_session_t *session, const uint8_t *crypto_data, size_t crypto_len) {
    if (!session || !crypto_data || crypto_len == 0) {
        return -1;
    }
    /* Stub: pretend handshake completes when any crypto is received. */
    (void)crypto_data;
    (void)crypto_len;
    session->handshake_complete = 1;
    /* Derive placeholder application secret. */
    for (size_t i = 0; i < sizeof(session->keys.application_secret); ++i) {
        session->keys.application_secret[i] = (uint8_t)(session->connection_id ^ (0xA5 + i));
    }
    return 0;
}

int quic_tls_get_1rtt_keys(const quic_tls_session_t *session, quic_tls_keys_t *out_keys) {
    if (!session || !out_keys || !session->handshake_complete) {
        return -1;
    }
    *out_keys = session->keys;
    return 0;
}
