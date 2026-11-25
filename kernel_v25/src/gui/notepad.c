/*
 * notepad.c -- 记事本组件（块子树，规格 §7 组件层）。
 *
 * 标题条块 + 文本区块（自绘回调）+ 状态行块；不引入进程 / IPC / 显示
 * 服务。键盘走现有输入路径，钩子回调内只打脏位，禁止嵌套提交。
 * 多行渲染按 8×16 位图字形、行高固定、不换行（超宽截断 "..."）。
 */
#include "gui/gui.h"
#include "gui_int.h"
#include "gui/tokens.h"
#include "fs.h"
#include "gfx24.h"
#include "keyboard.h"
#include <stdint.h>
#include <stddef.h>
#include "kstring.h"

#define NP_BODY_W   512
#define NP_BODY_H   272
#define NP_TITLE_H  24
#define NP_STAT_H   16
#define NP_PAD      4

static char        np_buf[FS_DATA_MAX];
static int         np_len;
static int         np_pos;            /* 光标绝对下标 */
static int         np_row, np_col;
static int         np_scroll;
static int         np_dirty;
static int         np_full;
static int         np_active;
static char        np_path[FS_PATH_MAX];
static char        np_title[80];
static char        np_status[120];
static fs_entry_t *np_file;

static block_id    np_root;
static block_id    np_body;
static block_id    np_status_blk;

/* ---- Freestanding 小字符串助手 ---------------------------------------- */

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
static void xnum(char *d, int cap, int v) {
    char tmp[12]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    int o = xlen(d);
    while (n && o < cap - 1) d[o++] = tmp[--n];
    d[o] = 0;
}

/* ---- 文本模型 ---------------------------------------------------------- */

static int np_line_count(void) {
    int n = 1;
    for (int i = 0; i < np_len; i++)
        if (np_buf[i] == '\n') n++;
    return n;
}

static int np_line_start(int row) {
    int r = 0;
    for (int i = 0; i <= np_len; i++) {
        if (r == row) return i;
        if (i < np_len && np_buf[i] == '\n') r++;
    }
    return np_len;
}

static int np_line_len(int row) {
    int s = np_line_start(row);
    int e = s;
    while (e < np_len && np_buf[e] != '\n') e++;
    return e - s;
}

/* 由绝对光标下标重算行列；边界钳制，滚动跟随光标。 */
static void np_recompute_cursor(void) {
    if (np_pos < 0) np_pos = 0;
    if (np_pos > np_len) np_pos = np_len;
    np_row = 0; np_col = 0;
    for (int i = 0; i < np_pos; i++) {
        if (np_buf[i] == '\n') { np_row++; np_col = 0; }
        else np_col++;
    }
    int rows_visible = NP_BODY_H / 16;
    if (np_row < np_scroll) np_scroll = np_row;
    if (np_row >= np_scroll + rows_visible)
        np_scroll = np_row - rows_visible + 1;
    if (np_scroll < 0) np_scroll = 0;
}

static void np_update_status(void) {
    xcopy(np_status, sizeof(np_status), np_path);
    xcat(np_status, sizeof(np_status), "  R");
    xnum(np_status, sizeof(np_status), np_row + 1);
    xcat(np_status, sizeof(np_status), " C");
    xnum(np_status, sizeof(np_status), np_col + 1);
    xcat(np_status, sizeof(np_status), np_dirty ? "  dirty" : "  saved");
    if (np_full) xcat(np_status, sizeof(np_status), "  full");
    block *st = btable_get(np_status_blk);
    if (st) {
        st->text = np_status;
        st->dirty_paint = 1;
    }
}

/* ---- 自绘回调 ---------------------------------------------------------- */

static void np_render(block_id id) {
    block *b = btable_get(id);
    if (!b) return;
    const struct style *tok = gui_active_style();
    uint32_t fg = tok->fg, bg = tok->panel, accent = tok->accent;
    gui_fill_rect(b->x, b->y, b->w, b->h, bg);

    int rows_visible = b->h / 16;
    int cols = (b->w - NP_PAD * 2) / 8;
    char line[64];

    for (int v = 0; v < rows_visible; v++) {
        int row = np_scroll + v;
        if (row >= np_line_count()) break;
        int s = np_line_start(row);
        int ln = np_line_len(row);
        int cap = ln < cols ? ln : cols - 3;
        if (cap < 0) cap = 0;
        int i;
        for (i = 0; i < cap; i++) line[i] = np_buf[s + i];
        if (ln > cols) { line[i++] = '.'; line[i++] = '.'; line[i++] = '.'; }
        line[i] = 0;
        gui_draw_text(b->x + NP_PAD, b->y + NP_PAD + v * 16, line, fg, 1);
    }

    /* 光标：竖线高亮。 */
    if (np_row >= np_scroll && np_row < np_scroll + rows_visible) {
        int cy = b->y + NP_PAD + (np_row - np_scroll) * 16;
        int cx = b->x + NP_PAD + np_col * 8;
        gui_fill_rect(cx, cy, 2, 16, accent);
    }
}

/* ---- 面板规格（本地副本，跟随活动令牌角色） ----------------------------- */

static blk_spec np_panel(uint16_t layer, int32_t w, int32_t h, uint8_t border) {
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

/* ---- 打开 / 关闭 -------------------------------------------------------- */

static void np_close_click(block_id id, int32_t lx, int32_t ly) {
    (void)id; (void)lx; (void)ly;
    gui_notepad_close();
}

void gui_notepad_close(void) {
    if (!np_active) return;
    gui_set_key_hook(0, 0);
    int rx = 0, ry = 0, rw = 0, rh = 0;
    block *pr = btable_get(np_root);
    if (pr) { rx = pr->x; ry = pr->y; rw = pr->w; rh = pr->h; }
    if (np_root) { b_remove(np_root); np_root = 0; }
    np_body = 0; np_status_blk = 0;
    np_active = 0;
    /* 释放矩形 ∪ 与之相交的同层兄弟标脏：补回被面板盖住的旧像素。 */
    gui_mark_dirty_union(0, rx, ry, rw, rh);
}

static int np_key(int code, void *ud);
static void gui_notepad_attach_keys(void) {
    if (np_active) gui_set_key_hook(np_key, 0);
}

static void np_load(const char *path) {
    xcopy(np_path, sizeof(np_path), path ? path : "/notes.txt");
    np_file = fs_open_file(np_path);
    if (!np_file) np_file = fs_create_file(np_path);
    np_len = 0; np_pos = 0; np_dirty = 0; np_full = 0; np_scroll = 0;
    if (np_file && np_file->size > 0 && np_file->size < FS_DATA_MAX) {
        np_len = np_file->size;
        for (int i = 0; i < np_len; i++) np_buf[i] = np_file->data[i];
    }
    np_buf[np_len] = 0;
    np_recompute_cursor();
}

void gui_notepad_open(const char *path) {
    gui_notepad_close();
    np_load(path);

    block_id root_id = gui_root_block()->id;
    blk_spec root = np_panel(LAYER_WORK, NP_BODY_W + NP_PAD * 2,
                             NP_TITLE_H + NP_BODY_H + NP_STAT_H, 1);
    root.pad_l = NP_PAD; root.pad_r = NP_PAD;
    root.pad_t = NP_PAD; root.pad_b = NP_PAD;
    root.gap = NP_PAD;
    root.focusable = 1;
    np_root = b_column(root_id, &root);
    block *pr = btable_get(np_root);
    pr->x = 140; pr->y = 64; pr->moved = 1;
    pr->st.corner_radius = 16;   /* §16.1 外框 r16 */
    pr->z = layer_max_z(LAYER_WORK) + 1;   /* 同层取最大 z+1，不与既有面板同序 */

    /* 标题条：固定串标签 + 关闭块，两端分列不叠（E4-⑦）。路径已在状态行显示。 */
    blk_spec title = np_panel(LAYER_WORK, NP_BODY_W, NP_TITLE_H, 0);
    title.role_bg = ROLE_BAR;
    title.bg = INHERIT_COLOR;
    title.align_cross = XALN_CENTER;
    title.align_main = ALN_SPACE_BETWEEN;
    block_id bar = b_row(np_root, &title);
    xcopy(np_title, sizeof(np_title), "Notepad");
    b_text(bar, 0, np_title);
    blk_spec close = np_panel(LAYER_WORK, 56, NP_TITLE_H - 4, 1);
    close.on_click = np_close_click;
    block_id cid = b_row(bar, &close);
    b_text(cid, 0, "Close");

    /* 文本区块（自绘）。 */
    blk_spec body = np_panel(LAYER_WORK, NP_BODY_W, NP_BODY_H, 0);
    body.render = np_render;
    body.focusable = 1;
    np_body = b_rect(np_root, &body);

    /* 状态行。 */
    blk_spec st = np_panel(LAYER_WORK, NP_BODY_W, NP_STAT_H, 0);
    np_status_blk = b_text(np_root, &st, np_status);
    np_update_status();

    np_active = 1;
    gui_notepad_attach_keys();
    layout_run();
    gui_raise_to_front(np_root);
    /* 面板矩形 ∪ 被遮挡的同层兄弟一起标脏，重叠区不得残留示例块像素。 */
    gui_mark_dirty_union(pr, pr->x, pr->y, pr->w, pr->h);
}

/* ---- 键盘编辑 ----------------------------------------------------------- */

static void np_insert(char c) {
    if (np_len >= FS_DATA_MAX - 1) { np_full = 1; return; }
    for (int i = np_len; i > np_pos; i--) np_buf[i] = np_buf[i - 1];
    np_buf[np_pos++] = c;
    np_len++;
    np_dirty = 1;
}

static void np_backspace(void) {
    if (np_pos <= 0) return;
    for (int i = np_pos - 1; i < np_len - 1; i++) np_buf[i] = np_buf[i + 1];
    np_len--; np_pos--;
    np_dirty = 1;
}

static int np_save(void) {
    if (!np_file) return -1;
    for (int i = 0; i < np_len; i++) np_file->data[i] = np_buf[i];
    np_file->size = (uint16_t)np_len;
    int r = fs_save(np_file);
    if (r == 0) { np_dirty = 0; np_full = 0; }
    return r;
}

static int np_key(int code, void *ud) {
    (void)ud;
    if (!np_active) return 0;

    if (code == 27) { gui_notepad_close(); return 1; }      /* Esc 关闭 */
    if (code == 19) { np_save(); np_update_status(); return 1; } /* Ctrl+S */

    int rows_visible = NP_BODY_H / 16;

    switch (code) {
    case 10: np_insert('\n'); break;                        /* Enter */
    case 8:  np_backspace(); break;                         /* Backspace */
    case KB_LEFT:  if (np_pos > 0) np_pos--; break;
    case KB_RIGHT: if (np_pos < np_len) np_pos++; break;
    case KB_UP:
        if (np_row > 0) {
            int ln = np_line_len(np_row - 1);
            np_col = np_col < ln ? np_col : ln;
            np_pos = np_line_start(np_row - 1) + np_col;
        }
        break;
    case KB_DOWN:
        if (np_row < np_line_count() - 1) {
            int ln = np_line_len(np_row + 1);
            np_col = np_col < ln ? np_col : ln;
            np_pos = np_line_start(np_row + 1) + np_col;
        }
        break;
    case KB_HOME:  np_pos = np_line_start(np_row); break;
    case KB_END:   np_pos = np_line_start(np_row) + np_line_len(np_row); break;
    case KB_PGUP:  np_scroll -= rows_visible; if (np_scroll < 0) np_scroll = 0; break;
    case KB_PGDN:  np_scroll += rows_visible; break;
    default:
        if (code >= 0x20 && code <= 0x7E) np_insert((char)code);
        else return 0;
    }
    np_recompute_cursor();
    block *b = btable_get(np_body);
    if (b) b->dirty_paint = 1;
    np_update_status();
    return 1;
}

/* 探针读取口径（chrome.c 探针打点使用）。 */
int  gui_notepad_active(void)  { return np_active; }
int  gui_notepad_lines(void)   { return np_line_count(); }
int  gui_notepad_col(void)      { return np_col; }
int  gui_notepad_dirty(void)    { return np_dirty; }
int  gui_notepad_full(void)     { return np_full; }
