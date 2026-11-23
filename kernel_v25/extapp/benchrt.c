/* benchrt.c -- ring3 syscall round-trip probe for the light perf axis.
 *
 * This is the ONLY ring3 load on the light baseline: it fires three cheap
 * syscall classes from user mode in tight loops, rdtsc-wraps each class,
 * and publishes the cycle totals to /tmp/benchrt.txt.  The ring0 bench
 * scenario (src/bench_light.c) spawns this image, waits for exit, reads
 * the file and turns the totals into [perf] syscall.rtt rows.
 *
 * Classes (kind matches bench_light.c):
 *   0 gettid  -- SYS_GETTID, no user buffer at all (cheapest control)
 *   1 write   -- SYS_WRITE, one char to the console
 *   2 file_read -- SYS_FILE_READ, first 64 B of /etc/passwd
 *
 * The kernel-side dispatcher segment (entry -> return) is measured by
 * hooks in task.c; this app measures the whole ring3 -> kernel -> ring3
 * round trip, including trap/iret and the user/kernel copy paths.
 *
 * Built with -std=c99 -mno-80387, no libc (see extapp/build_ce.sh).
 * Keep comments plain ASCII.  64-bit division is unavailable (no libgcc
 * helper), so totals stay uint32: with N=200 and sub-10k-cycle syscalls
 * the sum cannot wrap on qemu32/TCG.
 */
#include "ce_app.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

#define N 500u

static uint32_t rdtsc_lo(void) {
    uint32_t t;
    __asm__ volatile ("rdtsc" : "=a"(t) : : "edx");
    return t;
}

static void u32toa(uint32_t v, char *dst) {
    char tmp[10]; int k = 0;
    if (!v) { dst[0] = '0'; dst[1] = 0; return; }
    while (v) { tmp[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) *dst++ = tmp[--k];
    *dst = 0;
}

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = CE_API();
    (void)argc; (void)argv;

    char fbuf[96];
    uint32_t i;
    uint32_t gt = 0, wt = 0, rd = 0;
    uint32_t a, b;

    /* class 0: gettid (cheap, no user buffer) */
    for (i = 0; i < N; i++) {
        a = rdtsc_lo();
        api->gettid();
        b = rdtsc_lo();
        gt += b - a;
    }

    /* class 1: write one character (console render included) */
    for (i = 0; i < N; i++) {
        a = rdtsc_lo();
        api->write("x");
        b = rdtsc_lo();
        wt += b - a;
    }

    /* class 2: file_read a small seeded file, 64 B from offset 0 */
    for (i = 0; i < N; i++) {
        a = rdtsc_lo();
        api->file_read("/etc/passwd", fbuf, 64, 0);
        b = rdtsc_lo();
        rd += b - a;
    }

    /* Publish one line: gt=<u32> wt=<u32> rd=<u32> n=<N> */
    char out[160];
    char num[12];
    int o = 0;
    const char *p;

    p = "gt="; while (*p) out[o++] = *p++;
    u32toa(gt, num); p = num; while (*p) out[o++] = *p++;
    p = " wt="; while (*p) out[o++] = *p++;
    u32toa(wt, num); p = num; while (*p) out[o++] = *p++;
    p = " rd="; while (*p) out[o++] = *p++;
    u32toa(rd, num); p = num; while (*p) out[o++] = *p++;
    p = " n="; while (*p) out[o++] = *p++;
    u32toa(N, num); p = num; while (*p) out[o++] = *p++;
    out[o] = 0;

    api->file_write("/tmp/benchrt.txt", out, (uint32_t)o, 0);
    return 0;
}
