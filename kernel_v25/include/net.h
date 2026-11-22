#ifndef NET_H
#define NET_H

#include <stdint.h>

/*
 * CatOS networking.
 *
 * Driver layer  : AMD PCnet-PCI (Am79C970A), the default PCI NIC in QEMU
 *                 (`-netdev user,id=n0 -device pcnet,netdev=n0`).
 * Protocol layer: minimal Ethernet ARP / IPv4 / TCP (client) stack plus a
 *                 one-shot HTTP GET for the `curl` command and for CE apps.
 *
 * QEMU user-net (slirp) defaults used here:
 *   guest IP    10.0.2.15   gateway / host loopback  10.0.2.2
 *   DNS         10.0.2.3    (UDP resolver, optional)
 */

/* ---- driver / API ---- */
int  net_init(void);                                  /* probe + init NIC (idempotent) */
int  net_up(void);                                    /* 1 when the NIC is running */
int  net_send(const uint8_t *frame, uint32_t len);    /* raw Ethernet frame */
int  net_recv(uint8_t *frame, uint32_t max);          /* 0 = nothing yet */
void net_poll(void);                                  /* service incoming frames */
void net_rx_dispatch(const uint8_t *frame, uint32_t len);  /* protocol demux */
const char *net_backend_name(void);                    /* "e1000" / "pcnet" / "none" */

/* debug hooks for the `netdiag` shell command */
extern int      netdbg_tx_calls;
extern int      netdbg_tx_last;
extern int      netdbg_rx_frames;
extern int      netdbg_io_base;
extern uint16_t netdbg_csr0;
extern uint32_t netdbg_tx_word0;
extern uint32_t netdbg_rx_word0;
extern uint16_t netdbg_init_rcvrl;
extern uint16_t netdbg_init_xmtrl;
uint32_t netdbg_desc_tx0(void);
uint32_t netdbg_desc_rx0(void);
uint32_t netdbg_init_addr(void);
uint32_t netdbg_init_rxring(void);
uint32_t netdbg_init_txring(void);
uint16_t netdbg_csr(int i);

/* ---- protocol API (beginner friendly) ---- */
int  net_http_get(const char *url, char *buf, int max);
int  net_http_last_status(void);              /* v2.73: last HTTP status code */
const char *net_http_last_location(void);     /* v2.73: last 3xx Location       */
int  net_http_download(const char *url, uint8_t *buf, int max);
/* v2.78 D10: streaming download.  The sink is called with each body slice
 * as it arrives (0 = keep going, non-zero = abort).  Returns the total body
 * bytes, or a negative code: -9 chunk framing, -10 sink rejected,
 * -11 truncated/timeout, -12 peer reset (plus the shared net_http_get codes). */
typedef int (*net_stream_sink_t)(void *ud, const uint8_t *p, int len);
int  net_http_download_stream(const char *url, long range_from,
                              net_stream_sink_t sink, void *ud,
                              int *status_out, long *total_out);
/*   v2.78 D10: range_from > 0 requests "Range: bytes=<n>-" and the sink is
 *   called with the remaining body only.  -13 = the connection went idle
 *   with bytes outstanding (caller should resume from *total_out). */
/*   v2.70: binary-safe HTTP GET for .catpkg downloads.  Returns the exact
 *   body byte count (>=0) without NUL-terminating (packages contain zero
 *   bytes), or the same negative error codes as net_http_get. */
/*   HTTP GET; returns bytes copied into buf (>=0) or an error code:
 *     -1 NIC not initialised       -2 could not resolve host
 *     -3 connect failed/timeout    -4 HTTP status not 2xx
 *     -5 response too big / bad    -6 send failed
 *   URL form: http://host[:port]/path where host is an IP or a name
 *   resolved through the UDP DNS client (10.0.2.3 in QEMU user-net). */

/* ---- extended network API (UDP / DNS / ICMP, beginner friendly) ---- */
int  net_dns_lookup(const char *host, uint8_t ip[4]);
/*   resolve hostname to IPv4 (also accepts dotted-quad); 0 + ip, or -1 */
int  net_udp_send(const char *ip, uint16_t port, const uint8_t *data, uint32_t len);
/*   send one UDP datagram (ip may be a hostname); 0 = ok, -1 = error */
int  net_udp_recv(char *ip_out, int *port_out, uint8_t *buf, int max);
/*   pick up one received UDP datagram (one-shot); payload bytes,
 *   0 = nothing yet, -1 = bad args */
int  net_ping(const char *ip);
/*   one ICMP echo request (ip or hostname); 0 = reply, -1 = no reply */

/* ---- TCP server (v2.71): passive listener for sshd ----------------- */
int  net_tcp_listen(uint16_t port);
/*   arm the listener on `port` (one pending connection at a time) */
int  net_tcp_saccept(void);
/*   blocking wait for a completed handshake; 0 = established, -1 timeout */
int  net_tcp_ssend(const uint8_t *data, uint32_t len);
/*   send data on the accepted connection (chunked at MSS); 0 = ok */
int  net_tcp_srecv(uint8_t *buf, int max);
/*   drain buffered RX bytes; returns count, 0 = nothing yet, -1 = closed */
int  net_tcp_srecv_all(uint8_t *buf, int n);
/*   blocking exact-n read (polls the NIC while waiting); -1 = closed */
void net_tcp_sclose(void);
/*   FIN|ACK and tear the listener down */

/* ---- DHCP (v2.66): VMware NAT / VMnet8 auto-configuration ------------- */
int  net_dhcp_once(void);
/*   one DISCOVER->OFFER->REQUEST->ACK cycle; 0 = got a lease, -1 = none.
 *   Fills ip/gw/dns from the server; slirp (QEMU) answers with the old
 *   static values, VMware NAT with its own subnet. */
void net_dhcp_ensure(void);   /* lazy: first network use, unless 'netcfg' static was set */
void net_dhcp_retry(void);    /* 'netcfg dhcp': force a fresh discover */
void net_cfg_user_set(void);  /* 'netcfg IP GW': static config wins over DHCP */
int  net_dhcp_ok(void);       /* 1 when the current config came from DHCP */

#endif
