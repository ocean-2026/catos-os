#ifndef TLS_AES_H
#define TLS_AES_H

#include <stdint.h>

/* TLS 1.2 record-crypto helpers (D2: HTTPS client).
 *
 * AES-128-CBC with explicit per-record IV (TLS 1.2 server->client records
 * carry a 16-byte IV in the clear; the MAC is computed by the record layer
 * in tls.c, this module only does the block cipher).
 *
 * PKCS#7 padding is stripped/added by the record layer, not here.
 */

typedef struct {
    uint8_t rk[176];      /* 11 round keys, AES-128 */
} tls_aes128_t;

void tls_aes128_key(tls_aes128_t *c, const uint8_t key[16]);

/* Encrypt/decrypt `blocks` 16-byte blocks in CBC mode.  in/out may alias
 * only when in == out and blocks == 1; otherwise pass distinct buffers. */
void tls_aes128_cbc_enc(const tls_aes128_t *c, const uint8_t iv[16],
                        const uint8_t *in, uint8_t *out, int blocks);
void tls_aes128_cbc_dec(const tls_aes128_t *c, const uint8_t iv[16],
                        const uint8_t *in, uint8_t *out, int blocks);

/* TLS 1.2 PRF (RFC 5246 5): P_SHA256(secret, label || seed).
 * Fills `out` with `outlen` bytes.  label is a NUL-terminated C string. */
void tls_prf(uint8_t *out, int outlen,
             const uint8_t *secret, int slen,
             const char *label,
             const uint8_t *seed, int seedlen);

#endif
