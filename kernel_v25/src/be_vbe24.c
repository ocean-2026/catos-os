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
 * The LFB is taken from PCI BAR0 (16 MiB prefetchable on QEMU std VGA,
 * 0xfd000000).  BAR2 is the 4 KiB MMIO window -- using it as the LFB is
 * the bug that kept the 32bpp path black until v2.12 (issue #83).
 * vbe_lfb_map() installs SUPERVISOR-ONLY PTEs, so ring3 still cannot see
 * the framebuffer; every user draw is copied and performed by the kernel.
 */
#include "render.h"
#include "render_be.h"
#include "gfx24.h"
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
 * LFB byte order is B,G,R.  rgb arguments are always 0x00RRGGBB. */
static inline void put3(int x, int y, uint32_t c) {
    volatile uint8_t *p = lfb + (uint32_t)y * GFX24_PITCH + (uint32_t)x * 3u;
    p[0] = (uint8_t)(c);               /* blue  */
    p[1] = (uint8_t)(c >> 8);          /* green */
    p[2] = (uint8_t)(c >> 16);         /* red   */
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
        volatile uint8_t *p = lfb + (uint32_t)(y + j) * GFX24_PITCH
                                  + (uint32_t)x * 3u;
        for (int i = 0; i < w; i++) {
            p[i * 3] = b; p[i * 3 + 1] = g; p[i * 3 + 2] = r;
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
    case GFX_INFO_BPP:      return GFX24_BPP;
    case GFX_INFO_PITCH:    return GFX24_PITCH;
    case GFX_INFO_FB_BYTES: return GFX24_FB_BYTES;
    case GFX_INFO_LFB_PHYS: return (int)lfb_phys;
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
            volatile uint8_t *p = lfb + (uint32_t)dy * GFX24_PITCH
                                      + (uint32_t)dx * 3u;
            p[0] = s[2]; p[1] = s[1]; p[2] = s[0];   /* RGB -> BGR */
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
static int v24_enter(void) {
    uint16_t id = dispi_read(DISPI_ID);
    if (id < 0xB0C0u || id > 0xB0C5u) return -1;   /* no VBE hardware */

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

static void v24_leave(void) {
    dispi_write(DISPI_ENABLE, 0);
    active = 0;
    lfb = (volatile uint8_t *)0;
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
    con_write("vbe24 backend: 800x600x24bpp LFB, 100x37 8x16 text cells\n");
    con_write("pitch=2400 fb=1440000 B, BGR triples, no pan / no MTRR\n");
    con_write("user drawing API: gfx_info / gfx_rect / gfx_blit (kernel-side writes)\n");
    con_write("lfb=0x");
    char b[2];
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
