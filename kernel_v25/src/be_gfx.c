/*
 * be_gfx.c -- BACK-END B: VGA Mode 13h (320x200x256), TEXT CELLS ONLY.
 *
 * POLICY: graphics mode is a text mode with more colours and styles.
 * It is NOT a drawing surface.  Every framebuffer primitive below is
 * `static`.  The DAC palette is driven by the front-end's 256-entry CLUT,
 * so user programs can redefine any of the 256 colours (6-bit DAC).
 *
 * v2.9: 40x12 cells of 8x16 (was 40x25 of 8x8); BLINK is animated by
 * the front-end's PIT tick; scrolling moves the framebuffer instead of
 * repainting the whole screen; a software underline cursor is drawn.
 */
#include "render.h"
#include "render_be.h"
#include "io.h"

#define GFX_W 320
#define GFX_H 200
#define GFX_COLS 40
#define GFX_ROWS 12                     /* 12 * 16 = 192 px used */

static volatile uint8_t *fb = (volatile uint8_t *)0xA0000u;

/* ---- private framebuffer primitives (never exported) ------------------- */
static void px(int x, int y, uint8_t c) {
    if (x < 0 || x >= GFX_W || y < 0 || y >= GFX_H) return;
    fb[y * GFX_W + x] = c;
}
static void fill_rect(int x, int y, int w, int h, uint8_t c) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) px(x + i, y + j, c);
}
static void hline(int x, int y, int w, uint8_t c) {
    for (int i = 0; i < w; i++) px(x + i, y, c);
}

/* ---- 6-bit VGA DAC ------------------------------------------------------ */
static void dac_write(uint8_t idx, uint32_t rgb) {
    outb(0x3C8, idx);
    outb(0x3C9, (uint8_t)(CON_RGB_R(rgb) >> 2));
    outb(0x3C9, (uint8_t)(CON_RGB_G(rgb) >> 2));
    outb(0x3C9, (uint8_t)(CON_RGB_B(rgb) >> 2));
}
static void dac_load_all(void) {
    outb(0x3C8, 0x00);
    for (int i = 0; i < 256; i++) {
        uint32_t c = render_clut((uint8_t)i);
        outb(0x3C9, (uint8_t)(CON_RGB_R(c) >> 2));
        outb(0x3C9, (uint8_t)(CON_RGB_G(c) >> 2));
        outb(0x3C9, (uint8_t)(CON_RGB_B(c) >> 2));
    }
}

static int gfx_enter(void) {
    /* sequencer */
    outb(0x3C4, 0x00); outb(0x3C5, 0x01);
    outb(0x3C4, 0x01); outb(0x3C5, 0x01);
    outb(0x3C4, 0x02); outb(0x3C5, 0x0F);
    outb(0x3C4, 0x04); outb(0x3C5, 0x0E);
    /* CRTC.  Standard Mode 13h vertical timing is the same 400-line
     * frame as text mode (BIOS table: 04=55 05=81 06=BF 07=1F, 09=41
     * for the doubled 200-line scanout).  The old bogus values
     * (06=0B: vertical total of 11 lines!) only "worked" because QEMU
     * derives the 13h scanout from other fields.  Unlock CRTC 0-7
     * first -- protection is re-armed by the 0x11 write below. */
    outb(0x3D4, 0x11); outb(0x3D5, 0x0E);
    outb(0x3D4, 0x00); outb(0x3D5, 0x5F);
    outb(0x3D4, 0x01); outb(0x3D5, 0x4F);
    outb(0x3D4, 0x02); outb(0x3D5, 0x50);
    outb(0x3D4, 0x03); outb(0x3D5, 0x82);
    outb(0x3D4, 0x04); outb(0x3D5, 0x55);
    outb(0x3D4, 0x05); outb(0x3D5, 0x81);
    outb(0x3D4, 0x06); outb(0x3D5, 0xBF);
    outb(0x3D4, 0x07); outb(0x3D5, 0x1F);
    outb(0x3D4, 0x09); outb(0x3D5, 0x41);
    outb(0x3D4, 0x10); outb(0x3D5, 0x9C);
    outb(0x3D4, 0x11); outb(0x3D5, 0x8E);
    outb(0x3D4, 0x12); outb(0x3D5, 0x8F);
    outb(0x3D4, 0x13); outb(0x3D5, 0x28);
    outb(0x3D4, 0x14); outb(0x3D5, 0x40);
    outb(0x3D4, 0x15); outb(0x3D5, 0x96);
    outb(0x3D4, 0x16); outb(0x3D5, 0xB9);
    outb(0x3D4, 0x17); outb(0x3D5, 0xA3);
    /* graphics controller */
    outb(0x3CE, 0x00); outb(0x3CF, 0x00);
    outb(0x3CE, 0x01); outb(0x3CF, 0x00);
    outb(0x3CE, 0x02); outb(0x3CF, 0x00);
    outb(0x3CE, 0x03); outb(0x3CF, 0x00);
    outb(0x3CE, 0x04); outb(0x3CF, 0x00);
    outb(0x3CE, 0x05); outb(0x3CF, 0x40);
    outb(0x3CE, 0x06); outb(0x3CF, 0x05);
    outb(0x3CE, 0x07); outb(0x3CF, 0x0F);
    outb(0x3CE, 0x08); outb(0x3CF, 0xFF);
    /* attribute controller */
    (void)inb(0x3DA);
    outb(0x3C0, 0x30); outb(0x3C0, 0x00);
    outb(0x3C0, 0x33); outb(0x3C0, 0x00);
    outb(0x3C0, 0x10); outb(0x3C0, 0x41);
    (void)inb(0x3DA);
    outb(0x3C0, 0x20); outb(0x3C0, 0x00);
    dac_load_all();
    return 0;
}

static void gfx_leave(void) { /* be_text::enter does the full restore */ }
static void gfx_set_palette(uint8_t idx, uint32_t rgb) { dac_write(idx, rgb); }
static void gfx_clear(uint32_t bg_rgb) {
    fill_rect(0, 0, GFX_W, GFX_H, render_quantize(bg_rgb));
}

/* Scroll by moving the framebuffer: a 40x12 repaint is 480 cells, a
 * 320x192 byte move is a single forward copy.  No libc memmove here.
 * VGA memory in the first MiB is not WC-mapped and QEMU emulates it as
 * fast RAM, so a read-modify-write here is acceptable.  Returns 0 =
 * handled. */
static int gfx_scroll(int n, uint32_t bg_rgb) {
    if (n <= 0) return 0;
    if (n > GFX_ROWS) n = GFX_ROWS;
    uint8_t c = render_quantize(bg_rgb);
    uint32_t shift = (uint32_t)n * CON_CELL_H * GFX_W;
    uint32_t used  = (uint32_t)GFX_ROWS * CON_CELL_H * GFX_W;
    uint32_t keep  = used - shift;
    for (uint32_t i = 0; i < keep; i++) fb[i] = fb[i + shift];
    for (uint32_t i = keep; i < (uint32_t)GFX_W * GFX_H; i++) fb[i] = c;
    return 0;
}

static uint8_t dim_index(uint8_t c) {
    if (c < 16) return (uint8_t)(c & (uint8_t)~8);
    return (uint8_t)(16 + (c - 16) / 2);
}

static void resolve_colors(const con_cell_t *c, uint8_t *f, uint8_t *b) {
    uint8_t fi = c->fg_idx, bi = c->bg_idx;
    if (c->attrs & CON_ATTR_INVERSE) { uint8_t t = fi; fi = bi; bi = t; }
    if (c->attrs & CON_ATTR_HIDDEN)  fi = bi;
    if (c->attrs & CON_ATTR_DIM)     fi = dim_index(fi);
    *f = fi; *b = bi;
}

/* ASCII: the 8x8 font is stretched to the unified 8x16 cell.
 * #58: the glyph is inked inside a 7x15 box so there is one blank pixel
 * column/row between neighbours -- the VMware report called the old 8x16
 * packing "too tight"; 7x15 + the 16 px cell gives an effective 9x16
 * advance rhythm without changing the 8 px column grid (a real 9 px grid
 * would break the CJK = 2 x ASCII invariant and every pan/scroll offset). */
static void draw_narrow(int ox, int oy, const con_cell_t *c,
                        uint8_t f, uint8_t b) {
    fill_rect(ox, oy, 8, 16, b);
    if (c->cp) {
        const unsigned char *g = render_font_glyph((unsigned char)c->cp);
        for (int j = 0; j < 15; j++) {
            unsigned row = g[j >> 1];
            int shift = (c->attrs & CON_ATTR_ITALIC) && (j >> 1) < 5 ? 1 : 0;
            for (int i = 0; i < 7; i++) {
                if (!(row & (1u << (7 - i)))) continue;
                int dx = i + shift;
                if (dx >= 7) continue;
                px(ox + dx, oy + j, f);
                if ((c->attrs & CON_ATTR_BOLD) && dx + 1 < 7)
                    px(ox + dx + 1, oy + j, f);
            }
        }
    }
    if (c->attrs & CON_ATTR_UNDERLINE) hline(ox, oy + 15, 8, f);
    if (c->attrs & CON_ATTR_STRIKE)    hline(ox, oy + 10, 8, f);
}

/* CJK: 16x16 glyph == 2 columns x 1 row of the 8x16 grid. */
static void draw_wide(int ox, int oy, const con_cell_t *c,
                      uint8_t f, uint8_t b) {
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

static void gfx_draw_cell(int gx, int gy, const con_cell_t *c) {
    if (gx < 0 || gx >= GFX_COLS || gy < 0 || gy >= GFX_ROWS) return;
    if (c->flags == CELL_F_WIDE_R) return;
    uint8_t f, b;
    resolve_colors(c, &f, &b);
    int ox = gx * 8, oy = gy * CON_CELL_H;
    if (c->flags == CELL_F_WIDE_L) draw_wide(ox, oy, c, f, b);
    else                           draw_narrow(ox, oy, c, f, b);
}

static void gfx_set_cursor(int x, int y) { (void)x; (void)y; }
static void gfx_draw_cursor(int x, int y, uint32_t fg_rgb) {
    /* v2.21: EMPTY cell -> 1 px UNDERLINE on the cell's last scanline
     * (not the old #58 8x8 block).  render.c draws a glyph cell as
     * reverse video instead, and blinks this underline every 0.5 s. */
    hline(x * 8, y * CON_CELL_H + CON_CELL_H - 1, 8, render_quantize(fg_rgb));
}

static void gfx_diag(void) {
    con_write("gfx backend: Mode 13h 320x200x8bpp, 40x12 8x16 text cells\n");
    con_write("CLUT 256 entries via 6-bit DAC; no user drawing API\n");
    outb(0x3D4, 0x11); outb(0x3D5, 0x0E);
    outb(0x3CE, 0x06);
    unsigned v = inb(0x3CF);
    char b[4];
    b[0] = "0123456789ABCDEF"[v >> 4];
    b[1] = "0123456789ABCDEF"[v & 15];
    b[2] = 0;
    con_write("GC[06]="); con_write(b); con_write("\n");
}

render_be_t be_gfx = {
    .name       = "mode13h-40x12-text",
    .cols = GFX_COLS, .rows = GFX_ROWS,
    .pixel_w = GFX_W, .pixel_h = GFX_H,
    .colors = 256, .bpp = 8,
    .attr_mask  = CON_ATTR_ALL,
    .cjk        = 1,
    .sw_blink   = 1,
    .mode       = CON_MODE_GFX,
    .enter      = gfx_enter,
    .leave      = gfx_leave,
    .clear      = gfx_clear,
    .draw_cell  = gfx_draw_cell,
    .set_cursor = gfx_set_cursor,
    .set_palette= gfx_set_palette,
    .scroll     = gfx_scroll,
    .draw_cursor= gfx_draw_cursor,
    .diag       = gfx_diag
};
