/*
 * tui.c -- v2.67 cell-grid widget toolkit.
 *
 * Hard rules:
 *   1. every widget writes through con_set_style_rgb() + con_write_at(), so
 *      the cell grid and the dirty tracker always know what is on screen;
 *   2. no wide glyphs: boxes are '+', '-', '|' and gauges are '#'/'.';
 *   3. every widget clips to the screen and fills the cells it owns, so a
 *      later repaint of that row cannot leave half a border behind.
 */
#include <stdint.h>
#ifndef NULL
#define NULL ((void *)0)
#endif

#include "render.h"
#include "palette.h"
#include "tui.h"

/* ---- helpers ----------------------------------------------------------- */
static int t_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static void t_cat(char *d, int max, const char *s) {
    int n = t_len(d), i = 0;
    while (s && s[i] && n + i < max - 1) { d[n + i] = s[i]; i++; }
    d[n + i] = 0;
}
static void t_num(char *d, int max, int v) {
    char b[16]; int i = 0;
    if (v < 0) { t_cat(d, max, "-"); v = -v; }
    if (!v) b[i++] = '0';
    while (v > 0 && i < 15) { b[i++] = (char)('0' + v % 10); v /= 10; }
    while (i > 0) { char c[2]; c[0] = b[--i]; c[1] = 0; t_cat(d, max, c); }
}

static uint32_t role(uint32_t rgb, int idx) {
    if (rgb) return rgb;
    return pal_rgb((int)idx);
}

/* write one single-cell char with explicit colours */
static void put(int x, int y, char ch, uint32_t fg, uint32_t bg) {
    if (x < 0 || y < 0 || x >= con_cols() || y >= con_rows()) return;
    con_set_style_rgb(fg, bg, 0);
    char b[2]; b[0] = ch; b[1] = 0;
    con_write_at((uint8_t)x, (uint8_t)y, b);
}

/* write a trimmed string */
static void puts_fg(int x, int y, const char *s, uint32_t fg, uint32_t bg,
                    int maxcells) {
    if (!s) return;
    int n = 0;
    while (s[n] && n < maxcells) {
        char c = s[n];
        if (c == '\n' || c == '\r') break;
        put(x + n, y, c, fg, bg);
        n++;
    }
}

/* ---- primitives -------------------------------------------------------- */
void tui_fill(int gx, int gy, int w, int h, uint32_t bg_rgb) {
    bg_rgb = role(bg_rgb, PAL_BG);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) put(gx + x, gy + y, ' ', bg_rgb, bg_rgb);
}

void tui_box(int gx, int gy, int w, int h, uint32_t fg_rgb, const char *title) {
    if (w < 2 || h < 2) return;
    fg_rgb = role(fg_rgb, PAL_DIM);
    for (int x = 1; x < w - 1; x++) {
        put(gx + x, gy, '-', fg_rgb, 0);
        put(gx + x, gy + h - 1, '-', fg_rgb, 0);
    }
    for (int y = 1; y < h - 1; y++) {
        put(gx, gy + y, '|', fg_rgb, 0);
        put(gx + w - 1, gy + y, '|', fg_rgb, 0);
    }
    put(gx, gy, '+', fg_rgb, 0);
    put(gx + w - 1, gy, '+', fg_rgb, 0);
    put(gx, gy + h - 1, '+', fg_rgb, 0);
    put(gx + w - 1, gy + h - 1, '+', fg_rgb, 0);
    if (title && *title) {
        int tw = t_len(title);
        if (tw + 4 < w) {
            put(gx + 2, gy, ' ', fg_rgb, 0);
            puts_fg(gx + 3, gy, title, role(0, PAL_FG), 0, w - 5);
            put(gx + 3 + tw, gy, ' ', fg_rgb, 0);
        }
    }
}

void tui_panel(int gx, int gy, int w, int h, uint32_t fg_rgb, uint32_t bg_rgb,
               const char *title) {
    if (w < 2 || h < 2) return;
    bg_rgb = role(bg_rgb, PAL_BG);
    for (int y = 1; y < h - 1; y++)
        for (int x = 1; x < w - 1; x++) put(gx + x, gy + y, ' ', fg_rgb, bg_rgb);
    tui_box(gx, gy, w, h, fg_rgb, title);
}

void tui_label(int gx, int gy, uint32_t fg_rgb, const char *text) {
    puts_fg(gx, gy, text, role(fg_rgb, PAL_FG), role(0, PAL_BG), con_cols() - gx);
}

void tui_label_bg(int gx, int gy, uint32_t fg_rgb, uint32_t bg_rgb, const char *text) {
    puts_fg(gx, gy, text, role(fg_rgb, PAL_FG), role(bg_rgb, PAL_BG), con_cols() - gx);
}

void tui_hline_sep(int gx, int gy, int w, uint32_t fg_rgb) {
    fg_rgb = role(fg_rgb, PAL_DIM);
    for (int x = 0; x < w; x++) put(gx + x, gy, '-', fg_rgb, 0);
}

/* ---- widgets ----------------------------------------------------------- */
void tui_gauge(int gx, int gy, int w, int pct, uint32_t fg_rgb,
               uint32_t bg_rgb, const char *label) {
    if (w < 4) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    fg_rgb = role(fg_rgb, PAL_SUCCESS);
    bg_rgb = role(bg_rgb, PAL_BG);
    int labw = label ? t_len(label) + 1 : 0;
    int barw = w - labw - 5;                 /* "[...] 100%" */
    if (barw < 2) { barw = w - 5; labw = 0; }
    if (barw < 1) return;
    put(gx, gy, '[', role(0, PAL_DIM), bg_rgb);
    int fill = (pct * barw) / 100;
    for (int i = 0; i < barw; i++)
        put(gx + 1 + i, gy, i < fill ? '#' : '.',
            i < fill ? fg_rgb : role(0, PAL_DIM), bg_rgb);
    put(gx + 1 + barw, gy, ']', role(0, PAL_DIM), bg_rgb);
    char pb[8]; pb[0] = 0; t_num(pb, sizeof(pb), pct); t_cat(pb, sizeof(pb), "%");
    puts_fg(gx + 2 + barw, gy, pb, role(0, PAL_INFO), bg_rgb, 4);
    if (label) {
        int lx = gx + w - t_len(label);
        if (lx < gx + 2 + barw + 5) lx = gx + w - t_len(label);
        puts_fg(lx, gy, label, role(0, PAL_FG), bg_rgb, t_len(label));
    }
}

void tui_kv(int gx, int gy, int w, uint32_t kfg, uint32_t vfg,
            const char *key, const char *val) {
    if (w < 4) return;
    int kw = w / 2;
    puts_fg(gx, gy, key, role(kfg, PAL_DIM), role(0, PAL_BG), kw - 1);
    int vn = t_len(val);
    int vx = gx + w - vn;
    if (vx < gx + kw) vx = gx + kw;
    puts_fg(vx, gy, val, role(vfg, PAL_FG), role(0, PAL_BG), gx + w - vx);
}

void tui_tabs(int gx, int gy, int w, const char *const *names, int count,
              int selected, uint32_t accent, uint32_t dim) {
    (void)w;
    int x = gx;
    for (int i = 0; i < count; i++) {
        int n = t_len(names[i]);
        if (i == selected) {
            char b[40]; b[0] = 0;
            t_cat(b, sizeof(b), "[ ");
            t_cat(b, sizeof(b), names[i]);
            t_cat(b, sizeof(b), " ]");
            puts_fg(x, gy, b, role(accent, PAL_ACCENT), role(0, PAL_BG), t_len(b));
            x += t_len(b) + 2;
        } else {
            puts_fg(x, gy, names[i], role(dim, PAL_DIM), role(0, PAL_BG), n);
            x += n + 2;
        }
    }
}

void tui_section(int gx, int gy, int w, uint32_t fg, const char *title) {
    fg = role(fg, PAL_ACCENT);
    char b[64]; b[0] = 0;
    t_cat(b, sizeof(b), " ");
    t_cat(b, sizeof(b), title);
    t_cat(b, sizeof(b), " ");
    puts_fg(gx, gy, b, fg, role(0, PAL_BG), w);
    int n = t_len(b);
    for (int x = n; x < w; x++) put(gx + x, gy, '-', fg, role(0, PAL_BG));
}

void tui_hintbar(int gy, int w, uint32_t fg, uint32_t bg, const char *hints) {
    fg = role(fg, PAL_STATUS_FG);
    bg = role(bg, PAL_STATUS_BG);
    char b[96]; b[0] = 0;
    t_cat(b, sizeof(b), " ");
    t_cat(b, sizeof(b), hints);
    for (int x = 0; x < w && x < con_cols(); x++) put(x, gy, ' ', fg, bg);
    puts_fg(0, gy, b, fg, bg, w);
}

void tui_row(int gx, int gy, int w, const int *colw, int ncol,
             uint32_t fg, uint32_t bg, const char *const *vals, int header) {
    fg = role(fg, header ? PAL_ACCENT : PAL_FG);
    bg = role(bg, header ? PAL_SELECTION : PAL_BG);
    int x = gx;
    for (int c = 0; c < ncol; c++) {
        int cw = (c == ncol - 1) ? (gx + w - x) : colw[c];
        if (cw < 1) break;
        for (int i = 0; i < cw; i++) put(x + i, gy, ' ', fg, bg);
        if (vals && vals[c]) {
            int n = t_len(vals[c]);
            if (n > cw) n = cw;
            puts_fg(x, gy, vals[c], fg, bg, n);
        }
        x += cw;
        if (x >= gx + w) break;
    }
}

void tui_spark(int gx, int gy, int w, const uint8_t *v, int n,
               uint32_t fg, uint32_t bg) {
    if (!v || n <= 0) return;
    fg = role(fg, PAL_INFO);
    bg = role(bg, PAL_BG);
    static const char LV[] = " .:-=+*#%";
    for (int x = 0; x < w; x++) {
        int idx = (n <= w) ? (x * n / w) : x;
        if (idx >= n) idx = n - 1;
        int p = v[idx];
        if (p < 0) p = 0;
        if (p > 100) p = 100;
        put(gx + x, gy, LV[(p * 9) / 100], fg, bg);
    }
}
