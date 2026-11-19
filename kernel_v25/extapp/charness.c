/* charness.c - CatOS agent-session TUI (scripted harness, streaming)
 *
 * Port of the win-demo `catos-harness` design: a context bar, a timeline
 * of user/thinking/tool/diff/answer cards, and a toggleable side panel.
 * The session is scripted (no live LLM on the guest yet); Enter plays
 * the next step with char-by-char streaming of the answer lines, which
 * is the same interaction the Windows demo has.
 *
 * Keys: Enter next step   Space side panel   T theme   q/Esc quit
 */
#include "ce_abi.h"
catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

typedef unsigned int u32;

typedef struct { int fg[8]; int bg; } theme_t;
static const theme_t TH[2] = {
    { {7, 8, 12, 14, 10, 11, 12, 9}, 0 },   /* dark */
    { {0, 9, 4, 1, 2, 6, 1, 5}, 15 },       /* light */
};
#define T_TEXT 0
#define T_MUTE 1
#define T_BORD 2
#define T_ACC  3
#define T_OK   4
#define T_WARN 5
#define T_BAD  6
#define T_INFO 7

static int theme = 0;
static int side = 0;
static const char *status = "working";

typedef struct {
    int kind;           /* 0 user, 1 think, 2 tool, 3 diff, 4 answer */
    const char *text;
    const char *tool, *state;
} step_t;

static const step_t STEPS[8] = {
    { 0, "> user   review the task.c syscall ABI, propose a safer layout", 0, 0 },
    { 1, " asst   thinking... (3.2s)", 0, 0 },
    { 2, 0, "exec", "make -j4 [OK] 0.42s" },
    { 2, 0, "read", "/kernel_v25/src/task.c [OK] 12ms" },
    { 3, "  +  SYS_TASK_GET = 69,  /* append-only, v2.48 */", 0, 0 },
    { 3, "  -  /* no task introspection */", 0, 0 },
    { 4, " asst   slots 66-69 appended at the tail; old", 0, 0 },
    { 4, "        thunk addresses stay unchanged.", 0, 0 },
};
#define NSTEPS ((int)(sizeof(STEPS) / sizeof(STEPS[0])))

static int shown = 2;
static catos_api_t *A(void) { return __ce_api; }
static void st(int s, int a) { if (s < 0) s = 0; A()->set_style(TH[theme].fg[s], TH[theme].bg, a); }
static void W(const char *s) { A()->write(s); }
static void NL(void) { A()->write("\n"); }

static void ctxbar(int pct) {
    int i, on = (pct * 20) / 100;
    st(T_ACC, 1);
    W("catos-harness ");
    st(T_OK, 0);
    W("[");
    W(status);
    st(T_MUTE, 0);
    W("] ");
    st(T_INFO, 0);
    W("sess:catos-07   catos-flash   42 tok/s");
    st(T_ACC, 0);
    W(" ctx ");
    for (i = 0; i < 20; i++) A()->putc(i < on ? '#' : '.');
    W(" ");
    st(T_MUTE, 0);
    {
        int pct2 = pct;
        char b[8]; int n = 0;
        if (pct2 == 0) b[n++] = '0';
        else { char t[8]; int i2 = 0;
               while (pct2) { t[i2++] = '0' + (pct2 % 10); pct2 /= 10; }
               while (i2) b[n++] = t[--i2]; }
        b[n] = 0;
        for (int p = 0; p < n; p++) A()->putc(b[p]);
    }
    st(T_MUTE, 0);
    W("% sys12 prompt9 asst14 think7 tool5");
    st(T_OK, 0);
    W("  [OK] healthy");
    NL();
}

static void card(const step_t *s) {
    switch (s->kind) {
    case 0: st(T_ACC, 1); W(s->text); break;
    case 1: st(T_INFO, 0); W(s->text); break;
    case 2:
        st(T_TEXT, 0);
        W("  ");
        {
            const char *t = s->tool;
            while (*t) A()->putc(*t++);
            for (int i = 0; i < 5; i++) A()->putc(' ');
        }
        st(T_OK, 0);
        { const char *b = s->state; while (*b) A()->putc(*b++); }
        break;
    case 3:
        st(s->text[2] == '+' ? T_OK : T_BAD, 0);
        W(s->text);
        break;
    default:
        st(T_TEXT, 0);
        W(s->text);
        break;
    }
    st(T_TEXT, 0);
    NL();
}

static void side_panel(void) {
    catos_api_t *a = A();
    st(T_BORD, 0);
    a->write(" | side panel\n");
    st(T_OK, 0);
    a->write(" |  [x] freeze slot 64\n");
    a->write(" |  [x] append slots 66-69\n");
    st(T_TEXT, 0);
    a->write(" |  [ ] abi regression probe\n");
    a->write(" |  [ ] redteam review\n");
    st(T_MUTE, 0);
    a->write(" |  sessions\n");
    st(T_ACC, 0);
    a->write(" |  > catos-07  42 tok/s live\n");
    st(T_MUTE, 0);
    a->write(" |    catos-06  idle 1h\n");
}

static void stream_answer(const step_t *s) {
    catos_api_t *a = A();
    u32 len = a->strlen(s->text);
    char part[64];
    if (len > sizeof(part) - 1) len = sizeof(part) - 1;
    for (u32 pos = 0; pos <= len; pos += 2) {
        u32 n = pos < len ? pos : len;
        for (u32 i = 0; i < n; i++) part[i] = s->text[i];
        part[n] = 0;
        a->cls();
        ctxbar(20 + shown * 5);
        NL();
        st(T_BORD, 0);
        W("+-- transcript");
        for (int i = 0; i < 44; i++) A()->putc('-');
        NL();
        for (int i = 0; i < shown && i < NSTEPS; i++) {
            const step_t *st2 = &STEPS[i];
            if (i == shown - 1 && st2->kind == 4) {
                /* stream only the LAST line of the previous step group */
            }
            if (st2->kind == 4 && i > 0 && STEPS[i - 1].kind == 4) continue;
            card(st2);
            if (i == shown - 1 && st2->kind == 4) break; /* streamed below */
        }
        st(T_INFO, 0);
        W(" asst   ");
        st(T_TEXT, 0);
        W(part);
        NL();
        a->delay(30);
    }
}

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *a = A();
    int k;
    (void)argc; (void)argv;

    while (1) {
        int cols = 80, rows = 25;
        if (a->display_info) {
            cols = a->display_info(1);
            rows = a->display_info(2);
        }
        a->cls();
        ctxbar(20 + shown * 5);
        NL();
        st(T_BORD, 0);
        W("+-- transcript");
        for (int i = 0; i < 38; i++) A()->putc('-');
        if (side && cols >= 60) W("--");
        NL();
        st(T_TEXT, 0);
        for (int i = 0; i < shown && i < NSTEPS; i++) {
            card(&STEPS[i]);
            if ((STEPS[i].kind == 2 || STEPS[i].kind == 0)) NL();
            if (side && cols >= 60) side_panel();
        }
        st(T_BORD, 0);
        W("+-- input");
        for (int i = 0; i < 50; i++) A()->putc('-');
        NL();
        st(T_TEXT, 0);
        W("> propose a safer ABI for task.c and add a regression probe");
        st(T_MUTE, 0);
        W("  effort:high  catos-flash ");
        NL();
        st(T_MUTE, 0);
        W(" Enter next step   Space side panel   T theme   q quit");
        NL();

        k = -1;
        if (a->pollkey) {
            int got = 0;
            while ((k = a->pollkey()) >= 0) { got = 1; break; }
            if (!got) { a->delay(80); continue; }
        } else k = a->getch();

        if (k == 'q' || k == 27) break;
        if (k == 'T' || k == 't') theme ^= 1;
        if (k == 0x20) side ^= 1;
        if (k == 0x0A) {
            if (shown < NSTEPS) {
                const step_t *next = &STEPS[shown];
                if (next->kind == 4) stream_answer(next);
                status = "working";
                shown++;
            } else {
                status = "done";
            }
        }
    }
    a->cls();
    a->write("charness: bye\n");
    a->exit(0);
    return 0;
}
