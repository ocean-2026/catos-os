/*
 * svga.c -- VMware SVGA II framebuffer driver (ABI v2.20).
 *
 * Device layout (QEMU hw/display/vmware_vga.c, Linux vmwgfx):
 *   BAR0  I/O    0x0 index / 0x1 value / 0x2 bios   (SVGA_IO_MUL = 1)
 *   BAR1  MMIO   linear framebuffer (VRAM)
 *   BAR2  MMIO   command FIFO RAM
 *
 * Mode set: ENABLE=0 -> WIDTH/HEIGHT/BPP -> program FIFO header in BAR2 ->
 * CONFIG_DONE=1 -> ENABLE=1.  Once the FIFO is configured the host stops
 * VGA dirty tracking, so raw framebuffer writes are invisible until a
 * SVGA_CMD_UPDATE (5 dwords: cmd,x,y,w,h) is pushed and SYNC is written.
 *
 * register numbers are from VMware's public svga_reg.h; checked against
 * QEMU 8.2 vmware_vga.c.  SVGA II is a 32bpp device (BITS_PER_PIXEL=32,
 * DEPTH reads back 24); the RGB24 drawing API stores B,G,R,0xFF per pixel.
 */
#include "svga.h"
#include "io.h"
#include "paging.h"

/* ---- SVGA register indices -------------------------------------------- */
enum {
    R_ID = 0, R_ENABLE = 1, R_WIDTH = 2, R_HEIGHT = 3,
    R_MAX_WIDTH = 4, R_MAX_HEIGHT = 5, R_DEPTH = 6, R_BPP = 7,
    R_PSEUDO = 8, R_RED_MASK = 9, R_GREEN_MASK = 10, R_BLUE_MASK = 11,
    R_PITCH = 12, R_FB_START = 13, R_FB_OFFSET = 14, R_VRAM_SIZE = 15,
    R_FB_SIZE = 16, R_CAPS = 17, R_MEM_START = 18, R_MEM_SIZE = 19,
    R_CONFIG_DONE = 20, R_SYNC = 21, R_BUSY = 22, R_GUEST_ID = 23
};

#define SVGA_ID_2        0x90000002u
#define SVGA_ID_MASK     0xFFFFFF00u
#define SVGA_CMD_NOP     0u
#define SVGA_CMD_UPDATE  1u
#define FIFO_MIN_WORD    0u
#define FIFO_MAX_WORD    1u
#define FIFO_NEXT_WORD   2u
#define FIFO_STOP_WORD   3u
#define FIFO_CAPS_WORD   4u
#define FIFO_FLAGS_WORD  5u
#define FIFO_MIN_BYTES   64u          /* 16 header dwords, 4-byte aligned */
#define FIFO_CMD_BYTES   20u          /* SVGA_CMD_UPDATE + 4 arguments     */

/* PCI vendor/device */
#define VMWARE_VENDOR    0x15ADu
#define SVGA2_DEVICE     0x0405u
#define SVGA1_DEVICE     0x0710u

static uint16_t io_base;
static uint8_t  pci_bus, pci_dev;
static int      probe_done;           /* 0 = not scanned yet */
static int      probe_result;         /* 1 = SVGA II found, 0 = absent     */
static int      active;
static volatile uint32_t *fifo;
static uint32_t fifo_phys, fifo_bytes;
static uint32_t fifo_min, fifo_max, fifo_next;
static uint32_t fb_phys, fb_bytes, fb_pitch;
static uint32_t id_val, caps_val, width_val, height_val, bpp_val, io_val;
static uint32_t tick_div, flush_count;

/* ---- minimal PCI config access ---------------------------------------- */
static uint32_t pci_rd(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t a = 0x80000000u | ((uint32_t)bus << 16) |
                 ((uint32_t)dev << 11) | ((uint32_t)fn << 8) |
                 (off & 0xFCu);
    outl(0x0CF8, a);
    return inl(0x0CFC);
}
static void pci_wr(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off,
                   uint32_t v) {
    uint32_t a = 0x80000000u | ((uint32_t)bus << 16) |
                 ((uint32_t)dev << 11) | ((uint32_t)fn << 8) |
                 (off & 0xFCu);
    outl(0x0CF8, a);
    outl(0x0CFC, v);
}
static uint32_t bar_size(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t orig = pci_rd(bus, dev, fn, off);
    pci_wr(bus, dev, fn, off, 0xFFFFFFFFu);
    uint32_t v = pci_rd(bus, dev, fn, off);
    pci_wr(bus, dev, fn, off, orig);
    uint32_t mask = (orig & 1u) ? 0xFFFFFFFCu : 0xFFFFFFF0u;
    v &= mask;
    if (!v) return 0;
    return (~v) + 1u;
}

/* ---- indexed register access ------------------------------------------ */
static inline void reg_wr(uint32_t reg, uint32_t val) {
    outl(io_base, reg);
    outl((uint16_t)(io_base + 1u), val);
}
static inline uint32_t reg_rd(uint32_t reg) {
    outl(io_base, reg);
    return inl((uint16_t)(io_base + 1u));
}

/* Byte offset of an 8-bit colour channel inside a little-endian pixel. */
static uint8_t channel_off(uint32_t mask) {
    for (uint32_t i = 0; i < 4; i++)
        if (mask & (0xFFu << (i * 8u))) return (uint8_t)i;
    return 0;
}

/* ---- probe --------------------------------------------------------------
 * Only reads PCI config + the ID register.  Never touches display state. */
int svga_probe(void) {
    if (probe_done) return probe_result;
    probe_done = 1;
    for (uint8_t b = 0; b < 4; b++) {
        for (uint8_t d = 0; d < 32; d++) {
            uint32_t v = pci_rd(b, d, 0, 0);
            if (v == 0xFFFFFFFFu) continue;
            uint16_t vend = (uint16_t)(v & 0xFFFFu);
            uint16_t did  = (uint16_t)(v >> 16);
            if (vend != VMWARE_VENDOR) continue;
            if (did != SVGA2_DEVICE && did != SVGA1_DEVICE) continue;
            uint32_t bar0 = pci_rd(b, d, 0, 0x10);
            if (!(bar0 & 1u)) continue;            /* needs an I/O BAR */
            uint16_t base = (uint16_t)(bar0 & 0xFFFCu);
            if (!base) continue;
            io_base = base;
            io_val  = base;
            id_val  = reg_rd(R_ID);
            if ((id_val & SVGA_ID_MASK) == 0x90000000u) {
                pci_bus = b; pci_dev = d;
                probe_result = 1;
                return 1;
            }
        }
    }
    probe_result = 0;
    return 0;
}

/* ---- FIFO --------------------------------------------------------------- */
static void fifo_init(void) {
    fifo_min  = FIFO_MIN_BYTES;
    fifo_max  = fifo_bytes & ~3u;
    if (fifo_max > 0x10000u) fifo_max = 0x10000u;
    fifo_next = fifo_min;
    fifo[FIFO_MIN_WORD]   = fifo_min;
    fifo[FIFO_MAX_WORD]   = fifo_max;
    fifo[FIFO_NEXT_WORD]  = fifo_min;
    fifo[FIFO_STOP_WORD]  = fifo_min;
    /* CAPABILITIES / FLAGS are host-owned advertisement words.  Do NOT
     * zero them: real VMware may publish RESERVE there and expects the
     * guest to leave them alone.  We deliberately use the legacy plain
     * ring (write NEXT + SYNC), which needs no capability negotiation. */
}

static void fifo_sync(void) {
    reg_wr(R_SYNC, 1u);
    /* BUSY is the host's "sync in progress" flag.  QEMU clears it inside
     * the SYNC write itself; real VMware may need a short spin. */
    for (int i = 0; i < 1024 && (reg_rd(R_BUSY) & 1u); i++) { }
}

/* Publish one full-screen UPDATE.  On wrap we pad the tail with NOP words
 * (SVGA_CMD_INVALID_CMD is an explicit NOP in the QEMU device) so the host
 * consumer walks to MAX and wraps to MIN by itself -- the guest must never
 * rewrite FIFO_STOP. */
void svga_flush(void) {
    if (!active || !fifo) return;
    if (fifo_next + FIFO_CMD_BYTES > fifo_max) {
        while (fifo_next + 4u <= fifo_max) {
            fifo[fifo_next >> 2] = SVGA_CMD_NOP;
            fifo_next += 4u;
        }
        fifo[FIFO_NEXT_WORD] = fifo_next;
        fifo_sync();                   /* host consumes the NOP tail */
        fifo_next = fifo_min;
    }
    uint32_t i = fifo_next >> 2;
    fifo[i]     = SVGA_CMD_UPDATE;
    fifo[i + 1] = 0;                 /* x */
    fifo[i + 2] = 0;                 /* y */
    fifo[i + 3] = width_val;         /* w */
    fifo[i + 4] = height_val;        /* h */
    fifo_next += FIFO_CMD_BYTES;
    fifo[FIFO_NEXT_WORD] = fifo_next;
    fifo_sync();
    flush_count++;
}

void svga_tick(void) {
    if (!active) return;
    if (++tick_div < 4u) return;     /* 100 Hz PIT -> 25 Hz refresh */
    tick_div = 0;
    svga_flush();
}

/* ---- mode set ----------------------------------------------------------- */
int svga_mode_set(int w, int h, svga_surface_t *out) {
    if (!out || w <= 0 || h <= 0 || !svga_probe()) return -1;

    uint32_t bar1 = pci_rd(pci_bus, pci_dev, 0, 0x14) & 0xFFFFFFF0u;
    uint32_t bar2 = pci_rd(pci_bus, pci_dev, 0, 0x18) & 0xFFFFFFF0u;
    uint32_t sz1  = bar_size(pci_bus, pci_dev, 0, 0x14);
    uint32_t sz2  = bar_size(pci_bus, pci_dev, 0, 0x18);

    /* Negotiate SVGA II, then program the mode while disabled. */
    reg_wr(R_ID, SVGA_ID_2);
    reg_wr(R_ENABLE, 0);
    reg_wr(R_WIDTH,  (uint32_t)w);
    reg_wr(R_HEIGHT, (uint32_t)h);
    reg_wr(R_BPP, 32u);             /* SVGA II: 32bpp XRGB, 24-bit colour */

    width_val  = reg_rd(R_WIDTH);
    height_val = reg_rd(R_HEIGHT);
    bpp_val    = reg_rd(R_BPP);
    caps_val   = reg_rd(R_CAPS);
    id_val     = reg_rd(R_ID);
    fb_phys    = reg_rd(R_FB_START) & 0xFFFFFFF0u;
    fb_bytes   = reg_rd(R_FB_SIZE);
    fifo_phys  = reg_rd(R_MEM_START) & 0xFFFFFFF0u;
    fifo_bytes = reg_rd(R_MEM_SIZE);
    fb_pitch   = reg_rd(R_PITCH);

    if (!bar1) bar1 = fb_phys;
    if (!fb_phys)  fb_phys  = bar1;
    if (!fifo_phys) fifo_phys = bar2;
    if (!fb_bytes || fb_bytes > sz1)  fb_bytes  = sz1;
    if (!fifo_bytes || fifo_bytes > sz2) fifo_bytes = sz2;
    if (!fb_pitch) fb_pitch = (uint32_t)w * 4u;

    uint8_t stride = (bpp_val == 24u) ? 3u : 4u;
    uint32_t need  = fb_pitch * (uint32_t)h;
    if (!fb_phys || !fifo_phys || !fb_bytes || !fifo_bytes ||
        width_val != (uint32_t)w || height_val != (uint32_t)h ||
        fb_pitch < (uint32_t)w * stride ||
        fb_bytes < need || fifo_bytes < FIFO_MIN_BYTES + 10u * 1024u)
        goto fail;

    /* Both BARs must be reachable from whatever CR3 is active: a mode
     * switch may be a ring3 syscall.  vbe_lfb_map() mirrors the high PDEs
     * into every task directory (supervisor-only PTEs). */
    if (!vbe_lfb_map(fb_phys, need)) goto fail;
    if (!vbe_lfb_map(fifo_phys, fifo_bytes)) goto fail;
    fifo = (volatile uint32_t *)fifo_phys;
    fifo_init();
    if (fifo_max < fifo_min + 10u * 1024u) goto fail;

    /* Enable the FIFO first, then the SVGA mode.  QEMU stops VGA dirty
     * tracking here, so svga_tick() must keep issuing UPDATE commands. */
    reg_wr(R_CONFIG_DONE, 1u);
    reg_wr(R_GUEST_ID, 0x5010u);    /* "other" guest OS id, harmless */
    reg_wr(R_ENABLE, 1u);

    if (reg_rd(R_ENABLE) != 1u || reg_rd(R_WIDTH) != (uint32_t)w ||
        reg_rd(R_HEIGHT) != (uint32_t)h) goto fail;

    uint8_t ro = channel_off(reg_rd(R_RED_MASK));
    uint8_t go = channel_off(reg_rd(R_GREEN_MASK));
    uint8_t bo = channel_off(reg_rd(R_BLUE_MASK));
    uint8_t xo = 3u;
    if (stride == 4u) {
        for (uint8_t i = 0; i < 4; i++)
            if (i != ro && i != go && i != bo) { xo = i; break; }
    }

    out->phys  = fb_phys;
    out->bytes = fb_bytes;
    out->pitch = fb_pitch;
    out->w = width_val;
    out->h = height_val;
    out->bpp = (uint8_t)bpp_val;
    out->stride = stride;
    out->off_r = ro; out->off_g = go; out->off_b = bo; out->off_x = xo;

    active = 1;
    tick_div = 0;
    svga_flush();                   /* push the freshly blank frame now */
    return 0;

fail:
    reg_wr(R_ENABLE, 0);
    reg_wr(R_CONFIG_DONE, 0);
    active = 0;
    fifo = (volatile uint32_t *)0;
    return -2;
}

void svga_mode_off(void) {
    if (!active) return;
    reg_wr(R_ENABLE, 0);
    reg_wr(R_CONFIG_DONE, 0);
    active = 0;
    fifo = (volatile uint32_t *)0;
}

int svga_active(void) { return active; }

uint32_t svga_info(int what) {
    switch (what) {
    case SVGA_INFO_PRESENT:     return svga_probe() ? 1u : 0u;
    case SVGA_INFO_ACTIVE:      return active ? 1u : 0u;
    case SVGA_INFO_ID:          return id_val;
    case SVGA_INFO_CAPS:        return caps_val;
    case SVGA_INFO_WIDTH:       return width_val;
    case SVGA_INFO_HEIGHT:      return height_val;
    case SVGA_INFO_BPP:         return bpp_val;
    case SVGA_INFO_PITCH:       return fb_pitch;
    case SVGA_INFO_FB_PHYS:     return fb_phys;
    case SVGA_INFO_FB_BYTES:    return fb_bytes;
    case SVGA_INFO_FIFO_PHYS:   return fifo_phys;
    case SVGA_INFO_FIFO_BYTES:  return fifo_bytes;
    case SVGA_INFO_IO_BASE:     return io_val;
    case SVGA_INFO_FLUSHES:     return flush_count;
    default:                    return 0u;
    }
}
