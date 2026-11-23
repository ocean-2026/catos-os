/* tls_aes.c -- AES-128-CBC wrappers + TLS 1.2 PRF for the CatOS HTTPS client (D2).
 *
 * The block cipher is the vendored tiny-AES-c implementation (public domain,
 * unlicense; src/third_party/tiny-aes/aes.c), verified on host against the
 * NIST SP 800-38A ECB/CBC vectors and 100 random CBC round-trips.  The PRF
 * (P_SHA256, RFC 5246 section 5) is implemented here over the kernel HMAC-SHA256
 * primitive from crypto.c.
 */

#include "../include/tls_aes.h"
#include "third_party/tiny-aes/aes.h"
#include "../include/crypto.h"

void tls_aes128_key(tls_aes128_t *c, const uint8_t key[16]) {
    AES_init_ctx((struct AES_ctx *)c, key);
}

void tls_aes128_cbc_enc(const tls_aes128_t *c, const uint8_t iv[16],
                        const uint8_t *in, uint8_t *out, int blocks) {
    struct AES_ctx *x = (struct AES_ctx *)c;
    AES_ctx_set_iv(x, iv);
    for (int i = 0; i < blocks * 16; i++) out[i] = in[i];
    AES_CBC_encrypt_buffer(x, out, (size_t)blocks * 16);
}

void tls_aes128_cbc_dec(const tls_aes128_t *c, const uint8_t iv[16],
                        const uint8_t *in, uint8_t *out, int blocks) {
    struct AES_ctx *x = (struct AES_ctx *)c;
    AES_ctx_set_iv(x, iv);
    for (int i = 0; i < blocks * 16; i++) out[i] = in[i];
    AES_CBC_decrypt_buffer(x, out, (size_t)blocks * 16);
}

/* ------------------------------------------------------------------ */
/* TLS 1.2 PRF -- P_SHA256 (RFC 5246 section 5)                       */
/* ------------------------------------------------------------------ */

void tls_prf(uint8_t *out, int outlen,
             const uint8_t *secret, int slen,
             const char *label,
             const uint8_t *seed, int seedlen) {
    uint8_t ls[200];
    uint8_t A[32], blk[32], t[32];
    int ll = 0, i;

    for (i = 0; label[i] && ll < 190; i++) ls[ll++] = (uint8_t)label[i];
    for (i = 0; i < seedlen && ll < 200; i++) ls[ll++] = seed[i];

    /* A(1) = HMAC(secret, label || seed) */
    hmac_sha256(A, secret, slen, ls, ll, 0, 0);
    while (outlen > 0) {
        int take;
        /* P_i = HMAC(secret, A(i) || label || seed) */
        hmac_sha256(blk, secret, slen, A, 32, ls, ll);
        take = outlen < 32 ? outlen : 32;
        for (i = 0; i < take; i++) out[i] = blk[i];
        out += take; outlen -= take;
        /* A(i+1) = HMAC(secret, A(i)) */
        hmac_sha256(t, secret, slen, A, 32, 0, 0);
        for (i = 0; i < 32; i++) A[i] = t[i];
    }
}
