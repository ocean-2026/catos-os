/* cfm.c - CatOS file manager TUI (three panes: tree / list / preview)
 *
 * Port of the win-demo `catos-fm` design:
 *   TREE    : current path leaf + hint lines
 *   FILES   : the full listing of the current directory, cursor + scroll
 *   PREVIEW : metadata for the selected entry + up to 8 lines of text
 *
 * Keys: Up/Down move  Enter open/preview  Backspace go up  Tab cycle
 *       focus among panes (visual only)  T theme  q/Esc quit
 *
 * The kernel has no chdir(): the current directory is tracked locally as
 * an absolute path string.  Going up = strip everything after the last
 * '/'.  Listing goes through ls(path, cb, ud); preview through cat().
 */
#include "ce_abi.h"
catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

typedef unsigned int u32;
typedef unsigned char u8;

#define MAXENT   256
#define PREVIEWL 8
#define PATHMAX  64

struct ent {
    u8   is_dir;
    u32  size;
    char name[13];
};

static struct ent ents[MAXENT];
static u32 nent = 0;
static char cur[PATHMAX] = "/";
static u32 sel = 0;
static u32 top = 0;
static int focus = 1;
static int theme = 0;
static char status[48] = "ready";

typedef struct { int fg[6]; int bg; } theme_t;
static const theme_t TH[2] = {
    { {7, 8, 12, 14, 10, 11}, 0 },      /* dark */
    { {0, 9, 4, 1, 2, 6}, 15 },         /* light */
};
#define T_TEXT 0
#define T_MUTE 1
#define T_BORD 2
#define T_ACC  3
#define T_OK   4
#define T_WARN 5

static catos_api_t *A(void) { return __ce_api; }
static void st(int s, int attrs) {
    if (s < 0) s = 0;
    A()->set_style(TH[theme].fg[s], TH[theme].bg, attrs);
}
static void W(const char *s) { A()->write(s); }
static void NL(void) { A()->write("\n"); }

/* tiny size formatter (no sprintf in the CE runtime) */
static int fmt_u32(u32 v, char *b) {
    int n = 0;
    if (v == 0) b[n++] = '0';
    else {
        char t[12];
        u32 i = 0;
        while (v) { t[i++] = '0' + (v % 10); v /= 10; }
        while (i) b[n++] = t[--i];
    }
    b[n] = 0;
    return n;
}

/* right-justify into b (width w) */
static void fmt_pad(u32 v, int w) {
    char b[16];
    int n = fmt_u32(v, b), i;
    for (i = 0; i < w - n; i++) A()->putc(' ');
    for (i = 0; i < n; i++) A()->putc(b[i]);
}

/* ---------------- listing ---------------- */

static void ls_cb(int is_dir, const char *name, u32 size, void *ud) {
    struct ent *e;
    u32 i;
    (void)ud;
    if (nent >= MAXENT) return;
    e = &ents[nent++];
    e->is_dir = is_dir ? 1 : 0;
    e->size = size;
    for (i = 0; i < 12 && name[i]; i++) e->name[i] = name[i];
    e->name[12] = 0;
}

static void refresh(void) {
    int rc;
    nent = 0;
    rc = A()->ls(cur, ls_cb, 0);
    if (rc != 0) {
        W("warning: cannot list ");
        W(cur);
        W(", back to /\n");
        A()->strcpy(cur, "/");
        A()->ls(cur, ls_cb, 0);
    }
    /* insertion sort: directories first, then name-ascending */
    {
        u32 i, j;
        for (j = 1; j < nent; j++) {
            struct ent tmp = ents[j];
            for (i = j; i > 0; i--) {
                if (ents[i - 1].is_dir && !tmp.is_dir) break;
                if (!ents[i - 1].is_dir && tmp.is_dir) break;
                if (!ents[i - 1].is_dir && !tmp.is_dir &&
                    A()->strcmp(ents[i - 1].name, tmp.name) > 0) break;
                ents[i] = ents[i - 1];
            }
            ents[i] = tmp;
        }
    }
    if (sel >= nent) sel = 0;
    if (top > sel) top = sel;
}

static void go_up(void) {
    u32 i, l;
    if (A()->strcmp(cur, "/") == 0) return;
    l = A()->strlen(cur);
    for (i = l - 1; i > 0; i--)
        if (cur[i] == '/') break;
    cur[i] = 0;
    if (cur[0] == 0) A()->strcpy(cur, "/");
    refresh();
    A()->strcpy(status, "up to ");
    {
        u32 p = A()->strlen(status), k;
        for (k = 0; cur[k] && p < 44; k++, p++) status[p] = cur[k];
        status[p] = 0;
    }
}

static void open_entry(void) {
    struct ent *e;
    char np[PATHMAX + 13];
    u32 p, k;
    if (sel >= nent) return;
    e = &ents[sel];
    if (!e->is_dir) {
        focus = 2;
        A()->strcpy(status, "preview: ");
        p = A()->strlen(status);
        for (k = 0; e->name[k] && p < 44; k++, p++) status[p] = e->name[k];
        status[p] = 0;
        return;
    }
    A()->strcpy(np, cur);
    p = A()->strlen(np);
    if (p > 0 && np[p - 1] != '/') np[p++] = '/';
    for (k = 0; e->name[k] && p < sizeof(np) - 1; k++) np[p++] = e->name[k];
    np[p] = 0;
    A()->strcpy(cur, np);
    A()->strcpy(status, "opened ");
    p = A()->strlen(status);
    for (k = 0; e->name[k] && p < 44; k++, p++) status[p] = e->name[k];
    status[p] = 0;
    refresh();
}

/* ---------------- preview ---------------- */

static int is_text(const char *n) {
    u32 l = A()->strlen(n);
    const char *s = l >= 4 ? n + l - 4 : n;
    return A()->strcmp(s, ".txt") == 0 || A()->strcmp(s, ".TXT") == 0 ||
           A()->strcmp(s, ".c")   == 0 || A()->strcmp(s, ".inc") == 0 ||
           A()->strcmp(s, ".md")  == 0 || A()->strcmp(s, ".log") == 0 ||
           A()->strcmp(s, ".cfg") == 0 || A()->strcmp(s, ".lst") == 0;
}

static void draw_tree(int x, int y, int w, int ph) {
    const char *leaf = cur;
    u32 c = A()->strlen(cur);
    int i;
    if (c > 1) {
        for (u32 j = c; j > 0; j--)
            if (cur[j - 1] == '/') { leaf = cur + j; break; }
    }
    if (A()->gotoxy) A()->gotoxy(x + 1, y + 1);
    st(T_OK, 0);
    A()->putc('>');
    A()->putc(' ');
    for (i = 0; leaf[0] && i < w - 4; i++) A()->putc(leaf[i]);
    NL();
    st(T_MUTE, 0);
    if (A()->gotoxy) A()->gotoxy(x + 1, y + 2);
    { u32 l = A()->strlen(cur);
      for (i = 0; i < (int)l && i < w - 4; i++) A()->putc(cur[i]); }
    NL();
    if (A()->gotoxy) A()->gotoxy(x + 1, y + 4);
    W("dirs: ");
    { char b[12]; u32 nd = 0, k, bn;
      for (k = 0; k < nent; k++) if (ents[k].is_dir) nd++;
      bn = (u32)fmt_u32(nd, b);
      for (k = 0; k < bn; k++) A()->putc(b[k]); }
    NL();
    (void)ph;
}

static void draw_files(int x, int y, int w, int ph) {
    u32 shown = (u32)(ph - 1), i;
    if (sel < top) top = sel;
    if (sel >= top + shown) top = sel - shown + 1;
    for (i = top; i < nent && i < top + shown; i++) {
        struct ent *e = &ents[i];
        int hot = (i == sel && focus == 1);
        u32 nl = A()->strlen(e->name);
        u32 maxc = (u32)(w - 10);
        u32 k;
        st(hot ? T_ACC : (e->is_dir ? T_OK : T_TEXT), hot ? 0x01 : 0);
        if (A()->gotoxy) A()->gotoxy(x + 1, y + 1 + (i - top));
        A()->putc(e->is_dir ? '+' : ' ');
        A()->putc(' ');
        for (k = 0; k < nl && k < maxc; k++) A()->putc(e->name[k]);
        if (nl >= maxc && maxc) A()->putc('~');
        if (!e->is_dir) {
            st(T_MUTE, 0);
            fmt_pad(e->size, 8);
        }
        NL();
    }
}

static void draw_preview(int x, int y, int w, int ph) {
    struct ent *e;
    char fpath[PATHMAX + 13];
    u32 maxl = (u32)(ph - 1);
    char pbuf[8192];
    int n;

    if (sel >= nent) {
        st(T_MUTE, 0);
        if (A()->gotoxy) A()->gotoxy(x + 1, y + 1);
        W("(no selection)");
        NL();
        return;
    }
    e = &ents[sel];
    if (e->is_dir) {
        st(T_TEXT, 0);
        if (A()->gotoxy) A()->gotoxy(x + 1, y + 1);
        W("DIR   Enter to open, Backspace to go up");
        NL();
        return;
    }
    /* header: name + size */
    {
        u32 nl = A()->strlen(e->name);
        u32 maxc = (u32)(w - 4 - 8);
        u32 k;
        st(T_ACC, 0x01);
        if (A()->gotoxy) A()->gotoxy(x + 1, y + 1);
        for (k = 0; k < nl && k < maxc; k++) A()->putc(e->name[k]);
        if (nl >= maxc) A()->putc('~');
        st(T_MUTE, 0);
        A()->write("  ");
        fmt_pad(e->size, 8);
        A()->write(" B");
        NL();
    }
    /* body */
    if (!is_text(e->name)) {
        st(T_MUTE, 0);
        if (A()->gotoxy) A()->gotoxy(x + 1, y + 2);
        W("(binary preview not available)");
        NL();
        return;
    }
    {
        u32 p, k;
        A()->strcpy(fpath, cur);
        p = A()->strlen(fpath);
        if (p > 0 && fpath[p - 1] != '/') fpath[p++] = '/';
        for (k = 0; e->name[k] && p < sizeof(fpath) - 1; k++) fpath[p++] = e->name[k];
        fpath[p] = 0;
    }
    n = A()->cat(fpath, pbuf, sizeof(pbuf));
    if (n < 0) {
        st(T_WARN, 0);
        if (A()->gotoxy) A()->gotoxy(x + 1, y + 2);
        W("(unable to read)");
        NL();
        return;
    }
    if (n == 0) {
        st(T_MUTE, 0);
        if (A()->gotoxy) A()->gotoxy(x + 1, y + 2);
        W("(empty)");
        NL();
        return;
    }
    /* up to PREVIEWL printable rows */
    {
        u32 row = 0, pos = 0, maxl2 = PREVIEWL < maxl ? PREVIEWL : maxl;
        u32 colmax = (u32)(w - 2);
        while (row < maxl2 && pos < (u32)n) {
            u32 end = pos, col = 0;
            char linebuf[128];
            st(T_TEXT, 0);
            if (A()->gotoxy) A()->gotoxy(x + 1, y + 2 + row);
            while (pos < (u32)n && col < colmax - 1) {
                char ch = pbuf[pos++];
                if (ch == '\n') { end = pos; break; }
                if (ch == '\r') { pos++; end = pos; break; }
                linebuf[col++] = (ch >= 32 && ch < 127) ? ch : '.';
            }
            {
                u32 c2 = 0;
                while (c2 < col) A()->putc(linebuf[c2++]);
                if (end < (u32)n) A()->putc('~');
            }
            st(T_MUTE, 0);
            fmt_pad(pos + 1, 4);
            NL();
            row++;
            if (end >= (u32)n) break;
            pos = end;
        }
        if (pos < (u32)n && row > 0) {
            char b[16];
            int bn = fmt_u32((u32)n, b);
            st(T_MUTE, 0);
            if (A()->gotoxy) A()->gotoxy(x + 1, y + 2 + row);
            W(" ... ");
            A()->write(" of ");
            for (int bi = 0; bi < bn; bi++) A()->putc(b[bi]);
            A()->write(" B");
            NL();
        }
    }
}

/* ---------------- frame ---------------- */

static void pane_title(int x, int y, int w, const char *t, int hot) {
    int i, tl = (int)A()->strlen(t);
    st(hot ? T_ACC : T_BORD, hot ? 0x01 : 0);
    if (A()->gotoxy) A()->gotoxy(x, y);
    A()->putc('+');
    A()->putc('-');
    W(" ");
    W(t);
    W(" ");
    for (i = 0; i < w - tl - 4; i++) A()->putc('-');
    A()->putc('+');
    NL();
}

static void pane_bottom(int x, int y, int w) {
    int i;
    st(T_BORD, 0);
    if (A()->gotoxy) A()->gotoxy(x, y);
    A()->putc('+');
    for (i = 0; i < w - 2; i++) A()->putc('-');
    A()->putc('+');
    NL();
}

static void draw_frame(int cols, int rows) {
    int tw, fw, pw, py, ph, x;
    char nb[8];

    tw = cols / 4; if (tw < 12) tw = 12;
    fw = cols / 2 - tw - 2; if (fw < 12) fw = 12;
    pw = cols - tw - fw - 5; if (pw < 12) pw = 12;
    py = 3;
    ph = rows - 6; if (ph < 4) ph = 4;

    /* header */
    st(T_ACC, 0x01);
    if (A()->gotoxy) A()->gotoxy(1, 0);
    W("cfm ");
    {
        u32 l = A()->strlen(cur), k;
        st(T_TEXT, 0);
        for (k = 0; k < l && k < (u32)(cols - 20); k++) A()->putc(cur[k]);
        if (l >= (u32)(cols - 20)) A()->putc('~');
    }
    st(T_MUTE, 0);
    W("   theme: ");
    W(theme ? "light" : "dark");
    NL();
    st(T_BORD, 0);
    if (A()->gotoxy) A()->gotoxy(1, 1);
    for (int i = 0; i < cols - 2; i++) A()->putc('-');
    NL();

    /* panes */
    x = 1;
    pane_title(x, py, tw, "TREE", focus == 0);
    pane_title(x + tw + 1, py, fw, "FILES", focus == 1);
    pane_title(x + tw + fw + 2, py, pw, "PREVIEW", focus == 2);

    /* vertical borders (only where gotoxy is available; otherwise the
     * titles/bottoms already delimit the panes visually) */
    if (A()->gotoxy) {
        int i;
        st(T_BORD, 0);
        for (i = 0; i < ph; i++) {
            A()->gotoxy(x - 1, py + 1 + i);
            A()->putc('|');
            A()->gotoxy(x + tw, py + 1 + i);
            A()->putc('|');
            A()->gotoxy(x + tw + fw + 1, py + 1 + i);
            A()->putc('|');
            A()->gotoxy(x + tw + fw + pw + 1, py + 1 + i);
            A()->putc('|');
        }
        /* interior content */
        draw_tree(x, py, tw, ph);
        draw_files(x + tw + 1, py, fw, ph);
        draw_preview(x + tw + fw + 2, py, pw, ph);
        pane_bottom(x, py + ph + 1, tw + 1);
        pane_bottom(x + tw + 1, py + ph + 1, fw + 1);
        pane_bottom(x + tw + fw + 2, py + ph + 1, pw + 1);
    } else {
        /* fallback: no positioning, plain stacked sections */
        draw_tree(x, py, tw, ph);
        W("\n");
        draw_files(0, 0, fw, ph);
        W("\n");
        draw_preview(0, 0, pw, ph);
    }

    /* status + footer */
    st(T_MUTE, 0);
    if (A()->gotoxy) A()->gotoxy(1, rows - 2);
    {
        u32 l = A()->strlen(status), k;
        for (k = 0; k < l; k++) A()->putc(status[k]);
    }
    NL();
    st(T_BORD, 0);
    if (A()->gotoxy) A()->gotoxy(1, rows - 1);
    W(" up/down move  Enter open  Backspace up  Tab focus  T theme  q quit  [");
    {
        u32 pn = (u32)fmt_u32(nent, nb);
        st(T_ACC, 0);
        for (; pn; pn--) A()->putc(nb[pn - 1]);
        st(T_BORD, 0);
    }
    W(" items]");
}

/* ---------------- main ---------------- */

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *a = A();
    int cols, rows, k;

    (void)argc; (void)argv;
    cols = 80;
    rows = 25;
    if (a->display_info) {
        cols = a->display_info(1);
        rows = a->display_info(2);
        if (cols < 8) cols = 80;
        if (rows < 4) rows = 25;
    }
    if (cols < 60 || rows < 16) {
        a->write("cfm: needs at least 60x16; switch to text mode and retry\n");
        return 0;
    }

    refresh();

    while (1) {
        a->cls();
        draw_frame(cols, rows);

        k = -1;
        if (a->pollkey) {
            int got = 0;
            while ((k = a->pollkey()) >= 0) { got = 1; break; }
            if (!got) { a->delay(80); continue; }
        } else {
            k = a->getch();
        }

        if (k == 'q' || k == 27) break;
        if (k == 'T' || k == 't') theme ^= 1;
        if (k == 0x09) focus = (focus + 1) % 3;
        if (k == 0x100) { if (sel > 0) sel--; }
        if (k == 0x101) { if (sel + 1 < nent) sel++; }
        if (k == 0x0A) open_entry();
        if (k == 'u') go_up();               /* letter u as backspace twin */
        if (k == 0x08) go_up();              /* backspace (0x08 via getch) */
    }
    a->cls();
    a->write("cfm: bye\n");
    a->exit(0);
    return 0;
}
