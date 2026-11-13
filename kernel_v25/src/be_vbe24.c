/*
 * be_vbe24.c -- BACK-END D: Bochs/QEMU dispi 800x600x24bpp LFB + drawing.
 *
 * ABI v2.12.  Unlike be_vbe.c (800x600x32, text-only), this back-end is
 * BOTH a text-cell painter AND the owner of CatOS's first user-visible
 * drawing surface.  The drawing primitives in gfx24.h are non-static;
 * they are called by the shell's `gfx24` command and by the three new
 * ring3 syscalls (gfx_info / gfx_rect / gfx_blit).
 *
 * Why 24bpp and not another 32bpp mode:
 *   - 3 bytes/pixel, no wasted alpha byte, pitch 2400, 1,440,000 B total.
 *   - QEMU std VGA reports 24bpp through DISPI_BPP and routes each pixel
 *     through BGR triple writes; verified pixel-exact with screendump.
 *
 * Deliberately simple compared with be_vbe.c:
 *   - NO hardware panning (VIRT_HEIGHT == 600), so scroll is NULL and the
 *     front-end falls back to a write-only full re-render.  A virtual
 *     framebuffer would let a user rectangle persist off-screen and then
 *     appear after a scroll -- surprising and hard to reason about.
 *   - NO MTRR Write-Combining.  WC is a performance optimisation; mixing
 *     it with userspace-driven pixel writes needs more care than this
 *     first implementation should carry.  1.44 MB of UC writes is fine.
 *
 * The Bochs LFB is taken from PCI BAR0 (16 MiB prefetchable on QEMU std VGA,
 * 0xfd000000).  BAR2 is the 4 KiB MMIO window -- using it as the LFB is
 * the bug that kept the 32bpp path black until v2.12 (issue #83).
 * vbe_lfb_map() installs SUPERVISOR-ONLY PTEs, so ring3 still cannot see
 * the framebuffer; every user draw is copied and performed by the kernel.
 *
 * v2.20 -- VMware SVGA II (15AD:0405) fallback:
 *   VMware has no dispi ports, so `gfx24` / `vbe24` used to fail there.
 *   When the Bochs probe fails, svga_mode_set() supplies an 800x600
 *   32bpp XRGB framebuffer instead.  The RGB24 public API is unchanged;
 *   a runtime pitch / pixel stride / channel-offset table absorbs the
 *   3-byte (Bochs BGR) vs 4-byte (SVGA BGRX) difference.  svga_tick()
 *   keeps the host screen in sync through the SVGA command FIFO.
 */
#include "render.h"
#include "render_be.h"
#include "gfx24.h"
#include "svga.h"
#include "io.h"
#include "paging.h"

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

static volatile uint8_t *lfb;
static uint32_t lfb_phys;
static uint32_t lfb_bar_bytes = 16u * 1024u * 1024u;
static int      active;
static int      svga_backend;

/* Runtime pixel layout.  Bochs dispi: pitch 2400, 3-byte BGR.
 * VMware SVGA II: pitch from the device (3200 on QEMU), 4-byte BGRX. */
static uint32_t g_pitch  = GFX24_PITCH;
static uint8_t  g_stride = 3;
static uint8_t  g_r = 2, g_g = 1, g_b = 0, g_x = 3;

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
        if ((vend == 0x1234u && did == 0x1111u) ||
            (vend == 0x1B36u && did == 0x0100u) ||
            (vend == 0x1AF4u && did == 0x1050u)) {
            uint32_t bar = pci_read32(0, dev, 0, 0x10) & 0xFFFFFFF0u;
            if (bar) {
                uint32_t sz = pci_bar_size(0, dev, 0, 0x10);
                if (sz) lfb_bar_bytes = sz;
                return bar;
            }
        }
    }
    return 0xE0000000u;                 /* old Bochs fixed LFB */
}

/* ---- pixel primitives -------------------------------------------------
 * Logical colour is always 0x00RRGGBB.  The physical byte order comes
 * from the active device: BGR (Bochs 24bpp) or BGRX (SVGA II 32bpp).
 * The 4th byte is forced opaque: QEMU's x8r8g8b8 ignores it, real
 * VMware's ARGB path would otherwise treat the pixel as transparent. */
static inline void put3(int x, int y, uint32_t c) {
    volatile uint8_t *p = lfb + (uint32_t)y * g_pitch
                              + (uint32_t)x * g_stride;
    p[g_b] = (uint8_t)(c);
    p[g_g] = (uint8_t)(c >> 8);
    p[g_r] = (uint8_t)(c >> 16);
    if (g_stride == 4u) p[g_x] = 0xFFu;
}

static int clip(int *x, int *y, int *w, int *h) {
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > GFX24_W) *w = GFX24_W - *x;
    if (*y + *h > GFX24_H) *h = GFX24_H - *y;
    return (*w > 0 && *h > 0);
}

static void fill(int x, int y, int w, int h, uint32_t c) {
    if (!lfb || !clip(&x, &y, &w, &h)) return;
    uint8_t b = (uint8_t)(c), g = (uint8_t)(c >> 8), r = (uint8_t)(c >> 16);
    for (int j = 0; j < h; j++) {
        volatile uint8_t *p = lfb + (uint32_t)(y + j) * g_pitch
                                  + (uint32_t)x * g_stride;
        for (int i = 0; i < w; i++) {
            volatile uint8_t *q = p + (uint32_t)i * g_stride;
            q[g_b] = b; q[g_g] = g; q[g_r] = r;
            if (g_stride == 4u) q[g_x] = 0xFFu;
        }
    }
}

/* ---- public drawing API (gfx24.h) -------------------------------------- */
int gfx24_active(void) { return active; }
uint32_t gfx24_lfb_phys(void) { return lfb_phys; }

int gfx24_info(int what) {
    switch (what) {
    case GFX_INFO_ACTIVE:   return active;
    case GFX_INFO_WIDTH:    return GFX24_W;
    case GFX_INFO_HEIGHT:   return GFX24_H;
    case GFX_INFO_BPP:      return GFX24_BPP;  /* logical RGB24 colour */
    case GFX_INFO_PITCH:    return (int)g_pitch;
    case GFX_INFO_FB_BYTES: return (int)(g_pitch * GFX24_H);
    case GFX_INFO_LFB_PHYS: return (int)lfb_phys;
    /* v2.20 append-only selectors (see gfx24.h). */
    case GFX_INFO_BACKEND:  return svga_backend ? 1 : 0;
    case GFX_INFO_HW_BPP:   return svga_backend ? (int)svga_info(SVGA_INFO_BPP)
                                                : GFX24_BPP;
    case GFX_INFO_STRIDE:   return g_stride;
    default:                return -8;          /* ENOSYS */
    }
}

void gfx24_clear(uint32_t rgb) { fill(0, 0, GFX24_W, GFX24_H, rgb); }
void gfx24_pixel(int x, int y, uint32_t rgb) { fill(x, y, 1, 1, rgb); }
void gfx24_rect(int x, int y, int w, int h, uint32_t rgb) {
    fill(x, y, w, h, rgb);
}

void gfx24_blit(const uint8_t *src, int x, int y, int w, int h) {
    if (!lfb || !src || w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        int dy = y + j;
        if (dy < 0 || dy >= GFX24_H) continue;
        for (int i = 0; i < w; i++) {
            int dx = x + i;
            if (dx < 0 || dx >= GFX24_W) continue;
            const uint8_t *s = src +
                ((uint32_t)j * (uint32_t)w + (uint32_t)i) * 3u;
            volatile uint8_t *p = lfb + (uint32_t)dy * g_pitch
                                      + (uint32_t)dx * g_stride;
            p[g_r] = s[0]; p[g_g] = s[1]; p[g_b] = s[2];
            if (g_stride == 4u) p[g_x] = 0xFFu;   /* RGB24 -> device */
        }
    }
}

/* Fixed picture.  Coordinates/colours are asserted byte-for-byte by
 * tests/gfx24_regress.py; changing anything here requires updating that
 * suite.  Everything is at y >= 120 so the shell's top-row prompt and
 * the software cursor cannot disturb the sampled pixels. */
void gfx24_demo(void) {
    static const uint32_t bars[8] = {
        0x000000, 0xFF0000, 0x00FF00, 0xFFFF00,
        0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF
    };
    gfx24_clear(0x102040);                      /* dark blue backdrop */
    gfx24_rect(100, 120, 100, 100, 0xFF0000);  /* red   */
    gfx24_rect(250, 120, 100, 100, 0x00FF00);  /* green */
    gfx24_rect(100, 270, 100, 100, 0x0000FF);  /* blue  */
    gfx24_rect(250, 270, 100, 100, 0xFFFFFF);  /* white */
    /* 256-step red/blue ramp: proves all 8 bits of R and B survive. */
    for (int x = 0; x < GFX24_W; x++) {
        uint32_t r = (uint32_t)(x * 255 / (GFX24_W - 1));
        gfx24_rect(x, 420, 1, 40, CON_RGB(r, 0, 255 - r));
    }
    for (int i = 0; i < 8; i++)
        gfx24_rect(i * 100, 500, 100, 80, bars[i]);
}

/* ---- render back-end (text cells) -------------------------------------- */
static void bochs_layout(void) {
    g_pitch = GFX24_PITCH;
    g_stride = 3;
    g_b = 0; g_g = 1; g_r = 2; g_x = 3;
    svga_backend = 0;
}

/* QEMU's -vga vmware is built on the common VGA core and STILL answers
 * the Bochs dispi ID port, but its LFB is in BAR1, not at the legacy
 * 0xE0000000 fallback.  Choose by PCI identity first: if a VMware SVGA II
 * function exists it wins, otherwise dispi (QEMU -vga std / Bochs). */
static int v24_enter_bochs(void) {
    uint16_t id = dispi_read(DISPI_ID);

    if (id >= 0xB0C0u && id <= 0xB0C5u) {
        /* QEMU -vga std / Bochs: native 24bpp BGR LFB. */
        bochs_layout();
        lfb_phys = find_lfb();
        if (lfb_bar_bytes < GFX24_FB_BYTES) return -2;
        if (!vbe_lfb_map(lfb_phys, GFX24_FB_BYTES)) return -2;
        lfb = (volatile uint8_t *)lfb_phys;

        dispi_write(DISPI_ENABLE, 0);
        dispi_write(DISPI_XRES, GFX24_W);
        dispi_write(DISPI_YRES, GFX24_H);
        dispi_write(DISPI_BPP, 24);
        dispi_write(DISPI_VIRT_WIDTH, GFX24_W);
        dispi_write(DISPI_VIRT_HEIGHT, GFX24_H);
        dispi_write(DISPI_X_OFFSET, 0);
        dispi_write(DISPI_Y_OFFSET, 0);
        dispi_write(DISPI_ENABLE,
                    DISPI_ENABLED | DISPI_LFB_ENABLED | DISPI_NOCLEARMEM);

        if (dispi_read(DISPI_XRES) != GFX24_W ||
            dispi_read(DISPI_YRES) != GFX24_H ||
            dispi_read(DISPI_BPP) != 24) {
            dispi_write(DISPI_ENABLE, 0);
            return -3;
        }
        active = 1;
        gfx24_clear(0x000000);
        return 0;
    }
    return -1;
}

/* VMware Workstation / QEMU -vga vmware: SVGA II, 32bpp XRGB.
 * The RGB24 drawing API is unchanged; only the pixel layout differs. */
static int v24_enter_svga(void) {
    svga_surface_t s;
    if (svga_mode_set(GFX24_W, GFX24_H, &s) != 0) return -2;
    lfb_phys      = s.phys;
    lfb_bar_bytes = s.bytes;
    lfb           = (volatile uint8_t *)s.phys;
    g_pitch   = s.pitch;
    g_stride  = s.stride;
    g_r = s.off_r; g_g = s.off_g; g_b = s.off_b; g_x = s.off_x;
    svga_backend = 1;
    active = 1;
    gfx24_clear(0x000000);
    svga_flush();
    return 0;
}

static int v24_enter(void) {
    /* PCI identity decides.  On a VMware guest the dispi ID port also
     * answers, but its LFB is NOT at the legacy Bochs address. */
    if (svga_probe()) {
        if (v24_enter_svga() == 0) return 0;
        svga_mode_off();
        bochs_layout();
    }
    return v24_enter_bochs();
}

static void v24_leave(void) {
    if (svga_backend) svga_mode_off();
    else              dispi_write(DISPI_ENABLE, 0);
    active = 0;
    lfb = (volatile uint8_t *)0;
    /* Keep the last pixel layout: `display` and gfx_info(BACKEND) can
     * still tell the user which device supplied the surface.  enter()
     * always resets the layout before the next switch. */
}

static void v24_clear(uint32_t bg_rgb) { fill(0, 0, GFX24_W, GFX24_H, bg_rgb); }

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
    /* #58: 7x15 inked box inside the 8x16 cell -> one blank pixel of
     * letter/line spacing, same treatment as be_gfx. */
    fill(ox, oy, 8, 16, b);
    if (c->cp) {
        const unsigned char *g = render_font_glyph((unsigned char)c->cp);
        for (int j = 0; j < 15; j++) {
            unsigned row = g[j >> 1];
            int shift = (c->attrs & CON_ATTR_ITALIC) && (j >> 1) < 5 ? 1 : 0;
            for (int i = 0; i < 7; i++) {
                if (!(row & (1u << (7 - i)))) continue;
                int dx = i + shift;
                if (dx >= 7) continue;
                put3(ox + dx, oy + j, f);
                if ((c->attrs & CON_ATTR_BOLD) && dx + 1 < 7)
                    put3(ox + dx + 1, oy + j, f);
            }
        }
    }
    if (c->attrs & CON_ATTR_UNDERLINE) fill(ox, oy + 15, 8, 1, f);
    if (c->attrs & CON_ATTR_STRIKE)    fill(ox, oy + 10, 8, 1, f);
}

static void draw_wide(int ox, int oy, const con_cell_t *c,
                      uint32_t f, uint32_t b) {
    const uint8_t *g = render_font_hz(c->cp);
    fill(ox, oy, 16, 16, b);
    if (g) {
        for (int j = 0; j < 16; j++) {
            unsigned row = ((unsigned)g[2 * j] << 8) | g[2 * j + 1];
            for (int i = 0; i < 16; i++) {
                if (!(row & (1u << (15 - i)))) continue;
                put3(ox + i, oy + j, f);
                if ((c->attrs & CON_ATTR_BOLD) && i + 1 < 16)
                    put3(ox + i + 1, oy + j, f);
            }
        }
    }
    if (c->attrs & CON_ATTR_UNDERLINE) fill(ox, oy + 15, 16, 1, f);
    if (c->attrs & CON_ATTR_STRIKE)    fill(ox, oy + 8, 16, 1, f);
}

static void v24_draw_cell(int gx, int gy, const con_cell_t *c) {
    if (!lfb) return;
    if (gx < 0 || gx >= GFX24_W / 8 || gy < 0 || gy >= GFX24_H / 16) return;
    if (c->flags == CELL_F_WIDE_R) return;
    uint32_t f, b;
    resolve_colors(c, &f, &b);
    int ox = gx * 8, oy = gy * 16;
    if (c->flags == CELL_F_WIDE_L) draw_wide(ox, oy, c, f, b);
    else                           draw_narrow(ox, oy, c, f, b);
}

static void v24_set_cursor(int x, int y) { (void)x; (void)y; }
static void v24_draw_cursor(int x, int y, uint32_t fg_rgb) {
    /* #58: block cursor, lower half; render.c blinks it every 0.5 s */
    fill(x * 8, y * 16 + 8, 8, 8, fg_rgb & 0x00FFFFFFu);
}
static void v24_set_palette(uint8_t idx, uint32_t rgb) {
    (void)idx; (void)rgb;          /* truecolour: no hardware CLUT */
}

static void v24_diag(void) {
    if (svga_backend)
        con_write("vbe24 backend: VMware SVGA II 800x600 32bpp XRGB (RGB24 API)\n"
                  "4 B/px, host FIFO UPDATE at 25 Hz, no pan / no MTRR\n");
    else
        con_write("vbe24 backend: Bochs dispi 800x600x24bpp LFB\n"
                  "3 B/px BGR, pitch=2400 fb=1440000 B, no pan / no MTRR\n");
    con_write("100x37 8x16 text cells; gfx_info/gfx_rect/gfx_blit (kernel writes)\n");
    con_write("pitch=0x");
    char b[2];
    for (int i = 7; i >= 0; i--) {
        b[0] = "0123456789ABCDEF"[(g_pitch >> (i * 4)) & 15];
        b[1] = 0;
        con_write(b);
    }
    con_write(" stride=");
    b[0] = (char)('0' + g_stride); b[1] = 0;
    con_write(b);
    con_write("\nlfb=0x");
    for (int i = 7; i >= 0; i--) {
        b[0] = "0123456789ABCDEF"[(lfb_phys >> (i * 4)) & 15];
        b[1] = 0;
        con_write(b);
    }
    con_write(" bar=0x");
    for (int i = 7; i >= 0; i--) {
        b[0] = "0123456789ABCDEF"[(lfb_bar_bytes >> (i * 4)) & 15];
        b[1] = 0;
        con_write(b);
    }
    con_write("\n");
}

render_be_t be_vbe24 = {
    .name       = "vbe24-800x600x24",
    .cols = GFX24_W / 8, .rows = 37,   /* 37 * 16 = 592 px */
    .pixel_w = GFX24_W, .pixel_h = GFX24_H,
    .colors = 16777216u, .bpp = 24,
    .attr_mask  = CON_ATTR_ALL,
    .cjk        = 1,
    .sw_blink   = 1,
    .wc_mode    = 0,
    .hw_scroll  = 0,
    .mode       = CON_MODE_VBE24,
    .enter      = v24_enter,
    .leave      = v24_leave,
    .clear      = v24_clear,
    .draw_cell  = v24_draw_cell,
    .set_cursor = v24_set_cursor,
    .set_palette= v24_set_palette,
    .scroll     = 0,                    /* front-end full re-render */
    .draw_cursor= v24_draw_cursor,
    .diag       = v24_diag
};
