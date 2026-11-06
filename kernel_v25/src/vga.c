/*
 * vga.c -- thin LEGACY shim: vga_*() -> con_*().
 *
 * There is intentionally NO hardware access here.  Every pixel and every
 * VGA register is owned by src/be_text.c or src/be_gfx.c.
 */
#include "vga.h"

void vga_init(void)                       { render_init(); }
void vga_clear(void)                      { con_clear(); }
void vga_putc(char c)                     { con_putc(c); }
void vga_write(const char *s)             { con_write(s); }
void vga_set_color(uint8_t f, uint8_t b)  { con_set_style(f, b, con_get_attrs()); }
void vga_set_fg(uint8_t f)                { con_set_fg(f); }
uint8_t vga_get_fg(void)                  { return con_get_fg(); }
void vga_move_cursor(void)                { con_move_cursor(); }
void vga_set_cursor(uint8_t x, uint8_t y) { con_set_cursor(x, y); }
void vga_putc_at(uint8_t x, uint8_t y, char c) { con_putc_at(x, y, c); }
void vga_write_at(uint8_t x, uint8_t y, const char *s) {
    con_write_at(x, y, s);
}
