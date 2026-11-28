/*
 * images.c -- 图片查看器（BLK_CANVAS 承载，规格 §7 组件层）。
 *
 * 最小解码器：BMP BI_RGB 未压缩 24/32bpp（bottom-up / top-down，32 位
 * 丢 alpha 按不透明），PPM P6；RLE / 调色板压缩明确报不支持。画面 1:1
 * 不缩放、居中、超出按裁剪栈裁；方向键平移。缓冲运行时帧分配，不入 .bss。
 * 不新增进程 / IPC；大文件走 fs_read_at 流式读取，不经 fs_entry::data。
 */
#include "gui/gui.h"
#include "gui_int.h"
#include "gui/tokens.h"
#include "fs.h"
#include "fat.h"
#include "gfx24.h"
#include "keyboard.h"
#include "paging.h"
#include "serial_log.h"
#include <stdint.h>
#include <stddef.h>
#include "kstring.h"

/* 毫秒级启动计时（intr.c 提供；本地声明避免拉入中断帧结构）。 */
extern uint32_t uptime_ms(void);

#define VP_W        560
#define VP_H        400
#define IM_MAX_PAGES 400
#define IM_ROWBUF   3204

static char        im_path[FS_PATH_MAX];
static char        im_name[FS_NAME_MAX];
static char        im_status[140];
static int         im_w, im_h;
static int         im_ok;
static int         im_off_x, im_off_y;
static uint32_t    im_dec_us;
static int         im_active;
static int         im_key(int code, void *ud);

static block_id    im_root;
static block_id    im_img_blk;
static block_id    im_status_blk;
static uint8_t    *im_buf;
static uint32_t    im_phys[IM_MAX_PAGES];
static int         im_pages;

/* ---- 小字符串助手 ------------------------------------------------------- */

static int xlen(const char *s) { int n = 0; if (s) while (s[n]) n++; return n; }
static void xcopy(char *d, int cap, const char *s) {
    int n = xlen(s); if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++) d[i] = s[i];
    d[n] = 0;
}
static void xcat(char *d, int cap, const char *s) {
    int o = xlen(d); int n = xlen(s);
    for (int i = 0; i < n && o < cap - 1; i++) d[o++] = s[i];
    d[o] = 0;
}
static void xnum(char *d, int cap, uint32_t v) {
    char tmp[12]; int m = 0;
    if (v == 0) tmp[m++] = '0';
    while (v) { tmp[m++] = (char)('0' + v % 10); v /= 10; }
    int o = xlen(d);
    while (m && o < cap - 1) d[o++] = tmp[--m];
    d[o] = 0;
}

static int im_ext_ok(const char *name) {
    int dot = -1;
    for (int k = 0; k < xlen(name); k++) if (name[k] == '.') dot = k;
    if (dot < 0) return 0;
    const char *e = name + dot;
    if ((e[1] == 'b' || e[1] == 'B') && (e[2] == 'm' || e[2] == 'M') &&
        (e[3] == 'p' || e[3] == 'P') && e[4] == 0) return 1;
    if ((e[1] == 'p' || e[1] == 'P') && (e[2] == 'p' || e[2] == 'P') &&
        (e[3] == 'm' || e[3] == 'M') && e[4] == 0) return 1;
    return 0;
}

/* ---- 帧缓冲 ------------------------------------------------------------- */

static void im_free_buf(void) {
    for (int p = 0; p < im_pages; p++) frame_free(im_phys[p]);
    im_pages = 0; im_buf = 0;
}

static int im_alloc_buf(int w, int h) {
    int bytes = w * h * 3;
    int pages = (bytes + 4095) / 4096;
    if (pages > IM_MAX_PAGES) return -1;
    for (int p = 0; p < pages; p++) {
        uint32_t pa = frame_alloc();
        if (!pa) { im_free_buf(); return -1; }
        im_phys[p] = pa;
    }
    im_pages = pages;
    im_buf = (uint8_t *)im_phys[0];
    memset(im_buf, 0, bytes);
    return 0;
}

/* ---- BMP / PPM 解码 ------------------------------------------------------ */

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static int32_t rd_i32(const uint8_t *p) {
    return (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

static int im_decode_ppm(const char *path) {
    char hdr[256];
    int n = fs_read_at(path, hdr, 0, sizeof(hdr) - 1);
    if (n < 5) return -1;
    hdr[n] = 0;
    if (hdr[0] != 'P' || hdr[1] != '6') return -1;

    int p = 2;
    int vals[3] = { 0, 0, 0 };
    for (int v = 0; v < 3; v++) {
        while (hdr[p] == ' ' || hdr[p] == '\t' || hdr[p] == '\n' || hdr[p] == '\r' || hdr[p] == '#') {
            if (hdr[p] == '#') while (hdr[p] && hdr[p] != '\n') p++;
            else p++;
        }
        while (hdr[p] >= '0' && hdr[p] <= '9') {
            vals[v] = vals[v] * 10 + (hdr[p] - '0'); p++;
        }
    }
    int w = vals[0], h = vals[1];
    if (w <= 0 || h <= 0 || w > 800 || h > 600) return -1;
    int data_off = p;

    if (im_alloc_buf(w, h) < 0) return -1;
    static uint8_t row[3204];
    for (int y = 0; y < h; y++) {
        int got = fs_read_at(path, (char *)row, data_off + y * w * 3, w * 3);
        if (got < w * 3) return -1;
        for (int x = 0; x < w; x++) {
            uint8_t *d = im_buf + (y * w + x) * 3;
            d[0] = row[x * 3]; d[1] = row[x * 3 + 1]; d[2] = row[x * 3 + 2];
        }
    }
    im_w = w; im_h = h;
    return 0;
}

static int im_decode_bmp(const char *path) {
    uint8_t hdr[64];
    int n = fs_read_at(path, (char *)hdr, 0, sizeof(hdr));
    if (n < 34) return -1;
    if (hdr[0] != 'B' || hdr[1] != 'M') return -1;
    uint32_t data_off = rd_u16(hdr + 10);
    int32_t info = rd_i32(hdr + 14);
    if (info < 40) return -1;
    int32_t wh = rd_i32(hdr + 22);
    int32_t w = rd_i32(hdr + 18);
    int bpp = rd_u16(hdr + 28);
    uint32_t comp = (uint32_t)rd_i32(hdr + 30);
    if (comp != 0) return -1;                       /* RLE / 压缩：不支持 */
    if (bpp != 24 && bpp != 32) return -1;          /* 调色板：不支持 */
    if (w <= 0 || w > 800) return -1;
    int topdown = wh < 0;
    int h = topdown ? -wh : wh;
    if (h <= 0 || h > 600) return -1;

    int bpc = bpp / 8;
    int stride = ((w * bpc + 3) & ~3);
    if (stride > IM_ROWBUF) return -1;

    if (im_alloc_buf(w, h) < 0) return -1;
    static uint8_t row[IM_ROWBUF];
    for (int y = 0; y < h; y++) {
        int got = fs_read_at(path, (char *)row, data_off + y * stride, stride);
        if (got < bpc * w) return -1;
        int dst = topdown ? y : (h - 1 - y);
        for (int x = 0; x < w; x++) {
            uint8_t B = row[x * bpc];
            uint8_t G = row[x * bpc + 1];
            uint8_t R = row[x * bpc + 2];
            uint8_t *d = im_buf + (dst * w + x) * 3;
            d[0] = R; d[1] = G; d[2] = B;
        }
    }
    im_w = w; im_h = h;
    return 0;
}

static void im_pick_default(char *out, int cap) {
    char names[32][FS_NAME_MAX];
    int nn = fs_enum_entries("/samples", names, 32);
    for (int i = 0; i < nn; i++) {
        if (im_ext_ok(names[i])) {
            xcopy(out, cap, "/samples/");
            xcat(out, cap, names[i]);
            return;
        }
    }
    xcopy(out, cap, "/samples/desktop.bmp");
}

static void im_update_status(void) {
    if (!im_ok) {
        xcopy(im_status, sizeof(im_status), "unsupported format");
    } else {
        xcopy(im_status, sizeof(im_status), im_name);
        xcat(im_status, sizeof(im_status), "  ");
        xnum(im_status, sizeof(im_status), (uint32_t)im_w);
        xcat(im_status, sizeof(im_status), "x");
        xnum(im_status, sizeof(im_status), (uint32_t)im_h);
        xcat(im_status, sizeof(im_status), "  off=");
        xnum(im_status, sizeof(im_status), (uint32_t)im_off_x);
        xcat(im_status, sizeof(im_status), ",");
        xnum(im_status, sizeof(im_status), (uint32_t)im_off_y);
        xcat(im_status, sizeof(im_status), "  decode=");
        xnum(im_status, sizeof(im_status), im_dec_us);
        xcat(im_status, sizeof(im_status), "us");
    }
    block *st = btable_get(im_status_blk);
    if (st) { st->text = im_status; st->dirty_paint = 1; }
}

static blk_spec im_panel(uint16_t layer, int32_t w, int32_t h, uint8_t border) {
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

/* 点击窗口任意位置：raise 置前 + 切回图片查看器自己的键盘钩子。 */
static void im_root_click(block_id id, int32_t lx, int32_t ly) {
    (void)id; (void)lx; (void)ly;
    if (!im_active) return;
    gui_raise_to_front(im_root);
    gui_set_key_hook(im_key, 0);
    ser_write("FOCUS:kind=images idx=0\n");
}

void gui_images_close(void) {
    if (!im_active) return;
    gui_set_key_hook(0, 0);
    int rx = 0, ry = 0, rw = 0, rh = 0;
    block *pr = btable_get(im_root);
    if (pr) { rx = pr->x; ry = pr->y; rw = pr->w; rh = pr->h; }
    if (im_root) { b_remove(im_root); im_root = 0; }
    im_img_blk = im_status_blk = 0;
    im_free_buf();
    im_active = 0;
    gui_mark_dirty_union(0, rx, ry, rw, rh);
}

static int im_key(int code, void *ud) {
    (void)ud;
    if (!im_active) return 0;
    if (code == 27) { gui_images_close(); return 1; }
    if (!im_ok) return 1;
    switch (code) {
    case KB_LEFT:  im_off_x -= 8; break;
    case KB_RIGHT: im_off_x += 8; break;
    case KB_UP:    im_off_y -= 8; break;
    case KB_DOWN:  im_off_y += 8; break;
    default: return 0;
    }
    block *b = btable_get(im_img_blk);
    if (b) {
        b->x = 152 + im_off_x;
        b->y = 72 + im_off_y;
        b->dirty_paint = 1;
    }
    im_update_status();
    return 1;
}

void gui_images_open(const char *path) {
    gui_images_close();
    if (!path || !*path) im_pick_default(im_path, sizeof(im_path));
    else                 xcopy(im_path, sizeof(im_path), path);

    uint32_t t0 = uptime_ms();
    int rc = im_decode_bmp(im_path);
    if (rc < 0) rc = im_decode_ppm(im_path);
    uint32_t t1 = uptime_ms();
    im_dec_us = (t1 > t0 ? t1 - t0 : 0) * 1000u;
    im_ok = (rc == 0);

    const char *slash = im_path;
    for (const char *q = im_path; *q; q++) if (*q == '/') slash = q + 1;
    xcopy(im_name, sizeof(im_name), slash);

    block_id root_id = gui_root_block()->id;
    blk_spec root = im_panel(LAYER_WORK, VP_W + 16, VP_H + 56, 1);
    root.pad_l = 8; root.pad_r = 8; root.pad_t = 8; root.pad_b = 8;
    root.gap = 4;
    root.focusable = 1;
    root.on_click = im_root_click;
    im_root = b_column(root_id, &root);
    block *pr = btable_get(im_root);
    pr->x = 112; pr->y = 64; pr->moved = 1;

    blk_spec title = im_panel(LAYER_WORK, VP_W, 16, 0);
    title.role_bg = ROLE_BAR; title.bg = INHERIT_COLOR;
    b_text(im_root, &title, "Images");

    /* 视口块：缓冲为解码图，1:1 blit，超出视口由裁剪栈裁掉。 */
    block *b = btable_alloc(BLK_CANVAS);
    if (b) {
        tree_attach(btable_get(im_root), b);
        b->req_w = VP_W; b->req_h = VP_H;
        b->pix_w = im_ok ? im_w : 0;
        b->pix_h = im_ok ? im_h : 0;
        b->buf = im_buf;
        b->layer = LAYER_WORK; b->visible = 1;
        b->dirty_layout = 1;
        b->x = 152; b->y = 72;
        im_img_blk = b->id;
    }

    blk_spec st = im_panel(LAYER_WORK, VP_W, 16, 0);
    im_status_blk = b_text(im_root, &st, im_status);

    im_off_x = im_ok ? (VP_W - im_w) / 2 : 0;
    im_off_y = im_ok ? (VP_H - im_h) / 2 : 0;
    if (b) { b->x = 152 + im_off_x; b->y = 72 + im_off_y; }
    im_update_status();

    im_active = 1;
    gui_set_key_hook(im_key, 0);
    layout_run();
    gui_raise_to_front(im_root);
}

int       gui_images_active(void) { return im_active; }
int       gui_images_ok(void)     { return im_ok; }
int       gui_images_w(void)      { return im_w; }
int       gui_images_h(void)      { return im_h; }
const char *gui_images_name(void) { return im_name; }
