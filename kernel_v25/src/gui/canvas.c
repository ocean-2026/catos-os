/*
 * canvas.c -- BLK_CANVAS 兼容层（规格 §7 组件层 / §7 像素缓冲）。
 *
 * 一块 24bpp 不透明缓冲，运行时帧分配（不入 .bss 大数组），销毁归还。
 * 原语全部经块内裁剪；内容变化只标自身 dirty_paint，按 damage 只 blit
 * 脏区；不缩放、不 alpha、不圆角。未来第三方渲染路径按本形状接入。
 */
#include "gui/gui.h"
#include "gui_int.h"
#include "gui/tokens.h"
#include "gfx24.h"
#include "paging.h"
#include <stdint.h>
#include <stddef.h>
#include "kstring.h"

#define CV_MAX_PAGES 64
#define CV_DEMO_W    200
#define CV_DEMO_H    160

struct cv_alloc {
    uint32_t *phys;               /* PFN 表：帧分配一页，不入 .bss 静态数组 */
    int      pages;
    int      w, h;
    int      dx, dy, dw, dh;      /* 脏子矩形；dw=0 = 无脏区 */
};

static struct cv_alloc cv_pool[2];
static int             cv_root;            /* 演示组件根块 id */
static int             cv_active;

/* ---- 帧缓冲分配 / 归还 -------------------------------------------------- */

static struct cv_alloc *cv_alloc_record(int w, int h) {
    int bytes = w * h * 3;
    int pages = (bytes + 4095) / 4096;
    if (pages > CV_MAX_PAGES) return 0;
    for (int i = 0; i < 2; i++) {
        if (cv_pool[i].pages == 0 && !cv_pool[i].phys) {
            uint32_t ppt = frame_alloc();
            if (!ppt) return 0;
            uint32_t *phys = (uint32_t *)ppt;
            for (int p = 0; p < pages; p++) {
                uint32_t pa = frame_alloc();
                if (!pa) {
                    for (int q = 0; q < p; q++) frame_free(phys[q]);
                    frame_free(ppt);
                    return 0;
                }
                phys[p] = pa;
            }
            cv_pool[i].phys = phys;
            cv_pool[i].pages = pages;
            cv_pool[i].w = w; cv_pool[i].h = h;
            cv_pool[i].dx = 0; cv_pool[i].dy = 0;
            cv_pool[i].dw = w; cv_pool[i].dh = h;   /* 首帧整幅脏 */
            return &cv_pool[i];
        }
    }
    return 0;
}

static void cv_free_record(struct cv_alloc *r) {
    if (!r) return;
    for (int p = 0; p < r->pages; p++) frame_free(r->phys[p]);
    if (r->phys) frame_free((uint32_t)r->phys);
    memset(r, 0, sizeof(*r));
}

/* 脏子矩形并集（裁剪到缓冲内）；绘后由合成层清位。 */
static void cv_union_rect(struct cv_alloc *r, int x, int y, int w, int h) {
    if (!r || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > r->w) w = r->w - x;
    if (y + h > r->h) h = r->h - y;
    if (w <= 0 || h <= 0) return;
    if (r->dw == 0) {
        r->dx = x; r->dy = y; r->dw = w; r->dh = h;
    } else {
        int x0 = r->dx < x ? r->dx : x;
        int y0 = r->dy < y ? r->dy : y;
        int x1 = r->dx + r->dw > x + w ? r->dx + r->dw : x + w;
        int y1 = r->dy + r->dh > y + h ? r->dy + r->dh : y + h;
        r->dx = x0; r->dy = y0; r->dw = x1 - x0; r->dh = y1 - y0;
    }
}

/* ---- 建块 --------------------------------------------------------------- */

block_id b_canvas(block_id parent, const blk_spec *s, int w, int h) {
    blk_spec local;
    if (s) local = *s; else memset(&local, 0, sizeof(local));
    local.req_w = w; local.req_h = h;
    local.layer = local.layer ? local.layer : LAYER_WORK;
    local.visible = 1;
    struct cv_alloc *r = cv_alloc_record(w, h);
    if (!r) return 0;
    block *b = btable_alloc(BLK_CANVAS);
    if (!b) { cv_free_record(r); return 0; }
    block *p = btable_get(parent);
    /* 直接落表：attach 到父块（根块兜底）。 */
    tree_attach(p ? p : gui_root_block(), b);
    if (s) {
        b->layer = s->layer; b->z = s->z; b->visible = s->visible;
        b->focusable = s->focusable; b->inert = s->inert;
        b->on_click = s->on_click; b->on_drag = s->on_drag;
        b->render = s->render;
    }
    b->req_w = w; b->req_h = h;
    b->pix_w = w; b->pix_h = h;
    b->buf = (uint8_t *)r->phys[0];
    b->usr = r;
    b->dirty_layout = 1;
    memset(b->buf, 0, w * h * 3);
    return b->id;
}

uint8_t *gui_canvas_buffer(block_id id) {
    block *b = btable_get(id);
    return b ? b->buf : 0;
}

/* ---- 原语（块内裁剪） ---------------------------------------------------- */

static int cv_clip(block *b, int *x, int *y) {
    if (*x < 0 || *y < 0 || *x >= b->pix_w || *y >= b->pix_h) return 0;
    return 1;
}

void cv_pixel(block_id id, int x, int y, uint32_t rgb) {
    block *b = btable_get(id);
    if (!b || !b->buf) return;
    if (!cv_clip(b, &x, &y)) return;
    uint8_t *p = b->buf + (y * b->pix_w + x) * 3;
    p[0] = (uint8_t)(rgb >> 16);
    p[1] = (uint8_t)(rgb >> 8);
    p[2] = (uint8_t)(rgb);
    cv_union_rect(b->usr, x, y, 1, 1);
    b->dirty_paint = 1;
}

void cv_hline(block_id id, int x0, int x1, int y, uint32_t rgb) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    for (int x = x0; x <= x1; x++) cv_pixel(id, x, y, rgb);
}

void cv_vline(block_id id, int x, int y0, int y1, uint32_t rgb) {
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++) cv_pixel(id, x, y, rgb);
}

void cv_fill_rect(block_id id, int x, int y, int w, int h, uint32_t rgb) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) cv_pixel(id, xx, yy, rgb);
}

void cv_blit24(block_id id, const uint8_t *src, int x, int y,
               int w, int h, int src_stride) {
    block *b = btable_get(id);
    if (!b || !b->buf || !src) return;
    for (int yy = 0; yy < h; yy++) {
        int dy = y + yy;
        if (dy < 0 || dy >= b->pix_h) continue;
        for (int xx = 0; xx < w; xx++) {
            int dx = x + xx;
            if (dx < 0 || dx >= b->pix_w) continue;
            const uint8_t *s = src + yy * src_stride + xx * 3;
            uint8_t *d = b->buf + (dy * b->pix_w + dx) * 3;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
        }
    }
    cv_union_rect(b->usr, x, y, w, h);
    b->dirty_paint = 1;
}

/* 子矩形标脏：只重 blit 该子矩形；绘后由合成层清位。 */
void cv_dirty(block_id id, int x, int y, int w, int h) {
    block *b = btable_get(id);
    if (!b || !b->usr) return;
    cv_union_rect(b->usr, x, y, w, h);
    b->dirty_paint = 1;
}

int cv_dirty_rect(block *b, int *rx, int *ry, int *rw, int *rh) {
    if (!b || !b->usr) return 0;
    struct cv_alloc *r = b->usr;
    if (r->dw <= 0) return 0;
    *rx = r->dx; *ry = r->dy; *rw = r->dw; *rh = r->dh;
    return 1;
}

void cv_dirty_clear(block *b) {
    if (!b || !b->usr) return;
    struct cv_alloc *r = b->usr;
    r->dw = 0; r->dh = 0;
}

/* ---- 自测演示：棋盘 + 渐变 + 移动小方块 ---------------------------------- */

static void cv_demo_paint(block *b) {
    uint8_t *buf = b->buf;
    int W = b->pix_w, H = b->pix_h;
    /* 底色深灰。 */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t *p = buf + (y * W + x) * 3;
            p[0] = 30; p[1] = 30; p[2] = 38;
        }
    /* 棋盘：左上 64×64，8px 格。 */
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            int on = ((x / 8) ^ (y / 8)) & 1;
            uint8_t *p = buf + (y * W + x) * 3;
            if (on) { p[0] = 232; p[1] = 232; p[2] = 236; }
            else    { p[0] = 24;  p[1] = 24;  p[2] = 30;  }
        }
    /* 水平渐变：右半段按 x 从蓝到青。 */
    for (int y = 0; y < H; y++)
        for (int x = 72; x < W; x++) {
            uint8_t *p = buf + (y * W + x) * 3;
            int t = (x - 72) * 255 / (W - 72);
            p[0] = 40; p[1] = (uint8_t)t; p[2] = 255;
        }
    /* 移动小方块：位置随帧计数推进。 */
    uint32_t fr = stat_frames();
    int sx = 72 + (int)((fr * 3) % (W - 24));
    cv_fill_rect(b->id, sx, 80, 16, 16, 0x4C8DFF);
}

static void cv_demo_render(block_id id) {
    block *b = btable_get(id);
    if (!b) return;
    cv_demo_paint(b);
}

static blk_spec cv_panel(uint16_t layer, int32_t w, int32_t h, uint8_t border) {
    blk_spec s;
    memset(&s, 0, sizeof(s));
    s.req_w = w; s.req_h = h;
    s.layer = layer; s.z = 0;
    s.visible = 1; s.focusable = 0; s.inert = 0;
    s.role_fg = ROLE_FG; s.role_accent = ROLE_ACCENT;
    s.role_bg = ROLE_PANEL;
    s.fg = INHERIT_COLOR; s.bg = INHERIT_COLOR; s.accent = INHERIT_COLOR;
    s.scale = 1; s.border_w = border;
    return s;
}

void gui_canvas_close_all(void) {
    if (cv_root) { b_remove(cv_root); cv_root = 0; }
    for (int i = 0; i < 2; i++) cv_free_record(&cv_pool[i]);
    cv_active = 0;
}

void gui_canvas_demo(void) {
    gui_canvas_close_all();
    block_id root_id = gui_root_block()->id;
    blk_spec root = cv_panel(LAYER_WORK, CV_DEMO_W + 16, CV_DEMO_H + 40, 1);
    root.pad_l = 8; root.pad_r = 8; root.pad_t = 8; root.pad_b = 8;
    root.focusable = 1;
    cv_root = b_column(root_id, &root);
    block *pr = btable_get(cv_root);
    pr->x = 150; pr->y = 80; pr->moved = 1;

    blk_spec title = cv_panel(LAYER_WORK, CV_DEMO_W, 16, 0);
    b_text(cv_root, &title, "Canvas demo");

    blk_spec cv = cv_panel(LAYER_WORK, CV_DEMO_W, CV_DEMO_H, 0);
    cv.render = cv_demo_render;
    block_id cid = b_canvas(cv_root, &cv, CV_DEMO_W, CV_DEMO_H);
    /* 首帧 blit 前先把演示内容写入缓冲（回调在默认 blit 之后才执行，
     * 惰性填充会让第一帧只看到清空的黑缓冲）。 */
    block *cb = btable_get(cid);
    if (cb) cv_demo_paint(cb);
    cv_active = 1;
    layout_run();
    gui_mark_dirty(cv_root);
}

int  gui_canvas_active(void) { return cv_active; }
int  gui_canvas_w(void)     { return CV_DEMO_W; }
int  gui_canvas_h(void)      { return CV_DEMO_H; }
