/*
 * filemgr.c -- 资源管理器组件（块子树，规格 §7 组件层）。
 *
 * 路径栏块 + 工具栏块（上级 / 刷新）+ 列表块（每行一个块）。数据来自
 * fs_enum_entries / fs_enum_entries_raw；进入子目录、返回上级、滚动、
 * 单击选中、双击/回车打开。文本文件交记事本组件，目录进入，其他类型
 * 提示不支持。不新增进程或 IPC。
 */
#include "gui/gui.h"
#include "gui_int.h"
#include "gui/tokens.h"
#include "fs.h"
#include "fat.h"
#include "gfx24.h"
#include "keyboard.h"
#include <stdint.h>
#include <stddef.h>
#include "kstring.h"

#define FM_MAX_ENT   64
#define FM_ROWS_VIEW 14
#define FM_BODY_W    504
#define FM_BODY_H    280
#define FM_ROW_H     18

static char              fm_cwd[FS_PATH_MAX];
static char              fm_names[FM_MAX_ENT][FS_NAME_MAX];
static struct fs_raw_ent fm_raw[FM_MAX_ENT];
static int               fm_n;
static int               fm_sel;
static int               fm_scroll;
static int               fm_active;
static char              fm_status[120];
static char              fm_labels[FM_MAX_ENT][FS_NAME_MAX + 16];

static block_id          fm_root;
static block_id          fm_pathbar;
static block_id          fm_list;
static block_id          fm_status_blk;
static block_id          fm_rows[FM_MAX_ENT];

static void fm_row_click(block_id id, int32_t lx, int32_t ly);
static blk_spec cv_panel_local(uint16_t layer, int32_t w, int32_t h, uint8_t border);

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

static void fm_join(const char *dir, const char *name, char *out, int cap) {
    xcopy(out, cap, dir);
    int o = xlen(out);
    if (o == 0 || out[o - 1] != '/') { out[o] = '/'; out[o + 1] = 0; }
    xcat(out, cap, name);
}

static int fm_is_dir(int i) {
    return (fm_raw[i].attr & FAT_ATTR_DIR) != 0;
}

static void fm_set_status(const char *msg) {
    xcopy(fm_status, sizeof(fm_status), msg);
    block *st = btable_get(fm_status_blk);
    if (st) { st->text = fm_status; st->dirty_paint = 1; }
}

/* ---- 列表重建 ------------------------------------------------------------ */

static void fm_clear_rows(void) {
    for (int i = 0; i < FM_MAX_ENT; i++) {
        if (fm_rows[i]) { b_remove(fm_rows[i]); fm_rows[i] = 0; }
    }
}

static void fm_row_label(int i) {
    char *lb = fm_labels[i];
    int o = 0;
    lb[o++] = (i == fm_sel) ? '>' : ' ';
    const char *nm = fm_names[i];
    int n = xlen(nm);
    for (int k = 0; k < n && o < FS_NAME_MAX + 10; k++) lb[o++] = nm[k];
    lb[o] = 0;
    if (!fm_is_dir(i)) {
        char num[12]; int m = 0;
        uint32_t v = fm_raw[i].size;
        if (v == 0) num[m++] = '0';
        while (v) { num[m++] = (char)('0' + v % 10); v /= 10; }
        xcat(lb, FS_NAME_MAX + 16, "  ");
        while (m) { char one[2] = { num[--m], 0 }; xcat(lb, FS_NAME_MAX + 16, one); }
    }
}

static void fm_refresh(void) {
    fm_clear_rows();
    int hidden = 0;
    fm_n = fs_enum_entries_raw(fm_cwd, fm_raw, FM_MAX_ENT, &hidden);
    if (fm_n < 0) { fm_n = 0; fm_set_status("read failed"); return; }
    /* 名字表与 raw 表按同一序配对；目录名补 '/' 后缀。 */
    char names[FM_MAX_ENT][FS_NAME_MAX];
    int nn = fs_enum_entries(fm_cwd, names, FM_MAX_ENT);
    for (int i = 0; i < fm_n; i++) {
        if (i < nn) xcopy(fm_names[i], FS_NAME_MAX, names[i]);
        else        xcopy(fm_names[i], FS_NAME_MAX, fm_raw[i].name);
    }
    fm_sel = 0; fm_scroll = 0;
    for (int i = 0; i < fm_n; i++) fm_row_label(i);

    if (fm_n == 0) fm_set_status("empty");
    else           fm_set_status("ready");

    for (int v = 0; v < FM_ROWS_VIEW && v < fm_n; v++) {
        int i = fm_scroll + v;
        blk_spec row = cv_panel_local(LAYER_WORK, FM_BODY_W, FM_ROW_H, 0);
        row.on_click = fm_row_click;
        row.z = i;
        block_id rid = b_row(fm_list, &row);
        fm_rows[i] = rid;
        b_text(rid, 0, fm_labels[i]);
    }
    layout_run();
    if (fm_root) gui_mark_dirty(fm_root);
}

/* ---- 打开条目 ------------------------------------------------------------ */

static void fm_open_entry(int i) {
    if (i < 0 || i >= fm_n) return;
    char path[FS_PATH_MAX];
    char nm[FS_NAME_MAX];
    xcopy(nm, sizeof(nm), fm_names[i]);
    int n = xlen(nm);
    if (n && nm[n - 1] == '/') nm[n - 1] = 0;

    if (fm_is_dir(i)) {
        fm_join(fm_cwd, nm, path, sizeof(path));
        xcopy(fm_cwd, sizeof(fm_cwd), path);
        fm_refresh();
        return;
    }

    /* 文本文件交记事本；其他类型提示不支持。 */
    int dot = -1;
    for (int k = 0; k < xlen(nm); k++) if (nm[k] == '.') dot = k;
    int is_txt = 0;
    if (dot >= 0) {
        const char *e = nm + dot;
        if ((e[1] == 't' || e[1] == 'T') &&
            (e[2] == 'x' || e[2] == 'X') &&
            (e[3] == 't' || e[3] == 'T') && e[4] == 0)
            is_txt = 1;
    }
    fm_join(fm_cwd, nm, path, sizeof(path));
    if (is_txt) {
        gui_notepad_open(path);
        fm_active = 0;   /* 焦点转入记事本 */
    } else {
        fm_set_status("unsupported type");
    }
}

static void fm_open_parent(void) {
    if (xlen(fm_cwd) <= 1) return;
    char p[FS_PATH_MAX];
    xcopy(p, sizeof(p), fm_cwd);
    int o = xlen(p);
    while (o > 1 && p[o - 1] == '/') p[--o] = 0;
    int slash = o;
    while (slash > 0 && p[slash - 1] != '/') slash--;
    if (slash <= 1) { p[0] = '/'; p[1] = 0; }
    else p[slash] = 0;
    xcopy(fm_cwd, sizeof(fm_cwd), p);
    fm_refresh();
}

/* ---- 交互回调 ------------------------------------------------------------ */

static void fm_row_click(block_id id, int32_t lx, int32_t ly) {
    (void)lx; (void)ly;
    block *row = btable_get(id);
    if (!row) return;
    int i = (int)row->z;
    if (i < 0 || i >= fm_n) return;
    fm_sel = i;
    for (int k = 0; k < fm_n; k++) fm_row_label(k);
    for (int k = 0; k < fm_n; k++) {
        block *rb = btable_get(fm_rows[k]);
        if (rb) { rb->text = fm_labels[k]; rb->dirty_paint = 1; }
    }
}

static void fm_tool_parent(block_id id, int32_t lx, int32_t ly) {
    (void)id; (void)lx; (void)ly;
    fm_open_parent();
}

static void fm_tool_refresh(block_id id, int32_t lx, int32_t ly) {
    (void)id; (void)lx; (void)ly;
    fm_refresh();
}

static blk_spec cv_panel_local(uint16_t layer, int32_t w, int32_t h, uint8_t border) {
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

static void fm_close_click(block_id id, int32_t lx, int32_t ly) {
    (void)id; (void)lx; (void)ly;
    gui_filemgr_close();
}

void gui_filemgr_close(void) {
    if (!fm_active) return;
    gui_set_key_hook(0, 0);
    fm_clear_rows();
    int rx = 0, ry = 0, rw = 0, rh = 0;
    block *pr = btable_get(fm_root);
    if (pr) { rx = pr->x; ry = pr->y; rw = pr->w; rh = pr->h; }
    if (fm_root) { b_remove(fm_root); fm_root = 0; }
    fm_pathbar = fm_list = fm_status_blk = 0;
    fm_active = 0;
    gui_mark_dirty_union(0, rx, ry, rw, rh);
}

static int fm_key(int code, void *ud) {
    (void)ud;
    if (!fm_active) return 0;
    if (code == 27) { gui_filemgr_close(); return 1; }
    if (fm_n == 0) return 1;
    switch (code) {
    case KB_UP:
        if (fm_sel > 0) fm_sel--;
        if (fm_sel < fm_scroll) fm_scroll = fm_sel;
        break;
    case KB_DOWN:
        if (fm_sel < fm_n - 1) fm_sel++;
        if (fm_sel >= fm_scroll + FM_ROWS_VIEW) fm_scroll = fm_sel - FM_ROWS_VIEW + 1;
        break;
    case 10: fm_open_entry(fm_sel); return 1;
    default: return 0;
    }
    for (int k = 0; k < fm_n; k++) fm_row_label(k);
    for (int k = 0; k < fm_n; k++) {
        block *rb = btable_get(fm_rows[k]);
        if (rb) { rb->text = fm_labels[k]; rb->dirty_paint = 1; }
    }
    return 1;
}

void gui_filemgr_open(const char *path) {
    gui_filemgr_close();
    xcopy(fm_cwd, sizeof(fm_cwd), path ? path : "/");
    if (xlen(fm_cwd) == 0) xcopy(fm_cwd, sizeof(fm_cwd), "/");

    block_id root_id = gui_root_block()->id;
    blk_spec root = cv_panel_local(LAYER_WORK, FM_BODY_W + 16, FM_BODY_H + 64, 1);
    root.pad_l = 8; root.pad_r = 8; root.pad_t = 8; root.pad_b = 8;
    root.gap = 4;
    root.focusable = 1;
    fm_root = b_column(root_id, &root);
    block *pr = btable_get(fm_root);
    pr->x = 140; pr->y = 64; pr->moved = 1;

    blk_spec title = cv_panel_local(LAYER_WORK, FM_BODY_W, 16, 0);
    title.role_bg = ROLE_BAR; title.bg = INHERIT_COLOR;
    block_id bar = b_row(fm_root, &title);
    b_text(bar, 0, "Files");
    blk_spec close = cv_panel_local(LAYER_WORK, 56, 14, 1);
    close.on_click = fm_close_click;
    block_id cid = b_row(bar, &close);
    b_text(cid, 0, "Close");

    blk_spec pb = cv_panel_local(LAYER_WORK, FM_BODY_W, 16, 0);
    fm_pathbar = b_text(fm_root, &pb, fm_cwd);

    blk_spec tb = cv_panel_local(LAYER_WORK, FM_BODY_W, 20, 0);
    block_id tools = b_row(fm_root, &tb);
    blk_spec up = cv_panel_local(LAYER_WORK, 64, 18, 1);
    up.on_click = fm_tool_parent;
    block_id upid = b_row(tools, &up);
    b_text(upid, 0, "..");
    blk_spec rf = cv_panel_local(LAYER_WORK, 64, 18, 1);
    rf.on_click = fm_tool_refresh;
    block_id rfid = b_row(tools, &rf);
    b_text(rfid, 0, "Refresh");

    blk_spec list = cv_panel_local(LAYER_WORK, FM_BODY_W, FM_BODY_H, 0);
    fm_list = b_column(fm_root, &list);

    blk_spec st = cv_panel_local(LAYER_WORK, FM_BODY_W, 16, 0);
    fm_status_blk = b_text(fm_root, &st, fm_status);

    fm_refresh();
    fm_active = 1;
    gui_set_key_hook(fm_key, 0);
    gui_raise_to_front(fm_root);
}

int gui_filemgr_active(void) { return fm_active; }
int gui_filemgr_count(void) { return fm_n; }
