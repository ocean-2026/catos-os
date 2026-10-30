#ifndef VGA_H
#define VGA_H

#include <stdint.h>

enum vga_color {
    VGA_BLACK = 0, VGA_BLUE = 1, VGA_GREEN = 2, VGA_CYAN = 3,
    VGA_RED = 4, VGA_MAGENTA = 5, VGA_BROWN = 6, VGA_LIGHT_GREY = 7,
    VGA_DARK_GREY = 8, VGA_LIGHT_BLUE = 9, VGA_LIGHT_GREEN = 10,
    VGA_LIGHT_CYAN = 11, VGA_LIGHT_RED = 12, VGA_LIGHT_MAGENTA = 13,
    VGA_YELLOW = 14, VGA_WHITE = 15
};

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

#endif
