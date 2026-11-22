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
    uint8_t A[160], block[32], ls[160], tmp[32];
    int ll = 0, i;
    int first = 1;

    for (i = 0; label[ll]; i++) { ls[ll] = (uint8_t)label[ll]; ll++; }
    for (i = 0; i < seedlen && ll < 160; i++) ls[ll++] = seed[i];

    /* A(0) = label || seed: the full P_hash seed (RFC 5246 section 5).
     * The label is part of the seed fed into the A-chain, not only into
     * the output block. */
    for (i = 0; i < ll; i++) A[i] = ls[i];

    while (outlen > 0) {
        /* A(i) = HMAC(secret, A(i-1)); block = HMAC(secret, A(i)||label||seed) */
        hmac_sha256(tmp, secret, slen, A, first ? ll : 32, 0, 0);
        first = 0;
        for (i = 0; i < 32; i++) A[i] = tmp[i];
        hmac_sha256(block, secret, slen, A, 32, ls, ll);
        int take = outlen < 32 ? outlen : 32;
        for (i = 0; i < take; i++) out[i] = block[i];
        out += take; outlen -= take;
    }
}
