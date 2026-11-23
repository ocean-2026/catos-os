/* bench_heavy.c -- "heavy" performance axes (PERF_SPEC.md §6).
 *
 * Four scenarios, each self-contained and driven from the `bench` shell
 * command in a bench build (BENCH=1).  Every number is an rdtsc cycle
 * delta; derived units (us / MB/s / fps) are computed on the host from the
 * guest's own calibrated TSC mhz.  Split phases are timed with separate
 * rdtsc windows so a bottleneck claim has cycle evidence, not a guess.
 *
 *   gfx.frame  -- one full-screen repaint on the gfx24 (VBE 24bpp) back-end;
 *                 split into glyph/cell painting vs bulk framebuffer flush.
 *   disk.seq   -- raw ATA PIO sequential read; split into status/DRQ
 *                 polling vs the 16-bit data-port transfer.
 *   pipe.tput  -- concurrent writer/reader over the kernel FIFO (E3 8192 B
 *                 pattern, scaled to 64 KiB); reports block/wake count.
 *   task.spawn -- spawn -> exec -> exit full chain; split into address
 *                 space / task-table setup vs user entry + exit reclaim.
 *
 * Scenarios that need a mounted volume (disk, task) emit present=0 instead
 * of fake zero cycles when no IDE drive / FS is up; the host driver flags
 * those runs as blocked rather than recording a 0 metric.
 */
#include "perf.h"
#include "task.h"
#include "ata.h"
#include "gfx24.h"
#include "render.h"
#include "render_be.h"
#include "fs.h"
#include "intr.h"
#include "io.h"
#include <stdint.h>

/* ---- tiny local string helpers (no libc in the kernel) ------------------ */
static void h_strcpy(char *d, const char *s) {
    while ((*d++ = *s++)) {}
}

/* Match the first whitespace-delimited token of arg against tok. */
static int h_token(const char *arg, const char *tok) {
    while (*arg == ' ') arg++;
    const char *t = tok;
    while (*t && *arg == *t) { arg++; t++; }
    if (*t) return 0;
    return (*arg == 0 || *arg == ' ');
}

/* =========================================================================
 * gfx.frame -- full-screen repaint on the active 24bpp back-end.
 *
 * Total: F repetitions of con_repaint_frame(), the public full-frame repaint
 * entry (be->clear bulk fill + repaint_all cell loop) -- the worst-case
 * whole-screen redraw the console takes on a theme switch / scroll fallback.
 *
 * Split (rdtsc windows):
 *   cycles_flush -- F bulk full-screen LFB fills (gfx24_clear), the
 *                   "framebuffer 冲刷拷贝" phase.
 *   cycles_glyph -- total minus flush: the grid walk + glyph/cell
 *                   rasterization into the LFB, the "像素/字形绘制" phase.
 * (Streaming a full grid through con_write per char also exercises the
 * newline/scroll fallback, which is a different path and is deliberately
 * not counted here.)
 * ========================================================================= */
#define GFX_FRAMES 6

static void bench_gfx(void) {
    int active = gfx24_active();
    int cols = con_cols();
    int rows = con_rows();
    if (!active || cols <= 0 || rows <= 0) {
        perf_begin("gfx.frame");
        perf_kv("present", 0u);
        perf_kv("frames", 0u);
        perf_kv("mhz", perf_mhz());
        perf_end();
        return;
    }

    const int F = GFX_FRAMES;

    /* flush phase: bulk full-screen LFB fill */
    uint64_t t_flush = 0;
    for (int f = 0; f < F; f++) {
        uint64_t a = perf_now();
        gfx24_clear(0x0C0C0C);
        uint64_t b = perf_now();
        t_flush += b - a;
    }

    /* total: real full-frame repaint entry, F frames */
    uint64_t t_total = 0;
    for (int f = 0; f < F; f++) {
        uint64_t a = perf_now();
        con_repaint_frame();
        uint64_t b = perf_now();
        t_total += b - a;
    }

    uint64_t t_glyph = t_total > t_flush ? t_total - t_flush : 0;

    perf_begin("gfx.frame");
    perf_kv64("cycles", t_total);
    perf_kv("frames", (uint32_t)F);
    perf_kv64("cycles_glyph", t_glyph);
    perf_kv64("cycles_flush", t_flush);
    perf_kv("cols", (uint32_t)cols);
    perf_kv("rows", (uint32_t)rows);
    perf_kv("mhz", perf_mhz());
    perf_end();

    /* ---- single-character micro-bench: glyph fetch vs full cell paint ---- */
    struct render_be *be = con_backend();
    if (be && be->draw_cell) {
        const int CN = 2000;
        con_cell_t cc;
        cc.cp = 'x';
        cc.fg = 0xDCDCDCu; cc.bg = 0x0C0C0Cu;
        cc.fg_idx = 7; cc.bg_idx = 0;
        cc.attrs = 0; cc.flags = CELL_F_SINGLE; cc.gslot = CELL_GSLOT_NONE;

        /* glyph table lookup only */
        uintptr_t sink = 0;
        uint64_t a = perf_now();
        for (int i = 0; i < CN; i++)
            sink ^= (uintptr_t)render_font_glyph((unsigned char)cc.cp);
        uint64_t b = perf_now();
        uint64_t t_fetch = b - a;

        /* full back-end cell paint: resolve colors + bg fill + ink raster */
        a = perf_now();
        for (int i = 0; i < CN; i++)
            be->draw_cell(3, 3, &cc);
        b = perf_now();
        uint64_t t_draw = b - a;
        (void)sink;

        perf_begin("gfx.char");
        perf_kv64("cycles", t_draw);
        perf_kv("count", (uint32_t)CN);
        perf_kv64("cycles_fetch", t_fetch);
        perf_kv("mhz", perf_mhz());
        perf_end();
    }
}

/* =========================================================================
 * disk.seq -- raw ATA PIO sequential read, drive 0, raw LBA (no FAT).
 *
 * Total path goes through the real ata_read() driver.  The split probe
 * below mirrors ata_read()'s exact port sequence (ata.c: ata_read) so the
 * two phases can be timed separately:
 *   cycles_wait -- status/DRQ polling (BSY/DRQ spin on port 0x1F7)
 *   cycles_xfer -- 256 inw data-port reads per sector
 * This split is the evidence for/against DMA or multi-sector PIO.
 * ========================================================================= */
#define DISK_MIB       4u
#define DISK_SECTORS   (DISK_MIB * 1024u * 1024u / 512u)  /* 8192 */
#define DISK_CHUNK     32u                                  /* sectors/read */

/* primary master ports, matching ata.c slot 0 */
#define ATA_DATA    0x1F0
#define ATA_SECNT   0x1F2
#define ATA_LBA_LO  0x1F3
#define ATA_LBA_MID 0x1F4
#define ATA_LBA_HI  0x1F5
#define ATA_DEV     0x1F6
#define ATA_CMD     0x1F7
#define ATA_DEV_MASTER 0xE0
#define ATA_CMD_READ  0x20

static void bench_disk(void) {
    if (ata_drive_count() == 0) {
        perf_begin("disk.seq");
        perf_kv("present", 0u);
        perf_kv("bytes", 0u);
        perf_kv("mhz", perf_mhz());
        perf_end();
        return;
    }
    uint32_t avail = ata_drive_sectors(0);
    uint32_t sectors = DISK_SECTORS;
    if (avail && sectors > avail) sectors = avail;
    if (sectors == 0) {
        perf_begin("disk.seq");
        perf_kv("present", 0u);
        perf_kv("bytes", 0u);
        perf_kv("mhz", perf_mhz());
        perf_end();
        return;
    }

    static uint8_t buf[DISK_CHUNK * 512u] __attribute__((aligned(16)));

    /* --- total: real driver path -------------------------------------- */
    uint64_t t0 = perf_now();
    uint32_t lba = 0, done = 0;
    int fail = 0;
    while (done < sectors) {
        uint32_t n = DISK_CHUNK;
        if (sectors - done < n) n = sectors - done;
        if (ata_read(0, lba, (uint8_t)n, buf) != 0) { fail = 1; break; }
        lba += n; done += n;
    }
    uint64_t t1 = perf_now();
    if (fail) {
        perf_begin("disk.seq");
        perf_kv("present", 0u);
        perf_kv("bytes", 0u);
        perf_kv("mhz", perf_mhz());
        perf_end();
        return;
    }

    /* --- split probe: faithful mirror of ata_read's two phases ---------
     * Re-reads the same LBA region; only the internal wait/transfer split
     * is timed here, the total throughput above goes through ata_read(). */
    uint64_t t_wait = 0, t_xfer = 0;
    uint32_t plba = 0;
    for (uint32_t off = 0; off < sectors; ) {
        uint32_t n = DISK_CHUNK;
        if (sectors - off < n) n = sectors - off;

        uint64_t w0 = perf_now();
        /* ata_wait_ready: spin until BSY clears */
        for (int k = 0; k < 10000000; k++) {
            if (!(inb(ATA_CMD) & 0x80)) break;
        }
        outb(ATA_DEV, ATA_DEV_MASTER | ((plba >> 24) & 0x0F));
        outb(ATA_SECNT, (uint8_t)n);
        outb(ATA_LBA_LO, (uint8_t)plba);
        outb(ATA_LBA_MID, (uint8_t)(plba >> 8));
        outb(ATA_LBA_HI, (uint8_t)(plba >> 16));
        outb(ATA_CMD, ATA_CMD_READ);
        uint64_t w1 = perf_now();
        t_wait += w1 - w0;

        for (uint32_t s = 0; s < n; s++) {
            uint64_t q0 = perf_now();
            /* ata_wait_drq: BSY -> ERR/DRQ */
            for (int k = 0; k < 10000000; k++) {
                uint8_t st = inb(ATA_CMD);
                if (st & 0x80) continue;
                if (st & 0x01 || st & 0x08) break;
            }
            uint64_t q1 = perf_now();
            uint16_t *w = (uint16_t *)(buf + s * 512u);
            for (int i = 0; i < 256; i++) w[i] = inw(ATA_DATA);
            uint64_t q2 = perf_now();
            t_wait += q1 - q0;
            t_xfer += q2 - q1;
        }
        plba += n; off += n;
    }

    perf_begin("disk.seq");
    perf_kv64("cycles", t1 - t0);
    perf_kv("bytes", (uint32_t)(done * 512u));
    perf_kv("sectors", done);
    perf_kv64("cycles_wait", t_wait);
    perf_kv64("cycles_xfer", t_xfer);
    perf_kv("mhz", perf_mhz());
    perf_end();
}

/* =========================================================================
 * pipe.tput -- concurrent FIFO write/drain, E3 pattern cycle.
 *
 * Writer: ring0 kernel thread, 256 B chunks over kpipe_write.
 * Reader: bench task drains 64 KiB through kpipe_read, verifying the same
 * 16-byte cycle E3 checks.  Block/wake events go through task_sleep_ticks,
 * counted by kpipe_sleeps(); elapsed timer ticks give the blocked share.
 * ========================================================================= */
#define PIPE_TOTAL  65536u
#define PIPE_CHUNK  256u

static int      g_wr_pipe;
static uint32_t g_wr_total;
static uint32_t g_wr_written;
static char     g_wr_buf[PIPE_CHUNK];

static void heavy_writer(void) {
    uint32_t off = 0;
    while (off < g_wr_total) {
        uint32_t n = g_wr_total - off;
        if (n > PIPE_CHUNK) n = PIPE_CHUNK;
        if (kpipe_write(g_wr_pipe, g_wr_buf, (int)n) != 0) break;
        off += n;
    }
    g_wr_written = off;
    kpipe_close_write(g_wr_pipe);
    task_exit_current(0);
}

static void bench_pipe(void) {
    int h = kpipe_create();
    if (h < 0) {
        perf_begin("pipe.tput");
        perf_kv("present", 0u);
        perf_kv("bytes", 0u);
        perf_kv("mhz", perf_mhz());
        perf_end();
        return;
    }

    static char rbuf[PIPE_CHUNK];
    const char *cycle = "0123456789ABCDEF";
    for (uint32_t i = 0; i < PIPE_CHUNK; i++)
        g_wr_buf[i] = cycle[i & 15];
    g_wr_pipe = h;
    g_wr_total = PIPE_TOTAL;
    g_wr_written = 0;

    uint32_t sleeps0 = kpipe_sleeps();
    uint32_t ticks0 = timer_ticks;
    uint64_t t0 = perf_now();

    int tid = task_spawn_r0(heavy_writer, "bench-pipe-w", 1);

    uint32_t got = 0, bad = 0, pos = 0, chunks = 0;
    while (got < PIPE_TOTAL) {
        int n = kpipe_read(h, rbuf, PIPE_CHUNK);
        if (n <= 0) break;
        for (int i = 0; i < n; i++)
            if (rbuf[i] != cycle[(pos + i) & 15]) bad++;
        pos += (uint32_t)n;
        got += (uint32_t)n;
        chunks++;
    }

    uint64_t t1 = perf_now();
    uint32_t ticks1 = timer_ticks;

    if (tid > 0) {
        task_t *ch = task_by_tid(tid);
        if (ch) task_wait_child(ch);
    }
    kpipe_close_read(h);

    perf_begin("pipe.tput");
    perf_kv64("cycles", t1 - t0);
    perf_kv("bytes", got);
    perf_kv("written", g_wr_written);
    perf_kv("chunks", chunks);
    perf_kv("bad", bad);
    perf_kv("sleeps", kpipe_sleeps() - sleeps0);
    perf_kv("ticks", ticks1 - ticks0);
    perf_kv("mhz", perf_mhz());
    perf_end();
}

/* =========================================================================
 * task.spawn -- spawn -> exec -> exit full chain, N iterations.
 *
 *   cycles_setup: task_spawn_ce() itself -- slot allocation, FS resolve,
 *                 image header parse, address-space create, image load,
 *                 vsyscall/stack setup.
 *   cycles_exit : wait_child() until the child exits and is reaped --
 *                 ring3 entry, ce_main, exit syscall, cleanup/reclaim.
 * ========================================================================= */
#define TASK_N 10

static void bench_task(void) {
    if (!fs_fs_ready()) {
        perf_begin("task.spawn");
        perf_kv("present", 0u);
        perf_kv("count", 0u);
        perf_kv("mhz", perf_mhz());
        perf_end();
        return;
    }

    static char path[32];
    static char *argv[1];
    h_strcpy(path, "/bin/hello.ce");
    argv[0] = path;

    const int N = TASK_N;
    uint64_t t_setup = 0, t_exit = 0;
    int ok = 0;

    for (int k = 0; k < N; k++) {
        uint64_t a = perf_now();
        int tid = task_spawn_ce(path, 1, argv, UID_ROOT);
        uint64_t b = perf_now();
        if (tid < 0) break;
        t_setup += b - a;

        task_t *ch = task_by_tid(tid);
        uint64_t c = perf_now();
        task_wait_child(ch);
        uint64_t d = perf_now();
        t_exit += d - c;
        ok++;
    }

    perf_begin("task.spawn");
    perf_kv64("cycles", t_setup + t_exit);
    perf_kv("count", (uint32_t)ok);
    perf_kv64("cycles_setup", t_setup);
    perf_kv64("cycles_exit", t_exit);
    perf_kv("mhz", perf_mhz());
    perf_end();
}

/* ---- command dispatch ---------------------------------------------------- */
int bench_heavy_cmd(const char *arg) {
    if (h_token(arg, "heavy")) {
        bench_gfx();
        bench_pipe();
        bench_disk();
        bench_task();
        return 1;
    }
    if (h_token(arg, "gfx"))  { bench_gfx();  return 1; }
    if (h_token(arg, "disk")) { bench_disk(); return 1; }
    if (h_token(arg, "pipe")) { bench_pipe(); return 1; }
    if (h_token(arg, "task"))  { bench_task();  return 1; }
    return 0;
}
