/*
 * be_text.c -- BACK-END A: VGA text mode 03 (80x25 cells of 8x16).
 *
 * Only this file touches 0xB8000 and the text-mode VGA register set.
 *
 * v2.9: text mode CAN display hanzi.  VGA text cells are already 8x16,
 * exactly half a 16x16 CJK glyph, so a hanzi is two adjacent cells whose
 * character indices point at two dynamically loaded 8x16 font slots.
 * Slots 0x80..0xFF give room for 64 distinct hanzi on screen; the
 * front-end assigns them (con_cell_t::gslot) before every full redraw.
 * The glyph data comes from render_font_hz(), so no font file is needed
 * at runtime.  BLINK here is the real hardware attribute bit (0x80).
 */
#include "render.h"
#include "render_be.h"
#include "io.h"

static volatile uint16_t *vram = (volatile uint16_t *)0xB8000u;

static uint8_t attr_byte(uint32_t fg, uint32_t bg, uint8_t attrs) {
    if (attrs & CON_ATTR_INVERSE) { uint32_t t = fg; fg = bg; bg = t; }
    if (attrs & CON_ATTR_HIDDEN)  fg = bg;
    if (attrs & CON_ATTR_DIM) {
        fg = CON_RGB(CON_RGB_R(fg) >> 1, CON_RGB_G(fg) >> 1,
                     CON_RGB_B(fg) >> 1);
    }
    uint8_t f4 = render_rgb_to_vga(fg) & 0x0F;
    uint8_t b4 = render_rgb_to_vga(bg) & 0x07;
    if (attrs & CON_ATTR_BOLD) f4 |= 8;
    uint8_t a = (uint8_t)((b4 << 4) | f4);
    if (attrs & CON_ATTR_BLINK) a |= 0x80;   /* hardware blink */
    /* ITALIC / UNDERLINE / STRIKE cannot be expressed in VGA text mode. */
    return a;
}

/* ---- plane-2 font RAM access -------------------------------------------
 * The glyphs live in VGA plane 2.  To touch it the sequencer must switch
 * to sequential addressing (odd/even splits bytes between planes 0/1) and
 * the Graphics Misc register must map the CPU window at A0000 -- with the
 * text-mode value (0x06 = 0x0E, memory_map 3) BOTH 0xA0000 and 0xB8000
 * alias the text buffer, which is what v2.9 accidentally relied on: the
 * "font reload" sprayed glyph bytes into the text buffer (boot only
 * survived because redraw_all() overwrote the first 4 KiB again) and the
 * real font RAM kept whatever Mode 13h left there. */
static volatile uint8_t *fontwin = (volatile uint8_t *)0xA0000u;
static void font_begin(void) {
    /* Standard "load font in text mode" sequence.  The old code kept
     * Graphics Misc (0x06) at 0x0E, whose memory-map bits (11) alias the
     * CPU window to B8000 -- the glyph bytes were written into the TEXT
     * BUFFER, the font RAM stayed whatever Mode 13h left there, and the
     * boot path only survived because redraw_all() immediately overwrote
     * the trashed first 4 KiB of text VRAM.  0x00 = A0000 window,
     * graphics off, odd/even off.  Seq 0x04 must also switch to
     * sequential addressing (0x02 = odd/even splits bytes between
     * planes 0 and 1). */
    outb(0x3C4, 0x04); outb(0x3C5, 0x06);   /* seq: sequential + ext mem */
    outb(0x3CE, 0x05); outb(0x3CF, 0x00);   /* gfx mode: no shift        */
    outb(0x3CE, 0x06); outb(0x3CF, 0x00);   /* gfx misc: A0000 window    */
}
static void font_end(void) {
    outb(0x3C4, 0x02); outb(0x3C5, 0x0F);   /* map mask: all planes      */
    outb(0x3C4, 0x04); outb(0x3C5, 0x02);   /* seq: odd/even + ext mem   */
    outb(0x3CE, 0x05); outb(0x3CF, 0x10);   /* gfx mode back             */
    outb(0x3CE, 0x06); outb(0x3CF, 0x0E);   /* gfx misc back: text map   */
}
static void font_put(int idx, int row, unsigned char byte) {
    outb(0x3C4, 0x02); outb(0x3C5, 0x04);   /* map mask -> plane 2 */
    fontwin[32 * idx + row] = byte;
}

static void vga_reload_font(void) {
    font_begin();
    for (int c = 0; c < 256; c++) {
        const unsigned char *g = render_font_glyph((unsigned char)c);
        for (int r = 0; r < 16; r++) font_put(c, r, g[r >> 1]);
    }
    font_end();
}

/* Cached so a full redraw does not rewrite the same glyphs 3700 times. */
static uint32_t loaded_cp[CON_TEXT_HZ_SLOTS];

static void text_load_hz(uint32_t cp, uint8_t slot) {
    if (slot >= CON_TEXT_HZ_SLOTS || loaded_cp[slot] == cp) return;
    const uint8_t *g = render_font_hz(cp);
    int left = 0x80 + 2 * (int)slot;
    font_begin();
    for (int r = 0; r < 16; r++) {
        font_put(left,     r, g ? g[2 * r]     : 0);
        font_put(left + 1, r, g ? g[2 * r + 1] : 0);
    }
    font_end();
    loaded_cp[slot] = cp;
}

static void text_clear(uint32_t bg_rgb) {
    uint8_t a = attr_byte(render_clut(7), bg_rgb, 0);
    uint16_t blank = (uint16_t)' ' | ((uint16_t)a << 8);
    for (int i = 0; i < 80 * 25; i++) vram[i] = blank;
}

static void text_draw_cell(int gx, int gy, const con_cell_t *c) {
    if (gx < 0 || gx >= 80 || gy < 0 || gy >= 25) return;
    if (c->flags == CELL_F_WIDE_R) return;
    uint8_t a = attr_byte(c->fg, c->bg, c->attrs);
    uint16_t word;
    if (c->flags == CELL_F_WIDE_L) {
        uint8_t s = c->gslot;
        if (s >= CON_TEXT_HZ_SLOTS) {         /* no slot -> fallback */
            word = (uint16_t)'?' | ((uint16_t)a << 8);
            vram[gy * 80 + gx] = word;
            if (gx + 1 < 80) vram[gy * 80 + gx + 1] = word;
            return;
        }
        text_load_hz(c->cp, s);
        vram[gy * 80 + gx] =
            (uint16_t)(0x80 + 2 * (int)s) | ((uint16_t)a << 8);
        if (gx + 1 < 80)
            vram[gy * 80 + gx + 1] =
                (uint16_t)(0x81 + 2 * (int)s) | ((uint16_t)a << 8);
        return;
    }
    unsigned char ch = (c->cp && c->cp <= 0x7F) ? (unsigned char)c->cp :
                       (c->cp <= 0xFF ? '?' : ' ');
    word = (uint16_t)ch | ((uint16_t)a << 8);
    vram[gy * 80 + gx] = word;
}

static void text_set_cursor(int x, int y) {
    if (x < 0) x = 0; if (x > 79) x = 79;
    if (y < 0) y = 0; if (y > 24) y = 24;
    uint16_t pos = (uint16_t)(y * 80 + x);
    outb(0x3D4, 14); outb(0x3D5, (uint8_t)(pos >> 8));
    outb(0x3D4, 15); outb(0x3D5, (uint8_t)(pos & 0xFF));
}

static int text_enter(void) {
    outb(0x3C2, 0x67);
    outb(0x3C4, 0x00); outb(0x3C5, 0x03);
    outb(0x3C4, 0x01); outb(0x3C5, 0x00);
    outb(0x3C4, 0x02); outb(0x3C5, 0x03);
    outb(0x3C4, 0x03); outb(0x3C5, 0x00);
    outb(0x3C4, 0x04); outb(0x3C5, 0x02);
    outb(0x3D4, 0x00); outb(0x3D5, 0x5F);
    outb(0x3D4, 0x01); outb(0x3D5, 0x4F);
    outb(0x3D4, 0x02); outb(0x3D5, 0x50);
    outb(0x3D4, 0x03); outb(0x3D5, 0x82);
    outb(0x3D4, 0x04); outb(0x3D5, 0x55);
    outb(0x3D4, 0x05); outb(0x3D5, 0x81);
    outb(0x3D4, 0x06); outb(0x3D5, 0xBF);
    outb(0x3D4, 0x07); outb(0x3D5, 0x1F);
    outb(0x3D4, 0x08); outb(0x3D5, 0x00);
    outb(0x3D4, 0x09); outb(0x3D5, 0x4F);
    outb(0x3D4, 0x0A); outb(0x3D5, 0x0D);
    outb(0x3D4, 0x0B); outb(0x3D5, 0x0E);
    outb(0x3D4, 0x0C); outb(0x3D5, 0x00);
    outb(0x3D4, 0x0D); outb(0x3D5, 0x00);
    outb(0x3D4, 0x0E); outb(0x3D5, 0x00);
    outb(0x3D4, 0x0F); outb(0x3D5, 0x00);
    outb(0x3D4, 0x10); outb(0x3D5, 0x9C);
    outb(0x3D4, 0x11); outb(0x3D5, 0x8E);
    outb(0x3D4, 0x12); outb(0x3D5, 0x8F);
    outb(0x3D4, 0x13); outb(0x3D5, 0x28);
    outb(0x3D4, 0x14); outb(0x3D5, 0x1F);
    outb(0x3D4, 0x15); outb(0x3D5, 0x96);
    outb(0x3D4, 0x16); outb(0x3D5, 0xB9);
    outb(0x3D4, 0x17); outb(0x3D5, 0xA3);
    outb(0x3D4, 0x18); outb(0x3D5, 0xFF);
    outb(0x3CE, 0x00); outb(0x3CF, 0x00);
    outb(0x3CE, 0x01); outb(0x3CF, 0x00);
    outb(0x3CE, 0x02); outb(0x3CF, 0x00);
    outb(0x3CE, 0x03); outb(0x3CF, 0x00);
    outb(0x3CE, 0x04); outb(0x3CF, 0x00);
    outb(0x3CE, 0x05); outb(0x3CF, 0x10);
    outb(0x3CE, 0x06); outb(0x3CF, 0x0E);
    outb(0x3CE, 0x07); outb(0x3CF, 0x00);
    outb(0x3CE, 0x08); outb(0x3CF, 0xFF);
    (void)inb(0x3DA);
    outb(0x3C0, 0x00); outb(0x3C0, 0x00);
    outb(0x3C0, 0x01); outb(0x3C0, 0x01);
    outb(0x3C0, 0x02); outb(0x3C0, 0x02);
    outb(0x3C0, 0x03); outb(0x3C0, 0x03);
    outb(0x3C0, 0x04); outb(0x3C0, 0x04);
    outb(0x3C0, 0x05); outb(0x3C0, 0x05);
    outb(0x3C0, 0x06); outb(0x3C0, 0x14);
    outb(0x3C0, 0x07); outb(0x3C0, 0x07);
    outb(0x3C0, 0x08); outb(0x3C0, 0x38);
    outb(0x3C0, 0x09); outb(0x3C0, 0x39);
    outb(0x3C0, 0x0A); outb(0x3C0, 0x3A);
    outb(0x3C0, 0x0B); outb(0x3C0, 0x3B);
    outb(0x3C0, 0x0C); outb(0x3C0, 0x3C);
    outb(0x3C0, 0x0D); outb(0x3C0, 0x3D);
    outb(0x3C0, 0x0E); outb(0x3C0, 0x3E);
    outb(0x3C0, 0x0F); outb(0x3C0, 0x3F);
    outb(0x3C0, 0x10); outb(0x3C0, 0x0C);
    outb(0x3C0, 0x11); outb(0x3C0, 0x00);
    outb(0x3C0, 0x12); outb(0x3C0, 0x0F);
    outb(0x3C0, 0x13); outb(0x3C0, 0x08);
    outb(0x3C0, 0x14); outb(0x3C0, 0x00);
    outb(0x3C0, 0x20); outb(0x3C0, 0x00);
    (void)inb(0x3DA);
    vga_reload_font();
    for (int i = 0; i < CON_TEXT_HZ_SLOTS; i++) loaded_cp[i] = 0;
    return 0;
}

static void text_leave(void) { /* the next back-end programs the hardware */ }
static void text_set_palette(uint8_t idx, uint32_t rgb) {
    (void)idx; (void)rgb;               /* 16 fixed VGA colours */
}

static void text_diag(void) {
    char buf[8];
    con_write("VGA regs:\n");
    outb(0x3D4, 0x11); outb(0x3D5, 0x0E);
    for (int i = 0; i < 6; i++) {
        unsigned v = 0;
        if (i == 0) { outb(0x3C4, 0x01); v = inb(0x3C5); }
        else if (i == 1) { outb(0x3CE, 0x05); v = inb(0x3CF); }
        else if (i == 2) { outb(0x3CE, 0x06); v = inb(0x3CF); }
        else if (i == 3) { outb(0x3D4, 0x09); v = inb(0x3D5); }
        else if (i == 4) { outb(0x3D4, 0x13); v = inb(0x3D5); }
        else { (void)inb(0x3DA); outb(0x3C0, 0x10); v = inb(0x3C1); }
        buf[0] = "0123456789ABCDEF"[v >> 4];
        buf[1] = "0123456789ABCDEF"[v & 15];
        buf[2] = ' '; buf[3] = 0;
        con_write("  "); con_write(buf);
    }
    con_write("\nattrpal:");
    (void)inb(0x3DA);
    for (int i = 0; i < 8; i++) {
        outb(0x3C0, (uint8_t)i); con_write(" ");
        unsigned v = inb(0x3C1);
        buf[0] = "0123456789ABCDEF"[v >> 4];
        buf[1] = "0123456789ABCDEF"[v & 15];
        buf[2] = 0;
        con_write(buf);
    }
    con_write("\ndac0-2:");
    outb(0x3C7, 0);
    for (int i = 0; i < 3; i++) {
        unsigned v = inb(0x3C9);
        buf[0] = "0123456789ABCDEF"[v >> 4];
        buf[1] = "0123456789ABCDEF"[v & 15];
        buf[2] = 0;
        con_write(" "); con_write(buf);
    }
    con_set_cursor(0, 3);
    con_write("glyphA: ");
    outb(0x3CE, 0x05); outb(0x3CF, 0x00);
    for (int i = 0; i < 8; i++) {
        outb(0x3CE, 0x04); outb(0x3CF, 0x02);
        unsigned v = fontwin[65 * 32 + i];
        buf[0] = "0123456789ABCDEF"[v >> 4];
        buf[1] = "0123456789ABCDEF"[v & 15];
        buf[2] = 0;
        con_write(buf);
    }
    outb(0x3CE, 0x04); outb(0x3CF, 0x00);
    outb(0x3CE, 0x05); outb(0x3CF, 0x10);
    con_set_cursor(0, 5);
}

render_be_t be_text = {
    .name       = "vga-text-80x25",
    .cols = 80, .rows = 25, .pixel_w = 640, .pixel_h = 400,
    .colors = 16, .bpp = 4,
    .attr_mask  = CON_ATTR_BOLD | CON_ATTR_DIM | CON_ATTR_BLINK |
                  CON_ATTR_INVERSE | CON_ATTR_HIDDEN,
    .cjk        = 1,
    .sw_blink   = 0,               /* hardware attribute bit */
    .mode       = CON_MODE_TEXT,
    .enter      = text_enter,
    .leave      = text_leave,
    .clear      = text_clear,
    .draw_cell  = text_draw_cell,
    .set_cursor = text_set_cursor,
    .set_palette= text_set_palette,
    .scroll     = 0,               /* full redraw rebuilds font slots */
    .draw_cursor= 0,               /* hardware cursor */
    .diag       = text_diag
};
