#ifndef RENDER_BE_H
#define RENDER_BE_H

/*
 * render_be.h -- the BACK-END contract (v2.9).  Private to the rendering
 * layer.
 *
 * A back-end is a dumb painter: it receives one styled logical cell and
 * pushes it to hardware.  It owns NO console state (no cursor, no scroll
 * position, no current style, no UTF-8 state) -- all of that is render.c.
 *
 * Since v2.12 the 24bpp back-end ALSO exports kernel-side drawing calls
 * (gfx24.h) reached through syscalls.  It is the only back-end that does
 * so; the framebuffer stays supervisor-only on every mode.
 *
 * v2.9: ONE unified cell = 8 x 16 pixels on every back-end.  ASCII glyphs
 * are stretched 8x8 -> 8x16; a 16x16 hanzi occupies exactly 2 columns x
 * 1 row.  This removes the old "even row / scroll two rows" rule, lets
 * ASCII scroll one line at a time, and makes VGA text mode (whose hardware
 * cells really are 8x16) capable of displaying hanzi too.
 */
#include <stdint.h>
#include "render.h"

/* ---- one logical cell ---------------------------------------------------
 * cp == 0            blank cell
 * cp  < 0x1100       8x16 ASCII glyph, 1x1 cell
 * cp >= 0x1100       16x16 CJK glyph, WIDE_L + WIDE_R (2 columns x 1 row)
 *
 * Colours are BOTH a 32-bit RGB (for the truecolour back-end) and a
 * palette index (for the 8bpp indexed back-end).  Direct RGB supplied by
 * set_style_rgb() is quantised once, at style-set time.
 *
 * gslot is only meaningful in text mode: index of the pair of dynamic
 * 8x16 VGA font slots (0x80+2*gslot / 0x81+2*gslot) holding the left and
 * right halves of this hanzi.  CELL_GSLOT_NONE means "not assigned yet".
 */
typedef struct {
    uint32_t cp;
    uint32_t fg, bg;            /* 0x00RRGGBB                       */
    uint8_t  fg_idx, bg_idx;   /* 0..255 CLUT index (quantised at set) */
    uint8_t  attrs;
    uint8_t  flags;
    uint8_t  gslot;
} con_cell_t;

#define CELL_F_SINGLE   0u
#define CELL_F_WIDE_L   1u      /* left half of a 2x1 CJK block     */
#define CELL_F_WIDE_R   2u      /* right half, painted by WIDE_L    */
#define CELL_GSLOT_NONE 0xFFu

#define CON_CELL_W 8
#define CON_CELL_H 16
#define CON_TEXT_HZ_SLOTS 64    /* 128 font slots / 2 halves per char */

typedef struct render_be {
    const char *name;
    uint16_t    cols, rows;     /* logical 8x16 grid                  */
    uint16_t    pixel_w, pixel_h;
    uint32_t    colors;         /* 16 / 256 / 16777216                */
    uint8_t     bpp;            /* 4 / 8 / 32                         */
    uint8_t     attr_mask;      /* supported CON_ATTR_* bits          */
    uint8_t     cjk;            /* can paint 16x16 CJK glyphs         */
    uint8_t     sw_blink;       /* BLINK needs a software timer        */
    uint8_t     wc_mode;        /* framebuffer is Write-Combining      */
    uint8_t     hw_scroll;      /* scroll() moves the viewport only    */
    int         mode;           /* CON_MODE_TEXT / GFX / VBE          */

    /* Program the hardware.  Returns 0, or negative when the hardware is
     * unavailable (e.g. no VBE device).  NOT called by render_init() at
     * boot -- GRUB already set text mode 03. */
    int  (*enter)(void);
    void (*leave)(void);
    void (*clear)(uint32_t bg_rgb);
    void (*draw_cell)(int gx, int gy, const con_cell_t *c);
    void (*set_cursor)(int x, int y);
    void (*set_palette)(uint8_t idx, uint32_t rgb);

    /* v2.9 optional hooks.  NULL is always safe: the front-end falls
     * back to a full redraw.  draw_cursor() overlays a software cursor
     * on an already-painted cell.  scroll(n, bg) scrolls n cell rows and
     * returns 0 when it handled the pixels itself (e.g. VBE hardware
     * panning via DISPI_Y_OFFSET -- zero pixel copies, no VRAM reads);
     * a nonzero return asks the front-end for a full re-render from the
     * cell grid (write-only), which is the correct fallback because a
     * read-modify-write of VRAM is poison under Write-Combining. */
    int  (*scroll)(int n, uint32_t bg_rgb);
    void (*draw_cursor)(int x, int y, uint32_t fg_rgb);
    void (*diag)(void);

    /* v2.25: adjustable glyph scale (vbe24 only today).  NULL = fixed
     * 8x16 cell.  A backend implementing set_scale MUST also update its
     * own cols/rows/cell_w/cell_h before returning 0. */
    uint16_t    cell_w, cell_h;     /* current cell size in pixels       */
    int  (*set_scale)(int s);       /* 0 compact / 1 normal / 2 large    */
} render_be_t;

extern render_be_t be_text;
extern render_be_t be_gfx;
extern render_be_t be_vbe;
extern render_be_t be_vbe24;

/* ---- shared fonts ------------------------------------------------------ */
/* 8x8 ASCII (95 printable glyphs), 8 row bytes, MSB = left pixel.
 * All back-ends stretch each row twice to fill the 8x16 cell. */
const unsigned char *render_font_glyph(unsigned char ch);
/* 16x16 GB2312 hanzi by Unicode codepoint, 32 row bytes (2 per row),
 * MSB = left pixel.  Returns 0 when the codepoint is not covered. */
const uint8_t *render_font_hz(uint32_t cp);
unsigned       render_font_hz_count(void);

/* ---- shared colour helpers -------------------------------------------- */
/* 256-entry colour look-up table owned by the front-end. */
uint32_t render_clut(uint8_t idx);
/* Nearest CLUT index for an arbitrary RGB (used by 8bpp back-ends). */
uint8_t  render_quantize(uint32_t rgb);
/* Nearest of the 16 standard VGA colours (used by the text back-end). */
uint8_t  render_rgb_to_vga(uint32_t rgb);

#endif /* RENDER_BE_H */
