/*
 * be_vbe.c -- BACK-END C: VBE / Bochs-QEMU dispi, 800x600x32bpp LFB.
 *
 * This is the truecolour path.  It still paints TEXT CELLS ONLY: no
 * putpixel/rect/framebuffer API is exported, and the LFB is mapped
 * supervisor-only by vbe_lfb_map() (ring3 directories copy PDEs 0..15
 * only, so a high LFB is invisible to user programs).
 *
 * v2.9.1 -- performance (the front-end cell grid IS the backbuffer):
 *
 * 1. MTRR Write-Combining.  The LFB would otherwise be UC: every 4-byte
 *    write becomes its own slow PCI transaction.  We program one free
 *    variable MTRR over the framebuffer region as WC so the CPU can
 *    coalesce writes (10-20x on real hardware).  The full SDM dance is
 *    done: CD|NW, flush, MTRRs off, program, re-enable, flush, restore.
 *    If no MTRR is free / misaligned / unsupported, we silently keep UC.
 *
 * 2. Hardware scrolling.  We request a VIRTUAL height far taller than
 *    the 600 visible pixels and scroll by reprogramming DISPI_Y_OFFSET
 *    -- zero pixel copies.  The newly exposed line is blanked once
 *    (write-only) before the viewport moves.  When the viewport reaches
 *    the end of the virtual buffer we wrap to offset 0 and return 1 so
 *    the front-end re-renders from the cell grid; no VRAM is ever READ
 *    (a read under WC costs more than re-rendering every cell).
 *
 * 3. No pixel backbuffer.  The authoritative screen copy is the
 *    front-end's cell grid; every pixel we emit is derived from it, so
 *    an extra 1.9 MB RAM mirror would buy nothing.  Fallback when the
 *    hardware cannot pan: be->scroll returns 1 and the front-end does a
 *    write-only full re-render (still no VRAM reads, unlike the old
 *    framebuffer memmove).
 *
 * v2.20: on VMware (and QEMU -vga vmware) there is no dispi interface,
 * so vbe_enter() falls back to the SVGA II LFB.  Panning/MTRR are
 * disabled there; svga_tick() pushes FIFO UPDATE commands from IRQ0.
 * If neither device is present, enter() returns a negative value and
 * con_request_mode(VBE) reports CON_ENODEV without changing the mode.
 */
#include "render.h"
#include "render_be.h"
#include "svga.h"
#include "io.h"
#include "paging.h"

#define VBE_W 800
#define VBE_H 600
#define VBE_COLS 100
#define VBE_ROWS 37                     /* 37 * 16 = 592 px visible */
#define VBE_MAX_VIRT_ROWS 256           /* cap virtual height at 4096 px */
#define VBE_MIN_PAN_ROWS  (VBE_ROWS + 8)/* below this, panning is useless */

#define DISPI_INDEX_PORT 0x01CE
#define DISPI_DATA_PORT  0x01CF

enum {
    DISPI_ID = 0, DISPI_XRES, DISPI_YRES, DISPI_BPP, DISPI_ENABLE,
    DISPI_BANK, DISPI_VIRT_WIDTH, DISPI_VIRT_HEIGHT,
    DISPI_X_OFFSET, DISPI_Y_OFFSET
};
#define DISPI_ENABLED      0x01
#define DISPI_LFB_ENABLED  0x40
#define DISPI_NOCLEARMEM   0x80

static volatile uint32_t *lfb;
static uint32_t lfb_phys;
static uint32_t pitch_px = VBE_W;      /* dwords per scanline (v2.20) */
static int      svga_backend;          /* 1 = VMware SVGA II supplies LFB */
static uint32_t lfb_mapped;             /* bytes actually mapped */
static int      pan_rows;               /* total physical lines (0 = no panning) */
static int      pan_off;                /* current viewport offset in lines */
static int      wc_enabled;             /* MTRR WC programmed successfully */

static void dispi_write(uint16_t idx, uint16_t val) {
    outw(DISPI_INDEX_PORT, idx);
    outw(DISPI_DATA_PORT, val);
}
static uint16_t dispi_read(uint16_t idx) {
    outw(DISPI_INDEX_PORT, idx);
    return inw(DISPI_DATA_PORT);
}

/* ---- minimal PCI config access (independent of net.c) ------------------ */
static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn,
                           uint8_t off) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)dev << 11) | ((uint32_t)fn << 8) |
                    (off & 0xFCu);
    outl(0x0CF8, addr);
    return inl(0x0CFC);
}
static void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn,
                        uint8_t off, uint32_t v) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)dev << 11) | ((uint32_t)fn << 8) |
                    (off & 0xFCu);
    outl(0x0CF8, addr);
    outl(0x0CFC, v);
}

static uint32_t lfb_bar_bytes = 16u * 1024u * 1024u;   /* sane default */

/* Probe a memory BAR's size (type-1 read): write all-ones, read back. */
static uint32_t pci_bar_size(uint8_t bus, uint8_t dev, uint8_t fn,
                             uint8_t off) {
    uint32_t orig = pci_read32(bus, dev, fn, off);
    pci_write32(bus, dev, fn, off, 0xFFFFFFF0u);
    uint32_t v = pci_read32(bus, dev, fn, off);
    pci_write32(bus, dev, fn, off, orig);
    if (v == 0) return 0;
    return (~(v & 0xFFFFFFF0u)) + 1u;
}

static uint32_t find_lfb(void) {
    for (uint8_t dev = 0; dev < 32; dev++) {
        uint32_t id = pci_read32(0, dev, 0, 0);
        if (id == 0xFFFFFFFFu) continue;
        uint16_t vend = (uint16_t)(id & 0xFFFFu);
        uint16_t did  = (uint16_t)(id >> 16);
        /* QEMU std VGA / QXL / Virtio GPU expose the linear framebuffer
         * in BAR0 (16 MiB prefetchable).  BAR2 is only the 4 KiB MMIO
         * window holding the DISPI register mirror + QEMU extensions;
         * using it as the LFB writes pixels past the MMIO aperture and
         * the screen stays black.  Info: `info pci` on QEMU 11.1 shows
         * BAR0 0xfd000000[0xfdffffff], BAR2 0xfebf0000[0xfebf0fff]. */
        if ((vend == 0x1234u && did == 0x1111u) ||
            (vend == 0x1B36u && did == 0x0100u) ||
            (vend == 0x1AF4u && did == 0x1050u)) {
            uint32_t bar = pci_read32(0, dev, 0, 0x10);
            bar &= 0xFFFFFFF0u;             /* memory BAR */
            if (bar) {
                uint32_t sz = pci_bar_size(0, dev, 0, 0x10);
                if (sz) lfb_bar_bytes = sz;
                return bar;
            }
        }
    }
    return 0xE0000000u;                     /* old Bochs fixed LFB */
}

/* ---- MTRR Write-Combining ---------------------------------------------- */
static inline uint64_t rdmsr_(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr_(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr"
                     : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
static int cpu_has_mtrr(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    return (int)((d >> 12) & 1u);
}
static uint32_t maxphyaddr(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(0x80000000u));
    if (a >= 0x80000008u) {
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(0x80000008u));
        uint32_t p = a & 0xFFu;
        if (p >= 32 && p <= 48) return p;
    }
    return 36;
}

/* Program one variable MTRR covering [base, base+size) as WC.
 * Canonical SDM sequence: CD|NW -> wbinvd -> MTRRs off -> wbinvd ->
 * program -> MTRRs on -> wbinvd -> restore CR0.  Interrupt state is
 * preserved (vbe_enter may run with IF=0 through a ring3 syscall gate). */
static int mtrr_set_wc(uint32_t base, uint32_t size) {
    if (!cpu_has_mtrr()) return -1;
    if (base < 0xC0000000u) return -1;      /* never mark system RAM WC */
    if (size == 0 || (size & (size - 1))) return -1;
    if (base & (size - 1)) return -1;

    uint32_t paddr_bits = maxphyaddr();
    /* VCNT lives in MTRRcap (0x0FE) bits 7:0 -- NOT in MTRRdefType,
     * whose low byte is the default memory type. */
    uint64_t cap = rdmsr_(0x0FE);
    uint64_t def = rdmsr_(0x2FF);           /* MTRRdefType: type bits + E */
    uint32_t vcnt = (uint32_t)(cap & 0xFFu);
    if (vcnt == 0) return -2;

    uint32_t efl, cr0;
    __asm__ volatile("pushfl; popl %0" : "=r"(efl));
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    uint32_t cr0_nofill = cr0 | (1u << 30) | (1u << 29);   /* CD | NW */

    __asm__ volatile("cli");
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0_nofill));
    __asm__ volatile("wbinvd");
    wrmsr_(0x2FF, def & ~(1ull << 11));     /* MTRRs disabled */
    __asm__ volatile("wbinvd");

    int slot = -1;
    for (uint32_t i = 0; i < vcnt; i++) {
        if (!(rdmsr_(0x200u + 2 * i) & (1ull << 11))) { slot = (int)i; break; }
    }
    if (slot < 0) {
        wrmsr_(0x2FF, def);
        __asm__ volatile("wbinvd");
        __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
        __asm__ volatile("pushl %0; popfl" :: "r"(efl));
        return -3;
    }

    uint64_t mask = (~(uint64_t)(size - 1)) &
                    ((((1ull << paddr_bits) - 1)) & ~0xFFull);
    mask |= (1ull << 11);                   /* valid */
    wrmsr_(0x200u + 2 * (uint32_t)slot, (uint64_t)base | 1ull);  /* WC */
    wrmsr_(0x200u + 2 * (uint32_t)slot + 1, mask);
    /* Re-enable with the firmware's original default type preserved. */
    wrmsr_(0x2FF, def | (1ull << 11));

    __asm__ volatile("wbinvd");
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
    __asm__ volatile("pushl %0; popfl" :: "r"(efl));
    return 0;
}

/* v2.20: VMware SVGA II has no Bochs dispi ports.  It does provide a
 * 32bpp XRGB LFB with the same B,G,R,x byte order, so this 32-bit-cell
 * painter works unchanged once pitch_px comes from the device.  No
 * hardware panning and no MTRR on this path: both are dispi-specific. */
static int vbe_enter_svga(void) {
    if (!svga_probe()) return -1;
    svga_surface_t s;
    if (svga_mode_set(VBE_W, VBE_H, &s) != 0) return -2;
    if (s.stride != 4u || s.off_b != 0 || s.off_g != 1 || s.off_r != 2) {
        svga_mode_off();                 /* unknown pixel layout */
        return -3;
    }
    lfb_phys      = s.phys;
    lfb_bar_bytes = s.bytes;
    lfb_mapped    = s.pitch * VBE_H;
    lfb           = (volatile uint32_t *)s.phys;
    pitch_px      = s.pitch / 4u;
    pan_rows      = 0;
    pan_off       = 0;
    wc_enabled    = 0;
    svga_backend  = 1;
    be_vbe.wc_mode   = 0;
    be_vbe.hw_scroll = 0;
    return 0;
}

static int vbe_enter_bochs(void) {
    uint16_t id = dispi_read(DISPI_ID);
    if (id < 0xB0C0u || id > 0xB0C5u) return -1;

    svga_backend = 0;
    pitch_px     = VBE_W;
    lfb_phys = find_lfb();

    /* Virtual height for hardware scrolling: as many 16-px lines as the
     * BAR can hold (capped at 4096 px).  Below VBE_MIN_PAN_ROWS lines of
     * headroom, panning buys nothing -- fall back to full re-render. */
    uint32_t max_px = lfb_bar_bytes / (VBE_W * 4u);
    int virt = (int)(max_px / 16u);
    if (virt > VBE_MAX_VIRT_ROWS) virt = VBE_MAX_VIRT_ROWS;
    if (virt < VBE_ROWS) virt = VBE_ROWS;
    pan_rows = (virt >= VBE_MIN_PAN_ROWS) ? virt : 0;
    uint32_t virt_px = (uint32_t)(pan_rows ? virt : VBE_ROWS) * 16u;

    lfb_mapped = VBE_W * 4u * virt_px;
    if (!vbe_lfb_map(lfb_phys, lfb_mapped)) return -2;
    lfb = (volatile uint32_t *)lfb_phys;
    pan_off = 0;

    /* WC over an aligned power-of-two region covering the mapping. */
    wc_enabled = 0;
    {
        uint32_t base = lfb_phys, sz = 1;
        while (sz < lfb_mapped) sz <<= 1;
        while ((base & (sz - 1)) && sz < 0x10000000u) {
            sz <<= 1; base &= ~(sz - 1);
        }
        while (sz < (lfb_phys - base) + lfb_mapped && sz < 0x10000000u)
            sz <<= 1;
        if ((base & (sz - 1)) == 0 &&
            (lfb_phys - base) + lfb_mapped <= sz &&
            mtrr_set_wc(base, sz) == 0)
            wc_enabled = 1;
    }

    /* All probes above leave the display hardware untouched.  From here
     * on we actually reprogram it, so failures must restore VBE off. */
    dispi_write(DISPI_ENABLE, 0);
    dispi_write(DISPI_XRES, VBE_W);
    dispi_write(DISPI_YRES, VBE_H);
    dispi_write(DISPI_BPP, 32);
    dispi_write(DISPI_VIRT_WIDTH, VBE_W);
    dispi_write(DISPI_VIRT_HEIGHT, (uint16_t)virt_px);
    dispi_write(DISPI_X_OFFSET, 0);
    dispi_write(DISPI_Y_OFFSET, 0);
    dispi_write(DISPI_ENABLE,
                DISPI_ENABLED | DISPI_LFB_ENABLED | DISPI_NOCLEARMEM);

    if (dispi_read(DISPI_XRES) != VBE_W ||
        dispi_read(DISPI_YRES) != VBE_H ||
        dispi_read(DISPI_BPP) != 32) {
        dispi_write(DISPI_ENABLE, 0);
        return -3;
    }
    if (dispi_read(DISPI_VIRT_HEIGHT) != (uint16_t)virt_px)
        pan_rows = 0;                       /* virtual height rejected */
    be_vbe.wc_mode    = (uint8_t)(wc_enabled ? 1 : 0);
    be_vbe.hw_scroll  = (uint8_t)(pan_rows ? 1 : 0);
    return 0;
}

/* PCI identity decides: QEMU -vga vmware answers the dispi ID port too,
 * but its LFB lives in the SVGA II BAR1.  SVGA wins whenever present. */
static int vbe_enter(void) {
    if (svga_probe()) {
        if (vbe_enter_svga() == 0) return 0;
        svga_mode_off();
        svga_backend = 0;
        pitch_px = VBE_W;
    }
    return vbe_enter_bochs();
}

static void vbe_leave(void) {
    if (svga_backend) { svga_mode_off(); svga_backend = 0; }
    else              dispi_write(DISPI_ENABLE, 0);
    pan_off = 0;
    pitch_px = VBE_W;
}

/* ---- private framebuffer primitives (never exported) -------------------
 * All coordinates are ABSOLUTE LFB pixels; with panning the visible
 * logical row gy lives at physical pixels (gy + pan_off) * 16. */

static void px_abs(int x, int y, uint32_t c) {
    if (x < 0 || x >= VBE_W || y < 0) return;
    uint32_t limit = (uint32_t)(pan_rows ? pan_rows : (VBE_H / 16)) * 16u;
    if ((uint32_t)y >= limit) return;
    /* SVGA XRGB: force the unused high byte opaque.  QEMU's x8r8g8b8
     * ignores it; VMware's ARGB path would otherwise drop the pixel. */
    lfb[y * pitch_px + x] = svga_backend ? (c | 0xFF000000u) : c;
}
static void fill_abs(int x, int y, int w, int h, uint32_t c) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) px_abs(x + i, y + j, c);
}
static void hline_abs(int x, int y, int w, uint32_t c) {
    for (int i = 0; i < w; i++) px_abs(x + i, y, c);
}

static void vbe_clear(uint32_t bg_rgb) {
    if (!lfb) return;
    uint32_t c = bg_rgb & 0x00FFFFFFu;
    pan_off = 0;
    if (pan_rows) dispi_write(DISPI_Y_OFFSET, 0);
    /* Reset the viewport FIRST, then blank the visible 37 lines (write
     * only).  Stale pixels beyond the viewport are blanked on demand as
     * the viewport advances over them. */
    fill_abs(0, 0, VBE_W, VBE_ROWS * 16, c);
}

/* Scroll n cell rows WITHOUT touching visible pixels: blank the not-yet-
 * visible strip (write-only), then move the viewport.  Returns 0 when
 * handled, 1 to ask the front-end for a full re-render (wrap, or panning
 * unsupported).  Never reads VRAM. */
static int vbe_scroll(int n, uint32_t bg_rgb) {
    if (!lfb || !pan_rows || n <= 0) return 1;
    int noff = pan_off + n;
    if (noff + VBE_ROWS > pan_rows) {
        pan_off = 0;
        dispi_write(DISPI_Y_OFFSET, 0);
        return 1;                           /* wrap: re-render from grid */
    }
    fill_abs(0, (pan_off + VBE_ROWS) * 16, VBE_W, n * 16,
             bg_rgb & 0x00FFFFFFu);
    pan_off = noff;
    dispi_write(DISPI_Y_OFFSET, (uint16_t)(pan_off * 16));
    return 0;
}

static uint32_t dim_rgb(uint32_t c) {
    return CON_RGB(CON_RGB_R(c) >> 1, CON_RGB_G(c) >> 1,
                   CON_RGB_B(c) >> 1);
}
static void resolve_colors(const con_cell_t *c, uint32_t *f, uint32_t *b) {
    uint32_t fi = c->fg, bi = c->bg;
    if (c->attrs & CON_ATTR_INVERSE) { uint32_t t = fi; fi = bi; bi = t; }
    if (c->attrs & CON_ATTR_HIDDEN)  fi = bi;
    if (c->attrs & CON_ATTR_DIM)     fi = dim_rgb(fi);
    *f = fi & 0x00FFFFFFu;
    *b = bi & 0x00FFFFFFu;
}

static void draw_narrow(int ox, int oy, const con_cell_t *c,
                        uint32_t f, uint32_t b) {
    fill_abs(ox, oy, 8, 16, b);
    if (c->cp) {
        const unsigned char *g = render_font_glyph((unsigned char)c->cp);
        /* #58: 7x15 inked box -> 1 px letter/line spacing inside 8x16 */
        for (int j = 0; j < 15; j++) {
            unsigned row = g[j >> 1];
            int shift = (c->attrs & CON_ATTR_ITALIC) && (j >> 1) < 5 ? 1 : 0;
            for (int i = 0; i < 7; i++) {
                if (!(row & (1u << (7 - i)))) continue;
                int dx = i + shift;
                if (dx >= 7) continue;
                px_abs(ox + dx, oy + j, f);
                if ((c->attrs & CON_ATTR_BOLD) && dx + 1 < 7)
                    px_abs(ox + dx + 1, oy + j, f);
            }
        }
    }
    if (c->attrs & CON_ATTR_UNDERLINE) hline_abs(ox, oy + 15, 8, f);
    if (c->attrs & CON_ATTR_STRIKE)    hline_abs(ox, oy + 10, 8, f);
}

static void draw_wide(int ox, int oy, const con_cell_t *c,
                      uint32_t f, uint32_t b) {
    const uint8_t *g = render_font_hz(c->cp);
    fill_abs(ox, oy, 16, 16, b);
    if (g) {
        for (int j = 0; j < 16; j++) {
            unsigned row = ((unsigned)g[2 * j] << 8) | g[2 * j + 1];
            for (int i = 0; i < 16; i++) {
                if (!(row & (1u << (15 - i)))) continue;
                px_abs(ox + i, oy + j, f);
                if ((c->attrs & CON_ATTR_BOLD) && i + 1 < 16)
                    px_abs(ox + i + 1, oy + j, f);
            }
        }
    }
    if (c->attrs & CON_ATTR_UNDERLINE) hline_abs(ox, oy + 15, 16, f);
    if (c->attrs & CON_ATTR_STRIKE)    hline_abs(ox, oy + 8, 16, f);
}

static void vbe_draw_cell(int gx, int gy, const con_cell_t *c) {
    if (!lfb) return;
    if (gx < 0 || gx >= VBE_COLS || gy < 0 || gy >= VBE_ROWS) return;
    if (c->flags == CELL_F_WIDE_R) return;
    uint32_t f, b;
    resolve_colors(c, &f, &b);
    int ox = gx * 8, oy = (gy + pan_off) * 16;
    if (c->flags == CELL_F_WIDE_L) draw_wide(ox, oy, c, f, b);
    else                           draw_narrow(ox, oy, c, f, b);
}

static void vbe_set_cursor(int x, int y) { (void)x; (void)y; }
static void vbe_draw_cursor(int x, int y, uint32_t fg_rgb) {
    /* v2.21: render.c only calls draw_cursor for an EMPTY cell (a cell
     * that holds a glyph is drawn as reverse video instead), so the
     * cursor must be a 1 px UNDERLINE on the cell's last scanline --
     * not the old #58 8x8 block.  render.c blinks it every 0.5 s.
     * Absolute LFB pixels: with panning, logical row y lives at
     * physical pixels (y + pan_off) * 16. */
    hline_abs(x * 8, (y + pan_off) * 16 + 15, 8, fg_rgb & 0x00FFFFFFu);
}
static void vbe_set_palette(uint8_t idx, uint32_t rgb) {
    (void)idx; (void)rgb;      /* truecolour: no CLUT in hardware */
}

static void vbe_diag(void) {
    con_write(svga_backend
        ? "vbe backend: VMware SVGA II 800x600x32 XRGB, FIFO UPDATE @25Hz\n"
        : "vbe backend: Bochs dispi 800x600x32bpp LFB\n");
    con_write("100x37 8x16 text cells; truecolour RGB per cell, no draw API\n");
    char b[6];
    if (!svga_backend) {
        con_write("dispi id=0x");
        unsigned v = dispi_read(DISPI_ID);
        b[0] = "0123456789ABCDEF"[(v >> 12) & 15];
        b[1] = "0123456789ABCDEF"[(v >> 8) & 15];
        b[2] = "0123456789ABCDEF"[(v >> 4) & 15];
        b[3] = "0123456789ABCDEF"[v & 15];
        b[4] = 0;
        con_write(b);
        con_write(" ");
    }
    con_write("lfb=0x");
    for (int i = 7; i >= 0; i--) {
        b[0] = "0123456789ABCDEF"[(lfb_phys >> (i * 4)) & 15];
        b[1] = 0;
        con_write(b);
    }
    con_write("\nbar=0x");
    for (int i = 7; i >= 0; i--) {
        b[0] = "0123456789ABCDEF"[(lfb_bar_bytes >> (i * 4)) & 15];
        b[1] = 0;
        con_write(b);
    }
    con_write(" mapped=0x");
    for (int i = 7; i >= 0; i--) {
        b[0] = "0123456789ABCDEF"[(lfb_mapped >> (i * 4)) & 15];
        b[1] = 0;
        con_write(b);
    }
    con_write(wc_enabled ? " mtrr=WC" : " mtrr=UC(fallback)");
    con_write(pan_rows ? " scroll=HW-pan" : " scroll=re-render");
    con_write("\n");
}

render_be_t be_vbe = {
    .name       = "vbe-800x600x32-text",
    .cols = VBE_COLS, .rows = VBE_ROWS,
    .pixel_w = VBE_W, .pixel_h = VBE_H,
    .colors = 16777216u, .bpp = 32,
    .attr_mask  = CON_ATTR_ALL,
    .cjk        = 1,
    .sw_blink   = 1,
    .mode       = CON_MODE_VBE,
    .enter      = vbe_enter,
    .leave      = vbe_leave,
    .clear      = vbe_clear,
    .draw_cell  = vbe_draw_cell,
    .set_cursor = vbe_set_cursor,
    .set_palette= vbe_set_palette,
    .scroll     = vbe_scroll,
    .draw_cursor= vbe_draw_cursor,
    .diag       = vbe_diag
};
