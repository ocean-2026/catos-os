/* cmenu.c - CatOS TUI launcher (the win-demo `catos-menu` port)
 *
 * A menu that lists the TUI lab apps and launches them through the
 * kernel's own spawn() (slot 46): after the child exits we re-enter the
 * menu.  No exec() in CE, so launching = spawn + wait; the menu is
 * re-drawn on the shell's console when we come back.
 *
 * Keys: 1-5 launch  T theme  q/Esc quit
 */
#include "ce_abi.h"
catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

typedef struct { int fg[8]; int bg; } theme_t;
static const theme_t TH[2] = {
    { {7, 8, 12, 14, 10, 11, 12, 9}, 0 },   /* dark */
    { {0, 9, 4, 1, 2, 6, 1, 5}, 15 },       /* light */
};
static int theme = 0;

typedef struct {
    const char *key, *title, *desc, *cmd;
} item_t;

static const item_t ITEMS[6] = {
    { "1", "ctop",      "task manager: CPU / MEM / task table",   "/bin/ctop.ce" },
    { "2", "cfm",       "file explorer: tree / list / preview",   "/bin/cfm.ce" },
    { "3", "charness",  "agent TUI: context bar / timeline",      "/bin/charness.ce" },
    { "4", "ctheme",    "theme lab: token inspector",             "/bin/ctheme.ce" },
    { "5", "cgreet",    "whale + banner greeting page",           "/bin/cgreet.ce" },
    { "6", "btop",      "shell dashboard (kernel built-in)",      0 },
};
#define NITEMS 6

static catos_api_t *A(void) { return __ce_api; }
static void st(int s, int a) { if (s < 0) s = 0; A()->set_style(TH[theme].fg[s < 0 ? 0 : s], TH[theme].bg, a); }
static void W(const char *s) { A()->write(s); }
static void NL(void) { A()->write("\n"); }
static int launch(const item_t *it) {
    catos_api_t *a = A();
    char path[64];
    char *argp[2];
    int tid;

    a->strcpy(path, it->cmd ? it->cmd : "/bin/ctop.ce");
    argp[0] = path;          /* argv[0] = the .ce path (documented shape) */
    argp[1] = 0;
    tid = a->spawn(path, 1, argp);
    if (tid <= 0) return tid;
    a->wait(tid);
    return 0;
}

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *a = A();
    int k, choice;
    (void)argc; (void)argv;

    while (1) {
        a->cls();
        st(3, 1);
        W("CatOS TUI lab  (CE front-end, ring3)\n");
        st(2, 0);
        for (int i = 0; i < 46; i++) A()->putc('=');
        NL();
        st(1, 0);
        W(" theme: ");
        st(3, 0);
        W(theme ? "light" : "dark");
        st(1, 0);
        W("   (T = cycle)      q = quit      1-5 = launch\n\n");

        for (int i = 0; i < NITEMS; i++) {
            st(3, 0);
            W(" [");
            W(ITEMS[i].key);
            W("] ");
            W(ITEMS[i].title);
            st(0, 0);
            W("   ");
            st(1, 0);
            W(ITEMS[i].desc);
            NL();
        }

        NL();
        st(1, 0);
        W(" launching = spawn()+wait() of a sibling CE task; the menu\n");
        W(" reappears here when it exits (CE has no exec, by design).\n");
        NL();
        st(2, 0);
        for (int i = 0; i < 46; i++) A()->putc('=');
        NL();
        st(1, 0);
        W("1-6 launch    T theme    q quit\n");

        choice = -1;
        k = -1;
        if (a->pollkey) {
            int got = 0;
            while ((k = a->pollkey()) >= 0) { got = 1; break; }
            if (!got) { a->delay(80); continue; }
        } else k = a->getch();

        if (k == 'q' || k == 27) break;
        if (k == 'T' || k == 't') { theme ^= 1; continue; }
        if (k >= '1' && k <= '6') choice = k - '1';
        if (k >= 0x31 && k <= 0x36) choice = k - 0x31;
        if (choice < 0 || choice >= NITEMS) continue;

        /* item 6 is the shell built-in btop: not a CE spawn, just tell
         * the user to run it from the shell after exiting this menu */
        if (ITEMS[choice].cmd == 0) {
            a->write("btop is a shell built-in: exit the menu (q) and type 'btop'\n");
            a->getch();
            continue;
        }
        launch(&ITEMS[choice]);
    }
    a->cls();
    a->write("cmenu: bye\n");
    a->exit(0);
    return 0;
}
