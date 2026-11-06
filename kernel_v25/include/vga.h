#ifndef VGA_H
#define VGA_H

/*
 * vga.h -- LEGACY compatibility facade.  DEPRECATED.
 *
 * Kept so that the ~150 existing vga_* call sites keep compiling unchanged
 * while the actual rendering moves into render.c + the back-ends.  New code
 * must include render.h and call con_*() directly.
 *
 * vga_* no longer means "write to the VGA text buffer" -- it means "write
 * to the active display front-end", which may be the graphics back-end.
 */
#include <stdint.h>
#include "render.h"

#define VGA_BLACK         CON_BLACK
#define VGA_BLUE          CON_BLUE
#define VGA_GREEN         CON_GREEN
#define VGA_CYAN          CON_CYAN
#define VGA_RED           CON_RED
#define VGA_MAGENTA       CON_MAGENTA
#define VGA_BROWN         CON_BROWN
#define VGA_LIGHT_GREY    CON_LIGHT_GREY
#define VGA_DARK_GREY     CON_DARK_GREY
#define VGA_LIGHT_BLUE    CON_LIGHT_BLUE
#define VGA_LIGHT_GREEN   CON_LIGHT_GREEN
#define VGA_LIGHT_CYAN    CON_LIGHT_CYAN
#define VGA_LIGHT_RED     CON_LIGHT_RED
#define VGA_LIGHT_MAGENTA CON_LIGHT_MAGENTA
#define VGA_YELLOW        CON_YELLOW
#define VGA_WHITE         CON_WHITE

void vga_init(void);
void vga_clear(void);
void vga_putc(char c);
void vga_write(const char *s);
void vga_set_color(uint8_t fg, uint8_t bg);
void vga_set_fg(uint8_t fg);
uint8_t vga_get_fg(void);
void vga_move_cursor(void);

/* absolute cursor / raw cell writes (used by the nano-style editor) */
void vga_set_cursor(uint8_t x, uint8_t y);
void vga_putc_at(uint8_t x, uint8_t y, char c);
void vga_write_at(uint8_t x, uint8_t y, const char *s);
/* v2.9: streaming write (UTF-8 state survives non-final chunks) and a
 * UTF-8 aware positioned write for editors / candidate bars. */
void vga_write_n(const char *s, int n, int final);
void vga_write_at_utf8(uint8_t x, uint8_t y, const char *s);

#endif
