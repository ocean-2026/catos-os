/*
 * e1000.c -- Intel PRO/1000 (82540EM / 82545EM / 82544GC) NIC driver.
 *
 * Supported PCI IDs (verified against QEMU v8.2.0 hw/net/e1000.c):
 *   8086:100E  82540EM   (QEMU `-device e1000` default)
 *   8086:100F  82545EM   (VMware "e1000")
 *   8086:1004  82544GC
 *
 * =====================================================================
 * WHY BAR0 MMIO, NOT BAR1 I/O   (the single easiest thing to get wrong)
 * ---------------------------------------------------------------------
 * In QEMU v8.2.0 pci_e1000_realize() (/d/_qemu_e1000.c:1655-1657):
 *     pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &d->mmio);
 *     pci_register_bar(pci_dev, 1, PCI_BASE_ADDRESS_SPACE_IO,    &d->io);
 * BAR0 is the register/MMIO window (PNPMMIO_SIZE = 0x20000 = 128 KiB,
 * line 68).  BAR1 is a *stub*: e1000_io_read() returns a constant 0 and
 * e1000_io_write() is a no-op (lines 1325-1340).  Any driver that pokes
 * registers through the I/O BAR reads back zeros and programs nothing.
 * All register access here is 32-bit MMIO (e1000_mmio_ops.impl.min/max
 * access size == 4, lines 1315-1323), so registers are addressed as
 * mmio[byte_offset / 4].  QEMU derives the same index from
 * `index = (addr & 0x1ffff) >> 2` (lines 1268, 1295).
 *
 * =====================================================================
 * WHY THE STATIC PHYSICAL MEMORY AT 0x530000
 * ---------------------------------------------------------------------
 * The low 64 MiB is identity-mapped (paging_init(), 16 x 4 MiB PSE
 * supervisor pages), so a low physical address IS a usable C pointer --
 * net.c already treats 0x500000 as a pointer to its DMA descriptors.
 * QEMU DMAs by guest *physical* address (pci_dma_read/write), which never
 * consults the guest page tables, so these rings live in plain RAM.
 * Already taken: 0x500000..0x510000 (pcnet rings), 0x520000 (SB16 32 KiB
 * DMA).  We start at 0x530000, comfortably above those and far below the
 * ~21.5 MiB kernel image, and lay out both descriptor rings + packet
 * buffers in one contiguous, 16-byte-aligned block:
 *
 *   0x530000  RX descriptors  32 x 16 = 512 B   (0x530000..0x5301FF)
 *   0x530200  TX descriptors  16 x 16 = 256 B   (0x530200..0x5302FF)
 *   0x530400  RX buffers     32 x 2048 = 64 KiB (0x530400..0x5403FF)
 *   0x540400  TX buffers     16 x 2048 = 32 KiB (0x540400..0x5483FF)
 *
 * The whole block ends at 0x548400 -- STRICTLY BELOW the ES1371 audio
 * DMA buffer at 0x550000..0x560000 (include/es1371.h).  The TX ring is
 * only 16 deep because QEMU completes each TX synchronously inside the
 * TDT write, so 16 outstanding frames is far more than the stack ever
 * queues; keeping RX at 32 leaves plenty of burst absorption.  A larger
 * TX ring would push the tail into 0x550000 and corrupt audio samples.
 *
 * No kmalloc, no heap, nothing overlaps.  Ring base registers are masked
 * with ~0xF by QEMU (rx_desc_base/tx_desc_base, lines 745-751, 857-863);
 * both rings start on 16-byte boundaries.  RDLEN/TDLEN must be a multiple
 * of 128 (set_dlen, line 1096): 512 bytes satisfies that.
 *
 * =====================================================================
 * WHY POLLING FIRST
 * ---------------------------------------------------------------------
 * CatOS int 0x80 is an *interrupt gate* (IF is cleared on entry) and this
 * code must never `hlt` or wait on timer_ticks; a bounded pure busy-loop is
 * the only legal way to wait.  QEMU delivers RX frames through the net
 * queue: e1000_can_receive()/e1000_receive_iov() consume descriptors as
 * long as RDH != RDT, and set_rdt() (line 1075) calls
 * qemu_flush_queued_packets() whenever buffers are available.  So simply
 * advancing RDT from the poll loop is a complete, interrupt-free RX path --
 * ping/curl work with e1000_poll() alone, no IRQ required.  e1000_irq() is
 * provided as an optional top half for intr.c (it only reads/clears ICR and
 * sets a flag); reception never depends on it.  NOTE: set_rx_control()
 * (line 423) arms a one-shot 1000 ms flush_queue_timer on the RCTL write,
 * during which QEMU defers (queues, does not drop) incoming packets; the
 * guest's normal poll loop easily outlives that window.
 *
 * Register offsets/bits and the descriptor format below were cross-checked
 * against /d/_e1000_regs.h (QEMU hw/net/e1000_regs.h), /d/_qemu_e1000.c,
 * and QEMU v8.2.0 hw/net/e1000x_regs.h / e1000x_common.c.
 */

#include "e1000.h"
#include "io.h"
#include "paging.h"     /* vbe_lfb_map(): generic high-MMIO identity mapper */

/* ---- PCI configuration space (bare 0xCF8 / 0xCFC, copied from net.c) -- */
static uint32_t pci_cfg_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)(dev & 0x1F) << 11) | ((uint32_t)(fn & 7) << 8) |
                    (uint32_t)(off & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}
static void pci_cfg_write(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)(dev & 0x1F) << 11) | ((uint32_t)(fn & 7) << 8) |
                    (uint32_t)(off & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, v);
}

#define PCI_VENDOR_INTEL   0x8086u
#define PCI_CMD_IO         0x0001u
#define PCI_CMD_MEM        0x0002u
#define PCI_CMD_MASTER     0x0004u   /* QEMU gates DMA + RX on bus-master
                                      (e1000x_rx_ready): REQUIRED */

/* ---- Register offsets (BYTES; mmio[off/4]).  From e1000_regs.h -------- */
#define R_CTRL    0x00000u
#define R_STATUS  0x00008u
#define R_EECD    0x00010u
#define R_EERD    0x00014u
#define R_ICR     0x000C0u
#define R_IMS     0x000D0u
#define R_IMC     0x000D8u
#define R_RCTL    0x00100u
#define R_TCTL    0x00400u
#define R_MTA     0x05200u
#define R_RAL     0x05400u   /* Receive Address Low  (RA[0]) */
#define R_RAH     0x05404u   /* Receive Address High (RA[0]+1) */
#define R_RDBAL   0x02800u
#define R_RDBAH   0x02804u
#define R_RDLEN   0x02808u
#define R_RDH     0x02810u
#define R_RDT     0x02818u
#define R_TDBAL   0x03800u
#define R_TDBAH   0x03804u
#define R_TDLEN   0x03808u
#define R_TDH     0x03810u
#define R_TDT     0x03818u

/* ---- Register bits (QEMU v8.2.0 hw/net/e1000x_regs.h) ----------------- */
#define CTRL_RST    0x04000000u
#define CTRL_SLU    0x00000040u
#define CTRL_ASDE   0x00000020u

#define STATUS_LU   0x00000002u

#define RCTL_EN     0x00000002u
#define RCTL_UPE    0x00000008u   /* unicast promiscuous  -> accept all UCAST */
#define RCTL_MPE    0x00000010u   /* multicast promiscuous -> accept all MCAST */
#define RCTL_BAM    0x00008000u   /* accept broadcasts (ARP!) */
#define RCTL_SECRC  0x04000000u   /* strip Ethernet CRC: descriptor length
                                     == frame length, no +4 (e1000x_fcs_len) */
/* RCTL_SZ_2048 == 0x0 (BSIZE=0, BSEX=0): e1000x_rxbufsize() returns 2048 */

#define TCTL_EN     0x00000002u
#define TCTL_PSP    0x00000008u   /* pad short packets */
#define TCTL_CT     0x00000100u   /* collision threshold (0x10 << 4) */
#define TCTL_COLD   0x00040000u   /* collision distance  (0x40 << 12) */

#define RAH_AV      0x80000000u   /* address-valid bit in RAH */

#define IMC_ALL     0xFFFFFFFFu

/* legacy TX descriptor command/status bits (cmd/status are single BYTES) */
#define TXD_CMD_EOP   0x01u
#define TXD_CMD_IFCS  0x02u
#define TXD_CMD_RS    0x08u       /* report status -> QEMU writes DD back */
#define TXD_STAT_DD   0x01u
/* legacy RX descriptor status bits */
#define RXD_STAT_DD   0x01u
#define RXD_STAT_EOP  0x02u

/* EEPROM EERD bits (QEMU flash_eerd_read, lines 519-532) */
#define EEPROM_START       0x00000001u
#define EEPROM_DONE        0x00000010u
#define EEPROM_ADDR_SHIFT  8u
#define EEPROM_DATA_SHIFT  16u

/* ---- legacy descriptors (16 bytes each).  Packed + explicit 32-bit
 * address words: no uint64_t, so no 32-bit u64-alignment surprises.
 * Layout matches QEMU struct e1000_rx_desc / struct e1000_tx_desc
 * (the latter is spelled out in /d/_e1000_regs.h:276-294).         ---- */
typedef struct {
    uint32_t addr_lo;   /* +0  buffer_addr low  dword */
    uint32_t addr_hi;   /* +4  buffer_addr high dword (0: bufs < 4 GiB) */
    uint16_t length;    /* +8  */
    uint16_t csum;      /* +10 */
    uint8_t  status;    /* +12  DD/EOP written by hardware */
    uint8_t  errors;    /* +13 */
    uint16_t special;   /* +14 */
} __attribute__((packed)) e1k_rx_desc_t;

typedef struct {
    uint32_t addr_lo;   /* +0  */
    uint32_t addr_hi;   /* +4  */
    uint16_t length;    /* +8  */
    uint8_t  cso;       /* +10 */
    uint8_t  cmd;       /* +11 EOP/IFCS/RS */
    uint8_t  status;    /* +12 DD written back by hardware */
    uint8_t  css;       /* +13 */
    uint16_t special;   /* +14 */
} __attribute__((packed)) e1k_tx_desc_t;

/* ---- static physical DMA layout --------------------------------------- */
#define RX_DESC_COUNT   32
#define TX_DESC_COUNT   16          /* see the memory-map note above: this
                                     * keeps the block below 0x550000 */
#define RX_BUF_SIZE     2048
#define TX_BUF_SIZE     2048
#define MMIO_SIZE       0x20000u   /* PNPMMIO_SIZE, QEMU line 68 */

/* Explicit 0x100-aligned bases.  A full 0x200 is reserved for the TX ring
 * (only 0x100 used) so RX/TX buffer bases stay at 0x530400 / 0x540400.
 * End of block = 0x540400 + 16*2048 = 0x548400 < 0x550000 (ES1371). */
#define E1K_DMA_BASE   0x00530000u
#define RX_RING_PHYS   (E1K_DMA_BASE)                 /* 0x530000 */
#define TX_RING_PHYS   (RX_RING_PHYS + 0x200u)        /* 0x530200 */
#define RX_BUF_PHYS    (TX_RING_PHYS + 0x200u)        /* 0x530400 */
#define TX_BUF_PHYS    (RX_BUF_PHYS  + 0x10000u)      /* 0x540400 */

static volatile e1k_rx_desc_t *rx_ring = (volatile e1k_rx_desc_t *)RX_RING_PHYS;
static volatile e1k_tx_desc_t *tx_ring = (volatile e1k_tx_desc_t *)TX_RING_PHYS;
static volatile uint8_t       *rx_buf  = (volatile uint8_t *)RX_BUF_PHYS;
static volatile uint8_t       *tx_buf  = (volatile uint8_t *)TX_BUF_PHYS;

/* ---- driver state ------------------------------------------------------ */
static volatile uint32_t *mmio = (volatile uint32_t *)0;
static uint8_t  e_bus, e_dev, e_fn;
static uint32_t bar0_phys;
static int      e_irq_line = -1;
static int      probe_done, probe_result;
static int      e_ready;                 /* 1 = up */
static int      e_irq_pending;          /* set by e1000_irq() top half */
static uint8_t  e_mac[6];
static int      rx_next;                 /* next RX descriptor to hand back */
static int      tx_head;                 /* oldest outstanding TX descriptor */
static int      tx_tail;                 /* next free TX descriptor */
/* debug counters (read via e1000_info()) */
static uint32_t dbg_rx, dbg_tx, dbg_icr, dbg_irq_cause;
/* static scratch so e1000_poll() doesn't put a 2 KiB array on the stack */
static uint8_t  poll_frame[RX_BUF_SIZE];

/* Optional protocol-stack hook.  netstack.c defines net_rx_dispatch()
 * (netstack.c:550) and is always linked into the kernel (build.sh), so a
 * plain extern is safe.  e1000_poll() dispatches received frames straight
 * to the stack; e1000_recv() remains available for callers that prefer to
 * pull frames themselves. */
extern void net_rx_dispatch(const uint8_t *frame, uint32_t len);

static inline void wr(uint32_t off, uint32_t v) { mmio[off >> 2] = v; }
static inline uint32_t rd(uint32_t off)         { return mmio[off >> 2]; }

/* ---- PCI probe --------------------------------------------------------- */
static int scan_bus(int bus) {
    for (int dev = 0; dev < 32; dev++)
        for (int fn = 0; fn < 8; fn++) {
            uint8_t b = (uint8_t)bus, d = (uint8_t)dev, f = (uint8_t)fn;
            uint32_t id   = pci_cfg_read(b, d, f, 0);
            uint16_t vend = (uint16_t)(id & 0xFFFFu);
            uint16_t did  = (uint16_t)(id >> 16);
            if (vend == 0xFFFFu || vend == 0) continue;
            if (vend != PCI_VENDOR_INTEL) continue;
            if (did != 0x100Eu && did != 0x100Fu && did != 0x1004u) continue;
            uint32_t bar0 = pci_cfg_read(b, d, f, 0x10);
            if (bar0 & 1u) continue;                 /* needs a MEMORY BAR */
            uint32_t base = bar0 & 0xFFFFFFF0u;
            if (!base) continue;
            /* QEMU >= 11 gates both DMA and RX on bus-master enable
             * (e1000x_rx_ready / e1000_write_config). */
            uint16_t cmd = (uint16_t)pci_cfg_read(b, d, f, 0x04);
            cmd |= PCI_CMD_IO | PCI_CMD_MEM | PCI_CMD_MASTER;   /* 0x0007 */
            pci_cfg_write(b, d, f, 0x04, cmd);
            e_bus = b; e_dev = d; e_fn = f;
            bar0_phys = base;
            e_irq_line = (int)(pci_cfg_read(b, d, f, 0x3C) & 0xFFu);
            return 1;
        }
    return 0;
}

int e1000_probe(void) {
    if (probe_done) return probe_result;
    probe_done = 1;
    probe_result = 0;
    if (scan_bus(0)) { probe_result = 1; return 1; }
    /* VMware may hang a card behind a PCI-PCI bridge: walk bus 0 bridges
     * and scan their secondary bus (same trick as net.c). */
    for (int dev = 0; dev < 32; dev++)
        for (int fn = 0; fn < 8; fn++) {
            uint8_t d = (uint8_t)dev, f = (uint8_t)fn;
            uint32_t id   = pci_cfg_read(0, d, f, 0);
            uint16_t vend = (uint16_t)(id & 0xFFFFu);
            if (vend == 0xFFFFu || vend == 0) continue;
            uint32_t cls = (pci_cfg_read(0, d, f, 0x08) >> 8) & 0xFFFFFFu;
            if ((cls >> 16) != 0x06u || ((cls >> 8) & 0xFFu) != 0x04u) continue;
            uint32_t bn  = pci_cfg_read(0, d, f, 0x18);
            int      sec = (int)((bn >> 8) & 0xFFu);
            if (sec > 0 && scan_bus(sec)) { probe_result = 1; return 1; }
        }
    return 0;
}

/* ---- EEPROM (MAC fallback) -------------------------------------------- */
static int eeprom_read(uint8_t addr, uint16_t *out) {
    wr(R_EERD, EEPROM_START | ((uint32_t)addr << EEPROM_ADDR_SHIFT));
    for (uint32_t w = 0; w < 100000u; w++) {
        uint32_t v = rd(R_EERD);                 /* flash_eerd_read() */
        if (v & EEPROM_DONE) {
            *out = (uint16_t)(v >> EEPROM_DATA_SHIFT);
            return 0;
        }
    }
    return -1;
}

static void read_mac(void) {
    uint32_t ral = rd(R_RAL);
    uint32_t rah = rd(R_RAH);
    e_mac[0] = (uint8_t)(ral);
    e_mac[1] = (uint8_t)(ral >> 8);
    e_mac[2] = (uint8_t)(ral >> 16);
    e_mac[3] = (uint8_t)(ral >> 24);
    e_mac[4] = (uint8_t)(rah);
    e_mac[5] = (uint8_t)(rah >> 8);
    /* all-zero or all-ones => no station address; fall back to EEPROM
     * words 0/1/2 (QEMU e1000x_core_prepare_eeprom stores MAC there). */
    int all_zero = 1, all_ones = 1;
    for (int i = 0; i < 6; i++) {
        if (e_mac[i] != 0)    all_zero = 0;
        if (e_mac[i] != 0xFF) all_ones = 0;
    }
    if (all_zero || all_ones) {
        uint16_t w0, w1, w2;
        if (eeprom_read(0, &w0) == 0 && eeprom_read(1, &w1) == 0 &&
            eeprom_read(2, &w2) == 0) {
            e_mac[0] = (uint8_t)(w0);       e_mac[1] = (uint8_t)(w0 >> 8);
            e_mac[2] = (uint8_t)(w1);       e_mac[3] = (uint8_t)(w1 >> 8);
            e_mac[4] = (uint8_t)(w2);       e_mac[5] = (uint8_t)(w2 >> 8);
        }
    }
}

/* ---- TX descriptor reclaim --------------------------------------------
 * With RS set in the command byte, QEMU's txdesc_writeback() (line 730)
 * ORs DD into the descriptor's status byte.  Reclaim completed descriptors
 * in order, clearing DD so the slot can be reused.                        */
static void reclaim_tx(void) {
    while (tx_head != tx_tail) {
        volatile e1k_tx_desc_t *d = &tx_ring[tx_head];
        if (!(d->status & TXD_STAT_DD)) break;   /* hardware not done yet */
        d->status = 0;
        tx_head = (tx_head + 1) % TX_DESC_COUNT;
    }
}

/* ---- init -------------------------------------------------------------- */
int e1000_init(void) {
    if (e_ready) return 0;
    if (!e1000_probe()) return -1;

    /* Map BAR0 MMIO.  vbe_lfb_map() is a generic mapper for physical
     * addresses >= 64 MiB (it identity-maps high PDEs into the kernel and
     * the current page directory and returns phys as the usable pointer).
     * QEMU normally puts PCI MMIO high, so this is the common path.
     * If a BAR happens to sit below 64 MiB it is already covered by the
     * identity 4 MiB PSE pages (paging_init) -- vbe_lfb_map() would refuse
     * it anyway (it will not split a PSE page), so use it directly.      */
    if (bar0_phys >= 0x04000000u) {
        void *p = vbe_lfb_map(bar0_phys, MMIO_SIZE);
        if (!p) return -2;
        mmio = (volatile uint32_t *)p;
    } else {
        mmio = (volatile uint32_t *)bar0_phys;
    }

    /* ---- reset sequence (QEMU set_ctrl, line 407: RST self-clears) ---- */
    wr(R_IMC, IMC_ALL);                 /* mask all interrupts (poll mode) */
    wr(R_CTRL, CTRL_RST);
    for (uint32_t w = 0; w < 100000u; w++)   /* pure spin, no hlt/timer */
        if (!(rd(R_CTRL) & CTRL_RST)) break;
    wr(R_IMC, IMC_ALL);                 /* re-mask after reset */
    wr(R_CTRL, CTRL_SLU | CTRL_ASDE);  /* set link up */
    for (uint32_t w = 0; w < 1000000u; w++)
        if (rd(R_STATUS) & STATUS_LU) break;

    read_mac();

    /* Program Receive Address registers with the resolved MAC.  RAL/RAH
     * were already set from EEPROM by QEMU at reset; rewriting is standard
     * driver behaviour and matters on real silicon.  RAH carries AV.      */
    uint32_t ral = (uint32_t)e_mac[0] | ((uint32_t)e_mac[1] << 8) |
                   ((uint32_t)e_mac[2] << 16) | ((uint32_t)e_mac[3] << 24);
    uint32_t rah = (uint32_t)e_mac[4] | ((uint32_t)e_mac[5] << 8) | RAH_AV;
    wr(R_RAL, ral);
    wr(R_RAH, rah);

    /* ---- descriptor rings in static physical memory ---- */
    for (int i = 0; i < RX_DESC_COUNT; i++) {
        rx_ring[i].addr_lo = RX_BUF_PHYS + (uint32_t)i * RX_BUF_SIZE;
        rx_ring[i].addr_hi = 0;
        rx_ring[i].length  = 0;
        rx_ring[i].csum    = 0;
        rx_ring[i].status  = 0;
        rx_ring[i].errors  = 0;
        rx_ring[i].special = 0;
    }
    for (int i = 0; i < TX_DESC_COUNT; i++) {
        tx_ring[i].addr_lo = TX_BUF_PHYS + (uint32_t)i * TX_BUF_SIZE;
        tx_ring[i].addr_hi = 0;
        tx_ring[i].length  = 0;
        tx_ring[i].cso     = 0;
        tx_ring[i].cmd     = 0;
        tx_ring[i].status  = 0;
        tx_ring[i].css     = 0;
        tx_ring[i].special = 0;
    }

    rx_next = 0; tx_head = 0; tx_tail = 0;

    /* ---- TX ring registers (base must be 16-byte aligned) ---- */
    wr(R_TDBAL, TX_RING_PHYS);
    wr(R_TDBAH, 0);
    wr(R_TDLEN, (uint32_t)TX_DESC_COUNT * 16u);
    wr(R_TDH,   0);
    wr(R_TDT,   0);

    /* ---- RX ring registers ----
     * RDH (hardware head) starts at 0.  RDT (driver tail) points at the
     * last descriptor given back; with N descriptors the convention is
     * RDT = N-1, which leaves N-1 buffers owned by the hardware and keeps
     * RDH != RDT (e1000_has_rxbufs / e1000_can_receive).                  */
    wr(R_RDBAL, RX_RING_PHYS);
    wr(R_RDBAH, 0);
    wr(R_RDLEN, (uint32_t)RX_DESC_COUNT * 16u);
    wr(R_RDH,   0);
    wr(R_RDT,   (uint32_t)(RX_DESC_COUNT - 1));

    /* ---- enable TX then RX ---- */
    wr(R_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);
    wr(R_RCTL, RCTL_EN | RCTL_UPE | RCTL_MPE | RCTL_BAM | RCTL_SECRC);

    e_ready = 1;
    return 0;
}

int e1000_up(void) { return e_ready; }

/* ---- send -------------------------------------------------------------- */
int e1000_send(const uint8_t *frame, uint32_t len) {
    if (!e_ready || !frame || len == 0) return -1;
    if (len > TX_BUF_SIZE) return -2;

    /* Ensure a free TX slot.  reclaim_tx() + bounded pure spin: never
     * hlt, never wait on timer_ticks (IF may be 0 inside a syscall).      */
    for (uint32_t w = 0; w < 100000u; w++) {
        reclaim_tx();
        if ((tx_tail + 1) % TX_DESC_COUNT != tx_head) break;
        if (w == 99999u) return -3;
    }

    int i = tx_tail;
    volatile uint8_t *dst = &tx_buf[(uint32_t)i * TX_BUF_SIZE];
    for (uint32_t k = 0; k < len; k++) dst[k] = frame[k];
    /* Ethernet minimum frame is 60 bytes (without FCS); pad runts. */
    uint32_t plen = len;
    if (plen < 60u) {
        for (uint32_t k = len; k < 60u; k++) dst[k] = 0;
        plen = 60u;
    }

    volatile e1k_tx_desc_t *d = &tx_ring[i];
    d->addr_lo  = TX_BUF_PHYS + (uint32_t)i * TX_BUF_SIZE;
    d->addr_hi  = 0;
    d->length   = (uint16_t)plen;
    d->cso      = 0;
    d->cmd      = (uint8_t)(TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS);
    d->status   = 0;                 /* clear any stale DD */
    d->css      = 0;
    d->special  = 0;

    tx_tail = (i + 1) % TX_DESC_COUNT;
    wr(R_TDT, (uint32_t)tx_tail);     /* writing TDT triggers start_xmit()
                                         in QEMU (set_tctl, line 1102) */
    dbg_tx++;
    return 0;
}

/* ---- receive one frame straight from the hardware ring ----------------
 * Self-sufficient: works even if e1000_poll() was never called.          */
int e1000_recv(uint8_t *frame, uint32_t max) {
    if (!e_ready) return -1;
    if (!frame || max == 0) return -1;

    volatile e1k_rx_desc_t *d = &rx_ring[rx_next];
    if (!(d->status & RXD_STAT_DD)) return 0;   /* hardware hasn't filled it */

    uint32_t len = d->length;                   /* SECRC => no +4 CRC */
    if (len > max) len = max;
    volatile uint8_t *src = &rx_buf[(uint32_t)rx_next * RX_BUF_SIZE];
    for (uint32_t k = 0; k < len; k++) frame[k] = src[k];

    /* clear DD/length, then give this buffer back to the hardware by
     * writing RDT = index just consumed (set_rdt flushes QEMU's queue). */
    d->status = 0;
    d->length = 0;
    wr(R_RDT, (uint32_t)rx_next);
    rx_next = (rx_next + 1) % RX_DESC_COUNT;
    dbg_rx++;
    return (int)len;
}

/* ---- poll: the primary, interrupt-free data path ---------------------- */
void e1000_poll(void) {
    if (!e_ready || !mmio) return;

    /* ICR is clear-on-read (mac_icr_read, line 1031 / real HW).  Reading
     * it here both samples and clears the pending causes. */
    dbg_icr = rd(R_ICR);

    reclaim_tx();

    /* Drain every pending frame (bounded to one ring's worth), handing
     * each to the protocol stack.  e1000_recv() advances RDT, which is
     * exactly the kick QEMU's net queue needs to deliver more packets. */
    for (int i = 0; i < RX_DESC_COUNT; i++) {
        int n = e1000_recv(poll_frame, RX_BUF_SIZE);
        if (n <= 0) break;
        net_rx_dispatch(poll_frame, (uint32_t)n);
    }
}

/* ---- optional IRQ top half (PIC EOI is sent by intr.c) ---------------- */
int e1000_irq(void) {
    if (!e_ready || !mmio) return 0;
    uint32_t icr = rd(R_ICR);        /* clear-on-read */
    if (!icr) return 0;              /* not our interrupt */
    dbg_irq_cause |= icr;
    dbg_icr = icr;
    e_irq_pending = 1;
    return 1;
}

int e1000_irq_line(void) {
    return (probe_done && probe_result) ? e_irq_line : -1;
}

void e1000_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = e_ready ? e_mac[i] : 0;
}

int e1000_info(int what) {
    switch (what) {
    case E1000_INFO_PRESENT:   return e1000_probe();
    case E1000_INFO_UP:        return e_ready;
    case E1000_INFO_BAR0:      return (int)bar0_phys;
    case E1000_INFO_MMIO:      return (int)(uint32_t)mmio;
    case E1000_INFO_IRQ_LINE:  return e1000_irq_line();
    case E1000_INFO_MAC_LO:
        return (int)((uint32_t)e_mac[0] | ((uint32_t)e_mac[1] << 8) |
                     ((uint32_t)e_mac[2] << 16) | ((uint32_t)e_mac[3] << 24));
    case E1000_INFO_MAC_HI:
        return (int)((uint32_t)e_mac[4] | ((uint32_t)e_mac[5] << 8));
    case E1000_INFO_RX_FRAMES: return (int)dbg_rx;
    case E1000_INFO_TX_FRAMES: return (int)dbg_tx;
    case E1000_INFO_ICR:       return (int)dbg_icr;
    default: break;
    }
    if (!mmio) return 0;
    switch (what) {
    case E1000_INFO_RDH:    return (int)rd(R_RDH);
    case E1000_INFO_RDT:    return (int)rd(R_RDT);
    case E1000_INFO_TDH:    return (int)rd(R_TDH);
    case E1000_INFO_TDT:    return (int)rd(R_TDT);
    case E1000_INFO_STATUS: return (int)rd(R_STATUS);
    case E1000_INFO_CTRL:   return (int)rd(R_CTRL);
    default:                return 0;
    }
}
