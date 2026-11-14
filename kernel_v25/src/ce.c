#include "ce.h"
#include "vga.h"
#include "keyboard.h"
#include "rtc.h"
#include "fs.h"
#include "fat.h"
#include "net.h"

/* ce.c - CE format support that stays kernel-side:
 *   - ce_init(): called once at boot (the old ring0 heap is gone; every
 *     task now gets its own heap via the alloc/free syscalls)
 *   - ce_time_str_fill(): the time_str() syscall body
 *
 * Loading and RUNNING a .ce file moved to task.c: programs execute in
 * ring3 with a private address space and reach the kernel through the
 * int 0x80 gate + the per-task vsyscall thunk page (binary-compatible
 * with unmodified .ce files built against ce_abi.h).
 */

void ce_init(void) {
    /* nothing left to set up statically */
}

void ce_time_str_fill(char *out) {
    rtc_time_t t;
    rtc_read(&t);
    if (!out) return;
    /* YYYY-MM-DD HH:MM:SS */
    int y = t.year, mo = t.month, d = t.day;
    int h = t.hour, mi = t.minute, s = t.second;
    char b[24];
    int i = 0;
    b[i++] = (char)('0' + (y / 1000) % 10);
    b[i++] = (char)('0' + (y / 100) % 10);
    b[i++] = (char)('0' + (y / 10) % 10);
    b[i++] = (char)('0' + (y % 10));
    b[i++] = '-';
    b[i++] = (char)('0' + (mo / 10) % 10);
    b[i++] = (char)('0' + (mo % 10));
    b[i++] = '-';
    b[i++] = (char)('0' + (d / 10) % 10);
    b[i++] = (char)('0' + (d % 10));
    b[i++] = ' ';
    b[i++] = (char)('0' + (h / 10) % 10);
    b[i++] = (char)('0' + (h % 10));
    b[i++] = ':';
    b[i++] = (char)('0' + (mi / 10) % 10);
    b[i++] = (char)('0' + (mi % 10));
    b[i++] = ':';
    b[i++] = (char)('0' + (s / 10) % 10);
    b[i++] = (char)('0' + (s % 10));
    b[i++] = 0;
    for (i = 0; b[i]; i++) out[i] = b[i];
    out[i] = 0;
}
