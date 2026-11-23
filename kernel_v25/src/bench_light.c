/* bench_light.c -- "light" performance axes: boot, shell command round trip,
 * syscall round trip, login (password digest) path.
 *
 * Ownership: this file is the light-axis scenario owner (see
 * bench/PERF_SPEC.md §7).  Every scenario is self-contained here; the only
 * kernel-side timing hooks outside this file are:
 *   - shell.c main-loop idle/dispatch stamps (under #ifdef CATOS_BENCH),
 *   - task.c dispatcher entry/exit stamps for the syscall kernel segment.
 *
 * Metric names are fixed so later "before/after" runs compare like-for-like:
 *   shell.cmd    dispatch segment of one shell builtin (kind: 1 echo, 2 ls,
 *                3 edit -c); count = internal repetitions
 *   shell.idle   prompt + readline segment, emitted by the shell loop hook
 *   syscall.rtt  ring3 -> kernel -> ring3 whole round trip (ring3 benchrt)
 *   syscall.kernel dispatcher entry -> return only (task.c hooks)
 *   login.verify F11 $C$ salted SHA-256 verify, full path incl. hex parse
 *   login.hash   pure SHA-256(salt||pw) over a fixed salt, no parse/compare
 *
 * All cycles are rdtsc deltas; never a host sleep.  Units are derived on
 * the host from the guest-calibrated mhz.
 */
#include "perf.h"
#include <stdint.h>
#ifndef NULL
#define NULL ((void *)0)
#endif

#include "fs.h"
#include "user.h"
#include "task.h"
#include "crypto.h"

/* shell.c: programmatic one-line exec, captures builtin output via tee. */
extern int shell_ssh_exec_line(const char *line, char *out, int outmax,
                               int *rc);

/* task.c light-axis counters (defined under #ifdef CATOS_BENCH there). */
#ifdef CATOS_BENCH
extern int      perf_sc_collect;
extern uint64_t perf_sc_kern[3];
extern uint32_t perf_sc_count[3];
extern uint64_t perf_fr_pre, perf_fr_copyout;
extern uint64_t perf_fs_resolve, perf_fs_chain, perf_fs_ide, perf_fs_mem;
extern uint32_t perf_fs_count;
#endif

/* ---- tunable sample counts ------------------------------------------- */
#define N_SHELL   50u    /* internal repetitions per shell command        */
#define N_LOGIN   50u    /* repetitions per login/hash sample             */
#define RT_APP    "/bin/benchrt.ce"
#define RT_RESULT "/tmp/benchrt.txt"

/* kind map, shared with extapp/benchrt.c and the task.c bucket table:
 * 0 = gettid, 1 = write, 2 = file_read. */

/* ---- tiny decimal scan (freestanding, no libc) ----------------------- */
static int pw_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

static uint32_t grab_u32(const char *s, const char *key) {
    while (s && *s) {
        const char *k = key, *q = s;
        while (*k && *q && *k == *q) { k++; q++; }
        if (!*k) {
            uint32_t v = 0;
            while (*q >= '0' && *q <= '9') { v = v * 10u + (uint32_t)(*q - '0'); q++; }
            return v;
        }
        s++;
    }
    return 0;
}

/* ---- axis 2: shell command dispatch segment --------------------------- */
static void bench_shell_one(const char *line, uint32_t kind) {
    char out[1024];
    int rc = 0;
    uint64_t sum = 0;
    shell_ssh_exec_line(line, out, (int)sizeof(out), &rc);  /* warmup: cold caches */
    for (uint32_t i = 0; i < N_SHELL; i++) {
        uint64_t a = perf_now();
        shell_ssh_exec_line(line, out, (int)sizeof(out), &rc);
        uint64_t b = perf_now();
        sum += b - a;
    }
    perf_begin("shell.cmd");
    perf_kv64("cycles", sum);
    perf_kv("count", N_SHELL);
    perf_kv("kind", kind);
    perf_kv("mhz", perf_mhz());
    perf_end();
}

static void bench_shell(void) {
    /* 1: echo (builtin, no fs), 2: ls /bin (directory walk),
     * 3: edit -c (read a small seeded file). */
    bench_shell_one("echo benchprobe", 1);
    bench_shell_one("ls /bin", 2);
    bench_shell_one("edit -c /etc/passwd", 3);
}

/* ---- axis 3: syscall round trip (ring3 probe) ------------------------ */
static void bench_syscall(void) {
    char rbuf[256];
    rbuf[0] = 0;
    int spawn_rc = 0;
    uint32_t rt[3] = { 0, 0, 0 };
    uint32_t rt_n = 0;

#ifdef CATOS_BENCH
    perf_sc_collect = 1;
    perf_fr_pre = perf_fr_copyout = 0;
    perf_fs_resolve = perf_fs_chain = perf_fs_ide = perf_fs_mem = 0;
    perf_fs_count = 0;
#endif
    int tid = task_spawn_ce(RT_APP, 0, (char **)0, UID_ROOT);
    if (tid >= 0) {
        task_t *ch = task_by_tid(tid);
        if (ch) task_wait_child(ch);
        spawn_rc = 0;
    } else {
        spawn_rc = tid;                 /* negative: CE loader error code */
    }
#ifdef CATOS_BENCH
    perf_sc_collect = 0;
#endif

    if (fs_read_text(RT_RESULT, rbuf, (int)sizeof(rbuf) - 1) > 0) {
        rt[0] = grab_u32(rbuf, "gt=");
        rt[1] = grab_u32(rbuf, "wt=");
        rt[2] = grab_u32(rbuf, "rd=");
        rt_n  = grab_u32(rbuf, "n=");
    }

    for (int k = 0; k < 3; k++) {
        /* whole ring3 round trip: trap entry, dispatch, copy paths, iret. */
        perf_begin("syscall.rtt");
        perf_kv64("cycles", (uint64_t)rt[k]);
        perf_kv("count", rt_n);
        perf_kv("kind", (uint32_t)k);
        perf_kv("spawn_rc", (uint32_t)spawn_rc);
        perf_kv("mhz", perf_mhz());
        perf_end();
    }

#ifdef CATOS_BENCH
    for (int k = 0; k < 3; k++) {
        /* dispatcher segment only: measured inside isr_syscall from entry
         * to return.  Excludes trap/iret and any ring3<->ring0 switch. */
        perf_begin("syscall.kernel");
        perf_kv64("cycles", perf_sc_kern[k]);
        perf_kv("count", perf_sc_count[k]);
        perf_kv("kind", (uint32_t)k);
        perf_kv("mhz", perf_mhz());
        perf_end();
    }

    /* file_read segment breakdown (seg: 0 pre, 1 resolve, 2 chain,
     * 3 ide, 4 mem, 5 copyout), summed over the probe's file_read loop. */
    {
        const uint64_t *seg[6] = {
            &perf_fr_pre, &perf_fs_resolve, &perf_fs_chain,
            &perf_fs_ide, &perf_fs_mem, &perf_fr_copyout
        };
        for (int s = 0; s < 6; s++) {
            perf_begin("fs.read");
            perf_kv64("cycles", *seg[s]);
            perf_kv("count", perf_fs_count);
            perf_kv("seg", (uint32_t)s);
            perf_kv("mhz", perf_mhz());
            perf_end();
        }
    }
#endif
}

/* ---- axis 4: login password verify (F11 $C$ salted SHA-256) ---------- */
static void bench_login(void) {
    const char *pw = "BenchPass123";
    user_rec_t *u = user_by_name("root");
    if (!u) {
        perf_begin("login.verify");
        perf_kv("cycles", 0); perf_kv("count", 0); perf_kv("bad", 1);
        perf_kv("mhz", perf_mhz()); perf_end();
        return;
    }
    /* Force an in-memory $C$ record with a known password.  Deliberately
     * do NOT call user_save(): the reference disk must stay untouched. */
    user_set_pw("root", pw);

    uint64_t vsum = 0, hsum = 0;
    int rc = -1;
    uint8_t digest[32];
    /* fixed 8-byte salt for the pure-hash baseline (parse/compare excluded). */
    static const uint8_t salt[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    for (uint32_t i = 0; i < N_LOGIN; i++) {
        uint64_t a = perf_now();
        rc = user_check_pw(u, pw);
        uint64_t b = perf_now();
        vsum += b - a;
    }
    for (uint32_t i = 0; i < N_LOGIN; i++) {
        uint64_t a = perf_now();
        sha256_2(digest, salt, 8, (const uint8_t *)pw, (uint32_t)pw_len(pw));
        uint64_t b = perf_now();
        hsum += b - a;
    }

    perf_begin("login.verify");
    perf_kv64("cycles", vsum);
    perf_kv("count", N_LOGIN);
    perf_kv("bad", rc == 0 ? 0u : 1u);
    perf_kv("mhz", perf_mhz());
    perf_end();

    perf_begin("login.hash");
    perf_kv64("cycles", hsum);
    perf_kv("count", N_LOGIN);
    perf_kv("mhz", perf_mhz());
    perf_end();
}

/* ---- byte-exactness probe: content digest across offsets / lengths ---- */
static uint32_t fnv1a(const uint8_t *p, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static void bench_fscheck(void) {
    static uint8_t fbuf[24 * 1024];
    struct { const char *path; int off; int max; uint32_t idx; } t[] = {
        { "/etc/passwd",       0,     24 * 1024, 0 },
        { "/bin/hello.ce",     0,     24 * 1024, 1 },
        { "/bin/ocntools.ce",  0,     20480,      2 },  /* 20 KiB, crosses 16 KiB */
        { "/bin/ocntools.ce",  16384, 4096,      3 },  /* tail slice 16K..20K   */
    };
    for (uint32_t k = 0; k < sizeof(t) / sizeof(t[0]); k++) {
        int n1 = fs_read_at(t[k].path, (char *)fbuf, t[k].off, t[k].max);
        uint32_t h1 = (n1 > 0) ? fnv1a(fbuf, n1) : 0;
        /* second read: cluster/path cache now warm; bytes must match the
         * first (cold disk) read byte-for-byte. */
        int n2 = fs_read_at(t[k].path, (char *)fbuf, t[k].off, t[k].max);
        uint32_t h2 = (n2 > 0) ? fnv1a(fbuf, n2) : 0;
        uint32_t match = (n1 == n2 && h1 == h2) ? 1u : 0u;
        perf_begin("fscheck");
        perf_kv("idx", t[k].idx);
        perf_kv("off", (uint32_t)t[k].off);
        perf_kv("max", (uint32_t)t[k].max);
        perf_kv("len", (uint32_t)(n1 < 0 ? 0 : n1));
        perf_kv("hash", h2);
        perf_kv("hash_cold", h1);
        perf_kv("match", match);
        perf_kv("rc", (uint32_t)n1);
        perf_end();
    }
}

/* ---- cache coherency gate: stale path dirent / cluster snapshot checks ----
 * Every case writes through the public fs_* API, reads back twice (cold and
 * cache-warm), and reports len/hash/rc on a `[perf] coh` line.  Scratch
 * files live under /tmp so built-in apps are never touched. */
static uint8_t coh_w[FS_DATA_MAX];
static uint8_t coh_e[FS_DATA_MAX];

static void coh_fill(uint8_t *b, int n, uint32_t seed) {
    for (int i = 0; i < n; i++) b[i] = (uint8_t)(((uint32_t)i * 31u + seed) & 0xffu);
}

static int coh_read(const char *path, uint8_t *buf, int max, uint32_t *h) {
    int n = fs_read_at(path, (char *)buf, 0, max);
    *h = (n > 0) ? fnv1a(buf, n) : 0u;
    return n;
}

static void coh_report(uint32_t id, int pass, int len, uint32_t hash,
                       uint32_t h_exp, int rc) {
    perf_begin("coh");
    perf_kv("case", id);
    perf_kv("pass", (uint32_t)(pass ? 1u : 0u));
    perf_kv("len", (uint32_t)(len < 0 ? 0 : len));
    perf_kv("hash", hash);
    perf_kv("hash_exp", h_exp);
    perf_kv("rc", (uint32_t)(rc < 0 ? 0 : rc));
    perf_end();
}

static void bench_fscoh(void) {
    uint32_t h, he;
    int n, ok;

    /* case 1: long -> short overwrite (shrink; cached size/cluster must move) */
    {
        const char *p = "/tmp/coha.txt";
        coh_fill(coh_w, 100, 1u); he = fnv1a(coh_w, 100);
        int rc1 = fs_write_text(p, (char *)coh_w, 100);
        n = coh_read(p, coh_w, FS_DATA_MAX, &h);
        int ok1 = rc1 == 0 && n == 100 && h == he;
        coh_fill(coh_w, 20, 2u); he = fnv1a(coh_w, 20);
        int rc2 = fs_write_text(p, (char *)coh_w, 20);
        n = coh_read(p, coh_w, FS_DATA_MAX, &h);
        ok = ok1 && rc2 == 0 && n == 20 && h == he;
        coh_report(1, ok, n, h, he, n);
    }

    /* case 2: short -> long overwrite (grow) */
    {
        const char *p = "/tmp/cohb.txt";
        coh_fill(coh_w, 20, 3u); he = fnv1a(coh_w, 20);
        int rc1 = fs_write_text(p, (char *)coh_w, 20);
        n = coh_read(p, coh_w, FS_DATA_MAX, &h);
        int ok1 = rc1 == 0 && n == 20 && h == he;
        coh_fill(coh_w, 100, 4u); he = fnv1a(coh_w, 100);
        int rc2 = fs_write_text(p, (char *)coh_w, 100);
        n = coh_read(p, coh_w, FS_DATA_MAX, &h);
        ok = ok1 && rc2 == 0 && n == 100 && h == he;
        coh_report(2, ok, n, h, he, n);
    }

    /* case 3: delete then recreate under the same name with new content */
    {
        const char *p = "/tmp/cohc.txt";
        coh_fill(coh_w, 40, 5u);
        int rc1 = fs_write_text(p, (char *)coh_w, 40);
        n = coh_read(p, coh_w, FS_DATA_MAX, &h);   /* warm the path cache */
        int ok1 = rc1 == 0 && n == 40;
        int rd = fs_delete(p);
        coh_fill(coh_w, 60, 6u); he = fnv1a(coh_w, 60);
        int rc2 = fs_write_text(p, (char *)coh_w, 60);
        n = coh_read(p, coh_w, FS_DATA_MAX, &h);
        ok = ok1 && rd == 0 && rc2 == 0 && n == 60 && h == he;
        coh_report(3, ok, n, h, he, n);
    }

    /* case 4: raw_mode truncate 100B -> 20B (dirent size must shrink) */
    {
        const char *p = "/tmp/cohd.txt";
        coh_fill(coh_w, 100, 7u); he = fnv1a(coh_w, 100);
        int rc1 = fs_write_raw_mode(p, (char *)coh_w, 100, 0);
        n = coh_read(p, coh_w, FS_DATA_MAX, &h);
        int ok1 = rc1 == 0 && n == 100 && h == he;
        coh_fill(coh_w, 20, 8u); he = fnv1a(coh_w, 20);
        int rc2 = fs_write_raw_mode(p, (char *)coh_w, 20, 0);
        n = coh_read(p, coh_w, FS_DATA_MAX, &h);
        uint32_t cl = 0, sz = 0;
        int rr = fs_resolve_file(p, &cl, &sz);
        ok = ok1 && rc2 == 0 && n == 20 && h == he && rr == 0 && sz == 20;
        coh_report(4, ok, n, h, he, n);
    }

    /* case 5: append 20B + 10B = 30B; content must be the concatenation */
    {
        const char *p = "/tmp/cohd.txt";   /* still 20B seed 8 from case 4 */
        coh_fill(coh_w, 10, 9u);
        int rc = fs_write_raw_mode(p, (char *)coh_w, 10, 1);
        coh_fill(coh_e, 20, 8u);
        coh_fill(coh_e + 20, 10, 9u); he = fnv1a(coh_e, 30);
        n = coh_read(p, coh_w, FS_DATA_MAX, &h);
        ok = rc == 0 && n == 30 && h == he;
        coh_report(5, ok, n, h, he, n);
    }

    /* case 6: cluster snapshot reuse.  Read X (warms the one-slot cluster
     * snapshot), write+delete+recreate Y/Z so the allocator can hand back a
     * reused cluster number; X must still read back its original bytes. */
    {
        const char *x = "/tmp/cohx.txt", *y = "/tmp/cohy.txt", *z = "/tmp/cohz.txt";
        coh_fill(coh_w, 300, 11u);
        int rcx = fs_write_text(x, (char *)coh_w, 300);
        uint32_t hx = fnv1a(coh_w, 300);
        int nx = coh_read(x, coh_e, FS_DATA_MAX, &h);   /* warm cc snapshot */
        int okx = rcx == 0 && nx == 300 && h == hx;

        coh_fill(coh_w, 300, 12u);
        int rcy = fs_write_text(y, (char *)coh_w, 300);
        int ny = coh_read(y, coh_w, FS_DATA_MAX, &h);
        int oky = rcy == 0 && ny == 300;
        int rdy = fs_delete(y);

        coh_fill(coh_w, 300, 13u); uint32_t hz = fnv1a(coh_w, 300);
        int rcz = fs_write_text(z, (char *)coh_w, 300);
        int nz = coh_read(z, coh_w, FS_DATA_MAX, &h);
        int okz = rcz == 0 && nz == 300 && h == hz;

        nx = coh_read(x, coh_w, FS_DATA_MAX, &h);
        ok = okx && oky && rdy == 0 && okz && nx == 300 && h == hx;
        coh_report(6, ok, nx, h, hx, nx);
    }
}

/* case 7: after a format the wiped volume must not serve cached dirent/data.
 * Runs on a qrun private disk copy only; never against catos_disk.img. */
static void bench_fsformat(void) {
    const char *p = "/tmp/cohf.txt";
    uint32_t h;
    int ok;
    coh_fill(coh_w, 50, 20u);
    int rc1 = fs_write_text(p, (char *)coh_w, 50);
    int n = coh_read(p, coh_w, FS_DATA_MAX, &h);
    int warm = rc1 == 0 && n == 50;

    if (fs_format() != 0) {
        coh_report(7, 0, 0, 0, 0, -1);
        return;
    }
    int n2 = fs_read_at(p, (char *)coh_w, 0, FS_DATA_MAX);
    uint32_t cl = 0, sz = 0;
    int rr = fs_resolve_file(p, &cl, &sz);
    ok = warm && n2 <= 0 && rr != 0;
    coh_report(7, ok, n2, 0, 0, n2);
}

/* ---- dispatcher ------------------------------------------------------- */
int bench_light_cmd(const char *arg) {
    const char *a = arg ? arg : "";
    while (*a == ' ') a++;

    /* Recognised subcommands: light (all), shell, syscall, login, fscheck,
     * fscoh (cache coherency gate), fsformat (format-wipe coherency). */
    int do_shell = 0, do_sys = 0, do_login = 0, do_fscheck = 0;
    int do_coh = 0, do_fmt = 0;
    if (*a == 0 || a[0] == 'l') {
        do_shell = do_sys = do_login = 1;
    } else if (a[0] == 's' && a[1] == 'h') {
        do_shell = 1;
    } else if (a[0] == 's') {
        do_sys = 1;
    } else if (a[0] == 'l' && a[1] == 'o') {
        do_login = 1;
    } else if (a[0] == 'f' && a[1] == 's' && a[2] == 'c' && a[3] == 'h') {
        do_fscheck = 1;
    } else if (a[0] == 'f' && a[1] == 's' && a[2] == 'c' && a[3] == 'o') {
        do_coh = 1;
    } else if (a[0] == 'f' && a[1] == 's' && a[2] == 'f') {
        do_fmt = 1;
    } else {
        return 0;
    }

    if (do_shell) bench_shell();
    if (do_sys)   bench_syscall();
    if (do_login) bench_login();
    if (do_fscheck) bench_fscheck();
    if (do_coh)   bench_fscoh();
    if (do_fmt)   bench_fsformat();
    return 1;
}
