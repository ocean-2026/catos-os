/* p256.c -- secp256r1 (NIST P-256) ECDHE for the CatOS HTTPS client (D2).
 *
 * Field arithmetic and the fast P-256 reduction follow the micro-ecc generic-C
 * reference (Copyright 2014 Kenneth MacKay, BSD 2-clause;
 * https://github.com/kmackay/micro-ecc).  The point layer here uses plain
 * affine coordinates (double-and-add) instead of the original Jacobian
 * ladder; the host-verified field multiply and Fermat inverse are sufficient
 * for a single handshake and keep the point math auditable.
 *
 * Wire format: public keys are the 65-byte uncompressed point
 * 0x04 || X(32) || Y(32), big-endian (TLS ServerKeyExchange /
 * ClientKeyExchange).  Scalars are 32-byte big-endian.
 */

#include "../include/p256.h"

typedef uint32_t word_t;
#define NWORDS 8

/* ------------------------------ curve constants --------------------- */

static const uint8_t PP_BE[32] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};
static const uint8_t NN_BE[32] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xBC, 0xE6, 0xFA, 0xAD, 0xA7, 0x17, 0x9E, 0x84,
    0xF3, 0xB9, 0xCA, 0xC2, 0xFC, 0x63, 0x25, 0x51,
};
static const uint8_t GX_BE[32] = {
    0x6B, 0x17, 0xD1, 0xF2, 0xE1, 0x2C, 0x42, 0x47,
    0xF8, 0xBC, 0xE6, 0xE5, 0x63, 0xA4, 0x40, 0xF2,
    0x77, 0x03, 0x7D, 0x81, 0x2D, 0xEB, 0x33, 0xA0,
    0xF4, 0xA1, 0x39, 0x45, 0xD8, 0x98, 0xC2, 0x96,
};
static const uint8_t GY_BE[32] = {
    0x4F, 0xE3, 0x42, 0xE2, 0xFE, 0x1A, 0x7F, 0x9B,
    0x8E, 0xE7, 0xEB, 0x4A, 0x7C, 0x0F, 0x9E, 0x16,
    0x2B, 0xCE, 0x33, 0x57, 0x6B, 0x31, 0x5E, 0xCE,
    0xCB, 0xB6, 0x40, 0x68, 0x37, 0xBF, 0x51, 0xF5,
};

static word_t PP[NWORDS], NN[NWORDS], GX[NWORDS], GY[NWORDS];
static int C_ready = 0;

static void bytes_to_native(word_t *native, const uint8_t *bytes) {
    int i;
    for (i = 0; i < NWORDS; i++) native[i] = 0;
    for (i = 0; i < 32; i++) {
        unsigned b = (unsigned)(31 - i);
        native[b / 4] |= (word_t)bytes[i] << (8 * (b % 4));
    }
}

static void native_to_bytes(uint8_t *bytes, const word_t *native) {
    int i;
    for (i = 0; i < 32; i++) {
        unsigned b = (unsigned)(31 - i);
        bytes[i] = (uint8_t)(native[b / 4] >> (8 * (b % 4)));
    }
}

static void curve_init(void) {
    if (C_ready) return;
    bytes_to_native(PP, PP_BE);
    bytes_to_native(NN, NN_BE);
    bytes_to_native(GX, GX_BE);
    bytes_to_native(GY, GY_BE);
    C_ready = 1;
}

/* ------------------------------ VLI helpers ------------------------- */

static void vli_clear(word_t *v) { for (int i = 0; i < NWORDS; i++) v[i] = 0; }

static word_t vli_isZero(const word_t *v) {
    word_t z = 0;
    for (int i = 0; i < NWORDS; i++) z |= v[i];
    return z == 0;
}

static word_t vli_testBit(const word_t *v, int bit) {
    return (v[bit >> 5] >> (bit & 31)) & 1u;
}

static void vli_set(word_t *dst, const word_t *src) {
    for (int i = 0; i < NWORDS; i++) dst[i] = src[i];
}

static int vli_cmp(const word_t *l, const word_t *r) {
    for (int i = NWORDS - 1; i >= 0; i--)
        if (l[i] != r[i]) return l[i] < r[i] ? -1 : 1;
    return 0;
}

static word_t vli_sub(word_t *res, const word_t *l, const word_t *r) {
    word_t borrow = 0;
    for (int i = 0; i < NWORDS; i++) {
        word_t diff = l[i] - r[i] - borrow;
        if (diff != l[i]) borrow = diff > l[i];
        res[i] = diff;
    }
    return borrow;
}

static word_t vli_add(word_t *res, const word_t *l, const word_t *r) {
    word_t carry = 0;
    for (int i = 0; i < NWORDS; i++) {
        word_t sum = l[i] + r[i] + carry;
        if (sum != l[i]) carry = sum < l[i];
        res[i] = sum;
    }
    return carry;
}

static void muladd(word_t a, word_t b, word_t *r0, word_t *r1, word_t *r2) {
    uint64_t p = (uint64_t)a * b;
    uint64_t r01 = ((uint64_t)(*r1) << 32) | *r0;
    r01 += p;
    *r2 += (r01 < p);
    *r1 = (word_t)(r01 >> 32);
    *r0 = (word_t)r01;
}

static void vli_mult(word_t *res, const word_t *l, const word_t *r) {
    word_t r0 = 0, r1 = 0, r2 = 0;
    for (int k = 0; k < NWORDS; k++) {
        for (int i = 0; i <= k; i++) muladd(l[i], r[k - i], &r0, &r1, &r2);
        res[k] = r0; r0 = r1; r1 = r2; r2 = 0;
    }
    for (int k = NWORDS; k < 2 * NWORDS - 1; k++) {
        for (int i = (k + 1) - NWORDS; i < NWORDS; i++)
            muladd(l[i], r[k - i], &r0, &r1, &r2);
        res[k] = r0; r0 = r1; r1 = r2; r2 = 0;
    }
    res[2 * NWORDS - 1] = r0;
}

/* P-256 fast reduction (NSA FIPS 186 style, ported from micro-ecc
 * curve-specific.inc). */
static void mmod_fast(word_t *res, word_t *prod) {
    word_t tmp[NWORDS];
    int carry = 0;

    vli_set(res, prod);

    tmp[0] = tmp[1] = tmp[2] = 0;
    tmp[3] = prod[11]; tmp[4] = prod[12]; tmp[5] = prod[13];
    tmp[6] = prod[14]; tmp[7] = prod[15];
    carry  = vli_add(tmp, tmp, tmp);
    carry += vli_add(res, res, tmp);

    tmp[3] = prod[12]; tmp[4] = prod[13]; tmp[5] = prod[14];
    tmp[6] = prod[15]; tmp[7] = 0;
    carry += vli_add(tmp, tmp, tmp);
    carry += vli_add(res, res, tmp);

    tmp[0] = prod[8];  tmp[1] = prod[9];  tmp[2] = prod[10];
    tmp[3] = tmp[4] = tmp[5] = 0;
    tmp[6] = prod[14]; tmp[7] = prod[15];
    carry += vli_add(res, res, tmp);

    tmp[0] = prod[9];  tmp[1] = prod[10]; tmp[2] = prod[11];
    tmp[3] = prod[13]; tmp[4] = prod[14]; tmp[5] = prod[15];
    tmp[6] = prod[13]; tmp[7] = prod[8];
    carry += vli_add(res, res, tmp);

    tmp[0] = prod[11]; tmp[1] = prod[12]; tmp[2] = prod[13];
    tmp[3] = tmp[4] = tmp[5] = 0;
    tmp[6] = prod[8];  tmp[7] = prod[10];
    carry -= vli_sub(res, res, tmp);

    tmp[0] = prod[12]; tmp[1] = prod[13]; tmp[2] = prod[14];
    tmp[3] = prod[15]; tmp[4] = tmp[5] = 0;
    tmp[6] = prod[9];  tmp[7] = prod[11];
    carry -= vli_sub(res, res, tmp);

    tmp[0] = prod[13]; tmp[1] = prod[14]; tmp[2] = prod[15];
    tmp[3] = prod[8];  tmp[4] = prod[9];  tmp[5] = prod[10];
    tmp[6] = 0;        tmp[7] = prod[12];
    carry -= vli_sub(res, res, tmp);

    tmp[0] = prod[14]; tmp[1] = prod[15]; tmp[2] = 0;
    tmp[3] = prod[9];  tmp[4] = prod[10]; tmp[5] = prod[11];
    tmp[6] = 0;        tmp[7] = prod[13];
    carry -= vli_sub(res, res, tmp);

    if (carry < 0) {
        do { carry += vli_add(res, res, PP); } while (carry < 0);
    } else {
        while (carry || vli_cmp(PP, res) != 1)
            carry -= vli_sub(res, res, PP);
    }
}

static void vli_modMult(word_t *res, const word_t *l, const word_t *r) {
    word_t product[2 * NWORDS];
    vli_mult(product, l, r);
    mmod_fast(res, product);
}

/* Modular inverse via Fermat: a^-1 = a^(p-2) mod p. */
static void vli_modInv(word_t *res, const word_t *input) {
    static const uint8_t PMINUS2_BE[32] = {
        0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFD };
    word_t exp[NWORDS], base[NWORDS], acc[NWORDS];
    if (vli_isZero(input)) { vli_clear(res); return; }
    bytes_to_native(exp, PMINUS2_BE);
    vli_set(base, input);
    vli_clear(acc); acc[0] = 1;
    for (int bit = 255; bit >= 0; --bit) {
        vli_modMult(acc, acc, acc);
        if (vli_testBit(exp, bit)) vli_modMult(acc, acc, base);
    }
    vli_set(res, acc);
}

static void modAdd(word_t *res, const word_t *l, const word_t *r) {
    word_t carry = vli_add(res, l, r);
    if (carry || vli_cmp(PP, res) != 1) vli_sub(res, res, PP);
}

static void modSub(word_t *res, const word_t *l, const word_t *r) {
    word_t borrow = vli_sub(res, l, r);
    if (borrow) vli_add(res, res, PP);
}

/* ------------------------- affine point arithmetic ------------------- */

typedef struct { int inf; word_t x[NWORDS], y[NWORDS]; } pt_t;

static void pt_set(pt_t *r, const pt_t *p) {
    r->inf = p->inf; vli_set(r->x, p->x); vli_set(r->y, p->y);
}

static void pt_double(const pt_t *P, pt_t *R) {
    if (P->inf || vli_isZero(P->y)) { R->inf = 1; return; }
    word_t inv2y[NWORDS], twoy[NWORDS], x2[NWORDS], num[NWORDS];
    word_t slope[NWORDS], s2[NWORDS], x3[NWORDS], t[NWORDS], y3[NWORDS];

    vli_add(twoy, P->y, P->y);
    if (vli_cmp(PP, twoy) != 1) vli_sub(twoy, twoy, PP);
    vli_modInv(inv2y, twoy);

    vli_modMult(x2, P->x, P->x);
    modAdd(num, x2, x2); modAdd(num, num, x2);          /* 3*x^2 */
    { word_t three[NWORDS] = {3,0,0,0,0,0,0,0}; modSub(num, num, three); } /* 3*x^2 - 3 (a=-3) */
    vli_modMult(slope, num, inv2y);

    vli_modMult(s2, slope, slope);
    modSub(x3, s2, P->x); modSub(x3, x3, P->x);         /* s^2 - 2x */

    modSub(t, P->x, x3);                                /* x - x3 */
    vli_modMult(y3, slope, t);
    modSub(y3, y3, P->y);

    R->inf = 0; vli_set(R->x, x3); vli_set(R->y, y3);
}

static void pt_add(const pt_t *P, const pt_t *Q, pt_t *R) {
    if (P->inf) { pt_set(R, Q); return; }
    if (Q->inf) { pt_set(R, P); return; }
    if (vli_cmp(P->x, Q->x) == 0) {
        if (vli_cmp(P->y, Q->y) != 0) { R->inf = 1; return; }
        pt_double(P, R); return;
    }
    word_t dx[NWORDS], dy[NWORDS], invdx[NWORDS], slope[NWORDS];
    word_t s2[NWORDS], x3[NWORDS], t[NWORDS], y3[NWORDS];

    modSub(dx, Q->x, P->x);
    modSub(dy, Q->y, P->y);
    vli_modInv(invdx, dx);
    vli_modMult(slope, dy, invdx);

    vli_modMult(s2, slope, slope);
    modSub(x3, s2, P->x); modSub(x3, x3, Q->x);

    modSub(t, P->x, x3);
    vli_modMult(y3, slope, t);
    modSub(y3, y3, P->y);

    R->inf = 0; vli_set(R->x, x3); vli_set(R->y, y3);
}

static void pt_mul(const word_t *scalar, const pt_t *Q, pt_t *R) {
    pt_t base; pt_set(&base, Q);
    pt_t res; res.inf = 1;
    for (int i = 255; i >= 0; --i) {
        pt_t doubled; pt_double(&res, &doubled); pt_set(&res, &doubled);
        if (vli_testBit(scalar, i)) {
            pt_t summed; pt_add(&res, &base, &summed); pt_set(&res, &summed);
        }
    }
    pt_set(R, &res);
}

/* ------------------------------- public API ------------------------- */

int p256_pubgen(const uint8_t priv[32], uint8_t pub[65]) {
    curve_init();
    word_t scalar[NWORDS];
    pt_t G, R;
    bytes_to_native(scalar, priv);
    G.inf = 0; vli_set(G.x, GX); vli_set(G.y, GY);
    pt_mul(scalar, &G, &R);
    if (R.inf) return 0;
    pub[0] = 4;
    native_to_bytes(pub + 1, R.x);
    native_to_bytes(pub + 33, R.y);
    return 1;
}

int p256_shared(const uint8_t priv[32], const uint8_t pub[65],
                uint8_t shared[32]) {
    curve_init();
    if (pub[0] != 4) return 0;
    word_t scalar[NWORDS];
    pt_t P, R;
    uint8_t bx[32], by[32];
    bytes_to_native(scalar, priv);
    P.inf = 0;
    for (int i = 0; i < 32; i++) { bx[i] = pub[1 + i]; by[i] = pub[33 + i]; }
    bytes_to_native(P.x, bx);
    bytes_to_native(P.y, by);
    pt_mul(scalar, &P, &R);
    if (R.inf) return 0;
    native_to_bytes(shared, R.x);
    return 1;
}
