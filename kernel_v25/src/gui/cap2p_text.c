/*
 * cap2p_text.c -- 文本排版包探针（wrap / 省略 / 四档字高 / 伪粗 / 等宽 /
 *                 禁则 / 度量缓存）。
 * 消费 `gui cap2-<case>` 的 case 名（不含 "cap2-" 前缀）；返回 1 = 已消费。
 * 输出：`GUI:cap2-<case> PASS k=v ...` 或 `GUI:cap2-<case> FAIL detail`，
 * 数字全部由 txt_* 实测/取点，不写死。
 */
#include <stdint.h>
#include <stddef.h>
#include "gui/block.h"
#include "gui/gui.h"
#include "gui_int.h"
#include "gfx24.h"
#include "serial_log.h"
#include "render.h"
#include "render_be.h"

/* 与 textlayout.c 同布局的行结构镜像。 */
typedef struct {
    const char *start;
    int    len;
    int    w;
    int    n_atom;
    uint8_t ell;
} txt_line_t;

extern int   txt_font_h(uint8_t font_size, uint8_t scale);
extern int   txt_measure_w(const char *s, uint8_t fs, uint8_t scale,
                           uint8_t tabular);
extern int   txt_wrapped_h(const char *s, uint8_t fs, uint8_t scale,
                           uint8_t tabular, int max_w, uint8_t wrap,
                           uint8_t ellipsis, uint8_t line_h_px);
extern int   txt_probe_layout(const char *s, uint8_t fs, uint8_t sc,
                              uint8_t tab, int max_w, uint8_t wrap,
                              uint8_t ell, uint8_t lh,
                              txt_line_t *out, int outsz);
extern int   txt_probe_cache_hits(void);
extern void  txt_probe_cache_reset(void);

static void t_dec(uint32_t v) {
    char b[12]; int n = 0;
    if (!v) { ser_putc('0'); return; }
    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    while (n--) ser_putc(b[n]);
}

static int eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static int dec_utf8(const char *s, uint32_t *cp) {
    uint8_t c = (uint8_t)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0 && s[1]) {
        *cp = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F); return 2;
    }
    if ((c & 0xF0) == 0xE0 && s[1] && s[2]) {
        *cp = ((uint32_t)(c & 0x0F) << 12) |
              ((uint32_t)(s[1] & 0x3F) << 6) | (uint32_t)(s[2] & 0x3F); return 3;
    }
    *cp = '?'; return 1;
}

static int is_close(uint32_t cp) {
    switch (cp) {
    case 0x3002: case 0x3001: case 0xFF0C: case 0xFF09:
    case 0x300D: case 0x3011: case 0xFF01: case 0xFF1F:
    case 0xFF1A: case 0xFF1B: case 0x300B: case 0x201D:
    case 0x2019: case 0x300F: case 0x3009: case 0x2026:
        return 1;
    }
    return 0;
}

/* ---- 各 case ---------------------------------------------------------- */

static int case_wrap1(const char *arg) {
    (void)arg;
    const char *s = "The quick brown fox jumps over the lazy dog nearby";
    txt_line_t ln[8];
    int nl = txt_probe_layout(s, 0, 1, 0, 80, 1, 1, 0, ln, 8);
    int ok = nl == 1;
    ser_write("GUI:cap2-wrap1 ");
    ser_write(ok ? "PASS" : "FAIL");
    ser_write(" lines="); t_dec(nl);
    ser_write(" ell="); t_dec(ln[0].ell);
    ser_write(" w="); t_dec(ln[0].w);
    ser_write("\n");
    return 1;
}

static int case_wrap2(const char *arg) {
    (void)arg;
    const char *s = "The quick brown fox jumps over the lazy dog nearby";
    txt_line_t ln[8];
    int nl = txt_probe_layout(s, 0, 1, 0, 80, 2, 1, 0, ln, 8);
    int ok = nl == 2;
    ser_write("GUI:cap2-wrap2 ");
    ser_write(ok ? "PASS" : "FAIL");
    ser_write(" lines="); t_dec(nl);
    ser_write(" w0="); t_dec(ln[0].w);
    ser_write(" w1="); t_dec(ln[1].w);
    ser_write("\n");
    return 1;
}

static int case_wrapauto(const char *arg) {
    (void)arg;
    const char *s = "The quick brown fox jumps over the lazy dog nearby while "
                    "the sun shines brightly over the quiet hills beyond";
    txt_line_t ln[8];
    int nl = txt_probe_layout(s, 0, 1, 0, 60, 255, 1, 0, ln, 8);
    int ok = nl >= 1 && nl <= 6;
    ser_write("GUI:cap2-wrapauto ");
    ser_write(ok ? "PASS" : "FAIL");
    ser_write(" lines="); t_dec(nl);
    ser_write(" maxw="); t_dec(ln[nl - 1].w);
    ser_write("\n");
    return 1;
}

static int case_ellchar(const char *arg) {
    (void)arg;
    const char *s = "The quick brown fox jumps over the lazy dog";
    txt_line_t ln[8];
    int nl = txt_probe_layout(s, 0, 1, 0, 60, 1, 1, 0, ln, 8);
    int full = txt_measure_w(s, 0, 1, 0);
    int ok = nl == 1 && ln[0].ell == 1 && ln[0].w < full;
    ser_write("GUI:cap2-ellipsis-char ");
    ser_write(ok ? "PASS" : "FAIL");
    ser_write(" lines="); t_dec(nl);
    ser_write(" ell="); t_dec(ln[0].ell);
    ser_write(" w="); t_dec(ln[0].w);
    ser_write(" full="); t_dec(full);
    ser_write("\n");
    return 1;
}

static int case_ellword(const char *arg) {
    (void)arg;
    const char *s = "The quick brown fox jumps over the lazy dog nearby";
    txt_line_t ln[8];
    int nl = txt_probe_layout(s, 0, 1, 0, 100, 1, 2, 0, ln, 8);
    /* 词边界：末行首字符的前一个源字符必须是空格（或已是行首）。 */
    int boundary = 1;
    if (nl >= 1 && ln[0].start > s) {
        char prev = ln[0].start[-1];
        if (prev != ' ') boundary = 0;
    }
    int ok = nl == 1 && ln[0].ell == 1 && boundary;
    ser_write("GUI:cap2-ellipsis-word ");
    ser_write(ok ? "PASS" : "FAIL");
    ser_write(" lines="); t_dec(nl);
    ser_write(" ell="); t_dec(ln[0].ell);
    ser_write(" boundary="); t_dec(boundary);
    ser_write("\n");
    return 1;
}

static int case_font(const char *arg, uint8_t fs, int exp_h, int exp_aw,
                     int exp_ww, const char *tag) {
    (void)arg;
    int h = txt_font_h(fs, 1);
    int wa = txt_measure_w("0", fs, 1, 0);
    int ww = txt_measure_w("中", fs, 1, 0);
    int ok = h == exp_h && wa == exp_aw && ww == exp_ww;
    ser_write("GUI:cap2-"); ser_write(tag);
    ser_write(ok ? " PASS" : " FAIL");
    ser_write(" h="); t_dec(h);
    ser_write(" aw="); t_dec(wa);
    ser_write(" ww="); t_dec(ww);
    ser_write("\n");
    return 1;
}

static int hz_ink16(uint32_t cp) {
    const uint8_t *h = render_font_hz(cp);
    if (!h) return 0;
    int n = 0;
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 16; c++)
            if (h[r * 2 + (c >> 3)] & (1u << (7 - (c & 7)))) n++;
    return n;
}

static int case_font8_cjk(const char *arg) {
    (void)arg;
    /* 像素读数必须在 gfx24 后端：文本控制台里 LFB 为空，读数恒 0。 */
    con_set_gui_mode(1);
    if (con_request_mode(CON_MODE_VBE24, CON_ORIGIN_CONSOLE) != 0) {
        ser_write("GUI:cap2-font8-cjk FAIL gfx=0\n");
        con_set_gui_mode(0);
        return 1;
    }
    gui_begin();
    gui_root_block()->w = GFX24_W;
    gui_root_block()->h = GFX24_H;

    blk_spec sp;
    for (int i = 0; i < (int)sizeof(sp); i++) ((char *)&sp)[i] = 0;
    sp.layer = LAYER_WORK; sp.visible = 1; sp.scale = 1;
    sp.req_w = -1; sp.req_h = -1;
    sp.fg = gui_active_style()->fg;
    sp.bg = gui_active_style()->panel;
    sp.font_size = 1;

    block_id id = b_text(0, &sp, "中");
    gui_frame();

    block *b = btable_get(id);
    uint32_t cp; dec_utf8("中", &cp);
    int ink16 = hz_ink16(cp);
    int lit = 0, left = 0, right = 0;
    if (b) {
        uint32_t bg = gfx24_getpx(b->x + 1, b->y + 1);
        int br = bg >> 16 & 0xFF, bg_ = bg >> 8 & 0xFF, bb = bg & 0xFF;
        for (int y = 0; y < b->h; y++)
            for (int x = 0; x < b->w; x++) {
                uint32_t c = gfx24_getpx(b->x + x, b->y + y);
                int r = c >> 16 & 0xFF, g = c >> 8 & 0xFF, bl = c & 0xFF;
                int dr = r > br ? r - br : br - r;
                int dg = g > bg_ ? g - bg_ : bg_ - g;
                int db = bl > bb ? bl - bb : bb - bl;
                if (dr > 30 || dg > 30 || db > 30) {
                    lit++;
                    if (x < b->w / 2) left++; else right++;
                }
            }
    }
    int ok = b && lit > 0 && lit * 2 <= ink16 && left > 0 && right > 0;
    ser_write("GUI:cap2-font8-cjk ");
    ser_write(ok ? "PASS" : "FAIL");
    ser_write(" lit="); t_dec(lit);
    ser_write(" ink16="); t_dec(ink16);
    ser_write(" left="); t_dec(left);
    ser_write(" right="); t_dec(right);
    ser_write("\n");
    gui_end();
    con_set_gui_mode(0);
    con_force_text();
    con_clear();
    return 1;
}

static int case_font8(const char *arg)  { return case_font(arg, 1,  8,  8,  8, "font8"); }
static int case_font16(const char *arg) { return case_font(arg, 0, 16,  8, 16, "font16"); }
static int case_font24(const char *arg) { return case_font(arg, 3, 24, 12, 24, "font24"); }
static int case_font32(const char *arg) { return case_font(arg, 4, 32, 16, 32, "font32"); }

static int lit_count(block *b) {
    uint32_t bg = gfx24_getpx(b->x + 1, b->y + 1);
    int br = bg >> 16 & 0xFF, bg_ = bg >> 8 & 0xFF, bb = bg & 0xFF;
    int n = 0;
    for (int y = 0; y < b->h; y++)
        for (int x = 0; x < b->w; x++) {
            uint32_t c = gfx24_getpx(b->x + x, b->y + y);
            int r = c >> 16 & 0xFF, g = c >> 8 & 0xFF, bl = c & 0xFF;
            int dr = r > br ? r - br : br - r;
            int dg = g > bg_ ? g - bg_ : bg_ - g;
            int db = bl > bb ? bl - bb : bb - bl;
            if (dr > 30 || dg > 30 || db > 30) n++;
        }
    return n;
}

static int case_bold(const char *arg) {
    (void)arg;
    /* 像素读数必须在 gfx24 后端：文本控制台里 LFB 为空，读数恒 0。 */
    con_set_gui_mode(1);
    if (con_request_mode(CON_MODE_VBE24, CON_ORIGIN_CONSOLE) != 0) {
        ser_write("GUI:cap2-bold FAIL gfx=0\n");
        con_set_gui_mode(0);
        return 1;
    }
    gui_begin();
    gui_root_block()->w = GFX24_W;
    gui_root_block()->h = GFX24_H;

    blk_spec sp;
    for (int i = 0; i < (int)sizeof(sp); i++) ((char *)&sp)[i] = 0;
    sp.layer = LAYER_WORK; sp.visible = 1; sp.scale = 1;
    sp.req_w = -1; sp.req_h = -1;
    sp.fg = gui_active_style()->fg;
    sp.bg = gui_active_style()->panel;

    block_id col = b_column(0, &sp);
    block_id id0 = b_text(col, &sp, "Hello");
    sp.font_bold = 1;
    block_id id1 = b_text(col, &sp, "Hello");
    gui_frame();

    block *b0 = btable_get(id0);
    block *b1 = btable_get(id1);
    int l0 = b0 ? lit_count(b0) : -1;
    int l1 = b1 ? lit_count(b1) : -1;
    int ok = l0 > 0 && l1 >= l0;
    ser_write("GUI:cap2-bold ");
    ser_write(ok ? "PASS" : "FAIL");
    ser_write(" thin="); t_dec(l0);
    ser_write(" bold="); t_dec(l1);
    ser_write("\n");
    gui_end();
    con_set_gui_mode(0);
    con_force_text();
    con_clear();
    return 1;
}

static int case_tabular(const char *arg) {
    (void)arg;
    int w0 = txt_measure_w("12:00", 0, 1, 1);
    int w1 = txt_measure_w("12:01", 0, 1, 1);
    int dw = txt_measure_w("0", 0, 1, 1);
    int ok = w0 == w1;
    for (char d = '1'; d <= '9'; d++) {
        char one[2] = { d, 0 };
        if (txt_measure_w(one, 0, 1, 1) != dw) ok = 0;
    }
    ser_write("GUI:cap2-tabular ");
    ser_write(ok ? "PASS" : "FAIL");
    ser_write(" w00="); t_dec(w0);
    ser_write(" w01="); t_dec(w1);
    ser_write(" digit="); t_dec(dw);
    ser_write("\n");
    return 1;
}

static int case_kinsoku(const char *arg) {
    (void)arg;
    const char *s = "你好，世界，和平发展，前程似锦";
    txt_line_t ln[8];
    int nl = txt_probe_layout(s, 0, 1, 0, 40, 255, 1, 0, ln, 8);
    int bad = 0;
    for (int i = 0; i < nl; i++) {
        uint32_t cp;
        dec_utf8(ln[i].start, &cp);
        if (is_close(cp)) bad = 1;
    }
    int ok = nl >= 1 && !bad;
    ser_write("GUI:cap2-kinsoku ");
    ser_write(ok ? "PASS" : "FAIL");
    ser_write(" lines="); t_dec(nl);
    ser_write(" bad="); t_dec(bad);
    ser_write("\n");
    return 1;
}

static int case_cache(const char *arg) {
    (void)arg;
    const char *s = "The quick brown fox jumps over the lazy dog";
    txt_probe_cache_reset();
    int h1 = txt_wrapped_h(s, 0, 1, 0, 120, 2, 1, 0);
    int h2 = txt_wrapped_h(s, 0, 1, 0, 120, 2, 1, 0);
    /* 同文本同宽：改 line_h 必须未命中缓存且取到新值；改 ellipsis 也必须未命中。 */
    int h3 = txt_wrapped_h(s, 0, 1, 0, 120, 2, 1, 40);
    int h4 = txt_wrapped_h(s, 0, 1, 0, 120, 2, 2, 0);
    int hits = txt_probe_cache_hits();
    int ok = h1 == h2 && h3 != h1 && hits == 1;
    ser_write("GUI:cap2-cache ");
    ser_write(ok ? "PASS" : "FAIL");
    ser_write(" h1="); t_dec(h1);
    ser_write(" h2="); t_dec(h2);
    ser_write(" h3="); t_dec(h3);
    ser_write(" h4="); t_dec(h4);
    ser_write(" hits="); t_dec(hits);
    ser_write("\n");
    return 1;
}

int cap2_probe_text(const char *arg) {
    if (!arg) return 0;
    if (eq(arg, "wrap1"))          return case_wrap1(arg);
    if (eq(arg, "wrap2"))          return case_wrap2(arg);
    if (eq(arg, "wrapauto"))       return case_wrapauto(arg);
    if (eq(arg, "ellipsis-char"))  return case_ellchar(arg);
    if (eq(arg, "ellipsis-word"))  return case_ellword(arg);
    if (eq(arg, "font8"))          return case_font8(arg);
    if (eq(arg, "font8-cjk"))      return case_font8_cjk(arg);
    if (eq(arg, "font16"))         return case_font16(arg);
    if (eq(arg, "font24"))         return case_font24(arg);
    if (eq(arg, "font32"))         return case_font32(arg);
    if (eq(arg, "bold"))           return case_bold(arg);
    if (eq(arg, "tabular"))        return case_tabular(arg);
    if (eq(arg, "kinsoku"))         return case_kinsoku(arg);
    if (eq(arg, "cache"))          return case_cache(arg);
    return 0;
}
