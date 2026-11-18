/*
 * bsod.c -- v2.59 self-contained full-screen kernel crash report.
 *
 * See include/bsod.h for the contract.  Everything here runs with
 * interrupts masked: the only rendering calls allowed are the vga_*
 * facade (con_* front-end) plus a direct 0xB8000 fallback; no heap, no
 * FS, no shell, no IME, no font loader is touched on the crash path.
 */
#include "bsod.h"
#include "io.h"
#include "render.h"
#include "vga.h"
#include "task.h"
#include "intr.h"
#include "cpu.h"
#include "fs.h"
#include "hw.h"
#include "diag.h"
#include "serial_log.h"

#define BSOD_COLS 80
#define BSOD_ROWS 25

/* P0-2 second-fault guard.  Plain bss, no lock: set once on entry and only
 * read/incremented on the panic path.  The dispatcher treats any non-zero
 * value as "already panicking" and takes the hard halt shortcut. */
volatile int g_in_panic;
static int g_panic_depth;

static int g_mode = BSOD_MODE_REBOOT;
static int g_timeout_sec = BSOD_TIMEOUT_SEC;

/* P0-2: hard halt after a nested fatal fault.  No painting, no serial,
 * no recursion into bsod_enter: mask IRQs and spin on hlt. */
static void bsod_halt_loop(void) {
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

/* ---- tiny local strings (no libc on the crash path) ------------------- */

static int slen(const char *s) {
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static int has_tok(const char *s, const char *tok) {
    if (!s || !tok) return 0;
    int tl = slen(tok);
    for (int i = 0; s[i]; i++) {
        int j = 0;
        while (j < tl && s[i + j] && s[i + j] == tok[j]) j++;
        if (j == tl) return 1;
    }
    return 0;
}

static void ncpy(char *d, const char *s, int n) {
    int i = 0;
    if (!s) { d[0] = 0; return; }
    while (i < n - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = 0;
}

/* P0-1 dual output: the same line goes to COM1 first (polled, timeout
 * drops the byte), then to the VGA facade.  Either channel failing must not
 * disturb the other: ser_* already drops on guard exhaustion, and the VGA
 * facade is the only rendering path allowed on this screen. */
static void dual_write_at(int x, int y, const char *s) {
    ser_write(s);
    vga_write_at((uint8_t)x, (uint8_t)y, s);
}

static void dual_puts(const char *s) {
    ser_write(s);
    vga_write(s);
}

/* ---- STOP code mapping (fixed table, documented in docs/PROGRESS.md) --- */

static const char *stop_code(int intno) {
    switch (intno) {
    case 0:  return "DIVIDE_BY_ZERO";
    case 6:  return "INVALID_OPCODE";
    case 13: return "IRQL_NOT_LESS_OR_EQUAL";
    case 14: return "PAGE_FAULT_IN_NONPAGED_AREA";
    default: return "UNEXPECTED_KERNEL_MODE_TRAP";
    }
}

static const char *exc_label(int intno) {
    static const char *nm[16] = {
        "#DE", "#DB", "#NMI", "#BP", "#OF", "#BR", "#UD", "#NM",
        "#DF", "#CO", "#TS", "#NP", "#SS", "#GP", "#PF", "#RES"
    };
    return (intno >= 0 && intno < 16) ? nm[intno] : "#??";
}

/* ---- snapshot captured at entry (stable across F1 redraws) ---------- */

typedef struct {
    int      intno;
    uint32_t eip, cr2, err, eflags, ticks;
    int      ring, tid;
    char     task[16];
} snap_t;

/* ---- positioned frontend writers (stack buffer -> vga_write_at) ------ */

static void w_hex(int x, int y, uint32_t v) {
    static const char *h = "0123456789ABCDEF";
    char b[11];
    b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 8; i++) b[2 + i] = h[(v >> (28 - i * 4)) & 0xF];
    b[10] = 0;
    vga_write_at((uint8_t)x, (uint8_t)y, b);
}

static void w_int(int x, int y, int v) {
    char b[12];
    int i = 11;
    b[11] = 0;
    if (v < 0) { b[--i] = '-'; v = -v; }
    if (v == 0) b[--i] = '0';
    while (v > 0) { b[--i] = (char)('0' + v % 10); v /= 10; }
    vga_write_at((uint8_t)x, (uint8_t)y, &b[i]);
}

/* ---- saved-fault (F1) second page ------------------------------------- */

static void draw_info_page(const snap_t *s) {
    fault_record_t *r = &g_fault_last;
    (void)s;
    vga_write_at(2, 10, "saved crash info (faultinfo):");
    if (!r->valid) {
        vga_write_at(2, 11, "no fault recorded since boot");
        return;
    }
    vga_write_at(2, 11, "vector ");
    w_int(10, 11, r->intno);
    vga_write_at(20, 11, "task ");
    vga_write_at(26, 11, r->task[0] ? r->task : "?");
    vga_write_at(2, 12, "tid ");
    w_int(6, 12, r->tid);
    vga_write_at(18, 12, "ring ");
    w_int(24, 12, r->ring);
    vga_write_at(2, 13, "EIP=");  w_hex(6, 13, r->eip);
    vga_write_at(34, 13, "CR2="); w_hex(39, 13, r->cr2);
    vga_write_at(2, 14, "ERR=");  w_hex(6, 14, r->err);
    vga_write_at(34, 14, "EFLAGS="); w_hex(42, 14, r->eflags);
    vga_write_at(2, 15, "tick "); w_hex(7, 15, r->ticks);
    if (r->intno == 14) {
        vga_write_at(2, 17,  (r->err & 1) ? "present " : "not-present ");
        vga_write_at(16, 17, (r->err & 2) ? "write " : "read ");
        vga_write_at(24, 17, (r->err & 4) ? "user " : "supervisor ");
        if (r->err & 8)  vga_write_at(38, 17, "rsvd");
        if (r->err & 16) vga_write_at(44, 17, "instr-fetch");
    }
}

/* ---- main screen -------------------------------------------------------- */

static void draw_screen(const snap_t *s, const char *msg, int soft, int info) {
    vga_set_color(CON_WHITE, CON_BLUE);
    vga_clear();

    /* ASCII art smiley ":(" -- paren column + colon dots. */
    vga_write_at(4, 1, "  ###    #");
    vga_write_at(4, 2, "  ###    #");
    vga_write_at(4, 3, "  ###");
    vga_write_at(4, 4, "  ###    #");
    vga_write_at(4, 5, "  ###    #");
    vga_write_at(4, 6, ":(");

    /* P0-4: title stays ASCII.  The built-in GB2312 hanzi subset
     * (font_hz.inc) does not cover several glyphs in the previous Chinese
     * title (遇/严/重/问/题/新/启/动/，), so those would have fallen
     * back to '?' cells on the crash screen.  Per the deps checklist the
     * crash path degrades to the English line instead of relying on a font
     * that may be missing; evidence for the built-in table is in
     * docs/panic-tiers.md §4 and docs/CatOS-内核问题清单.md entry #202. */
    dual_write_at(2, 8, "CatOS has encountered a problem and needs to restart.");

    if (!info) {
        vga_write_at(2, 10, "STOP: ");
        vga_write_at(8, 10, stop_code(s->intno));
        if (s->intno >= 0) {
            vga_write_at(46, 10, exc_label(s->intno));
            w_hex(52, 10, (uint32_t)s->intno);
        } else {
            vga_write_at(46, 10, "manual");
        }

        vga_write_at(2, 11, "task ");
        vga_write_at(8, 11, s->task[0] ? s->task : "?");
        vga_write_at(30, 11, "tid ");
        w_int(34, 11, s->tid);
        vga_write_at(44, 11, "ring ");
        w_int(49, 11, s->ring);

        vga_write_at(2, 12, "EIP=");   w_hex(6, 12, s->eip);
        vga_write_at(34, 12, "CR2=");  w_hex(39, 12, s->cr2);
        vga_write_at(2, 13, "ERR=");   w_hex(6, 13, s->err);
        vga_write_at(34, 13, "EFLAGS="); w_hex(42, 13, s->eflags);
        vga_write_at(2, 14, "tick ");  w_hex(7, 14, s->ticks);

        if (msg && msg[0]) vga_write_at(2, 16, msg);
    } else {
        draw_info_page(s);
    }

    dual_write_at(2, 23, "Press a key to reboot; F1: saved crash info.");
    if (soft) vga_write_at(2, 24, "(--no-reboot: keypress returns to shell)");
}

/* ---- low-level helpers --------------------------------------------------- */

static uint32_t read_eflags(void) {
    uint32_t e;
    __asm__ volatile ("pushfl; pop %0" : "=r"(e) : : "memory");
    return e;
}

static uint64_t rdtsc_(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ---- public entry -------------------------------------------------------- */

void bsod_enter(const char *msg, const int_frame_t *f, int soft_return) {
    __asm__ volatile ("cli");

    /* P0-2 second-fault guard: if a fatal fault fires while we are already
     * on the crash screen, do NOT re-enter the painter.  Burn one depth
     * level; past the cap, or on any nested triple fault, hard halt. */
    if (g_in_panic) {
        g_panic_depth++;
        if (g_panic_depth >= BSOD_PANIC_DEPTH_MAX) {
            ser_write("[bsod] nested fault -> hard halt\n");
            bsod_halt_loop();
        }
    }
    g_in_panic = 1;

    /* If we are sitting in a graphics back-end, force the emergency switch
     * back to 80x25 text before painting.  The facade writes go through
     * whichever text surface ends up mapped. */
    if (con_get_mode() != CON_MODE_TEXT) con_force_text();

    snap_t s;
    s.intno  = f ? f->intno : -1;
    s.eip    = f ? f->eip   : (uint32_t)__builtin_return_address(0);
    s.cr2    = cpu_cr2_read();
    s.err    = f ? f->err    : 0;
    s.eflags = f ? f->eflags : read_eflags();
    s.ticks  = timer_ticks;

    task_t *t = task_current();
    if (t) {
        ncpy(s.task, t->name, sizeof(s.task));
        s.tid  = t->tid;
        s.ring = t->ring0 ? 0 : 3;
    } else {
        s.task[0] = 0; s.tid = -1; s.ring = -1;
    }
    if (f) s.ring = (int)(f->cs & 3);

    int info = 0;
    draw_screen(&s, msg, soft_return, info);

    /* flush any stale scancodes left in the 8042 output buffer */
    while (inb(0x64) & 1) (void)inb(0x60);

    int mhz = hw_num(HW_CPU_MHZ);
    uint64_t t0 = rdtsc_();
    /* MHz = millions of cycles / second; N s of TSC = mhz * 1e6 * N. */
    uint64_t deadline = mhz > 0 && mhz < 10000
        ? t0 + (uint64_t)mhz * 1000000u * (uint32_t)g_timeout_sec : 0;

    for (;;) {
        if (inb(0x64) & 1) {
            uint8_t sc = inb(0x60);
            if (!(sc & 0x80)) {            /* make code, ignore break */
                if (sc == 0x3B) {          /* F1: saved crash info */
                    info ^= 1;
                    draw_screen(&s, msg, soft_return, info);
                    while (inb(0x64) & 1) (void)inb(0x60);
                    continue;
                }
                break;                     /* any other key */
            }
            continue;
        }
        if (deadline && rdtsc_() >= deadline) break;
        io_wait();
    }

    if (soft_return) {
        con_force_text();
        vga_clear();
        __asm__ volatile ("sti");
        return;
    }

    if (g_mode == BSOD_MODE_HALT || g_mode == BSOD_MODE_WAIT) {
        /* halt: stay on the report forever.  wait: same visible behaviour
         * for the crash screen (no auto action); only a key can act. */
        __asm__ volatile ("cli");
        for (;;) __asm__ volatile ("hlt");
    }

    outb(0x64, 0xFE);                      /* 8042 CPU reset */
    for (;;) __asm__ volatile ("hlt");
}

void bsod_init(void) {
    if (!fs_fs_ready()) return;
    static char buf[80];
    int n = fs_read_text("/etc/panicmode", buf, (int)sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = 0;
    /* P2-9: values are `reboot`, `halt`, `wait`, or `reboot:<seconds>`
     * (e.g. `reboot:30s`).  Parsed once at boot; the crash path itself
     * never opens a file.  Development builds default to WAIT. */
    if (has_tok(buf, "halt")) {
        g_mode = BSOD_MODE_HALT;
    } else if (has_tok(buf, "wait")) {
        g_mode = BSOD_MODE_WAIT;
    } else if (has_tok(buf, "reboot")) {
        g_mode = BSOD_MODE_REBOOT;
        /* optional `reboot:<N>s` countdown override */
        const char *p = buf;
        while (*p) {
            if (p[0] == 'r' && p[1] == 'e' && p[2] == 'b' && p[3] == 'o' &&
                p[4] == 'o' && p[5] == 't' && p[6] == ':') {
                const char *q = p + 7;
                int sec = 0;
                while (*q >= '0' && *q <= '9') {
                    sec = sec * 10 + (*q - '0');
                    q++;
                }
                if (sec >= 1 && sec <= 600) g_timeout_sec = sec;
                break;
            }
            p++;
        }
    }
}

int bsod_mode(void) { return g_mode; }

void bsod_set_mode(int mode) {
    g_mode = (mode == BSOD_MODE_HALT) ? BSOD_MODE_HALT : BSOD_MODE_REBOOT;
}
