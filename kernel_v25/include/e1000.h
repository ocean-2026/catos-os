#ifndef E1000_H
#define E1000_H

#include <stdint.h>

/*
 * CatOS -- standalone Intel PRO/1000 (e1000) driver.
 *
 * QEMU:   -netdev user,id=n0 -device e1000,netdev=n0   (8086:100E)
 * VMware: virtual NIC "e1000"                          (8086:100F)
 * Also accepts 8086:1004 (82544GC) variants.
 *
 * The driver is polling-first: e1000_poll() services the hardware (clears
 * ICR, reclaims TX descriptors) and drains received frames into the
 * protocol stack.  e1000_recv() is a self-sufficient single-frame pull for
 * callers that would rather drive reception themselves.  e1000_irq() is an
 * optional top half for intr.c; reception never *depends* on interrupts.
 */

int  e1000_probe(void);                                  /* 1 = found (8086:100E/100F/1004), idempotent */
int  e1000_init(void);                                   /* 0 = up & running, negative = failure       */
int  e1000_up(void);                                     /* 1 = initialised and ready to send/receive  */
int  e1000_send(const uint8_t *frame, uint32_t len);     /* raw Ethernet frame, 0 = ok, negative = err */
int  e1000_recv(uint8_t *frame, uint32_t max);           /* frame length; 0 = none pending            */
void e1000_poll(void);                                   /* service RX/TX, to be called from the poll loop */
int  e1000_irq(void);                                    /* IRQ top half; 1 = ours (EOI sent by intr.c)   */
int  e1000_irq_line(void);                               /* PCI config 0x3C IRQ line, -1 if not probed   */
void e1000_mac(uint8_t mac[6]);                          /* copy 6-byte MAC, all-zero if not initialised  */
int  e1000_info(int what);                               /* diagnostics, selector = E1000_INFO_*           */

/* ---- e1000_info() selectors ------------------------------------------- */
#define E1000_INFO_PRESENT    0   /* e1000_probe() result                    */
#define E1000_INFO_UP         1   /* e1000_up() result                       */
#define E1000_INFO_BAR0       2   /* BAR0 physical base                      */
#define E1000_INFO_MMIO       3   /* mapped MMIO base (virtual == phys)      */
#define E1000_INFO_IRQ_LINE   4   /* PCI IRQ line                            */
#define E1000_INFO_MAC_LO     5   /* MAC bytes 0..3 packed little-endian     */
#define E1000_INFO_MAC_HI     6   /* MAC bytes 4..5                          */
#define E1000_INFO_RX_FRAMES  7   /* received-frame counter                  */
#define E1000_INFO_TX_FRAMES  8   /* sent-frame counter                      */
#define E1000_INFO_ICR        9   /* last ICR value seen by poll/irq         */
#define E1000_INFO_RDH       10   /* RX descriptor head (hardware)           */
#define E1000_INFO_RDT       11   /* RX descriptor tail (driver)             */
#define E1000_INFO_TDH       12   /* TX descriptor head (hardware)           */
#define E1000_INFO_TDT       13   /* TX descriptor tail (driver)             */
#define E1000_INFO_STATUS    14   /* STATUS register (link up etc.)          */
#define E1000_INFO_CTRL      15   /* CTRL register                           */

#endif /* E1000_H */
