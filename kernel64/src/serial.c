/* serial.c - COM1 polled output (P0.4 early printk) */
#include "k64.h"

static void outb(u16 port, u8 v) { __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port)); }
static u8 inb(u16 port) { u8 v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }

void serial_init(void)
{
    outb(0x3F9, 0x00);              /* disable UART irqs */
    outb(0x3FB, 0x80);              /* DLAB */
    outb(0x3F8, 0x01);              /* divisor low: 115200 */
    outb(0x3F9, 0x00);              /* divisor high */
    outb(0x3FB, 0x03);              /* 8N1 */
    outb(0x3FA, 0xC7);              /* FIFO enable, clear */
    outb(0x3FC, 0x03);              /* DTR|RTS */
}

void serial_putc(char c)
{
    int guard = 100000;
    while (!(inb(0x3FD) & 0x20) && --guard) { }
    outb(0x3F8, (u8)c);
}

void serial_puts(const char *s)
{
    while (*s) serial_putc(*s++);
}

void serial_hex(u64 v)
{
    serial_puts("0x");
    for (int i = 60; i >= 0; i -= 4) {
        u8 d = (u8)((v >> i) & 0xF);
        serial_putc((char)(d < 10 ? '0' + d : 'a' + d - 10));
    }
}

void serial_dec(u64 v)
{
    char buf[24];
    int n = 0;
    if (!v) { serial_putc('0'); return; }
    while (v && n < 23) { buf[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n--) serial_putc(buf[n]);
}

char serial_getc(void)
{
    if (inb(0x3FD) & 0x01) return (char)inb(0x3F8);
    return 0;
}
