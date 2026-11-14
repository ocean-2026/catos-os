/*
 * ime.c -- pinyin IME (v2.9).  Chinese input on a US-QWERTY keyboard.
 *
 * F2 toggles.  Letters build a syllable, the candidate bar lives on the
 * bottom console row (con_status_bar saves/restores it verbatim), 1-9
 * pick a candidate, Space picks the first, -/= page, Esc cancels.
 * Committed characters are 3-byte UTF-8; the front-end paints them.
 *
 * There is NO phrase/word mode -- this is a per-character IME, which is
 * honest about what 400 syllables and a 6763-glyph GB2312 font can do.
 */
#include "ime.h"
#include "keyboard.h"
#include "render.h"
#include "pinyin.inc"

static int  active;
static char py[8];
static int  py_len;
static int  page;
static int  anchor_row = -1;    /* row of the input line (editor sets) */

#define PER_PAGE 9
#define BAR_MAX  200

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

void ime_set_anchor(int row) { anchor_row = row; }

/* v2.25: candidates appear directly BELOW the input line (row+1) instead
 * of always hugging the bottom edge.  With prompt anchoring the input
 * line sits mid-screen, so the bar no longer covers fresh output. */
static int ime_bar_row(void) {
    int rows = con_rows();
    int r = (anchor_row >= 0) ? anchor_row + 1 : rows - 1;
    if (r >= rows) r = anchor_row - 1;          /* input on the last row */
    if (r < 0)     r = rows - 1;
    return r;
}

static void ime_show(void) {
    char bar[BAR_MAX];
    int n = 0;
    int row = ime_bar_row();
    bar[0] = 0;
    if (py_len == 0) {
        bar_put(bar, &n, "[IME] F2 off | type pinyin, 1-9 select, space first, -/= page");
        bar[n] = 0;
        con_status_bar_at(row, bar);
        return;
    }
    const py_entry_t *e = py_find(py);
    if (!e) {
        bar_put(bar, &n, "[IME] ");
        bar_put(bar, &n, py);
        bar_put(bar, &n, ": no match");
        bar[n] = 0;
        con_status_bar_at(row, bar);
        return;
    }
    int total = py_len_(e->hz) / 3;
    int start = page * PER_PAGE;
    if (start >= total) { page = 0; start = 0; }
    bar_put(bar, &n, "[IME] ");
    bar_put(bar, &n, py);
    bar_put(bar, &n, " ");
    bar_num(bar, &n, page + 1);
    bar_put(bar, &n, "/");
    bar_num(bar, &n, (total + PER_PAGE - 1) / PER_PAGE);
    bar_put(bar, &n, " ");
    for (int i = 0; i < PER_PAGE && start + i < total; i++) {
        bar_num(bar, &n, i + 1);
        for (int k = 0; k < 3 && n < BAR_MAX - 1; k++)
            bar[n++] = e->hz[(start + i) * 3 + k];
        bar_put(bar, &n, " ");
    }
    bar_put(bar, &n, "(-/= page, esc)");
    bar[n] = 0;
    con_status_bar_at(row, bar);
}

static void ime_cancel(void) {
    py_len = 0; py[0] = 0; page = 0;
    ime_show();                        /* back to the idle hint */
}

static int ime_commit(int idx, char out[5]) {
    const py_entry_t *e = py_find(py);
    if (!e || py_len == 0) return IME_PASS;
    int total = py_len_(e->hz) / 3;
    int k = page * PER_PAGE + idx;
    if (k >= total) return IME_CONSUME;  /* empty slot: just consume */
    out[0] = e->hz[k * 3];
    out[1] = e->hz[k * 3 + 1];
    out[2] = e->hz[k * 3 + 2];
    out[3] = 0;
    ime_cancel();
    return IME_COMMIT;
}

int ime_active(void) { return active; }

int ime_feed(int key, char out[5]) {
    if (key == KB_F2) {
        active = !active;
        if (active) { py_len = 0; py[0] = 0; page = 0; ime_show(); }
        else          con_status_bar_off();
        return IME_CONSUME;
    }
    if (!active) return IME_PASS;
    if (key >= 'A' && key <= 'Z') key += 32;
    if (key >= 'a' && key <= 'z') {
        if (py_len < 6) { py[py_len++] = (char)key; py[py_len] = 0; page = 0; }
        ime_show();
        return IME_CONSUME;
    }
    if (key == '\b') {
        if (py_len > 0) { py_len--; py[py_len] = 0; page = 0; ime_show(); }
        return IME_CONSUME;
    }
    if (key == 27)  { ime_cancel(); return IME_CONSUME; }
    if (key == '-') { if (page > 0) page--; ime_show(); return IME_CONSUME; }
    if (key == '=' || key == ']') { page++; ime_show(); return IME_CONSUME; }
    if (key >= '1' && key <= '9') return ime_commit(key - '1', out);
    if (key == ' ' && py_len > 0) return ime_commit(0, out);
    if (key == '\n') { if (py_len > 0) ime_cancel(); return IME_PASS; }
    return IME_PASS;
}

void ime_refresh(void) { if (active) ime_show(); }

void ime_off(void) {
    if (!active) return;
    active = 0;
    py_len = 0; py[0] = 0; page = 0;
    con_status_bar_off();
}
