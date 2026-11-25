/*
 * paint.c -- LFB 原语封装与节点绘制（规格 §5）。
 *
 * 原语：fill_rect / blit24 / draw_text（8x16 位图字形）/ draw_border。
 * 全部走裁剪栈：clip 矩形与父链求交；无效区域外不写 VRAM。
 * v1 无 alpha / 圆角 / 阴影 / 半透明；位图 1:1 blit，越界裁剪。
 */
#include "gui/gui.h"
#include "gui_int.h"
#include "gui/tokens.h"
#include "gfx24.h"
#include "render_be.h"

#define CLIP_DEPTH 8
typedef struct { int x0, y0, x1, y1; } clip_rect_t;
static clip_rect_t clip_stack[CLIP_DEPTH];
static int clip_depth;

/* 平移栈（OVF_SCROLL 视口偏移；只改绘制落点，不改子块几何）。 */
#define TRANS_DEPTH 8
static int tr_stack[TRANS_DEPTH][2];
static int tr_depth;

static void paint_trans_push(int dx, int dy) {
    if (tr_depth >= TRANS_DEPTH) return;
    int px = tr_depth ? tr_stack[tr_depth - 1][0] : 0;
    int py = tr_depth ? tr_stack[tr_depth - 1][1] : 0;
    tr_stack[tr_depth][0] = px + dx;
    tr_stack[tr_depth][1] = py + dy;
    tr_depth++;
}

static void paint_trans_pop(void) {
    if (tr_depth > 0) tr_depth--;
}

static int trans_x(void) { return tr_depth ? tr_stack[tr_depth - 1][0] : 0; }
static int trans_y(void) { return tr_depth ? tr_stack[tr_depth - 1][1] : 0; }

void paint_clip_push(int x, int y, int w, int h) {
    if (clip_depth >= CLIP_DEPTH) return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (clip_depth > 0) {
        clip_rect_t *t = &clip_stack[clip_depth - 1];
        if (x0 < t->x0) x0 = t->x0;
        if (y0 < t->y0) y0 = t->y0;
        if (x1 > t->x1) x1 = t->x1;
        if (y1 > t->y1) y1 = t->y1;
    }
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    clip_stack[clip_depth].x0 = x0;
    clip_stack[clip_depth].y0 = y0;
    clip_stack[clip_depth].x1 = x1;
    clip_stack[clip_depth].y1 = y1;
    clip_depth++;
}

void paint_clip_pop(void) {
    if (clip_depth > 0) clip_depth--;
}

static int clip_rect(int *x, int *y, int *w, int *h) {
    if (clip_depth > 0) {
        clip_rect_t *t = &clip_stack[clip_depth - 1];
        if (*x < t->x0) { *w += *x - t->x0; *x = t->x0; }
        if (*y < t->y0) { *h += *y - t->y0; *y = t->y0; }
        if (*x + *w > t->x1) *w = t->x1 - *x;
        if (*y + *h > t->y1) *h = t->y1 - *y;
    }
    return *w > 0 && *h > 0;
}

void gui_fill_rect(int x, int y, int w, int h, uint32_t rgb) {
    x -= trans_x(); y -= trans_y();
    if (!clip_rect(&x, &y, &w, &h)) return;
    gfx24_rect(x, y, w, h, rgb);
}

void gui_blit24_stride(const uint8_t *src, int x, int y, int w, int h,
                       int stride) {
    if (!src || w <= 0 || h <= 0) return;
    x -= trans_x(); y -= trans_y();
    int ox = x, oy = y;
    if (!clip_rect(&x, &y, &w, &h)) return;
    /* 裁剪后源指针按原始步进（stride 列）偏移到被保留的子矩形起点。 */
    const uint8_t *s = src +
        ((uint32_t)(y - oy) * (uint32_t)stride + (uint32_t)(x - ox)) * 3u;
    gfx24_blit_stride(s, x, y, w, h, stride);
}

void gui_blit24(const uint8_t *src, int x, int y, int w, int h) {
    gui_blit24_stride(src, x, y, w, h, w);
}

void gui_draw_text(int x, int y, const char *txt, uint32_t fg, uint8_t scale) {
    if (!txt) return;
    x -= trans_x(); y -= trans_y();
    if (scale != 2) scale = 1;
    int step = 8 * scale;
    for (const char *p = txt; *p; p++) {
        const unsigned char *g = render_font_glyph((unsigned char)*p);
        for (int j = 0; j < 16; j++) {
            unsigned row = g[j];
            for (int i = 0; i < 8; i++) {
                if (!(row & (1u << (7 - i)))) continue;
                if (scale == 2) {
                    gfx24_putpx(x + i * 2,     y + j * 2,     fg);
                    gfx24_putpx(x + i * 2 + 1, y + j * 2,     fg);
                    gfx24_putpx(x + i * 2,     y + j * 2 + 1, fg);
                    gfx24_putpx(x + i * 2 + 1, y + j * 2 + 1, fg);
                } else {
                    gfx24_putpx(x + i, y + j, fg);
                }
            }
        }
        x += step;
    }
}

void gui_draw_border(int x, int y, int w, int h, uint32_t color, uint8_t bw) {
    if (w <= 0 || h <= 0) return;
    if (bw < 1) bw = 1;
    /* 外层 1px 描边。 */
    gui_fill_rect(x, y, w, 1, color);
    gui_fill_rect(x, y + h - 1, w, 1, color);
    gui_fill_rect(x, y, 1, h, color);
    gui_fill_rect(x + w - 1, y, 1, h, color);
    if (bw >= 2) {
        /* v1 第二层：内缩 1px 的同色细线（无阴影/无圆角）。 */
        gui_fill_rect(x + 1, y + 1, w - 2, 1, color);
        gui_fill_rect(x + 1, y + h - 2, w - 2, 1, color);
        gui_fill_rect(x + 1, y + 1, 1, h - 2, color);
        gui_fill_rect(x + w - 2, y + 1, 1, h - 2, color);
    }
}

/* ---- 样式继承：显式色 → 父链 → 活动令牌角色（三级回落） ------------- */

static uint32_t role_token(uint8_t role, const struct style *tok) {
    switch (role) {
    case ROLE_FG:     return tok->fg;
    case ROLE_BG:     return tok->bg;
    case ROLE_PANEL:  return tok->panel;
    case ROLE_BAR:    return tok->bar;
    case ROLE_ACCENT: return tok->accent;
    case ROLE_BORDER: return tok->border;
    case ROLE_LINE:   return tok->line;
    default:          return 0;
    }
}

static uint32_t resolve_color(block *b, int which) {
    const struct style *tok = gui_active_style();
    block *cur = b;
    while (cur) {
        if (which == 0) {
            if (cur->st.fg)       return cur->st.fg;
            if (cur->st.role_fg)  return role_token(cur->st.role_fg, tok);
        }
        if (which == 1) {
            if (cur->st.bg)       return cur->st.bg;
            if (cur->st.role_bg)  return role_token(cur->st.role_bg, tok);
        }
        if (which == 2) {
            if (cur->st.accent)       return cur->st.accent;
            if (cur->st.role_accent)  return role_token(cur->st.role_accent, tok);
        }
        cur = cur->parent;
    }
    return which == 1 ? tok->bg : tok->fg;
}

static int text_trimmed(const char *txt, int max_w, uint8_t scale,
                        char *out, int outsz) {
    int step = 8 * scale;
    int cap = max_w / step;
    int len = 0;
    while (txt && txt[len]) len++;
    if (len <= cap) {
        int n = len < outsz - 1 ? len : outsz - 1;
        for (int i = 0; i < n; i++) out[i] = txt[i];
        out[n] = 0;
        return len;
    }
    /* 超宽截断，尾部补 "..."。 */
    int keep = cap - 3;
    if (keep < 0) keep = 0;
    int o = 0;
    for (int i = 0; i < keep && o < outsz - 4; i++) out[o++] = txt[i];
    out[o++] = '.'; out[o++] = '.'; out[o++] = '.'; out[o] = 0;
    return len;
}

/* ---- Calm & soft：角部离线掩码（超椭圆 n≈5，查表填像素，无逐像素浮点） ---- */

#define CALM_R_N 4
static const int CALM_RADII[CALM_R_N] = { 8, 12, 16, 24 };
/* 覆盖率掩码 cov∈[0,255]：角象限逐像素；255=块内，0=角外（保留父表面原值），
 * 过渡带内随超椭圆距离单调回落（无锯齿弧）。 */
static uint8_t calm_mask[CALM_R_N][24][24];
static int     calm_mask_ready;

static uint32_t fifth_pow(int v) { uint32_t x = (uint32_t)v; return x*x*x*x*x; }

static void calm_masks_init(void) {
    if (calm_mask_ready) return;
    for (int ri = 0; ri < CALM_R_N; ri++) {
        int r = CALM_RADII[ri];
        uint32_t r5 = fifth_pow(r);
        uint32_t inner = r5 * 80u / 100u;   /* 80% 以内全不透明 */
        for (int dy = 0; dy < r; dy++)
            for (int dx = 0; dx < r; dx++) {
                /* 角象限：u/v 为到圆心 (r,r) 的距离，角点 (0,0) 在圆外。 */
                uint32_t u = (uint32_t)(r - dx), v = (uint32_t)(r - dy);
                uint32_t t = fifth_pow(u) + fifth_pow(v);
                uint8_t cov;
                if (t <= inner)        cov = 255;
                else if (t >= r5)      cov = 0;
                else {
                    uint32_t rem = r5 - t;
                    uint32_t span = r5 - inner;
                    cov = (uint8_t)(span ? 255u * rem / span : 0);
                }
                calm_mask[ri][dy][dx] = cov;
            }
    }
    calm_mask_ready = 1;
}

/* ---- 覆盖率合成：out = under*(1-cov) + color*cov，under 为当前父表面结果 ---- */

static void blend_px(int x, int y, uint32_t color, int cov) {
    if (cov <= 0) return;                       /* 角外：逐像素保留父表面原值 */
    if (cov >= 255) { gfx24_putpx(x, y, color); return; }
    uint32_t under = gfx24_getpx(x, y);
    int ia = 255 - cov;
    int r = ((int)(under >> 16 & 0xFF) * ia + (int)(color >> 16 & 0xFF) * cov) / 255;
    int g = ((int)(under >>  8 & 0xFF) * ia + (int)(color >>  8 & 0xFF) * cov) / 255;
    int b = ((int)(under      & 0xFF) * ia + (int)(color      & 0xFF) * cov) / 255;
    gfx24_putpx(x, y, ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
}

/* ---- per-corner 贴边判定：8pt 量化后坐标精确相等（容差 0）即贴边 ---- */

/* 与 layout.c::grid_snap 同口径：calm 档位下内边距归整到 {4,8,12,16,24}。 */
static int snap_pad(int v) {
    if (!gui_style_calm() || v <= 0) return v;
    static const int steps[5] = { 4, 8, 12, 16, 24 };
    for (int i = 0; i < 5; i++)
        if (v <= steps[i]) return steps[i];
    return 24;
}

static void flush_edges(block *b, int *fl, int *fr, int *ft, int *fb) {
    *fl = *fr = *ft = *fb = 0;
    block *p = b->parent;
    if (!p) { *fl = *fr = *ft = *fb = 1; return; }
    int pl = snap_pad(p->pad_l), pr = snap_pad(p->pad_r);
    int pt = snap_pad(p->pad_t), pb = snap_pad(p->pad_b);
    int cx = p->x + pl, cy = p->y + pt;
    int cw = p->w - pl - pr, ch = p->h - pt - pb;
    if (cw < 0) cw = 0;
    if (ch < 0) ch = 0;
    if (b->bleed) { cx = p->x; cy = p->y; cw = p->w; ch = p->h; }
    if (b->x      == cx)     *fl = 1;
    if (b->x + b->w == cx + cw) *fr = 1;
    if (b->y      == cy)     *ft = 1;
    if (b->y + b->h == cy + ch) *fb = 1;
    /* 同列兄弟紧贴：相接边整条方角。 */
    for (block *s = p->first_child; s; s = s->next_sibling) {
        if (s == b) continue;
        if (s->x == b->x && s->x + s->w == b->x + b->w) {
            if (s->y + s->h == b->y) *ft = 1;
            if (b->y + b->h == s->y) *fb = 1;
        }
    }
}

/* 四角半径档：贴边角强制 0；返回 -1 表示该角方角。 */
static int corner_ri(int base_ri, int flush) { return flush ? -1 : base_ri; }

static int calm_on(void) {
    if (!gui_style_calm()) return 0;
    return gui_active_style() != gui_token_style(TOK_HIGH_CONTRAST);
}

/* full-bleed / 根块 / chrome / 桌面底：不圆角、不加外扩边距、不画阴影。 */
static int calm_shape_eligible(block *b) {
    if (!calm_on()) return 0;
    if (b->kind == BLK_ROOT || b->bleed) return 0;
    if (b->layer == LAYER_CHROME || b->layer == LAYER_BG) return 0;
    if (b->st.corner_mode == 1) return 0;
    return 1;
}

static int calm_r_index(int w, int h) {
    int s = w < h ? w : h;
    if (s >= 128) return 3;   /* 24：应用图标 / 大型欢迎卡 */
    if (s >= 64)  return 2;   /* 16：面板 / 弹出层外框 */
    if (s >= 32)  return 1;   /* 12：按钮 / 图标卡 */
    return 0;                 /* 8：小控件 */
}

/* 取某角掩码在 (col,row) 处的覆盖率；ri=-1（方角）返回 255。 */
static int corner_cov(int ri, int r, int col, int row,
                       int w, int h, int corner) {
    if (ri < 0 || r <= 0) return 255;
    int dy, dx;
    switch (corner) {
    case 0: dy = row;            dx = col;            break;  /* TL */
    case 1: dy = row;            dx = r - 1 - (col - (w - r)); break; /* TR */
    case 2: dy = r - 1 - (h - 1 - row); dx = col;    break;  /* BL */
    default:dy = r - 1 - (h - 1 - row); dx = r - 1 - (col - (w - r)); break; /* BR */
    }
    if (dy < 0 || dy >= r || dx < 0 || dx >= r) return 255;
    return calm_mask[ri][dy][dx];
}

/* 逐角覆盖率填充：角带逐像素混合，中段不透明直写；cov=0 保留父表面原值。 */
static void fill_round(block *b, int ri_tl, int ri_tr, int ri_bl, int ri_br,
                       uint32_t color) {
    int w = b->w, h = b->h;
    if (w <= 0 || h <= 0) return;
    int rtl = ri_tl < 0 ? 0 : CALM_RADII[ri_tl];
    int rtr = ri_tr < 0 ? 0 : CALM_RADII[ri_tr];
    int rbl = ri_bl < 0 ? 0 : CALM_RADII[ri_bl];
    int rbr = ri_br < 0 ? 0 : CALM_RADII[ri_br];
    if (rtl * 2 > w) rtl = w / 2;
    if (rtr * 2 > w) rtr = w / 2;
    if (rbl * 2 > w) rbl = w / 2;
    if (rbr * 2 > w) rbr = w / 2;

    for (int row = 0; row < h; row++) {
        int rL = row < rtl ? rtl : (row >= h - rbl ? rbl : 0);
        int rR = row < rtr ? rtr : (row >= h - rbr ? rbr : 0);
        int riL = row < rtl ? ri_tl : (row >= h - rbl ? ri_bl : -1);
        int riR = row < rtr ? ri_tr : (row >= h - rbr ? ri_br : -1);
        int y = b->y + row;
        /* 左角带逐像素。 */
        for (int col = 0; col < rL; col++)
            blend_px(b->x + col, y, color,
                     corner_cov(riL, rL, col, row, w, h,
                                row < rtl ? 0 : 2));
        /* 右角带逐像素。 */
        for (int col = w - rR; col < w; col++)
            blend_px(b->x + col, y, color,
                     corner_cov(riR, rR, col, row, w, h,
                                row < rtr ? 1 : 3));
        /* 中段不透明。 */
        int mx = rL, mw = w - rL - rR;
        if (mw > 0) gui_fill_rect(b->x + mx, y, mw, 1, color);
    }
}

static void corner_ris(block *b, int base,
                       int *tl, int *tr, int *bl, int *br) {
    int fl, fr, ft, fb;
    flush_edges(b, &fl, &fr, &ft, &fb);
    *tl = corner_ri(base, fl || ft);
    *tr = corner_ri(base, fr || ft);
    *bl = corner_ri(base, fl || fb);
    *br = corner_ri(base, fr || fb);
}

static void body_fill(block *b, uint32_t color) {
    if (calm_shape_eligible(b)) {
        calm_masks_init();
        int tl, tr, bl, br;
        corner_ris(b, calm_r_index(b->w, b->h), &tl, &tr, &bl, &br);
        fill_round(b, tl, tr, bl, br, color);
    } else {
        gui_fill_rect(b->x, b->y, b->w, b->h, color);
    }
}

static void body_border(block *b, uint32_t color, uint32_t bg, uint8_t bw) {
    if (bw < 1) return;
    if (calm_shape_eligible(b)) {
        calm_masks_init();
        int tl, tr, bl, br;
        corner_ris(b, calm_r_index(b->w, b->h), &tl, &tr, &bl, &br);
        fill_round(b, tl, tr, bl, br, color);
        if (bw >= 1) {
            block inner = *b;
            inner.x += bw; inner.y += bw;
            inner.w -= 2 * bw; inner.h -= 2 * bw;
            if (inner.w > 0 && inner.h > 0)
                fill_round(&inner, tl, tr, bl, br, bg);
        }
    } else {
        gui_draw_border(b->x, b->y, b->w, b->h, color, bw);
    }
}

static uint32_t mix_black(uint32_t c, int kpermil) {
    int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, bl = c & 0xFF;
    r = r * (1000 - kpermil) / 1000;
    g = g * (1000 - kpermil) / 1000;
    bl = bl * (1000 - kpermil) / 1000;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

/* 双层柔和阴影：WORK 卡片单层接触；OVERLAY/TOAST 双层。贴边/成组紧贴的块
 * 不投影（接缝处不得造假空隙或暗带），阴影只出现在自由边外侧。 */
static void paint_halo(block *b) {
    if (!calm_shape_eligible(b)) return;
    int el = 0;
    if (b->layer == LAYER_OVERLAY || b->layer == LAYER_TOAST) el = 2;
    else if (b->layer == LAYER_WORK) el = 1;
    if (!el) return;

    int fl, fr, ft, fb;
    flush_edges(b, &fl, &fr, &ft, &fb);
    /* 成组紧贴（上下接兄弟）或贴父上/下边：不投接触/投影层。 */
    if (ft || fb) return;

    uint32_t under = b->parent ? resolve_color(b->parent, 1)
                               : gui_active_style()->bg;
    uint32_t contact = mix_black(under, 180);   /* 接触层：黑 18% */
    gui_draw_border(b->x, b->y + 1, b->w, b->h, contact, 1);
    if (el == 2) {
        uint32_t drop = mix_black(under, 100); /* 投影层：黑 10% */
        gui_draw_border(b->x, b->y + 3, b->w, b->h, drop, 1);
        gui_draw_border(b->x, b->y + 4, b->w, b->h, drop, 1);
    }
}

void paint_node(block *b) {
    if (!b || !b->visible) return;

    uint32_t fg = resolve_color(b, 0);
    uint32_t bg = resolve_color(b, 1);
    uint32_t accent = resolve_color(b, 2);

    paint_halo(b);

    switch (b->kind) {
    case BLK_RECT:
        body_fill(b, bg);
        if (b->st.border_w)
            body_border(b, gui_active_style()->border, bg, b->st.border_w);
        break;
    case BLK_TEXT: {
        char buf[160];
        text_trimmed(b->text, b->w, b->st.scale, buf, sizeof(buf));
        if (bg) body_fill(b, bg);
        gui_draw_text(b->x, b->y, buf, fg, b->st.scale);
        break;
    }
    case BLK_IMAGE:
    case BLK_SURFACE:
        gui_blit24(b->pix, b->x, b->y, b->pix_w, b->pix_h);
        break;
    case BLK_CANVAS: {
        int rx, ry, rw, rh;
        if (cv_dirty_rect(b, &rx, &ry, &rw, &rh)) {
            const uint8_t *s = b->buf +
                ((uint32_t)ry * (uint32_t)b->pix_w + (uint32_t)rx) * 3u;
            gui_blit24_stride(s, b->x + rx, b->y + ry, rw, rh, b->pix_w);
            cv_dirty_clear(b);
        }
        break;
    }
    case BLK_DIVIDER:
        gui_fill_rect(b->x, b->y, b->w, 1, gui_active_style()->line);
        break;
    case BLK_BUTTON: {
        uint32_t bgc = bg, fgc = fg;
        if (b->focused) { fgc = bg; bgc = fg; }   /* 按压/聚焦反色反馈 */
        body_fill(b, bgc);
        if (b->st.border_w)
            body_border(b, gui_active_style()->border, bgc, b->st.border_w);
        char buf[160];
        text_trimmed(b->text, b->w, b->st.scale, buf, sizeof(buf));
        int blen = 0; while (buf[blen]) blen++;
        uint8_t sc = b->st.scale ? b->st.scale : 1;
        int tx = b->x + (b->w - 8 * sc * blen) / 2;
        int ty = b->y + (b->h - 16 * sc) / 2;
        gui_draw_text(tx, ty, buf, fgc, sc);
        break;
    }
    case BLK_INPUT:
        body_fill(b, bg);
        if (b->st.border_w)
            body_border(b, gui_active_style()->border, bg, b->st.border_w);
        break;
    case BLK_COLUMN:
    case BLK_ROW:
    case BLK_STACK:
    case BLK_FREE:
    case BLK_ROOT:
        if (b->st.bg || b->st.role_bg) body_fill(b, bg);
        break;
    default:
        break;
    }

    /* 组件自绘：在默认绘制之后、本块裁剪框内回调；回调只打脏位。 */
    if (b->render) b->render(b->id);

    /* hover 高亮：可缩放/整块可拖的块，悬停走令牌 accent 描边，不硬编码配色。 */
    block *hov = gui_hover_target();
    if (hov == b && (b->resize_dir || b->drag_mode == DRAG_FULL))
        gui_draw_border(b->x, b->y, b->w, b->h, accent, 1);

    /* 裁剪口径：OVF_VISIBLE 不压自身框（越界仍绘制）；CLIP/SCROLL 压框。
     * SCROLL 额外压平移栈，内容按 scroll 偏移平移，子块几何不动。 */
    int clip_self = (b->overflow != OVF_VISIBLE);
    if (clip_self) paint_clip_push(b->x, b->y, b->w, b->h);
    if (b->overflow == OVF_SCROLL)
        paint_trans_push(b->scroll_x, b->scroll_y);
    for (block *c = b->first_child; c; c = c->next_sibling) {
        /* 逃逸父框：子块自身 VISIBLE / 父块放开裁剪 / 层高于父级浮层。 */
        int escape = c->layer > b->layer ||
                     c->overflow == OVF_VISIBLE ||
                     b->overflow == OVF_VISIBLE;
        if (escape && clip_self) paint_clip_pop();
        paint_node(c);
        if (escape && clip_self) paint_clip_push(b->x, b->y, b->w, b->h);
    }
    if (b->overflow == OVF_SCROLL) paint_trans_pop();
    if (clip_self) paint_clip_pop();
    /* 角外像素由覆盖率合成保留父表面原值，不再用全局桌面底色覆写（漏底根因）。 */

    b->dirty_paint = 0;
    b->snap = 0;
}

/* 平面遍历入口：脏叶子可能挂在未脏的祖先容器下（祖先本帧 snap=0，未走
 * 递归绘制）。此时祖先裁剪框不在栈上，直接 paint_node 会让位图越出父块。
 * 进入前先把祖先链裁剪框压栈，退出时还原；与递归路径同口径。 */
void paint_node_clipped(block *b) {
    if (!b || !b->visible) return;
    int depth0 = clip_depth;
    block *chain[CLIP_DEPTH];
    int n = 0;
    for (block *p = b->parent; p && p->parent && n < CLIP_DEPTH - 1;
         p = p->parent)
        chain[n++] = p;
    for (int k = n - 1; k >= 0; k--) {
        block *p = chain[k];
        if (b->overflow == OVF_VISIBLE || p->overflow == OVF_VISIBLE) continue;
        if (b->layer > p->layer) continue;         /* 浮层逃逸父框 */
        paint_clip_push(p->x, p->y, p->w, p->h);
    }
    paint_node(b);
    while (clip_depth > depth0)
        paint_clip_pop();
}

void paint_begin_frame(void) {
    clip_depth = 0;
}
