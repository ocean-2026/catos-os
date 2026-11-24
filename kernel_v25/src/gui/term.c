/*
 * term.c -- BLK_TERM 多会话块内终端（规格 §7 组件层）。
 *
 * 每个会话独立持有：当前目录、200 行回看环、历史环、单行编辑缓冲。
 * 后端抽象（vc_backend）把输出写进本会话的行环，不直写 VGA；命令集为
 * 内核内置子集（help/pwd/ls/cd/echo/clear/history）。行编辑支持
 * Left/Right/Home/End/Backspace/Up/Down；Tab 不补全。最小 SGR：不解析
 * 转义序列，输出侧不生成彩色码。Ctrl+N 新建会话，Esc 只关当前会话，
 * 重开会话缓冲保留（同槽位复用）。
 */
#include "gui/gui.h"
#include "gui_int.h"
#include "gui/tokens.h"
#include "fs.h"
#include "keyboard.h"
#include <stdint.h>
#include <stddef.h>

#define TERM_MAX   4
#define T_COLS     80
#define T_ROWS     30
#define T_RING     200
#define T_HIST     32
#define T_LINE     80
#define T_BODY_W   (T_COLS * 8)
#define T_BODY_H   ((T_ROWS - 1) * 16)   /* 末行留给输入行 */
#define T_TITLE_H  24
#define T_STAT_H   16
#define T_PAD      4

typedef struct {
    int       in_use;
    block_id  root, body, status;
    char      cwd[FS_PATH_MAX];
    char      ring[T_RING][T_COLS];
    int       ring_tail, ring_n;
    char      hist[T_HIST][T_LINE];
    int       hist_n;
    int       hist_idx;
    char      line[T_LINE];
    int       len, pos;
    int       view_top;                 /* 0 = 跟随末尾 */
} term_t;

static term_t terms[TERM_MAX];
static int    active = -1;

/* ---- 小字符串助手 ------------------------------------------------------ */

static int strcmp_(const char *a, const char *b);
static int t_len(const char *s) { int n = 0; if (s) while (s[n]) n++; return n; }
static void t_copy(char *d, int cap, const char *s) {
    int n = t_len(s); if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++) d[i] = s[i];
    d[n] = 0;
}
static void t_cat(char *d, int cap, const char *s) {
    int o = t_len(d); int n = t_len(s);
    for (int i = 0; i < n && o < cap - 1; i++) d[o++] = s[i];
    d[o] = 0;
}
static void t_trim(char *s) {
    int n = t_len(s), l = 0, r = n - 1;
    while (l < n && (s[l] == ' ' || s[l] == '\t')) l++;
    while (r >= l && (s[r] == ' ' || s[r] == '\t')) r--;
    for (int i = 0; i <= r - l; i++) s[i] = s[l + i];
    s[r - l + 1] = 0;
}

/* ---- 行环后端（vc_backend：只写本会话缓冲，不碰真实控制台） ------------- */

static void t_ring_push(term_t *t, const char *text) {
    const char *p = text;
    while (*p) {
        int o = 0;
        while (*p && *p != '\n') {
            if (o < T_COLS - 1) t->ring[t->ring_tail][o++] = *p;
            p++;
        }
        t->ring[t->ring_tail][o] = 0;
        for (int k = o; k < T_COLS; k++) t->ring[t->ring_tail][k] = 0;
        t->ring_tail = (t->ring_tail + 1) % T_RING;
        if (t->ring_n < T_RING) t->ring_n++;
        if (*p == '\n') p++;
    }
}

static void t_ring_clear(term_t *t) { t->ring_n = 0; t->ring_tail = 0; }

static const char *t_ring_at(term_t *t, int logical) {
    if (logical < 0 || logical >= t->ring_n) return "";
    return t->ring[(t->ring_tail - t->ring_n + logical + T_RING) % T_RING];
}

/* ---- 路径解析（每会话独立 cwd，不读写全局 shell 目录） ------------------- */

static void t_resolve(term_t *t, const char *arg, char *out, int cap) {
    char raw[FS_PATH_MAX];
    raw[0] = 0;
    if (arg && arg[0] == '/') t_copy(raw, sizeof(raw), arg);
    else if (arg && arg[0]) {
        t_copy(raw, sizeof(raw), t->cwd);
        if (raw[t_len(raw) - 1] != '/') t_cat(raw, sizeof(raw), "/");
        t_cat(raw, sizeof(raw), arg);
    } else t_copy(raw, sizeof(raw), t->cwd);

    char segs[16][FS_NAME_MAX];
    int ns = 0;
    char *scan = raw;
    while (*scan) {
        while (*scan == '/') scan++;
        if (!*scan) break;
        char comp[FS_NAME_MAX]; int cl = 0;
        while (*scan && *scan != '/') {
            if (cl < FS_NAME_MAX - 1) comp[cl++] = *scan;
            scan++;
        }
        comp[cl] = 0;
        if (comp[0] == 0 || !strcmp_(comp, ".")) continue;
        if (!strcmp_(comp, "..")) { if (ns > 0) ns--; continue; }
        if (ns < FS_PATH_MAX / FS_NAME_MAX) t_copy(segs[ns++], FS_NAME_MAX, comp);
    }
    out[0] = '/'; out[1] = 0;
    for (int i = 0; i < ns; i++) {
        t_cat(out, cap, segs[i]);
        if (i < ns - 1) t_cat(out, cap, "/");
    }
}

static int strcmp_(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void t_exec(term_t *t, const char *raw);
static int  t_key(int code, void *ud);

/* ---- 内置命令 ----------------------------------------------------------- */

static void t_exec(term_t *t, const char *raw) {
    char line[T_LINE];
    t_copy(line, sizeof(line), raw);
    t_trim(line);
    if (line[0] == 0) return;

    /* 入历史环 */
    if (t->hist_n < T_HIST) t_copy(t->hist[t->hist_n], T_LINE, line);
    else {
        for (int i = 0; i < T_HIST - 1; i++)
            t_copy(t->hist[i], T_LINE, t->hist[i + 1]);
        t_copy(t->hist[T_HIST - 1], T_LINE, line);
    }
    t->hist_n = t->hist_n < T_HIST ? t->hist_n + 1 : T_HIST;
    t->hist_idx = t->hist_n;

    char cmd[32], rest[T_LINE];
    int cl = 0; const char *p = line;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && cl < 31) cmd[cl++] = *p++;
    cmd[cl] = 0;
    while (*p == ' ') p++;
    t_copy(rest, sizeof(rest), p);

    if (!strcmp_(cmd, "help")) {
        t_ring_push(t, "Commands: help pwd ls [dir] cd <dir> echo <text> clear history");
    } else if (!strcmp_(cmd, "pwd")) {
        t_ring_push(t, t->cwd);
    } else if (!strcmp_(cmd, "clear")) {
        t_ring_clear(t);
    } else if (!strcmp_(cmd, "mkdir")) {
        char target[FS_PATH_MAX];
        t_resolve(t, rest, target, sizeof(target));
        int rc = fs_mkdir(target);
        if (rc != FS_OK) {
            char msg[T_COLS];
            t_copy(msg, sizeof(msg), "mkdir: cannot create ");
            t_cat(msg, sizeof(msg), rest);
            t_ring_push(t, msg);
        }
    } else if (!strcmp_(cmd, "history")) {
        char buf[T_COLS];
        for (int i = 0; i < t->hist_n; i++) {
            buf[0] = 0;
            char num[8]; int n = 0, v = i + 1;
            if (v == 0) num[n++] = '0';
            while (v) { num[n++] = (char)('0' + v % 10); v /= 10; }
            while (n) t_cat(buf, sizeof(buf), &num[--n]);
            t_cat(buf, sizeof(buf), "  ");
            t_cat(buf, sizeof(buf), t->hist[i]);
            t_ring_push(t, buf);
        }
    } else if (!strcmp_(cmd, "echo")) {
        t_ring_push(t, rest[0] ? rest : "");
    } else if (!strcmp_(cmd, "cd")) {
        char target[FS_PATH_MAX];
        t_resolve(t, rest[0] ? rest : "/", target, sizeof(target));
        if (fs_is_dir(target)) t_copy(t->cwd, sizeof(t->cwd), target);
        else {
            char msg[T_COLS];
            t_copy(msg, sizeof(msg), "cd: not a directory: ");
            t_cat(msg, sizeof(msg), rest);
            t_ring_push(t, msg);
        }
    } else if (!strcmp_(cmd, "ls")) {
        char target[FS_PATH_MAX];
        t_resolve(t, rest[0] ? rest : t->cwd, target, sizeof(target));
        char names[32][FS_NAME_MAX];
        int n = fs_enum_entries(target, names, 32);
        if (n < 0) {
            t_ring_push(t, "ls: cannot open directory");
        } else if (n == 0) {
            t_ring_push(t, "(empty)");
        } else {
            char buf[T_COLS]; buf[0] = 0;
            for (int i = 0; i < n; i++) {
                int add = t_len(names[i]) + 1;
                if (t_len(buf) + add >= T_COLS - 1) {
                    t_ring_push(t, buf); buf[0] = 0;
                }
                t_cat(buf, sizeof(buf), names[i]);
                t_cat(buf, sizeof(buf), " ");
            }
            if (buf[0]) t_ring_push(t, buf);
        }
    } else {
        char msg[T_COLS];
        t_copy(msg, sizeof(msg), "term: command not found: ");
        t_cat(msg, sizeof(msg), cmd);
        t_ring_push(t, msg);
    }
}

/* ---- 面板规格 ----------------------------------------------------------- */

static blk_spec t_panel(uint16_t layer, int32_t w, int32_t h, uint8_t border) {
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

/* ---- 自绘 --------------------------------------------------------------- */

static void t_render(block_id id) {
    block *b = btable_get(id);
    if (!b) return;
    term_t *t = (term_t *)b->usr;
    if (!t) return;
    const struct style *tok = gui_active_style();
    uint32_t fg = tok->fg, bg = tok->panel, accent = tok->accent;
    gui_fill_rect(b->x, b->y, b->w, b->h, bg);

    int show = T_ROWS - 1;
    int first = t->ring_n <= show ? 0 : t->ring_n - show;
    char line[T_COLS + 2];

    for (int v = 0; v < show; v++) {
        const char *src = t_ring_at(t, first + v);
        t_copy(line, sizeof(line), src);
        gui_draw_text(b->x + T_PAD, b->y + T_PAD + v * 16, line, fg, 1);
    }

    /* 输入行：提示符 + 编辑缓冲 + 光标竖线。 */
    int row = show;
    char prompt[T_COLS];
    t_copy(prompt, sizeof(prompt), t->cwd);
    t_cat(prompt, sizeof(prompt), "$ ");
    int pn = t_len(prompt);
    t_copy(line, sizeof(line), prompt);
    t_cat(line, sizeof(line), t->line);
    gui_draw_text(b->x + T_PAD, b->y + T_PAD + row * 16, line, fg, 1);

    int cx = b->x + T_PAD + (pn + t->pos) * 8;
    int cy = b->y + T_PAD + row * 16;
    gui_fill_rect(cx, cy, 2, 16, accent);
}

static void t_update_status(term_t *t) {
    block *st = btable_get(t->status);
    if (!st) return;
    char buf[T_COLS];
    int used = 0;
    for (int i = 0; i < TERM_MAX; i++) if (terms[i].in_use) used++;
    t_copy(buf, sizeof(buf), "Terminal ");
    int idx = (int)(t - terms) + 1;
    char num[8]; int n = 0, v = idx;
    if (v == 0) num[n++] = '0';
    while (v) { num[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) t_cat(buf, sizeof(buf), &num[--n]);
    t_cat(buf, sizeof(buf), "/");
    v = used; n = 0;
    if (v == 0) num[n++] = '0';
    while (v) { num[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) t_cat(buf, sizeof(buf), &num[--n]);
    t_cat(buf, sizeof(buf), "  ");
    t_cat(buf, sizeof(buf), t->cwd);
    st->text = buf;
    st->dirty_paint = 1;
}

/* ---- 打开 / 关闭 --------------------------------------------------------- */

static void t_close_click(block_id id, int32_t lx, int32_t ly) {
    (void)id; (void)lx; (void)ly;
    gui_term_close_active();
}

static void t_build(int idx) {
    term_t *t = &terms[idx];
    block_id root_id = gui_root_block()->id;

    blk_spec root = t_panel(LAYER_WORK, T_BODY_W + T_PAD * 2,
                            T_TITLE_H + T_BODY_H + T_STAT_H, 1);
    root.pad_l = T_PAD; root.pad_r = T_PAD;
    root.pad_t = T_PAD; root.pad_b = T_PAD;
    root.gap = T_PAD;
    root.focusable = 1;
    t->root = b_column(root_id, &root);
    block *pr = btable_get(t->root);
    pr->x = 36 + idx * 20;
    pr->y = 28 + idx * 16;
    pr->moved = 1;
    pr->z = layer_max_z(LAYER_WORK) + 1;

    blk_spec title = t_panel(LAYER_WORK, T_BODY_W, T_TITLE_H, 0);
    title.role_bg = ROLE_BAR;
    block_id bar = b_row(t->root, &title);
    char tt[32];
    t_copy(tt, sizeof(tt), "Terminal ");
    char num[8]; int n = 0, v = idx + 1;
    if (v == 0) num[n++] = '0';
    while (v) { num[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) t_cat(tt, sizeof(tt), &num[--n]);
    b_text(bar, 0, tt);
    blk_spec close = t_panel(LAYER_WORK, 56, T_TITLE_H - 4, 1);
    close.on_click = t_close_click;
    block_id cid = b_row(bar, &close);
    b_text(cid, 0, "Close");

    blk_spec body = t_panel(LAYER_WORK, T_BODY_W, T_BODY_H, 0);
    body.render = t_render;
    body.focusable = 1;
    body.usr = t;
    t->body = b_rect(t->root, &body);

    blk_spec st = t_panel(LAYER_WORK, T_BODY_W, T_STAT_H, 0);
    t->status = b_text(t->root, &st, "");
    t_update_status(t);
}

void gui_term_open(void) {
    int idx = -1;
    for (int i = 0; i < TERM_MAX; i++)
        if (!terms[i].in_use) { idx = i; break; }
    if (idx < 0) { active = 0; return; }

    term_t *t = &terms[idx];
    t->in_use = 1;
    t->ring_tail = 0; t->ring_n = 0;
    t->hist_n = 0; t->hist_idx = 0;
    t->len = 0; t->pos = 0; t->line[0] = 0;
    t->view_top = 0;
    if (t->cwd[0] == 0) t_copy(t->cwd, sizeof(t->cwd), "/");
    t_ring_push(t, "CatOS block terminal (builtins: help pwd ls cd echo clear history)");
    t_build(idx);
    active = idx;
    gui_set_key_hook(t_key, t);
    layout_run();
    gui_raise_to_front(t->root);
    block *pr = btable_get(t->root);
    if (pr) gui_mark_dirty_union(pr, pr->x, pr->y, pr->w, pr->h);
}

void gui_term_close_active(void) {
    if (active < 0) return;
    term_t *t = &terms[active];
    int rx = 0, ry = 0, rw = 0, rh = 0;
    block *pr = btable_get(t->root);
    if (pr) { rx = pr->x; ry = pr->y; rw = pr->w; rh = pr->h; }
    if (t->root) { b_remove(t->root); t->root = 0; }
    t->body = 0; t->status = 0;
    t->in_use = 0;
    /* 会话缓冲保留在槽位内，重开续上；仅释放块子树。 */
    active = -1;
    for (int i = 0; i < TERM_MAX; i++)
        if (terms[i].in_use) { active = i; break; }
    if (active >= 0) gui_set_key_hook(t_key, &terms[active]);
    else gui_set_key_hook(0, 0);
    gui_mark_dirty_union(0, rx, ry, rw, rh);
}

/* ---- 行编辑 ------------------------------------------------------------- */

static void t_insert(term_t *t, char c) {
    if (t->len >= T_LINE - 1) return;
    for (int i = t->len; i > t->pos; i--) t->line[i] = t->line[i - 1];
    t->line[t->pos++] = c;
    t->len++;
    t->line[t->len] = 0;
}

static void t_backspace(term_t *t) {
    if (t->pos <= 0) return;
    for (int i = t->pos - 1; i < t->len - 1; i++) t->line[i] = t->line[i + 1];
    t->len--; t->pos--;
    t->line[t->len] = 0;
}

static void t_submit(term_t *t) {
    char echo[T_COLS];
    t_copy(echo, sizeof(echo), t->cwd);
    t_cat(echo, sizeof(echo), "$ ");
    t_cat(echo, sizeof(echo), t->line);
    t_ring_push(t, echo);
    t_exec(t, t->line);
    t->len = 0; t->pos = 0; t->line[0] = 0;
    t_update_status(t);
}

static int t_key(int code, void *ud) {
    term_t *t = (term_t *)ud;
    if (!t || !t->in_use) return 0;

    if (code == 14) { gui_term_open(); return 1; }        /* Ctrl+N 新建 */
    if (code == 27) { gui_term_close_active(); return 1; } /* Esc 只关当前 */

    switch (code) {
    case 10:  t_submit(t); break;
    case 8:   t_backspace(t); break;
    case KB_LEFT:  if (t->pos > 0) t->pos--; break;
    case KB_RIGHT: if (t->pos < t->len) t->pos++; break;
    case KB_HOME:  t->pos = 0; break;
    case KB_END:   t->pos = t->len; break;
    case KB_UP:
        if (t->hist_idx > 0) {
            t->hist_idx--;
            t_copy(t->line, T_LINE, t->hist[t->hist_idx]);
            t->len = t_len(t->line); t->pos = t->len;
        }
        break;
    case KB_DOWN:
        if (t->hist_idx < t->hist_n) {
            t->hist_idx++;
            if (t->hist_idx == t->hist_n) { t->line[0] = 0; t->len = 0; }
            else t_copy(t->line, T_LINE, t->hist[t->hist_idx]);
            t->len = t_len(t->line); t->pos = t->len;
        }
        break;
    default:
        if (code >= 0x20 && code <= 0x7E) t_insert(t, (char)code);
        else return 0;
    }
    block *b = btable_get(t->body);
    if (b) b->dirty_paint = 1;
    return 1;
}

/* ---- 探针读取口径 -------------------------------------------------------- */

int gui_term_count(void) {
    int n = 0;
    for (int i = 0; i < TERM_MAX; i++) if (terms[i].in_use) n++;
    return n;
}

int gui_term_active_idx(void) { return active; }

void gui_term_run(int i, const char *cmd) {
    if (i < 0 || i >= TERM_MAX || !terms[i].in_use) return;
    char echo[T_COLS];
    t_copy(echo, sizeof(echo), terms[i].cwd);
    t_cat(echo, sizeof(echo), "$ ");
    t_cat(echo, sizeof(echo), cmd ? cmd : "");
    t_ring_push(&terms[i], echo);
    t_exec(&terms[i], cmd ? cmd : "");
    t_update_status(&terms[i]);
}

const char *gui_term_cwd(int i) {
    if (i < 0 || i >= TERM_MAX || !terms[i].in_use) return "";
    return terms[i].cwd;
}

const char *gui_term_last_line(int i) {
    if (i < 0 || i >= TERM_MAX || !terms[i].in_use) return "";
    return t_ring_at(&terms[i], terms[i].ring_n - 1);
}

/* 复位（进桌面 / 退桌面时清空会话句柄，缓冲槽位保留供探针复用）。 */
void gui_term_reset(void) {
    for (int i = 0; i < TERM_MAX; i++) {
        if (terms[i].root) b_remove(terms[i].root);
        terms[i].root = terms[i].body = terms[i].status = 0;
        terms[i].in_use = 0;
    }
    active = -1;
    gui_set_key_hook(0, 0);
}
