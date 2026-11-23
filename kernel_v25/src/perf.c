/* perf.c -- rdtsc benchmark instrumentation (see perf.h / bench/PERF_SPEC.md).
 *
 * Deliberately tiny: no allocation, no locks, no floating point.  Output is
 * polled COM1 only (the default console is gfx24, whose 0xB8000 text buffer
 * is empty, so screen scraping cannot be used for measurements).
 */
#include "perf.h"
#include "hw.h"
#include "vga.h"
#include "serial_log.h"

uint64_t perf_now(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

uint32_t perf_mhz(void) {
    uint32_t m = (uint32_t)hw_num(HW_CPU_MHZ);
    return m ? m : 1000u;
}

/* ---- line builder ---------------------------------------------------- */
#define PB_MAX 200
static char pb[PB_MAX];
static int  pn;

static void pb_ch(char c) { if (pn < PB_MAX - 1) pb[pn++] = c; }

static void pb_str(const char *s) {
    while (*s) pb_ch(*s++);
}

static void pb_u32(uint32_t v) {
    char t[10];
    int k = 0;
    if (!v) { pb_ch('0'); return; }
    while (v) { t[k++] = (char)('0' + (v % 10)); v /= 10; }
    while (k) pb_ch(t[--k]);
}

void perf_begin(const char *metric) {
    pn = 0;
    pb_str("[perf] ");
    pb_str(metric);
}

void perf_kv(const char *key, uint32_t v) {
    pb_ch(' ');
    pb_str(key);
    pb_ch('=');
    pb_u32(v);
}

void perf_kv64(const char *key, uint64_t v) {
    char k[32];
    int i = 0;
    while (key[i] && i < 30) { k[i] = key[i]; i++; }
    k[i] = 0;
    pb_ch(' ');
    pb_str(k); pb_str("_hi=");
    pb_u32((uint32_t)(v >> 32));
    pb_ch(' ');
    pb_str(k); pb_str("_lo=");
    pb_u32((uint32_t)(v & 0xFFFFFFFFu));
}

void perf_end(void) {
    pb_ch('\n');
    pb[pn] = 0;
    ser_write(pb);
}

void perf_cycles(const char *metric, uint64_t cycles) {
    perf_begin(metric);
    perf_kv64("cycles", cycles);
    perf_kv("mhz", perf_mhz());
    perf_end();
}

void perf_cycles_bytes(const char *metric, uint64_t cycles, uint32_t bytes) {
    perf_begin(metric);
    perf_kv64("cycles", cycles);
    perf_kv("bytes", bytes);
    perf_kv("mhz", perf_mhz());
    perf_end();
}

void perf_cycles_count(const char *metric, uint64_t cycles, uint32_t count) {
    perf_begin(metric);
    perf_kv64("cycles", cycles);
    perf_kv("count", count);
    perf_kv("mhz", perf_mhz());
    perf_end();
}

void perf_boot_marker(void) {
    extern uint32_t uptime_ms(void);
    extern int hw_num(int);
    extern uint8_t _bss_end[];
    ser_write("[boot] shell-ready\n");
    perf_begin("boot");
    perf_kv("uptime_ms", uptime_ms());
    perf_kv("mhz", perf_mhz());
    perf_kv("tasks", (uint32_t)hw_num(HW_TASKS));
    perf_kv("bss_end_lo", (uint32_t)(uintptr_t)_bss_end);
    perf_end();
}

/* ---- `bench` shell command ------------------------------------------- */
void bench_cmd(const char *arg) {
#ifdef CATOS_BENCH
    const char *a = arg;
    int handled = 0;
    while (*a == ' ') a++;
    if (*a) handled = bench_light_cmd(a) || bench_heavy_cmd(a);
    /* Always emit the completion sentinel: the host driver anchors on it,
     * and `handled=` tells a real scenario apart from a usage message. */
    perf_begin("end");
    perf_kv("handled", handled ? 1u : 0u);
    perf_kv("args", (uint32_t)0);
    perf_end();
    if (handled)
        vga_write("bench: ok\n");
    else
        vga_write("bench: usage: bench light | bench syscall | bench disk | "
                  "bench gfx | bench pipe | bench task | bench login\n");
#else
    (void)arg;
    vga_write("bench: not built (rebuild with BENCH=1)\n");
#endif
}
