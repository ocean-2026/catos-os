#ifndef PERF_H
#define PERF_H

/*
 * perf.h -- rdtsc benchmark instrumentation for CatOS (bench builds only).
 *
 * Build with `BENCH=1 sh kernel_v25/build.sh` to define CATOS_BENCH and get
 * the `bench` shell command.  Production builds compile this file but never
 * call into it (the shell dispatch entry is behind #ifdef CATOS_BENCH), so
 * the only cost is the unused code size.
 *
 * Contract (see bench/PERF_SPEC.md):
 *   - every measurement is a TSC (rdtsc) cycle delta, never a wall-clock
 *     sleep, so TCG/QEMU constant factors do not hide a real regression;
 *   - output is one line on COM1, always shaped
 *         [perf] <metric> key=value key=value ...
 *     with integer decimal values only.  Values wider than 32 bits are
 *     emitted as <key>_hi / <key>_lo (the kernel has no libgcc 64-bit
 *     divide), and the host report reassembles them;
 *   - the host converts cycles to us / MB/s using `[perf] cpu mhz=N`.
 */
#include <stdint.h>

/* TSC read.  Callers must not migrate their timing across a task switch
 * unless the interval is deliberately task-independent. */
uint64_t perf_now(void);

/* Calibrated TSC frequency (hw_calibrate_cpu); falls back to 1000 MHz. */
uint32_t perf_mhz(void);

/* --- line builder ---------------------------------------------------- */
void perf_begin(const char *metric);          /* start "[perf] <metric>"  */
void perf_kv(const char *key, uint32_t v);    /* append " key=<dec>"      */
void perf_kv64(const char *key, uint64_t v);  /* "<key>_hi/_lo=<dec>"     */
void perf_end(void);                          /* newline + flush to COM1  */

/* Convenience: metric with cycles/mhz (mhz known) and optional extras. */
void perf_cycles(const char *metric, uint64_t cycles);
void perf_cycles_bytes(const char *metric, uint64_t cycles, uint32_t bytes);
void perf_cycles_count(const char *metric, uint64_t cycles, uint32_t count);

/* One-off boot marker: prints "[boot] shell-ready" (driver wait anchor) and
 * a [perf] line with uptime + mhz + kernel image size. */
void perf_boot_marker(void);

/* `bench <arg>` shell command (bench builds only).  It dispatches to the two
 * self-contained scenario modules so their measurements stay independent:
 *   bench_light_cmd  -- boot / shell / syscall / login axes
 *   bench_heavy_cmd  -- graphics / disk / pipe / task axes
 * Either returns 1 when it recognised and ran the subcommand. */
int bench_light_cmd(const char *arg);
int bench_heavy_cmd(const char *arg);
void bench_cmd(const char *arg);

#endif /* PERF_H */
