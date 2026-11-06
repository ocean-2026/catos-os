/*
 * be_gfx.c -- BACK-END B: VGA Mode 13h (320x200x256), TEXT CELLS ONLY.
 *
 * POLICY: graphics mode is a text mode with more colours and styles.
 * It is NOT a drawing surface.  Every framebuffer primitive below is
 * `static` -- there is no gfx_putpixel / gfx_fillrect / gfx_rect symbol,
 * no syscall, no CE API slot, and no user mapping of 0xA0000 (PDE 0 is
 * supervisor-only in task_space_create()).  The only public operation is
 * draw_cell(x, y, ch, fg, bg, attrs).
 */
#include "render.h"
#include "render_be.h"
#include "io.h"

#define GFX_W 320
#define GFX_H 200
#define GFX_COLS 40
#define GFX_ROWS 25

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

/* ---- DAC palette: 16 standard colours + 240 greys ---------------------- */
static void set_palette(void) {
    static const uint8_t v[16][3] = {
        {0,0,0},{0,0,170},{0,170,0},{0,170,170},
        {170,0,0},{170,0,170},{170,85,0},{170,170,170},
        {85,85,85},{85,85,255},{85,255,85},{85,255,255},
        {255,85,85},{255,85,255},{255,255,85},{255,255,255}
    };
    outb(0x3C8, 0x00);
    for (int i = 0; i < 16; i++) {
        outb(0x3C9, v[i][0]); outb(0x3C9, v[i][1]); outb(0x3C9, v[i][2]);
    }
    for (int i = 16; i < 256; i++) {
        uint8_t g = (uint8_t)((i - 16) * 255 / 239);
        outb(0x3C9, g); outb(0x3C9, g); outb(0x3C9, g);
    }
}

static void gfx_enter(void) {
    /* sequencer */
    outb(0x3C4, 0x00); outb(0x3C5, 0x01);
    outb(0x3C4, 0x01); outb(0x3C5, 0x01);
    outb(0x3C4, 0x02); outb(0x3C5, 0x0F);
    outb(0x3C4, 0x04); outb(0x3C5, 0x0E);
    /* CRTC */
    outb(0x3D4, 0x00); outb(0x3D5, 0x5F);
    outb(0x3D4, 0x01); outb(0x3D5, 0x4F);
    outb(0x3D4, 0x02); outb(0x3D5, 0x50);
    outb(0x3D4, 0x03); outb(0x3D5, 0x82);
    outb(0x3D4, 0x04); outb(0x3D5, 0x54);
    outb(0x3D4, 0x05); outb(0x3D5, 0x80);
    outb(0x3D4, 0x06); outb(0x3D5, 0x0B);
    outb(0x3D4, 0x07); outb(0x3D5, 0x3E);
    outb(0x3D4, 0x09); outb(0x3D5, 0x40);
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
    /* attribute controller (input status resets the index/data flipflop) */
    (void)inb(0x3DA);
    outb(0x3C0, 0x30); outb(0x3C0, 0x00);
    outb(0x3C0, 0x33); outb(0x3C0, 0x00);
    outb(0x3C0, 0x10); outb(0x3C0, 0x41);
    (void)inb(0x3DA);
    outb(0x3C0, 0x20); outb(0x3C0, 0x00);
    set_palette();
}

static void gfx_leave(void) { /* be_text::enter does the full restore */ }

static void gfx_clear(uint8_t bg) { fill_rect(0, 0, GFX_W, GFX_H, bg); }

static uint8_t dim_color(uint8_t c) {
    if (c < 16) return (uint8_t)(c & (uint8_t)~8);
    return (uint8_t)(16 + (c - 16) / 2);
}

static void gfx_draw_cell(int x, int y, unsigned char ch,
                          uint8_t fg, uint8_t bg, uint8_t attrs) {
    if (x < 0 || x >= GFX_COLS || y < 0 || y >= GFX_ROWS) return;
    int ox = x * 8, oy = y * 8;
    uint8_t f = fg, b = bg;

    if (attrs & CON_ATTR_INVERSE) { uint8_t t = f; f = b; b = t; }
    if (attrs & CON_ATTR_HIDDEN)  f = b;
    if (attrs & CON_ATTR_DIM)     f = dim_color(f);
    /* BLINK needs a timer-driven repaint; accepted but not animated yet. */

    fill_rect(ox, oy, 8, 8, b);
    if (ch) {
        const unsigned char *g = render_font_glyph(ch);
        for (int j = 0; j < 8; j++) {
            unsigned row = g[j];
            int shift = (attrs & CON_ATTR_ITALIC) && j < 5 ? 1 : 0;
            for (int i = 0; i < 8; i++) {
                if (!(row & (1u << (7 - i)))) continue;
                int dx = i + shift;
                if (dx >= 8) continue;                 /* keep inside the cell */
                px(ox + dx, oy + j, f);
                if ((attrs & CON_ATTR_BOLD) && dx + 1 < 8)
                    px(ox + dx + 1, oy + j, f);
            }
        }
    }
    if (attrs & CON_ATTR_UNDERLINE) hline(ox, oy + 7, 8, f);
    if (attrs & CON_ATTR_STRIKE)    hline(ox, oy + 4, 8, f);
}

/* No hardware cursor exists in Mode 13h.  The front-end keeps the logical
 * cursor; this is intentionally a no-op (documented limitation). */
static void gfx_set_cursor(int x, int y) { (void)x; (void)y; }

static void gfx_diag(void) {
    con_write("gfx backend: Mode 13h 320x200x256, 40x25 text cells\n");
    con_write("framebuffer 0xA0000 is supervisor-only; no user drawing API\n");
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
    .name      = "mode13h-40x25-text",
    .cols      = GFX_COLS,
    .rows      = GFX_ROWS,
    .pixel_w   = GFX_W,
    .pixel_h   = GFX_H,
    .colors    = 256,
    .attr_mask = CON_ATTR_ALL & (uint8_t)~CON_ATTR_BLINK,
    .mode      = CON_MODE_GFX,
    .enter     = gfx_enter,
    .leave     = gfx_leave,
    .clear     = gfx_clear,
    .draw_cell = gfx_draw_cell,
    .set_cursor= gfx_set_cursor,
    .diag      = gfx_diag
};
