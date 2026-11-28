/*
 * filemgr.c -- 资源管理器组件（块子树，规格 §7 组件层）。多实例：每实例独立
 * cwd / 条目表 / 选中 / 滚动 / 状态行，整结构 fm_inst_t 运行时 kvmalloc 堆分配
 * （不进 .bss），池上限 FM_MAX。点击窗口任意位置聚焦（切 active + 切键盘钩子 +
 * 置前）；关闭按钮只关被点实例，其余实例的目录/选中/滚动不受影响。
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
#include "serial_log.h"
#include "kvheap.h"
#include <stdint.h>
#include <stddef.h>
#include "kstring.h"
#include "lang.h"       /* i18n 第一刀：标题/状态/错误 trs 内联对 */

#define FM_MAX_ENT   64
#define FM_ROWS_VIEW 14
#define FM_BODY_W    368
#define FM_BODY_H    280
#define FM_ROW_H     18
#define FM_MAX       3

typedef struct fm_inst {
    int       in_use;
    int       slot;
    block_id  root, pathbar, list, status_blk, title_blk;
    block_id  rows[FM_MAX_ENT];
    char      cwd[FS_PATH_MAX];
    char      names[FM_MAX_ENT][FS_NAME_MAX];
    struct fs_raw_ent raw[FM_MAX_ENT];
    char      labels[FM_MAX_ENT][FS_NAME_MAX + 16];
    int       n, sel, scroll;
    char      status[120];
    char      title_buf[32];
} fm_inst_t;

static fm_inst_t *pool[FM_MAX] = { 0 };
static int        fm_active = -1;

static void fm_row_click(block_id id, int32_t lx, int32_t ly);
static blk_spec cv_panel_local(uint16_t layer, int32_t w, int32_t h, uint8_t border);
static void fm_focus(int slot);
static int  fm_key(int code, void *ud);

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

static void fm_ser_dec(uint32_t v) {
    char buf[12]; int n = 0;
    if (v == 0) { ser_putc('0'); return; }
    while (v) { buf[n++] = (char)('0' + v % 10u); v /= 10u; }
    while (n) ser_putc(buf[--n]);
}

static fm_inst_t *fm_active_inst(void) {
    if (fm_active >= 0 && fm_active < FM_MAX &&
        pool[fm_active] && pool[fm_active]->in_use)
        return pool[fm_active];
    return 0;
}

static void fm_join(const char *dir, const char *name, char *out, int cap) {
    xcopy(out, cap, dir);
    int o = xlen(out);
    if (o == 0 || out[o - 1] != '/') { out[o] = '/'; out[o + 1] = 0; }
    xcat(out, cap, name);
}

static int fm_is_dir(fm_inst_t *f, int i) {
    return (f->raw[i].attr & FAT_ATTR_DIR) != 0;
}

static void fm_set_status(fm_inst_t *f, const char *msg) {
    xcopy(f->status, sizeof(f->status), msg);
    block *st = btable_get(f->status_blk);
    if (st) { st->text = f->status; st->dirty_paint = 1; }
}

/* ---- 列表重建 ------------------------------------------------------------ */

static void fm_clear_rows(fm_inst_t *f) {
    for (int i = 0; i < FM_MAX_ENT; i++) {
        if (f->rows[i]) { b_remove(f->rows[i]); f->rows[i] = 0; }
    }
}

static void fm_row_label(fm_inst_t *f, int i) {
    char *lb = f->labels[i];
    int o = 0;
    lb[o++] = (i == f->sel) ? '>' : ' ';
    const char *nm = f->names[i];
    int n = xlen(nm);
    for (int k = 0; k < n && o < FS_NAME_MAX + 10; k++) lb[o++] = nm[k];
    lb[o] = 0;
    if (!fm_is_dir(f, i)) {
        char num[12]; int m = 0;
        uint32_t v = f->raw[i].size;
        if (v == 0) num[m++] = '0';
        while (v) { num[m++] = (char)('0' + v % 10); v /= 10; }
        xcat(lb, FS_NAME_MAX + 16, "  ");
        while (m) { char one[2] = { num[--m], 0 }; xcat(lb, FS_NAME_MAX + 16, one); }
    }
}

static void fm_refresh(fm_inst_t *f) {
    fm_clear_rows(f);
    int hidden = 0;
    f->n = fs_enum_entries_raw(f->cwd, f->raw, FM_MAX_ENT, &hidden);
    if (f->n < 0) { f->n = 0; fm_set_status(f, trs("read failed", "读取失败")); return; }
    /* 名字表与 raw 表按同一序配对；目录名补 '/' 后缀。 */
    char names[FM_MAX_ENT][FS_NAME_MAX];
    int nn = fs_enum_entries(f->cwd, names, FM_MAX_ENT);
    for (int i = 0; i < f->n; i++) {
        if (i < nn) xcopy(f->names[i], FS_NAME_MAX, names[i]);
        else        xcopy(f->names[i], FS_NAME_MAX, f->raw[i].name);
    }
    f->sel = 0; f->scroll = 0;
    for (int i = 0; i < f->n; i++) fm_row_label(f, i);

    if (f->n == 0) fm_set_status(f, trs("empty", "空目录"));
    else           fm_set_status(f, trs("ready", "就绪"));

    for (int v = 0; v < FM_ROWS_VIEW && v < f->n; v++) {
        int i = f->scroll + v;
        blk_spec row = cv_panel_local(LAYER_WORK, FM_BODY_W, FM_ROW_H, 0);
        row.on_click = fm_row_click;
        row.usr = f;
        row.z = i;
        block_id rid = b_row(f->list, &row);
        f->rows[i] = rid;
        b_text(rid, 0, f->labels[i]);
    }
    layout_run();
    if (f->root) gui_mark_dirty(f->root);
}

/* ---- 打开条目 ------------------------------------------------------------ */

static void fm_open_entry(fm_inst_t *f, int i) {
    if (i < 0 || i >= f->n) return;
    char path[FS_PATH_MAX];
    char nm[FS_NAME_MAX];
    xcopy(nm, sizeof(nm), f->names[i]);
    int n = xlen(nm);
    if (n && nm[n - 1] == '/') nm[n - 1] = 0;

    if (fm_is_dir(f, i)) {
        fm_join(f->cwd, nm, path, sizeof(path));
        xcopy(f->cwd, sizeof(f->cwd), path);
        fm_refresh(f);
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
    fm_join(f->cwd, nm, path, sizeof(path));
    if (is_txt) {
        gui_notepad_open(path);   /* 钩子由记事本接管 */
    } else {
        fm_set_status(f, trs("unsupported type", "不支持的类型"));
    }
}

static void fm_open_parent(fm_inst_t *f) {
    if (xlen(f->cwd) <= 1) return;
    char p[FS_PATH_MAX];
    xcopy(p, sizeof(p), f->cwd);
    int o = xlen(p);
    while (o > 1 && p[o - 1] == '/') p[--o] = 0;
    int slash = o;
    while (slash > 0 && p[slash - 1] != '/') slash--;
    if (slash <= 1) { p[0] = '/'; p[1] = 0; }
    else p[slash] = 0;
    xcopy(f->cwd, sizeof(f->cwd), p);
    fm_refresh(f);
}

/* ---- 交互回调 ------------------------------------------------------------ */

static void fm_row_click(block_id id, int32_t lx, int32_t ly) {
    (void)lx; (void)ly;
    block *row = btable_get(id);
    if (!row) return;
    fm_inst_t *f = (fm_inst_t *)row->usr;
    if (!f || !f->in_use) return;
    int i = (int)row->z;
    if (i < 0 || i >= f->n) return;
    fm_focus(f->slot);
    f->sel = i;
    for (int k = 0; k < f->n; k++) fm_row_label(f, k);
    for (int k = 0; k < f->n; k++) {
        block *rb = btable_get(f->rows[k]);
        if (rb) { rb->text = f->labels[k]; rb->dirty_paint = 1; }
    }
}

static void fm_tool_parent(block_id id, int32_t lx, int32_t ly) {
    (void)lx; (void)ly;
    block *b = btable_get(id);
    fm_inst_t *f = b ? (fm_inst_t *)b->usr : 0;
    if (f && f->in_use) { fm_focus(f->slot); fm_open_parent(f); }
}

static void fm_tool_refresh(block_id id, int32_t lx, int32_t ly) {
    (void)lx; (void)ly;
    block *b = btable_get(id);
    fm_inst_t *f = b ? (fm_inst_t *)b->usr : 0;
    if (f && f->in_use) { fm_focus(f->slot); fm_refresh(f); }
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

/* 标题条自绘：聚焦实例 accent 底高亮，其余回退 bar 令牌色。 */
static void fm_title_render(block_id id) {
    block *b = btable_get(id);
    if (!b) return;
    fm_inst_t *f = (fm_inst_t *)b->usr;
    const struct style *tok = gui_active_style();
    fm_inst_t *act = fm_active_inst();
    uint32_t c = (f && f->in_use && f == act) ? tok->accent : tok->bar;
    gui_fill_rect(b->x, b->y, b->w, b->h, c);
}

static void fm_root_click(block_id id, int32_t lx, int32_t ly) {
    (void)lx; (void)ly;
    block *b = btable_get(id);
    fm_inst_t *f = b ? (fm_inst_t *)b->usr : 0;
    if (f && f->in_use) fm_focus(f->slot);
}

/* 聚焦某实例：切 active + 切键盘钩子 + 置前；新旧标题条补脏换高亮。 */
static void fm_focus(int slot) {
    if (slot < 0 || slot >= FM_MAX || !pool[slot] || !pool[slot]->in_use) return;
    int prev = fm_active;
    fm_active = slot;
    gui_set_key_hook(fm_key, pool[slot]);
    gui_raise_to_front(pool[slot]->root);
    if (prev >= 0 && pool[prev] && pool[prev]->in_use) {
        block *pb = btable_get(pool[prev]->title_blk);
        if (pb) pb->dirty_paint = 1;
    }
    block *nb = btable_get(pool[slot]->title_blk);
    if (nb) nb->dirty_paint = 1;
    ser_write("FM:focus idx="); fm_ser_dec((uint32_t)slot); ser_write("\n");
    ser_write("FOCUS:kind=files idx="); fm_ser_dec((uint32_t)slot); ser_write("\n");
}

static void fm_close_inst(int slot, int report) {
    if (slot < 0 || slot >= FM_MAX) return;
    fm_inst_t *f = pool[slot];
    if (!f || !f->in_use) return;
    int rx = 0, ry = 0, rw = 0, rh = 0;
    block *pr = btable_get(f->root);
    if (pr) { rx = pr->x; ry = pr->y; rw = pr->w; rh = pr->h; }
    fm_clear_rows(f);
    if (f->root) { b_remove(f->root); f->root = 0; }
    f->pathbar = f->list = f->status_blk = f->title_blk = 0;
    f->in_use = 0;
    kvmfree(f);
    pool[slot] = 0;
    if (report) { ser_write("FM:close idx="); fm_ser_dec((uint32_t)slot); ser_write("\n"); }
    if (fm_active == slot) {
        fm_active = -1;
        for (int i = 0; i < FM_MAX; i++)
            if (pool[i]) { fm_active = i; break; }
        if (fm_active >= 0) gui_set_key_hook(fm_key, pool[fm_active]);
        else gui_set_key_hook(0, 0);
    }
    gui_mark_dirty_union(0, rx, ry, rw, rh);
}

static void fm_close_click(block_id id, int32_t lx, int32_t ly) {
    (void)lx; (void)ly;
    block *b = btable_get(id);
    fm_inst_t *f = b ? (fm_inst_t *)b->usr : 0;
    if (f && f->in_use) fm_close_inst(f->slot, 1);
}

void gui_filemgr_close(void) {
    for (int i = FM_MAX - 1; i >= 0; i--) fm_close_inst(i, 0);
}

static int fm_key(int code, void *ud) {
    fm_inst_t *f = (fm_inst_t *)ud;
    if (!f || !f->in_use) return 0;
    if (code == 27) { fm_close_inst(f->slot, 1); return 1; }
    if (f->n == 0) return 1;
    switch (code) {
    case KB_UP:
        if (f->sel > 0) f->sel--;
        if (f->sel < f->scroll) f->scroll = f->sel;
        break;
    case KB_DOWN:
        if (f->sel < f->n - 1) f->sel++;
        if (f->sel >= f->scroll + FM_ROWS_VIEW) f->scroll = f->sel - FM_ROWS_VIEW + 1;
        break;
    case 10: fm_open_entry(f, f->sel); return 1;
    default: return 0;
    }
    for (int k = 0; k < f->n; k++) fm_row_label(f, k);
    for (int k = 0; k < f->n; k++) {
        block *rb = btable_get(f->rows[k]);
        if (rb) { rb->text = f->labels[k]; rb->dirty_paint = 1; }
    }
    return 1;
}

void gui_filemgr_open(const char *path) {
    int slot = -1;
    for (int i = 0; i < FM_MAX; i++)
        if (!pool[i]) { slot = i; break; }
    if (slot < 0) { fm_focus(fm_active); return; }   /* 到上限：聚焦现有实例，不崩 */

    fm_inst_t *f = kvmalloc(sizeof(fm_inst_t), 16);
    if (!f) return;
    memset(f, 0, sizeof(*f));
    pool[slot] = f;
    f->in_use = 1;
    f->slot = slot;
    xcopy(f->cwd, sizeof(f->cwd), path ? path : "/");
    if (xlen(f->cwd) == 0) xcopy(f->cwd, sizeof(f->cwd), "/");

    block_id root_id = gui_root_block()->id;
    /* 高度 = pad_t/b + 标题 + 路径条 + 工具行 + 列表 + 状态行 + 4×gap；
     * 此前漏算 padding/gap，状态行被 clamp_child 压成 h=0。 */
    int32_t root_h = 16 + 16 + 20 + FM_BODY_H + 16 + 8 * 2 + 4 * 4;
    blk_spec root = cv_panel_local(LAYER_WORK, FM_BODY_W + 16, root_h, 1);
    root.pad_l = 8; root.pad_r = 8; root.pad_t = 8; root.pad_b = 8;
    root.gap = 4;
    root.focusable = 1;
    root.on_click = fm_root_click;
    root.usr = f;
    f->root = b_column(root_id, &root);
    block *pr = btable_get(f->root);
    ui_cascade_place(pr, slot, FM_BODY_W + 16, root_h, 12);
    pr->z = layer_max_z(LAYER_WORK) + 1;
    ser_write("POS:fm idx="); fm_ser_dec((uint32_t)slot);
    ser_write(" x="); fm_ser_dec((uint32_t)pr->x);
    ser_write(" y="); fm_ser_dec((uint32_t)pr->y); ser_write("\n");

    blk_spec title = cv_panel_local(LAYER_WORK, FM_BODY_W, 16, 0);
    title.role_bg = ROLE_BAR; title.bg = INHERIT_COLOR;
    title.align_cross = XALN_CENTER;
    title.align_main = ALN_SPACE_BETWEEN;   /* 左标签 + 右 Close 分列，与终端/记事本/设置一致 */
    title.render = fm_title_render;
    title.usr = f;
    block_id bar = b_row(f->root, &title);
    f->title_blk = bar;
    char *tt = f->title_buf;
    xcopy(tt, sizeof(f->title_buf), trs("Files ", "文件 "));
    { char d[2]; d[0] = (char)('0' + slot + 1); d[1] = 0; xcat(tt, sizeof(f->title_buf), d); }
    b_text(bar, 0, tt);
    blk_spec close = cv_panel_local(LAYER_WORK, 56, 14, 1);
    close.on_click = fm_close_click;
    close.usr = f;
    block_id cid = b_row(bar, &close);
    b_text(cid, 0, trs("Close", "关闭"));

    blk_spec pb = cv_panel_local(LAYER_WORK, FM_BODY_W, 16, 0);
    f->pathbar = b_text(f->root, &pb, f->cwd);

    blk_spec tb = cv_panel_local(LAYER_WORK, FM_BODY_W, 20, 0);
    block_id tools = b_row(f->root, &tb);
    blk_spec up = cv_panel_local(LAYER_WORK, 64, 18, 1);
    up.on_click = fm_tool_parent;
    up.usr = f;
    block_id upid = b_row(tools, &up);
    b_text(upid, 0, "..");
    blk_spec rf = cv_panel_local(LAYER_WORK, 64, 18, 1);
    rf.on_click = fm_tool_refresh;
    rf.usr = f;
    block_id rfid = b_row(tools, &rf);
    b_text(rfid, 0, trs("Refresh", "刷新"));

    blk_spec list = cv_panel_local(LAYER_WORK, FM_BODY_W, FM_BODY_H, 0);
    f->list = b_column(f->root, &list);

    blk_spec st = cv_panel_local(LAYER_WORK, FM_BODY_W, 16, 0);
    f->status_blk = b_text(f->root, &st, f->status);

    fm_refresh(f);
    fm_active = slot;
    gui_set_key_hook(fm_key, f);
    gui_raise_to_front(f->root);
    ser_write("FM:open idx="); fm_ser_dec((uint32_t)slot);
    ser_write(" cwd="); ser_write(f->cwd); ser_write("\n");
}

int gui_filemgr_active(void) { return fm_active_inst() ? 1 : 0; }
int gui_filemgr_count(void) {
    fm_inst_t *f = fm_active_inst();
    return f ? f->n : 0;
}

/* ---- 多实例隔离探针读取/注入口径（不进 CE ABI） -------------------------- */

static fm_inst_t *fm_slot_inst(int slot) {
    if (slot < 0 || slot >= FM_MAX || !pool[slot] || !pool[slot]->in_use) return 0;
    return pool[slot];
}

void gui_fm_focus(int slot) { fm_focus(slot); }

const char *gui_fm_cwd_inst(int slot) {
    fm_inst_t *f = fm_slot_inst(slot);
    return f ? f->cwd : "";
}

int gui_fm_count_inst(int slot) {
    fm_inst_t *f = fm_slot_inst(slot);
    return f ? f->n : 0;
}

int gui_fm_sel_inst(int slot) {
    fm_inst_t *f = fm_slot_inst(slot);
    return f ? f->sel : -1;
}

void gui_fm_select(int slot, int i) {
    fm_inst_t *f = fm_slot_inst(slot);
    if (!f || i < 0 || i >= f->n) return;
    f->sel = i;
    for (int k = 0; k < f->n; k++) fm_row_label(f, k);
    for (int k = 0; k < f->n; k++) {
        block *rb = btable_get(f->rows[k]);
        if (rb) { rb->text = f->labels[k]; rb->dirty_paint = 1; }
    }
}

/* ---- 任务栏实例按钮查询口径（内部，不进 CE ABI） -------------------------- */

int gui_fm_in_use(int slot) { return fm_slot_inst(slot) ? 1 : 0; }

block_id gui_fm_root(int slot) {
    fm_inst_t *f = fm_slot_inst(slot);
    return f ? f->root : 0;
}

int gui_fm_active_slot(void) { return fm_active; }

void *gui_fm_focus_ud(int slot) {
    if (slot < 0 || slot >= FM_MAX || !pool[slot] || !pool[slot]->in_use) return 0;
    return pool[slot];
}
