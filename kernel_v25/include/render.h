#ifndef RENDER_H
#define RENDER_H

/*
 * render.h -- CatOS display FRONT-END contract (mode-agnostic console).
 *
 * This is the ONLY rendering entry point for the rest of the kernel.
 * No file outside src/render*.c and src/be_*.c may touch VGA registers,
 * 0xB8000, 0xA0000 or a VBE linear framebuffer.
 *
 *   shell / fs / net / intr / keyboard / CE syscalls
 *        |  con_*()  (or the legacy vga_* facade)
 *        v
 *   [front-end] render.c   logical cell grid, cursor, style, UTF-8,
 *                          256-entry CLUT, mode policy, consent gate
 *        |  render_be_t
 *        v
 *   [back-ends] be_text.c   VGA text 03, 80x25, 16 colours
 *               be_gfx.c    Mode 13h, 40x25, 8bpp indexed, 256 colours
 *               be_vbe.c    VBE LFB 800x600x32, 100x75, truecolour
 *
 * Every back-end paints TEXT CELLS.  None exports a drawing API.
 */
#include <stdint.h>

/* 16 standard VGA colours -- also CLUT indices 0..15. */
enum con_color {
    CON_BLACK = 0, CON_BLUE = 1, CON_GREEN = 2, CON_CYAN = 3,
    CON_RED = 4, CON_MAGENTA = 5, CON_BROWN = 6, CON_LIGHT_GREY = 7,
    CON_DARK_GREY = 8, CON_LIGHT_BLUE = 9, CON_LIGHT_GREEN = 10,
    CON_LIGHT_CYAN = 11, CON_LIGHT_RED = 12, CON_LIGHT_MAGENTA = 13,
    CON_YELLOW = 14, CON_WHITE = 15
};

enum con_attr {
    CON_ATTR_BOLD      = 0x01,
    CON_ATTR_DIM       = 0x02,
    CON_ATTR_ITALIC    = 0x04,
    CON_ATTR_UNDERLINE = 0x08,
    CON_ATTR_BLINK     = 0x10,
    CON_ATTR_INVERSE   = 0x20,
    CON_ATTR_STRIKE    = 0x40,
    CON_ATTR_HIDDEN    = 0x80
};
#define CON_ATTR_ALL 0xFFu

enum con_mode   {
    CON_MODE_TEXT = 0,
    CON_MODE_GFX  = 1,
    CON_MODE_VBE  = 2,
    CON_MODE_QUERY = -1
};
enum con_origin { CON_ORIGIN_KERNEL = 0, CON_ORIGIN_CONSOLE = 1,
                  CON_ORIGIN_TASK = 2 };

enum con_info_sel {
    CON_INFO_MODE = 0,
    CON_INFO_COLS,
    CON_INFO_ROWS,
    CON_INFO_COLORS,
    CON_INFO_ATTRS,
    CON_INFO_PIXEL_W,
    CON_INFO_PIXEL_H,
    CON_INFO_BPP,
    CON_INFO_CJK,
    CON_INFO_HZ_COUNT
};

#define CON_EINVAL  (-3)
#define CON_ENOSYS  (-8)
#define CON_EAGAIN  (-11)
#define CON_EACCES  (-13)
#define CON_ENODEV  (-19)

/* 0x00RRGGBB */
#define CON_RGB(r,g,b) (((uint32_t)(uint8_t)(r) << 16) | \
                        ((uint32_t)(uint8_t)(g) << 8)  | \
                        ((uint32_t)(uint8_t)(b)))
#define CON_RGB_R(c) ((uint8_t)(((c) >> 16) & 0xFFu))
#define CON_RGB_G(c) ((uint8_t)(((c) >> 8)  & 0xFFu))
#define CON_RGB_B(c) ((uint8_t)( (c)        & 0xFFu))

/* ---- front-end console API ---------------------------------------------- */
void        render_init(void);
void        con_clear(void);
void        con_putc(char c);
void        con_write(const char *s);
void        con_write_utf8(const char *s);   /* decode UTF-8, then flush */
void        con_flush_utf8(void);            /* flush a dangling sequence */

/* Colour setters.  The uint8_t forms take CLUT indices (0..255); the
 * _rgb forms take a literal 0x00RRGGBB and work at full depth on VBE. */
void        con_set_style(uint8_t fg_idx, uint8_t bg_idx, uint8_t attrs);
void        con_set_style_rgb(uint32_t fg, uint32_t bg, uint8_t attrs);
void        con_set_fg(uint8_t fg_idx);
void        con_set_fg_rgb(uint32_t rgb);
void        con_set_attrs(uint8_t attrs);
uint8_t     con_get_fg(void);
uint8_t     con_get_attrs(void);

/* User-definable 256-entry palette.  On Mode 13h the RGB is written to
 * the 6-bit VGA DAC; on VBE the CLUT drives indexed cells. */
int         con_set_palette(uint8_t idx, uint32_t rgb);
uint32_t    con_palette(uint8_t idx);

void        con_move_cursor(void);
void        con_set_cursor(uint8_t x, uint8_t y);
void        con_putc_at(uint8_t x, uint8_t y, char c);
void        con_write_at(uint8_t x, uint8_t y, const char *s);

/* ---- display policy / introspection ------------------------------------- */
int         con_get_mode(void);
int         con_cols(void);
int         con_rows(void);
int         con_info(int what);
const char *con_backend_name(void);
const char *con_mode_name(int mode);

/*
 * con_request_mode() is the single choke point for mode changes.
 *   -> GFX / -> VBE   allowed for every origin, including ring3 tasks.
 *   -> TEXT from any non-text mode (GFX or VBE) ALWAYS requires a
 *      physical 'Y' keypress at the console.  No parameter, token or
 *      syscall can supply that consent, and the origin is ignored so
 *      even a shell-typed `text` command must confirm.
 */
int         con_request_mode(int mode, int origin);
int         con_force_text(void);   /* kernel-only emergency, no syscall */
void        con_diag(void);

#endif /* RENDER_H */
