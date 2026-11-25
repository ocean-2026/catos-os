/*
 * sb.c -- cap2 标准滚动条：track + thumb 内核统一绘制 + 滚轮 + 拖 thumb。
 *
 * 可见三态：scrollbar 0=auto（有溢出才画）1=always 2=never。
 * 轨道宽 6px：y 轴贴右侧，x 轴贴下侧，按 scroll_axis 选择。
 * thumb 比例 = 视口 / 内容；滚轮一次 3 行；鼠标 phase 0=press 1=drag 2=release。
 */
#include "gui/block.h"
#include "gui/tokens.h"
#include "gui_int.h"
#include "gfx24.h"

#define SB_W 6
#define SB_LINE 16

static void content_extent(block *b, int *cw, int *ch) {
    int maxx = b->w, maxy = b->h;
    for (block *c = b->first_child; c; c = c->next_sibling) {
        int ex = c->x - b->x + c->w;
        int ey = c->y - b->y + c->h;
        if (ex > maxx) maxx = ex;
        if (ey > maxy) maxy = ey;
    }
    *cw = maxx; *ch = maxy;
}

static int need_x(block *b) {
    int cw, ch; content_extent(b, &cw, &ch);
    if (b->scroll_axis == 1) return 1;
    if (b->scroll_axis == 2) return 0;
    if (b->scroll_axis == 3) return cw > b->w;
    return cw > b->w;
}

static int need_y(block *b) {
    int cw, ch; content_extent(b, &cw, &ch);
    if (b->scroll_axis == 2) return 1;
    if (b->scroll_axis == 1) return 0;
    if (b->scroll_axis == 3) return ch > b->h;
    return ch > b->h;
}

int sb_visible(block *b) {
    if (!b || b->overflow != OVF_SCROLL) return 0;
    if (b->scrollbar == 2) return 0;
    if (b->scrollbar == 1) return 1;
    return need_x(b) || need_y(b);
}

/* ---- thumb 几何 ---------------------------------------------------------- */

static void y_thumb(block *b, int *ty, int *th, int track_h) {
    int cw, ch; content_extent(b, &cw, &ch);
    int view = b->h;
    int ratio = ch > view ? view * 255 / ch : 255;
    int len = track_h * ratio / 255;
    if (len < 16) len = 16;
    if (len > track_h) len = track_h;
    int maxscroll = ch - view;
    int frac = maxscroll > 0 ? b->scroll_y * (track_h - len) / maxscroll : 0;
    *ty = b->y + frac;
    *th = len;
}

static void x_thumb(block *b, int *tx, int *tw, int track_w) {
    int cw, ch; content_extent(b, &cw, &ch);
    int view = b->w;
    int ratio = cw > view ? view * 255 / cw : 255;
    int len = track_w * ratio / 255;
    if (len < 16) len = 16;
    if (len > track_w) len = track_w;
    int maxscroll = cw - view;
    int frac = maxscroll > 0 ? b->scroll_x * (track_w - len) / maxscroll : 0;
    *tx = b->x + frac;
    *tw = len;
}

void sb_paint(block *b) {
    if (!sb_visible(b)) return;
    const struct style *tok = gui_active_style();
    uint32_t track = tok->line ? tok->line : tok->border;
    uint32_t thumb = tok->fg;

    if (need_y(b)) {
        int tx = b->x + b->w - SB_W, ty = b->y;
        gui_fill_rect(tx, ty, SB_W, b->h, track);
        int my, mh; y_thumb(b, &my, &mh, b->h);
        gui_fill_rect(tx, my, SB_W, mh, thumb);
    }
    if (need_x(b)) {
        int tx = b->x, ty = b->y + b->h - SB_W;
        gui_fill_rect(tx, ty, b->w, SB_W, track);
        int mx, mw; x_thumb(b, &mx, &mw, b->w);
        gui_fill_rect(mx, ty, mw, SB_W, thumb);
    }
}

int sb_wheel(block *b, int dy) {
    if (!b || b->overflow != OVF_SCROLL) return 0;
    int cw, ch; content_extent(b, &cw, &ch);
    const struct style *tok = gui_active_style();
    int line = tok->line_h ? tok->line_h : SB_LINE;
    int step = 3 * line;
    int ate = 0;
    if (ch > b->h && b->scroll_axis != 1) {
        int maxs = ch - b->h;
        int ny = b->scroll_y + (dy > 0 ? step : -step);
        if (ny < 0) ny = 0;
        if (ny > maxs) ny = maxs;
        b->scroll_y = (int16_t)ny;
        ate = 1;
    }
    if (cw > b->w && b->scroll_axis != 2) {
        int maxs = cw - b->w;
        int nx = b->scroll_x + (dy > 0 ? step : -step);
        if (nx < 0) nx = 0;
        if (nx > maxs) nx = maxs;
        b->scroll_x = (int16_t)nx;
        ate = 1;
    }
    return ate;
}

/* ---- 拖 thumb 状态 -------------------------------------------------------- */

static block *sb_drag_block;
static uint8_t sb_drag_axis;     /* 0 none 1=x 2=y */
static int     sb_drag_grab;     /* 按下点距 thumb 起点的偏移 */

/* input.c 接线用：当前被拖动的滚动条块（0 = 无拖动态）。 */
block *sb_drag_target(void) { return sb_drag_block; }

/* 会话重建 / 进桌面复位：与 input.c 的拖拽捕获一起归零。 */
void sb_state_reset(void) {
    sb_drag_block = 0;
    sb_drag_axis = 0;
    sb_drag_grab = 0;
}

int sb_mouse(block *b, int x, int y, uint8_t phase) {
    if (!b || b->overflow != OVF_SCROLL || !sb_visible(b)) return 0;

    if (phase == 0) {               /* press */
        int cw, ch; content_extent(b, &cw, &ch);
        (void)cw; (void)ch;
        if (need_y(b)) {
            int tx = b->x + b->w - SB_W;
            if (x >= tx && x < tx + SB_W && y >= b->y && y < b->y + b->h) {
                int my, mh; y_thumb(b, &my, &mh, b->h);
                if (y >= my && y < my + mh) {
                    sb_drag_block = b; sb_drag_axis = 2;
                    sb_drag_grab = y - my;
                }
                return 1;
            }
        }
        if (need_x(b)) {
            int ty = b->y + b->h - SB_W;
            if (y >= ty && y < ty + SB_W && x >= b->x && x < b->x + b->w) {
                int mx, mw; x_thumb(b, &mx, &mw, b->w);
                if (x >= mx && x < mx + mw) {
                    sb_drag_block = b; sb_drag_axis = 1;
                    sb_drag_grab = x - mx;
                }
                return 1;
            }
        }
        return 0;
    }

    if (phase == 2) {               /* release */
        if (sb_drag_block == b) {
            sb_drag_block = 0; sb_drag_axis = 0; sb_drag_grab = 0;
        }
        return 1;
    }

    /* phase 1：drag，thumb 像素跟随。 */
    if (sb_drag_block != b || !sb_drag_axis) return 0;
    int cw, ch; content_extent(b, &cw, &ch);
    if (sb_drag_axis == 2) {
        int my, mh; y_thumb(b, &my, &mh, b->h);
        int top = y - sb_drag_grab;
        int track = b->h;
        int maxs = ch - b->h;
        /* 三种边界不除零 / 不越界：内容高 <= 视口（maxs<=0）、thumb 占满轨道
         * （track<=mh）、拖出轨道两端；无溢出时保持原位 0。 */
        int frac = 0;
        if (maxs > 0 && track > mh)
            frac = (top - b->y) * maxs / (track - mh);
        if (frac < 0) frac = 0;
        if (maxs > 0 && frac > maxs) frac = maxs;
        b->scroll_y = (int16_t)frac;
    } else {
        int mx, mw; x_thumb(b, &mx, &mw, b->w);
        int left = x - sb_drag_grab;
        int track = b->w;
        int maxs = cw - b->w;
        int frac = 0;
        if (maxs > 0 && track > mw)
            frac = (left - b->x) * maxs / (track - mw);
        if (frac < 0) frac = 0;
        if (maxs > 0 && frac > maxs) frac = maxs;
        b->scroll_x = (int16_t)frac;
    }
    return 1;
}
