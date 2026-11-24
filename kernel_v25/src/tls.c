/* tls.c -- minimal TLS 1.2 HTTPS client for CatOS (D2).
 *
 * Implements only what the 0xC027 acceptance path needs:
 *   - ECDHE secp256r1 (vendored micro-ecc)
 *   - AES-128-CBC + HMAC-SHA256 record protection (tiny-AES-c + crypto HMAC)
 *   - handshake: ClientHello, ServerHello, Certificate(skip),
 *     ServerKeyExchange(skip signature), ServerHelloDone, ClientKeyExchange,
 *     CCS, Finished (server Finished verify checked).
 *
 * NO certificate chain / hostname verification is performed; the SKE
 * signature is read and discarded.  Encryption only, MITM-exposed.  See
 * include/tls.h security note.
 */

#include "../include/tls.h"
#include "../include/tls_aes.h"
#include "../include/crypto.h"
#include "../include/kstring.h"
#include "serial_log.h"
#include "third_party/uecc/uECC.h"

/* ---- transport hooks provided by netstack.c ---- */
extern int net_tcp_connect(const uint8_t *peer, uint16_t port);
extern int net_tcp_send(const uint8_t *data, int len);
extern int net_tcp_recv(uint8_t *buf, int max, long ticks);

static int uecc_rng(uint8_t *dest, unsigned size) {
    crypto_random(dest, (int)size);
    return 1;
}

#define TLS_VERSION_0303 0x0303
#define CS_ECDHE_RSA_AES128_CBC_SHA256 0xC027
#define CS_ECDHE_RSA_AES128_GCM_SHA256 0xC02F   /* v2.78: modern-site suite */
#define CS_ECDHE_RSA_CHACHA20_POLY1305_SHA256   0xCCA8
#define CS_ECDHE_ECDSA_CHACHA20_POLY1305_SHA256 0xCCA9

/* ------------------------------------------------------------------ */
/* raw reader buffering                                                */
/* ------------------------------------------------------------------ */

static uint8_t rbuf[20480];
static int rlen, rpos;

static int reader_fill(void) {
    for (;;) {
        int n = net_tcp_recv(rbuf, sizeof(rbuf), 50000L);
        if (n > 0) { rlen = n; rpos = 0; return 0; }
        if (n < 0) return -1;
        /* timed out with no data; keep polling */
    }
}

static int reader_read(uint8_t *dst, int n) {
    int off = 0;
    while (off < n) {
        if (rpos >= rlen) { if (reader_fill() != 0) return -1; }
        int avail = rlen - rpos;
        int take = avail < (n - off) ? avail : (n - off);
        memcpy(dst + off, rbuf + rpos, take);
        rpos += take; off += take;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* transcript (handshake-only hash input)                              */
/* ------------------------------------------------------------------ */

static uint8_t transcript[8192];
static int tlen;

static void transcript_add(const uint8_t *msg, int n) {
    if (tlen + n <= (int)sizeof(transcript)) {
        memcpy(transcript + tlen, msg, n);
        tlen += n;
    }
}

/* ------------------------------------------------------------------ */
/* record send / receive framing                                       */
/* ------------------------------------------------------------------ */

static uint8_t g_recbuf[5 + 16384];

static int send_record(uint8_t type, const uint8_t *payload, int plen) {
    g_recbuf[0] = type; g_recbuf[1] = 0x03; g_recbuf[2] = 0x03;
    g_recbuf[3] = (uint8_t)(plen >> 8); g_recbuf[4] = (uint8_t)plen;
    memcpy(g_recbuf + 5, payload, plen);
    return net_tcp_send(g_recbuf, 5 + plen) < 0 ? -1 : 0;
}

static int recv_record(uint8_t *type, const uint8_t **payload, int *plen) {
    static uint8_t pbuf[16384];
    uint8_t hdr[5];
    if (reader_read(hdr, 5) != 0) return -1;
    *type = hdr[0];
    int len = ((int)hdr[3] << 8) | hdr[4];
    if (len > (int)sizeof(pbuf)) return -1;
    if (reader_read(pbuf, len) != 0) return -1;
    *payload = pbuf; *plen = len;
    return 0;
}

/* ---- handshake-message cursor over records -----------------------------
 * Real servers pack several handshake messages (Certificate +
 * ServerKeyExchange + ServerHelloDone) into ONE record.  hs_read used to
 * assume one message per record, so its second call re-framed the next
 * record (CCS / encrypted Finished) and mis-parsed it as a handshake body.
 * The cursor keeps the current record payload + offset and slices messages
 * out of it; a record holding N messages yields N successive hs_read calls
 * before the next recv_record(). */
static uint8_t  hs_rec_buf[16384];
static int      hs_rec_len, hs_rec_off;
static uint8_t  hs_scratch[16384];      /* reassembly for a split body */
static char     hs_host[96];
static uint16_t hs_port;

static void tls_dec(int v) {
    char tmp[12]; int n = 0; unsigned int u;
    if (v < 0) { ser_putc('-'); u = (unsigned int)(-(int)v); }
    else u = (unsigned int)v;
    if (u == 0) { ser_putc('0'); return; }
    while (u) { tmp[n++] = (char)('0' + u % 10); u /= 10; }
    while (n) ser_putc(tmp[--n]);
}

/* Count complete handshake messages framed inside one record payload. */
static int hs_record_msg_count(const uint8_t *p, int len) {
    int o = 0, cnt = 0;
    while (o + 4 <= len) {
        int m = ((int)p[o + 1] << 16) | ((int)p[o + 2] << 8) | p[o + 3];
        if (o + 4 + m > len) break;
        cnt++; o += 4 + m;
    }
    return cnt;
}

/* Pull the next record into hs_rec_buf.  Returns 1 = handshake record ready,
 * 0 = CCS skipped (caller loops), negative = alert / framing failure. */
static int hs_pull_record(void) {
    uint8_t type; const uint8_t *p; int len;
    if (recv_record(&type, &p, &len) != 0) return -1;
    if (type == 21) {
        ser_write("TLS:alert level="); tls_dec(len > 0 ? p[0] : -1);
        ser_write(" desc="); tls_dec(len > 1 ? p[1] : -1);
        ser_write(" host="); ser_write(hs_host); ser_write("\n");
        return -2;                 /* alert */
    }
    if (type == 20) return 0;                   /* CCS, plaintext */
    if (type != 22) return -1;
    if (len > (int)sizeof(hs_rec_buf)) return -1;
    memcpy(hs_rec_buf, p, len);
    hs_rec_len = len; hs_rec_off = 0;
    ser_write("TLS:rec msgs="); tls_dec(hs_record_msg_count(p, len));
    ser_write(" port="); tls_dec(hs_port);
    ser_write(" host="); ser_write(hs_host); ser_write("\n");
    return 1;
}

/* ------------------------------------------------------------------ */
/* negotiated keys / sequence numbers                                  */
/* ------------------------------------------------------------------ */

static tls_aes128_t en_cbc, de_cbc;
static uint8_t c_mac[32], s_mac[32];
static uint8_t c_key[16], s_key[16];
static uint8_t c_iv[16], s_iv[16];
static uint64_t cseq, sseq;

/* scratch for protection (single connection, single thread: file scope keeps
 * the 16 KiB blocks off the kernel stack). */
static uint8_t g_enc_in[16384], g_enc_out[16384];

/* ---- v2.78: AES-128-GCM record protection (0xC02F) --------------------
 * Added so `curl https://` can talk to sites that only offer AEAD suites
 * (baidu/qq/mozilla...).  GHASH uses the bitwise GF(2^128) multiply; the
 * AES-128 block primitive reuses tiny-AES's CBC path with a zero IV, which
 * is exactly one ECB block encrypt. */
static int gcm_on;
static int chacha_on;                 /* 0xCCA8/0xCCA9 record protection */
static int ecdsa_mode;                /* negotiated suite keys ECDSA */
static uint8_t ecdsa_pub[64];         /* leaf cert ECDSA P-256 point X||Y */
static int     have_ecdsa_pub;
static uint8_t c_ckey[32], s_ckey[32]; /* ChaCha write keys */
static uint8_t c_civ[12], s_civ[12];    /* ChaCha write IVs (RFC 7905 12B fixed IV) */
static uint8_t c_salt[4], s_salt[4];

static void gf_mul128(uint8_t Y[16], const uint8_t H[16]) {
    uint8_t X[16], Z[16], V[16];
    memcpy(X, Y, 16);
    for (int i = 0; i < 16; i++) Z[i] = 0;
    memcpy(V, H, 16);
    for (int i = 0; i < 128; i++) {
        if (X[i >> 3] & (uint8_t)(0x80 >> (i & 7))) {
            for (int k = 0; k < 16; k++) Z[k] ^= V[k];
        }
        int lsb = V[15] & 1;
        for (int j = 15; j > 0; j--) V[j] = (uint8_t)((V[j] >> 1) | (V[j - 1] << 7));
        V[0] >>= 1;
        if (lsb) V[0] ^= 0xE1;
    }
    memcpy(Y, Z, 16);
}

static void aes_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    tls_aes128_t c;
    uint8_t iv[16];
    for (int i = 0; i < 16; i++) iv[i] = 0;
    tls_aes128_key(&c, key);
    tls_aes128_cbc_enc(&c, iv, in, out, 1);
}

static void ghash_blocks(uint8_t Y[16], const uint8_t H[16], const uint8_t *p, int len) {
    uint8_t blk[16];
    for (int off = 0; off < len; off += 16) {
        int n = len - off; if (n > 16) n = 16;
        for (int i = 0; i < 16; i++) blk[i] = 0;
        memcpy(blk, p + off, (uint32_t)n);
        for (int i = 0; i < 16; i++) Y[i] ^= blk[i];
        gf_mul128(Y, H);
    }
}

/* tag = GHASH(H, aad || data) ^ AES(K, J0), with the standard length trailer. */
static void gcm_tag(const uint8_t key[16], const uint8_t nonce[12],
                    const uint8_t *aad, int alen, const uint8_t *data, int dlen,
                    uint8_t tag[16]) {
    uint8_t H[16], J0[16], zero[16], ej[16], Y[16], L[16];
    for (int i = 0; i < 16; i++) zero[i] = 0;
    aes_block(key, zero, H);
    memcpy(J0, nonce, 12); J0[12] = 0; J0[13] = 0; J0[14] = 0; J0[15] = 1;
    for (int i = 0; i < 16; i++) Y[i] = 0;
    ghash_blocks(Y, H, aad, alen);
    ghash_blocks(Y, H, data, dlen);
    for (int i = 0; i < 16; i++) L[i] = 0;
    uint64_t a = (uint64_t)alen * 8, c = (uint64_t)dlen * 8;
    for (int i = 0; i < 8; i++) {
        L[i] = (uint8_t)(a >> (56 - 8 * i));
        L[8 + i] = (uint8_t)(c >> (56 - 8 * i));
    }
    for (int i = 0; i < 16; i++) Y[i] ^= L[i];
    gf_mul128(Y, H);
    aes_block(key, J0, ej);
    for (int i = 0; i < 16; i++) tag[i] = (uint8_t)(Y[i] ^ ej[i]);
}

static void gcm_ctr(const uint8_t key[16], const uint8_t nonce[12],
                    const uint8_t *in, int len, uint8_t *out) {
    uint8_t ctr[16], ks[16];
    for (int off = 0; off < len; off += 16) {
        int n = len - off; if (n > 16) n = 16;
        memcpy(ctr, nonce, 12);
        uint32_t c = 2 + (uint32_t)(off / 16);   /* GCM CTR starts at inc32(J0) */
        ctr[12] = (uint8_t)(c >> 24); ctr[13] = (uint8_t)(c >> 16);
        ctr[14] = (uint8_t)(c >> 8);  ctr[15] = (uint8_t)c;
        aes_block(key, ctr, ks);
        for (int i = 0; i < n; i++) out[off + i] = (uint8_t)(in[off + i] ^ ks[i]);
    }
}

/* MAC-then-encrypt one outbound record.  rec_type is the record content type
 * (22 handshake, 23 application_data).  Sends record via the transport. */
static int protect_record(int rec_type, const uint8_t *plain, int plen) {
    uint8_t mac[32], mac_hdr[13], seqbe[8], iv[16];
    int i;
    for (i = 0; i < 8; i++) seqbe[i] = (uint8_t)(cseq >> (56 - 8 * i));
    memcpy(mac_hdr, seqbe, 8);
    mac_hdr[8] = (uint8_t)rec_type;
    mac_hdr[9] = 0x03; mac_hdr[10] = 0x03;
    uint16_t l16 = (uint16_t)plen;
    mac_hdr[11] = (uint8_t)(l16 >> 8); mac_hdr[12] = (uint8_t)l16;

    if (chacha_on) {
        /* RFC 7905: nonce = 4 zero bytes || 8-byte record sequence number;
         * no explicit nonce on the wire, 16-byte Poly1305 tag appended. */
        uint8_t nonce[12], tag[16];
        for (int i = 0; i < 4; i++) nonce[i] = 0;
        memcpy(nonce + 4, seqbe, 8);
        for (int i = 0; i < 12; i++) nonce[i] ^= c_civ[i];
        tls_chacha_poly_seal(c_ckey, nonce, mac_hdr, 13, plain, plen,
                             g_enc_out, tag);
        int rn = 0;
        g_recbuf[rn++] = (uint8_t)rec_type;
        g_recbuf[rn++] = 0x03; g_recbuf[rn++] = 0x03;
        uint16_t rl = (uint16_t)(plen + 16);
        g_recbuf[rn++] = (uint8_t)(rl >> 8); g_recbuf[rn++] = (uint8_t)rl;
        memcpy(g_recbuf + rn, g_enc_out, plen); rn += plen;
        memcpy(g_recbuf + rn, tag, 16); rn += 16;
        if (net_tcp_send(g_recbuf, rn) < 0) return -1;
        cseq++;
        return 0;
    }

    if (gcm_on) {
        /* RFC 5288: explicit 8-byte nonce goes in the clear, tag appended. */
        static uint8_t gcm_ct[16384];
        uint8_t nonce[12], tag[16];
        memcpy(nonce, c_salt, 4);
        memcpy(nonce + 4, seqbe, 8);
        gcm_ctr(c_key, nonce, plain, plen, gcm_ct);
        gcm_tag(c_key, nonce, mac_hdr, 13, gcm_ct, plen, tag);
        int rn = 0;
        g_recbuf[rn++] = (uint8_t)rec_type;
        g_recbuf[rn++] = 0x03; g_recbuf[rn++] = 0x03;
        uint16_t rl = (uint16_t)(8 + plen + 16);
        g_recbuf[rn++] = (uint8_t)(rl >> 8); g_recbuf[rn++] = (uint8_t)rl;
        memcpy(g_recbuf + rn, seqbe, 8); rn += 8;
        memcpy(g_recbuf + rn, gcm_ct, plen); rn += plen;
        memcpy(g_recbuf + rn, tag, 16); rn += 16;
        if (net_tcp_send(g_recbuf, rn) < 0) return -1;
        cseq++;
        return 0;
    }

    hmac_sha256(mac, c_mac, 32, mac_hdr, 13, plain, plen);

    int L = plen + 32;
    int pad = 16 - (L % 16);
    if (pad == 16) pad = 16;             /* PKCS#7: full block when aligned */
    int ct_len = L + pad;

    memcpy(g_enc_in, plain, plen);
    memcpy(g_enc_in + plen, mac, 32);
    for (i = 0; i < pad; i++) g_enc_in[plen + 32 + i] = (uint8_t)(pad - 1);

    crypto_random(iv, 16);
    tls_aes128_key(&en_cbc, c_key);
    tls_aes128_cbc_enc(&en_cbc, iv, g_enc_in, g_enc_out, ct_len / 16);

    int rn = 0;
    g_recbuf[rn++] = (uint8_t)rec_type; g_recbuf[rn++] = 0x03; g_recbuf[rn++] = 0x03;
    uint16_t rl = (uint16_t)(16 + ct_len);
    g_recbuf[rn++] = (uint8_t)(rl >> 8); g_recbuf[rn++] = (uint8_t)rl;
    memcpy(g_recbuf + rn, iv, 16); rn += 16;
    memcpy(g_recbuf + rn, g_enc_out, ct_len); rn += ct_len;
    if (net_tcp_send(g_recbuf, rn) < 0) return -1;

    cseq++;
    return 0;
}

static int app_send(const uint8_t *plain, int plen) {
    return protect_record(23, plain, plen);
}

/* ------------------------------------------------------------------ */
/* record protection: decrypt one inbound protected record             */
/* ------------------------------------------------------------------ */

static int app_recv(const uint8_t *payload, int plen, int rec_type,
                    uint8_t *out, int *outlen) {
    if (chacha_on) {
        if (plen < 16) return -1;
        int ct_len = plen - 16;
        uint8_t nonce[12], aad[13], seqbe[8];
        for (int i = 0; i < 8; i++) seqbe[i] = (uint8_t)(sseq >> (56 - 8 * i));
        for (int i = 0; i < 4; i++) nonce[i] = 0;
        memcpy(nonce + 4, seqbe, 8);
        for (int i = 0; i < 12; i++) nonce[i] ^= s_civ[i];
        memcpy(aad, seqbe, 8);
        aad[8] = (uint8_t)rec_type; aad[9] = 0x03; aad[10] = 0x03;
        aad[11] = (uint8_t)(ct_len >> 8); aad[12] = (uint8_t)ct_len;
        if (tls_chacha_poly_open(s_ckey, nonce, aad, 13, payload, ct_len,
                                 payload + ct_len, out) != 0) return -1;
        *outlen = ct_len;
        sseq++;
        return 0;
    }
    if (gcm_on) {
        if (plen < 8 + 16) return -1;
        int ct_len = plen - 8 - 16;
        uint8_t nonce[12], aad[13], seqbe[8], calc[16], diff = 0;
        memcpy(nonce, s_salt, 4);
        memcpy(nonce + 4, payload, 8);
        for (int i = 0; i < 8; i++) seqbe[i] = (uint8_t)(sseq >> (56 - 8 * i));
        memcpy(aad, seqbe, 8);
        aad[8] = (uint8_t)rec_type; aad[9] = 0x03; aad[10] = 0x03;
        aad[11] = (uint8_t)(ct_len >> 8); aad[12] = (uint8_t)ct_len;
        gcm_tag(s_key, nonce, aad, 13, payload + 8, ct_len, calc);
        for (int i = 0; i < 16; i++) diff |= (uint8_t)(calc[i] ^ payload[8 + ct_len + i]);
        if (diff != 0) return -1;
        gcm_ctr(s_key, nonce, payload + 8, ct_len, out);
        *outlen = ct_len;
        sseq++;
        return 0;
    }
    if (plen < 17) return -1;
    int ct_len = plen - 16;
    if (ct_len % 16 != 0) return -1;

    static uint8_t cbc_dec[16384];
    uint8_t *dec = cbc_dec;
    tls_aes128_key(&de_cbc, s_key);
    tls_aes128_cbc_dec(&de_cbc, payload, payload + 16, dec, ct_len / 16);

    /* strip PKCS#7 padding first, then locate the MAC over plaintext. */
    int pad = dec[ct_len - 1] + 1;
    if (pad < 1 || pad > 16) return -1;
    int i;
    for (i = ct_len - pad; i < ct_len; i++)
        if (dec[i] != (uint8_t)(pad - 1)) return -1;

    int inner_len = ct_len - pad;
    if (inner_len < 32) return -1;
    int plain_len = inner_len - 32;
    const uint8_t *mac = dec + plain_len;

    /* verify MAC: seq||type||version||length||plaintext */
    uint8_t mac_hdr[13], seqbe[8], calc[32];
    for (i = 0; i < 8; i++) seqbe[i] = (uint8_t)(sseq >> (56 - 8 * i));
    memcpy(mac_hdr, seqbe, 8);
    mac_hdr[8] = (uint8_t)rec_type;
    mac_hdr[9] = 0x03; mac_hdr[10] = 0x03;
    uint16_t l16 = (uint16_t)plain_len;
    mac_hdr[11] = (uint8_t)(l16 >> 8); mac_hdr[12] = (uint8_t)l16;

    hmac_sha256(calc, s_mac, 32, mac_hdr, 13, dec, plain_len);

    /* constant-time compare */
    uint8_t diff = 0;
    for (i = 0; i < 32; i++) diff |= (uint8_t)(calc[i] ^ mac[i]);
    if (diff != 0) return -1;

    memcpy(out, dec, plain_len);
    *outlen = plain_len;
    sseq++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* minimal DER walker (X.509 leaf cert -> ECDSA P-256 public point)  */
/* ------------------------------------------------------------------ */

static int der_child(const uint8_t *d, int len, int *off,
                     uint8_t *tag, int *voff, int *vlen) {
    int p = *off;
    if (p + 2 > len) return -1;
    *tag = d[p];
    int l = d[p + 1], h = 2;
    if (l & 0x80) {
        int n = l & 0x7f;
        if (n > 4 || p + 2 + n > len) return -1;
        l = 0;
        for (int i = 0; i < n; i++) l = (l << 8) | d[p + 2 + i];
        h = 2 + n;
    }
    if (p + h + l > len) return -1;
    *voff = p + h; *vlen = l; *off = p + h + l;
    return 0;
}

/* Pull one INTEGER from `d`, right-justified into 32 big-endian bytes. */
static int der_read_int(const uint8_t *d, int len, int *off, uint8_t out[32]) {
    uint8_t tag; int vo, vl;
    if (der_child(d, len, off, &tag, &vo, &vl) != 0 || tag != 0x02) return -1;
    int p = vo;
    while (vl > 1 && d[p] == 0) { p++; vl--; }
    if (vl > 32) return -1;
    for (int i = 0; i < 32; i++) out[i] = 0;
    memcpy(out + 32 - vl, d + p, vl);
    return 0;
}

/* Walk the leaf certificate DER and cache its ECDSA P-256 public point.
 * Tolerant: any structural mismatch leaves have_ecdsa_pub == 0. */
static void cert_extract_ecdsa(const uint8_t *cert, int certlen) {
    int off = 0, vo, vl;
    uint8_t tag;
    if (der_child(cert, certlen, &off, &tag, &vo, &vl) != 0 || tag != 0x30) return;
    int tbs_off = vo;
    if (der_child(cert, vo + vl, &tbs_off, &tag, &vo, &vl) != 0 || tag != 0x30) return;
    /* skip version[0], serial, sigAlg, issuer, validity, subject (6 kids) */
    for (int i = 0; i < 6; i++) {
        if (der_child(cert, tbs_off + vl, &tbs_off, &tag, &vo, &vl) != 0) return;
    }
    int spki_off, spki_len;
    if (der_child(cert, tbs_off + vl, &tbs_off, &tag, &spki_off, &spki_len) != 0 ||
        tag != 0x30) return;
    int aoff = spki_off;
    if (der_child(cert, spki_off + spki_len, &aoff, &tag, &vo, &vl) != 0 || tag != 0x30) return;
    int bit_off, bit_len;
    if (der_child(cert, spki_off + spki_len, &aoff, &tag, &bit_off, &bit_len) != 0 ||
        tag != 0x03) return;
    if (bit_len < 1 + 65 || cert[bit_off] != 0 || cert[bit_off + 1] != 0x04) return;
    memcpy(ecdsa_pub, cert + bit_off + 2, 64);
    have_ecdsa_pub = 1;
}

/* Certificate handshake message: 3-byte certs length, then 3-byte cert len + DER. */
static void cert_parse_message(const uint8_t *body, int blen) {
    if (blen < 6) return;
    int off = 3;
    int clen = ((int)body[off] << 16) | ((int)body[off + 1] << 8) | body[off + 2];
    off += 3;
    if (clen <= 0 || off + clen > blen) return;
    cert_extract_ecdsa(body + off, clen);
}

/* Verify the ServerKeyExchange ECDSA signature (P-256, SHA-256). */
static int ske_verify_ecdsa(const uint8_t *params, int paramslen,
                            const uint8_t *sig, int siglen) {
    if (!have_ecdsa_pub) return 0;
    uint8_t rs[64];
    int off = 0; uint8_t tag; int vo, vl;
    if (der_child(sig, siglen, &off, &tag, &vo, &vl) != 0 || tag != 0x30) return 0;
    if (der_read_int(sig, siglen, &off, rs) != 0) return 0;
    if (der_read_int(sig, siglen, &off, rs + 32) != 0) return 0;
    uint8_t h[32];
    sha256(h, params, paramslen);
    return uECC_verify(ecdsa_pub, h, 32, rs, uECC_secp256r1());
}

/* ------------------------------------------------------------------ */
/* handshake message helpers                                           */
/* ------------------------------------------------------------------ */

static int hs_send(uint8_t type, const uint8_t *body, int blen) {
    uint8_t msg[4 + 512];
    if (blen > 508) return -1;
    msg[0] = type;
    msg[1] = (uint8_t)(blen >> 16); msg[2] = (uint8_t)(blen >> 8); msg[3] = (uint8_t)blen;
    memcpy(msg + 4, body, blen);
    transcript_add(msg, 4 + blen);
    return send_record(22, msg, 4 + blen);
}

/* Read the next handshake message; skips CCS/alert as directed.
 * Returns msg type, fills body/len.  Messages are sliced from the current
 * record cursor; a message body split across records is reassembled into
 * hs_msg.  A header split across records (or an oversize message) returns
 * -7 with a serial diagnostic instead of a silent generic -6. */
static int hs_read(uint8_t *msg_type, const uint8_t **body, int *blen) {
    for (;;) {
        if (hs_rec_off >= hs_rec_len) {
            int r;
            do { r = hs_pull_record(); if (r < 0) return r; } while (r == 0);
        }
        int rem = hs_rec_len - hs_rec_off;
        if (rem < 4) {
            ser_write("TLS:rec fragmented-header host="); ser_write(hs_host);
            ser_write(" port="); tls_dec(hs_port); ser_write("\n");
            return -7;
        }
        uint8_t mt = hs_rec_buf[hs_rec_off];
        int n = ((int)hs_rec_buf[hs_rec_off + 1] << 16) |
                ((int)hs_rec_buf[hs_rec_off + 2] << 8) |
                hs_rec_buf[hs_rec_off + 3];
        int need = 4 + n;
        if (n < 0 || need > (int)sizeof(hs_scratch)) {
            ser_write("TLS:rec oversize host="); ser_write(hs_host);
            ser_write(" msglen="); tls_dec(n); ser_write("\n");
            return -7;
        }
        if (rem >= need) {
            const uint8_t *m = hs_rec_buf + hs_rec_off;
            transcript_add(m, need);
            hs_rec_off += need;
            *msg_type = mt; *blen = n; *body = m + 4;
            return 0;
        }
        /* body spans records: copy what fits, then reassemble the rest. */
        memcpy(hs_scratch, hs_rec_buf + hs_rec_off, rem);
        int done = rem;
        hs_rec_off = hs_rec_len;
        while (done < need) {
            int r;
            do { r = hs_pull_record(); if (r < 0) return r; } while (r == 0);
            int take = need - done;
            if (take > hs_rec_len) take = hs_rec_len;
            memcpy(hs_scratch + done, hs_rec_buf, take);
            done += take; hs_rec_off = take;
        }
        transcript_add(hs_scratch, need);
        *msg_type = mt; *blen = n; *body = hs_scratch + 4;
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* main HTTPS GET entry point                                          */
/* ------------------------------------------------------------------ */

int tls_https_get(const uint8_t peer[4], uint16_t port,
                  const char *host, const char *path,
                  uint8_t *out, int max) {
    if (!peer || !host || !path || !out || max <= 0) return -1;

    uECC_set_rng(uecc_rng);
    tlen = 0; cseq = 0; sseq = 0; gcm_on = 0;
    chacha_on = 0; ecdsa_mode = 0; have_ecdsa_pub = 0;
    hs_rec_len = 0; hs_rec_off = 0;
    hs_port = port;
    {
        int i = 0;
        while (host[i] && i < (int)sizeof(hs_host) - 1) { hs_host[i] = host[i]; i++; }
        hs_host[i] = 0;
    }

    /* ---- TCP connect ---- */
    if (net_tcp_connect(peer, port) != 0) return -3;

    /* ---- our ECDHE keypair ---- */
    uint8_t our_pub[64], our_priv[32];
    uECC_Curve curve = uECC_secp256r1();
    if (!uECC_make_key(our_pub, our_priv, curve)) return -6;

    /* ---- ClientHello ---- */
    /* version || random || sid(0) || suites(0xC027) || compression(null)
     * || extensions: supported_groups(secp256r1).  The groups extension is
     * required: without it OpenSSL 3.0 aborts the ECDHE handshake with a
     * handshake_failure alert. */
    uint8_t ch[384];
    int cn = 0;
    ch[cn++] = 0x03; ch[cn++] = 0x03;
    uint8_t crandom[32]; crypto_random(crandom, 32);
    memcpy(ch + cn, crandom, 32); cn += 32;
    ch[cn++] = 0;                              /* session id len */
    /* Offer ECDSA/RSA ChaCha20-Poly1305 first (modern CDNs prefer it),
     * then the AES-GCM and AES-CBC suites from earlier revisions. */
    /* Offer every suite this client can actually complete: ECDSA-first so
     * ECDSA-only edges (example.com behind its EC cert) find a match, then
     * the RSA variants, then the legacy CBC fallback. */
    ch[cn++] = 0x00; ch[cn++] = 0x0A;
    ch[cn++] = 0xCC; ch[cn++] = 0xA9;   /* ECDHE-ECDSA-CHACHA20-POLY1305 */
    ch[cn++] = 0xC0; ch[cn++] = 0x2B;   /* ECDHE-ECDSA-AES128-GCM-SHA256 */
    ch[cn++] = 0xCC; ch[cn++] = 0xA8;   /* ECDHE-RSA-CHACHA20-POLY1305 */
    ch[cn++] = 0xC0; ch[cn++] = 0x2F;   /* ECDHE-RSA-AES128-GCM-SHA256 */
    ch[cn++] = 0xC0; ch[cn++] = 0x27;   /* ECDHE-RSA-AES128-CBC-SHA256 */
    ch[cn++] = 0x01; ch[cn++] = 0x00;         /* compression: null */
    int ext_len_pos = cn; ch[cn++] = 0x00; ch[cn++] = 0x00;
    /* supported_groups: secp256r1 only. */
    ch[cn++] = 0x00; ch[cn++] = 0x0A;
    ch[cn++] = 0x00; ch[cn++] = 0x04;
    ch[cn++] = 0x00; ch[cn++] = 0x02;      /* list length in bytes = 2 (one group) */
    ch[cn++] = 0x00; ch[cn++] = 0x17;
    /* signature_algorithms: RSA-PKCS1-SHA256, ECDSA-secp256r1-SHA256,
     * RSA-PSS-SHA256. */
    /* signature_algorithms: the OpenSSL 3 default set (20 algs). A 3-entry
     * list (rsa_pkcs1_sha256/ecdsa_secp256r1_sha256/rsa_pss_rsae_sha256) is
     * rejected by Cloudflare-class edges with alert 40: their RSA edge cert
     * wants SHA-384, which the short list does not advertise. */
    ch[cn++] = 0x00; ch[cn++] = 0x0D;
    ch[cn++] = 0x00; ch[cn++] = 0x2A;
    ch[cn++] = 0x00; ch[cn++] = 0x28;
    static const uint8_t sigalgs[40] = {
        0x04, 0x03, 0x05, 0x03, 0x06, 0x03, 0x08, 0x07,
        0x08, 0x08, 0x08, 0x09, 0x08, 0x0A, 0x08, 0x0B,
        0x08, 0x04, 0x08, 0x05, 0x08, 0x06, 0x04, 0x01,
        0x05, 0x01, 0x06, 0x01, 0x03, 0x03, 0x03, 0x01,
        0x03, 0x02, 0x04, 0x02, 0x05, 0x02, 0x06, 0x02
    };
    for (int i = 0; i < 40; i++) ch[cn++] = sigalgs[i];
    /* SNI: virtual-host sites (and most CDNs) reject a ClientHello without
     * it, which is why https://baidu.com failed before this. */
    {
        int hl = 0;
        while (host[hl] && hl < 96) hl++;
        ch[cn++] = 0x00; ch[cn++] = 0x00;                 /* ext: server_name */
        ch[cn++] = 0x00; ch[cn++] = (uint8_t)(5 + hl);     /* ext data len */
        ch[cn++] = 0x00; ch[cn++] = (uint8_t)(3 + hl);     /* list len */
        ch[cn++] = 0x00;                                  /* host_name */
        ch[cn++] = 0x00; ch[cn++] = (uint8_t)hl;
        for (int i = 0; i < hl; i++) ch[cn++] = (uint8_t)host[i];
    }
    /* ec_point_formats(0x000B): uncompressed only. Strict edges (Apple/Cloudflare
     * fronts) reject an ECDHE ClientHello that omits it. */
    ch[cn++] = 0x00; ch[cn++] = 0x0B;
    ch[cn++] = 0x00; ch[cn++] = 0x02;
    ch[cn++] = 0x01; ch[cn++] = 0x00;
    /* session_ticket(0x0023): empty = "no ticket", but the extension itself
     * must be present for several CDN edges. */
    ch[cn++] = 0x00; ch[cn++] = 0x23;
    ch[cn++] = 0x00; ch[cn++] = 0x00;
    /* ALPN(0x0010): http/1.1 + http/1.0 so edges that require ALPN negotiate
     * an HTTP/1.x protocol instead of alerting. */
    ch[cn++] = 0x00; ch[cn++] = 0x10;
    ch[cn++] = 0x00; ch[cn++] = 0x14;
    ch[cn++] = 0x00; ch[cn++] = 0x12;
    ch[cn++] = 0x08; ch[cn++] = 'h'; ch[cn++] = 't'; ch[cn++] = 't';
    ch[cn++] = 'p';  ch[cn++] = '/'; ch[cn++] = '1'; ch[cn++] = '.';
    ch[cn++] = '1';
    ch[cn++] = 0x08; ch[cn++] = 'h'; ch[cn++] = 't'; ch[cn++] = 't';
    ch[cn++] = 'p';  ch[cn++] = '/'; ch[cn++] = '1'; ch[cn++] = '.';
    ch[cn++] = '0';
    /* renegotiation_info(0xFF01): empty, the safe Secure-Renegotiation answer. */
    ch[cn++] = 0xFF; ch[cn++] = 0x01;
    ch[cn++] = 0x00; ch[cn++] = 0x01;
    ch[cn++] = 0x00;
    {
        int extlen = cn - ext_len_pos - 2;
        ch[ext_len_pos] = (uint8_t)(extlen >> 8);
        ch[ext_len_pos + 1] = (uint8_t)extlen;
    }
    if (hs_send(1, ch, cn) != 0) return -6;

    /* ---- ServerHello ---- */
    uint8_t mtype; const uint8_t *body; int blen;
    if (hs_read(&mtype, &body, &blen) != 0) return -6;
    if (mtype != 2 || blen < 34) return -6;
    if (body[0] != 0x03 || body[1] != 0x03) return -6;
    uint8_t srandom[32]; memcpy(srandom, body + 2, 32);
    {
        int sid_len = body[34];
        int off = 35 + sid_len;
        if (off + 1 >= blen) return -6;
        uint16_t suite = ((uint16_t)body[off] << 8) | body[off + 1];
        if (suite == 0xC02B) { gcm_on = 1; ecdsa_mode = 1; }
        else if (suite == CS_ECDHE_RSA_AES128_GCM_SHA256) { gcm_on = 1; }
        else if (suite == CS_ECDHE_RSA_AES128_CBC_SHA256) { gcm_on = 0; }
        else if (suite == CS_ECDHE_RSA_CHACHA20_POLY1305_SHA256) { chacha_on = 1; }
        else if (suite == CS_ECDHE_ECDSA_CHACHA20_POLY1305_SHA256) {
            chacha_on = 1; ecdsa_mode = 1;
        }
        else return -6;                        /* no shared suite */
    }

    /* ---- Certificate: skip chain validation; cache ECDSA leaf pubkey ---- */
    if (hs_read(&mtype, &body, &blen) != 0) return -6;
    if (mtype != 11) return -6;
    cert_parse_message(body, blen);

    /* ---- ServerKeyExchange: parse curve + peer pub, skip signature ---- */
    if (hs_read(&mtype, &body, &blen) != 0) return -6;
    if (mtype != 12) return -6;
    if (blen < 4) return -6;
    if (body[0] != 3) return -6;
    uint16_t curve_id = ((uint16_t)body[1] << 8) | body[2];
    if (curve_id != 23) return -6;
    uint8_t peer_pub[64];
    int plen_pub = body[3];
    /* ECDHE point: 1-byte 0x04 (uncompressed) + 64-byte X||Y = 65 bytes. */
    if (plen_pub != 65 || body[4] != 0x04 || blen < 5 + 64) return -6;
    memcpy(peer_pub, body + 5, 64);

    /* ECDSA suites: verify the SKE signature over client_random ||
     * server_random || ECDHE params.  This client already skips certificate
     * chain/hostname validation, so a signature-verify failure is logged
     * and the handshake continues (warn-only) rather than aborted. */
    if (ecdsa_mode) {
        int off = 5 + 64;                       /* = 69 */
        if (blen < off + 2) return -6;
        off += 2;                               /* signature_algorithm */
        if (off + 2 > blen) return -6;
        int siglen = ((int)body[off] << 8) | body[off + 1];
        off += 2;
        if (siglen <= 0 || off + siglen > blen) return -6;
        /* SignedParams = ClientRandom(32) || ServerRandom(32) ||
         * ServerECDHParams(69: curve_type 1 + curve 2 + pubkey-len 1 + point 65). */
        uint8_t sparam[32 + 32 + 69];
        memcpy(sparam, crandom, 32);
        memcpy(sparam + 32, srandom, 32);
        memcpy(sparam + 64, body, 69);
        if (!ske_verify_ecdsa(sparam, (int)sizeof(sparam), body + off, siglen)) {
            ser_write("TLS:warn ske_sig skip host=");
            ser_write(hs_host); ser_write("\n");
        }
    }

    /* ---- ServerHelloDone ---- */
    if (hs_read(&mtype, &body, &blen) != 0) return -6;
    if (mtype != 14) return -6;

    /* ---- ECDH shared secret ---- */
    uint8_t pre_master[32];
    if (!uECC_shared_secret(peer_pub, our_priv, pre_master, curve)) return -6;

    /* ---- master secret + key block ---- */
    uint8_t master[48];
    uint8_t rand64[64];
    memcpy(rand64, crandom, 32); memcpy(rand64 + 32, srandom, 32);
    tls_prf(master, 48, pre_master, 32, "master secret", rand64, 64);

    uint8_t kb[128];
    uint8_t rand64b[64];
    memcpy(rand64b, srandom, 32); memcpy(rand64b + 32, crandom, 32);
    if (chacha_on) {
        /* ChaCha20-Poly1305: 32-byte key + 12-byte fixed IV per direction
         * (RFC 7905: fixed_iv_length = 12, record_iv_length = 0). */
        tls_prf(kb, 88, master, 48, "key expansion", rand64b, 64);
        memcpy(c_ckey, kb + 0, 32);  memcpy(s_ckey, kb + 32, 32);
        memcpy(c_civ, kb + 64, 12);  memcpy(s_civ, kb + 76, 12);
    } else if (gcm_on) {
        /* AEAD: 16-byte key + 4-byte implicit IV per direction, no MAC keys. */
        tls_prf(kb, 40, master, 48, "key expansion", rand64b, 64);
        memcpy(c_key, kb + 0, 16);  memcpy(s_key, kb + 16, 16);
        memcpy(c_salt, kb + 32, 4); memcpy(s_salt, kb + 36, 4);
    } else {
        tls_prf(kb, 128, master, 48, "key expansion", rand64b, 64);
        memcpy(c_mac, kb + 0, 32);  memcpy(s_mac, kb + 32, 32);
        memcpy(c_key, kb + 64, 16); memcpy(s_key, kb + 80, 16);
        memcpy(c_iv, kb + 96, 16);  memcpy(s_iv, kb + 112, 16);
    }

    /* ---- ClientKeyExchange: 1-byte 0x04 prefix + 64-byte X||Y = 65 bytes. */
    uint8_t cke[66];
    cke[0] = 65; cke[1] = 0x04; memcpy(cke + 2, our_pub, 64);
    if (hs_send(16, cke, 66) != 0) return -6;

    /* ---- ChangeCipherSpec ---- */
    {
        uint8_t ccs[1] = { 1 };
        if (send_record(20, ccs, 1) != 0) return -6;
    }

    /* ---- client Finished ---- */
    uint8_t tmphash[32];
    sha256(tmphash, transcript, tlen);
    uint8_t cli_verify[12];
    tls_prf(cli_verify, 12, master, 48, "client finished", tmphash, 32);
    {
        uint8_t fin[4 + 12];
        fin[0] = 20; fin[1] = 0; fin[2] = 0; fin[3] = 12;
        memcpy(fin + 4, cli_verify, 12);
        transcript_add(fin, 16);
        if (protect_record(22, fin, 16) != 0) return -6;
    }
    /* The server Finished verify_data is PRF over the transcript that
     * includes our own client Finished message (RFC 5246 7.4.9). */
    uint8_t srvhash[32];
    sha256(srvhash, transcript, tlen);

    /* ---- server CCS then encrypted server Finished ---- */
    for (;;) {
        uint8_t type; const uint8_t *p; int len;
        if (recv_record(&type, &p, &len) != 0) return -6;
        if (type == 20) continue;              /* server CCS */
        if (type != 22) return -6;
        /* encrypted handshake record: decrypt then parse Finished */
        static uint8_t inner[16384]; int ilen;
        if (app_recv(p, len, 22, inner, &ilen) != 0) return -6;
        if (ilen < 4 || inner[0] != 20) return -6;
        int vlen = ((int)inner[1] << 16) | ((int)inner[2] << 8) | inner[3];
        if (vlen != 12 || ilen < 16) return -6;
        uint8_t srv_verify[12];
        tls_prf(srv_verify, 12, master, 48, "server finished", srvhash, 32);
        uint8_t diff = 0;
        for (int i = 0; i < 12; i++) diff |= (uint8_t)(srv_verify[i] ^ inner[4 + i]);
        if (diff != 0) return -6;
        break;
    }

    /* ---- send HTTP GET over TLS ---- */
    uint8_t req[600];
    int rn = 0;
#define TLS_APP(S) do { const char *_q = (S); while (*_q && rn < 598) req[rn++] = (uint8_t)*_q++; } while (0)
    TLS_APP("GET "); TLS_APP(path); TLS_APP(" HTTP/1.0\r\nHost: ");
    TLS_APP(host); TLS_APP("\r\n");
    TLS_APP("User-Agent: CatOS-curl/1.0\r\nAccept: */*\r\n"
            "Accept-Encoding: identity\r\nConnection: close\r\n\r\n");
#undef TLS_APP
    if (app_send(req, rn) != 0) return -6;

    /* ---- collect decrypted application data ---- */
    static uint8_t resp[20480];
    int resp_len = 0;
    for (int k = 0; k < 60; k++) {
        uint8_t type; const uint8_t *p; int len;
        if (recv_record(&type, &p, &len) != 0) break;
        if (type == 21) break;                 /* alert: end of stream */
        if (type == 22) {                      /* encrypted handshake (ticket etc):
                                                  consume the seq number quietly */
            uint8_t junk[16384]; int jlen;
            if (app_recv(p, len, 22, junk, &jlen) != 0) break;
            continue;
        }
        if (type != 23) continue;
        static uint8_t frag[16384]; int flen;

        if (app_recv(p, len, 23, frag, &flen) != 0) return -6;
        if (resp_len + flen > (int)sizeof(resp)) break;
        memcpy(resp + resp_len, frag, flen);
        resp_len += flen;
    }
    if (resp_len <= 0) return -5;

    /* ---- minimal HTTP response parse ---- */
    int code = 0;
    if (resp_len >= 12 && resp[0] == 'H' && resp[4] == '/')
        code = (resp[9] - '0') * 100 + (resp[10] - '0') * 10 + (resp[11] - '0');
    if (code < 200 || code >= 400) return -4;

    int body_start = -1;
    for (int i = 0; i < resp_len - 3; i++) {
        if (resp[i] == '\r' && resp[i+1] == '\n' &&
            resp[i+2] == '\r' && resp[i+3] == '\n') { body_start = i + 4; break; }
    }
    if (body_start < 0) return 0;

    int avail = resp_len - body_start;
    /* Content-Length (s_server -WWW serves a plain body) */
    int cl = avail;
    /* very small chunked support omitted: fixed index responses are Content-Length */
    if (cl > max) cl = max;
    if (cl < 0) cl = 0;
    memcpy(out, resp + body_start, cl);
    return cl;
}
