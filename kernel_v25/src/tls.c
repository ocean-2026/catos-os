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
#include "../include/serial_log.h"
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
    if (plen < 17) return -1;
    int ct_len = plen - 16;
    if (ct_len % 16 != 0) return -1;

    uint8_t dec[16384];
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
 * Returns msg type, fills body/len.  */
static int hs_read(uint8_t *msg_type, const uint8_t **body, int *blen) {
    for (;;) {
        uint8_t type; const uint8_t *p; int len;
        if (recv_record(&type, &p, &len) != 0) return -1;
        if (type == 21) return -2;                 /* alert */
        if (type == 20) continue;                  /* CCS, plaintext */
        ser_write("hs_recv type="); ser_hex("x", (uint32_t)type);
        ser_write(" len="); ser_hex("x", (uint32_t)len); ser_write("\n");
        if (type != 22) return -1;
        if (len < 4) return -1;
        *msg_type = p[0];
        *blen = ((int)p[1] << 16) | ((int)p[2] << 8) | p[3];
        *body = p + 4;
        transcript_add(p, len);
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
    tlen = 0; cseq = 0; sseq = 0;

    /* ---- TCP connect ---- */
    if (net_tcp_connect(peer, port) != 0) { ser_write("tls: connect fail\n"); return -3; }
    ser_write("tls: connected\n");

    /* ---- our ECDHE keypair ---- */
    uint8_t our_pub[64], our_priv[32];
    uECC_Curve curve = uECC_secp256r1();
    if (!uECC_make_key(our_pub, our_priv, curve)) { ser_write("tls: make_key fail\n"); return -6; }
    ser_write("tls: key\n");

    /* ---- ClientHello ---- */
    /* version || random || sid(0) || suites(0xC027) || compression(null)
     * || extensions: supported_groups(secp256r1).  The groups extension is
     * required: without it OpenSSL 3.0 aborts the ECDHE handshake with a
     * handshake_failure alert. */
    uint8_t ch[64];
    int cn = 0;
    ch[cn++] = 0x03; ch[cn++] = 0x03;
    uint8_t crandom[32]; crypto_random(crandom, 32);
    memcpy(ch + cn, crandom, 32); cn += 32;
    ch[cn++] = 0;                              /* session id len */
    ch[cn++] = 0x00; ch[cn++] = 0x02;         /* cipher suite list len */
    ch[cn++] = 0xC0; ch[cn++] = 0x27;         /* 0xC027 */
    ch[cn++] = 0x01; ch[cn++] = 0x00;         /* compression: null */
    /* extensions: supported_groups(secp256r1) + signature_algorithms.
     * Both are required by OpenSSL 3.0: without supported_groups the ECDHE
     * group is unknown; without signature_algorithms the handshake aborts
     * with handshake_failure. */
    ch[cn++] = 0x00; ch[cn++] = 0x14;         /* extensions block len (20) */
    ch[cn++] = 0x00; ch[cn++] = 0x0A;         /* ext: supported_groups */
    ch[cn++] = 0x00; ch[cn++] = 0x04;         /* ext data len */
    ch[cn++] = 0x00; ch[cn++] = 0x02;         /* group list len */
    ch[cn++] = 0x00; ch[cn++] = 0x17;         /* secp256r1 (23) */
    ch[cn++] = 0x00; ch[cn++] = 0x0D;         /* ext: signature_algorithms */
    ch[cn++] = 0x00; ch[cn++] = 0x08;         /* ext data len */
    ch[cn++] = 0x00; ch[cn++] = 0x06;         /* alg list len */
    ch[cn++] = 0x04; ch[cn++] = 0x01;         /* rsa_pkcs1_sha256 */
    ch[cn++] = 0x03; ch[cn++] = 0x03;         /* ecdsa_secp256r1_sha256 */
    ch[cn++] = 0x08; ch[cn++] = 0x04;         /* rsa_pss_rsae_sha256 */
    if (hs_send(1, ch, cn) != 0) return -6;
    ser_write("tls: ch sent\n");

    /* ---- ServerHello ---- */
    uint8_t mtype; const uint8_t *body; int blen;
    if (hs_read(&mtype, &body, &blen) != 0) { ser_write("tls: read SH fail\n"); return -6; }
    if (mtype != 2 || blen < 34) { ser_write("tls: SH type/len bad\n"); return -6; }
    if (body[0] != 0x03 || body[1] != 0x03) return -6;
    uint8_t srandom[32]; memcpy(srandom, body + 2, 32);
    ser_write("tls: sh ok\n");
    /* skip session id, cipher, compression */

    /* ---- Certificate: skip ---- */
    if (hs_read(&mtype, &body, &blen) != 0) { ser_write("tls: read Cert fail\n"); return -6; }
    if (mtype != 11) return -6;
    ser_write("tls: cert ok\n");

    /* ---- ServerKeyExchange: parse curve + peer pub, skip signature ---- */
    if (hs_read(&mtype, &body, &blen) != 0) { ser_write("tls: read SKE fail\n"); return -6; }
    if (mtype != 12) { ser_write("tls: SKE type bad\n"); return -6; }
    ser_write("ske b="); ser_hex("x", body[0]); ser_hex("x", body[1]);
    ser_hex("x", body[2]); ser_hex("x", body[3]); ser_write("\n");
    if (blen < 4) return -6;
    if (body[0] != 3) { ser_write("tls: SKE curve_type bad\n"); return -6; }
    uint16_t curve_id = ((uint16_t)body[1] << 8) | body[2];
    if (curve_id != 23) { ser_write("tls: SKE curve bad\n"); return -6; }
    uint8_t peer_pub[64];
    int plen_pub = body[3];
    /* ECDHE point: 1-byte 0x04 (uncompressed) + 64-byte X||Y = 65 bytes. */
    if (plen_pub != 65 || body[4] != 0x04 || blen < 5 + 64) {
        ser_write("tls: SKE pub len bad\n"); return -6;
    }
    memcpy(peer_pub, body + 5, 64);
    ser_write("tls: ske ok\n");

    /* ---- ServerHelloDone ---- */
    if (hs_read(&mtype, &body, &blen) != 0) { ser_write("tls: read SHD fail\n"); return -6; }
    if (mtype != 14) { ser_write("tls: SHD type bad\n"); return -6; }
    ser_write("tls: shd\n");

    /* ---- ECDH shared secret ---- */
    uint8_t pre_master[32];
    if (!uECC_shared_secret(peer_pub, our_priv, pre_master, curve)) { ser_write("tls: shared secret fail\n"); return -6; }
    ser_write("tls: shared\n");

    /* ---- master secret + key block ---- */
    uint8_t master[48];
    uint8_t rand64[64];
    memcpy(rand64, crandom, 32); memcpy(rand64 + 32, srandom, 32);
    tls_prf(master, 48, pre_master, 32, "master secret", rand64, 64);

    uint8_t kb[128];
    uint8_t rand64b[64];
    memcpy(rand64b, srandom, 32); memcpy(rand64b + 32, crandom, 32);
    tls_prf(kb, 128, master, 48, "key expansion", rand64b, 64);
    memcpy(c_mac, kb + 0, 32);  memcpy(s_mac, kb + 32, 32);
    memcpy(c_key, kb + 64, 16); memcpy(s_key, kb + 80, 16);
    memcpy(c_iv, kb + 96, 16);  memcpy(s_iv, kb + 112, 16);
    ser_write("tls: keys derived\n");

    /* ---- ClientKeyExchange: 1-byte 0x04 prefix + 64-byte X||Y = 65 bytes. */
    uint8_t cke[66];
    cke[0] = 65; cke[1] = 0x04; memcpy(cke + 2, our_pub, 64);
    if (hs_send(16, cke, 66) != 0) return -6;
    ser_write("tls: cke sent\n");

    /* ---- ChangeCipherSpec ---- */
    {
        uint8_t ccs[1] = { 1 };
        if (send_record(20, ccs, 1) != 0) return -6;
    }
    ser_write("tls: ccs sent\n");

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
    ser_write("tls: cli fin sent\n");

    /* ---- server CCS then encrypted server Finished ---- */
    for (;;) {
        uint8_t type; const uint8_t *p; int len;
        if (recv_record(&type, &p, &len) != 0) return -6;
        if (type == 20) continue;              /* server CCS */
        if (type != 22) return -6;
        /* encrypted handshake record: decrypt then parse Finished */
        uint8_t inner[16384]; int ilen;
        if (app_recv(p, len, 22, inner, &ilen) != 0) { ser_write("tls: srv fin decrypt fail\n"); return -6; }
        ser_write("tls: srv fin decrypted\n");
        if (ilen < 4 || inner[0] != 20) return -6;
        int vlen = ((int)inner[1] << 16) | ((int)inner[2] << 8) | inner[3];
        if (vlen != 12 || ilen < 16) return -6;
        uint8_t srv_verify[12];
        tls_prf(srv_verify, 12, master, 48, "server finished", srvhash, 32);
        uint8_t diff = 0;
        for (int i = 0; i < 12; i++) diff |= (uint8_t)(srv_verify[i] ^ inner[4 + i]);
        if (diff != 0) { ser_write("tls: srv fin verify fail\n"); return -6; }
        ser_write("tls: srv fin ok\n");
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
    uint8_t resp[20480];
    int resp_len = 0;
    for (int k = 0; k < 60; k++) {
        uint8_t type; const uint8_t *p; int len;
        if (recv_record(&type, &p, &len) != 0) break;
        if (type == 21) return -6;
        if (type != 23) continue;
        uint8_t frag[16384]; int flen;
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
