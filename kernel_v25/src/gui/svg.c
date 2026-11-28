/*
 * svg.c -- 内核极简 SVG 光栅化子集（规格 v3 追加，见 include/gui/svg.h）。
 *
 * 设计约束：
 *   - 只用整数运算；扫描线填充按行预取交点，不做每像素除法。
 *   - 缓存像素/几何缓冲全部 kvmalloc 堆分配，静态 .bss 只留小元数据表。
 *   - 解析失败 / 截断 / 未知元素 / 超大源 / 超大输出一律安全回落，绝不崩、
 *     不留脏像素；不支持的元素跳过而不报错。
 *
 * 不支持：gradient / clipPath / transform / filter / 文本 / pattern / 圆角
 * （rect rx 忽略）/ 描边（stroke 仅按 1px 等效折线绘制）。
 */
#include <stdint.h>
#include <stddef.h>
#include "gui/svg.h"
#include "fs.h"
#include "kvheap.h"
#include "kstring.h"
#include "serial_log.h"

/* ---- 本地 freestanding 助手 --------------------------------------------- */

static int xlen(const char *s) { int n = 0; if (s) while (s[n]) n++; return n; }

static int xeq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

static int xhas(const char *s, const char *sub) {
    if (!s || !sub) return 0;
    for (; *s; s++) {
        const char *a = s, *b = sub;
        while (*b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

static int lowerch(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; return c; }

/* ---- 固定点 Q8.8 -------------------------------------------------------- */
#define FIX 256

struct pt { int x, y; };

#define SVG_EDGE_MAX 64
#define SVG_PT_MAX   160
#define SVG_NAME_MAX 24
#define SVG_VAL_MAX  96

struct parse {
    const char *src;
    int   len;
    int   vx, vy, vw, vh;          /* viewBox Q8.8；vw=0 未设 */
    int   ow, oh;                  /* 输出 px */
    int   scale;                   /* Q8.8：px 每 viewBox 单位 */
    int   ox, oy;                  /* Q8.8：居中偏移 */
    uint32_t fill;
    uint8_t have_fill;
    uint8_t *mask;
    uint8_t *rgb;

    int edge_n;
    struct { int x1, y1, x2, y2; } edges[SVG_EDGE_MAX];
    int pt_n;
    struct pt pts[SVG_PT_MAX];
};

/* ---- 词法 --------------------------------------------------------------- */

static int skip_ws(const char *s, int i, int n) {
    while (i < n) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',') i++;
        else break;
    }
    return i;
}

/* 解析一个数（整数 / 一位小数），返回 Q8.8；失败 *ok=0。 */
static int parse_num(const char *s, int *io, int n, int *ok) {
    int i = skip_ws(s, *io, n);
    int neg = 0;
    if (i < n && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; i++; }
    int whole = 0, digit = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') { whole = whole * 10 + (s[i] - '0'); i++; digit++; }
    int frac = 0;
    if (i < n && s[i] == '.') {
        i++;
        if (i < n && s[i] >= '0' && s[i] <= '9') { frac = s[i] - '0'; i++; }
        if (i < n && s[i] >= '0' && s[i] <= '9') { frac = frac * 10 + (s[i] - '0'); i++; }
    }
    if (digit == 0 && frac == 0) { *ok = 0; return 0; }
    int v = whole * FIX + frac * FIX / 100;
    *io = i;
    *ok = 1;
    return neg ? -v : v;
}

/* 在标签子串 [a,b) 内取 name="value"；value 拷到 out（去引号）。命中 1。 */
static int attr_value(const char *tag, int a, int b, const char *name,
                      char *out, int outsz) {
    int nl = xlen(name);
    int i = a;
    while (i < b) {
        while (i < b && (tag[i] == ' ' || tag[i] == '\t' || tag[i] == '\n')) i++;
        int st = i;
        while (i < b && tag[i] != '=' && tag[i] != ' ' && tag[i] != '\t') i++;
        int kn = i - st;
        int match = (kn == nl);
        if (match) {
            for (int k = 0; k < nl; k++)
                if (lowerch(tag[st + k]) != lowerch(name[k])) { match = 0; break; }
        }
        if (match) {
            while (i < b && tag[i] != '=') i++;
            if (i < b && tag[i] == '=') i++;
            while (i < b && (tag[i] == ' ' || tag[i] == '\t')) i++;
            char q = 0;
            if (i < b && (tag[i] == '"' || tag[i] == '\'')) { q = tag[i]; i++; }
            int o = 0;
            while (i < b && tag[i] != '>' && tag[i] != '/' &&
                   tag[i] != q && o < outsz - 1) {
                out[o++] = tag[i++];
            }
            out[o] = 0;
            return 1;
        }
        /* 失配：跳过 '=' 及其值（引号包裹 / 裸值），保证 i 单调前进。 */
        while (i < b && tag[i] != ' ' && tag[i] != '\t' && tag[i] != '=') i++;
        if (i < b && tag[i] == '=') {
            i++;
            char q = 0;
            if (i < b && (tag[i] == '"' || tag[i] == '\'')) { q = tag[i]; i++; }
            while (i < b && tag[i] != '>' && tag[i] != '/' && tag[i] != q) i++;
            if (q && i < b && tag[i] == q) i++;
        }
    }
    return 0;
}

/* ---- 颜色 --------------------------------------------------------------- */

static uint32_t hex2(int c1, int c2) {
    int h = 0;
    for (int k = 0; k < 2; k++) {
        char c = k ? c2 : c1;
        int v = (c >= '0' && c <= '9') ? c - '0'
              : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0;
        h = h * 16 + v;
    }
    return (uint32_t)h;
}

/* 解析 fill 值；返回 0xFF000000 无效/默认；0 表示 none（透明跳过）。 */
static uint32_t parse_color(const char *v, int *none) {
    *none = 0;
    if (!v || !*v) return 0xFF000000u;
    if (v[0] == '#') {
        const char *h = v + 1;
        if (h[0] && h[1] && h[2] && h[3] == 0) {            /* #rgb */
            uint32_t r = hex2(h[0], h[0]), g = hex2(h[1], h[1]), b = hex2(h[2], h[2]);
            return 0xFF000000u | (r << 16) | (g << 8) | b;
        }
        if (h[0] && h[1] && h[2] && h[3] && h[4] && h[5] && h[6] == 0) {
            uint32_t r = hex2(h[0], h[1]), g = hex2(h[2], h[3]), b = hex2(h[4], h[5]);
            return 0xFF000000u | (r << 16) | (g << 8) | b;
        }
        return 0xFF000000u;
    }
    if (xeq(v, "none") || xeq(v, "transparent")) { *none = 1; return 0; }
    if (xeq(v, "black"))   return 0xFF000000u;
    if (xeq(v, "white"))   return 0xFFFFFFFFu;
    if (xeq(v, "red"))     return 0xFFE03A3Au;
    if (xeq(v, "green"))   return 0xFF3AA85Au;
    if (xeq(v, "blue"))    return 0xFF4C8DFFu;
    if (xeq(v, "yellow"))  return 0xFFE8C53Au;
    if (xeq(v, "gray") || xeq(v, "grey")) return 0xFF808080u;
    if (xeq(v, "silver"))  return 0xFFC0C0C0u;
    return 0xFF000000u;
}

/* ---- 几何 --------------------------------------------------------------- */

static void edge_add(struct parse *p, int x1, int y1, int x2, int y2) {
    if (p->edge_n >= SVG_EDGE_MAX) return;
    if (y1 == y2) return;                       /* 水平边不参与扫描线 */
    p->edges[p->edge_n].x1 = x1; p->edges[p->edge_n].y1 = y1;
    p->edges[p->edge_n].x2 = x2; p->edges[p->edge_n].y2 = y2;
    p->edge_n++;
}

static void pts_push(struct parse *p, int x, int y) {
    if (p->pt_n >= SVG_PT_MAX) return;
    p->pts[p->pt_n].x = x; p->pts[p->pt_n].y = y;
    p->pt_n++;
}

static void poly_edges(struct parse *p) {
    for (int i = 0; i < p->pt_n; i++) {
        struct pt a = p->pts[i], b = p->pts[(i + 1) % p->pt_n];
        edge_add(p, a.x, a.y, b.x, b.y);
    }
}

/* 坐标变换：viewBox -> 输出 Q8.8 像素空间。scale 懒计算（viewBox 在 <svg>
 * 标签先于图元读到；未声明按 16×16 归一）。 */
static void ensure_view(struct parse *p) {
    if (p->scale) return;
    int vw = p->vw, vh = p->vh;
    if (vw <= 0) vw = 16 * FIX;
    if (vh <= 0) vh = 16 * FIX;
    /* scale 为 Q8.8「px / viewBox 单位」；vw/vh 是 Q8.8 宽度。 */
    int sxv = p->ow * FIX * FIX / vw, syv = p->oh * FIX * FIX / vh;
    p->scale = sxv < syv ? sxv : syv;
    int contw = (vw / FIX) * p->scale;
    int conth = (vh / FIX) * p->scale;
    p->ox = (p->ow * FIX - contw) / 2;
    p->oy = (p->oh * FIX - conth) / 2;
}
static int tx(struct parse *p, int vx) { ensure_view(p); int u = vx / FIX; return p->ox + u * p->scale; }
static int ty(struct parse *p, int vy) { ensure_view(p); int u = vy / FIX; return p->oy + u * p->scale; }

static void raster_fill(struct parse *p) {
    if (p->edge_n == 0) return;
    int xs[SVG_MAX_OUT];
    for (int row = 0; row < p->oh; row++) {
        int yc = row * FIX + FIX / 2;
        int nx = 0;
        for (int e = 0; e < p->edge_n; e++) {
            int x1 = p->edges[e].x1, y1 = p->edges[e].y1;
            int x2 = p->edges[e].x2, y2 = p->edges[e].y2;
            int lo = y1 < y2 ? y1 : y2, hi = y1 < y2 ? y2 : y1;
            if (yc <= lo || yc > hi) continue;
            int t = (yc - y1) * FIX / (y2 - y1);
            int x = x1 + ((x2 - x1) * t / FIX);
            if (nx < SVG_MAX_OUT) xs[nx++] = x;
        }
        if (nx < 2) continue;
        for (int a = 0; a < nx - 1; a++)
            for (int b = a + 1; b < nx; b++)
                if (xs[b] < xs[a]) { int t = xs[a]; xs[a] = xs[b]; xs[b] = t; }
        for (int k = 0; k + 1 < nx; k += 2) {
            int x0 = xs[k] >> 8, x1 = xs[k + 1] >> 8;
            if (x0 < 0) x0 = 0;
            if (x1 >= p->ow) x1 = p->ow - 1;
            for (int px = x0; px <= x1; px++) {
                int idx = row * p->ow + px;
                p->mask[idx] = 255;
                if (p->rgb) {
                    p->rgb[idx * 3 + 0] = (uint8_t)((p->fill >> 16) & 0xFFu);
                    p->rgb[idx * 3 + 1] = (uint8_t)((p->fill >> 8) & 0xFFu);
                    p->rgb[idx * 3 + 2] = (uint8_t)(p->fill & 0xFFu);
                }
            }
        }
    }
}

/* Bresenham 折线（line/polyline/开放 path），1px 等效描边。 */
static void raster_stroke(struct parse *p) {
    if (p->pt_n == 0) return;
    for (int i = 0; i + 1 < p->pt_n; i++) {
        int x0 = p->pts[i].x >> 8, y0 = p->pts[i].y >> 8;
        int x1 = p->pts[i + 1].x >> 8, y1 = p->pts[i + 1].y >> 8;
        int dx = x1 - x0, dy = y1 - y0;
        int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
        int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
        int err = adx - ady;
        int x = x0, y = y0;
        while (1) {
            if (x >= 0 && x < p->ow && y >= 0 && y < p->oh) {
                int idx = y * p->ow + x;
                p->mask[idx] = 255;
                if (p->rgb) {
                    p->rgb[idx * 3 + 0] = (uint8_t)((p->fill >> 16) & 0xFFu);
                    p->rgb[idx * 3 + 1] = (uint8_t)((p->fill >> 8) & 0xFFu);
                    p->rgb[idx * 3 + 2] = (uint8_t)(p->fill & 0xFFu);
                }
            }
            if (x == x1 && y == y1) break;
            int e2 = err * 2;
            if (e2 > -ady) { err -= ady; x += sx; }
            if (e2 < adx)  { err += adx; y += sy; }
        }
    }
}

/* 曲线细分：三次 / 二次贝塞尔压成折线点（Q8.8，整数 Bernstein）。 */
static void cubic_flat(struct parse *p, struct pt a, struct pt b,
                       struct pt c, struct pt d) {
    const int SEG = 8;
    for (int k = 0; k <= SEG; k++) {
        int t = k * FIX / SEG, u = FIX - t;
        int uu = u * u / FIX, tt = t * t / FIX;
        int uuu = uu * u / FIX, ttt = tt * t / FIX;
        int uut = uu * t / FIX, ttu = tt * u / FIX;
        int x = (uuu * a.x + 3 * uut * b.x + 3 * ttu * c.x + ttt * d.x) / FIX;
        int y = (uuu * a.y + 3 * uut * b.y + 3 * ttu * c.y + ttt * d.y) / FIX;
        pts_push(p, x, y);
    }
}

static void quad_flat(struct parse *p, struct pt a, struct pt b, struct pt c) {
    const int SEG = 8;
    for (int k = 0; k <= SEG; k++) {
        int t = k * FIX / SEG, u = FIX - t;
        int uu = u * u / FIX, tt = t * t / FIX, ut = u * t / FIX;
        int x = (uu * a.x + 2 * ut * b.x + tt * c.x) / FIX;
        int y = (uu * a.y + 2 * ut * b.y + tt * c.y) / FIX;
        pts_push(p, x, y);
    }
}

/* ---- 图元 --------------------------------------------------------------- */

static void shape_reset(struct parse *p) { p->edge_n = 0; p->pt_n = 0; }

static void do_rect(struct parse *p, const char *tag, int a, int b) {
    char v[SVG_VAL_MAX];
    int ok = 1, ii = 0;
    int x = 0, y = 0, w = 0, h = 0;
    if (attr_value(tag, a, b, "x", v, sizeof(v))) { ii = 0; x = parse_num(v, &ii, xlen(v), &ok); }
    if (attr_value(tag, a, b, "y", v, sizeof(v))) { ii = 0; y = parse_num(v, &ii, xlen(v), &ok); }
    if (attr_value(tag, a, b, "width", v, sizeof(v))) { ii = 0; w = parse_num(v, &ii, xlen(v), &ok); }
    if (attr_value(tag, a, b, "height", v, sizeof(v))) { ii = 0; h = parse_num(v, &ii, xlen(v), &ok); }
    if (!ok || w <= 0 || h <= 0) return;
    shape_reset(p);
    pts_push(p, tx(p, x),          ty(p, y));
    pts_push(p, tx(p, x + w),      ty(p, y));
    pts_push(p, tx(p, x + w),      ty(p, y + h));
    pts_push(p, tx(p, x),          ty(p, y + h));
    poly_edges(p);
    raster_fill(p);
}

static void do_circle(struct parse *p, const char *tag, int a, int b, int ell) {
    char v[SVG_VAL_MAX];
    int ok = 1, ii = 0;
    int cx = 0, cy = 0, r = 0, ry = 0;
    if (attr_value(tag, a, b, "cx", v, sizeof(v))) { ii = 0; cx = parse_num(v, &ii, xlen(v), &ok); }
    if (attr_value(tag, a, b, "cy", v, sizeof(v))) { ii = 0; cy = parse_num(v, &ii, xlen(v), &ok); }
    if (attr_value(tag, a, b, "r", v, sizeof(v)))  { ii = 0; r = parse_num(v, &ii, xlen(v), &ok); }
    if (ell) {
        if (attr_value(tag, a, b, "rx", v, sizeof(v))) { ii = 0; r = parse_num(v, &ii, xlen(v), &ok); }
        if (attr_value(tag, a, b, "ry", v, sizeof(v))) { ii = 0; ry = parse_num(v, &ii, xlen(v), &ok); }
        else ry = r;
    } else ry = r;
    if (!ok || r <= 0 || ry <= 0) return;
    shape_reset(p);
    const int N = 16;
    for (int k = 0; k < N; k++) {
        /* 角度 2πk/N；用整数近似：cos/sin 查表代价高，这里用圆上四点 + 中点细分。
         * 16 段弦近似在 ≤64px 输出下视觉可接受；整数运算，无三角函数。 */
        int ang = k * 360 / N;
        /* 用 Bresenham 圆思路直接在 viewBox Q8.8 上算点：cos/sin 以单位圆 16 点表近似。 */
        static const int cc[16] = { 256,236,181,98,0,-98,-181,-236,-256,-236,-181,-98,0,98,181,236 };
        static const int ss[16] = { 0,98,181,236,256,236,181,98,0,-98,-181,-236,-256,-236,-181,-98 };
        int px = cx + r * cc[ang * N / 360] / FIX;
        int py = cy + ry * ss[ang * N / 360] / FIX;
        pts_push(p, tx(p, px), ty(p, py));
    }
    poly_edges(p);
    raster_fill(p);
}

static void do_line(struct parse *p, const char *tag, int a, int b, int poly) {
    char v[SVG_VAL_MAX];
    int ok = 1, ii = 0;
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    if (poly) {
        if (!attr_value(tag, a, b, "points", v, sizeof(v))) return;
        int i = 0, n = xlen(v);
        shape_reset(p);
        while (i < n) {
            int px, py;
            px = parse_num(v, &i, n, &ok); if (!ok) break;
            py = parse_num(v, &i, n, &ok); if (!ok) break;
            pts_push(p, tx(p, px), ty(p, py));
        }
        if (p->pt_n < 2) return;
        if (poly == 2) { poly_edges(p); raster_fill(p); }
        else raster_stroke(p);
        return;
    }
    if (attr_value(tag, a, b, "x1", v, sizeof(v))) { ii = 0; x1 = parse_num(v, &ii, xlen(v), &ok); }
    if (attr_value(tag, a, b, "y1", v, sizeof(v))) { ii = 0; y1 = parse_num(v, &ii, xlen(v), &ok); }
    if (attr_value(tag, a, b, "x2", v, sizeof(v))) { ii = 0; x2 = parse_num(v, &ii, xlen(v), &ok); }
    if (attr_value(tag, a, b, "y2", v, sizeof(v))) { ii = 0; y2 = parse_num(v, &ii, xlen(v), &ok); }
    if (!ok) return;
    shape_reset(p);
    pts_push(p, tx(p, x1), ty(p, y1));
    pts_push(p, tx(p, x2), ty(p, y2));
    raster_stroke(p);
}

/* path：M/L/C/Q/Z（绝对 + 相对）。closed=1 走填充多边形，否则折线。 */
static void do_path(struct parse *p, const char *tag, int a, int b) {
    char d[SVG_VAL_MAX];
    if (!attr_value(tag, a, b, "d", d, sizeof(d))) return;
    int n = xlen(d), i = 0;
    shape_reset(p);
    char cmd = 0;
    int curx = 0, cury = 0;
    int closed = 0;
    while (i < n) {
        i = skip_ws(d, i, n);
        if (i >= n) break;
        char c = d[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) { cmd = c; i++; }
        int rel = (cmd >= 'a' && cmd <= 'z');
        char U = rel ? cmd - 32 : cmd;
        if (U == 'Z') { closed = 1; break; }
        int ok = 1;
        if (U == 'M' || U == 'L') {
            int px = parse_num(d, &i, n, &ok);
            int py = parse_num(d, &i, n, &ok);
            if (!ok) break;
            if (rel) { px += curx; py += cury; }
            curx = px; cury = py;
            if (U == 'M') pts_push(p, tx(p, px), ty(p, py));
            else          pts_push(p, tx(p, px), ty(p, py));
            /* M 后续重复点隐式为 L */
            cmd = rel ? 'l' : 'L';
        } else if (U == 'C') {
            int c1x = parse_num(d, &i, n, &ok), c1y = parse_num(d, &i, n, &ok);
            int c2x = parse_num(d, &i, n, &ok), c2y = parse_num(d, &i, n, &ok);
            int ex  = parse_num(d, &i, n, &ok), ey  = parse_num(d, &i, n, &ok);
            if (!ok) break;
            if (rel) { c1x += curx; c1y += cury; c2x += curx; c2y += cury; ex += curx; ey += cury; }
            struct pt A = { tx(p, curx), ty(p, cury) };
            struct pt B = { tx(p, c1x), ty(p, c1y) };
            struct pt C = { tx(p, c2x), ty(p, c2y) };
            struct pt D = { tx(p, ex),  ty(p, ey) };
            cubic_flat(p, A, B, C, D);
            curx = ex; cury = ey;
        } else if (U == 'Q') {
            int qx = parse_num(d, &i, n, &ok), qy = parse_num(d, &i, n, &ok);
            int ex = parse_num(d, &i, n, &ok), ey = parse_num(d, &i, n, &ok);
            if (!ok) break;
            if (rel) { qx += curx; qy += cury; ex += curx; ey += cury; }
            struct pt A = { tx(p, curx), ty(p, cury) };
            struct pt B = { tx(p, qx),  ty(p, qy) };
            struct pt C = { tx(p, ex),  ty(p, ey) };
            quad_flat(p, A, B, C);
            curx = ex; cury = ey;
        } else break;   /* 未知命令：跳过剩余 */
    }
    if (p->pt_n < 2) return;
    if (closed) { poly_edges(p); raster_fill(p); }
    else raster_stroke(p);
}

/* 遍历顶层标签，分派支持的图元。 */
static void parse_svg(struct parse *p) {
    int i = 0, n = p->len;
    while (i < n) {
        if (p->src[i] != '<') { i++; continue; }
        i++;
        if (i < n && p->src[i] == '?') { while (i < n && p->src[i] != '>') i++; continue; }
        if (i < n && p->src[i] == '/') { while (i < n && p->src[i] != '>') i++; continue; }
        int st = i;
        while (i < n && p->src[i] != '>' && p->src[i] != ' ') i++;
        int tn = i - st;
        int gt = i;
        while (gt < n && p->src[gt] != '>') gt++;
        char name[SVG_NAME_MAX];
        int k = 0;
        while (k < tn && k < SVG_NAME_MAX - 1) { name[k] = lowerch(p->src[st + k]); k++; }
        name[k] = 0;

        char v[SVG_VAL_MAX];
        int none = 0;
        if (attr_value(p->src, st, gt, "fill", v, sizeof(v))) {
            p->fill = parse_color(v, &none);
            p->have_fill = !none;
        } else { p->fill = 0xFF000000u; p->have_fill = 1; }

        if (xeq(name, "svg")) {
            if (attr_value(p->src, st, gt, "viewBox", v, sizeof(v))) {
                int vi = 0, ok = 1;
                p->vx = parse_num(v, &vi, xlen(v), &ok);
                p->vy = parse_num(v, &vi, xlen(v), &ok);
                p->vw = parse_num(v, &vi, xlen(v), &ok);
                p->vh = parse_num(v, &vi, xlen(v), &ok);
                if (!ok || p->vw <= 0 || p->vh <= 0) { p->vw = 0; p->vh = 0; }
            }
        } else if (!p->have_fill) {
            /* none 填充：整图元跳过（但继续解析后续图元） */
        } else if (xeq(name, "rect")) {
            do_rect(p, p->src, st, gt);
        } else if (xeq(name, "circle")) {
            do_circle(p, p->src, st, gt, 0);
        } else if (xeq(name, "ellipse")) {
            do_circle(p, p->src, st, gt, 1);
        } else if (xeq(name, "line")) {
            do_line(p, p->src, st, gt, 0);
        } else if (xeq(name, "polygon")) {
            do_line(p, p->src, st, gt, 2);
        } else if (xeq(name, "polyline")) {
            do_line(p, p->src, st, gt, 1);
        } else if (xeq(name, "path")) {
            do_path(p, p->src, st, gt);
        }
        /* 其它元素（gradient/clipPath/filter/text/...）显式跳过，不报错。 */
        i = gt + 1;
    }
}

/* ---- 内置兜底图标（16×16 单色位掩码；行 16bit，col c 位 15-c） ------------- */

static const uint16_t BUILTIN_GLYPH[4][16] = {
    /* 0 ok：对勾 */
    { 0x0000,0x0000,0x0000,0x0200,
      0x0400,0x0800,0x1000,0x2000,
      0x4000,0x2000,0x1000,0x0800,
      0x0400,0x03E0,0x0000,0x0000 },
    /* 1 cancel：X */
    { 0x0000,0x0000,0x0000,0x0810,
      0x0420,0x0240,0x0180,0x0180,
      0x0240,0x0420,0x0810,0x0000,
      0x0000,0x0000,0x0000,0x0000 },
    /* 2 folder */
    { 0x0000,0x0000,0x0000,0x0F00,
      0xFFF8,0x1008,0x1008,0x1008,
      0x1008,0x1008,0x1008,0xFFF8,
      0x0000,0x0000,0x0000,0x0000 },
    /* 3 file */
    { 0x0000,0x0000,0x0000,0x07E0,
      0x0810,0x07E0,0x0810,0x0810,
      0x0810,0x0810,0x0810,0x07E0,
      0x0000,0x0000,0x0000,0x0000 }
};

const char *svg_builtin_name(uint8_t kind) {
    switch (kind) {
    case SVG_BTN_OK:     return "ok";
    case SVG_BTN_CANCEL: return "cancel";
    case SVG_BTN_FOLDER: return "folder";
    case SVG_BTN_FILE:   return "file";
    default:             return "file";
    }
}

static uint8_t kind_from_path(const char *path) {
    if (xhas(path, "folder") || xhas(path, "dir")) return SVG_BTN_FOLDER;
    if (xhas(path, "ok") || xhas(path, "check") || xhas(path, "tick")) return SVG_BTN_OK;
    if (xhas(path, "cancel") || xhas(path, "close") || xhas(path, "no")) return SVG_BTN_CANCEL;
    return SVG_BTN_FILE;
}

/* ---- 缓存 --------------------------------------------------------------- */

static struct svg_icon slots[SVG_CACHE_SLOTS];
static uint32_t svg_clk;
static int svg_miss, svg_fallback;

static uint32_t fnv_str(const char *s) {
    uint32_t h = 0x811C9DC5u;
    if (s) while (*s) { h ^= (uint8_t)*s; h *= 16777619u; s++; }
    return h;
}

static void slot_free(struct svg_icon *it) {
    if (it->rgb)  { kvmfree(it->rgb);  it->rgb = 0; }
    if (it->mask) { kvmfree(it->mask); it->mask = 0; }
    it->w = it->h = 0; it->mono = 0; it->key = 0;
}

void svg_cache_reset(void) {
    for (int i = 0; i < SVG_CACHE_SLOTS; i++) slot_free(&slots[i]);
    svg_clk = 0; svg_miss = 0; svg_fallback = 0;
}

int svg_cache_stats(int *miss, int *fallback) {
    int n = 0;
    for (int i = 0; i < SVG_CACHE_SLOTS; i++) if (slots[i].mask) n++;
    if (miss) *miss = svg_miss;
    if (fallback) *fallback = svg_fallback;
    return n;
}

/* 渲染内置兜底到位图（mono 掩码，最近邻）。成功 1。 */
static int render_builtin(struct svg_icon *it, uint8_t kind, int size) {
    if (kind > 3) kind = SVG_BTN_FILE;
    int ow = size, oh = size;
    uint8_t *mask = kvmalloc((uint32_t)(ow * oh), 16);
    if (!mask) return 0;
    memset(mask, 0, ow * oh);
    const uint16_t *g = BUILTIN_GLYPH[kind];
    for (int dy = 0; dy < oh; dy++) {
        int sr = dy * 16 / oh;
        if (sr > 15) sr = 15;
        uint16_t bits = g[sr];
        for (int dx = 0; dx < ow; dx++) {
            int sc = dx * 16 / ow;
            if (sc > 15) sc = 15;
            if (bits & (1u << (15 - sc))) mask[dy * ow + dx] = 255;
        }
    }
    it->w = ow; it->h = oh; it->rgb = 0; it->mask = mask;
    it->mono = 1; it->kind = kind;
    return 1;
}

/* 解析 + 光栅化文件 SVG；成功 1（it 像素已分配），失败 0。 */
static int render_file_svg(struct svg_icon *it, const char *path, int size) {
    uint32_t fsize = 0;
    if (fs_resolve_file(path, 0, &fsize) != 0) return 0;
    if (fsize == 0 || fsize > SVG_MAX_SRC) return 0;

    char *src = kvmalloc(fsize + 1, 16);
    if (!src) return 0;
    int got = fs_read_at(path, src, 0, (int)fsize);
    if (got <= 0) { kvmfree(src); return 0; }
    src[got] = 0;

    int ow = size, oh = size;
    uint8_t *mask = kvmalloc((uint32_t)(ow * oh), 16);
    uint8_t *rgb  = kvmalloc((uint32_t)(ow * oh * 3), 16);
    if (!mask || !rgb) {
        kvmfree(src);
        if (mask) kvmfree(mask);
        if (rgb) kvmfree(rgb);
        return 0;
    }
    memset(mask, 0, ow * oh);
    memset(rgb, 0, ow * oh * 3);

    /* 几何 scratch 走堆分配：内核栈仅 8KB，struct parse 约 8KB，禁止压栈。 */
    struct parse *p = kvmalloc(sizeof(struct parse), 16);
    if (!p) {
        kvmfree(src); kvmfree(mask); kvmfree(rgb); return 0;
    }
    memset(p, 0, sizeof(*p));
    p->src = src; p->len = got;
    p->ow = ow; p->oh = oh;
    p->mask = mask; p->rgb = rgb;
    p->vw = 0; p->vh = 0;

    parse_svg(p);

    /* 空掩码（解析后无墨迹）视为失败，走兜底。 */
    int ink = 0;
    for (int i = 0; i < ow * oh; i++) if (mask[i]) { ink = 1; break; }

    kvmfree(p);
    kvmfree(src);
    if (!ink) { kvmfree(mask); kvmfree(rgb); return 0; }

    /* viewBox 等比映射在 transform 内完成；scale/ox/oy 在解析 svg 标签后补齐。
     * 若 viewBox 未声明，按 16×16 归一。 */
    it->w = ow; it->h = oh; it->mask = mask; it->rgb = rgb;
    it->mono = 0; it->kind = 0;
    return 1;
}

const struct svg_icon *svg_resolve(const char *path, int size, uint8_t state) {
    if (size <= 0) size = 16;
    if (size > SVG_MAX_OUT) size = SVG_MAX_OUT;
    /* 仅 16 / 24 两档对外；其它值钳到最近档。 */
    if (size < 24) size = 16; else size = 24;

    uint32_t key = fnv_str(path) ^ ((uint32_t)size << 8) ^ (uint32_t)state;

    for (int i = 0; i < SVG_CACHE_SLOTS; i++) {
        if (slots[i].mask && slots[i].key == key) {
            slots[i].lru = ++svg_clk;
            return &slots[i];
        }
    }

    svg_miss++;

    /* 选槽：空槽优先；否则淘汰 LRU 最旧。 */
    int slot = -1, oldest = 0;
    for (int i = 0; i < SVG_CACHE_SLOTS; i++) {
        if (!slots[i].mask) { slot = i; break; }
        if (slot < 0 || slots[i].lru < slots[oldest].lru) oldest = i;
    }
    if (slot < 0) slot = oldest;
    slot_free(&slots[slot]);

    struct svg_icon *it = &slots[slot];
    int ok = 0;
    if (path && *path) ok = render_file_svg(it, path, size);

    if (!ok) {
        uint8_t kind = kind_from_path(path);
        if (render_builtin(it, kind, size)) {
            svg_fallback++;
            const char *reason = 0;
            /* reason 由调用路径推断：文件缺失 / 解析失败。render_file 内部不细分；
             * 这里按 fs_resolve 是否存在再判一次（缓存未命中路径才到这里）。 */
            uint32_t fsz = 0;
            if (!path || !*path) reason = "nopath";
            else if (fs_resolve_file(path, 0, &fsz) != 0) reason = "noent";
            else if (fsz > SVG_MAX_SRC) reason = "toolarge";
            else reason = "parse";
            ser_write("GUI:img fallback path=");
            ser_write(path ? path : "(null)");
            ser_write(" reason=");
            ser_write(reason ? reason : "parse");
            ser_write(" icon=");
            ser_write(svg_builtin_name(kind));
            ser_write("\n");
            it->key = key;
            it->lru = ++svg_clk;
            return it;
        }
        slot_free(it);
        return 0;
    }

    it->key = key;
    it->lru = ++svg_clk;
    return it;
}
