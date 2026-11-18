#ifndef CATOS_SERIAL_LOG_H
#define CATOS_SERIAL_LOG_H

/*
 * serial_log.h -- v2.59-crashscreen P0-1: polled COM1 (0x3F8) channel
 * shared by the exception dispatcher and the crash screen.
 *
 * Contract for the crash path:
 *   - every write waits on LSR.THRE with a SHORT cycle guard; if the UART
 *     never becomes ready the byte is DROPPED, the caller is never blocked.
 *   - the channel does not allocate, lock, sleep or touch the FS / IME.
 *   - it is safe to call from a second, nested panic entry: the guard caps
 *     total work per character.
 */
#include <stdint.h>

/* Poll one byte out to COM1.  Drops on timeout; never blocks. */
void ser_putc(const char c);

/* Poll a NUL-terminated ASCII string out to COM1. */
void ser_write(const char *s);

/* Hex helper: tag + 8-digit hex + trailing space. */
void ser_hex(const char *tag, uint32_t v);

#endif /* CATOS_SERIAL_LOG_H */
