#ifndef CRYPTO_H
#define CRYPTO_H

#include <stdint.h>

/*
 * CatOS v2.71 SSH cryptography primitives.
 *
 * Everything the kernel SSH-2 server needs, self-contained (no libc):
 *   - SHA-256            (exchange hash H, key derivation)
 *   - HMAC-SHA256        (packet MAC, hmac-sha2-256)
 *   - SHA-512            (Ed25519 internals; TweetNaCl-derived)
 *   - AES-128-CTR        (aes128-ctr, RFC 4344 SDCTR: continuous
 *                         big-endian 128-bit counter keystream that
 *                         must persist across packets)
 *   - X25519             (curve25519-sha256 KEX; TweetNaCl-derived)
 *   - Ed25519            (ssh-ed25519 host key; TweetNaCl-derived)
 *
 * The curve code is a trimmed port of tweetnacl.c (D. J. Bernstein,
 * Wesley Janssen, Peter Schwachse, Daniel J. Bernstein et al., public
 * domain).  The same file compiles on the host for self-testing
 * (gcc -DCRYPTO_SELFTEST) and inside the kernel.
 */

/* ---- SHA-256 / HMAC-SHA256 ---- */
void sha256(uint8_t out[32], const void *in, uint32_t n);
void sha256_2(uint8_t out[32], const uint8_t *a, uint32_t la,
              const uint8_t *b, uint32_t lb);   /* SHA-256(a || b), no copy */
void hmac_sha256(uint8_t out[32], const uint8_t *key, uint32_t klen,
                 const uint8_t *m1, uint32_t l1,
                 const uint8_t *m2, uint32_t l2);
/* hmac_sha256 computes HMAC over m1 || m2 (m2 may be 0/NULL). */

/* ---- SHA-512 (Ed25519 internals; also handy) ---- */
void crypto_sha512(uint8_t out[64], const uint8_t *m, uint64_t n);

/* ---- X25519 ---- */
void crypto_x25519_pub(uint8_t pub[32], const uint8_t priv[32]);
void crypto_x25519(uint8_t shared[32], const uint8_t priv[32],
                   const uint8_t peer[32]);

/* ---- Ed25519 ---- */
void crypto_ed25519_keypair(uint8_t pk[32], uint8_t sk[64]);
void crypto_ed25519_sign(uint8_t sig[64], const uint8_t *m, uint32_t n,
                         const uint8_t sk[64]);
/* m must be <= 96 bytes (the exchange hash H is 32). */

/* ---- AES-128-CTR (continuous keystream across packets) ---- */
typedef struct {
    uint8_t rk[176];      /* expanded key (11 round keys) */
    uint8_t ctr[16];      /* big-endian 128-bit counter, persists */
    uint8_t ks[16];       /* buffered keystream */
    int     ks_len;       /* bytes of ks not yet consumed */
} aes128_ctr_t;

void crypto_aes128_ctr_init(aes128_ctr_t *c, const uint8_t key[16],
                            const uint8_t iv[16]);
void crypto_aes128_ctr_crypt(aes128_ctr_t *c, uint8_t *buf, uint32_t len);

/* ---- kernel RNG (rdtsc-mixed xorshift; demo-grade entropy) ---- */
void crypto_random(uint8_t *buf, int n);

#endif
