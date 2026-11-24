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
    if (!clip_rect(&x, &y, &w, &h)) return;
    gfx24_rect(x, y, w, h, rgb);
}

void gui_blit24(const uint8_t *src, int x, int y, int w, int h) {
    if (!src || w <= 0 || h <= 0) return;
    int sw = w, sh = h, ox = x, oy = y;
    if (!clip_rect(&x, &y, &w, &h)) return;
    /* 裁剪后源指针按原始步进（sw 列）偏移到被保留的子矩形起点。 */
    const uint8_t *s = src +
        ((uint32_t)(y - oy) * (uint32_t)sw + (uint32_t)(x - ox)) * 3u;
    gfx24_blit_stride(s, x, y, w, h, sw);
}

void gui_draw_text(int x, int y, const char *txt, uint32_t fg, uint8_t scale) {
    if (!txt) return;
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

void paint_node(block *b) {
    if (!b || !b->visible) return;

    uint32_t fg = resolve_color(b, 0);
    uint32_t bg = resolve_color(b, 1);
    uint32_t accent = resolve_color(b, 2);
    (void)accent;   /* v1 尚未消费强调色，保留解析链路 */

    switch (b->kind) {
    case BLK_RECT:
        gui_fill_rect(b->x, b->y, b->w, b->h, bg);
        if (b->st.border_w)
            gui_draw_border(b->x, b->y, b->w, b->h,
                            gui_active_style()->border, b->st.border_w);
        break;
    case BLK_TEXT: {
        char buf[160];
        text_trimmed(b->text, b->w, b->st.scale, buf, sizeof(buf));
        if (bg) gui_fill_rect(b->x, b->y, b->w, b->h, bg);
        gui_draw_text(b->x, b->y, buf, fg, b->st.scale);
        break;
    }
    case BLK_IMAGE:
    case BLK_SURFACE:
        gui_blit24(b->pix, b->x, b->y, b->pix_w, b->pix_h);
        break;
    case BLK_CANVAS:
        gui_blit24(b->buf, b->x, b->y, b->pix_w, b->pix_h);
        break;
    case BLK_DIVIDER:
        gui_fill_rect(b->x, b->y, b->w, 1, gui_active_style()->line);
        break;
    case BLK_BUTTON: {
        uint32_t bgc = bg, fgc = fg;
        if (b->focused) { fgc = bg; bgc = fg; }   /* 按压/聚焦反色反馈 */
        gui_fill_rect(b->x, b->y, b->w, b->h, bgc);
        if (b->st.border_w)
            gui_draw_border(b->x, b->y, b->w, b->h,
                            gui_active_style()->border, b->st.border_w);
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
        gui_fill_rect(b->x, b->y, b->w, b->h, bg);
        if (b->st.border_w)
            gui_draw_border(b->x, b->y, b->w, b->h,
                            gui_active_style()->border, b->st.border_w);
        break;
    case BLK_COLUMN:
    case BLK_ROW:
    case BLK_STACK:
    case BLK_ROOT:
        if (b->st.bg || b->st.role_bg) gui_fill_rect(b->x, b->y, b->w, b->h, bg);
        break;
    default:
        break;
    }

    /* 组件自绘：在默认绘制之后、本块裁剪框内回调；回调只打脏位。 */
    if (b->render) b->render(b->id);

    paint_clip_push(b->x, b->y, b->w, b->h);
    for (block *c = b->first_child; c; c = c->next_sibling)
        paint_node(c);
    paint_clip_pop();

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
    for (int k = n - 1; k >= 0; k--)
        paint_clip_push(chain[k]->x, chain[k]->y, chain[k]->w, chain[k]->h);
    paint_node(b);
    while (clip_depth > depth0)
        paint_clip_pop();
}

void paint_begin_frame(void) {
    clip_depth = 0;
}
