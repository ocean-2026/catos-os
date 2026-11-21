/*
 * bsod.c -- v2.59-crashscreen full-screen kernel crash report.
 *
 * See include/bsod.h for the contract.  Everything here runs with
 * interrupts masked: the only rendering calls allowed are the vga_*
 * facade (con_* front-end); no heap, no FS, no shell, no IME, no font
 * loader is touched on the crash path.  The symbol table is a static
 * .rodata array emitted by tools/gen_symtab.py at build time.
 */
#include "bsod.h"
#include "io.h"
#include "render.h"
#include "vga.h"
#include "task.h"
#include "intr.h"
#include "cpu.h"
#include "fs.h"
#include "hive.h"
#include "palette.h"       /* v2.62: panicmode from registry */
#include "hw.h"
#include "diag.h"
#include "serial_log.h"
#include "symtab.inc"

#define BSOD_COLS 80
#define BSOD_ROWS 25

/* P0-2 second-fault guard.  Plain bss, no lock: set once on entry and only
 * read/incremented on the panic path.  The dispatcher treats any non-zero
 * value as "already panicking" and takes the hard halt shortcut. */
volatile int g_in_panic;
static int g_panic_depth;

/* Development build default: WAIT (stay on the report; no auto reboot).
 * /etc/panicmode overrides at boot; the crash path itself never opens a
 * file.  Release builds may flip this default to REBOOT. */
static int g_mode = BSOD_MODE_WAIT;
static int g_timeout_sec = BSOD_TIMEOUT_SEC;

/* Crash screen version tag, shown on the summary and in the panic record. */
#define BSOD_KERNEL_TAG "CatOS v2.59-crashscreen"

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

/* bounded append: returns chars copied */
static int ncpy_at(char *d, const char *s, int n) {
    int i = 0;
    if (!s) { d[0] = 0; return 0; }
    while (i < n - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = 0;
    return i;
}

/* P0-1 dual output: the same line goes to COM1 first (polled, timeout
 * drops the byte), then to the VGA facade.  Either channel failing must not
 * disturb the other: ser_* already drops on guard exhaustion, and the VGA
 * facade is the only rendering path allowed on this screen. */
static void dual_write_at(int x, int y, const char *s) {
    ser_write(s);
    vga_write_at((uint8_t)x, (uint8_t)y, s);
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

/* ---- snapshot captured at entry (stable across redraws) --------------- */

typedef struct {
    int      intno;
    uint32_t eip, cr2, err, eflags, esp_k, esp, ticks;
    int      ring, tid, cpu;
    char     task[16];
    uint32_t frames[5];      /* raw stack frames sampled at entry */
    int      nframes;
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

/* ---- symbol resolution (static .rodata table, no allocation) ----------- */

/* Binary search the nearest CODE symbol at or below `addr`.  Returns the
 * symbol index, or -1 when addr is below the first symbol. */
static int sym_nearest(uint32_t addr, uint32_t *off) {
    int lo = 0, hi = (int)g_symtab_n - 1, best = -1;
    while (lo <= hi) {
        int m = lo + ((hi - lo) >> 1);
        if (g_symtab[m].addr <= addr) { best = m; lo = m + 1; }
        else hi = m - 1;
    }
    if (best < 0) { if (off) *off = 0; return -1; }
    if (off) *off = addr - g_symtab[best].addr;
    return best;
}

/* Render `addr` as name+offset into a caller buffer (max 40 chars). */
void bsod_sym_fmt(char *out, int n, uint32_t addr) {
    uint32_t off = 0;
    int k = sym_nearest(addr, &off);
    if (k < 0) {
        static const char *h = "0123456789ABCDEF";
        int i = 0;
        out[i++] = '0'; out[i++] = 'x';
        for (int d = 0; d < 8; d++) out[i++] = h[(addr >> (28 - d * 4)) & 0xF];
        out[i] = 0;
        return;
    }
    const char *nm = g_symtab[k].name;
    int j = 0;
    while (nm[j] && j < n - 12) { out[j] = nm[j]; j++; }
    out[j] = 0;
    if (off) {
        out[j++] = '+';
        static const char *h = "0123456789ABCDEF";
        out[j++] = '0'; out[j++] = 'x';
        for (int d = 7; d >= 0; d--) out[j++] = h[(off >> (d * 4)) & 0xF];
    }
    out[j] = 0;
}

/* ---- CPU number (cpuid leaf 1 ebx[31:24], SMP deferred) ----------------- */

static int read_cpu(void) {
    uint32_t a, b, c, d;
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    return (int)(b >> 24);
}

/* ---- saved-fault second page (legacy F1 / S stack page) ----------------- */

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

/* ---- detail pages (D registers / S stack / M mapping / A cpus) --------- */

static void draw_regs_page(const snap_t *s, const int_frame_t *f) {
    vga_write_at(2, 10, "--- full register dump ---");
    vga_write_at(2, 11, "EIP=");   w_hex(6, 11, s->eip);
    vga_write_at(34, 11, "CR2=");  w_hex(39, 11, s->cr2);
    vga_write_at(2, 12, "ERR=");   w_hex(6, 12, s->err);
    vga_write_at(34, 12, "EFLAGS="); w_hex(42, 12, s->eflags);
    vga_write_at(2, 13, "ESP(k)="); w_hex(9, 13, s->esp_k);
    vga_write_at(34, 13, "ESP(u)="); w_hex(41, 13, s->esp);
    if (f) {
        vga_write_at(2, 15, "EAX="); w_hex(6, 15, f->eax);
        vga_write_at(34, 15, "EBX="); w_hex(39, 15, f->ebx);
        vga_write_at(2, 16, "ECX="); w_hex(6, 16, f->ecx);
        vga_write_at(34, 16, "EDX="); w_hex(39, 16, f->edx);
        vga_write_at(2, 17, "ESI="); w_hex(6, 17, f->esi);
        vga_write_at(34, 17, "EDI="); w_hex(39, 17, f->edi);
        vga_write_at(2, 18, "EBP="); w_hex(6, 18, f->ebp);
        vga_write_at(34, 18, "CS="); w_hex(39, 18, f->cs);
    }
}

static void draw_stack_page(const snap_t *s) {
    vga_write_at(2, 10, "--- stack trace (top frames, nearest symbol) ---");
    for (int i = 0; i < s->nframes; i++) {
        char b[44];
        bsod_sym_fmt(b, sizeof(b), s->frames[i]);
        vga_write_at(2, 11 + i, "#");
        w_int(4, 11 + i, i);
        vga_write_at(8, 11 + i, b);
    }
    if (!s->nframes) vga_write_at(2, 11, "no stack frames captured");
}

static void draw_map_page(const snap_t *s) {
    uint32_t cr3 = cpu_cr3_read();
    vga_write_at(2, 10, "--- memory mapping / page tables ---");
    vga_write_at(2, 11, "CR3="); w_hex(6, 11, cr3);
    vga_write_at(34, 11, "CR2="); w_hex(39, 11, s->cr2);
    vga_write_at(2, 12, "task ");
    vga_write_at(8, 12, s->task[0] ? s->task : "?");
    vga_write_at(30, 12, "tid "); w_int(34, 12, s->tid);
    vga_write_at(2, 14, "page-table summary: CR3 holds the current root");
    vga_write_at(2, 15, "directory physical base; full walk deferred.");
}

static void draw_cpus_page(const snap_t *s) {
    vga_write_at(2, 10, "--- per-CPU state ---");
    vga_write_at(2, 11, "CPU "); w_int(6, 11, s->cpu);
    vga_write_at(16, 11, "(SMP deferred: single CPU view; IPI/NMI stop flags are interface-only)");
    vga_write_at(2, 13, "per-CPU buffer: reserved interface, not active on UP build.");
}

/* ---- summary page ------------------------------------------------------- */

static void draw_summary(const snap_t *s, const char *msg, int soft,
                         int timed_out) {
    /* v2.67: crash colours come from HKLM\System\Crash\Colors; the
     * values are read once at boot (no FS access on the panic path). */
    con_set_style_rgb(pal_rgb(PAL_BSOD_FG), pal_rgb(PAL_BSOD_BG), 0);
    vga_clear();

    /* P2-9 narrow screen: on a small/serial-only surface the wide layout
     * would wrap off-grid.  Keep the core summary only: vector, EIP, reason,
     * hint.  The serial channel already receives the same core breadcrumb. */
    int narrow = con_cols() < 60;

    /* Title bar, inverse video so it stays legible without colour:
     * "!!! PANIC !!!" on a white/blue reversed row. */
    con_set_style_rgb(pal_rgb(PAL_BSOD_BG), pal_rgb(PAL_BSOD_TITLE), 0);
    vga_write_at(2, 0, "!!! PANIC !!!  " BSOD_KERNEL_TAG);
    con_set_style_rgb(pal_rgb(PAL_BSOD_FG), pal_rgb(PAL_BSOD_BG), 0);

    /* ASCII art smiley ":(" */
    vga_write_at(4, 1, "  ###    #");
    vga_write_at(4, 2, "  ###    #");
    vga_write_at(4, 3, "  ###");
    vga_write_at(4, 4, "  ###    #");
    vga_write_at(4, 5, "  ###    #");
    vga_write_at(4, 6, ":(");

    /* P0-4: title stays ASCII (GB2312 subset lacks several hanzi; #202). */
    dual_write_at(2, 8, "CatOS has encountered a problem and needs to restart.");

    /* === PANIC: CPU EXCEPTION 0x0E (#PF) === */
    char line[72];
    int o = 0;
    const char *tag = (s->intno >= 0) ? "CPU EXCEPTION" : "MANUAL PANIC";
    o += ncpy_at(line + o, tag, sizeof(line) - o);
    if (s->intno >= 0) {
        static const char *h = "0123456789ABCDEF";
        line[o++] = ' '; line[o++] = '0'; line[o++] = 'x';
        for (int i = 7; i >= 0; i--) line[o++] = h[(s->intno >> (i * 4)) & 0xF];
        line[o++] = ' ';
        const char *lb = exc_label(s->intno);
        for (int i = 0; lb[i]; i++) line[o++] = lb[i];
    }
    line[o] = 0;
    dual_write_at(2, 10, line);

    if (narrow) {
        /* narrow / serial-only surface: core summary only, no two-column
         * layout, no stack dump. */
        vga_write_at(2, 11, "FATAL RECOVER: NO");
        vga_write_at(2, 12, "EIP="); w_hex(6, 12, s->eip);
        if (msg && msg[0]) vga_write_at(2, 13, msg);
        return;
    }

    /* STOP code on the same row, right-aligned */
    vga_write_at(40, 10, stop_code(s->intno));

    /* FATAL / RECOVER row.  Ring0 fatal faults are NOT recoverable here;
     * a manual panic is equally fatal.  RECOVER: NO is the honest default. */
    vga_write_at(2, 11, "FATAL");
    vga_write_at(20, 11, "RECOVER: NO");
    vga_write_at(40, 11, "CPU "); w_int(44, 11, s->cpu);
    vga_write_at(54, 11, "ring "); w_int(59, 11, s->ring);

    /* task / tid row */
    vga_write_at(2, 12, "task ");
    vga_write_at(8, 12, s->task[0] ? s->task : "?");
    vga_write_at(30, 12, "tid ");
    w_int(34, 12, s->tid);

    /* register-ish summary: EIP / ERR / CR2(#PF only) / RSP / EFLAGS */
    vga_write_at(2, 13, "EIP=");   w_hex(6, 13, s->eip);
    vga_write_at(34, 13, "ERR=");  w_hex(39, 13, s->err);
    if (s->intno == 14) {
        vga_write_at(2, 14, "CR2="); w_hex(6, 14, s->cr2);
        vga_write_at(34, 14, "RSP="); w_hex(39, 14, s->esp_k);
    } else {
        vga_write_at(34, 14, "RSP="); w_hex(39, 14, s->esp_k);
    }
    vga_write_at(2, 15, "EFLAGS="); w_hex(9, 15, s->eflags);
    vga_write_at(34, 15, "tick "); w_hex(39, 15, s->ticks);

    /* EIP symbol: nearest function + offset */
    char sym[44];
    bsod_sym_fmt(sym, sizeof(sym), s->eip);
    vga_write_at(2, 16, "EIP->");
    vga_write_at(8, 16, sym);

    /* short reason / panic message */
    if (msg && msg[0]) vga_write_at(2, 18, msg);

    /* stack top 3 frames with symbol+offset */
    vga_write_at(2, 19, "stack:");
    for (int i = 0; i < s->nframes && i < 3; i++) {
        char b[44];
        bsod_sym_fmt(b, sizeof(b), s->frames[i]);
        vga_write_at(8, 19 + i, b);
    }

    /* hint row + countdown.  Narrow screen keeps a one-line hint only. */
    if (timed_out) {
        dual_write_at(2, 23, "auto-restart timeout reached.");
    } else if (narrow) {
        dual_write_at(2, 23, "R reboot; other keys unavailable on this view.");
    } else {
        dual_write_at(2, 23,
            "R reboot  D regs  S stack  M map  P dump  A cpus  C continue");
    }
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

/* ---- scan-code -> ASCII (set 1, make code only) ------------------------- */

static int sc_to_ascii(uint8_t sc) {
    switch (sc) {
    case 0x10: return 'q'; case 0x11: return 'w'; case 0x12: return 'e';
    case 0x13: return 'r'; case 0x14: return 't'; case 0x15: return 'y';
    case 0x16: return 'u'; case 0x17: return 'i'; case 0x18: return 'o';
    case 0x19: return 'p'; case 0x1E: return 'a'; case 0x1F: return 's';
    case 0x20: return 'd'; case 0x21: return 'f'; case 0x22: return 'g';
    case 0x23: return 'h'; case 0x24: return 'j'; case 0x25: return 'k';
    case 0x26: return 'l'; case 0x2C: return 'z'; case 0x2D: return 'x';
    case 0x2E: return 'c'; case 0x2F: return 'v'; case 0x30: return 'b';
    case 0x31: return 'n'; case 0x32: return 'm'; case 0x3B: return 0; /* F1 */
    default:   return -1;   /* unknown / non-ASCII: ignore */
    }
}

/* ---- SMP interface placeholders (UP build; deferred) ---------------------
 *
 * CatOS is still uniprocessor.  The crash screen reserves the per-CPU
 * contract so an SMP port can fill these without touching the painter:
 *   - per-CPU snapshot buffer: one snap_t per CPU, indexed by cpu#;
 *   - NMI/IPI stop flag: other cores are parked via IPI before the first
 *     core paints; each parked core sets its stop marker;
 *   - line prefix "[CPU n]" on every dual-output breadcrumb.
 * On the UP build only CPU 0 is ever sampled; draw_cpus_page() reports the
 * deferred state explicitly.  No IPI/NMI code path is wired yet.
 */
static volatile uint8_t g_cpu_stop_flag[1] = { 0 };   /* placeholder array */

/* ---- panic record (bss fixed area; no FS on the crash path) ------------- */

static panic_record_t g_panic_record;   /* bss, fixed address, no allocation */

static void panic_record_clear(void) { g_panic_record.magic = 0; }

static void panic_record_save(const snap_t *s, const char *msg) {
    g_panic_record.magic  = PANIC_RECORD_MAGIC;
    g_panic_record.ticks  = s->ticks;
    g_panic_record.intno  = s->intno;
    g_panic_record.eip    = s->eip;
    g_panic_record.cr2    = s->cr2;
    g_panic_record.err    = s->err;
    g_panic_record.eflags = s->eflags;
    g_panic_record.esp_k  = s->esp_k;
    g_panic_record.nframes = s->nframes;
    for (int i = 0; i < PANIC_FRAMES_MAX; i++)
        g_panic_record.frames[i] = (i < s->nframes) ? s->frames[i] : 0;
    ncpy(g_panic_record.version, BSOD_KERNEL_TAG,
         sizeof(g_panic_record.version));
    ncpy(g_panic_record.msg, msg ? msg : "(no message)",
         sizeof(g_panic_record.msg));
}

/* Read-back accessor for the shell `crashlog` builtin. */
const panic_record_t *panic_record_get(void) { return &g_panic_record; }
uint32_t panic_record_magic(void) { return g_panic_record.magic; }

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
    s.esp_k  = f ? f->esp_k  : (uint32_t)&s;
    s.esp    = f ? f->esp    : 0;
    s.ticks  = timer_ticks;
    s.cpu    = read_cpu();

    task_t *t = task_current();
    if (t) {
        ncpy(s.task, t->name, sizeof(s.task));
        s.tid  = t->tid;
        s.ring = t->ring0 ? 0 : 3;
    } else {
        s.task[0] = 0; s.tid = -1; s.ring = -1;
    }
    if (f) s.ring = (int)(f->cs & 3);

    /* Sample raw frames off the kernel stack: walk esp_k toward the top.
     * Guard every read against a clearly invalid pointer. */
    s.nframes = 0;
    uint32_t *sp = (uint32_t *)s.esp_k;
    for (int i = 0; i < PANIC_FRAMES_MAX; i++) {
        uint32_t v = (uint32_t)sp[i];
        s.frames[i] = v;
        if (v >= 0x1000000u && v < 0xFFFFF000u) s.nframes = i + 1;
    }

    /* Persist the fixed-area record before painting (no FS write). */
    panic_record_save(&s, msg);

    /* P0-1 dual output: breadcrumb the summary to COM1 as well.
     * P2-9 SMP placeholder: line prefix "[CPU n]". */
    ser_write("\n[CPU ");
    { static const char *d = "0123456789"; ser_putc(d[s.cpu % 10]); }
    ser_write("] [bsod] === PANIC: ");
    ser_write(s.intno >= 0 ? "CPU EXCEPTION" : "MANUAL PANIC");
    ser_hex(" vector=", (uint32_t)s.intno);
    ser_hex("eip=", s.eip); ser_hex("err=", s.err);
    ser_hex("cr2=", s.cr2); ser_hex("esp=", s.esp_k);
    ser_write(msg ? msg : ""); ser_write("\n");

    enum { PAGE_SUMMARY, PAGE_REGS, PAGE_STACK, PAGE_MAP, PAGE_CPUS,
           PAGE_INFO };
    int page = PAGE_SUMMARY;
    int dump_queued = 0;

    draw_summary(&s, msg, soft_return, 0);

    /* flush any stale scancodes left in the 8042 output buffer */
    while (inb(0x64) & 1) (void)inb(0x60);

    int mhz = hw_num(HW_CPU_MHZ);
    uint64_t t0 = rdtsc_();
    /* MHz = millions of cycles / second; N s of TSC = mhz * 1e6 * N. */
    uint64_t deadline = mhz > 0 && mhz < 10000
        ? t0 + (uint64_t)mhz * 1000000u * (uint32_t)g_timeout_sec : 0;
    int timed_out = 0;

    for (;;) {
        if (inb(0x64) & 1) {
            uint8_t sc = inb(0x60);
            if (!(sc & 0x80)) {            /* make code, ignore break */
                int a = sc_to_ascii(sc);
                if (a == 0) {              /* F1: saved faultinfo page */
                    page = (page == PAGE_INFO) ? PAGE_SUMMARY : PAGE_INFO;
                } else if (a > 0) {
                    char k = (char)a;
                    switch (k) {
                    case 'r': case 'R':
                        if (soft_return) goto soft_exit;
                        goto reboot_now;
                    case 'd': case 'D':
                        page = PAGE_REGS; break;
                    case 's': case 'S':
                        page = PAGE_STACK; break;
                    case 'm': case 'M':
                        page = PAGE_MAP; break;
                    case 'p': case 'P':
                        dump_queued = 1;
                        /* P: only show `dump queued`, no real FS write. */
                        break;
                    case 'a': case 'A':
                        page = PAGE_CPUS; break;
                    case 'c': case 'C':
                        /* Continue is only valid for RECOVER: YES; this
                         * screen always reports RECOVER: NO, so refuse. */
                        vga_write_at(2, 22, "continue not available (RECOVER: NO)");
                        continue;
                    default:
                        break;
                    }
                }
                /* full-screen redraw on page switch (no incremental scroll) */
                switch (page) {
                case PAGE_REGS:  draw_regs_page(&s, f); break;
                case PAGE_STACK: draw_stack_page(&s);   break;
                case PAGE_MAP:   draw_map_page(&s);     break;
                case PAGE_CPUS:  draw_cpus_page(&s);    break;
                case PAGE_INFO:  draw_info_page(&s);    break;
                default:         draw_summary(&s, msg, soft_return, timed_out);
                }
                if (dump_queued)
                    vga_write_at(2, 22, "dump queued");
                while (inb(0x64) & 1) (void)inb(0x60);
                continue;
            }
            continue;
        }
        if (deadline && rdtsc_() >= deadline) { timed_out = 1; break; }
        io_wait();
    }

soft_exit:
    if (soft_return) {
        /* The soft test path leaves the crash screen and returns to the
         * shell; clear the guard so a later deliberate crash does not look
         * like a nested fault.  The hard paths below never reach here. */
        g_in_panic = 0;
        g_panic_depth = 0;
        panic_record_clear();
        con_force_text();
        vga_clear();
        __asm__ volatile ("sti");
        return;
    }

reboot_now:
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
    /* v2.62: registry first, legacy file as fallback/migration source. */
    const char *rv = hive_sz("HKLM\\System\\Crash\\Mode", "");
    if (rv && *rv) {
        int i = 0;
        while (*rv && i < (int)sizeof(buf) - 1) buf[i++] = *rv++;
        buf[i] = 0;
    } else {
        int n = fs_read_text("/etc/panicmode", buf, (int)sizeof(buf) - 1);
        if (n <= 0) return;
        buf[n] = 0;
        hive_set_sz("HKLM\\System\\Crash\\Mode", buf); hive_save(); /* migrate */
    }
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

void bsod_set_wait(void) { g_mode = BSOD_MODE_WAIT; }
