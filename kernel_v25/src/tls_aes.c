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
#include "../include/kstring.h"

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

/* ================================================================== */
/* ChaCha20 (RFC 8439 section 2.3 / 2.4)                              */
/* ================================================================== */

static uint32_t ch_ld_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void ch_st_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

#define CH_ROT(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define CH_QR(a, b, c, d) do { \
    (a) += (b); (d) ^= (a); (d) = CH_ROT((d), 16); \
    (c) += (d); (b) ^= (c); (b) = CH_ROT((b), 12); \
    (a) += (b); (d) ^= (a); (d) = CH_ROT((d), 8);  \
    (c) += (d); (b) ^= (c); (b) = CH_ROT((b), 7);  \
} while (0)

static void chacha20_block(const uint8_t key[32], uint32_t counter,
                           const uint8_t nonce[12], uint8_t out[64]) {
    static const char sigma[16] = "expand 32-byte k";
    uint32_t s[16], x[16];
    s[0] = ch_ld_le32((const uint8_t *)sigma);
    s[1] = ch_ld_le32((const uint8_t *)sigma + 4);
    s[2] = ch_ld_le32((const uint8_t *)sigma + 8);
    s[3] = ch_ld_le32((const uint8_t *)sigma + 12);
    for (int i = 0; i < 8; i++) s[4 + i] = ch_ld_le32(key + 4 * i);
    s[12] = counter;
    s[13] = ch_ld_le32(nonce + 0);
    s[14] = ch_ld_le32(nonce + 4);
    s[15] = ch_ld_le32(nonce + 8);
    for (int i = 0; i < 16; i++) x[i] = s[i];
    for (int round = 0; round < 10; round++) {
        CH_QR(x[0], x[4], x[8],  x[12]);
        CH_QR(x[1], x[5], x[9],  x[13]);
        CH_QR(x[2], x[6], x[10], x[14]);
        CH_QR(x[3], x[7], x[11], x[15]);
        CH_QR(x[0], x[5], x[10], x[15]);
        CH_QR(x[1], x[6], x[11], x[12]);
        CH_QR(x[2], x[7], x[8],  x[13]);
        CH_QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++) ch_st_le32(out + 4 * i, x[i] + s[i]);
}

void tls_chacha20(const uint8_t key[32], uint32_t counter,
                  const uint8_t nonce[12],
                  const uint8_t *in, int len, uint8_t *out) {
    uint8_t ks[64];
    int off = 0;
    while (len > 0) {
        chacha20_block(key, counter, nonce, ks);
        int n = len < 64 ? len : 64;
        for (int i = 0; i < n; i++) out[off + i] = (uint8_t)(in[off + i] ^ ks[i]);
        off += n; len -= n; counter++;
    }
}

/* ================================================================== */
/* Poly1305 (RFC 8439 section 2.5), 26-bit 5-limb reduction            */
/* ================================================================== */

#define POLY_MASK 0x3ffffffUL

/* Poly1305 state, layout adapted from the public-domain poly1305-donna-32
 * reference (saved under artifacts/overnight-20261124/poly1305-donna-32.h). */
typedef struct {
    unsigned long r[5];
    unsigned long h[5];
    unsigned long pad[4];
    size_t leftover;
    unsigned char buffer[16];
} poly_ctx_t;

static void poly_init(poly_ctx_t *st, const uint8_t key[32]) {
    st->r[0] = (ch_ld_le32(key + 0))       & 0x3ffffffu;
    st->r[1] = (ch_ld_le32(key + 3) >> 2)  & 0x3ffff03u;
    st->r[2] = (ch_ld_le32(key + 6) >> 4)  & 0x3ffc0ffu;
    st->r[3] = (ch_ld_le32(key + 9) >> 6)  & 0x3f03fffu;
    st->r[4] = (ch_ld_le32(key + 12) >> 8) & 0x00fffffu;
    st->h[0] = st->h[1] = st->h[2] = st->h[3] = st->h[4] = 0;
    st->pad[0] = ch_ld_le32(key + 16);
    st->pad[1] = ch_ld_le32(key + 20);
    st->pad[2] = ch_ld_le32(key + 24);
    st->pad[3] = ch_ld_le32(key + 28);
    st->leftover = 0;
}

/* Process one 16-byte block.  `nbytes` is the number of message bytes in the
 * block (1..16); the trailing 0x01 pad byte sets the 2^(8*nbytes) bit per
 * RFC 8439 section 2.5. */
static void poly_block16(poly_ctx_t *st, const uint8_t *m, int nbytes) {
    unsigned long r0 = st->r[0], r1 = st->r[1], r2 = st->r[2];
    unsigned long r3 = st->r[3], r4 = st->r[4];
    unsigned long s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    unsigned long h0 = st->h[0], h1 = st->h[1], h2 = st->h[2];
    unsigned long h3 = st->h[3], h4 = st->h[4];

    h0 += ch_ld_le32(m + 0)       & POLY_MASK;
    h1 += (ch_ld_le32(m + 3) >> 2) & POLY_MASK;
    h2 += (ch_ld_le32(m + 6) >> 4) & POLY_MASK;
    h3 += (ch_ld_le32(m + 9) >> 6) & POLY_MASK;
    h4 += ch_ld_le32(m + 12) >> 8;
    if (nbytes == 16) {
        h4 += 1UL << 24;
    } else {
        int bit = 8 * nbytes, li = bit / 26, mo = bit % 26;
        unsigned long add = 1UL << mo;
        if (li == 0) h0 += add;
        else if (li == 1) h1 += add;
        else if (li == 2) h2 += add;
        else if (li == 3) h3 += add;
        else h4 += add;
    }

    uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 +
                  (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
    uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 +
                  (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
    uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 +
                  (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
    uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 +
                  (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
    uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 +
                  (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
    unsigned long c;
    c = (unsigned long)(d0 >> 26); h0 = (unsigned long)d0 & POLY_MASK; d1 += c;
    c = (unsigned long)(d1 >> 26); h1 = (unsigned long)d1 & POLY_MASK; d2 += c;
    c = (unsigned long)(d2 >> 26); h2 = (unsigned long)d2 & POLY_MASK; d3 += c;
    c = (unsigned long)(d3 >> 26); h3 = (unsigned long)d3 & POLY_MASK; d4 += c;
    c = (unsigned long)(d4 >> 26); h4 = (unsigned long)d4 & POLY_MASK;
    h0 += c * 5; c = h0 >> 26; h0 &= POLY_MASK; h1 += c;

    st->h[0] = h0; st->h[1] = h1; st->h[2] = h2;
    st->h[3] = h3; st->h[4] = h4;
}

static void poly_feed(poly_ctx_t *st, const uint8_t *msg, int len) {
    while (len > 0) {
        int n = len < 16 - (int)st->leftover ? len : 16 - (int)st->leftover;
        for (int i = 0; i < n; i++) st->buffer[st->leftover + i] = msg[i];
        st->leftover += n; msg += n; len -= n;
        if (st->leftover == 16) { poly_block16(st, st->buffer, 16); st->leftover = 0; }
    }
}

static void poly_final(poly_ctx_t *st, const uint8_t key[32], uint8_t mac[16]) {
    if (st->leftover) {
        int n = (int)st->leftover;
        for (int i = n; i < 16; i++) st->buffer[i] = 0;
        poly_block16(st, st->buffer, n);
        st->leftover = 0;
    }

    unsigned long h0 = st->h[0], h1 = st->h[1], h2 = st->h[2];
    unsigned long h3 = st->h[3], h4 = st->h[4], c;
    c = h1 >> 26; h1 &= POLY_MASK; h2 += c;
    c = h2 >> 26; h2 &= POLY_MASK; h3 += c;
    c = h3 >> 26; h3 &= POLY_MASK; h4 += c;
    c = h4 >> 26; h4 &= POLY_MASK; h0 += c * 5; c = h0 >> 26; h0 &= POLY_MASK; h1 += c;

    unsigned long g0, g1, g2, g3, g4, mask;
    g0 = h0 + 5; c = g0 >> 26; g0 &= POLY_MASK;
    g1 = h1 + c; c = g1 >> 26; g1 &= POLY_MASK;
    g2 = h2 + c; c = g2 >> 26; g2 &= POLY_MASK;
    g3 = h3 + c; c = g3 >> 26; g3 &= POLY_MASK;
    g4 = h4 + c - (1UL << 26);
    mask = (g4 >> ((sizeof(unsigned long) * 8) - 1)) - 1;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2; h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;

    h0 = ((h0) | (h1 << 26)) & 0xffffffffUL;
    h1 = ((h1 >> 6) | (h2 << 20)) & 0xffffffffUL;
    h2 = ((h2 >> 12) | (h3 << 14)) & 0xffffffffUL;
    h3 = ((h3 >> 18) | (h4 << 8))  & 0xffffffffUL;

    uint64_t f;
    f = (uint64_t)h0 + st->pad[0];                   h0 = (unsigned long)f;
    f = (uint64_t)h1 + st->pad[1] + (f >> 32);       h1 = (unsigned long)f;
    f = (uint64_t)h2 + st->pad[2] + (f >> 32);       h2 = (unsigned long)f;
    f = (uint64_t)h3 + st->pad[3] + (f >> 32);      h3 = (unsigned long)f;

    ch_st_le32(mac + 0, h0); ch_st_le32(mac + 4, h1);
    ch_st_le32(mac + 8, h2); ch_st_le32(mac + 12, h3);
    (void)key;
}

void tls_poly1305(const uint8_t key[32],
                  const uint8_t *msg, int len, uint8_t mac[16]) {
    poly_ctx_t pc;
    poly_init(&pc, key);
    poly_feed(&pc, msg, len);
    poly_final(&pc, key, mac);
}

/* ------------------------------------------------------------------ */
/* AEAD (RFC 8439 section 2.8)                                         */
/* ------------------------------------------------------------------ */

/* Reusable scratch for the one-shot Poly message:
 * AAD || pad16(AAD) || CT || pad16(CT) || alen LE64 || dlen LE64.
 * TLS is a single blocking connection, so this block is shared; it must
 * stay well under the .bss ceiling (no hundred-KB array). */
#define AEAD_MSG_MAX (16 + 16384 + 32)
static uint8_t g_aead_msg[AEAD_MSG_MAX];

static int aead_poly(const uint8_t pkey[32], const uint8_t *aad, int alen,
                     const uint8_t *ct, int ctlen, uint8_t tag[16]) {
    int m = 0;
    memcpy(g_aead_msg + m, aad, alen); m += alen;
    int pad = (16 - alen % 16) % 16;
    memset(g_aead_msg + m, 0, pad); m += pad;
    memcpy(g_aead_msg + m, ct, ctlen); m += ctlen;
    pad = (16 - ctlen % 16) % 16;
    memset(g_aead_msg + m, 0, pad); m += pad;
    uint64_t a = (uint64_t)alen, d = (uint64_t)ctlen;
    for (int i = 0; i < 8; i++) {
        g_aead_msg[m + i]     = (uint8_t)(a >> (8 * i));
        g_aead_msg[m + 8 + i] = (uint8_t)(d >> (8 * i));
    }
    m += 16;
    tls_poly1305(pkey, g_aead_msg, m, tag);
    return 0;
}

int tls_chacha_poly_seal(const uint8_t key[32], const uint8_t nonce[12],
                         const uint8_t *aad, int alen,
                         const uint8_t *in, int len,
                         uint8_t *out, uint8_t tag[16]) {
    uint8_t pkey[32], zero[32];
    for (int i = 0; i < 32; i++) zero[i] = 0;
    tls_chacha20(key, 0, nonce, zero, 32, pkey);    // one-time Poly key
    tls_chacha20(key, 1, nonce, in, len, out);
    aead_poly(pkey, aad, alen, out, len, tag);
    return 0;
}

int tls_chacha_poly_open(const uint8_t key[32], const uint8_t nonce[12],
                         const uint8_t *aad, int alen,
                         const uint8_t *in, int len, const uint8_t tag[16],
                         uint8_t *out) {
    uint8_t pkey[32], zero[32];
    uint8_t calc[16];
    for (int i = 0; i < 32; i++) zero[i] = 0;
    tls_chacha20(key, 0, nonce, zero, 32, pkey);

    aead_poly(pkey, aad, alen, in, len, calc);

    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(calc[i] ^ tag[i]);
    if (diff != 0) return -1;
    tls_chacha20(key, 1, nonce, in, len, out);
    return 0;
}
