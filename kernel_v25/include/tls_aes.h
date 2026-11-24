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
    uint8_t _ctx[200];    /* opaque: wraps tiny-AES struct AES_ctx */
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

/* ------------------------------------------------------------------ */
/* ChaCha20-Poly1305 AEAD (RFC 8439) -- added for the ECDHE-*         */
/* CHACHA20-POLY1305-SHA256 suites (0xCCA8 / 0xCCA9).  The existing  */
/* AES/GCM/CBC paths above are untouched.                              */
/* ------------------------------------------------------------------ */

/* ChaCha20 keystream XOR: `counter` is the initial block counter,
 * `nonce` is the 12-byte per-message nonce.  in/out may alias. */
void tls_chacha20(const uint8_t key[32], uint32_t counter,
                  const uint8_t nonce[12],
                  const uint8_t *in, int len, uint8_t *out);

/* Poly1305 one-shot MAC.  key[32] = one-time Poly1305 key, mac out 16 B. */
void tls_poly1305(const uint8_t key[32],
                  const uint8_t *msg, int len, uint8_t mac[16]);

/* AEAD seal (RFC 8439 2.8): outputs ciphertext to `out` (len bytes) and
 * the 16-byte authentication tag.  Returns 0. */
int tls_chacha_poly_seal(const uint8_t key[32], const uint8_t nonce[12],
                         const uint8_t *aad, int alen,
                         const uint8_t *in, int len,
                         uint8_t *out, uint8_t tag[16]);

/* AEAD open: verifies the tag in constant time, then decrypts into `out`.
 * Returns 0 on success, -1 on tag mismatch / bad input. */
int tls_chacha_poly_open(const uint8_t key[32], const uint8_t nonce[12],
                         const uint8_t *aad, int alen,
                         const uint8_t *in, int len, const uint8_t tag[16],
                         uint8_t *out);

#endif
