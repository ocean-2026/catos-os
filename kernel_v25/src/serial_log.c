/* serial_log.c -- polled COM1 (0x3F8) breadcrumb channel.
 *
 * v2.59-crashscreen P0-1: the exception dispatcher used to keep its own
 * private UART init/write pair in intr.c.  The crash screen needs the same
 * dual-output channel, so the shared implementation lives here.  On the
 * crash path every character waits on LSR.THRE (0x3F8+5 bit 5) with a cycle
 * guard; an unready UART loses that byte rather than blocking the panic
 * flow.  The init sequence mirrors the 115200 8N1 setup from intr.c.
 */
#include "serial_log.h"
#include "io.h"

#define SER_COM1      0x3F8
#define SER_LSR_THRE  0x20u
/* Per-character wait guard.  At 115200 baud a byte takes ~87 us; the guard
 * only has to survive a missing/ghost UART, so a few thousand spins suffice.
 */
#define SER_GUARD     4000

static int ser_inited;

static void ser_init(void) {
    outb(SER_COM1 + 1, 0x00);          /* no interrupts */
    outb(SER_COM1 + 3, 0x80);          /* DLAB */
    outb(SER_COM1 + 0, 0x01);          /* divisor 1 = 115200 */
    outb(SER_COM1 + 1, 0x00);
    outb(SER_COM1 + 3, 0x03);          /* 8N1 */
    ser_inited = 1;
}

void ser_putc(char c) {
    if (!ser_inited) ser_init();
    int guard = SER_GUARD;
    while (!(inb(SER_COM1 + 5) & SER_LSR_THRE) && --guard > 0) { }
    if (guard <= 0) return;            /* timeout: drop, do not block */
    outb(SER_COM1, (uint8_t)c);
}

void ser_write(const char *s) {
    if (!s) return;
    while (*s) ser_putc(*s++);
}

void ser_hex(const char *tag, uint32_t v) {
    static const char *h = "0123456789abcdef";
    ser_write(tag);
    for (int i = 7; i >= 0; i--) ser_putc(h[(v >> (i * 4)) & 0xF]);
    ser_putc(' ');
}
