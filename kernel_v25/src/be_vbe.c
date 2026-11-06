/*
 * be_vbe.c -- BACK-END C: VBE / Bochs-QEMU dispi, 800x600x32bpp LFB.
 *
 * This is the truecolour path.  It still paints TEXT CELLS ONLY: no
 * putpixel/rect/framebuffer API is exported, and the LFB is mapped
 * supervisor-only by vbe_lfb_map() (ring3 directories copy PDEs 0..15
 * only, so a high LFB is invisible to user programs).
 *
 * Mode setup uses the Bochs/QEMU VBE "dispi" I/O registers (0x1CE/0x1CF)
 * so no real-mode BIOS call is needed from protected mode.  The LFB
 * physical address comes from the VGA PCI BAR2, with 0xE0000000 as a
 * fallback for old Bochs builds.
 *
 * v2.9: 100x37 cells of 8x16; scrolling is a 32-bit framebuffer move
 * (one row = 51,200 dwords) instead of a full 1.9 MB cell repaint;
 * software cursor + timer-driven BLINK.
 *
 * RUNNING STATUS: compiled and linked, NOT verified on QEMU (this host
 * has no qemu-system-i386).  If the hardware probe fails, enter()
 * returns a negative value and con_request_mode(VBE) reports -19
 * without changing the active mode.
 */
#include "render.h"
#include "render_be.h"
#include "io.h"
#include "paging.h"

#define VBE_W 800
#define VBE_H 600
#define VBE_COLS 100
#define VBE_ROWS 37                     /* 37 * 16 = 592 px used */
#define VBE_USED_H (VBE_ROWS * CON_CELL_H)
#define VBE_FB_SIZE ((uint32_t)(VBE_W * VBE_H * 4))

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

static void dispi_write(uint16_t idx, uint16_t val) {
    outw(DISPI_INDEX_PORT, idx);
    outw(DISPI_DATA_PORT, val);
}
static uint16_t dispi_read(uint16_t idx) {
    outw(DISPI_INDEX_PORT, idx);
    return inw(DISPI_DATA_PORT);
}

/* ---- minimal PCI config read (independent of net.c) -------------------- */
static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn,
                           uint8_t off) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)dev << 11) | ((uint32_t)fn << 8) |
                    (off & 0xFCu);
    outl(0x0CF8, addr);
    return inl(0x0CFC);
}

static uint32_t find_lfb(void) {
    for (uint8_t dev = 0; dev < 32; dev++) {
        uint32_t id = pci_read32(0, dev, 0, 0);
        if (id == 0xFFFFFFFFu) continue;
        uint16_t vend = (uint16_t)(id & 0xFFFFu);
        uint16_t did  = (uint16_t)(id >> 16);
        /* QEMU std VGA / QXL / Virtio GPU all expose an LFB in BAR2. */
        if ((vend == 0x1234u && did == 0x1111u) ||
            (vend == 0x1B36u && did == 0x0100u) ||
            (vend == 0x1AF4u && did == 0x1050u)) {
            uint32_t bar = pci_read32(0, dev, 0, 0x18);
            bar &= 0xFFFFFFF0u;             /* memory BAR */
            if (bar) return bar;
        }
    }
    return 0xE0000000u;                     /* old Bochs fixed LFB */
}

static int vbe_enter(void) {
    uint16_t id = dispi_read(DISPI_ID);
    if (id < 0xB0C0u || id > 0xB0C5u) return -1;   /* no VBE hardware */

    lfb_phys = find_lfb();
    if (!vbe_lfb_map(lfb_phys, VBE_FB_SIZE)) return -2;
    lfb = (volatile uint32_t *)lfb_phys;

    /* All probes above leave the display hardware untouched.  From here
     * on we actually reprogram it, so failures must restore VBE off. */
    dispi_write(DISPI_ENABLE, 0);
    dispi_write(DISPI_XRES, VBE_W);
    dispi_write(DISPI_YRES, VBE_H);
    dispi_write(DISPI_BPP, 32);
    dispi_write(DISPI_VIRT_WIDTH, VBE_W);
    dispi_write(DISPI_VIRT_HEIGHT, VBE_H);
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
    return 0;
}

static void vbe_leave(void) { dispi_write(DISPI_ENABLE, 0); }

/* ---- private framebuffer primitives (never exported) ------------------- */
static void px(int x, int y, uint32_t c) {
    if (x < 0 || x >= VBE_W || y < 0 || y >= VBE_H) return;
    lfb[y * VBE_W + x] = c;
}
static void fill_rect(int x, int y, int w, int h, uint32_t c) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) px(x + i, y + j, c);
}
static void hline(int x, int y, int w, uint32_t c) {
    for (int i = 0; i < w; i++) px(x + i, y, c);
}

static void vbe_clear(uint32_t bg_rgb) {
    if (!lfb) return;
    fill_rect(0, 0, VBE_W, VBE_H, bg_rgb & 0x00FFFFFFu);
}

/* One row is 16 * 800 = 12,800 dwords = 51,200 bytes.  A plain 32-bit
 * forward copy is ~40x cheaper than repainting 3700 cells with glyph
 * loops; overlapping regions are safe because dst < src. */
static void vbe_scroll(int n, uint32_t bg_rgb) {
    if (!lfb || n <= 0) return;
    if (n > VBE_ROWS) n = VBE_ROWS;
    uint32_t c = bg_rgb & 0x00FFFFFFu;
    uint32_t shift = (uint32_t)n * CON_CELL_H * VBE_W;
    uint32_t used  = (uint32_t)VBE_USED_H * VBE_W;
    uint32_t keep  = used - shift;
    for (uint32_t i = 0; i < keep; i++) lfb[i] = lfb[i + shift];
    for (uint32_t i = keep; i < (uint32_t)VBE_W * VBE_H; i++) lfb[i] = c;
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
    fill_rect(ox, oy, 8, 16, b);
    if (c->cp) {
        const unsigned char *g = render_font_glyph((unsigned char)c->cp);
        for (int j = 0; j < 16; j++) {
            unsigned row = g[j >> 1];
            int shift = (c->attrs & CON_ATTR_ITALIC) && (j >> 1) < 5 ? 1 : 0;
            for (int i = 0; i < 8; i++) {
                if (!(row & (1u << (7 - i)))) continue;
                int dx = i + shift;
                if (dx >= 8) continue;
                px(ox + dx, oy + j, f);
                if ((c->attrs & CON_ATTR_BOLD) && dx + 1 < 8)
                    px(ox + dx + 1, oy + j, f);
            }
        }
    }
    if (c->attrs & CON_ATTR_UNDERLINE) hline(ox, oy + 14, 8, f);
    if (c->attrs & CON_ATTR_STRIKE)    hline(ox, oy + 8, 8, f);
}

static void draw_wide(int ox, int oy, const con_cell_t *c,
                      uint32_t f, uint32_t b) {
    const uint8_t *g = render_font_hz(c->cp);
    fill_rect(ox, oy, 16, 16, b);
    if (g) {
        for (int j = 0; j < 16; j++) {
            unsigned row = ((unsigned)g[2 * j] << 8) | g[2 * j + 1];
            for (int i = 0; i < 16; i++) {
                if (!(row & (1u << (15 - i)))) continue;
                px(ox + i, oy + j, f);
                if ((c->attrs & CON_ATTR_BOLD) && i + 1 < 16)
                    px(ox + i + 1, oy + j, f);
            }
        }
    }
    if (c->attrs & CON_ATTR_UNDERLINE) hline(ox, oy + 15, 16, f);
    if (c->attrs & CON_ATTR_STRIKE)    hline(ox, oy + 8, 16, f);
}

static void vbe_draw_cell(int gx, int gy, const con_cell_t *c) {
    if (!lfb) return;
    if (gx < 0 || gx >= VBE_COLS || gy < 0 || gy >= VBE_ROWS) return;
    if (c->flags == CELL_F_WIDE_R) return;
    uint32_t f, b;
    resolve_colors(c, &f, &b);
    int ox = gx * 8, oy = gy * CON_CELL_H;
    if (c->flags == CELL_F_WIDE_L) draw_wide(ox, oy, c, f, b);
    else                           draw_narrow(ox, oy, c, f, b);
}

static void vbe_set_cursor(int x, int y) { (void)x; (void)y; }
static void vbe_draw_cursor(int x, int y, uint32_t fg_rgb) {
    hline(x * 8, y * CON_CELL_H + 15, 8, fg_rgb & 0x00FFFFFFu);
}
static void vbe_set_palette(uint8_t idx, uint32_t rgb) {
    (void)idx; (void)rgb;      /* truecolour: no CLUT in hardware */
}

static void vbe_diag(void) {
    con_write("vbe backend: 800x600x32bpp LFB, 100x37 8x16 text cells\n");
    con_write("truecolour RGB per cell; LFB supervisor-only, no draw API\n");
    con_write("dispi id=0x");
    char b[5];
    unsigned v = dispi_read(DISPI_ID);
    b[0] = "0123456789ABCDEF"[(v >> 12) & 15];
    b[1] = "0123456789ABCDEF"[(v >> 8) & 15];
    b[2] = "0123456789ABCDEF"[(v >> 4) & 15];
    b[3] = "0123456789ABCDEF"[v & 15];
    b[4] = 0;
    con_write(b);
    con_write(" lfb=0x");
    for (int i = 7; i >= 0; i--) {
        b[0] = "0123456789ABCDEF"[(lfb_phys >> (i * 4)) & 15];
        b[1] = 0;
        con_write(b);
    }
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
