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
    outb(0x3C4, 0x02); outb(0x3C5, 0x04);   /* map mask: PLANE 2 ONLY */
    outb(0x3CE, 0x05); outb(0x3CF, 0x00);   /* gfx mode: no shift        */
    outb(0x3CE, 0x06); outb(0x3CF, 0x04);   /* gfx misc: GRAPHICS MODE
         * (bit0 MUST be 1) + A0000 64K window.  With bit0=0 the A0000
         * window is character mode: every access goes to the TEXT
         * BUFFER via odd/even, plane 2 is unreachable, and the "font"
         * was being sprayed over screen memory -- boot only looked fine
         * because GRUB's BIOS font was still in plane 2, and after a
         * Mode 13h excursion plane 2 held framebuffer garbage which the
         * scanout rendered as the post-gfx stripes. */
}
static void font_end(void) {
    /* Text mode needs planes 0+1 ONLY (0x03).  Leaving the all-planes
     * mask (0x0F) here was the STRIPE BUG: QEMU routes text-mode writes
     * (Seq04 odd/even, non-sequential) as mask &= (addr&1)?0x0a:0x05,
     * so every character byte written to B8000 with mask 0x0F ALSO
     * landed in plane 2 (0x0F & 0x05 = 0x05 = planes 0+2) and every
     * attribute byte in plane 3.  Cell j's char overwrites plane2[j] --
     * font RAM slots 0x00..0x3E (plane2 bytes 0..1999) get smeared with
     * screen text, e.g. the space glyph (slot 0x20 = plane2[1024..1055])
     * became 0x20 on every row -> a 1px vertical line at column 2 of
     * every blank cell: the post-gfx "stripes".  ASCII letters survived
     * because slot 0x41 starts at plane2[2080], beyond the 2000 cells. */
    outb(0x3C4, 0x02); outb(0x3C5, 0x03);   /* map mask: planes 0,1      */
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
    /* printable ASCII only; anything else (blanked cells from '\b',
     * control codepoints, >0xFF) paints as a space -- cp==0 used to
     * paint '?' and every backspace left ghost marks (#103). */
    unsigned char ch = (c->cp >= 0x20 && c->cp <= 0x7F)
                     ? (unsigned char)c->cp : ' ';
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

static void text_dac_reset(void);

static int text_enter(void) {
    outb(0x3C2, 0x67);
    outb(0x3C4, 0x00); outb(0x3C5, 0x03);
    outb(0x3C4, 0x01); outb(0x3C5, 0x01);   /* 8-dot characters (bit0=1).
         * With 9-dot chars QEMU's vga_draw_glyph9 leaves the 9th column
         * of every cell UNWRITTEN when dup9=0, so after a graphics
         * excursion the recycled surface shows stale gfx pixels as a
         * 1px stripe on every cell.  8-dot cells cover the surface
         * completely. */
    outb(0x3C4, 0x02); outb(0x3C5, 0x03);
    outb(0x3C4, 0x03); outb(0x3C5, 0x00);
    outb(0x3C4, 0x04); outb(0x3C5, 0x02);
    /* CRTC.  MUST unlock first: gfx_enter re-armed the write protect
     * bit (CRTC11 bit7), and with it set, writes to 0x00..0x07 are
     * silently dropped -- which is exactly what left gfx's vertical
     * timing in place after a gfx->text roundtrip (vertical stripes). */
    outb(0x3D4, 0x11); outb(0x3D5, 0x0E);       /* unlock CRTC 0-7 */
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
    /* Cursor shape: scanlines 14..15 = a 1px UNDERLINE instead of the old
     * 2px block (13..14).  It blinks via the CRTC (enable bit, no 0x20
     * disable), satisfying "cursor should blink, not a solid block". */
    outb(0x3D4, 0x0A); outb(0x3D5, 0x0E);
    outb(0x3D4, 0x0B); outb(0x3D5, 0x0F);
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
    text_dac_reset();
    for (int i = 0; i < CON_TEXT_HZ_SLOTS; i++) loaded_cp[i] = 0;
    {   /* Wipe planes 0/1/3: Mode 13h chain4 splatters all four planes,
         * and stale chain4 bytes there render as diagonal RGB stripes
         * over the restored text screen (QEMU std-VGA surface cache).
         * A0000 graphics window (GC06=04), map mask 0x0B -> planes
         * 0,1,3; plane 2 (font RAM) was just reloaded and survives. */
        outb(0x3CE, 0x06); outb(0x3CF, 0x04);   /* A0000 window, graphics */
        outb(0x3C4, 0x02); outb(0x3C5, 0x0B);   /* map mask: planes 0,1,3 */
        volatile uint8_t *v = (volatile uint8_t *)0xA0000u;
        for (uint32_t i = 0; i < 0x8000u; i++) v[i] = 0;
        outb(0x3C4, 0x02); outb(0x3C5, 0x03);   /* text mask: planes 0,1
             * MUST be restored here too -- 0x0F would mirror every cell
             * char into plane 2 (font RAM) during the following redraw */
        outb(0x3CE, 0x06); outb(0x3CF, 0x0E);   /* B8000 text window     */
    }
    /* Sequencer reset pulse: forces the VGA (and QEMU's internal surface
     * state) to resynchronise.  Without it QEMU keeps a stale surface
     * after a Mode 13h excursion and scans out diagonal RGB garbage over
     * the restored text screen. */
    outb(0x3C4, 0x00); outb(0x3C5, 0x01);       /* async reset */
    outb(0x3C4, 0x00); outb(0x3C5, 0x03);       /* running */
    /* AC video-off/on toggle: the other half of the standard BIOS
     * mode-set epilogue; QEMU needs the display-state transition to
     * rebuild its internal surface format. */
    (void)inb(0x3DA);
    outb(0x3C0, 0x00);                          /* video off (index 0) */
    (void)inb(0x3DA);
    outb(0x3C0, 0x20);                          /* video on, index 0 */
    /* Cache-buster: QEMU's vga_draw_text keeps a per-cell (char,attr)
     * cache and SKIPS cells whose content is unchanged since the last
     * text frame.  Blank cells (0x20,0x07) are identical before and
     * after a graphics excursion, so their surface pixels keep the
     * stale graphics-era garbage -- the "stripes".  Pre-fill with
     * (0x00,0x00) -- black on black, invisible -- so every real cell
     * written by redraw_all differs from the cache and gets repainted. */
    {
        volatile uint16_t *tv = (volatile uint16_t *)0xB8000u;
        for (int i = 0; i < 80 * 25; i++) tv[i] = 0x0000;
    }
    return 0;
}

static void text_leave(void) { /* the next back-end programs the hardware */ }
static void text_set_palette(uint8_t idx, uint32_t rgb) {
    (void)idx; (void)rgb;               /* 16 fixed VGA colours */
}

/* Restore the standard 16-colour text DAC.  gfx_enter reloads all 256
 * DAC entries with the graphics CLUT; without this, text mode keeps
 * rendering through those leftovers after a roundtrip. */
static void text_dac_reset(void) {
    static const uint8_t pal[16][3] = {
        {0x00,0x00,0x00},{0x00,0x00,0xAA},{0x00,0xAA,0x00},{0x00,0xAA,0xAA},
        {0xAA,0x00,0x00},{0xAA,0x00,0xAA},{0xAA,0x55,0x00},{0xAA,0xAA,0xAA},
        {0x55,0x55,0x55},{0x55,0x55,0xFF},{0x55,0xFF,0x55},{0x55,0xFF,0xFF},
        {0xFF,0x55,0x55},{0xFF,0x55,0xFF},{0xFF,0xFF,0x55},{0xFF,0xFF,0xFF},
    };
    outb(0x3C8, 0x00);
    for (int i = 0; i < 16; i++) {
        outb(0x3C9, pal[i][0] >> 2);
        outb(0x3C9, pal[i][1] >> 2);
        outb(0x3C9, pal[i][2] >> 2);
    }
}

/* Raw register dump readable from the QEMU monitor via `xp` (physical
 * memory) -- immune to on-screen artifacts.  Layout:
 *   [0] Seq01 [1] Seq03 [2] Seq04 [3] GC05 [4] GC06
 *   [5..30]  CRTC 0x00..0x19 (26 regs, 0x11 written as read: skip)
 *   [31] AC10 [32] AC11 [33] AC12 [34] AC13 [35] AC14
 *   [36] total length */
volatile uint8_t vga_reg_dump[128];

static void text_diag(void) {
    char buf[8];
    con_write("VGA regs:\n");
    outb(0x3D4, 0x11); outb(0x3D5, 0x0E);
    outb(0x3C4, 0x01); vga_reg_dump[0] = inb(0x3C5);
    outb(0x3C4, 0x03); vga_reg_dump[1] = inb(0x3C5);
    outb(0x3C4, 0x04); vga_reg_dump[2] = inb(0x3C5);
    outb(0x3CE, 0x05); vga_reg_dump[3] = inb(0x3CF);
    outb(0x3CE, 0x06); vga_reg_dump[4] = inb(0x3CF);
    for (int r = 0; r <= 0x19; r++) {
        if (r == 0x11) { vga_reg_dump[5 + r] = 0xEE; continue; }
        outb(0x3D4, (uint8_t)r); vga_reg_dump[5 + r] = inb(0x3D5);
    }
    (void)inb(0x3DA);
    outb(0x3C0, 0x10 | 0x20); vga_reg_dump[31] = inb(0x3C1);
    outb(0x3C0, 0x11 | 0x20); vga_reg_dump[32] = inb(0x3C1);
    outb(0x3C0, 0x12 | 0x20); vga_reg_dump[33] = inb(0x3C1);
    outb(0x3C0, 0x13 | 0x20); vga_reg_dump[34] = inb(0x3C1);
    outb(0x3C0, 0x14 | 0x20); vga_reg_dump[35] = inb(0x3C1);
    (void)inb(0x3DA);
    outb(0x3C0, 0x20);                    /* re-enable video, index 0 */
    vga_reg_dump[36] = 37;
    /* plane-2 font readback with the CORRECT window sequence: sequential
     * addressing + A0000 graphics map + read map 2.  Dumps glyph 0x00,
     * 0x20, 0x41 and 0xFF (16 bytes each) so the monitor can verify the
     * font RAM actually holds our reloaded glyphs. */
    {
        volatile const uint8_t *fr;
        outb(0x3C4, 0x04); outb(0x3C5, 0x06);   /* sequential */
        outb(0x3C4, 0x02); outb(0x3C5, 0x04);   /* (read: GC04 map 2) */
        outb(0x3CE, 0x05); outb(0x3CF, 0x00);   /* gfx mode: no shift */
        outb(0x3CE, 0x06); outb(0x3CF, 0x04);   /* A0000, GRAPHICS on */
        outb(0x3CE, 0x04); outb(0x3CF, 0x02);   /* read map 2: QEMU uses gr[4]&3 as the plane index */
        fr = (volatile const uint8_t *)0xA0000u;
        for (int i = 0; i < 16; i++) vga_reg_dump[40 + i] = fr[i];
        for (int i = 0; i < 16; i++) vga_reg_dump[56 + i] = fr[0x20 * 32 + i];
        for (int i = 0; i < 16; i++) vga_reg_dump[72 + i] = fr[0x41 * 32 + i];
        for (int i = 0; i < 16; i++) vga_reg_dump[88 + i] = fr[0xFF * 32 + i];
        outb(0x3CE, 0x04); outb(0x3CF, 0x00);
        outb(0x3CE, 0x05); outb(0x3CF, 0x10);
        outb(0x3CE, 0x06); outb(0x3CF, 0x0E);
        outb(0x3C4, 0x04); outb(0x3C5, 0x02);
        outb(0x3C4, 0x02); outb(0x3C5, 0x03);   /* restore text map mask:
             * the 0x04 (plane 2 only) set above would make every later
             * cell write land in FONT RAM and drop the attribute byte */
    }
    vga_reg_dump[104] = 0x5A;             /* magic: dump v2 present */
    for (int i = 0; i < 6; i++) {
        unsigned v = 0;
        if (i == 0) { outb(0x3C4, 0x01); v = inb(0x3C5); }
        else if (i == 1) { outb(0x3CE, 0x05); v = inb(0x3CF); }
        else if (i == 2) { outb(0x3CE, 0x06); v = inb(0x3CF); }
        else if (i == 3) { outb(0x3D4, 0x09); v = inb(0x3D5); }
        else if (i == 4) { outb(0x3D4, 0x13); v = inb(0x3D5); }
        else { (void)inb(0x3DA); outb(0x3C0, 0x10 | 0x20); v = inb(0x3C1); }
        buf[0] = "0123456789ABCDEF"[v >> 4];
        buf[1] = "0123456789ABCDEF"[v & 15];
        buf[2] = ' '; buf[3] = 0;
        con_write("  "); con_write(buf);
    }
    con_write("\nattrpal:");
    /* #100: the AC index/data flip-flop must be reset to INDEX state
     * before EVERY 0x3C0 write.  The old code did inb(0x3DA) once before
     * the loop, so from i=1 onward each outb(0x3C0, i|0x20) was taken as
     * DATA and silently rewrote palette registers -- `vdiag` turned the
     * black background purple.  Also collect first, restore video, then
     * print: con_write() must never run mid-AC-sequence. */
    {
        char pal[8][3];
        for (int i = 0; i < 8; i++) {
            (void)inb(0x3DA);
            outb(0x3C0, (uint8_t)(i | 0x20));   /* PAS=1 => palette read */
            unsigned v = inb(0x3C1);
            vga_reg_dump[113 + i] = (uint8_t)v;
            pal[i][0] = "0123456789ABCDEF"[v >> 4];
            pal[i][1] = "0123456789ABCDEF"[v & 15];
            pal[i][2] = 0;
        }
        (void)inb(0x3DA);
        outb(0x3C0, 0x20);                    /* re-enable video */
        for (int i = 0; i < 8; i++) { con_write(" "); con_write(pal[i]); }
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
    /* #100: the 'A' glyph was already captured into vga_reg_dump[72..79]
     * by the plane-2 readback above.  The old code re-entered the font
     * window, called con_write() while GC05/GC04 were reconfigured, and
     * used con_set_cursor(0,3)/(0,5) -- that overwrote row 3 of the
     * user's screen (the "????????" artifact on the syn.go ls line) and
     * left the cursor somewhere unrelated.  Now: no register writes, no
     * cursor moves, just append to the normal output stream. */
    con_write("\nglyphA: ");
    for (int i = 0; i < 8; i++) {
        unsigned v = vga_reg_dump[72 + i];
        buf[0] = "0123456789ABCDEF"[v >> 4];
        buf[1] = "0123456789ABCDEF"[v & 15];
        buf[2] = 0;
        con_write(buf);
    }
    con_write("\n");
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
    .diag       = text_diag,
    .cell_w     = 8, .cell_h = 16
};
