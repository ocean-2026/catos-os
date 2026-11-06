#ifndef RENDER_BE_H
#define RENDER_BE_H

/*
 * render_be.h -- the BACK-END contract.  Private to the rendering layer.
 *
 * A back-end is a dumb painter: it receives one styled logical cell and
 * pushes it to hardware.  It owns NO console state (no cursor, no scroll
 * position, no current style, no UTF-8 state) -- all of that is render.c.
 *
 * There is still NO pixel/rectangle drawing API.  Graphics mode and VBE
 * mode are text terminals, not drawing surfaces.  Every framebuffer
 * primitive inside a back-end is `static`.
 */
#include <stdint.h>
#include "render.h"

/* ---- one logical cell ---------------------------------------------------
 * cp == 0            blank cell
 * cp  < 0x1100       8x8 ASCII glyph, 1x1 grid cell
 * cp >= 0x1100       16x16 CJK glyph, 2x2 grid cells (WIDE_TL + 3 CONT)
 *
 * Colours are BOTH a 32-bit RGB (for the truecolour back-end) and a
 * palette index (for the 8bpp indexed back-end).  Direct RGB supplied by
 * set_style_rgb() is quantised once, at style-set time. */
typedef struct {
    uint32_t cp;
    uint32_t fg, bg;            /* 0x00RRGGBB                       */
    uint8_t  fg_idx, bg_idx;   /* 0..255 CLUT index (quantised at set) */
    uint8_t  attrs;
    uint8_t  flags;
} con_cell_t;

#define CELL_F_SINGLE   0u
#define CELL_F_WIDE_TL  1u      /* top-left of a 2x2 CJK block        */
#define CELL_F_CONT     2u      /* continuation cell, not painted      */

typedef struct render_be {
    const char *name;
    uint16_t    cols, rows;     /* logical 8x8 grid                   */
    uint16_t    pixel_w, pixel_h;
    uint32_t    colors;         /* 16 / 256 / 16777216                */
    uint8_t     bpp;            /* 4 / 8 / 32                         */
    uint8_t     attr_mask;      /* supported CON_ATTR_* bits          */
    uint8_t     cjk;            /* can paint 16x16 CJK glyphs         */
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
    void (*diag)(void);
} render_be_t;

extern render_be_t be_text;
extern render_be_t be_gfx;
extern render_be_t be_vbe;

/* ---- shared fonts ------------------------------------------------------ */
/* 8x8 ASCII (95 printable glyphs), 8 row bytes, MSB = left pixel. */
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
