/* ctop.c - CatOS task manager TUI (real kernel data, CatOS semantic tokens)
 *
 * Port of the win-demo `catos-top` design into a real ring3 CE app:
 * the CPU bar is the kernel's own busy% sampler (hw_num(HW_CPU_BUSY_PCT)),
 * MEM/DSK come from the frame allocator and the ATA IDENTIFY cache, and the
 * process table is the live kernel task table through the v2.48 task_get()
 * snapshot slot (falls back to hw counters on a pre-v2.48 kernel).
 *
 * Keys: q/Esc quit   T theme   P/M sort cpu|mem   Up/Down select
 * Rendering: cls() + full repaint (the fmgr_port pattern; no gotoxy
 * dependency so the app also runs on kernels older than v2.21).
 */
#include "ce_abi.h"
catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

typedef unsigned int  u32;
typedef unsigned char u8;

/* hw_num selectors (include/hw.h, kernel-side; CE apps carry the values) */
#define H_CPU_MHZ    1
#define H_CPU_LOGICAL 16
#define H_MEM_TOTAL  6
#define H_MEM_USED   8
#define H_DISK_MB    10
#define H_NIC_UP     12
#define H_UPTIME     13
#define H_TASKS      14
#define H_TICKS      15
#define H_CPU_BUSY   18   /* v2.48: -1 on older kernels */

/* task states (include/task.h) */
static const char *state_names[] = {
    "UNUSED", "READY ", "RUN   ", "SLEEP ", "KB    ", "CHILD ", "ZOMBIE"
};

#define MAX_T 16
#define BARW   18

typedef struct {
    int tid, uid, st, ring;
    u32 ck, pg;
    char nm[16];
} taskrow_t;

static taskrow_t cur[MAX_T], prev[MAX_T];
static int nr = 0;
static int sel = 0;
static int theme = 0;         /* 0 dark, 1 light */
static int sort_mem = 0;
static int cpu_pct = 0;       /* windowed busy% from the kernel sampler */
static u32  ticks0, cksum0;

/* palette slots: 0=fg1 text, 1=fg2 muted, 2=border, 3=accent, 4=ok,
 * 5=warn, 6=bad, 7=info */
typedef struct { int fg[8]; int bg; } theme_t;
static const theme_t TH[2] = {
    /* dark: light text on black/dark-blue */
    { {7, 8, 12, 14, 10, 11, 12, 9}, 0 },
    /* light: dark text on white */
    { {0, 9, 4, 1, 2, 6, 1, 5}, 15 },
};

static catos_api_t *A(void) { return __ce_api; }

static void st(int slot, int bg, int attrs) {
    catos_api_t *a = A();
    int fg = TH[theme].fg[slot >= 0 ? slot : 0];
    if (bg < 0) bg = TH[theme].bg;
    a->set_style(fg, bg, attrs);
}

static void W(const char *s) { A()->write(s); }
static void NL(void) { W("\n"); }
static void PAD(int n) { int i; for (i = 0; i < n; i++) A()->putc(' '); }

static void I2A(int v, char *b) {
    b[0] = 0;
    if (v < 0) { b[0] = '-'; v = -v; }
    char tmp[12]; int n = 0;
    do { tmp[n++] = '0' + (v % 10); v /= 10; } while (v && n < 11);
    int i, l = 0;
    for (i = n - 1; i >= 0; i--) b[l++] = tmp[i];
    b[l] = 0;
}

static void print_pad(int v, int w) {
    char b[12];
    I2A(v, b);
    int l = 0; while (b[l]) l++;
    PAD(w - l);
    W(b);
}

/* right-aligned fixed width */
static void print_padr(int v, int w) {
    char b[12];
    I2A(v, b);
    int l = 0; while (b[l]) l++;
    PAD(w - l);
    W(b);
}

static void bar(int pct, int width, int slot) {
    int on = (pct * width) / 100;
    if (on > width) on = width;
    if (on < 0) on = 0;
    st(slot, -1, 0x01);
    W("[");
    int i;
    for (i = 0; i < width; i++) A()->putc(i < on ? '#' : '.');
    W("]");
    st(0, -1, 0);
    print_padr(pct, 4);
}

/* ---------------- data collection ---------------- */

static void read_tasks(u32 now) {
    catos_api_t *a = A();
    int i;
    if (a->task_get == 0) return;            /* pre-v2.48 kernel: no rows */
    nr = 0;
    for (i = 0; i < MAX_T; i++) {
        ce_task_info_t info;
        int rc;
        /* stack-local, right after validation: task_get copies the
         * struct in through the task's own page tables */
        rc = a->task_get(i, &info);
        if (rc != 1) continue;
        taskrow_t *r = &cur[nr++];
        r->tid = info.tid;
        r->uid = info.uid;
        r->st = info.state;
        r->ring = info.ring;
        r->ck = info.cpu_ticks;
        r->pg = info.pages;
        for (int j = 0; j < 15 && info.name[j]; j++) r->nm[j] = info.name[j];
        r->nm[15] = 0;
    }
    (void)now;
}

static u32 cksum(void) {
    u32 s = 0;
    int i;
    for (i = 0; i < nr; i++) s += cur[i].ck;
    return s;
}

static int task_pct(int i) {
    u32 d = 0;
    if (i < nr) d = cur[i].ck >= prev[i].ck ? cur[i].ck - prev[i].ck : 0;
    /* window = 1 s = 100 ticks */
    return (int)(d * 100 / 100u);
}

static int mem_cmp(const taskrow_t *a, const taskrow_t *b) {
    return (int)(b->pg - a->pg);
}

static void sort_tasks(void) {
    /* insertion sort, 16 rows max */
    int i, j;
    if (!sort_mem) return;
    for (i = 1; i < nr; i++) {
        taskrow_t tmp = cur[i];
        j = i - 1;
        while (j >= 0 && mem_cmp(&cur[j], &tmp) > 0) {
            cur[j + 1] = cur[j];
            j--;
        }
        cur[j + 1] = tmp;
    }
}

/* ---------------- draw ---------------- */

static void draw(void) {
    catos_api_t *a = A();
    int mem_tot, mem_used, mem_pct, disk_mb, up;
    char tb[24];

    mem_tot = a->hw_num(H_MEM_TOTAL);
    mem_used = a->hw_num(H_MEM_USED);
    mem_pct = (mem_tot > 0) ? (mem_used * 100 / mem_tot) : 0;
    if (mem_pct > 100) mem_pct = 100;
    disk_mb = a->hw_num(H_DISK_MB);
    up = a->hw_num(H_UPTIME) / 1000;

    a->cls();
    st(3, -1, 0x01);
    W("catos-top");
    st(1, -1, 0);
    a->time_str(tb);
    W("  --  ");
    W(tb);
    W("  up ");
    print_pad(up, 4);
    W("s");
    NL();
    st(2, -1, 0);
    { int i; for (i = 0; i < 78; i++) A()->putc('-'); }
    NL();

    st(7, -1, 0x01);
    W("CPU ");
    st(0, -1, 0);
    print_pad(a->hw_num(H_CPU_LOGICAL), 3);
    W(" log ");
    st(4, -1, 0);
    bar(cpu_pct, BARW, 4);
    NL();
    W("MEM ");
    print_pad(mem_used, 6);
    W("/");
    print_pad(mem_tot, 6);
    W("K ");
    bar(mem_pct, BARW, 7);
    NL();
    W("DSK ");
    print_pad(disk_mb, 6);
    W("MB ");
    st(a->hw_num(H_NIC_UP) ? 4 : 5, -1, 0);
    W(a->hw_num(H_NIC_UP) ? "NET up " : "NET dn ");
    st(0, -1, 0);
    NL();

    /* process table */
    st(3, -1, 0x01);
    W("PROCESS  sort=");
    W(sort_mem ? "MEM" : "CPU");
    W("   (");
    print_pad(nr, 2);
    W(" live)");
    st(2, -1, 0);
    NL();
    st(1, -1, 0);
    W(" TID  USER   S      CPU%  MEM(K) NAME");
    st(0, -1, 0);
    NL();

    sort_tasks();
    int i;
    for (i = 0; i < nr; i++) {
        const taskrow_t *r = &cur[i];
        st(i == sel ? 3 : 0, -1, i == sel ? 0x01 : 0);
        W(i == sel ? " >" : "  ");
        print_padr(r->tid, 3);
        W(" ");
        print_padr(r->uid, 4);
        W("  ");
        if (r->st >= 0 && r->st <= 6) W(state_names[r->st]);
        else W("?      ");
        W(" ");
        print_pad(task_pct(i), 4);
        W(" ");
        print_pad((int)(r->pg * 4), 6);
        W(" ");
        /* clip the name to 16 columns */
        {
            int n = 0;
            while (n < 15 && r->nm[n]) n++;
            for (int k = 0; k < n; k++) A()->putc(r->nm[k]);
            PAD(16 - n);
        }
        st(0, -1, 0);
        NL();
    }

    st(1, -1, 0);
    W(" q quit  T theme  P/M sort  up/down select  theme: ");
    st(3, -1, 0);
    W(theme ? "light" : "dark");
    st(0, -1, 0);
    NL();
}

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *a = A();
    int k;
    (void)argc; (void)argv;

    ticks0 = a->hw_num(H_TICKS);
    cksum0 = 0;
    read_tasks(0);
    for (int i = 0; i < nr; i++) prev[i] = cur[i];

    while (1) {
        u32 tnow = a->hw_num(H_TICKS);
        u32 win = tnow - ticks0;
        if (win >= 100) {                 /* 1 s window: 100 Hz PIT */
            /* CPU windowed busy%: prefer the kernel sampler, fall back
             * to the task-table tick diff on older kernels */
            int sb = a->hw_num(H_CPU_BUSY);
            if (sb >= 0) {
                cpu_pct = sb;
            } else {
                u32 s1 = cksum();
                u32 dt = s1 >= cksum0 ? s1 - cksum0 : 0;
                cpu_pct = (int)(dt * 100 / (win * (u32)nr + 1u));
                if (cpu_pct > 100) cpu_pct = 100;
            }
            for (int i = 0; i < nr; i++) prev[i] = cur[i];
            read_tasks(tnow);
            ticks0 = tnow;
            cksum0 = cksum();
            draw();
        }

        if (a->pollkey) {
            while ((k = a->pollkey()) >= 0) {
                if (k == 'q' || k == 27) goto done;
                if (k == 'T' || k == 't') theme ^= 1;
                if (k == 'P' || k == 'M' || k == 'p' || k == 'm') sort_mem ^= 1;
                if (k == 0x100) { if (sel > 0) sel--; }
                if (k == 0x101) { if (sel + 1 < nr) sel++; }
            }
        } else {
            k = a->getch();
            if (k == 'q' || k == 27) goto done;
            if (k == 'T' || k == 't') theme ^= 1;
            if (k == 'P' || k == 'M') sort_mem ^= 1;
            if (k == 0x100) { if (sel > 0) sel--; }
            if (k == 0x101) { if (sel + 1 < nr) sel++; }
        }
        a->delay(100);
        draw();
    }
done:
    a->cls();
    st(0, -1, 0);
    a->write("ctop: bye\n");
    a->exit(0);
    return 0;
}
