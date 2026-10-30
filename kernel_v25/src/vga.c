#include "vga.h"
#include "io.h"

static volatile uint16_t *vga_mem = (uint16_t *)0xB8000;
static uint8_t cursor_x = 0;
static uint8_t cursor_y = 0;
static uint8_t fg = VGA_LIGHT_GREY;
static uint8_t bg = VGA_BLACK;

static inline uint16_t vga_entry(char c) {
    return (uint16_t)(unsigned char)c | ((uint16_t)(bg & 0x0F) << 12) | ((uint16_t)(fg & 0x0F) << 8);
}

static void move_cursor_hw(void) {
    uint16_t pos = cursor_y * 80 + cursor_x;
    outb(0x3D4, 14); outb(0x3D5, pos >> 8);
    outb(0x3D4, 15); outb(0x3D5, pos & 0xFF);
}

void vga_init(void) {
    cursor_x = 0; cursor_y = 0;
    vga_clear();
}

void vga_clear(void) {
    for (int y = 0; y < 25; y++)
        for (int x = 0; x < 80; x++)
            vga_mem[y * 80 + x] = vga_entry(' ');
    cursor_x = 0; cursor_y = 0;
    move_cursor_hw();
}

static void scroll(void) {
    if (cursor_y < 25) return;
    for (int y = 1; y < 25; y++)
        for (int x = 0; x < 80; x++)
            vga_mem[(y - 1) * 80 + x] = vga_mem[y * 80 + x];
    for (int x = 0; x < 80; x++)
        vga_mem[24 * 80 + x] = vga_entry(' ');
    cursor_y = 24;
}

void vga_putc(char c) {
    switch (c) {
    case '\n':
        cursor_x = 0; cursor_y++;
        break;
    case '\r':
        cursor_x = 0;
        break;
    case '\b':
        if (cursor_x > 0) {
            cursor_x--;
            vga_mem[cursor_y * 80 + cursor_x] = vga_entry(' ');
        }
        break;
    case '\t':
        cursor_x = (cursor_x + 4) & ~3;
        break;
    default:
        vga_mem[cursor_y * 80 + cursor_x] = vga_entry(c);
        cursor_x++;
        break;
    }
    if (cursor_x >= 80) { cursor_x = 0; cursor_y++; }
    scroll();
    move_cursor_hw();
}

void vga_write(const char *s) {
    while (*s) vga_putc(*s++);
}

void vga_set_color(uint8_t f, uint8_t b) { fg = f; bg = b; }
void vga_set_fg(uint8_t f) { fg = f; }
uint8_t vga_get_fg(void) { return fg; }
void vga_move_cursor(void) { move_cursor_hw(); }

void vga_set_cursor(uint8_t x, uint8_t y) {
    cursor_x = x; cursor_y = y;
    move_cursor_hw();
}

void vga_putc_at(uint8_t x, uint8_t y, char c) {
    if (x >= 80 || y >= 25) return;
    vga_mem[y * 80 + x] = vga_entry(c);
}

void vga_write_at(uint8_t x, uint8_t y, const char *s) {
    while (*s && x < 80) { vga_putc_at(x++, y, *s++); }
}
