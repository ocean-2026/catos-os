/* cgreet.c - CatOS greeting TUI (whale + block banner + scripted session)
 *
 * Port of the win-demo `catos-greeting` design (dsh-TUI pixel whale + BOLD
 * block banner + streaming Chinese-friendly transcript).  The Windows demo
 * packs 2 pixel rows per terminal cell with half-block SGR; the CatOS
 * front-end is a styled TEXT grid (PLAN I5: graphics = the shell's
 * backend, no GUI concepts), so the whale is a compact ASCII sprite and
 * the banner is a hand-crafted 5-row block font drawn with '#'.  The
 * semantic-token idea carries over: every line gets one of the token
 * classes, and T cycles the theme.
 *
 * Keys: Enter play next reply (streams char by char)  T theme  q/Esc quit
 * Needs roughly 80x25 (text mode); in the narrow gfx grid it still runs
 * but the transcript box shrinks.
 */
#include "ce_abi.h"
catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

typedef unsigned int u32;

/* token classes (same family as ctop/cfm) */
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

/* the 20x12 whale (brand asset: fixed, does NOT follow the theme) */
static const char whale[13][21] = {
    "                    ",
    "      .-~~~~~-.     ",
    "    .~'       '~.   ",
    "   (  o    .    )   ",
    "  (        (   )~    ",
    "   '-.____.'~~      ",
    "    ~~~~~~'    .    ",
    "   '~~~'      (~)~  ",
    "  ~~~~      ~~~~    ",
    "                    ",
    "                    ",
    "                    ",
    "                    ",
};

/* 5-row block banner font, ASCII only */
typedef struct { char r[5][6]; } glyph5_t;
static const glyph5_t GL[26] = {
    { {".###.", "#...#", "#...#", "#...#", ".###."} },      /* A */
    { {"####", "#..#", "#..#", "#..#", "####"} },         /* B */
    { {"####", "#...#", "#...#", "#...#", "####"} },      /* C */
    { {"####", "#..#", "#..#", "#..#", "####"} },         /* D */
    { {"####", "#...", "#...#", "#...", "####"} },        /* E */
    { {"####", "#...", "#...#", "#...", "####"} },        /* F */
    { {"####", "#...#", "#..##", "#...#", "####"} },      /* G */
    { {"#..#", "#..#", "####", "#..#", "#..#"} },        /* H */
    { {"###", "..#", "..#", "..#", "###"} },              /* I */
    { {"###..", "...##", "...##", "...##", "###.."} },    /* J */
    { {"#..#", "#.#.", "###.", "#.#.", "#..#"} },        /* K */
    { {"#...", "#...#", "#...#", "#...#", "####"} },      /* L */
    { {"#..#.#", "##.##", "####", "#..#.#", "#..#.#"} },  /* M */
    { {"#...#", "##..#", ".#.#.", "..##.", "...#."} },     /* N */
    { {"###.", "#..#", "#..#", "#..#", "###."} },         /* O */
    { {"####", "#..#", "####", "#...", "#..."} },         /* P */
    { {"###.", "#..#", "#..#", "#..##", "###"} },         /* Q */
    { {"####", "#..#", "#..#", "#..#", "####"} },         /* R */
    { {"####", "#...", ".#..#", "...#", "####"} },        /* S */
    { {"####", "..#.", "..#.", "..#.", "..#."} },         /* T */
    { {"#..#", "#..#", "#..#", "#..#", ".##."} },        /* U */
    { {"#..#", "#..#", "#..#", ".#.#.", "..#.."} },       /* V */
    { {"#..#.#", "##.##.", ".##.#.", "#..#.#", "#..#.#"} }, /* W */
    { {"#..#", "#..#", ".##.", "#..#", "#..#"} },        /* X */
    { {"#..#", "#..#", ".##.", "..#.", "..#."} },        /* Y */
    { {"####", "...#", "..#.", ".#..", "####"} },        /* Z */
};

static void draw_banner(const char *text, int x, int y, int accent) {
    catos_api_t *a = A();
    const char *s = text;
    (void)x; (void)y;            /* banners are drawn inline on their own rows */
    int r, c;
    for (r = 0; r < 5; r++) {
        while (*s) {
            const glyph5_t *g = 0;
            if (*s >= 'A' && *s <= 'Z') g = &GL[*s - 'A'];
            else if (*s >= 'a' && *s <= 'z') g = &GL[*s - 'a'];
            if (*s == ' ') { a->putc(' '); a->putc(' '); a->putc(' '); a->putc(' '); a->putc(' '); }
            else if (g) {
                for (c = 0; c < 6; c++) {
                    char ch = g->r[r][c];
                    /* accent color on every third row gives the demo's
                     * "fading" feel with plain ANSI intensity */
                    a->set_style(accent ? TH[theme].fg[T_ACC] : TH[theme].fg[T_TEXT],
                                 TH[theme].bg, 0);
                    a->putc(ch == '#' ? '#' : '.');
                }
            }
            s++;
        }
        a->write("\n");
    }
}

typedef struct { const char *text; int tok; } line_t;

static const char REPLIES[3][96] = {
    "located task.c; ABI slots 66-68 appended at the table tail,",
    "context usage 47%: sys 12% / prompt 9% / asst 14% / think 7% / tools 5%",
    "next: check the dlclose path, or build a regression probe.",
};
static int shown = 0;

static void st(int s, int a) { if (s < 0) s = 0; A()->set_style(TH[theme].fg[s], TH[theme].bg, a); }

static int cols_g, rows_g;

static void header(void) {
    catos_api_t *a = A();
    a->cls();
    st(T_ACC, 1);
    a->write("catos-greet v2.48");
    st(T_MUTE, 0);
    a->write("  up ");
    { char tb[24]; a->time_str(tb); a->write(tb); }
    a->write("\n");
    st(T_BORD, 0);
    for (int i = 0; i < cols_g - 2; i++) a->putc('-');
    a->write("\n");
}

static void transcript_box(const char *stream_line, int streamed) {
    catos_api_t *a = A();
    st(T_BORD, 0);
    a->write("+-- session -----------------------------------------------------------+\n");
    st(T_ACC, 0);
    a->write("> hi~\n");
    st(T_TEXT, 0);
    a->write("  asst   hi - your CatOS assistant, ready.\n");
    for (int i = 0; i < shown && i < 3; i++) {
        st(T_INFO, 0);
        a->write("  asst   ");
        st(T_TEXT, 0);
        a->write(REPLIES[i]);
        a->write("\n");
    }
    if (streamed) {
        st(T_ACC, 0);
        a->write("  asst   ");
        st(T_INFO, 0);
        a->write(stream_line);
        a->write("\n");
    }
    st(T_BORD, 0);
    a->write("+---------------------------------------------------------------------+\n");
}

static void footer(void) {
    catos_api_t *a = A();
    st(T_MUTE, 0);
    a->write(" Enter stream next reply    T theme    q quit\n");
}

static void stream_reply(void) {
    catos_api_t *a = A();
    const char *full = REPLIES[shown];
    u32 len = a->strlen(full);
    char part[96];
    if (len > sizeof(part) - 1) len = sizeof(part) - 1;
    for (u32 pos = 0; pos <= len; pos += 4) {
        u32 n = pos < len ? pos : len;
        for (u32 i = 0; i < n; i++) part[i] = full[i];
        part[n] = 0;
        a->cls();
        header();
        a->write("\n");
        draw_banner("CATOS", 0, 0, 1);
        draw_banner("TUI", 0, 0, 0);
        a->write("\n");
        st(T_INFO, 0);
        a->write("deepseek-flash style:  max effort\n");
        st(T_MUTE, 0);
        a->write("working dir  /bin\n");
        a->write("\n");
        transcript_box(part, 1);
        footer();
        a->delay(24);
    }
    shown++;
}

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *a = A();
    int k;
    (void)argc; (void)argv;

    cols_g = 80;
    rows_g = 25;
    if (a->display_info) {
        cols_g = a->display_info(1);
        rows_g = a->display_info(2);
        if (cols_g < 8) cols_g = 80;
        if (rows_g < 4) rows_g = 25;
    }
    if (cols_g < 50 || rows_g < 16) {
        a->write("cgreet: needs at least 50x16; switch to text mode and retry\n");
        a->getch();
        return 0;
    }

    while (1) {
        a->cls();
        header();
        a->write("\n");
        draw_banner("CATOS", 0, 0, 1);
        draw_banner("TUI", 0, 0, 0);
        a->write("\n");
        /* the whale sits on the left; draw it as text rows (12 rows) */
        st(T_INFO, 0);
        for (int r = 0; r < 12; r++) {
            for (int c = 0; c < 20; c++) {
                char ch = whale[r][c];
                if (ch == 'o') a->set_style(TH[theme].fg[T_WARN], TH[theme].bg, 0);
                else if (ch == '~') a->set_style(TH[theme].fg[T_OK], TH[theme].bg, 0);
                else if (ch == '(' || ch == ')' || ch == '.')
                    a->set_style(TH[theme].fg[T_MUTE], TH[theme].bg, 0);
                else a->set_style(TH[theme].fg[T_ACC], TH[theme].bg, 0);
                a->putc(ch);
            }
            a->write("\n");
        }
        st(T_MUTE, 0);
        a->write("deepseek-flash style: max effort\n");
        a->write("working dir /bin\n");
        a->write("\n");
        transcript_box(0, 0);
        footer();

        k = -1;
        if (a->pollkey) {
            int got = 0;
            while ((k = a->pollkey()) >= 0) { got = 1; break; }
            if (!got) { a->delay(80); continue; }
        } else k = a->getch();

        if (k == 'q' || k == 27) break;
        if (k == 'T' || k == 't') theme ^= 1;
        if (k == 0x0A && shown < 3) stream_reply();
    }
    a->cls();
    a->write("cgreet: bye\n");
    a->exit(0);
    return 0;
}
