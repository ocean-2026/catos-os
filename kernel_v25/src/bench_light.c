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

/* ---- dispatcher ------------------------------------------------------- */
int bench_light_cmd(const char *arg) {
    const char *a = arg ? arg : "";
    while (*a == ' ') a++;

    /* Recognised subcommands: light (all), shell, syscall, login. */
    int do_shell = 0, do_sys = 0, do_login = 0;
    if (*a == 0 || a[0] == 'l') {
        do_shell = do_sys = do_login = 1;
    } else if (a[0] == 's' && a[1] == 'h') {
        do_shell = 1;
    } else if (a[0] == 's') {
        do_sys = 1;
    } else if (a[0] == 'l' && a[1] == 'o') {
        do_login = 1;
    } else {
        return 0;
    }

    if (do_shell) bench_shell();
    if (do_sys)   bench_syscall();
    if (do_login) bench_login();
    return 1;
}
