#ifndef RENDER_H
#define RENDER_H

/*
 * render.h -- CatOS display FRONT-END contract (mode-agnostic console).
 *
 * This is the ONLY rendering entry point for the rest of the kernel.
 * No file outside src/render*.c and src/be_*.c may touch VGA registers,
 * 0xB8000 (text VRAM) or 0xA0000 (graphics framebuffer).
 *
 * Layering ("front/back separation"):
 *
 *   shell / fs / net / intr / keyboard / CE syscalls
 *        |  con_*()   <-- this header
 *        v
 *   [front-end] src/render.c   logical cell grid, cursor, style,
 *                             mode policy, user-consent gate
 *        |  render_be_t        (include/render_be.h)
 *        v
 *   [back-end]  src/be_text.c  VGA text 80x25
 *               src/be_gfx.c   Mode 13h 320x200, TEXT CELLS ONLY
 *        |
 *     hardware
 *
 * The front-end owns state; a back-end only knows how to paint one cell.
 * Switching back-ends re-wraps the logical grid, so text survives a mode
 * change and no upper layer has to know which display is active.
 */
#include <stdint.h>

/* 16 standard VGA colours.  Values >= 16 are palette indices and only
 * have a distinct visual meaning on a 256-colour back-end; the text
 * back-end maps them onto the nearest of black/dark-grey/light-grey/white. */
enum con_color {
    CON_BLACK = 0, CON_BLUE = 1, CON_GREEN = 2, CON_CYAN = 3,
    CON_RED = 4, CON_MAGENTA = 5, CON_BROWN = 6, CON_LIGHT_GREY = 7,
    CON_DARK_GREY = 8, CON_LIGHT_BLUE = 9, CON_LIGHT_GREEN = 10,
    CON_LIGHT_CYAN = 11, CON_LIGHT_RED = 12, CON_LIGHT_MAGENTA = 13,
    CON_YELLOW = 14, CON_WHITE = 15
};

/* Style bits.  A back-end advertises which of these it can actually draw
 * (render_be_t::attr_mask); the rest are ignored by that back-end. */
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

enum con_mode   { CON_MODE_TEXT = 0, CON_MODE_GFX = 1, CON_MODE_QUERY = -1 };
enum con_origin { CON_ORIGIN_KERNEL = 0, CON_ORIGIN_CONSOLE = 1,
                  CON_ORIGIN_TASK = 2 };

/* selectors for con_info() / the display_info() syscall */
enum con_info_sel {
    CON_INFO_MODE = 0,      /* CON_MODE_TEXT / CON_MODE_GFX */
    CON_INFO_COLS,          /* logical columns             */
    CON_INFO_ROWS,          /* logical rows                */
    CON_INFO_COLORS,        /* 16 or 256                   */
    CON_INFO_ATTRS,         /* supported style bitmask     */
    CON_INFO_PIXEL_W,       /* physical pixel width        */
    CON_INFO_PIXEL_H        /* physical pixel height       */
};

/* errors (negative, same flavour as the syscall error codes) */
#define CON_EINVAL  (-3)
#define CON_ENOSYS  (-8)
#define CON_EAGAIN  (-11)
#define CON_EACCES  (-13)

/* ---- front-end console API ---------------------------------------------- */
void        render_init(void);
void        con_clear(void);
void        con_putc(char c);
void        con_write(const char *s);
void        con_set_style(uint8_t fg, uint8_t bg, uint8_t attrs);
void        con_set_fg(uint8_t fg);
void        con_set_attrs(uint8_t attrs);
uint8_t     con_get_fg(void);
uint8_t     con_get_attrs(void);
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

/*
 * con_request_mode() is the single choke point for mode changes.
 *   TEXT -> GFX   allowed for every origin, including ring3 tasks.
 *   GFX  -> TEXT  ALWAYS requires a physical 'Y' keypress at the console,
 *                 regardless of origin.  An application cannot supply the
 *                 consent: there is no parameter, no token and no syscall
 *                 that sets it.  See con_ask_consent() in render.c.
 * Returns 0 on success, negative error on denial / invalid argument.
 */
int         con_request_mode(int mode, int origin);

/* Kernel-only emergency exit from graphics mode (panic / fatal fault).
 * Bypasses the consent gate.  Never reachable from ring3. */
int         con_force_text(void);

/* Back-end diagnostic dump (raw VGA registers + font check). */
void        con_diag(void);

#endif /* RENDER_H */
