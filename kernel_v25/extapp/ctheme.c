/* ctheme.c - CatOS theme lab (semantic token inspector)
 *
 * Port of the win-demo `catos-theme` design: the SAME layout under every
 * theme; T cycles dark <-> light (the CE palette is a 16-colour VGA
 * index, so "auto/ansi" has no meaning on the guest - the lab shows the
 * two resolvable modes and how every token maps onto it).
 */
#include "ce_abi.h"
catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

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
static catos_api_t *A(void) { return __ce_api; }
static void st(int s, int a) { if (s < 0) s = 0; A()->set_style(TH[theme].fg[s], TH[theme].bg, a); }
static void W(const char *s) { A()->write(s); }
static void NL(void) { A()->write("\n"); }

static const char *TOK[8] = {
    "text", "muted", "border", "accent", "ok", "warn", "bad", "info"
};

static void swatch(int slot, int width) {
    int i;
    st(slot, 0);
    for (i = 0; i < width; i++) A()->putc('#');
    st(T_TEXT, 0);
}

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *a = A();
    int k;
    (void)argc; (void)argv;

    while (1) {
        a->cls();
        st(T_ACC, 1);
        W("CatOS theme lab  resolved=");
        W(theme ? "light" : "dark");
        W("  (CE palette = 16-colour VGA index)");
        st(T_TEXT, 0);
        NL();
        st(T_BORD, 0);
        for (int i = 0; i < 74; i++) A()->putc('-');
        NL();
        st(T_MUTE, 0);
        W("TOKEN     SWATCH                        VALUE\n");
        st(T_BORD, 0);
        for (int i = 0; i < 50; i++) A()->putc('-');
        NL();
        for (int t = 0; t < 8; t++) {
            st(T_TEXT, 0);
            W(TOK[t]);
            for (int i = 0; i < 6; i++) A()->putc(' ');
            swatch(t, 28);
            st(T_MUTE, 0);
            W("  fg=");
            a->print_int(TH[theme].fg[t]);
            W(" bg=");
            a->print_int(TH[theme].bg);
            NL();
        }
        st(T_MUTE, 0);
        W(" layout is invariant; only the token mapping changes\n");
        W(" T cycle theme    q quit\n");

        k = -1;
        if (a->pollkey) {
            int got = 0;
            while ((k = a->pollkey()) >= 0) { got = 1; break; }
            if (!got) { a->delay(100); continue; }
        } else k = a->getch();
        if (k == 'q' || k == 27) break;
        if (k == 'T' || k == 't') theme ^= 1;
    }
    a->cls();
    a->write("ctheme: bye\n");
    a->exit(0);
    return 0;
}
