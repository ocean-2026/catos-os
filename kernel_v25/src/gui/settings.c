/*
 * settings.c -- 外观设置组件（块子树，规格 §7 组件层）。
 *
 * 左栏分类（本版仅 Appearance）+ 右栏选项区；当前分类高亮。选项只有
 * Style 一组单选：App custom（各块按自身 style 值）/ Calm & soft（舒缓
 * 轻柔：连续曲率圆角、8pt 网格、双层柔和阴影）。
 *
 * 持久化：/etc/style 一行 `style=app|calm`；开机由 gui_style_load() 读取，
 * 运行期切换立即整树标脏重绘。不引入进程 / IPC / 显示服务。
 */
#include "gui/gui.h"
#include "gui_int.h"
#include "gui/tokens.h"
#include "fs.h"
#include "gfx24.h"
#include "serial_log.h"
#include <stdint.h>
#include <stddef.h>
#include "kstring.h"
#include "lang.h"       /* i18n 第一刀：标题/关闭按钮 trs 内联对（面板细分项留待下一刀） */

void st_refresh_options(void);

#define ST_PANEL_W   320
#define ST_PANEL_H   200
#define ST_TITLE_H   24
#define ST_PAD       8
#define ST_PATH      "/etc/style"

static int       st_active;
static block_id  st_root;
static block_id  st_opt[2];
static char      st_opt_txt[2][24];

/* ---- 风格档位持久化 ------------------------------------------------------- */

static int       style_calm;      /* 0 = App custom，1 = Calm & soft */

static int st_contains(const char *s, const char *needle) {
    if (!s || !needle) return 0;
    for (; *s; s++) {
        const char *a = s, *b = needle;
        while (*b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

int gui_style_calm(void) { return style_calm; }

void gui_style_load(void) {
    char buf[64];
    style_calm = 0;
    if (fs_read_text(ST_PATH, buf, sizeof(buf) - 1) > 0) {
        buf[sizeof(buf) - 1] = 0;
        if (st_contains(buf, "calm")) style_calm = 1;
    }
}

static void st_persist(void) {
    fs_mkdir("/etc");   /* 卷上缺目录时兜底；不触碰交付盘镜像本体 */
    if (style_calm) fs_write_text(ST_PATH, "style=calm\n", 12);
    else            fs_write_text(ST_PATH, "style=app\n", 11);
}

void gui_style_set(int calm) {
    style_calm = calm ? 1 : 0;
    st_persist();
    /* 整树标脏重绘 + 重排（网格归整 / 角档 / 阴影都依赖档位）。 */
    gui_mark_dirty(gui_root_block()->id);
    gui_force_next_frame();
    layout_run();
    if (st_active) st_refresh_options();
}

/* ---- 面板规格 ------------------------------------------------------------- */

static blk_spec st_panel(uint16_t layer, int32_t w, int32_t h, uint8_t border) {
    blk_spec s;
    memset(&s, 0, sizeof(s));
    s.req_w = w; s.req_h = h;
    s.layer = layer; s.z = 0;
    s.visible = 1; s.focusable = 0; s.inert = 0;
    s.role_fg = ROLE_FG; s.role_accent = ROLE_ACCENT;
    if (layer == LAYER_CHROME || layer == LAYER_OVERLAY) s.role_bg = ROLE_BAR;
    else s.role_bg = ROLE_PANEL;
    s.fg = INHERIT_COLOR; s.bg = INHERIT_COLOR; s.accent = INHERIT_COLOR;
    s.scale = 1; s.border_w = border;
    return s;
}

static void st_option_label(int i) {
    const char *name = (i == 0) ? "App custom" : "Calm & soft";
    int o = 0;
    while (*name && o < 23) st_opt_txt[i][o++] = *name++;
    st_opt_txt[i][o] = 0;
}

/* 08c：单选真图形——16×16 外环（border 色），选中时中心 8×8 内点（accent）。
 * usr 存选项序号；不再用 ASCII "( )"/"(*)" 占位。 */
static void st_radio_render(block_id id) {
    block *b = btable_get(id);
    if (!b) return;
    const struct style *tok = gui_active_style();
    int idx = (int)(intptr_t)b->usr;
    int sel = (style_calm ? 1 : 0) == idx;
    int x = b->x, y = b->y, w = b->w, h = b->h;
    gui_draw_border(x, y, w, h, tok->border, 1);
    if (sel) {
        int dw = 8, dh = 8;
        int dx = x + (w - dw) / 2, dy = y + (h - dh) / 2;
        gui_fill_rect(dx, dy, dw, dh, tok->accent);
    }
}

void st_refresh_options(void) {
    for (int i = 0; i < 2; i++) {
        st_option_label(i);
        block *b = btable_get(st_opt[i]);
        if (b) { b->text = st_opt_txt[i]; b->dirty_paint = 1; }
    }
}

static void st_opt_click(block_id id, int32_t lx, int32_t ly) {
    (void)lx; (void)ly;
    block *b = btable_get(id);
    if (!b) return;
    gui_style_set(b->z);
}

static void st_close_click(block_id id, int32_t lx, int32_t ly) {
    (void)id; (void)lx; (void)ly;
    gui_settings_close();
}

/* 点击窗口：设置面板无键盘钩子，只 raise 置前并清掉前一组件钩子（不吞键，
 * 也不让背后已切走的组件继续收键）。 */
static void st_root_click(block_id id, int32_t lx, int32_t ly) {
    (void)id; (void)lx; (void)ly;
    if (!st_active) return;
    gui_raise_to_front(st_root);
    gui_set_key_hook(0, 0);
    ser_write("FOCUS:kind=settings idx=0\n");
}

void gui_settings_close(void) {
    if (!st_active) return;
    int rx = 0, ry = 0, rw = 0, rh = 0;
    block *pr = btable_get(st_root);
    if (pr) { rx = pr->x; ry = pr->y; rw = pr->w; rh = pr->h; }
    if (st_root) { b_remove(st_root); st_root = 0; }
    st_opt[0] = st_opt[1] = 0;
    st_active = 0;
    gui_mark_dirty_union(0, rx, ry, rw, rh);
}

void gui_settings_open(void) {
    gui_settings_close();

    block_id root_id = gui_root_block()->id;
    blk_spec root = st_panel(LAYER_WORK, ST_PANEL_W, ST_PANEL_H, 1);
    root.pad_l = ST_PAD; root.pad_r = ST_PAD;
    root.pad_t = ST_PAD; root.pad_b = ST_PAD;
    root.gap = ST_PAD;
    root.focusable = 1;
    root.on_click = st_root_click;
    st_root = b_column(root_id, &root);
    block *pr = btable_get(st_root);
    pr->x = 140; pr->y = 64; pr->moved = 1;
    pr->st.corner_radius = 16;   /* §16.1 外框 r16 */

    /* 标题条：标签 + 关闭块。 */
    blk_spec title = st_panel(LAYER_WORK, ST_PANEL_W - ST_PAD * 2, ST_TITLE_H, 0);
    title.role_bg = ROLE_BAR;
    title.bg = INHERIT_COLOR;
    title.align_cross = XALN_CENTER;
    title.align_main = ALN_SPACE_BETWEEN;   /* 08a：Settings 左、Close 右，分列不叠 */
    block_id bar = b_row(st_root, &title);
    b_text(bar, 0, trs("Settings", "设置"));
    blk_spec close = st_panel(LAYER_WORK, 56, ST_TITLE_H - 4, 1);
    close.on_click = st_close_click;
    block_id cid = b_row(bar, &close);
    b_text(cid, 0, trs("Close", "关闭"));

    /* 主体：左栏分类 + 右栏选项。 */
    blk_spec body = st_panel(LAYER_WORK, ST_PANEL_W - ST_PAD * 2,
                             ST_PANEL_H - ST_TITLE_H - ST_PAD * 2, 0);
    body.gap = ST_PAD;
    block_id body_id = b_row(st_root, &body);

    blk_spec cat = st_panel(LAYER_WORK, 96, 0, 0);
    cat.role_bg = ROLE_BAR; cat.bg = INHERIT_COLOR;
    cat.pad_l = ST_PAD;    /* 08b：左内边距，首字母不被切 */
    block_id cat_id = b_column(body_id, &cat);
    blk_spec cat_row = st_panel(LAYER_WORK, 96, 24, 0);
    cat_row.role_fg = ROLE_ACCENT;
    b_text(cat_id, &cat_row, "Appearance");

    blk_spec opts = st_panel(LAYER_WORK, 0, 0, 0);
    opts.gap = ST_PAD;
    block_id opts_id = b_column(body_id, &opts);
    for (int i = 0; i < 2; i++) {
        st_option_label(i);
        blk_spec row = st_panel(LAYER_WORK, 160, 24, 1);
        row.on_click = st_opt_click;
        row.z = (int32_t)i;
        row.gap = ST_PAD;
        st_opt[i] = b_row(opts_id, &row);

        blk_spec rad = st_panel(LAYER_WORK, 16, 16, 0);
        rad.role_bg = ROLE_NONE; rad.bg = INHERIT_COLOR;
        rad.render = st_radio_render;
        rad.usr = (void *)(intptr_t)i;
        b_rect(st_opt[i], &rad);

        b_text(st_opt[i], 0, st_opt_txt[i]);
    }

    st_active = 1;
    layout_run();
    gui_raise_to_front(st_root);
}

/* 08d：上报根框(i=-1)与选项行(i=0/1) bbox，供行框两两不相交断言。 */
void st_layout_rects(int i, int *x, int *y, int *w, int *h) {
    block *b = (i < 0) ? btable_get(st_root) : btable_get(st_opt[i]);
    if (!b) { if (x) *x = 0; if (y) *y = 0; if (w) *w = 0; if (h) *h = 0; return; }
    if (x) *x = b->x; if (y) *y = b->y; if (w) *w = b->w; if (h) *h = b->h;
}

int gui_settings_active(void) { return st_active; }
