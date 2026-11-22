/*
 * ime.c -- pinyin IME, stage 1 (v2.58).  Chinese input on a US-QWERTY
 * keyboard over the existing offline pinyin table (pinyin.inc).
 *
 * Stage-1 layout (docs/ime-plan.md 3.1, fixed):
 *   - the composition (pinyin) string is shown INLINE at the input cursor;
 *   - the fixed bottom two rows hold a separator line and the candidate
 *     list `1.好 2.号 3.浩 ...`, saved/restored verbatim by render.c;
 *   - F2 toggles; letters build the syllable; 1-9 / Space select;
 *     - = [ ] and Left/Right page; Esc cancels; Backspace edits;
 *     Enter commits the raw pinyin string.
 *
 * There is NO phrase/word mode yet -- per-character IME over 400
 * syllables and the GB2312 subset font, as the stage-1 scope states.
 */
#include "ime.h"
#include "keyboard.h"
#include "render.h"
#include "pinyin.inc"

#define IME_COMPOSE_MAX 16
#define BAR_MAX  240

static int  active;
static char py[IME_COMPOSE_MAX];
static int  py_len;
static int  page;
static int  sel;                 /* v2.68: arrow-selected candidate */

static int py_cmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static int py_len_(const char *s) { int n = 0; while (s[n]) n++; return n; }

static const py_entry_t *py_find(const char *s) {
    int lo = 0, hi = PY_TAB_COUNT;
    while (lo < hi) {
        int m = lo + (hi - lo) / 2;
        int c = py_cmp(py_tab[m].py, s);
        if (c == 0) return &py_tab[m];
        if (c < 0) lo = m + 1; else hi = m;
    }
    return 0;
}

static void bar_put(char *b, int *n, const char *s) {
    while (*s && *n < BAR_MAX - 1) b[(*n)++] = *s++;
}
static void bar_num(char *b, int *n, int v) {
    char t[8]; int i = 0;
    if (v == 0) t[i++] = '0';
    while (v > 0) { t[i++] = (char)('0' + v % 10); v /= 10; }
    while (i > 0) b[(*n)++] = t[--i];
}

void ime_set_anchor(int row) { (void)row; }   /* stage-1: fixed bottom rows */

const char *ime_composition(void) {
    return (active && py_len > 0) ? py : "";
}

/* Adaptive per-page capacity: how many candidates fit `cols` columns.
 * One candidate = digit(s) + '.' + hanzi(2 cells) + optional separating
 * space.  The trailing space is dropped first on a narrow grid; a hanzi
 * is never cut in half.  (Plan 3.1.4, all-integer width budget.) */
static int page_capacity(int cols, int total) {
    int used = 0, n = 0;
    for (int i = 0; i < total && i < 64; i++) {
        int d = 1 + (i >= 9) + (i >= 99);
        int w = d + 1 + 2;
        if (used + w + 1 <= cols)      { used += w + 1; n++; }
        else if (used + w <= cols)     { used += w;     n++; }
        else break;
    }
    return n < 1 ? 1 : n;
}

static void ime_clear(void) { py_len = 0; py[0] = 0; page = 0; sel = 0; }

static void ime_show(void) {
    char cand[BAR_MAX];
    int n = 0;
    if (!active) { con_status_bar_forget(); return; }
    int cols = con_cols();

    if (py_len == 0) {
        bar_put(cand, &n,
            "[IME] on: 1-9 pick, Up/Down select, Space confirm");
        cand[n] = 0;
        con_status_bar2("", cand);
        return;
    }
    const py_entry_t *e = py_find(py);
    if (!e) {
        bar_put(cand, &n, "[IME] ");
        bar_put(cand, &n, py);
        bar_put(cand, &n, ": no match");
        cand[n] = 0;
        con_status_bar2("", cand);
        return;
    }

    int total = py_len_(e->hz) / 3;
    /* reserve the "p/t " prefix on multi-page grids before counting the
     * candidates that still fit (all-integer width budget, plan 3.1.4). */
    int prefix_w = 0;
    int guess_per = page_capacity(cols, total);
    int pages = (total + guess_per - 1) / guess_per;
    if (pages > 1) {
        int d1 = 1, d2 = 1, v;
        for (v = page + 1; v >= 10; v /= 10) d1++;
        for (v = pages;    v >= 10; v /= 10) d2++;
        prefix_w = d1 + 1 + d2 + 1;
    }
    int per = page_capacity(cols - prefix_w, total);
    pages = (total + per - 1) / per;
    if (page >= pages) page = 0;
    if (page < 0) page = 0;
    int start = page * per;
    if (sel >= per) sel = per - 1;
    if (sel < 0) sel = 0;

    int used = prefix_w;
    if (pages > 1) {
        bar_num(cand, &n, page + 1);
        bar_put(cand, &n, "/");
        bar_num(cand, &n, pages);
        bar_put(cand, &n, " ");
    }
    for (int i = 0; i < per && start + i < total; i++) {
        int d = 1 + (i >= 9) + (i >= 99);
        int w = d + 1 + 2 + (i == sel ? 1 : 0);
        int with_space = used + w + 1 <= cols;
        int no_space   = used + w <= cols;
        if (!with_space && !no_space) break;
        bar_num(cand, &n, i + 1);
        bar_put(cand, &n, ".");
        for (int k = 0; k < 3 && n < BAR_MAX - 1; k++)
            cand[n++] = e->hz[(start + i) * 3 + k];
        if (i == sel && n < BAR_MAX - 1) cand[n++] = '*';
        used += with_space ? w + 1 : w;
        if (with_space) bar_put(cand, &n, " ");
    }
    cand[n] = 0;
    con_status_bar2("", cand);
}

static int ime_commit(int idx, char out[IME_OUT_MAX]) {
    const py_entry_t *e = py_find(py);
    if (!e || py_len == 0) return IME_PASS;
    int total = py_len_(e->hz) / 3;
    int per = page_capacity(con_cols(), total);
    int k = page * per + idx;
    if (k >= total) return IME_CONSUME;      /* empty slot: just consume */
    out[0] = e->hz[k * 3];
    out[1] = e->hz[k * 3 + 1];
    out[2] = e->hz[k * 3 + 2];
    out[3] = 0;
    ime_clear();
    ime_show();
    return IME_COMMIT;
}

/* Enter with an active composition: commit the raw pinyin letters as-is. */
static int ime_commit_raw(char out[IME_OUT_MAX]) {
    int j = 0;
    while (py[j] && j < IME_OUT_MAX - 1) { out[j] = py[j]; j++; }
    out[j] = 0;
    ime_clear();
    ime_show();
    return IME_COMMIT;
}

/* v2.68: commit the arrow-highlighted candidate on the current page. */
static int ime_commit_selected(char out[IME_OUT_MAX]) {
    int per = page_capacity(con_cols(), 64);
    if (sel < 0) sel = 0;
    if (sel >= per) sel = per - 1;
    return ime_commit(sel, out);
}

/* ASCII punctuation -> GB2312 full-width form while the IME is active. */
static int ime_punct(int key, char out[IME_OUT_MAX]) {
    /* v2.76: emit UTF-8, not raw GB2312 bytes.  The IME used to hand the
     * line editor two raw GB2312 bytes (e.g. 0xA1 0xA3 for '。'); the console
     * takes UTF-8 codepoints, so each byte fell into the "no glyph" branch and
     * painted '?'.  Result: a full-width Chinese period showed up as TWO
     * question marks in two cells.  The table below is generated from the
     * same GB2312 pairs, converted to their UTF-8 forms at build-edit time. */
    static const struct { int key; const char *u; } P[] = {
        {',', "\xEF\xBC\x8C"},
        {'.', "\xE3\x80\x82"},
        {'!', "\xEF\xBC\x81"},
        {'?', "\xEF\xBC\x9F"},
        {':', "\xEF\xBC\x9A"},
        {';', "\xEF\xBC\x9B"},
        {'`', "\xE3\x80\x81"},
        {'"', "\xE2\x80\x9C"},
        {'(', "\xEF\xBC\x88"},
        {')', "\xEF\xBC\x89"},
        {'<', "\xE3\x80\x8A"},
        {'>', "\xE3\x80\x8B"},
        {'[', "\xE3\x80\x90"},
        {']', "\xE3\x80\x91"},
        {'{', "\xE2\x80\x9C"},
        {'}', "\xE2\x80\x9D"},
        {'/', "\xEF\xBC\x8F"},
        {'|', "\xEF\xBD\x9E"},
        {'@', "\xEF\xBC\xA0"},
        {'#', "\xEF\xBC\x83"},
        {'$', "\xEF\xBF\xA5"},
        {'%', "\xEF\xBC\x85"},
        {'^', "\xE2\x80\xA6"},
        {'&', "\xEF\xBC\x86"},
        {'*', "\xEF\xBC\x8A"},
        {'-', "\xEF\xBC\x8D"},
        {'_', "\xEF\xBD\x9E"},
        {'+', "\xEF\xBC\x8B"},
        {'=', "\xEF\xBC\x9D"},
        {'~', "\xEF\xBD\x9E"},
    };
    for (unsigned i = 0; i < sizeof(P)/sizeof(P[0]); i++)
        if (P[i].key == key) {
            {
                const char *u = P[i].u;
                int n = 0;
                while (u[n] && n < IME_OUT_MAX - 1) { out[n] = u[n]; n++; }
                out[n] = 0;
            }
            ime_clear(); ime_show();
            return IME_COMMIT;
        }
    return IME_PASS;
}

int ime_active(void) { return active; }

int ime_feed(int key, char out[IME_OUT_MAX]) {
    if (key == KB_F2 || key == KB_IME_TOGGLE) {
        active = !active;
        if (active) { ime_clear(); ime_show(); }
        else        { ime_clear(); con_status_bar_off(); }
        return IME_CONSUME;
    }
    if (!active) return IME_PASS;

    if (key >= 'A' && key <= 'Z') key += 32;
    if (key >= 'a' && key <= 'z') {
        if (py_len < IME_COMPOSE_MAX - 1) {
            py[py_len++] = (char)key;
            py[py_len] = 0;
            page = 0;
        }
        ime_show();
        return IME_CONSUME;
    }
    if (key == '\b') {
        /* v2.62: only swallow Backspace while there is a pinyin draft to
         * edit.  With an empty composition (candidate page up, or raw text
         * already committed) pass it through so the line editor erases a
         * character instead of the key doing nothing. */
        if (py_len > 0) {
            py_len--; py[py_len] = 0; page = 0; ime_show();
            return IME_CONSUME;
        }
        return IME_PASS;
    }
    /* Esc: cancel the composition AND close the IME, restoring the two
     * bottom rows verbatim (no residue, plan 3.1.2). */
    if (key == 27) {
        active = 0;
        ime_clear();
        con_status_bar_off();
        return IME_CONSUME;
    }
    if ((key == KB_UP || key == KB_DOWN) && py_len > 0) {
        int per = page_capacity(con_cols(), 64);
        if (per < 1) per = 1;
        if (key == KB_UP)   sel = (sel > 0) ? sel - 1 : per - 1;
        else                sel = (sel + 1 < per) ? sel + 1 : 0;
        ime_show();
        return IME_CONSUME;
    }
    if (key == KB_LEFT || key == KB_RIGHT) {
        if (py_len == 0) return IME_PASS;     /* empty: let cursor move */
        if (key == KB_LEFT) { if (page > 0) page--; } else page++;
        sel = 0;
        ime_show();
        return IME_CONSUME;
    }
    /* digits pick the Nth candidate of the current page (1..9, 0 = 10th) */
    if (key >= '1' && key <= '9') {
        int idx = key - '1';
        int per = page_capacity(con_cols(), 64);
        if (idx >= per) return IME_CONSUME;
        sel = idx;
        return ime_commit(idx, out);
    }
    if (key == '0') {
        int per = page_capacity(con_cols(), 64);
        if (per <= 9) return IME_CONSUME;
        sel = 9;
        return ime_commit(9, out);
    }
    if (key == ' ' && py_len > 0) return ime_commit_selected(out);
    /* all ASCII punctuation becomes full-width Chinese punctuation here */
    {
        int pr = ime_punct(key, out);
        if (pr == IME_COMMIT) return pr;
    }
    if (key == '\n') {
        if (py_len > 0) return ime_commit_raw(out);
        return IME_PASS;
    }
    return IME_PASS;
}

void ime_refresh(void) {
    if (active) ime_show();
    else        con_status_bar_off();
}

void ime_off(void) {
    active = 0;
    ime_clear();
    con_status_bar_off();
}
