/* net.c - AMD PCnet-PCI (Am79C970A) driver.
 *
 * QEMU:   -netdev user,id=n0 -device pcnet,netdev=n0
 * VMware: virtual machine NIC type "AMD PCnet" (Am79C970A).
 *
 * Layout facts taken from QEMU hw/net/pcnet.c and the Am79C970A data
 * sheet (VMware implements the same chip semantics):
 *   - default (BCR_SSIZE32=0): 8-byte descriptors, address+status packed:
 *       word0 = buffer addr (low 24 bits) | (status bits 8..15 << 16)
 *       word1 = RX: buf_length (2's comp)   / TX: message length
 *       word2 = RX: message length (chip)   / TX: 0
 *   - CSR0 bits: INIT=0x0001 START=0x0002 STOP=0x0004 TDMD=0x0008
 *     IDON reported at bit8 (0x0100)
 *   - init-block pointer is written to CSR1|CSR2<<16
 *   - init block (16-bit variant): mode, padr[6], ladrf[8], rdra, tdra;
 *     ring length encoded in the top 3 bits of rdra/tdra (1<<n entries)
 *   - TX status: OWN=0x8000 STP=0x0200 ENP=0x0100
 *
 * I/O width: the chip supports both 16-bit and dword access; QEMU
 * auto-switches to dword on the first 16-bit RDP write, VMware and real
 * hardware stay in 16-bit mode.  pnet_probe_width() detects which one
 * works at runtime (see the register notes below).
 */

#include "net.h"
#include "task.h"
#include "io.h"

/* ---- PCI ---- */
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

#define PCI_VENDOR_AMD   0x1022u
#define PCI_DEV_PCNET    0x2000u
#define PCI_VENDOR_VMWARE 0x15ADu
/* NOTE: 15AD:0405 is the VMware SVGA II adapter (graphics), NOT a NIC.
 * The vlance NIC lives behind the PCI-PCI bridge 15AD:0790; its own id is
 * still AMD 1022:2000 (VMware emulates the real PCnet-PCI chip). */
#define PCI_COMMAND_IO       0x0001u   /* I/O space enable */
#define PCI_COMMAND_MEM      0x0002u   /* memory space enable */
#define PCI_COMMAND_MASTER   0x0004u   /* bus master enable -- REQUIRED by
                                          QEMU 11 gate (bus_master_enable_region);
                                          without it PCI DMA reads return 0 */

/* net_ready == 0 reasons (net_fail_code) */
#define NET_FAIL_NONE     0
#define NET_FAIL_NO_DEV   1   /* no 1022:2000 on bus 0 / behind any bridge */
#define NET_FAIL_BUS_FF   2   /* CSR reads back 0xFFFF/0x0000 -> no real chip */
#define NET_FAIL_MAC      3   /* MAC register unreadable (all FF/00) */
#define NET_FAIL_INIT     4   /* INIT never latched IDON, or START didn't stick */

static uint16_t pci_io_base;
static int pnet_bus, pnet_dev, pnet_fn;   /* where PCnet was found */

/* COM1 debug output (defined below; forward decls for pci_find_pcnet) */
static void com1_str(const char *s);
static void com1_hex(uint32_t v);
static void com1_num(int v);
static void pnet_wcsr(int idx, uint16_t v);    /* forward: used by width probe */
static uint16_t pnet_rcsr(int idx);

/* PCI scan summary (for diagnostics): every device found on bus 0 */
int      netdbg_pci_count = 0;
uint16_t netdbg_pci_vend[8];
uint16_t netdbg_pci_dev[8];
uint32_t netdbg_pci_cls[8];
static void pci_scan_summary(void) {
    netdbg_pci_count = 0;
    for (int dev = 0; dev < 32 && netdbg_pci_count < 8; dev++)
        for (int fn = 0; fn < 8 && netdbg_pci_count < 8; fn++) {
            uint32_t id = pci_cfg_read(0, dev, fn, 0);
            uint16_t v = (uint16_t)(id & 0xFFFFu);
            if (v == 0xFFFFu || v == 0) continue;    /* empty slot / fn */
            netdbg_pci_vend[netdbg_pci_count] = v;
            netdbg_pci_dev[netdbg_pci_count] = (uint16_t)(id >> 16);
            netdbg_pci_cls[netdbg_pci_count] = (pci_cfg_read(0, dev, fn, 0x08) >> 8) & 0xFFFFFFu;
            netdbg_pci_count++;
        }
}

static int find_pcnet_on_bus(int bus) {
    for (int dev = 0; dev < 32; dev++)
        for (int fn = 0; fn < 8; fn++) {
            uint32_t id = pci_cfg_read((uint8_t)bus, dev, fn, 0);
            uint16_t v = (uint16_t)(id & 0xFFFFu);
            uint16_t d = (uint16_t)(id >> 16);
            /* AMD PCnet-PCI II (QEMU) -- the only PCnet id we know so far.
             * VMware's vlance lives behind a PCI-PCI bridge; its id is
             * reported by find_pcnet_on_bus once we walk the bridge. */
            if (v == PCI_VENDOR_AMD && d == PCI_DEV_PCNET) {
                uint32_t bar0 = pci_cfg_read((uint8_t)bus, dev, fn, 0x10);
                uint32_t bar1 = pci_cfg_read((uint8_t)bus, dev, fn, 0x14);
                uint32_t cls  = pci_cfg_read((uint8_t)bus, dev, fn, 0x0C);
                pci_io_base = (uint16_t)(bar0 & 0xFFFCu);   /* I/O BAR, drop type bits */
                pnet_bus = bus; pnet_dev = dev; pnet_fn = fn;
                /* Enable IO + memory + bus-master in PCI command register.
                 * Critical for QEMU >= 11: DMA is gated behind PCI_COMMAND_MASTER
                 * (pci_set_master) -- without bit 2 the DMA address space stays
                 * disabled and every pci_dma_read returns zeroes. */
                uint16_t cmd = (uint16_t)pci_cfg_read((uint8_t)bus, dev, fn, 0x04);
                cmd |= PCI_COMMAND_IO | PCI_COMMAND_MEM | PCI_COMMAND_MASTER;
                pci_cfg_write((uint8_t)bus, dev, fn, 0x04, cmd);
                com1_str("[net] pcnet bus="); com1_num(bus);
                com1_str(" dev="); com1_num(dev);
                com1_str(" fn="); com1_num(fn);
                com1_str(" bar0="); com1_hex(bar0);
                com1_str(" bar1="); com1_hex(bar1);
                com1_str(" io="); com1_hex(pci_io_base);
                com1_str(" cmd="); com1_hex(pci_cfg_read((uint8_t)bus, dev, fn, 0x04));
                com1_str("\n");
                return 1;
            }
        }
    return 0;
}

static int pci_find_pcnet(void) {
    if (find_pcnet_on_bus(0)) return 1;
    /* VMware puts the vlance NIC behind a PCI-PCI bridge: walk every
     * bridge on bus 0 and scan its secondary bus. */
    for (int dev = 0; dev < 32; dev++)
        for (int fn = 0; fn < 8; fn++) {
            uint32_t id = pci_cfg_read(0, dev, fn, 0);
            if ((id & 0xFFFFu) == 0xFFFFu || (id & 0xFFFFu) == 0) continue;
            uint32_t cls = (pci_cfg_read(0, dev, fn, 0x08) >> 8) & 0xFFFFFFu;
            if ((cls >> 16) == 0x06u && ((cls >> 8) & 0xFFu) == 0x04u) {   /* PCI-PCI bridge */
                uint32_t bn = pci_cfg_read(0, dev, fn, 0x18);  /* primary@18 sec@19 sub@1A */
                int sec = (int)((bn >> 8) & 0xFFu);
                uint32_t win = pci_cfg_read(0, dev, fn, 0x1C); /* IO base/limit @1C/1D */
                uint32_t mwin = pci_cfg_read(0, dev, fn, 0x20);/* mem base/limit @20/24 */
                com1_str("[net] bridge dev="); com1_num(dev);
                com1_str(" secbus="); com1_num(sec);
                com1_str(" cmd="); com1_hex(pci_cfg_read(0, dev, fn, 0x04));
                com1_str(" iowin="); com1_hex(win);
                com1_str(" memwin="); com1_hex(mwin); com1_str("\n");
                if (sec > 0) {
                    if (find_pcnet_on_bus(sec)) return 1;
                    com1_str("[net] bridge secbus ");
                    com1_num(sec);
                    com1_str(" no pcnet; devs:");
                    for (int d2 = 0; d2 < 32; d2++)
                        for (int f2 = 0; f2 < 8; f2++) {
                            uint32_t i2 = pci_cfg_read((uint8_t)sec, d2, f2, 0);
                            if ((i2 & 0xFFFFu) == 0xFFFFu || (i2 & 0xFFFFu) == 0) continue;
                            com1_str(" ");
                            com1_hex(i2 & 0xFFFFu); com1_str(":");
                            com1_hex(i2 >> 16);
                        }
                    com1_str("\n");
                }
            }
        }
    return 0;
}

/* ---- PCnet registers (RAP @ +0x14, RDP @ +0x10) ----
 * The Am79C970A has TWO I/O access widths:
 *   - 16-bit mode (default, BCR_BSBC bit7=0): RAP/RDP are 16-bit ports.
 *     VMware Workstation's PCnet and real hardware use this.
 *   - dword (DWIO) mode: RAP/RDP are 32-bit ports.
 *     QEMU switches the device into DWIO automatically on the FIRST
 *     16-bit write to the RDP offset; afterwards CSR access MUST use
 *     32-bit port I/O on RDP (RAP stays 16-bit wide).
 * We detect the working width at init: try 16-bit first (works on
 * VMware / real hardware); if the CSR does not answer, the write already
 * switched QEMU into DWIO, so fall back to 32-bit access.  Never mix:
 * on VMware a 32-bit RDP write would spill into the BDP port at +0x12
 * and corrupt the bus-config registers (that is why the old single-mode
 * QEMU driver failed there). */
static int pnet_width = 0;   /* 16 or 32; 0 = not probed yet */

/* register layout: 1 = PCnet-PCI (QEMU: MAC@CSR16-18, ring addr in init block,
 * 8-byte descriptors); 0 = AMD LANCE (VMware vlance: MAC@CSR3-5, ring addr via
 * CSR6-9, 16-byte descriptors).  Detected by the width probe. */
static int pnet_layout = 1;

/* CSR0 control bits.  BOTH VMware vlance and QEMU's pcnet implement the
 * 79C970A layout: INIT=1, START=2, STOP=4, TDMD=8, IDON=0x100.  (The old
 * AMD LANCE/Am7990 used INIT=0x4000 etc., but VMware's "vlance" does not:
 * its CSR0 resets to 0x0004 = STOP, proving the 79C970A bit map.) */
#define CSR0_INIT   0x0001u
#define CSR0_START  0x0002u
#define CSR0_STOP   0x0004u
#define CSR0_TDMD   0x0008u
#define CSR0_IDON   0x0100u     /* emulator reports IDON at bit 8 */

static uint16_t csr0_init(void)  { return CSR0_INIT; }
static uint16_t csr0_start(void) { return CSR0_START; }
static uint16_t csr0_stop(void)  { return CSR0_STOP; }
static uint16_t csr0_tdmd(void)  { return CSR0_TDMD; }
static uint16_t csr0_idon(void)  { return CSR0_IDON; }
static uint16_t csr0_stopbit(void){ return CSR0_STOP; }

static int try_width(int w) {
    pnet_width = w;
    if (w == 32) {
        outw(pci_io_base + 0x10, 0);              /* first 16-bit RDP write -> DWIO */
        outl(pci_io_base + 0x14, 0);
    }
    /* STOP, then verify the CSR answers at this width */
    pnet_wcsr(0, csr0_stop());
    int ok = 0;
    for (int i = 0; i < 1000; i++) {
        uint16_t c0 = pnet_rcsr(0);
        /* An empty/absent device reads back all ones (or all zeros): that is
         * not a chip in STOP state, it is a dead bus.  Reject it here so
         * net_init() can report NET_FAIL_BUS_FF instead of a fake ready. */
        if (c0 == 0xFFFFu || c0 == 0x0000u) return 0;
        if (c0 & csr0_stopbit()) { ok = 1; break; }
    }
    if (!ok) return 0;
    if (w == 32) { pnet_layout = 1; return 1; }
    /* 16-bit works on BOTH VMware vlance and (since QEMU ~11) QEMU - but
     * QEMU's 16-bit reads just ECHO one value for every register.  Probe
     * two registers that always differ on real silicon (CSR4/CSR5 RX/TX
     * counters): equal reads = echo mode = QEMU -> fail so the caller
     * falls back to the 32-bit DWIO probe.  Different = VMware vlance. */
    {
        uint16_t r4 = pnet_rcsr(4), r5 = pnet_rcsr(5);
        if (r4 == r5) return 0;            /* echo: not a real LANCE    */
        pnet_layout = 0;                   /* LANCE (MAC in CSR12-14)   */
        return 1;
    }
}

static int pnet_probe_width(void) {
    /* Real hardware / VMware: plain 16-bit CSR access.
     * QEMU: first 16-bit RDP write auto-switches the device into DWIO;
     * afterwards only 32-bit CSR access is trustworthy.  Try 16 first
     * (plain hardware), fall back to 32 (QEMU / VMware after its own
     * DWIO switch).  Layout (LANCE vs PCnet-PCI) is detected inside
     * try_width via the MAC registers. */
    if (try_width(16)) return 16;
    com1_str("[net] 16-bit probe failed, trying 32-bit\n");
    if (try_width(32)) return 32;
    return 0;
}

static void pnet_wcsr(int idx, uint16_t v) {
    if (pnet_width == 32) {
        outl(pci_io_base + 0x14, (uint32_t)idx);   /* RAP (32-bit, DWIO) */
        outl(pci_io_base + 0x10, (uint32_t)v);     /* RDP (32-bit, DWIO) */
    } else {
        outw(pci_io_base + 0x14, (uint16_t)idx);   /* RAP (16-bit) */
        outw(pci_io_base + 0x10, v);               /* RDP (16-bit) */
    }
}
static uint16_t pnet_rcsr(int idx) {
    if (pnet_width == 32) {
        outl(pci_io_base + 0x14, (uint32_t)idx);   /* RAP (32-bit, DWIO) */
        return (uint16_t)inl(pci_io_base + 0x10);  /* RDP (32-bit) */
    }
    outw(pci_io_base + 0x14, (uint16_t)idx);       /* RAP (16-bit) */
    return inw(pci_io_base + 0x10);                /* RDP (16-bit) */
}

#define NET_RX_RING 8
#define NET_TX_RING 4
#define NET_BUF_SZ  1600

/* 8-byte descriptor, QEMU non-SSIZE32 format */
typedef struct {
    uint32_t word0;   /* addr(low 24) | status bits 8..15 << 16 */
    uint16_t word1;   /* RX buflen(2's comp) / TX msglen */
    uint16_t word2;   /* RX msglen (chip writes) / TX reserved */
} __attribute__((packed)) net_bd_t;

/* 16-bit init block (QEMU struct pcnet_initblk16) */
typedef struct {
    uint16_t mode;
    uint16_t padr[3];
    uint16_t ladrf[4];
    uint32_t rx_ring;   /* top 3 bits = rlen (1<<rlen entries) | low 24 bits = addr */
    uint32_t tx_ring;
} __attribute__((packed)) pnet_init_t;

/* ---- DMA memory layout ----
 * QEMU's PHYSADDR (non-SSIZE32 mode) extends every DMA address by
 * ((csr[2] & 0xff00) << 16).  csr[2] holds the init block's high word, so if
 * the init block lives above 16 MiB, csr[2] gets a non-zero bits8..15 and ALL
 * DMA addresses (init block, rings, buffers) get OR-ed with +16 MiB garbage.
 * Therefore every DMA structure must live below 16 MiB.  We park them at a
 * fixed low address (8 MiB, far below the 21.5 MiB kernel image). */
#define DMA_BASE 0x500000u

static pnet_init_t *net_init_blk = (pnet_init_t *)DMA_BASE;
static net_bd_t *net_rx_ring = (net_bd_t *)(DMA_BASE + 0x20);
static net_bd_t *net_tx_ring = (net_bd_t *)(DMA_BASE + 0x80);
static uint8_t *net_rx_buf0 = (uint8_t *)(DMA_BASE + 0xA0);
static uint8_t *net_tx_buf0 = (uint8_t *)(DMA_BASE + 0x32A0);
#define net_rx_buf(i) (net_rx_buf0 + (i) * NET_BUF_SZ)
#define net_tx_buf(i) (net_tx_buf0 + (i) * NET_BUF_SZ)

/* unified descriptor access: returns word0 of descriptor i (tx=1: TX ring).
 * BOTH emulators (QEMU pcnet, VMware vlance) use the PCnet 8-byte
 * descriptor: OWN bit31, addr low 24, STP/ENP at bits 25/24, MCNT in
 * word2.  VMware's vlance differs only in the init block layout (LANCE
 * 16-bit words) and the MAC registers (CSR12-14). */
static volatile uint32_t *pnet_bd(int i, int tx) {
    if (pnet_layout) return tx ? (volatile uint32_t *)&net_tx_ring[i].word0
                               : (volatile uint32_t *)&net_rx_ring[i].word0;
    /* LANCE rings at 0x506000/0x506080 (kept clear of the 0x5032A0-0x504B80
     * buffer area used by net_tx_buf/net_rx_buf) */
    return tx ? (volatile uint32_t *)(DMA_BASE + 0x6000 + 0x80 + i * 8)
              : (volatile uint32_t *)(DMA_BASE + 0x6000 + i * 8);
}
/* OWN bit test/set mask: bit31 for both layouts (PCnet descriptor format) */
static uint32_t pnet_own_mask(void) { return 0x80000000u; }
/* word1 lives at +4 bytes, word2 at +6 bytes in BOTH layouts */
static volatile uint16_t *pnet_bd_w1(int i, int tx) { return (volatile uint16_t *)pnet_bd(i, tx) + 2; }
static volatile uint16_t *pnet_bd_w2(int i, int tx) { return (volatile uint16_t *)pnet_bd(i, tx) + 3; }

uint8_t  net_mac[6];
int      net_ready = 0;
/* Why net_ready is 0 -- reported by netdiag and by the "not ready" hints.
 * (2026-11-12, TODO #54: "net_ready=1 mac=FF:FF:FF:FF" was a false-positive:
 * reading the CSR register of an empty PCI slot returns 0xFFFF, and
 * 0xFFFF & CSR0_STOP looks exactly like a successful STOP.  Every probe
 * below now treats an all-FF / all-00 read as a dead bus.) */
int      net_fail_code = NET_FAIL_NONE;
static int      net_tx_head = 0;
static int      net_rx_head = 0;

/* debug counters for the shell `netdiag` command */
int      netdbg_tx_calls = 0;
int      netdbg_tx_last  = 0;
int      netdbg_rx_frames = 0;
int      netdbg_io_base = 0;
int      netdbg_io_width = 0;   /* 16 or 32 (CSR access width) */
uint16_t netdbg_csr0 = 0;
uint32_t netdbg_tx_word0 = 0;
uint32_t netdbg_rx_word0 = 0;
uint16_t netdbg_init_rcvrl = 0;   /* RCVRL read right after INIT (0=unknown) */
uint16_t netdbg_init_xmtrl = 0;

int net_up(void) { return net_ready; }

/* Environment probe (2026-11-12, #54): VMware Workstation guests expose at
 * least one of
 *   15AD:0405  SVGA II graphics
 *   15AD:0790  PCI-PCI bridge (vlance sits behind it)
 *   15AD:07B0  vmxnet3
 *   1274:1371  ES1371 audio (Ensoniq AudioPCI)
 * The shell uses this to give VM-appropriate advice instead of QEMU flags. */
int net_env_vmware(void) {
    for (int i = 0; i < netdbg_pci_count; i++) {
        uint16_t v = netdbg_pci_vend[i], d = netdbg_pci_dev[i];
        if (v == 0x15AD && (d == 0x0405 || d == 0x0790 || d == 0x07B0 || d == 0x0740))
            return 1;
        if (v == 0x1274 && d == 0x1371) return 1;
    }
    return 0;
}

/* debug: current TX descriptor 0 and RX descriptor 0 words (for netdiag) */
uint32_t netdbg_desc_tx0(void) { return pnet_bd(0, 1)[0]; }
uint32_t netdbg_desc_rx0(void) { return pnet_bd(0, 0)[0]; }
uint32_t netdbg_init_addr(void)  { return (uint32_t)net_init_blk; }
uint32_t netdbg_init_rxring(void) { return net_init_blk->rx_ring; }
uint32_t netdbg_init_txring(void) { return net_init_blk->tx_ring; }
uint16_t netdbg_csr(int i)       { return pnet_rcsr(i); }

/* ---- COM1 debug output (VMware serial log) ---- */
static void com1_init(void) {
    outb(0x3F8 + 1, 0x00);                 /* disable interrupts */
    outb(0x3F8 + 3, 0x80);                 /* DLAB on */
    outb(0x3F8, 0x01); outb(0x3F8 + 1, 0x00);   /* 115200 8N1 */
    outb(0x3F8 + 3, 0x03);
    outb(0x3F8 + 2, 0xC7);                 /* FIFO */
    outb(0x3F8 + 4, 0x0B);                 /* IRQs enabled */
}
static void com1_putc(char c) {
    while (!(inb(0x3F8 + 5) & 0x20)) ;
    outb(0x3F8, (uint8_t)c);
}
static void com1_str(const char *s) { while (*s) com1_putc(*s++); }
static void com1_hex(uint32_t v) {
    static const char *d = "0123456789ABCDEF";
    com1_str("0x");
    for (int i = 28; i >= 0; i -= 4) com1_putc(d[(v >> i) & 0xF]);
}
static void com1_num(int v) {
    char t[16]; int n = 0;
    if (v == 0) { com1_putc('0'); return; }
    while (v > 0) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) com1_putc(t[--n]);
}

int net_init(void) {
    if (net_ready) return 1;
    net_fail_code = NET_FAIL_NONE;
    com1_init();
    com1_str("\n[net] init begin\n");
    pci_scan_summary();               /* record bus devices for netdiag */
    if (!pci_find_pcnet()) {
        net_fail_code = NET_FAIL_NO_DEV;
        com1_str("[net] FAIL: no PCnet; PCI bus has ");
        com1_num(netdbg_pci_count); com1_str(" dev(s):");
        for (int i = 0; i < netdbg_pci_count; i++) {
            com1_str(" ");
            com1_hex(netdbg_pci_vend[i]); com1_str(":");
            com1_hex(netdbg_pci_dev[i]); com1_str(":");
            com1_hex(netdbg_pci_cls[i]);
        }
        com1_str("\n");
        return -1;
    }
    netdbg_io_base = pci_io_base;

    /* pick 16-bit or 32-bit (DWIO) CSR access -- see the register notes */
    pnet_width = pnet_probe_width();
    if (pnet_width == 0) {
        net_fail_code = NET_FAIL_BUS_FF;
        com1_str("[net] FAIL: no CSR access (dead bus)\n");
        return -1;
    }
    netdbg_io_width = pnet_width;
    com1_str("[net] io_base="); com1_hex(pci_io_base);
    com1_str(" width="); com1_num(pnet_width); com1_str("-bit");
    com1_str(" layout="); com1_num(pnet_layout); com1_str(" (1=PCnet-PCI 0=LANCE)\n");

    /* STOP the chip */
    pnet_wcsr(0, csr0_stop());
    for (int i = 0; i < 100000; i++)
        if (pnet_rcsr(0) & csr0_stopbit()) break;

    /* read MAC from the layout's registers (validated by width probe) */
    uint16_t lo, mid, hi;
    if (pnet_layout) { lo = pnet_rcsr(16); mid = pnet_rcsr(17); hi = pnet_rcsr(18); }
    else             { lo = pnet_rcsr(12); mid = pnet_rcsr(13); hi = pnet_rcsr(14); }
    net_mac[0] = (uint8_t)(lo & 0xFF);     net_mac[1] = (uint8_t)(lo >> 8);
    net_mac[2] = (uint8_t)(mid & 0xFF);    net_mac[3] = (uint8_t)(mid >> 8);
    net_mac[4] = (uint8_t)(hi & 0xFF);     net_mac[5] = (uint8_t)(hi >> 8);
    /* An all-ones MAC means the register returned an open-bus pattern:
     * there is no chip listening at this I/O BAR (VMware with an E1000 or
     * vmxnet3 NIC, or a stale PCnet id in the config space).  Never adopt
     * FF:FF:FF:FF:FF:FF as a working MAC. */
    if (net_mac[0]==0xFF && net_mac[1]==0xFF && net_mac[2]==0xFF &&
        net_mac[3]==0xFF && net_mac[4]==0xFF && net_mac[5]==0xFF) {
        net_fail_code = NET_FAIL_MAC;
        com1_str("[net] FAIL: MAC reads FF:FF:FF:FF:FF:FF (no chip at io_base)\n");
        return -1;
    }
    if (net_mac[0]==0 && net_mac[1]==0 && net_mac[2]==0 &&
        net_mac[3]==0 && net_mac[4]==0 && net_mac[5]==0) {
        if (!pnet_layout) {
            /* VMware vlance (LANCE): no readable MAC registers; the chip
             * takes its physical address from the init block.  Fall back to
             * a locally-administered unicast address.  (NAT does not check
             * the source MAC.) */
            net_mac[0]=0x02; net_mac[1]=0x00; net_mac[2]=0x00;
            net_mac[3]=0x00; net_mac[4]=0x00; net_mac[5]=0x01;
            com1_str("[net] LANCE: no chip MAC, using fixed 02:00:00:00:00:01\n");
            /* diagnostic: dump the NIC's PCI config space -- some devices
             * expose the factory MAC in a config register */
            com1_str("[net] cfg:");
            for (int o = 0x10; o < 0x40; o += 4)
                { com1_str(" "); com1_hex(pci_cfg_read((uint8_t)pnet_bus, (uint8_t)pnet_dev, (uint8_t)pnet_fn, (uint8_t)o)); }
            com1_str("\n");
        } else {
            net_mac[0]=0x52; net_mac[1]=0x54; net_mac[2]=0x00;
            net_mac[3]=0x12; net_mac[4]=0x34; net_mac[5]=0x56;
        }
    }
    com1_str("[net] MAC=");
    for (int i = 0; i < 6; i++) { com1_hex(net_mac[i]); if (i < 5) com1_str(":"); }
    com1_str("\n");

    /* RX descriptors: NIC owns all (PCnet format, both layouts) */
    for (int i = 0; i < NET_RX_RING; i++) {
        uint32_t a = (uint32_t)net_rx_buf(i);
        pnet_bd(i, 0)[0] = (a & 0xFFFFFFu) | (0x8000u << 16);   /* OWN | addr */
        *pnet_bd_w1(i, 0) = (uint16_t)(-NET_BUF_SZ);           /* 2's complement */
        *pnet_bd_w2(i, 0) = 0;
    }
    /* TX descriptors: software owns them */
    for (int i = 0; i < NET_TX_RING; i++) {
        uint32_t a = (uint32_t)net_tx_buf(i);
        pnet_bd(i, 1)[0] = a & 0xFFFFFFu;                      /* no OWN */
        *pnet_bd_w1(i, 1) = 0;
        *pnet_bd_w2(i, 1) = 0;
    }

    if (pnet_layout) {
        /* 16-bit init block: mode=0 normal (accept broadcast+physical) */
        net_init_blk->mode = 0;
        net_init_blk->padr[0] = (uint16_t)(net_mac[0] | (net_mac[1] << 8));
        net_init_blk->padr[1] = (uint16_t)(net_mac[2] | (net_mac[3] << 8));
        net_init_blk->padr[2] = (uint16_t)(net_mac[4] | (net_mac[5] << 8));
        for (int i = 0; i < 4; i++) net_init_blk->ladrf[i] = 0;
        /* ring addresses: low 24 bits = address, top 3 bits = log2(entries) */
        net_init_blk->rx_ring = ((uint32_t)net_rx_ring & 0xFFFFFFu) |
                                ((uint32_t)(NET_RX_RING == 8 ? 3 : 4) << 29);
        net_init_blk->tx_ring = ((uint32_t)net_tx_ring & 0xFFFFFFu) |
                                ((uint32_t)(NET_TX_RING == 4 ? 2 : 3) << 29);
    } else {
        /* VMware vlance (AMD LANCE): init block is 12 16-bit words.
         *  0:mode 1-3:padr 4-7:filter 8:rx_ptr 9:rx_len 10:tx_ptr 11:tx_len
         * rx_len/tx_len: addr high 8 bits in bits 7:0, ring length (log2)
         * in bits 15:13.  Same layout as Linux drivers/net/7990.c. */
        volatile uint16_t *ib = (volatile uint16_t *)DMA_BASE;
        ib[0] = 0;                                              /* mode */
        for (int i = 0; i < 6; i++) ((volatile uint8_t *)ib)[2 + i] = net_mac[i];
        for (int i = 0; i < 8; i++) ((volatile uint8_t *)ib)[8 + i] = 0;   /* filter */
        uint32_t rr = DMA_BASE + 0x6000, tr = DMA_BASE + 0x6080;
        ib[8]  = (uint16_t)(rr & 0xFFFF);
        ib[9]  = (uint16_t)((((rr >> 16) & 0xFF)) | (3u << 13)); /* RLEN=3 (8) */
        ib[10] = (uint16_t)(tr & 0xFFFF);
        ib[11] = (uint16_t)((((tr >> 16) & 0xFF)) | (2u << 13)); /* TLEN=2 (4) */
    }

    /* init-block pointer -> CSR1|CSR2<<16 (both layouts) */
    uint32_t ib = (uint32_t)net_init_blk;
    pnet_wcsr(1, (uint16_t)(ib & 0xFFFF));
    pnet_wcsr(2, (uint16_t)(ib >> 16));

    /* (LANCE ring base/length come from the init block itself; VMware's
     * vlance does not take them from CSR6-9, so nothing extra to write.) */

    /* INIT, wait for IDON (emulator sets bit8) */
    pnet_wcsr(0, csr0_init());
    for (int i = 0; i < 2000000; i++) {
        uint16_t c0 = pnet_rcsr(0);
        if (c0 == 0xFFFFu) {                 /* open bus mid-init */
            net_fail_code = NET_FAIL_INIT;
            com1_str("[net] FAIL: CSR0 open-bus during INIT\n");
            return -1;
        }
        if (c0 & csr0_idon()) break;
        if (c0 & 0x0200) break;          /* data-sheet bit9 too */
    }
    if (pnet_layout) { netdbg_init_rcvrl = pnet_rcsr(76); netdbg_init_xmtrl = pnet_rcsr(78); }
    else             { netdbg_init_rcvrl = pnet_rcsr(7);  netdbg_init_xmtrl = pnet_rcsr(9);  }

    /* START */
    pnet_wcsr(0, csr0_start());
    {
        uint16_t c0s = pnet_rcsr(0);
        if (c0s == 0xFFFFu) {                /* device vanished: not ready */
            net_fail_code = NET_FAIL_INIT;
            com1_str("[net] FAIL: CSR0 open-bus after START\n");
            return -1;
        }
    }
    net_ready = 1;
    netdbg_csr0 = pnet_rcsr(0);
    if (!pnet_layout) {
        /* diagnostic dump: every CSR, to confirm the LANCE register map */
        com1_str("[net] csr:");
        for (int r = 0; r < 16; r++) { com1_str(" "); com1_hex(pnet_rcsr(r)); }
        com1_str("\n");
    }
    com1_str("[net] MAC="); com1_hex((uint32_t)(net_mac[0] << 8 | net_mac[1]));
    com1_str(":"); com1_hex((uint32_t)(net_mac[2] << 8 | net_mac[3]));
    com1_str(":"); com1_hex((uint32_t)(net_mac[4] << 8 | net_mac[5]));
    com1_str(" INIT ok, START ok, net_ready=1\n");

    if (!pnet_layout) {
        /* VMware NAT (VMnet8): static fallback addressing (the subnet is
         * host-specific; the shell 'netdiag' / curl commands can be used
         * to probe interactively).  No boot-time ping/HTTP probe: it used
         * to block the whole boot for millions of poll iterations when the
         * gateway did not answer. */
        extern uint8_t net_our_ip[4], net_gw_ip[4];
        net_our_ip[0]=192; net_our_ip[1]=168; net_our_ip[2]=64; net_our_ip[3]=10;
        net_gw_ip[0]=192;  net_gw_ip[1]=168;  net_gw_ip[2]=64;  net_gw_ip[3]=1;
        com1_str("[net] VMware NAT: ip=192.168.64.10 gw=192.168.64.1\n");
    }
    return 1;
}

int net_send(const uint8_t *frame, uint32_t len) {
    netdbg_tx_calls++;
    if (!net_ready || !frame || len == 0 || len > NET_BUF_SZ) { netdbg_tx_last = -1; return -1; }
    uint32_t own = pnet_own_mask();
    int i = net_tx_head;
    for (int w = 0; w < 10000000; w++) {
        task_maybe_yield();
        if (!(pnet_bd(i, 1)[0] & own)) break;          /* previous TX done */
        if (w == 9999999) { netdbg_tx_last = -2; return -2; }
    }
    for (uint32_t k = 0; k < len; k++) net_tx_buf(i)[k] = frame[k];
    /* Ethernet minimum frame is 60 bytes; pad short frames with zeros.
     * (QEMU's slirp tolerates runts, VMware NAT / real NICs drop them.) */
    uint32_t plen = len;
    if (plen < 60) {
        for (uint32_t k = len; k < 60; k++) net_tx_buf(i)[k] = 0;
        plen = 60;
    }
    /* TX length: high nibble ONES=0xF, low 12 bits BCNT = 4096-len (2's comp). */
    *pnet_bd_w1(i, 1) = (uint16_t)(0xF000u | (uint16_t)(4096 - plen));
    uint32_t addr = (uint32_t)net_tx_buf(i);
    /* OWN | STP | ENP at bits 31/25/24 (PCnet descriptor, both layouts) */
    pnet_bd(i, 1)[0] = (addr & 0xFFFFFFu) | ((0x8000u | 0x0200u | 0x0100u) << 16);
    *pnet_bd_w2(i, 1) = 0;
    if (!pnet_layout) {
        com1_str("[net] TX len="); com1_num((int)len); com1_str(" hdr:");
        for (uint32_t b = 0; b < 18 && b < len; b++) { com1_hex(frame[b]); com1_str(" "); }
        com1_str("\n");
    }
    netdbg_tx_word0 = pnet_bd(i, 1)[0];
    pnet_wcsr(0, csr0_tdmd());            /* transmit demand */
    pnet_rcsr(0);                         /* kick: emulator processes TDMD on any CSR read */
    net_tx_head = (i + 1) % NET_TX_RING;
    netdbg_tx_last = 0;
    return 0;
}

int net_recv(uint8_t *frame, uint32_t max) {
    if (!net_ready) return -1;
    uint32_t own = pnet_own_mask();
    for (int k = 0; k < NET_RX_RING; k++) {
        int i = (net_rx_head + k) % NET_RX_RING;
        uint32_t w0 = pnet_bd(i, 0)[0];
        /* completed?  QEMU pcnet clears OWN (bit31) when the frame lands.
         * VMware vlance instead sets SOP|EOP (bits 25/24) and leaves OWN set;
         * CSR0 RINT (0x400) is also latched.  Accept either indication. */
        int done = ((w0 & own) == 0);
        if (!done && (w0 & 0x03000000u)) done = 1;
        if (!done) continue;
        {
            uint32_t len = *pnet_bd_w2(i, 0) & 0x0FFF;   /* MCNT in word2 */
            netdbg_rx_word0 = w0;
            if (len > max) len = max;
            for (uint32_t b = 0; b < len; b++) frame[b] = net_rx_buf(i)[b];
            /* give OWN back and clear SOP/EOP for the next frame */
            pnet_bd(i, 0)[0] = (w0 & ~0x03000000u) | own;
            *pnet_bd_w2(i, 0) = 0;
            net_rx_head = (i + 1) % NET_RX_RING;
            netdbg_rx_frames++;
            return (int)len;
        }
    }
    return 0;
}

void net_poll(void) {
    pnet_rcsr(0);                        /* kick QEMU: runs pcnet_poll_timer,
                                            which drains pending RX into our ring */
    uint8_t f[NET_BUF_SZ];
    int n = net_recv(f, sizeof(f));
    while (n > 0) {
        if (!pnet_layout) {
            com1_str("[net] RX len="); com1_num(n); com1_str(" hdr:");
            for (int b = 0; b < 18 && b < n; b++) { com1_hex(f[b]); com1_str(" "); }
            com1_str("\n");
        }
        net_rx_dispatch(f, (uint32_t)n);   /* defined in netstack.c */
        n = net_recv(f, sizeof(f));
    }
}
