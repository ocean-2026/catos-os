/* netstack.c - minimal Ethernet ARP / IPv4 / TCP (client) / HTTP stack.
 *
 * The NIC is polled: net_poll() -> net_rx_dispatch() demultiplexes each
 * incoming frame.  TCP is a single simplified client connection used by
 * net_http_get() (SYN handshake, one-shot data exchange, FIN shutdown;
 * no retransmission - fine for the local slirp network in QEMU).
 */

#include "net.h"
#include "task.h"
extern volatile uint32_t timer_ticks;

/* ---- configuration (defaults: QEMU user-net) ----
 * net.c overwrites these for VMware NAT (192.168.64.x) after layout probe. */
uint8_t net_our_ip[4] = {10, 0, 2, 15};
uint8_t net_gw_ip[4]  = {10, 0, 2, 2};

/* ---- our MAC (filled by net.c) ---- */
extern uint8_t net_mac[6];
extern int     net_ready;

/* ------------------------------------------------------------------ */
/* byte helpers (all network quantities are big-endian on the wire)    */
/* ------------------------------------------------------------------ */

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static uint16_t ip_checksum(const uint8_t *data, uint32_t len) {
    uint32_t sum = 0;
    const uint8_t *p = data;
    while (len > 1) { sum += (uint16_t)((p[0] << 8) | p[1]); p += 2; len -= 2; }
    if (len) sum += (uint16_t)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t tcp_checksum(const uint8_t *src, const uint8_t *dst,
                             const uint8_t *seg, uint32_t seglen) {
    uint32_t sum = 0;
    for (int i = 0; i < 4; i += 2) sum += (uint16_t)((src[i] << 8) | src[i+1]);
    for (int i = 0; i < 4; i += 2) sum += (uint16_t)((dst[i] << 8) | dst[i+1]);
    sum += 6;                                   /* protocol TCP */
    sum += (uint32_t)seglen;                    /* TCP segment length */
    const uint8_t *p = seg;
    uint32_t n = seglen;
    while (n > 1) { sum += (uint16_t)((p[0] << 8) | p[1]); p += 2; n -= 2; }
    if (n) sum += (uint16_t)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static int ip_parse(const char *s, uint8_t out[4]) {
    int v[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; i++) {
        int n = 0;
        while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
        if (n > 255 || (*s != '.' && i < 3) || (*s == '.' && i == 3)) return -1;
        v[i] = n;
        if (*s == '.') s++;
    }
    out[0] = (uint8_t)v[0]; out[1] = (uint8_t)v[1];
    out[2] = (uint8_t)v[2]; out[3] = (uint8_t)v[3];
    return 0;
}

/* ------------------------------------------------------------------ */
/* ARP                                                                 */
/* ------------------------------------------------------------------ */

static uint8_t arp_cache_mac[6];
static uint8_t arp_cache_ip[4];
static int     arp_cache_valid = 0;
static uint32_t arp_cache_tick = 0;      /* aging, see arp_handle */

#define ARP_TTL_TICKS 6000u               /* 60 s at 100 Hz */

static void arp_request(const uint8_t tpa[4]) {
    uint8_t f[14 + 28];
    for (int i = 0; i < 6; i++) { f[i] = 0xFF; f[6+i] = net_mac[i]; }  /* dst=BCAST src=us */
    wr16(f + 12, 0x0806);                                                /* ARP */
    uint8_t *a = f + 14;
    wr16(a + 0, 1);       wr16(a + 2, 0x0800);                           /* htype/ptype */
    a[4] = 6; a[5] = 4;   wr16(a + 6, 1);                                /* hlen/plen/oper=request */
    for (int i = 0; i < 6; i++) a[8+i] = net_mac[i];                     /* sha */
    for (int i = 0; i < 4; i++) a[14+i] = net_our_ip[i];                 /* spa */
    for (int i = 0; i < 6; i++) a[18+i] = 0;                             /* tha */
    for (int i = 0; i < 4; i++) a[24+i] = tpa[i];                        /* tpa */
    net_send(f, sizeof(f));
}

static void arp_handle(const uint8_t *f, uint32_t len) {
    if (len < 14 + 28) return;
    const uint8_t *a = f + 14;
    if (rd16(a + 0) != 1 || rd16(a + 2) != 0x0800) return;
    uint16_t oper = rd16(a + 6);

    /* Cache the sender (sha/spa).  QEMU slirp answers proxy-ARP with a
     * REQUEST, so we still learn from requests - but an EXISTING mapping
     * is only replaced by a REPLY (a stray request no longer poisons the
     * cache), and entries age out after ARP_TTL_TICKS. */
    int spa_nonzero = 0;
    for (int i = 0; i < 4; i++) if (a[14+i] != 0) { spa_nonzero = 1; break; }
    if (arp_cache_valid &&
        (uint32_t)(timer_ticks - arp_cache_tick) > ARP_TTL_TICKS)
        arp_cache_valid = 0;              /* aged out */
    int overwrite = !arp_cache_valid || oper == 2;
    if (spa_nonzero && overwrite) {
        for (int i = 0; i < 4; i++) arp_cache_ip[i] = a[14+i];
        for (int i = 0; i < 6; i++) arp_cache_mac[i] = a[8+i];
        arp_cache_valid = 1;
        arp_cache_tick = timer_ticks;
    }

    if (oper == 2) {   /* reply: nothing extra to do; sender cached above */
        return;
    }
    if (oper == 1) {   /* request: answer only when tpa is our own IP */
        int for_us = 1;
        for (int i = 0; i < 4; i++) if (a[24+i] != net_our_ip[i]) { for_us = 0; break; }
        if (!for_us) return;
        uint8_t r[14 + 28];
        for (int i = 0; i < 6; i++) { r[i] = a[8+i]; r[6+i] = net_mac[i]; }
        wr16(r + 12, 0x0806);
        uint8_t *b = r + 14;
        wr16(b + 0, 1);       wr16(b + 2, 0x0800);
        b[4] = 6; b[5] = 4;   wr16(b + 6, 2);                 /* reply */
        for (int i = 0; i < 6; i++) b[8+i] = net_mac[i];
        for (int i = 0; i < 4; i++) b[14+i] = net_our_ip[i];
        for (int i = 0; i < 6; i++) b[18+i] = a[8+i];        /* tha = requester */
        for (int i = 0; i < 4; i++) b[24+i] = a[14+i];        /* tpa = requester's IP */
        net_send(r, sizeof(r));
    }
}

static int arp_resolve(const uint8_t ip[4], uint8_t mac[6]) {
    if (arp_cache_valid) {
        int same = 1;
        for (int i = 0; i < 4; i++) if (arp_cache_ip[i] != ip[i]) { same = 0; break; }
        if (same) { for (int i = 0; i < 6; i++) mac[i] = arp_cache_mac[i]; return 0; }
    }
    for (int tries = 0; tries < 3; tries++) {
        arp_request(ip);
        for (int i = 0; i < 300000; i++) {
            net_poll();
            task_maybe_yield();
            if (arp_cache_valid) {
                int same = 1;
                for (int k = 0; k < 4; k++) if (arp_cache_ip[k] != ip[k]) { same = 0; break; }
                if (same) {
                    for (int k = 0; k < 6; k++) mac[k] = arp_cache_mac[k];
                    return 0;
                }
            }
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* IPv4                                                                */
/* ------------------------------------------------------------------ */

static uint16_t ip_id_counter = 0;

static void ip_send(const uint8_t dst[4], uint8_t proto,
                    const uint8_t *payload, uint32_t plen) {
    uint8_t mac[6];
    /* route: same /24 subnet -> ARP the destination directly; anything
     * else goes via the gateway (10.0.2.2 in QEMU user-net). slirp only
     * proxy-answers ARP for addresses it knows (10.0.2.0/24 + gateway),
     * so ARPing a public IP would just time out. */
    int same24 = (net_our_ip[0]==dst[0] && net_our_ip[1]==dst[1] &&
                  net_our_ip[2]==dst[2]);
    if (same24) {
        if (arp_resolve(dst, mac) != 0) return;
    } else {
        if (arp_resolve(net_gw_ip, mac) != 0) return;
    }
    uint8_t f[14 + 60 + 1460];
    for (int i = 0; i < 6; i++) { f[i] = mac[i]; f[6+i] = net_mac[i]; }
    wr16(f + 12, 0x0800);
    uint8_t *ip = f + 14;
    ip[0] = 0x45; ip[1] = 0;
    wr16(ip + 2, (uint16_t)(20 + plen));
    wr16(ip + 4, ++ip_id_counter);
    wr16(ip + 6, 0);
    ip[8] = 64; ip[9] = proto;
    wr16(ip + 10, 0);                                  /* checksum filled below */
    for (int i = 0; i < 4; i++) ip[12+i] = net_our_ip[i];
    for (int i = 0; i < 4; i++) ip[16+i] = dst[i];
    wr16(ip + 10, ip_checksum(ip, 20));
    for (uint32_t i = 0; i < plen; i++) ip[20+i] = payload[i];
    net_send(f, 14 + 20 + plen);
}

/* ------------------------------------------------------------------ */
/* TCP client                                                          */
/* ------------------------------------------------------------------ */

#define TCP_CLOSED       0
#define TCP_SYN_SENT     1
#define TCP_ESTABLISHED  2
#define TCP_FIN_WAIT     3

static struct {
    int      active;
    uint8_t  peer_ip[4];
    uint16_t local_port;      /* host order */
    uint16_t peer_port;       /* host order */
    uint32_t seq;             /* next seq to send */
    uint32_t rcv_next;        /* next seq we expect */
    int      state;
    int      err;
    uint8_t  rcv[4096];
    int      rcv_len;
    uint8_t  rcv_buf[20480];   /* v2.70: enlarged from 4096 for binary
                                * .catpkg downloads (up to 16 KiB payload
                                * + HTTP headers).  net_http_download()
                                * cannot NUL-terminate, so this must hold
                                * a whole package. */
    int      rcv_buf_len;
} tcp;

/* ---- v2.71 TCP server context: declared before tcp_handle so the
 * RX router can see it; the state machine lives further below. ---- */
#define TS_CLOSED       0
#define TS_SYN_RCVD     1
#define TS_ESTABLISHED  2

static struct {
    int      listening;       /* listener armed */
    uint16_t listen_port;     /* host order */
    int      state;
    uint8_t  peer_ip[4];
    uint16_t peer_port;       /* host order */
    uint32_t seq;             /* next seq we send */
    uint32_t iss;
    uint32_t rcv_next;
    uint8_t  rcv_buf[16384];
    int      rcv_buf_len;
    int      peer_closed;     /* FIN or RST received */
} tcps;

static void tcp_do_send(uint8_t flags, const uint8_t *payload, uint32_t plen) {
    uint8_t seg[20 + 1460];
    wr16(seg + 0,  (uint16_t)tcp.local_port);
    wr16(seg + 2,  (uint16_t)tcp.peer_port);
    wr32(seg + 4,  tcp.seq);
    wr32(seg + 8,  tcp.rcv_next);
    seg[12] = 0x50;                                    /* data offset 5 */
    seg[13] = flags;
    wr16(seg + 14, 4096);                              /* window */
    wr16(seg + 16, 0);                                 /* checksum */
    wr16(seg + 18, 0);
    for (uint32_t i = 0; i < plen; i++) seg[20+i] = payload[i];
    wr16(seg + 16, tcp_checksum(net_our_ip, tcp.peer_ip, seg, 20 + plen));
    ip_send(tcp.peer_ip, 6, seg, 20 + plen);
}

static void tcp_server_handle(const uint8_t *ip, const uint8_t *t,
                              uint8_t flags, uint32_t seq, uint32_t ack,
                              uint32_t seglen);   /* v2.71 fwd decl */

static void tcp_handle(const uint8_t *ip, uint32_t iplen) {
    if (iplen < 20) return;
    uint32_t hlen = (ip[0] & 0x0F) * 4;
    if (iplen < hlen + 20) return;
    const uint8_t *t = ip + hlen;
    uint32_t seglen = iplen - hlen;
    uint16_t dport = rd16(t + 2);
    uint32_t seq  = rd32(t + 4);
    uint32_t ack  = rd32(t + 8);
    uint8_t  flags = t[13];
    /* v2.71: route sshd listener traffic to the server state machine */
    if (tcps.listening && dport == tcps.listen_port) {
        tcp_server_handle(ip, t, flags, seq, ack, seglen);
        return;
    }
    if (!tcp.active || dport != tcp.local_port) return;

    if (tcp.state == TCP_SYN_SENT) {
        if ((flags & 0x12) == 0x12) {                  /* SYN+ACK */
            tcp.rcv_next = seq + 1;
            tcp.seq = ack;
            tcp.state = TCP_ESTABLISHED;
            tcp_do_send(0x10, 0, 0);                   /* ACK */
        }
        return;
    }
    if (tcp.state == TCP_ESTABLISHED) {
        if (seglen > 20) {
            uint32_t plen = seglen - 20;
            if ((uint32_t)tcp.rcv_buf_len + plen > sizeof(tcp.rcv_buf)) plen = sizeof(tcp.rcv_buf) - tcp.rcv_buf_len;
            for (uint32_t i = 0; i < plen; i++) tcp.rcv_buf[tcp.rcv_buf_len + i] = t[20+i];
            tcp.rcv_buf_len += (int)plen;
            /* ACK only what we actually stored: if the buffer was full the
             * sender retransmits later (real flow control), instead of the
             * old bug where we ACKed bytes we had silently dropped. */
            tcp.rcv_next = seq + plen;
            tcp_do_send(0x10, 0, 0);                   /* ACK the data */
        }
        if (flags & 0x01) {                            /* FIN */
            tcp.rcv_next = seq + seglen - 20 + 1;
            tcp_do_send(0x10, 0, 0);                   /* ACK FIN */
            tcp.state = TCP_FIN_WAIT;
            tcp.active = 0;
        }
        return;
    }
}

/* ------------------------------------------------------------------ */
/* TCP server (v2.71): one passive connection for the sshd command.   */
/* Separate context from the client `tcp` struct; net_rx_dispatch     */
/* routes by destination port.  Minimal handshake + data + FIN, same  */
/* level of sophistication as the client side (lossless QEMU/VMnet).  */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* TCP server (v2.71): one passive connection for the sshd command.   */
/* (context struct `tcps` is declared up top, before tcp_handle)      */
/* ------------------------------------------------------------------ */

static void tcps_send_seg(uint8_t flags, const uint8_t *payload, uint32_t plen) {
    uint8_t seg[20 + 1460];
    wr16(seg + 0,  (uint16_t)tcps.listen_port);   /* our source port */
    wr16(seg + 2,  tcps.peer_port);
    wr32(seg + 4,  tcps.seq);
    wr32(seg + 8,  tcps.rcv_next);
    seg[12] = 0x50;
    seg[13] = flags;
    wr16(seg + 14, 4096);
    wr16(seg + 16, 0);
    wr16(seg + 18, 0);
    for (uint32_t i = 0; i < plen; i++) seg[20+i] = payload[i];
    wr16(seg + 16, tcp_checksum(net_our_ip, tcps.peer_ip, seg, 20 + plen));
    ip_send(tcps.peer_ip, 6, seg, 20 + plen);
}

static void tcp_server_handle(const uint8_t *ip, const uint8_t *t,
                              uint8_t flags, uint32_t seq, uint32_t ack,
                              uint32_t seglen) {
    uint32_t plen = (seglen > 20) ? seglen - 20 : 0;
    if (tcps.state == TS_CLOSED) {
        if ((flags & 0x02) && !(flags & 0x04)) {          /* SYN */
            for (int i = 0; i < 4; i++) tcps.peer_ip[i] = ip[12+i];
            tcps.peer_port = rd16(t + 0);
            tcps.rcv_next = seq + 1;
            tcps.iss = 0x20000000u + (uint32_t)(tcp.local_port << 8)
                       + (uint32_t)ip_id_counter * 7919u;
            tcps.seq = tcps.iss;
            tcps.rcv_buf_len = 0;
            tcps.peer_closed = 0;
            tcps_send_seg(0x12, 0, 0);                    /* SYN+ACK */
            tcps.state = TS_SYN_RCVD;
        }
        return;
    }
    if (tcps.state == TS_SYN_RCVD) {
        if ((flags & 0x10) && ack == tcps.iss + 1) {      /* handshake ACK */
            tcps.seq = tcps.iss + 1;
            tcps.state = TS_ESTABLISHED;
            /* fall through: the client's ACK often CARRIES the first
             * data segment (e.g. the SSH banner).  Dropping it here
             * deadlocks both sides (server waits for banner the client
             * already sent). */
        } else {
            return;
        }
    }
    /* TS_ESTABLISHED */
    if (flags & 0x04) {                                   /* RST */
        tcps.state = TS_CLOSED;
        tcps.peer_closed = 1;
        return;
    }
    if (plen > 0) {
        uint32_t n = plen;
        if ((uint32_t)tcps.rcv_buf_len + n > sizeof(tcps.rcv_buf))
            n = sizeof(tcps.rcv_buf) - tcps.rcv_buf_len;
        for (uint32_t i = 0; i < n; i++) tcps.rcv_buf[tcps.rcv_buf_len + i] = t[20+i];
        tcps.rcv_buf_len += (int)n;
        tcps.rcv_next = seq + n;
        tcps_send_seg(0x10, 0, 0);                        /* ACK the data */
    }
    if (flags & 0x01) {                                   /* FIN */
        tcps.rcv_next = seq + plen + 1;
        tcps_send_seg(0x10, 0, 0);
        tcps.peer_closed = 1;
    }
}

/* ---- server-side public API (net.h) ---- */

int net_tcp_listen(uint16_t port) {
    tcps.listening = 1;
    tcps.listen_port = port;
    tcps.state = TS_CLOSED;
    return 0;
}

int net_tcp_saccept(void) {
    /* reset any previous session state */
    tcps.state = TS_CLOSED;
    tcps.rcv_buf_len = 0;
    tcps.peer_closed = 0;
    for (long i = 0; i < 3000000L; i++) {     /* ~2s idle window (sshd ESC check) */
        net_poll();
        task_maybe_yield();
        if (tcps.state == TS_ESTABLISHED) return 0;
    }
    return -1;
}

int net_tcp_ssend(const uint8_t *data, uint32_t len) {
    if (tcps.state != TS_ESTABLISHED) return -1;
    while (len > 0) {
        uint32_t n = (len > 1400) ? 1400 : len;
        tcps_send_seg(0x18, data, n);                     /* PSH|ACK */
        tcps.seq += n;
        data += n;
        len -= n;
    }
    return 0;
}

/* non-blocking drain: returns bytes copied, 0 = nothing, -1 = peer closed */
int net_tcp_srecv(uint8_t *buf, int max) {
    if (tcps.rcv_buf_len == 0) return tcps.peer_closed ? -1 : 0;
    int n = tcps.rcv_buf_len;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) buf[i] = tcps.rcv_buf[i];
    /* compact the remainder */
    for (int i = n; i < tcps.rcv_buf_len; i++)
        tcps.rcv_buf[i - n] = tcps.rcv_buf[i];
    tcps.rcv_buf_len -= n;
    return n;
}

/* blocking exact-n read with net polling; -1 on peer close or ~30s idle */
int net_tcp_srecv_all(uint8_t *buf, int n) {
    int got = 0;
    long idle = 0;
    while (got < n) {
        int r = net_tcp_srecv(buf + got, n - got);
        if (r < 0) return -1;
        if (r == 0) {
            net_poll();
            task_maybe_yield();
            if (++idle > 30000000L) return -1;   /* no data for ~30s */
        } else {
            idle = 0;
            got += r;
        }
    }
    return got;
}

void net_tcp_sclose(void) {
    if (tcps.state == TS_ESTABLISHED) {
        tcps_send_seg(0x11, 0, 0);                        /* FIN|ACK */
        tcps.seq += 1;
        /* drain peer's FIN/ACK briefly */
        for (long i = 0; i < 300000L && tcps.state == TS_ESTABLISHED; i++) {
            net_poll();
            task_maybe_yield();
        }
    }
    tcps.state = TS_CLOSED;
    tcps.listening = 0;
}

/* ------------------------------------------------------------------ */
/* UDP (client): one-shot receive buffer, DNS + echo-style usage       */
/* ------------------------------------------------------------------ */

int net_dns_lookup(const char *host, uint8_t ip[4]);   /* forward decl (defined below) */
void net_dhcp_ensure(void);                            /* forward decl (defined below) */
void net_arp_flush(void);                              /* forward decl (defined below) */
void net_dns_server_set(const uint8_t ip[4]);          /* forward decl (defined below) */

static uint16_t udp_local_port = 0xC000;          /* ephemeral source port */
static uint8_t  udp_rx_data[2048];
static uint32_t udp_rx_len  = 0;
static uint16_t udp_rx_sport = 0;
static uint16_t udp_rx_dport = 0;
static uint8_t  udp_rx_src[4];

static void udp_send_pkt(const uint8_t dst[4], uint16_t dport, uint16_t sport,
                         const uint8_t *payload, uint32_t plen) {
    uint8_t seg[8 + 1460];
    wr16(seg + 0, sport);
    wr16(seg + 2, dport);
    wr16(seg + 4, (uint16_t)(8 + plen));
    wr16(seg + 6, 0);
    for (uint32_t i = 0; i < plen && i < 1460; i++) seg[8 + i] = payload[i];
    /* UDP checksum over pseudo header + datagram */
    uint32_t sum = 0;
    for (int i = 0; i < 4; i += 2) sum += (uint16_t)((net_our_ip[i] << 8) | net_our_ip[i+1]);
    for (int i = 0; i < 4; i += 2) sum += (uint16_t)((dst[i] << 8) | dst[i+1]);
    sum += 17;
    sum += (uint32_t)(8 + plen);
    const uint8_t *p = seg;
    uint32_t n = 8 + plen;
    while (n > 1) { sum += (uint16_t)((p[0] << 8) | p[1]); p += 2; n -= 2; }
    if (n) sum += (uint16_t)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    uint16_t ck = (uint16_t)~sum;
    if (ck == 0) ck = 0xFFFF;
    wr16(seg + 6, ck);
    ip_send(dst, 17, seg, 8 + plen);
}

static void udp_handle(const uint8_t *iph, const uint8_t *seg, uint32_t seglen) {
    if (seglen < 8) return;
    uint32_t ulen = rd16(seg + 4);
    if (ulen < 8 || ulen > seglen) ulen = seglen;
    /* verify the UDP checksum when the sender computed one (0 = none):
     * pseudo header (src, dst, 0, 17, udp length) + the datagram itself */
    if (rd16(seg + 6) != 0) {
        static uint8_t pseudo[12 + 8 + 2048];   /* kernel is non-preemptive: shared scratch is safe */
        uint32_t o = 0;
        for (int i = 0; i < 4; i++) pseudo[o++] = iph[12 + i];
        for (int i = 0; i < 4; i++) pseudo[o++] = iph[16 + i];
        pseudo[o++] = 0; pseudo[o++] = 17;
        pseudo[o++] = (uint8_t)(ulen >> 8); pseudo[o++] = (uint8_t)ulen;
        for (uint32_t i = 0; i < ulen && i < 2048; i++) pseudo[o++] = seg[i];
        /* odd trailing byte pads with zero (pseudo[] was not zeroed...) */
        if (ulen & 1) pseudo[o++] = 0;
        if (ip_checksum(pseudo, o) != 0) return;   /* corrupt: drop */
    }
    uint32_t plen = ulen - 8;
    if (plen > sizeof(udp_rx_data)) plen = sizeof(udp_rx_data);
    for (int i = 0; i < 4; i++) udp_rx_src[i] = iph[12+i];
    udp_rx_sport = rd16(seg + 0);
    udp_rx_dport = rd16(seg + 2);
    udp_rx_len = plen;
    for (uint32_t i = 0; i < plen; i++) udp_rx_data[i] = seg[8+i];
}

/* public (CE API): send a UDP datagram to "ip:port" (ip may be a hostname) */
int net_udp_send(const char *ip, uint16_t port, const uint8_t *data, uint32_t len) {
    if (!net_ready || !ip || !data || len == 0 || len > 1460) return -1;
    net_dhcp_ensure();                      /* v2.66: first-use DHCP */
    uint8_t dst[4];
    if (ip_parse(ip, dst) != 0) {
        if (net_dns_lookup(ip, dst) != 0) return -1;
    }
    udp_send_pkt(dst, port, udp_local_port, data, len);
    return 0;
}

/* public (CE API): pick up one received UDP datagram (one-shot).
 * ip_out/port_out optional. Returns payload bytes, 0 if nothing arrived,
 * -1 on bad args. */
int net_udp_recv(char *ip_out, int *port_out, uint8_t *buf, int max) {
    if (!net_ready || !buf || max <= 0) return -1;
    if (udp_rx_len == 0) return 0;
    uint32_t n = udp_rx_len;
    if (n > (uint32_t)max) n = (uint32_t)max;
    for (uint32_t i = 0; i < n; i++) buf[i] = udp_rx_data[i];
    if (ip_out) {
        char *o = ip_out;
        for (int i = 0; i < 4; i++) {
            int d = udp_rx_src[i];
            char t[4]; int ti = 0;
            if (d == 0) t[ti++] = '0';
            while (d > 0) { t[ti++] = (char)('0' + d % 10); d /= 10; }
            while (ti > 0) *o++ = t[--ti];
            if (i < 3) *o++ = '.';
        }
        *o = 0;
    }
    if (port_out) *port_out = (int)udp_rx_sport;
    udp_rx_len = 0;
    return (int)n;
}

/* ------------------------------------------------------------------ */
/* DHCP client (v2.66)                                                 */
/* VMware NAT (VMnet8) hands out host-specific subnets (typically      */
/* 192.168.x.0/24) and does NOT proxy-answer ARP for the hardcoded     */
/* QEMU-slirp range 10.0.2.x, so the static defaults are dead there.   */
/* A minimal DISCOVER -> OFFER -> REQUEST -> ACK over broadcast raw    */
/* frames fixes VMware without changing QEMU: slirp's built-in DHCP    */
/* server answers with exactly the old static values (10.0.2.15 etc).  */
/* ------------------------------------------------------------------ */

static int dhcp_tried = 0;          /* one lazy attempt per boot */
static int dhcp_ok   = 0;           /* last attempt succeeded */
static int dhcp_user_cfg = 0;       /* user ran 'netcfg IP GW' -> keep it */

static uint32_t dhcp_xid(void) {
    return 0xC0DE0000u | ((uint32_t)net_mac[2] << 16) |
           ((uint32_t)net_mac[4] << 8) | (uint32_t)net_mac[5];
}

/* Build Ethernet + IPv4(broadcast) + UDP(68->67) + DHCP into f[]. */
static uint32_t dhcp_frame(uint8_t *f, uint32_t xid, uint8_t msg_type,
                           const uint8_t *extra, uint32_t extra_len) {
    static const uint8_t bcast_mac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    static const uint8_t bcast_ip[4]  = {255,255,255,255};
    static const uint8_t zero_ip[4]   = {0,0,0,0};
    for (int i = 0; i < 6; i++) f[i] = bcast_mac[i];
    for (int i = 0; i < 6; i++) f[6+i] = net_mac[i];
    wr16(f + 12, 0x0800);
    uint8_t *ip = f + 14;
    uint8_t *u  = ip + 20;
    uint8_t *d  = u + 8;
    uint32_t o = 0;
    d[o++] = 1; d[o++] = 1; d[o++] = 6; d[o++] = 0;   /* BOOTREQUEST/eth/hlen */
    wr32(d + o, xid); o += 4;
    wr16(d + o, 0);    o += 2;                        /* secs */
    wr16(d + o, 0x8000); o += 2;                      /* flags: broadcast */
    for (int i = 0; i < 16; i++) d[o++] = 0;          /* ci/yi/si/gi addr */
    for (int i = 0; i < 16; i++) d[o++] = (i < 6) ? net_mac[i] : 0;
    for (int i = 0; i < 64; i++) d[o++] = 0;          /* sname */
    for (int i = 0; i < 128; i++) d[o++] = 0;         /* file */
    d[o++] = 0x63; d[o++] = 0x82; d[o++] = 0x53; d[o++] = 0x63;   /* magic */
    d[o++] = 53; d[o++] = 1; d[o++] = msg_type;
    for (uint32_t i = 0; i < extra_len && o < 340; i++) d[o++] = extra[i];
    d[o++] = 255;                                     /* end option */
    while (o < 300) d[o++] = 0;                       /* pad to minimum size */
    uint32_t dlen = o;
    wr16(u + 0, 68); wr16(u + 2, 67);
    wr16(u + 4, (uint16_t)(8 + dlen));
    wr16(u + 6, 0);                                   /* UDP checksum optional */
    ip[0] = 0x45; ip[1] = 0;
    wr16(ip + 2, (uint16_t)(20 + 8 + dlen));
    wr16(ip + 4, ++ip_id_counter);
    wr16(ip + 6, 0);
    ip[8] = 64; ip[9] = 17;
    wr16(ip + 10, 0);
    for (int i = 0; i < 4; i++) ip[12+i] = zero_ip[i];
    for (int i = 0; i < 4; i++) ip[16+i] = bcast_ip[i];
    wr16(ip + 10, ip_checksum(ip, 20));
    return 14 + 20 + 8 + dlen;
}

/* Take one DHCP reply off the one-shot UDP receive buffer (the normal
 * udp_handle demux already copied it there).  Returns option-53 type
 * (2=OFFER, 5=ACK, 6=NAK) or 0 when nothing usable arrived. */
static int dhcp_poll_reply(uint32_t xid, uint8_t yi[4], uint8_t srv[4],
                           uint8_t gw[4], uint8_t dns[4]) {
    if (udp_rx_len == 0) return 0;
    if (udp_rx_dport != 68) { udp_rx_len = 0; return 0; }
    const uint8_t *d = udp_rx_data;
    if (udp_rx_len < 240 || rd32(d + 4) != xid ||
        d[236] != 0x63 || d[237] != 0x82 || d[238] != 0x53 || d[239] != 0x63) {
        udp_rx_len = 0; return 0;
    }
    int type = 0;
    for (int i = 0; i < 4; i++) { yi[i] = d[16+i]; srv[i] = gw[i] = dns[i] = 0; }
    uint32_t o = 240;
    while (o + 2 <= udp_rx_len) {
        uint8_t opt = d[o];
        if (opt == 0) { o++; continue; }              /* pad */
        if (opt == 255) break;                        /* end */
        uint8_t l = d[o + 1];
        if (o + 2 + (uint32_t)l > udp_rx_len) break;
        if      (opt == 53 && l >= 1) type = d[o+2];
        else if (opt == 54 && l >= 4) for (int i = 0; i < 4; i++) srv[i] = d[o+2+i];
        else if (opt == 3  && l >= 4) for (int i = 0; i < 4; i++) gw[i]  = d[o+2+i];
        else if (opt == 6  && l >= 4) for (int i = 0; i < 4; i++) dns[i] = d[o+2+i];
        o += 2 + l;
    }
    udp_rx_len = 0;
    return type;
}

/* one DISCOVER -> OFFER -> REQUEST -> ACK cycle; 0 = configured, -1 = no */
int net_dhcp_once(void) {
    if (!net_ready) return -1;
    uint32_t xid = dhcp_xid();
    uint8_t f[576];
    uint8_t yi[4], srv[4], gw[4], dns[4];
    uint8_t extra[16];

    /* DISCOVER (param request list: 1 mask, 3 router, 6 dns) */
    extra[0] = 55; extra[1] = 3; extra[2] = 1; extra[3] = 3; extra[4] = 6;
    net_send(f, dhcp_frame(f, xid, 1, extra, 5));
    int have_offer = 0;
    for (long i = 0; i < 1500000L; i++) {
        net_poll();
        int t = dhcp_poll_reply(xid, yi, srv, gw, dns);
        if (t == 2 || t == 5) { have_offer = 1; break; }   /* some skip OFFER */
    }
    if (!have_offer) return -1;

    /* claim the offered address before REQUEST so a unicast ACK finds us */
    for (int i = 0; i < 4; i++) net_our_ip[i] = yi[i];
    net_arp_flush();

    /* REQUEST: server identifier (54) + requested IP (50) */
    extra[0] = 54; extra[1] = 4;
    for (int i = 0; i < 4; i++) extra[2+i] = srv[i];
    extra[6] = 50; extra[7] = 4;
    for (int i = 0; i < 4; i++) extra[8+i] = yi[i];
    net_send(f, dhcp_frame(f, xid, 3, extra, 12));
    int got_ack = 0;
    for (long i = 0; i < 1500000L; i++) {
        net_poll();
        int t = dhcp_poll_reply(xid, yi, srv, gw, dns);
        if (t == 5) { got_ack = 1; break; }
        if (t == 6) break;                                 /* NAK */
    }
    if (!got_ack) return -1;
    for (int i = 0; i < 4; i++) net_our_ip[i] = yi[i];
    for (int i = 0; i < 4; i++) net_gw_ip[i] = (gw[0] || gw[1] || gw[2] || gw[3])
                                             ? gw[i] : srv[i];
    if (dns[0] || dns[1] || dns[2] || dns[3]) net_dns_server_set(dns);
    net_arp_flush();
    dhcp_ok = 1;
    return 0;
}

/* Lazy one-shot before the first real network use.  Skipped when the
 * user configured a static address with 'netcfg IP GW'. */
void net_dhcp_ensure(void) {
    if (dhcp_tried || dhcp_user_cfg) return;
    dhcp_tried = 1;
    net_dhcp_once();
}

/* 'netcfg dhcp': force a fresh discover, even after a failed one */
void net_dhcp_retry(void) {
    dhcp_tried = 1;
    net_dhcp_once();
}

/* netcfg static: mark user configuration so DHCP never overrides it */
void net_cfg_user_set(void) { dhcp_user_cfg = 1; dhcp_ok = 0; }

int net_dhcp_ok(void) { return dhcp_ok; }

/* ------------------------------------------------------------------ */
/* DNS client (QEMU slirp resolver at 10.0.2.3, UDP :53)              */
/* ------------------------------------------------------------------ */

static uint8_t dns_server_v[4] = {10, 0, 2, 3};  /* netcfg-settable */
static char    dns_cache_name[64];
static uint8_t dns_cache_ip[4];
static int     dns_cache_valid = 0;

static void dns_build_qname(uint8_t *q, uint32_t *out_len, const char *name) {
    uint32_t o = 0;
    while (*name) {
        const char *dot = name;
        while (*dot && *dot != '.') dot++;
        uint32_t l = (uint32_t)(dot - name);
        q[o++] = (uint8_t)l;
        for (uint32_t i = 0; i < l; i++) q[o++] = (uint8_t)name[i];
        name = dot;
        if (*name == '.') name++;
    }
    q[o++] = 0;
    *out_len = o;
}

/* resolve a hostname to an IPv4 address (caches the last result).
 * Accepts dotted-quad input as-is. Returns 0 + fills ip, or -1. */
int net_dns_lookup(const char *host, uint8_t ip[4]) {
    if (!host || !ip || !*host) return -1;
    if (ip_parse(host, ip) == 0) return 0;                 /* already an IP */
    net_dhcp_ensure();                      /* v2.66: first-use DHCP */
    if (dns_cache_valid) {
        int same = 1, i = 0;
        for (i = 0; host[i] && dns_cache_name[i]; i++)
            if (host[i] != dns_cache_name[i]) { same = 0; break; }
        if (same && host[i] == 0 && dns_cache_name[i] == 0) {
            for (int k = 0; k < 4; k++) ip[k] = dns_cache_ip[k];
            return 0;
        }
    }
    if (!net_ready) return -1;

    uint8_t q[512];
    uint16_t id = (uint16_t)(0x4000u + ((uint32_t)net_mac[4] << 4) + (uint16_t)net_mac[5]);
    wr16(q + 0, id);
    wr16(q + 2, 0x0100);                    /* RD */
    wr16(q + 4, 1);                         /* QDCOUNT=1 */
    wr16(q + 6, 0); wr16(q + 8, 0); wr16(q + 10, 0);
    uint32_t qn = 0;
    dns_build_qname(q + 12, &qn, host);
    wr16(q + 12 + qn, 1);                   /* QTYPE=A */
    wr16(q + 12 + qn + 2, 1);               /* QCLASS=IN */
    uint32_t qlen = 12 + qn + 4;
    const uint8_t *dns_ip = dns_server_v;

    for (int try = 0; try < 3; try++) {
        udp_local_port++;
        udp_send_pkt(dns_ip, 53, udp_local_port, q, qlen);
        for (long i = 0; i < 1500000L; i++) {
            net_poll();
            if (udp_rx_len == 0) continue;
            if (udp_rx_dport != udp_local_port) { udp_rx_len = 0; continue; }
            const uint8_t *r = udp_rx_data;
            if (udp_rx_len < 12 || rd16(r + 0) != id) { udp_rx_len = 0; continue; }
            uint16_t ancount = rd16(r + 6);
            if (ancount == 0) { udp_rx_len = 0; return -1; }   /* NXDOMAIN */
            uint32_t off = 12;
            int q_was_ptr = 0;
            /* skip question: qname (possibly compressed) + qtype/qclass.
             * after a compression pointer there is NO root label, so the
             * skip is 4 (type+class), not 5 - the old +5 mis-parsed every
             * response whose question name was compressed */
            while (off < udp_rx_len && r[off] != 0) {
                if ((r[off] & 0xC0) == 0xC0) { off += 2; q_was_ptr = 1; break; }
                off += 1 + r[off];
            }
            off += q_was_ptr ? 4 : 5;        /* [+root label] + qtype + qclass */
            for (uint16_t a = 0; a < ancount && off + 12 <= udp_rx_len; a++) {
                /* answer name: compression pointer or plain name */
                if (off + 2 <= udp_rx_len && (r[off] & 0xC0) == 0xC0) off += 2;
                else {
                    while (off < udp_rx_len && r[off]) {
                        if ((r[off] & 0xC0) == 0xC0) { off += 2; break; }
                        off += 1 + r[off];
                    }
                    if (off < udp_rx_len) off++;
                }
                if (off + 10 > udp_rx_len) break;
                uint16_t type  = rd16(r + off + 0);
                uint16_t rdlen = rd16(r + off + 8);
                if (type == 1 && rdlen == 4 && off + 14 <= udp_rx_len) {
                    ip[0] = r[off+10]; ip[1] = r[off+11];
                    ip[2] = r[off+12]; ip[3] = r[off+13];
                    int ci = 0;
                    while (host[ci] && ci < 63) { dns_cache_name[ci] = host[ci]; ci++; }
                    dns_cache_name[ci] = 0;
                    for (int k = 0; k < 4; k++) dns_cache_ip[k] = ip[k];
                    dns_cache_valid = 1;
                    udp_rx_len = 0;
                    return 0;
                }
                off += 10 + rdlen;
            }
            udp_rx_len = 0;
            return -1;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* ICMP echo (ping)                                                   */
/* ------------------------------------------------------------------ */

static uint8_t  ping_reply_data[8];
static int      ping_reply_ok = 0;
static uint16_t ping_want_id = 0;
static uint16_t ping_want_seq = 0;

static void icmp_handle(const uint8_t *iph, const uint8_t *icmp, uint32_t len) {
    if (len < 8) return;
    if (icmp[0] != 0) return;               /* echo reply only */
    uint16_t id  = rd16(icmp + 4);
    uint16_t seq = rd16(icmp + 6);
    if (id != ping_want_id || seq != ping_want_seq) return;
    for (int i = 0; i < 8; i++) ping_reply_data[i] = icmp[i];
    ping_reply_ok = 1;
}

/* public (CE API / shell): one ICMP echo request to ip (or hostname).
 * Returns 0 on reply, -1 on failure. */
int net_ping(const char *ipstr) {
    if (!net_ready || !ipstr) return -1;
    net_dhcp_ensure();                      /* v2.66: first-use DHCP */
    uint8_t dst[4];
    if (ip_parse(ipstr, dst) != 0) {
        if (net_dns_lookup(ipstr, dst) != 0) return -1;
    }
    uint8_t icmp[8];
    ping_want_id  = (uint16_t)(0x4000u + ((uint32_t)net_mac[5] << 3) + (uint16_t)net_mac[4]);
    ping_want_seq = 1;
    icmp[0] = 8; icmp[1] = 0;
    wr16(icmp + 2, 0);
    wr16(icmp + 4, ping_want_id);
    wr16(icmp + 6, ping_want_seq);
    uint32_t sum = 0;
    for (int i = 0; i < 8; i += 2) sum += (uint16_t)((icmp[i] << 8) | icmp[i+1]);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    wr16(icmp + 2, (uint16_t)~sum);
    ping_reply_ok = 0;
    ip_send(dst, 1, icmp, 8);
    for (long i = 0; i < 1500000L; i++) {
        net_poll();
        if (ping_reply_ok) return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* frame demux                                                        */
/* ------------------------------------------------------------------ */

void net_rx_dispatch(const uint8_t *frame, uint32_t len) {
    if (len < 14) return;
    uint16_t type = rd16(frame + 12);
    if (type == 0x0806) { arp_handle(frame, len); return; }
    if (type == 0x0800) {
        const uint8_t *ip = frame + 14;
        /* use the IP total-length field, not the padded Ethernet frame size */
        uint32_t iplen = rd16(ip + 2);
        if (iplen < 20 || iplen > len - 14) iplen = len - 14;
        if (iplen < 20) return;
        if ((ip[0] & 0xF0) != 0x40) return;
        {   /* v2.66: dst must be us OR the IPv4 broadcast address, which
             * DHCP OFFER/ACK replies use before we have a lease */
            int bcast = (ip[16] == 255 && ip[17] == 255 &&
                         ip[18] == 255 && ip[19] == 255);
            if (!bcast)
                for (int i = 0; i < 4; i++) if (ip[16+i] != net_our_ip[i]) return;
        }
        uint32_t hlen = (ip[0] & 0x0F) * 4;
        /* verify the IP header checksum: a correct header (checksum
         * included) sums to 0 - corrupt frames are dropped instead of
         * being parsed (TX always computed them, RX never checked) */
        if (hlen < 20 || hlen > iplen) return;
        if (ip_checksum(ip, hlen) != 0) return;
        if (ip[9] == 6) tcp_handle(ip, iplen);
        else if (ip[9] == 17 && iplen >= hlen + 8) udp_handle(ip, ip + hlen, iplen - hlen);
        else if (ip[9] == 1  && iplen >= hlen + 8) icmp_handle(ip, ip + hlen, iplen - hlen);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* HTTP GET                                                           */
/* ------------------------------------------------------------------ */

static int http_parse_url(const char *url, uint8_t ip[4], uint16_t *port,
                          const char **path) {
    *port = 80;
    if (url[0]=='h' && url[1]=='t' && url[2]=='t' && url[3]=='p' &&
        url[4]==':' && url[5]=='/' && url[6]=='/') url += 7;
    else return -1;
    char host[128];
    int hi = 0;
    while (*url && *url != ':' && *url != '/' && hi < 127) host[hi++] = *url++;
    host[hi] = 0;
    if (*url == ':') {
        url++;
        int p = 0;
        while (*url >= '0' && *url <= '9') { p = p * 10 + (*url - '0'); url++; }
        if (p == 0 || p > 65535) return -1;
        *port = (uint16_t)p;
    }
    if (*url == '/') *path = url;
    else *path = "/";
    if (ip_parse(host, ip) != 0) {
        /* hostname: resolve via the UDP DNS client (10.0.2.3 in QEMU) */
        if (net_dns_lookup(host, ip) != 0) return -2;
    }
    return 0;
}

/* v2.70: shared HTTP GET core.  Fetches the response body into `out` up to
 * `max` bytes, returns the exact body length on success (no NUL appended)
 * or a negative error code.  Both net_http_get() (text, NUL-terminated) and
 * net_http_download() (binary-safe, verbatim) wrap this. */
static int http_fetch(const char *url, uint8_t *out, int max) {
    if (!net_ready || !url || !out || max <= 0) return -1;
    net_dhcp_ensure();                      /* v2.66: first-use DHCP */

    uint8_t peer[4];
    uint16_t port;
    const char *path;
    int r = http_parse_url(url, peer, &port, &path);
    if (r != 0) return r;                               /* -2 = hostname unsupported */

    /* reset connection */
    tcp.active = 0;
    tcp.rcv_buf_len = 0;
    for (int i = 0; i < 4; i++) tcp.peer_ip[i] = peer[i];
    tcp.local_port = (uint16_t)(40000 + (ip_id_counter & 0x3FFF));
    tcp.peer_port  = port;
    tcp.seq  = 0x04000000u + (uint32_t)((uint32_t)net_mac[4] << 16) + (uint32_t)net_mac[5] * 0x10001u;
    tcp.rcv_next = 0;
    tcp.state = TCP_SYN_SENT;
    tcp.active = 1;
    tcp.err = 0;

    /* SYN */
    tcp_do_send(0x02, 0, 0);

    /* wait for SYN+ACK (poll) */
    for (long i = 0; i < 3000000L && tcp.state == TCP_SYN_SENT; i++) {
        net_poll();
        task_maybe_yield();
        if (tcp.err) break;
    }
    if (tcp.state != TCP_ESTABLISHED) return -3;

    /* build and send GET request (HTTP/1.0: server closes after reply) */
    {
        uint8_t req[512];
        int n = 0;
        const char *m = "GET ";
        while (*m) req[n++] = (uint8_t)*m++;
        const char *p = path;
        while (*p && n < 500) req[n++] = (uint8_t)*p++;
        const char *v = " HTTP/1.0\r\nHost: ";
        while (*v) req[n++] = (uint8_t)*v++;
        /* Host header: use the hostname from the URL (IP for IP URLs).
         * IP-only Host headers get 403 from name-based vhosts (e.g. Cloudflare). */
        const char *hp = url + 7;                  /* skip "http://" */
        while (*hp && *hp != ':' && *hp != '/' && n < 505) req[n++] = (uint8_t)*hp++;
        if (port != 80) {
            req[n++] = ':';
            int d = port; char t[8]; int ti = 0;
            while (d > 0) { t[ti++] = (char)('0' + d % 10); d /= 10; }
            while (ti > 0) req[n++] = (uint8_t)t[--ti];
        }
        const char *v2 = "\r\nUser-Agent: CatOS-curl/1.0\r\nConnection: close\r\n\r\n";
        while (*v2 && n < 510) req[n++] = (uint8_t)*v2++;
        tcp_do_send(0x18, req, (uint32_t)n);            /* PSH|ACK */
    }

    /* receive until FIN (HTTP/1.0 + Connection: close) or timeout.
     * Local python http.server closes cleanly (FIN), so waiting for FIN
     * is correct; the buffer-full guard keeps us from spinning forever
     * against keep-alive servers. */
    for (long i = 0; i < 15000000L; i++) {
        net_poll();
        task_maybe_yield();
        if (!tcp.active) break;                          /* FIN received */
        if (tcp.rcv_buf_len >= (int)sizeof(tcp.rcv_buf)) break;   /* full */
    }
    if (tcp.rcv_buf_len <= 0) return -5;

    /* parse response header: status line + header/body split */
    int got_header = 0, code = 0, header_end = -1;
    int content_len = -1;
    int body_start = -1;
    /* first line: HTTP/1.x CODE */
    for (int i = 0; i < tcp.rcv_buf_len - 2; i++) {
        if (tcp.rcv_buf[i] == '\r' && tcp.rcv_buf[i+1] == '\n') {
            if (i >= 9 && tcp.rcv_buf[0]=='H' && tcp.rcv_buf[4]=='/' &&
                (tcp.rcv_buf[5]=='1')) {
                code = (tcp.rcv_buf[9]-'0')*100 +
                       (tcp.rcv_buf[10]-'0')*10 + (tcp.rcv_buf[11]-'0');
                got_header = 1;
            }
            break;
        }
    }
    /* scan headers for \r\n\r\n and Content-Length */
    for (int i = 0; i < tcp.rcv_buf_len - 3; i++) {
        if (tcp.rcv_buf[i]=='\r' && tcp.rcv_buf[i+1]=='\n' &&
            tcp.rcv_buf[i+2]=='\r' && tcp.rcv_buf[i+3]=='\n') {
            header_end = i + 3;                          /* position of blank line */
            body_start = i + 4;
            break;
        }
    }
    /* Content-Length: scan */
    {
        const char cl[] = "Content-Length:";
        for (int i = 0; i < tcp.rcv_buf_len - 15; i++) {
            int ok = 1;
            for (int k = 0; k < 15; k++)
                if ((char)tcp.rcv_buf[i+k] != cl[k]) { ok = 0; break; }
            if (ok) {
                int j = i + 15;
                while (j < tcp.rcv_buf_len && (tcp.rcv_buf[j]==' '||tcp.rcv_buf[j]=='\t')) j++;
                int v = 0;
                while (j < tcp.rcv_buf_len && tcp.rcv_buf[j] >= '0' && tcp.rcv_buf[j] <= '9') {
                    v = v * 10 + (tcp.rcv_buf[j] - '0'); j++;
                }
                content_len = v;
                break;
            }
        }
    }
    if (!got_header || code < 200 || code >= 300) return -4;
    if (body_start < 0) body_start = header_end >= 0 ? header_end + 1 : 0;

    int avail = tcp.rcv_buf_len - body_start;
    if (avail < 0) avail = 0;
    if (content_len >= 0 && avail > content_len) avail = content_len;
    if (avail > max) avail = max;
    for (int i = 0; i < avail; i++) out[i] = tcp.rcv_buf[body_start + i];
    return avail;                                /* exact byte count, no NUL */
}

/* v2.70: text HTTP GET for `curl`.  NUL-terminates the body. */
int net_http_get(const char *url, char *out, int max) {
    if (max <= 0) return -1;
    int n = http_fetch(url, (uint8_t *)out, max - 1);
    if (n < 0) return n;
    out[n] = 0;
    return n;
}

/* v2.70: binary-safe HTTP GET for .catpkg downloads.  Copies the body
 * verbatim (no NUL, zero bytes preserved) and returns the exact length.
 * Negative return values are the shared http_fetch() error codes. */
int net_http_download(const char *url, uint8_t *out, int max) {
    return http_fetch(url, out, max);
}

/* netcfg hook: change the DNS resolver (e.g. for VMware NAT) */
void net_dns_server_set(const uint8_t ip[4]) {
    for (int i = 0; i < 4; i++) dns_server_v[i] = ip[i];
}
const uint8_t *net_dns_server_get(void) { return dns_server_v; }

void net_arp_flush(void) { arp_cache_valid = 0; }
