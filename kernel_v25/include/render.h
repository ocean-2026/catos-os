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
 *   [front-end] render.c   logical 8x16 cell grid, cursor, style, UTF-8,
 *                          256-entry CLUT, mode policy, consent gate,
 *                          software cursor + BLINK timer
 *        |  render_be_t
 *        v
 *   [back-ends] be_text.c   VGA text 03, 80x25, 16 colours, CJK via
 *                           dynamic 8x16 font slots
 *               be_gfx.c    Mode 13h, 40x12, 8bpp indexed, 256 colours
 *               be_vbe.c    VBE LFB 800x600x32, 100x37, truecolour
 *               be_vbe24.c  VBE LFB 800x600x24, 100x37, truecolour + drawing
 *
 * Every back-end paints TEXT CELLS.  Since v2.12 be_vbe24.c ALSO exports
 * the kernel-side gfx24_* drawing API (gfx24.h); the framebuffer itself
 * stays supervisor-only and user programs never receive its address.
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
    CON_MODE_VBE24 = 3,        /* 800x600x24, first user-drawable mode */
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
    CON_INFO_HZ_COUNT,
    CON_INFO_CELL_W,        /* 10: logical cell width in pixels (8)   */
    CON_INFO_CELL_H,        /* 11: logical cell height in pixels (16) */
    CON_INFO_BLINK,         /* 12: animated blink supported (0/1)     */
    CON_INFO_SCROLL_FAST,   /* 13: backend does framebuffer scrolling  */
    CON_INFO_WC,            /* 14: framebuffer mapped Write-Combining  */
    CON_INFO_HW_SCROLL,     /* 15: scroll = viewport move, no copies   */
    CON_INFO_CELLS_DRAWN,   /* 16: cells actually painted since boot   */
    CON_INFO_CELLS_SKIPPED, /* 17: cells skipped by the dirty tracker  */
    CON_INFO_CELL_SCALE     /* 18: glyph scale (0 compact 1 normal 2 large) */
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
/* Streaming write: `final` = 0 keeps an incomplete UTF-8 sequence alive
 * across chunks, so a long string can be split at ANY byte boundary. */
void        con_write_n(const char *s, int n, int final);
void        con_flush_utf8(void);            /* flush a dangling sequence */
/* Called from the 100 Hz PIT IRQ: animates BLINK and the software cursor. */
void        con_tick(void);

/* Colour setters.  The uint8_t forms take CLUT indices (0..255); the
 * _rgb forms take a literal 0x00RRGGBB and work at full depth on VBE. */
void        con_set_style(uint8_t fg_idx, uint8_t bg_idx, uint8_t attrs);
void        con_set_style_rgb(uint32_t fg, uint32_t bg, uint8_t attrs);
void        con_set_fg(uint8_t fg_idx);
void        con_set_fg_rgb(uint32_t rgb);
void        con_set_bg_rgb(uint32_t rgb);
void        con_set_attrs(uint8_t attrs);
uint8_t     con_get_fg(void);
uint8_t     con_get_attrs(void);

/* User-definable 256-entry palette.  On Mode 13h the RGB is written to
 * the 6-bit VGA DAC; on VBE the CLUT drives indexed cells. */
int         con_set_palette(uint8_t idx, uint32_t rgb);
uint32_t    con_palette(uint8_t idx);

void        con_move_cursor(void);
void        con_set_cursor(uint8_t x, uint8_t y);
/* v2.60: snapshot cursor to bottom-left of live grid. */
void        con_reset_cursor(void);
/* v2.60: begin / end a high-frequency output burst.  Disables the slow
 * per-char software-cursor sync during the burst so help, banners, etc.
 * render fast; must always be paired with a matching con_unsuppress(). */
void        con_suppress_cursor(int on);
/* v2.60: end a suppress-burst and immediately repaint cursor position. */
void        con_unsuppress_cursor(void);
/* v2.61: parse a color spec into an RGB value.
 * Recognises "#RRGGBB" (with or without leading #) and the named tokens
 * defined by con_apply_theme(dark).  Returns 0 on success, -1 on bad input. */
int         con_parse_color(const char *s, uint32_t *out_rgb);
/* v2.61: apply a full theme palette (dark=1, light=0) to the CLUT and
 * default style.  Both palettes are committed to entries 0..47 + greys;
 * callers should call this after mode-switch so the first prompt uses it. */
void        con_apply_theme(int dark);
/* v2.21 T51: the shell line editor needs the live cursor position as the
 * anchor for in-place redraw after history / cursor-key edits. */
void        con_get_cursor(int *x, int *y);
void        con_putc_at(uint8_t x, uint8_t y, char c);
void        con_write_at(uint8_t x, uint8_t y, const char *s);
/* UTF-8 aware positioned write (hanzi consume two columns). */
void        con_write_at_utf8(uint8_t x, uint8_t y, const char *s);

/* Transient status bar on the bottom row (used by the pinyin IME).
 * The front-end saves that row's cells and restores them verbatim, so a
 * candidate list never destroys shell output or editor content. */
void        con_status_bar(const char *s);
void        con_status_bar_off(void);
/* v2.25: transient bar on an ARBITRARY row (the pinyin IME puts its
 * candidate list directly BELOW the input line).  Only one bar at a
 * time; switching rows restores the previous bar's row verbatim. */
void        con_status_bar_at(int row, const char *s);
/* v2.58: fixed two-row IME candidate area on the bottom two rows --
 * separator row (rows-2) and candidate row (rows-1).  Both rows'
 * original cells are saved and restored verbatim by con_status_bar_off.
 * An empty `sep` fills the separator row with '-'. */
void        con_status_bar2(const char *sep, const char *cand);

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

/* ---- v2.21 T51: console scrollback (Ctrl+Up / Ctrl+Down) ---------------
 * scroll_one() archives every row that scrolls off the top.  con_scrollback()
 * paints an archived window WITHOUT changing the live grid (a full copy is
 * kept in render.c), so returning to the live view restores the prompt and
 * the partially typed line verbatim.  delta > 0 = older, < 0 = newer.
 * Returns the current scroll offset; 0 means "live screen". */
int         con_scrollback(int delta);
int         con_scrollback_view(void);
void        con_scrollback_reset(void);

/* ---- v2.25 graphics-CLI enhancements ------------------------------------- */

/* Scroll the grid up (with archive) until the cursor row sits on the
 * MIDDLE row of the screen.  The shell calls this before each prompt so
 * the input line stays centred while output pushes it down. */
void        con_scroll_anchor(void);

/* Glyph scale for back-ends that support it (only vbe24 today):
 *   0 = compact  8x8   cell  (100x75 on 800x600)
 *   1 = normal   8x16  cell  (100x37, boot default)
 *   2 = large   16x32  cell  ( 50x18)
 * Returns CON_ENOSYS on fixed-cell back-ends, CON_EINVAL for a bad s. */
#define CON_SIZE_COMPACT 0
#define CON_SIZE_NORMAL  1
#define CON_SIZE_LARGE   2
int         con_set_cell_size(int s);
int         con_get_cell_scale(void);

/* v2.25 full-screen UI takeover: save the live grid + cursor, hand the
 * whole screen to a TUI, then restore verbatim on exit.  Only ONE level
 * (the useredit panel); nested pushes are refused with -1. */
int         con_screen_push(void);
int         con_screen_pop(void);

/* ---- v2.57: mouse glue (selection / wheel / middle-click paste) ---------
 * con_mouse_poll() drains the PS/2 mouse event queue in normal kernel
 * context: wheel notches scroll the console back, left button starts /
 * extends / finalizes a cell selection, right cancels, middle arms the
 * paste.  Text and gfx back-ends both consume wheel events; only the
 * drawable gfx24 back-end paints a software pixel cursor. */
void        con_mouse_poll(void);
/* Consume the pending middle-click paste request (1 when armed). */
int         con_mouse_paste_taken(void);
/* Snapshot driver pixel position + button mask for diagnostics/tests. */
void        con_mouse_state(int *x, int *y, uint8_t *buttons);
/* 1 when a non-empty cell selection currently exists. */
int         con_selection_nonempty(void);

/* v2.57: middle-click paste hook.  The shell line editor registers its
 * insert-and-redraw closure while a prompt is active; the console glue
 * invokes it on a middle button press with the clipboard UTF-8.  Passing
 * NULL detaches (and drains any pending paste). */
typedef void (*con_paste_fn)(const char *s, int n);
void        con_set_paste_hook(con_paste_fn fn);

#endif /* RENDER_H */
