#ifndef RENDER_BE_H
#define RENDER_BE_H

/*
 * render_be.h -- the BACK-END contract.  Private to the rendering layer.
 *
 * A back-end is a dumb painter: it receives one styled text cell at a time
 * and pushes it to the hardware.  It owns NO console state (no cursor, no
 * scroll position, no current style) -- all of that lives in render.c.
 *
 * IMPORTANT: a back-end must never export pixel/rectangle primitives.
 * Graphics mode is a text mode with more colours and styles, not a drawing
 * surface.  Every framebuffer primitive in be_gfx.c is `static`.
 */
#include <stdint.h>

typedef struct render_be {
    const char *name;
    uint8_t     cols, rows;         /* logical text grid            */
    uint16_t    pixel_w, pixel_h;   /* physical pixels              */
    uint16_t    colors;             /* 16 (text) or 256 (graphics)  */
    uint8_t     attr_mask;          /* supported CON_ATTR_* bits    */
    int         mode;               /* CON_MODE_TEXT / CON_MODE_GFX */

    /* Program the hardware for this back-end.  Called on entry only;
     * render_init() at boot does NOT call it (GRUB already set mode 03). */
    void (*enter)(void);
    void (*leave)(void);
    void (*clear)(uint8_t bg);
    void (*draw_cell)(int x, int y, unsigned char ch,
                      uint8_t fg, uint8_t bg, uint8_t attrs);
    void (*set_cursor)(int x, int y);
    void (*diag)(void);
} render_be_t;

extern render_be_t be_text;
extern render_be_t be_gfx;

/* Shared 8x8 bitmap font (ASCII 0x20..0x7E).  Used by the text back-end
 * to re-seed VGA plane 2 after a Mode 13h excursion, and by the graphics
 * back-end to paint cells.  Returns a pointer to 8 row bytes. */
const unsigned char *render_font_glyph(unsigned char ch);

#endif /* RENDER_BE_H */
