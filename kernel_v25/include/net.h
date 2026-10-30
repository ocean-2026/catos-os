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

#endif
