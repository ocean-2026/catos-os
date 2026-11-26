/*
 * textlayout.c -- cap2 文本排版（折行 / 省略 / 四档字号 / 伪粗 / 等宽数字 /
 *                 简版禁则 / 度量缓存）。
 *
 * 冻结接口（gui_int.h）：
 *   txt_font_h / txt_measure_w / txt_wrapped_h / txt_draw_block。
 * 度量与绘制共用同一套原子折行结果；wrap=0 且 16px 档时像素口径与旧版
 * 逐像素一致（旧版：8px/ASCII 字符、16px 字高、溢出尾部 "..."）。
 *
 * 覆盖率合成一律经 blk_cov / blk_px_blend（渲染包提供强定义；本文件内的
 * weak 回落仅为独立分支可链接，集成时被强定义覆盖）。
 */
#include <stdint.h>
#include <stddef.h>
#include "gui/block.h"
#include "gui/tokens.h"
#include "gui_int.h"
#include "gfx24.h"
#include "render_be.h"

/* 8px 档 ASCII 字模（font8.c）。 */
const uint8_t *f8_ascii(unsigned char ch);
void           f8_ascii_bold(unsigned char ch, uint8_t out[16]);

/* ---- 四档字号槽位：font_size 0/1/2/3/4 = 16/8/16/24/32 ------------- */
typedef struct { int h, aw, ww; } slot_t;   /* 字高 / ASCII 步进 / 宽字符步进 */
static const slot_t SLOT[5] = {
    { 16,  8, 16 },   /* 0 -> 16 */
    {  8,  8,  8 },   /* 1 ->  8 */
    { 16,  8, 16 },   /* 2 -> 16 */
    { 24, 12, 24 },   /* 3 -> 24 */
    { 32, 16, 32 },   /* 4 -> 32 */
};

static int slot_idx(uint8_t fs) {
    switch (fs) {
    case 1: return 1;
    case 3: return 3;
    case 4: return 4;
    case 2: return 2;
    default: return 0;
    }
}

/* ---- 探针可见的行结构（cap2p_text.c 镜像同布局） --------------------- */
typedef struct {
    const char *start;
    int    len;         /* 字节长（不含省略号） */
    int    w;           /* 行宽 px（不含省略号） */
    int    n_atom;
    uint8_t ell;        /* 1 = 尾部追加 "..." */
} txt_line_t;

#define MAX_ATOM 256
#define MAX_LINE 8

typedef struct {
    uint32_t cp;
    int      off;
    uint8_t  len;
    uint8_t  wide;
    uint8_t  space;
    uint8_t  close;     /* 行首禁则闭合标点 */
    int      adv;
} atom_t;

/* ---- UTF-8 解码：返回字节数；*cp 落码点；失败回落 '?' --------------- */
static int utf8_decode(const char *s, uint32_t *cp) {
    uint8_t c = (uint8_t)s[0];
    if (!c) return 0;
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0 && s[1]) {
        *cp = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && s[1] && s[2]) {
        *cp = ((uint32_t)(c &  0x0F) << 12) |
              ((uint32_t)(s[1] & 0x3F) <<  6) |
              (uint32_t)(s[2] & 0x3F);
        return 3;
    }
    *cp = '?';
    return 1;
}

/* 行首禁则：闭合标点不得出现在行首。 */
static int is_close_punct(uint32_t cp) {
    switch (cp) {
    case 0x3002: case 0x3001: case 0xFF0C: case 0xFF09:
    case 0x300D: case 0x3011: case 0xFF01: case 0xFF1F:
    case 0xFF1A: case 0xFF1B: case 0x300B: case 0x201D:
    case 0x2019: case 0x300F: case 0x3009: case 0x2026:
    case 0xFF08: /* 成对开括号不参与行首回落，列全避免误判 */
        return cp != 0xFF08;
    }
    return 0;
}

static int is_digit_colon(uint32_t cp) {
    return (cp >= '0' && cp <= '9') || cp == ':';
}

static int build_atoms(const char *s, uint8_t fs, uint8_t sc, uint8_t tabular,
                       atom_t *at, int maxa) {
    const slot_t *sl = &SLOT[slot_idx(fs)];
    int n = 0;
    const char *p = s;
    while (p && *p && n < maxa) {
        uint32_t cp;
        int len = utf8_decode(p, &cp);
        atom_t *a = &at[n];
        a->cp = cp;
        a->off = (int)(p - s);
        a->len = (uint8_t)len;
        a->wide = cp >= 0x1100u ? 1 : 0;
        a->space = (cp == ' ');
        a->close = is_close_punct(cp);
        int adv = a->wide ? sl->ww : sl->aw;
        if (tabular && is_digit_colon(cp)) adv = sl->aw;  /* 等宽步进 */
        a->adv = adv * sc;
        n++;
        p += len;
    }
    return n;
}

/* ---- 折行核心 -------------------------------------------------------- */
static atom_t  scratch_at[MAX_ATOM];

typedef struct { int at0, at1, w; uint8_t ell; } lrec_t;
static lrec_t scratch_rec[MAX_LINE];

static int layout_core(const char *s, uint8_t fs, uint8_t sc, uint8_t tabular,
                       int max_w, uint8_t wrap, uint8_t ellipsis,
                       txt_line_t *lines, int outsz) {
    atom_t *at = scratch_at;
    lrec_t *rec = scratch_rec;
    int n = build_atoms(s, fs, sc, tabular, at, MAX_ATOM);
    if (n == 0) {
        lines[0].start = s; lines[0].len = 0; lines[0].w = 0;
        lines[0].n_atom = 0; lines[0].ell = 0;
        return 0;
    }

    int max_lines = wrap == 0 ? 1 : (wrap == 255 ? 6 : (wrap > 6 ? 6 : wrap));
    if (max_w < 1) max_w = 1000000;

    int li = 0, i = 0;
    while (i < n && li < max_lines) {
        int start = i, lw = 0;
        while (i < n) {
            atom_t *a = &at[i];
            if (lw + a->adv <= max_w || i == start) {
                lw += a->adv; i++; continue;
            }
            /* 禁则：闭合标点不拉到行首，附加到当前行末（允许轻微溢出）。 */
            if (a->close && i > start) { lw += a->adv; i++; continue; }
            break;
        }
        /* 词边界省略/折行：回退到最后一个空格之后（空格留在上行）。 */
        if (ellipsis == 2 && wrap != 0) {
            int sp = -1;
            for (int k = start; k < i; k++) if (at[k].space) sp = k;
            if (sp > start) i = sp + 1;
        }
        rec[li].at0 = start; rec[li].at1 = i;
        rec[li].w = lw; rec[li].ell = 0;
        li++;
    }

    /* 超出行数：末行截断并按省略模式补 "..."。 */
    if (i < n && li > 0) {
        lrec_t *last = &rec[li - 1];
        if (ellipsis > 0) {
            const slot_t *sl = &SLOT[slot_idx(fs)];
            int dotw = 3 * sl->aw * sc;
            /* 词边界模式：先回退到行内最后一个空格。 */
            if (ellipsis == 2) {
                int sp = -1;
                for (int k = last->at1 - 1; k >= last->at0; k--)
                    if (at[k].space) { sp = k; break; }
                if (sp >= last->at0) last->at1 = sp;   /* 空格留在上行末 */
            }
            while (last->at1 - last->at0 > 1 && last->w + dotw > max_w) {
                last->at1--;
                last->w -= at[last->at1].adv;
            }
            last->ell = 1;
        }
    }

    /* 孤字回收：末行仅 1 个宽字符时并回上一行。 */
    if (li >= 2) {
        lrec_t *ll = &rec[li - 1];
        if (ll->at1 - ll->at0 == 1 && at[ll->at0].wide) {
            lrec_t *pl = &rec[li - 2];
            pl->at1 = ll->at1;
            pl->w += ll->w;
            pl->ell = ll->ell;
            li--;
        }
    }

    for (int k = 0; k < li; k++) {
        const atom_t *a0 = &at[rec[k].at0];
        const atom_t *a1 = &at[rec[k].at1 - 1];
        lines[k].start = s + a0->off;
        lines[k].len = a1->off + a1->len - a0->off;
        lines[k].w = rec[k].w;
        lines[k].n_atom = rec[k].at1 - rec[k].at0;
        lines[k].ell = rec[k].ell;
    }
    if (li > outsz) li = outsz;
    return li;
}

/* ---- 度量缓存 ---------------------------------------------------------- */
#define CACHE_N 8
static struct {
    const char *txt;
    int    max_w;
    uint8_t fs, wrap, ell, lh, sc, tab, used;
    int    lines, natw, h;
} cache[CACHE_N];
static int cache_hits = 0;

static int cache_get(const char *s, int max_w, uint8_t fs, uint8_t wrap,
                     uint8_t ell, uint8_t lh, uint8_t sc, uint8_t tab,
                     int *lines, int *natw, int *h) {
    for (int i = 0; i < CACHE_N; i++) {
        if (!cache[i].used || cache[i].txt != s || cache[i].max_w != max_w ||
            cache[i].fs != fs || cache[i].wrap != wrap || cache[i].ell != ell ||
            cache[i].lh != lh || cache[i].sc != sc || cache[i].tab != tab)
            continue;
        *lines = cache[i].lines; *natw = cache[i].natw; *h = cache[i].h;
        cache_hits++;
        return 1;
    }
    return 0;
}

static void cache_put(const char *s, int max_w, uint8_t fs, uint8_t wrap,
                      uint8_t ell, uint8_t lh, uint8_t sc, uint8_t tab,
                      int lines, int natw, int h) {
    static int ring = 0;
    int i = ring; ring = (ring + 1) % CACHE_N;
    cache[i].txt = s; cache[i].max_w = max_w;
    cache[i].fs = fs; cache[i].wrap = wrap; cache[i].ell = ell;
    cache[i].lh = lh; cache[i].sc = sc; cache[i].tab = tab;
    cache[i].used = 1;
    cache[i].lines = lines; cache[i].natw = natw; cache[i].h = h;
}

/* ---- 行高 -------------------------------------------------------------- */
static int line_height(uint8_t fs, uint8_t sc, uint8_t line_h_px) {
    if (line_h_px) return line_h_px;
    const struct style *tok = gui_active_style();
    if (tok->line_h) return tok->line_h;
    int h = SLOT[slot_idx(fs)].h * sc;
    return h + h / 2;                       /* 16->24, 8->12, 24->36, 32->48 */
}

int txt_font_h(uint8_t font_size, uint8_t scale) {
    uint8_t sc = scale ? scale : 1;
    return SLOT[slot_idx(font_size)].h * sc;
}

int txt_measure_w(const char *s, uint8_t font_size, uint8_t scale,
                  uint8_t tabular) {
    if (!s) return 0;
    uint8_t sc = scale ? scale : 1;
    atom_t at[MAX_ATOM];
    int n = build_atoms(s, font_size, sc, tabular, at, MAX_ATOM);
    int w = 0;
    for (int i = 0; i < n; i++) w += at[i].adv;
    return w;
}

int txt_wrapped_h(const char *s, uint8_t font_size, uint8_t scale,
                  uint8_t tabular, int max_w, uint8_t wrap_lines,
                  uint8_t ellipsis, uint8_t line_h_px) {
    if (!s || !*s) return txt_font_h(font_size, scale);
    uint8_t sc = scale ? scale : 1;
    int lines, natw, h;
    if (cache_get(s, max_w, font_size, wrap_lines, ellipsis, line_h_px, sc,
                  tabular, &lines, &natw, &h))
        return h;
    txt_line_t la[MAX_LINE];
    int nl = layout_core(s, font_size, sc, tabular, max_w, wrap_lines,
                          ellipsis, la, MAX_LINE);
    if (nl == 0) nl = 1;
    int lh = line_height(font_size, sc, line_h_px);
    int maxlw = 0;
    for (int i = 0; i < nl; i++) if (la[i].w > maxlw) maxlw = la[i].w;
    h = nl * lh;
    cache_put(s, max_w, font_size, wrap_lines, ellipsis, line_h_px, sc,
              tabular, nl, maxlw, h);
    return h;
}

/* ---- 字模覆盖率采样 ---------------------------------------------------- */
static int ascii_bit(const void *g, int c, int r) {
    const unsigned char *a = (const unsigned char *)g;
    return a[r] & (1u << (7 - c));
}
static int hz_bit(const void *g, int c, int r) {
    const uint8_t *h = (const uint8_t *)g;
    return h[r * 2 + (c >> 3)] & (1u << (7 - (c & 7)));
}

static const void *glyph_src(uint32_t cp, int *wide) {
    if (cp >= 0x1100u) {
        const uint8_t *hz = render_font_hz(cp);
        if (hz) { *wide = 1; return hz; }
    }
    *wide = 0;
    return render_font_glyph((unsigned char)cp);
}

/* 目标像素覆盖率 0..255；slot 见 SLOT 下标。 */
static int cov_at(int slot, int wide, const void *g, int tx, int ty,
                  int eight) {
    if (slot == 1) {                        /* 8px 档 */
        if (wide) {
            /* 16x16 汉字模 2x2 覆盖率加权降到 8x8：每目标点取源 2x2 块。 */
            int sx0 = tx * 2, sy0 = ty * 2, hits = 0;
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++)
                    if (hz_bit(g, sx0 + dx, sy0 + dy)) hits++;
            return hits * 255 / 4;
        }
        const unsigned char *a = (const unsigned char *)g;
        if (eight)                          /* f8 8x8 表 / f8 伪粗 16 行位图 */
            return a[ty] & (1u << (7 - tx)) ? 255 : 0;
        int sr = ty * 2;
        return (a[sr] | a[sr + 1]) & (1u << (7 - tx)) ? 255 : 0;
    }
    if (slot == 4) {                        /* 32px：2 倍 */
        int sx = tx >> 1, sy = ty >> 1;
        return wide ? (hz_bit(g, sx, sy) ? 255 : 0)
                    : (ascii_bit(g, sx, sy) ? 255 : 0);
    }
    if (slot == 3) {                        /* 24px：1.5 倍 2x2 加权 */
        int cw = wide ? 24 : 12, ch = 24;
        int c0 = tx * 16 / cw, c1 = (tx + 1) * 16 / cw - 1;
        int r0 = ty * 16 / ch, r1 = (ty + 1) * 16 / ch - 1;
        int hits = 0, tot = 0;
        for (int r = r0; r <= r1; r++)
            for (int c = c0; c <= c1; c++) {
                tot++;
                if (wide ? hz_bit(g, c, r) : ascii_bit(g, c, r)) hits++;
            }
        return tot ? hits * 255 / tot : 0;
    }
    /* 16px 原生 */
    return wide ? (hz_bit(g, tx, ty) ? 255 : 0)
                : (ascii_bit(g, tx, ty) ? 255 : 0);
}

static void draw_glyph(block *b, int x, int y, int slot, int wide,
                       const void *g, int cw, int ch, int bold, uint32_t fg,
                       int eight) {
    for (int ty = 0; ty < ch; ty++) {
        for (int tx = 0; tx < cw; tx++) {
            int cov = cov_at(slot, wide, g, tx, ty, eight);
            if (bold && tx > 0) cov |= cov_at(slot, wide, g, tx - 1, ty, eight);
            if (cov <= 0) continue;
            int bc = blk_cov(b, x + tx, y + ty);
            cov = cov * bc / 255;
            if (cov > 0) blk_px_blend(x + tx, y + ty, fg, cov);
        }
    }
}

static void draw_line(block *b, int x0, int y, const txt_line_t *ln,
                      uint8_t fs, uint8_t sc, uint8_t bold, uint8_t tab,
                      uint32_t fg) {
    int idx = slot_idx(fs);
    const slot_t *sl = &SLOT[idx];
    int x = x0;
    const char *p = ln->start, *end = p + ln->len;
    while (p < end) {
        uint32_t cp;
        int len = utf8_decode(p, &cp);
        int wide = 0;
        const void *g = glyph_src(cp, &wide);
        int eight = 0;
        if (idx == 1 && !wide) {
            uint8_t b8[16];
            if (bold) { f8_ascii_bold((unsigned char)cp, b8); g = b8; }
            else      { g = f8_ascii((unsigned char)cp); }
            eight = 1;
        }
        int adv = wide ? sl->ww : sl->aw;
        if (tab && is_digit_colon(cp)) adv = sl->aw;
        adv *= sc;
        int cw = (wide ? sl->ww : sl->aw) * sc;
        int ch = sl->h * sc;
        if (sc == 2) {
            int bw = wide ? sl->ww : sl->aw;
            for (int sy = 0; sy < 2; sy++)
                for (int sx = 0; sx < 2; sx++)
                    draw_glyph(b, x + sx * bw, y + sy * sl->h,
                               idx, wide, g, bw, sl->h, bold, fg, eight);
        } else {
            draw_glyph(b, x, y, idx, wide, g, cw, ch, bold, fg, eight);
        }
        x += adv;
        p += len;
    }
    if (ln->ell) {
        for (int d = 0; d < 3; d++) {
            const void *g = render_font_glyph('.');
            draw_glyph(b, x, y, idx, 0, g, sl->aw * sc, sl->h * sc, bold, fg, 0);
            x += sl->aw * sc;
        }
    }
}

void txt_draw_block(block *b, uint32_t fg) {
    if (!b || !b->text) return;
    uint8_t sc = b->st.scale ? b->st.scale : 1;
    txt_line_t la[MAX_LINE];
    int nl = layout_core(b->text, b->font_size, sc, b->font_tabular, b->w,
                         b->wrap_lines, b->ellipsis, la, MAX_LINE);
    if (nl == 0) return;
    int lh = line_height(b->font_size, sc, b->line_h);
    int gh = txt_font_h(b->font_size, sc);
    int multi = nl > 1;
    int total = multi ? nl * lh : gh;       /* 单行沿用旧版字高几何 */
    int y0 = b->kind == BLK_BUTTON ? b->y + (b->h - total) / 2 : b->y;
    for (int l = 0; l < nl; l++) {
        int x = b->x;
        if (b->kind == BLK_BUTTON) x = b->x + (b->w - la[l].w) / 2;
        int y = y0 + (multi ? l * lh : 0);
        draw_line(b, x, y, &la[l], b->font_size, sc,
                  b->font_bold, b->font_tabular, fg);
    }
}

/* ---- 探针导出（cap2p_text.c 经 extern 声明调用） ----------------------- */
int txt_probe_layout(const char *s, uint8_t fs, uint8_t sc, uint8_t tab,
                     int max_w, uint8_t wrap, uint8_t ell, uint8_t lh,
                     txt_line_t *out, int outsz) {
    (void)lh;
    txt_line_t la[MAX_LINE];
    int nl = layout_core(s, fs, sc, tab, max_w, wrap, ell, la, MAX_LINE);
    for (int i = 0; i < nl && i < outsz; i++) out[i] = la[i];
    return nl;
}

int txt_probe_cache_hits(void) { return cache_hits; }
void txt_probe_cache_reset(void) { cache_hits = 0; }

/* ---- weak 回落覆盖率合成（渲染包强定义落地后自动被覆盖） -------------- */
__attribute__((weak)) uint8_t blk_alpha_of(block *b) {
    uint32_t a = 255;
    for (; b; b = b->parent)
        if (b->alpha) a = a * b->alpha / 255;
    return (uint8_t)a;
}

__attribute__((weak)) uint8_t blk_cov(block *b, int x, int y) {
    (void)x; (void)y;
    return blk_alpha_of(b);
}

__attribute__((weak)) void blk_px_blend(int x, int y, uint32_t color, int cov) {
    if (cov <= 0) return;
    if (cov >= 255) { gfx24_putpx(x, y, color); return; }
    uint32_t under = gfx24_getpx(x, y);
    int ia = 255 - cov;
    int r = ((int)(under >> 16 & 0xFF) * ia + (int)(color >> 16 & 0xFF) * cov) / 255;
    int g = ((int)(under >>  8 & 0xFF) * ia + (int)(color >>  8 & 0xFF) * cov) / 255;
    int b = ((int)(under      & 0xFF) * ia + (int)(color      & 0xFF) * cov) / 255;
    gfx24_putpx(x, y, ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
}
