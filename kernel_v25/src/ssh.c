/*
 * ssh.c -- v2.71 SSH-2 server protocol stack.
 *
 * Faithful C port of deliverables/ssh-prototype/sshd_min.py (which was
 * verified end-to-end against OpenSSH 10.3).  The critical facts that
 * cost the prototype hours of debugging, and that MUST NOT regress:
 *
 *  1. Exchange hash H = SHA256(s(V_C)||s(V_S)||s(I_C)||s(I_S)||s(K_S)||
 *     s(q_c)||s(q_s)||mpint(K)) -- EVERY field carries the 4-byte
 *     string length prefix, including the version strings (CRLF
 *     stripped) and the full KEXINIT payloads (msg byte 20 included).
 *  2. Ed25519 signs H directly; the reply blob is
 *     s("ssh-ed25519") || s(sig64).
 *  3. Key derivation: key = SHA256(mpint(K) || H || letter || H || prev)
 *     chained.  Server receive direction uses A/C/E, send B/D/F.
 *  4. aes128-ctr (RFC 4344 SDCTR) encrypts the WHOLE binary packet
 *     INCLUDING the 4-byte packet_length field, with one long-lived
 *     CTR keystream that CONTINUES across packets (big-endian counter).
 *  5. MAC (non-EtM) = HMAC-SHA256(mac_key, seq || plaintext packet).
 *  6. padding: (4 + 1 + payload_len + pad) % 16 == 0 (AES block size,
 *     NOT 8 -- OpenSSH/paramiko both reject 8-aligned AES-CTR packets),
 *     pad >= 4.
 *  7. SERVICE_REQUEST/ACCEPT must happen between NEWKEYS and USERAUTH.
 *  8. channel request body: recipient_channel(4) then string type.
 */

#include <stdint.h>
#include "../include/crypto.h"
#include "../include/net.h"
#include "../include/ssh.h"
#include "../include/shell.h"
#include "../include/fs.h"
#include "../include/task.h"
#include "vga.h"

/* ------------------------------------------------------------------ */
/* tiny local helpers (kernel has no libc)                            */
/* ------------------------------------------------------------------ */

static int ssh_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void ssh_memcpy(uint8_t *d, const uint8_t *s, int n) {
    for (int i = 0; i < n; i++) d[i] = s[i];
}

static int ssh_memcmp(const uint8_t *a, const uint8_t *b, int n) {
    for (int i = 0; i < n; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

static void wr32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* SSH string append: 4-byte BE length + bytes; returns new offset */
static int ssh_put_str(uint8_t *out, const uint8_t *s, int len) {
    wr32be(out, (uint32_t)len);
    ssh_memcpy(out + 4, s, len);
    return 4 + len;
}

/* read a string at offset; returns data pointer/len, advances *off.
 * Returns 0 on truncation. */
static int ssh_str_at(const uint8_t *buf, int buflen, int off,
                      const uint8_t **data, int *dlen) {
    if (off + 4 > buflen) return 0;
    int n = (int)rd32be(buf + off);
    off += 4;
    if (n < 0 || off + n > buflen) return 0;
    *data = buf + off;
    *dlen = n;
    return 1;
}

/* mpint for the shared secret K (unsigned, 32 bytes): strip leading
 * zeros, keep one 0x00 when the high bit is set, 4-byte length prefix */
static int ssh_put_mpint32(uint8_t *out, const uint8_t *k) {
    int first = 0;
    while (first < 32 && k[first] == 0) first++;
    int n = 32 - first;
    const uint8_t *src = k + first;
    uint8_t tmp[33];
    int tn = n;
    if (n == 0) { tmp[0] = 0; src = tmp; tn = 1; }
    else if (src[0] & 0x80) {
        tmp[0] = 0;
        ssh_memcpy(tmp + 1, src, n);
        src = tmp;
        tn = n + 1;
    }
    return ssh_put_str(out, src, tn);
}

/* ------------------------------------------------------------------ */
/* protocol constants                                                 */
/* ------------------------------------------------------------------ */

#define MSG_DISCONNECT            1
#define MSG_SERVICE_REQUEST       5
#define MSG_SERVICE_ACCEPT        6
#define MSG_KEXINIT               20
#define MSG_NEWKEYS               21
#define MSG_KEX_ECDH_INIT         30
#define MSG_KEX_ECDH_REPLY        31
#define MSG_USERAUTH_REQUEST      50
#define MSG_USERAUTH_SUCCESS      52
#define MSG_USERAUTH_FAILURE      51
#define MSG_CHANNEL_OPEN          90
#define MSG_CHANNEL_OPEN_CONFIRM  91
#define MSG_CHANNEL_DATA          94
#define MSG_CHANNEL_EOF           96
#define MSG_CHANNEL_CLOSE         97
#define MSG_CHANNEL_REQUEST       98
#define MSG_CHANNEL_SUCCESS       99
#define MSG_CHANNEL_FAILURE       100

static const char SSH_VERSION[] = "SSH-2.0-CatOS_2.71";

#define SSH_IN_MAX   16384           /* max incoming packet payload */
#define SSH_OUT_MAX  16384           /* max outgoing packet we build */

/* per-session state (kernel is non-preemptive: one static session) */
static struct {
    /* host key */
    uint8_t host_pub[32];
    uint8_t host_priv[64];
    /* version strings (NUL-terminated ascii, no CRLF) */
    char    v_c[64];
    /* KEXINIT payloads, full wire bytes incl. msg type.
     * OpenSSH clients ship ~1.5 KB KEXINITs -- 4 KiB is the safe size. */
    uint8_t  ic[4096];  int ic_len;
    uint8_t  is[1024];  int is_len;
    /* ecdh */
    uint8_t  eph_priv[32];
    uint8_t  q_s[32];
    uint8_t  q_c[64];   int q_c_len;
    uint8_t  H[32];
    uint8_t  Kmp[40];   int Kmp_len;
    /* crypto streams */
    aes128_ctr_t enc_st, dec_st;
    uint8_t mac_out[32], mac_in[32];
    int     encrypted;
    uint32_t in_seq, out_seq;
    /* channel */
    uint32_t peer_chan;
    /* line assembly for the shell channel */
    uint8_t  line[256];
    int      line_len;
    /* per-session scratch buffers: owned by this session, reused across its
     * packet / kex / exec / shell phases.  Concurrent sessions are a 3B item;
     * the single session slot is reused one connection at a time. */
    uint8_t  s_out[SSH_OUT_MAX + 64];
    uint8_t  s_body[SSH_IN_MAX];
    uint8_t  s_macmsg[4 + SSH_IN_MAX];
    uint8_t  s_kdf[64 + 40 + 1 + 32];
    uint8_t  s_msg[8200];
    char     s_exec_line[512];
    char     s_exec_out[8192];
    uint8_t  s_shell_payload[SSH_IN_MAX];
    char     s_shell_out[8192];
    uint8_t  s_loop_payload[SSH_IN_MAX];
    uint8_t  s_hb[8192];
    uint8_t  s_reply[512];
} S;

/* ------------------------------------------------------------------ */
/* packet layer                                                       */
/* ------------------------------------------------------------------ */

static int ssh_send_raw(const uint8_t *buf, int n) {
    return net_tcp_ssend(buf, (uint32_t)n) == 0 ? 0 : -1;
}

/* build, optionally encrypt, and send one binary packet */
static int ssh_send_packet(const uint8_t *payload, int plen) {
    uint8_t *out = S.s_out;
    /* RFC 4253 6: total (4+packlen) must be a multiple of the cipher
     * block size or 8, whichever is LARGER -> 16 for aes128-ctr.
     * OpenSSH enforces this ("padding error: ... mod N") and paramiko
     * raises "Invalid packet blocking" otherwise.  16 also satisfies
     * the pre-NEWKEYS plaintext phase (block size 8). */
    int pad = 16 - ((4 + 1 + plen) % 16);
    if (pad < 4) pad += 16;
    int pktlen = 1 + plen + pad;                  /* padding_length..padding */
    if (plen < 0 || 4 + pktlen > SSH_OUT_MAX + 4) return -1;

    uint8_t *pkt = out + 4;
    pkt[0] = (uint8_t)pad;
    ssh_memcpy(pkt + 1, payload, plen);
    for (int i = 0; i < pad; i++) pkt[1 + plen + i] = 0;
    wr32be(out, (uint32_t)pktlen);                /* plaintext length field */

    if (!S.encrypted) {
        if (ssh_send_raw(out, 4 + pktlen) != 0) return -1;
    } else {
        uint8_t mac[32];
        /* MAC over seq || plaintext (length field + packet) */
        uint8_t seqb[4];
        wr32be(seqb, S.out_seq);
        hmac_sha256(mac, S.mac_out, 32, seqb, 4, out, 4 + pktlen);
        /* whole packet (length included) through the persistent CTR */
        crypto_aes128_ctr_crypt(&S.enc_st, out, (uint32_t)(4 + pktlen));
        ssh_memcpy(out + 4 + pktlen, mac, 32);
        if (ssh_send_raw(out, 4 + pktlen + 32) != 0) return -1;
    }
    S.out_seq++;
    return 0;
}

/* blocking read of exactly n bytes; 0 ok, -1 closed */
static int ssh_readn(uint8_t *buf, int n) {
    return net_tcp_srecv_all(buf, n) == n ? 0 : -1;
}

/* read one binary packet; returns payload length (>0), payload[0] is the
 * message type; -1 on error/close.  payload buffer must hold SSH_IN_MAX. */
static int ssh_recv_packet(uint8_t *payload) {
    uint8_t lenb[4], mac[32];
    if (!S.encrypted) {
        if (ssh_readn(lenb, 4) != 0) return -1;
        uint32_t n = rd32be(lenb);
        if (n < 2 || n > SSH_IN_MAX) return -1;
        uint8_t *body = payload;              /* reuse: padlen..payload..pad */
        if (ssh_readn(body, (int)n) != 0) return -1;
        int pad = body[0];
        if (pad < 4 || (int)pad + 1 > (int)n) return -1;
        S.in_seq++;
        /* shift payload to the front (type first) */
        int plen = (int)n - 1 - pad;
        for (int i = 0; i < plen; i++) payload[i] = body[1 + i];
        return plen;
    }
    /* encrypted: entire packet incl. length is CTR-protected */
    if (ssh_readn(lenb, 4) != 0) return -1;
    crypto_aes128_ctr_crypt(&S.dec_st, lenb, 4);
    uint32_t n = rd32be(lenb);
    if (n < 2 || n > SSH_IN_MAX) return -1;
    uint8_t *body = S.s_body;
    if (ssh_readn(body, (int)n) != 0) return -1;
    crypto_aes128_ctr_crypt(&S.dec_st, body, n);
    if (ssh_readn(mac, 32) != 0) return -1;
    /* verify MAC: seq || plaintext length field || plaintext body */
    {
        uint8_t expect[32], seqb[4];
        uint8_t *macmsg = S.s_macmsg;
        wr32be(seqb, S.in_seq);
        ssh_memcpy(macmsg, lenb, 4);
        ssh_memcpy(macmsg + 4, body, (int)n);
        hmac_sha256(expect, S.mac_in, 32, seqb, 4, macmsg, 4 + (int)n);
        if (ssh_memcmp(expect, mac, 32) != 0) return -2;   /* MAC failure */
    }
    S.in_seq++;
    int pad = body[0];
    if (pad < 4 || (int)pad + 1 > (int)n) return -1;
    int plen = (int)n - 1 - pad;
    for (int i = 0; i < plen; i++) payload[i] = body[1 + i];
    return plen;
}

/* key derivation (RFC 4253 7.2): Kmp || H || letter || H || prev */
static void ssh_derive(uint8_t *out, int count, const char letter) {
    uint8_t prev[32];
    int got = 0;
    uint8_t ltr = (uint8_t)letter;
    prev[0] = 0;                              /* prev = empty */
    int plen = 0;
    while (got < count) {
        /* hash input: Kmp || H || letter || H || prev */
        uint8_t *buf = S.s_kdf;
        int o = 0;
        ssh_memcpy(buf + o, S.Kmp, S.Kmp_len); o += S.Kmp_len;
        ssh_memcpy(buf + o, S.H, 32);          o += 32;
        buf[o++] = ltr;
        ssh_memcpy(buf + o, S.H, 32);          o += 32;
        ssh_memcpy(buf + o, prev, plen);       o += plen;
        sha256(prev, buf, (uint32_t)o);
        plen = 32;
        int take = count - got;
        if (take > 32) take = 32;
        ssh_memcpy(out + got, prev, take);
        got += take;
    }
}

static void ssh_set_keys(void) {
    uint8_t iv_a[16], key_c[16], iv_b[16], key_d[16];
    ssh_derive(iv_a, 16, 'A');                  /* server receive IV */
    ssh_derive(key_c, 16, 'C');                 /* server receive key */
    ssh_derive(iv_b, 16, 'B');                  /* server send IV */
    ssh_derive(key_d, 16, 'D');                 /* server send key */
    ssh_derive(S.mac_in, 32, 'E');              /* server receive MAC key */
    ssh_derive(S.mac_out, 32, 'F');             /* server send MAC key */
    crypto_aes128_ctr_init(&S.dec_st, key_c, iv_a);
    crypto_aes128_ctr_init(&S.enc_st, key_d, iv_b);
    S.encrypted = 1;
}

/* ------------------------------------------------------------------ */
/* payload helpers                                                    */
/* ------------------------------------------------------------------ */

/* blob for the exchange hash and ECDH reply: s("ssh-ed25519") || s(pub) */
static int ssh_host_blob(uint8_t *out) {
    int o = ssh_put_str(out, (const uint8_t *)"ssh-ed25519", 11);
    o += ssh_put_str(out + o, S.host_pub, 32);
    return o;
}

static int ssh_build_kexinit(uint8_t *out) {
    uint8_t cookie[16];
    crypto_random(cookie, 16);
    int o = 0;
    out[o++] = MSG_KEXINIT;
    ssh_memcpy(out + o, cookie, 16); o += 16;
    o += ssh_put_str(out + o, (const uint8_t *)"curve25519-sha256,curve25519-sha256@libssh.org", 46);
    o += ssh_put_str(out + o, (const uint8_t *)"ssh-ed25519", 11);
    o += ssh_put_str(out + o, (const uint8_t *)"aes128-ctr", 10);
    o += ssh_put_str(out + o, (const uint8_t *)"aes128-ctr", 10);
    o += ssh_put_str(out + o, (const uint8_t *)"hmac-sha2-256", 13);
    o += ssh_put_str(out + o, (const uint8_t *)"hmac-sha2-256", 13);
    o += ssh_put_str(out + o, (const uint8_t *)"none", 4);
    o += ssh_put_str(out + o, (const uint8_t *)"none", 4);
    o += ssh_put_str(out + o, (const uint8_t *)"", 0);
    o += ssh_put_str(out + o, (const uint8_t *)"", 0);
    out[o++] = 0;                                /* first_kex_packet_follows */
    wr32be(out + o, 0); o += 4;                  /* reserved */
    return o;
}

/* shell bridge (implemented in shell.c): run `line` as if typed at the
 * prompt, capturing console output into `out`; *rc gets the exit code */
extern int shell_ssh_exec_line(const char *line, char *out, int outmax,
                               int *rc);

/* ------------------------------------------------------------------ */
/* channel output                                                     */
/* ------------------------------------------------------------------ */

static int ssh_chan_data(const uint8_t *data, int len) {
    uint8_t *msg = S.s_msg;
    int chunk = len;
    if (chunk > 8000) chunk = 8000;
    msg[0] = MSG_CHANNEL_DATA;
    wr32be(msg + 1, S.peer_chan);
    ssh_put_str(msg + 5, data, chunk);           /* 4 len bytes + chunk */
    return ssh_send_packet(msg, 5 + 4 + chunk);
}

static int ssh_chan_send_all(const char *text) {
    int n = ssh_strlen(text);
    int off = 0;
    while (off < n) {
        int chunk = n - off;
        if (chunk > 8000) chunk = 8000;
        if (ssh_chan_data((const uint8_t *)text + off, chunk) != 0) return -1;
        off += chunk;
    }
    return 0;
}

static int ssh_chan_simple(uint8_t type, uint32_t recipient) {
    uint8_t msg[5];
    msg[0] = type;
    wr32be(msg + 1, recipient);
    return ssh_send_packet(msg, 5);
}

/* ------------------------------------------------------------------ */
/* session serving                                                    */
/* ------------------------------------------------------------------ */

static int ssh_read_banner(void) {
    int n = 0;
    for (;;) {
        uint8_t ch;
        if (ssh_readn(&ch, 1) != 0) return -1;
        if (ch == '\n') break;
        if (ch != '\r' && n < (int)sizeof(S.v_c) - 1)
            S.v_c[n++] = (char)ch;
    }
    S.v_c[n] = 0;
    return (int)n;
}

static int ssh_send_banner(void) {
    uint8_t buf[80];
    int n = 0;
    const char *v = SSH_VERSION;
    while (*v) buf[n++] = (uint8_t)*v++;
    buf[n++] = '\r';
    buf[n++] = '\n';
    return ssh_send_raw(buf, n);
}

/* exec "command": run it, stream output, send exit-status, close */
static int ssh_do_exec(const uint8_t *cmd, int cmdlen, uint32_t peer) {
    char *line = S.s_exec_line;
    char *out = S.s_exec_out;
    int rc = 0;
    int n = cmdlen;
    if (n > (int)sizeof(S.s_exec_line) - 1) n = (int)sizeof(S.s_exec_line) - 1;
    ssh_memcpy((uint8_t *)line, cmd, n);
    line[n] = 0;

    out[0] = 0;
    shell_ssh_exec_line(line, out, (int)sizeof(S.s_exec_out), &rc);

    /* stream captured output (send at least one data packet) */
    {
        int olen = ssh_strlen(out);
        int off = 0;
        do {
            int chunk = olen - off;
            if (chunk > 8000) chunk = 8000;
            if (ssh_chan_data((const uint8_t *)out + off, chunk) != 0)
                return -1;
            off += chunk;
        } while (off < olen);
    }
    /* exit-status */
    {
        uint8_t m[32];           /* 1+4+4+11+1+4 = 25 bytes */
        m[0] = MSG_CHANNEL_REQUEST;
        wr32be(m + 1, peer);
        int o = 5 + ssh_put_str(m + 5, (const uint8_t *)"exit-status", 11);
        m[o++] = 0;                              /* want_reply = 0 */
        wr32be(m + o, (uint32_t)rc);
        o += 4;
        if (ssh_send_packet(m, o) != 0) return -1;
    }
    if (ssh_chan_simple(MSG_CHANNEL_EOF, peer) != 0) return -1;
    if (ssh_chan_simple(MSG_CHANNEL_CLOSE, peer) != 0) return -1;
    return 0;
}

/* interactive shell channel: prompt / line / execute loop */
static int ssh_do_shell(uint32_t peer) {
    ssh_chan_send_all("CatOS sshd -- commands end with Enter; empty line exits\r\n");
    ssh_chan_send_all("> ");
    S.line_len = 0;
    for (;;) {
        uint8_t *payload = S.s_shell_payload;
        int plen = ssh_recv_packet(payload);
        if (plen < 0) return -1;
        if (plen == 0) continue;
        uint8_t type = payload[0];
        if (type == MSG_CHANNEL_CLOSE || type == MSG_DISCONNECT) {
            ssh_chan_simple(MSG_CHANNEL_CLOSE, peer);
            return 0;
        }
        if (type == MSG_CHANNEL_EOF) continue;
        if (type != MSG_CHANNEL_DATA) continue;
        /* payload: type(1) || recipient(4) || string data */
        const uint8_t *data; int dlen;
        if (!ssh_str_at(payload, plen, 5, &data, &dlen)) continue;
        for (int i = 0; i < dlen; i++) {
            uint8_t ch = data[i];
            if (ch == '\r' || ch == '\n') {
                ssh_chan_data((const uint8_t *)"\r\n", 2);
                S.line[S.line_len] = 0;
                if (S.line_len > 0) {
                    char *out = S.s_shell_out;
                    int rc = 0;
                    out[0] = 0;
                    shell_ssh_exec_line((const char *)S.line, out,
                                        (int)sizeof(S.s_shell_out), &rc);
                    ssh_chan_send_all(out);
                    S.line_len = 0;
                } else {
                    ssh_chan_send_all("(bye)\r\n");
                    ssh_chan_simple(MSG_CHANNEL_EOF, peer);
                    ssh_chan_simple(MSG_CHANNEL_CLOSE, peer);
                    return 0;
                }
                ssh_chan_send_all("> ");
            } else if ((ch == 0x7f || ch == 0x08) && S.line_len > 0) {
                S.line_len--;
                ssh_chan_data((const uint8_t *)"\b \b", 3);
            } else if (ch >= 0x20 && ch < 0x7f &&
                       S.line_len < (int)sizeof(S.line) - 1) {
                S.line[S.line_len++] = (char)ch;
                ssh_chan_data(&ch, 1);           /* echo (client has no PTY) */
            }
            /* other control bytes ignored */
        }
    }
}

/* v2.72: persistent host key -- /etc/ssh_host.key holds the 64-byte
 * Ed25519 private key (sk[0..31] seed, sk[32..63] public).  Generated on
 * first use, reused afterwards so client known_hosts entries stay valid. */
#define SSH_HOSTKEY_PATH "/etc/ssh_host.key"
static void ssh_host_key_load(void) {
    uint8_t sk[64];
    int have = 0;
    if (fs_fs_ready()) {
        int n = fs_read_at(SSH_HOSTKEY_PATH, (char *)sk, 0, 64);
        if (n == 64) {
            ssh_memcpy(S.host_priv, sk, 64);
            ssh_memcpy(S.host_pub, sk + 32, 32);
            have = 1;
        }
    }
    if (!have) {
        crypto_ed25519_keypair(S.host_pub, S.host_priv);
        if (fs_fs_ready())
            fs_write_raw(SSH_HOSTKEY_PATH, (const char *)S.host_priv, 64);
    }
}

/* v2.72: background daemon entry -- spawned via task_spawn_r0() from the
 * `sshd -d` command or the /etc/autoexec boot script ("@sshd").  NEVER
 * returns.  Waits for the NIC, brings up DHCP, listens on 22 and serves
 * sessions forever; ESC handling only exists in the foreground cmd_sshd. */
void sshd_daemon_main(void) {
    int listening = 0;
    for (;;) {
        if (!net_up()) { task_sleep_ticks(1); continue; }
        if (!listening) {
            net_dhcp_ensure();
            net_tcp_listen(22);
            listening = 1;
            vga_write("sshd[daemon]: listening on port 22\n");
        }
        if (net_tcp_saccept() == 0) {
            vga_write("sshd[daemon]: session opened\n");
            ssh_serve_session();
            vga_write("sshd[daemon]: session closed\n");
        }
        task_maybe_yield();          /* saccept already polled ~2s inside */
    }
}

int ssh_serve_session(void) {
    uint8_t *payload = S.s_loop_payload;
    S.encrypted = 0;
    S.in_seq = S.out_seq = 0;
    S.line_len = 0;
    S.ic_len = S.is_len = 0;
    S.q_c_len = 0;
    S.Kmp_len = 0;

    /* persistent host key (v2.72): loaded from /etc/ssh_host.key */
    ssh_host_key_load();

    if (ssh_read_banner() < 0) return -1;
    if (ssh_send_banner() != 0) return -1;

    /* KEXINIT exchange */
    S.is_len = ssh_build_kexinit(S.is);
    if (ssh_send_packet(S.is, S.is_len) != 0) return -1;
    int plen = ssh_recv_packet(payload);
    if (plen <= 0 || payload[0] != MSG_KEXINIT) return -1;
    ssh_memcpy(S.ic, payload, plen);
    S.ic_len = plen;

    /* KEX_ECDH_INIT: string q_c */
    plen = ssh_recv_packet(payload);
    if (plen <= 0 || payload[0] != MSG_KEX_ECDH_INIT) return -1;
    {
        const uint8_t *qc; int qlen;
        if (!ssh_str_at(payload, plen, 1, &qc, &qlen) || qlen != 32)
            return -1;
        ssh_memcpy(S.q_c, qc, qlen);
        S.q_c_len = qlen;
    }
    crypto_random(S.eph_priv, 32);
    crypto_x25519_pub(S.q_s, S.eph_priv);
    {
        uint8_t K[32];
        crypto_x25519(K, S.eph_priv, S.q_c);
        S.Kmp_len = ssh_put_mpint32(S.Kmp, K);
    }

    /* exchange hash H (see header comment rule 1) */
    {
        uint8_t *hb = S.s_hb;
        int o = 0;
        o += ssh_put_str(hb + o, (const uint8_t *)S.v_c, ssh_strlen(S.v_c));
        o += ssh_put_str(hb + o, (const uint8_t *)SSH_VERSION,
                         (int)sizeof(SSH_VERSION) - 1);
        o += ssh_put_str(hb + o, S.ic, S.ic_len);
        o += ssh_put_str(hb + o, S.is, S.is_len);
        {
            uint8_t blob[80];
            int bl = ssh_host_blob(blob);
            o += ssh_put_str(hb + o, blob, bl);
        }
        o += ssh_put_str(hb + o, S.q_c, S.q_c_len);
        o += ssh_put_str(hb + o, S.q_s, 32);
        /* S.Kmp is already a complete SSH string (ssh_put_mpint32 emitted
         * its own 4-byte length prefix) -- append it RAW, no re-wrap. */
        ssh_memcpy(hb + o, S.Kmp, S.Kmp_len); o += S.Kmp_len;
        sha256(S.H, hb, (uint32_t)o);
    }

    /* KEX_ECDH_REPLY: s(K_S) || s(q_s) || s(sigblob) */
    {
        uint8_t *reply = S.s_reply;
        uint8_t sig[64];
        uint8_t sigblob[96];     /* s("ssh-ed25519")+s(sig64) = 83 bytes! */
        crypto_ed25519_sign(sig, S.H, 32, S.host_priv);
        int sb = 0;
        sb += ssh_put_str(sigblob + sb, (const uint8_t *)"ssh-ed25519", 11);
        sb += ssh_put_str(sigblob + sb, sig, 64);
        int o = 0;
        reply[o++] = MSG_KEX_ECDH_REPLY;
        {
            uint8_t blob[80];
            int bl = ssh_host_blob(blob);
            o += ssh_put_str(reply + o, blob, bl);
        }
        o += ssh_put_str(reply + o, S.q_s, 32);
        o += ssh_put_str(reply + o, sigblob, sb);
        if (ssh_send_packet(reply, o) != 0) return -1;
    }
    {   /* NEWKEYS */
        uint8_t nk = MSG_NEWKEYS;
        if (ssh_send_packet(&nk, 1) != 0) return -1;
    }
    plen = ssh_recv_packet(payload);
    if (plen != 1 || payload[0] != MSG_NEWKEYS) return -1;
    ssh_set_keys();

    /* SERVICE_REQUEST -> ACCEPT (rule 7) */
    plen = ssh_recv_packet(payload);
    if (plen <= 0) return -1;
    if (payload[0] == MSG_SERVICE_REQUEST) {
        const uint8_t *svc; int sl;
        if (!ssh_str_at(payload, plen, 1, &svc, &sl)) return -1;
        uint8_t m[64];
        m[0] = MSG_SERVICE_ACCEPT;
        int o = 1 + ssh_put_str(m + 1, svc, sl);
        if (ssh_send_packet(m, o) != 0) return -1;
        plen = ssh_recv_packet(payload);
        if (plen <= 0) return -1;
    }

    /* USERAUTH loop: accept any request (v1; harden later) */
    while (payload[0] != MSG_USERAUTH_REQUEST) {
        plen = ssh_recv_packet(payload);
        if (plen <= 0) return -1;
    }
    {
        uint8_t ok = MSG_USERAUTH_SUCCESS;
        if (ssh_send_packet(&ok, 1) != 0) return -1;
    }

    /* channel loop: one TCP connection may multiplex several session
     * channels (paramiko reuses the connection for exec + shell), so
     * keep serving channels until the transport goes away. */
    for (;;) {
        plen = ssh_recv_packet(payload);
        if (plen <= 0) return (plen == 0) ? 0 : -1;
        uint8_t type = payload[0];
        if (type == MSG_DISCONNECT) return 0;
        if (type != MSG_CHANNEL_OPEN) continue;      /* EXT_INFO etc. */

        /* CHANNEL_OPEN "session" */
        {
            const uint8_t *typ; int tl;
            if (!ssh_str_at(payload, plen, 1, &typ, &tl)) return -1;
            S.peer_chan = rd32be(payload + 1 + 4 + tl);
            uint8_t m[17];
            m[0] = MSG_CHANNEL_OPEN_CONFIRM;
            wr32be(m + 1, S.peer_chan);
            wr32be(m + 5, 0);                        /* our channel id */
            wr32be(m + 9, 0x20000);                  /* initial window */
            wr32be(m + 13, 0x8000);                  /* max packet */
            if (ssh_send_packet(m, 17) != 0) return -1;
        }

        /* request loop for this channel */
        for (;;) {
            plen = ssh_recv_packet(payload);
            if (plen < 0) return -1;
            if (plen == 0) continue;
            uint8_t type2 = payload[0];
            if (type2 == MSG_DISCONNECT) return 0;
            if (type2 == MSG_CHANNEL_CLOSE) {
                ssh_chan_simple(MSG_CHANNEL_CLOSE, S.peer_chan);
                break;                               /* channel done */
            }
            if (type2 == MSG_CHANNEL_EOF) continue;
            if (type2 != MSG_CHANNEL_REQUEST) continue;
            /* body: recipient(4) || string type || want_reply || ... */
            const uint8_t *rt; int rl;
            if (!ssh_str_at(payload, plen, 5, &rt, &rl)) continue;
            int off = 5 + 4 + rl;
            uint8_t want = (off < plen) ? payload[off] : 0;
            off++;
            /* tolerate benign session requests: paramiko's invoke_shell
             * sends pty-req first, and _request_failed() CLOSES the
             * channel on a FAILURE reply -- accept these instead. */
            if ((rl == 7 &&
                 ssh_memcmp(rt, (const uint8_t *)"pty-req", 7) == 0) ||
                (rl == 13 &&
                 ssh_memcmp(rt, (const uint8_t *)"window-change", 13) == 0) ||
                (rl == 3 && ssh_memcmp(rt, (const uint8_t *)"env", 3) == 0)) {
                if (want)
                    ssh_chan_simple(MSG_CHANNEL_SUCCESS, S.peer_chan);
                continue;
            }
            if (rl == 4 && rt[0] == 'e' && rt[1] == 'x' && rt[2] == 'e' &&
                rt[3] == 'c') {
                const uint8_t *cmd = (const uint8_t *)"";
                int cl = 0;
                ssh_str_at(payload, plen, off, &cmd, &cl);
                if (want)
                    ssh_chan_simple(MSG_CHANNEL_SUCCESS, S.peer_chan);
                if (ssh_do_exec(cmd, cl, S.peer_chan) != 0) return -1;
                continue;    /* we sent EOF+CLOSE; consume peer CLOSE */
            }
            if (rl == 5 && rt[0] == 's' && rt[1] == 'h' && rt[2] == 'e' &&
                rt[3] == 'l' && rt[4] == 'l') {
                if (want)
                    ssh_chan_simple(MSG_CHANNEL_SUCCESS, S.peer_chan);
                if (ssh_do_shell(S.peer_chan) != 0) return -1;
                continue;
            }
            if (want)
                ssh_chan_simple(MSG_CHANNEL_FAILURE, S.peer_chan);
        }
    }
}
