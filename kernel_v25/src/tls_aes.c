/* tls_aes.c -- AES-128-CBC + TLS 1.2 PRF for the CatOS HTTPS client (D2).
 *
 * Standalone, freestanding, integer-only.  The AES key schedule follows the
 * same GF(2^8) construction as kernel_v25/src/crypto.c; the inverse cipher is
 * added here because the CTR primitive in crypto.c only encrypts.
 */

#include "../include/tls_aes.h"
#include "../include/crypto.h"

/* ------------------------------------------------------------------ */
/* AES S-box / inverse S-box                                           */
/* ------------------------------------------------------------------ */

static uint8_t aes_sbox[256];
static uint8_t aes_inv_sbox[256];
static int aes_tables_ready = 0;

static uint8_t gf_xt(uint8_t x) {
    return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b));
}

static uint8_t gf_mul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    while (b) {
        if (b & 1) p ^= a;
        a = gf_xt(a);
        b >>= 1;
    }
    return p;
}

static void aes_build_tables(void) {
    uint8_t logt[256], alog[256], x = 1;
    int i;
    for (i = 0; i < 255; i++) {
        logt[x] = (uint8_t)i;
        alog[i] = x;
        x = gf_mul(x, 3);
    }
    alog[255] = 1;
    for (i = 0; i < 256; i++) {
        uint8_t inv = (i == 0) ? 0 : alog[255 - logt[i]];
        uint8_t s = inv ^ (uint8_t)((inv << 1) | (inv >> 7))
                        ^ (uint8_t)((inv << 2) | (inv >> 6))
                        ^ (uint8_t)((inv << 3) | (inv >> 5))
                        ^ (uint8_t)((inv << 4) | (inv >> 4))
                        ^ 0x63;
        aes_sbox[i] = s;
        aes_inv_sbox[s] = (uint8_t)i;
    }
    aes_tables_ready = 1;
}

static const uint8_t rcon_tab[11] = { 0x00, 0x01, 0x02, 0x04, 0x08,
                                      0x10, 0x20, 0x40, 0x80, 0x1b, 0x36 };

void tls_aes128_key(tls_aes128_t *c, const uint8_t key[16]) {
    int i;
    if (!aes_tables_ready) aes_build_tables();
    for (i = 0; i < 16; i++) c->rk[i] = key[i];
    for (i = 16; i < 176; i += 4) {
        uint8_t t[4];
        int j;
        for (j = 0; j < 4; j++) t[j] = c->rk[i - 4 + j];
        if ((i % 16) == 0) {
            uint8_t tmp = t[0];
            t[0] = (uint8_t)(aes_sbox[t[1]] ^ rcon_tab[i / 16]);
            t[1] = aes_sbox[t[2]];
            t[2] = aes_sbox[t[3]];
            t[3] = aes_sbox[tmp];
        }
        for (j = 0; j < 4; j++) c->rk[i + j] = (uint8_t)(c->rk[i - 16 + j] ^ t[j]);
    }
}

/* state layout: s[row + 4*col], matching the column-major AES state. */

static void add_round_key(uint8_t *s, const uint8_t *rk, int round) {
    int i;
    for (i = 0; i < 16; i++) s[i] ^= rk[16 * round + i];
}

static void sub_bytes(uint8_t *s) {
    int i;
    for (i = 0; i < 16; i++) s[i] = aes_sbox[s[i]];
}

static void inv_sub_bytes(uint8_t *s) {
    int i;
    for (i = 0; i < 16; i++) s[i] = aes_inv_sbox[s[i]];
}

static void shift_rows(uint8_t *s) {
    uint8_t t[16];
    int r, c, i;
    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++)
            t[r + 4 * c] = s[r + 4 * ((c + r) & 3)];
    for (i = 0; i < 16; i++) s[i] = t[i];
}

static void inv_shift_rows(uint8_t *s) {
    uint8_t t[16];
    int r, c, i;
    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++)
            t[r + 4 * c] = s[r + 4 * ((c - r + 4) & 3)];
    for (i = 0; i < 16; i++) s[i] = t[i];
}

static void mix_columns(uint8_t *s) {
    int c;
    for (c = 0; c < 4; c++) {
        uint8_t a0 = s[4*c], a1 = s[4*c+1], a2 = s[4*c+2], a3 = s[4*c+3];
        s[4*c+0] = (uint8_t)(gf_mul(a0,2) ^ gf_mul(a1,3) ^ a2 ^ a3);
        s[4*c+1] = (uint8_t)(a0 ^ gf_mul(a1,2) ^ gf_mul(a2,3) ^ a3);
        s[4*c+2] = (uint8_t)(a0 ^ a1 ^ gf_mul(a2,2) ^ gf_mul(a3,3));
        s[4*c+3] = (uint8_t)(gf_mul(a0,3) ^ a1 ^ a2 ^ gf_mul(a3,2));
    }
}

static void inv_mix_columns(uint8_t *s) {
    int c;
    for (c = 0; c < 4; c++) {
        uint8_t a0 = s[4*c], a1 = s[4*c+1], a2 = s[4*c+2], a3 = s[4*c+3];
        s[4*c+0] = (uint8_t)(gf_mul(a0,0x0e) ^ gf_mul(a1,0x0b) ^ gf_mul(a2,0x0d) ^ gf_mul(a3,0x09));
        s[4*c+1] = (uint8_t)(gf_mul(a0,0x09) ^ gf_mul(a1,0x0e) ^ gf_mul(a2,0x0b) ^ gf_mul(a3,0x0d));
        s[4*c+2] = (uint8_t)(gf_mul(a0,0x0d) ^ gf_mul(a1,0x09) ^ gf_mul(a2,0x0e) ^ gf_mul(a3,0x0b));
        s[4*c+3] = (uint8_t)(gf_mul(a0,0x0b) ^ gf_mul(a1,0x0d) ^ gf_mul(a2,0x09) ^ gf_mul(a3,0x0e));
    }
}

static void aes_enc_block(const tls_aes128_t *c, const uint8_t in[16],
                          uint8_t out[16]) {
    uint8_t s[16];
    int r, i;
    for (i = 0; i < 16; i++) s[i] = in[i];
    add_round_key(s, c->rk, 0);
    for (r = 1; r <= 9; r++) {
        sub_bytes(s);
        shift_rows(s);
        mix_columns(s);
        add_round_key(s, c->rk, r);
    }
    sub_bytes(s);
    shift_rows(s);
    add_round_key(s, c->rk, 10);
    for (i = 0; i < 16; i++) out[i] = s[i];
}

static void aes_dec_block(const tls_aes128_t *c, const uint8_t in[16],
                          uint8_t out[16]) {
    uint8_t s[16];
    int r, i;
    for (i = 0; i < 16; i++) s[i] = in[i];
    add_round_key(s, c->rk, 10);
    for (r = 9; r >= 1; r--) {
        inv_shift_rows(s);
        inv_sub_bytes(s);
        inv_mix_columns(s);
        add_round_key(s, c->rk, r);
    }
    inv_shift_rows(s);
    inv_sub_bytes(s);
    add_round_key(s, c->rk, 0);
    for (i = 0; i < 16; i++) out[i] = s[i];
}

void tls_aes128_cbc_enc(const tls_aes128_t *c, const uint8_t iv[16],
                        const uint8_t *in, uint8_t *out, int blocks) {
    uint8_t prev[16];
    int b, i;
    for (i = 0; i < 16; i++) prev[i] = iv[i];
    for (b = 0; b < blocks; b++) {
        uint8_t x[16], y[16];
        for (i = 0; i < 16; i++) x[i] = in[16*b+i] ^ prev[i];
        aes_enc_block(c, x, y);
        for (i = 0; i < 16; i++) { out[16*b+i] = y[i]; prev[i] = y[i]; }
    }
}

void tls_aes128_cbc_dec(const tls_aes128_t *c, const uint8_t iv[16],
                        const uint8_t *in, uint8_t *out, int blocks) {
    uint8_t prev[16];
    int b, i;
    for (i = 0; i < 16; i++) prev[i] = iv[i];
    for (b = 0; b < blocks; b++) {
        uint8_t y[16], x[16];
        for (i = 0; i < 16; i++) y[i] = in[16*b+i];
        aes_dec_block(c, y, x);
        for (i = 0; i < 16; i++) { out[16*b+i] = x[i] ^ prev[i]; prev[i] = y[i]; }
    }
}

/* ------------------------------------------------------------------ */
/* TLS 1.2 PRF -- P_SHA256 (RFC 5246 section 5)                       */
/* ------------------------------------------------------------------ */

void tls_prf(uint8_t *out, int outlen,
             const uint8_t *secret, int slen,
             const char *label,
             const uint8_t *seed, int seedlen) {
    uint8_t A[64], block[32], ls[160], tmp[32];
    int ll = 0, i;
    int first = 1;

    while (label[ll]) { ls[ll] = (uint8_t)label[ll]; ll++; }
    for (i = 0; i < seedlen && ll < 160; i++) ls[ll++] = seed[i];

    for (i = 0; i < seedlen && i < 64; i++) A[i] = seed[i];  /* A(0) = seed */

    while (outlen > 0) {
        /* A(i) = HMAC(secret, A(i-1)); block = HMAC(secret, A(i)||label||seed) */
        if (first) {
            hmac_sha256(tmp, secret, slen, A, seedlen, 0, 0);
            first = 0;
        } else {
            hmac_sha256(tmp, secret, slen, A, 32, 0, 0);
        }
        for (i = 0; i < 32; i++) A[i] = tmp[i];
        hmac_sha256(block, secret, slen, A, 32, ls, ll);
        int take = outlen < 32 ? outlen : 32;
        for (i = 0; i < take; i++) out[i] = block[i];
        out += take; outlen -= take;
    }
}
