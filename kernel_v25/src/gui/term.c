/*
 * term.c -- BLK_TERM 多会话块内终端（规格 §7 组件层）。
 *
 * 每个会话独立持有：当前目录、200 行回看环、历史环、单行编辑缓冲。
 * vc_backend 复用 shell.c 全部内置：命令行经 shell_ssh_exec_line 提交，
 * 输出经 tee 捕获进本会话行环，不直写真实控制台；执行前把全局 cwd 切到
 * 会话目录，执行后回读。行编辑 Left/Right/Home/End/Backspace/Up/Down；
 * Tab 不补全。输出侧剥离 SGR 与其余转义序列（不画 `?`）。Ctrl+N 新建，
 * Esc 只关当前会话；重开会话清空回看环/历史/输入行，仅保留该槽位的
 * 工作目录（cwd）。
 */
#include "gui/gui.h"
#include "gui_int.h"
#include "gui/tokens.h"
#include "fs.h"
#include "keyboard.h"
#include "serial_log.h"
#include <stdint.h>
#include <stddef.h>
#include "kstring.h"  /* memset() decl; <string.h> pulls host glibc headers */

/* shell.c 远程命令执行入口：复用同一套内置分发，输出经 tee 捕获到缓冲。 */
extern int shell_ssh_exec_line(const char *line, char *out, int outmax,
                                int *rc);

#define TERM_MAX   4
#define T_COLS     80
#define T_ROWS     30
#define T_RING     200
#define T_HIST     32
#define T_LINE     80
/* 体块尺寸必须含 T_PAD：t_render 从 (b->x+T_PAD,b->y+T_PAD) 起画 30 行
 * （29 回看 + 1 输入），此前少算一行与双侧 padding，输入行被画到块外。 */
#define T_BODY_W   (T_COLS * 8 + T_PAD * 2)
#define T_BODY_H   (T_ROWS * 16 + T_PAD * 2)
#define T_TITLE_H  24
#define T_STAT_H   16
#define T_PAD      4

typedef struct {
    int       in_use;
    int       num;                      /* 展示编号 1..4；关闭不重排，空槽取最小可用 */
    block_id  root, body, status, title_blk;
    char      cwd[FS_PATH_MAX];
    char      ring[T_RING][T_COLS];
    int       ring_tail, ring_n;
    char      hist[T_HIST][T_LINE];
    int       hist_n;
    int       hist_idx;
    char      line[T_LINE];
    char      title_buf[32];            /* b_text 只存指针：标题/状态必须常驻 */
    char      status_buf[T_COLS];
    int       len, pos;
    int       view_top;                 /* 0 = 跟随末尾 */
} term_t;

static term_t terms[TERM_MAX];
static int    active = -1;

/* ---- 小字符串助手 ------------------------------------------------------ */

static int strcmp_(const char *a, const char *b);

static void t_ser_dec(uint32_t v) {
    char buf[12]; int n = 0;
    if (v == 0) { ser_putc('0'); return; }
    while (v) { buf[n++] = (char)('0' + v % 10u); v /= 10u; }
    while (n) ser_putc(buf[--n]);
}
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

/* 捕获输出入环：剥离 ESC 序列（SGR 等）与 \r / 其余 C0 控制字符，
 * 换行拆行；超宽截断，不做换行折叠。 */
static void t_ring_push(term_t *t, const char *text) {
    const char *p = text;
    while (*p) {
        int o = 0;
        while (*p && *p != '\n') {
            if (*p == 0x1B) {                 /* 转义序列：跳到终止字母 */
                p++;
                if (*p == '[') {
                    p++;
                    while (*p && !((*p >= 'A' && *p <= 'Z') ||
                                    (*p >= 'a' && *p <= 'z'))) p++;
                }
                if (*p) p++;
                continue;
            }
            if (*p == '\r') { p++; continue; }
            if (*p < 0x20)  { p++; continue; }
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

static int strcmp_(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void t_exec(term_t *t, const char *raw);
/* 小数字直查：之前用 num[] 倒序拼串没有 NUL 中止，t_cat 读越界会吞掉后续字符 */
static const char *t_digit(int v) {
    static const char *d[5] = { "0", "1", "2", "3", "4" };
    return (v >= 0 && v <= 4) ? d[v] : "?";
}
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

    /* clear/cls 只清本会话行环；其余命令交 shell.c 全内置分发。 */
    if (!strcmp_(line, "clear") || !strcmp_(line, "cls")) {
        t_ring_clear(t);
        return;
    }

    fs_chdir(t->cwd);                       /* 全局 cwd 对齐本会话 */
    char cap[2048];
    int rc = 0;
    shell_ssh_exec_line(line, cap, sizeof(cap), &rc);
    t_ring_push(t, cap);
    char pwd[FS_PATH_MAX];
    fs_pwd(pwd, sizeof(pwd));
    if (pwd[0]) t_copy(t->cwd, sizeof(t->cwd), pwd);
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

    /* 输出区行数按已产出内容动态计：输入行紧跟最后一行输出（真实 shell
     * 语义），窗口空时输入行在第 0 行；内容满屏后取最后 T_ROWS-1 行，
     * 输入行自然停在底行，等价于整体上滚。此前固定 29 行输出 + 底行输入，
     * 空会话时输入行悬在窗口底部、中间一片空白。 */
    int show_limit = T_ROWS - 1;
    int used = t->ring_n < show_limit ? t->ring_n : show_limit;
    int first = t->ring_n - used;
    char line[T_COLS + 2];

    for (int v = 0; v < used; v++) {
        const char *src = t_ring_at(t, first + v);
        t_copy(line, sizeof(line), src);
        gui_draw_text(b->x + T_PAD, b->y + T_PAD + v * 16, line, fg, 1);
    }

    /* 输入行：提示符 + 编辑缓冲 + 光标竖线。 */
    int row = used;
    char prompt[T_COLS];
    t_copy(prompt, sizeof(prompt), "");       /* Windows 风格：输入行只留 ">"，cwd 见状态行 */
    t_cat(prompt, sizeof(prompt), "> ");
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
    char *buf = t->status_buf;
    int used = 0;
    for (int i = 0; i < TERM_MAX; i++) if (terms[i].in_use) used++;
    t_copy(buf, sizeof(t->status_buf), "Terminal ");
    t_cat(buf, sizeof(t->status_buf), t_digit(t->num));
    t_cat(buf, sizeof(t->status_buf), "/");
    t_cat(buf, sizeof(t->status_buf), t_digit(used));
    t_cat(buf, sizeof(t->status_buf), "   ");
    t_cat(buf, sizeof(t->status_buf), t->cwd);
    st->text = buf;
    st->dirty_paint = 1;
}

/* 标题条自绘：聚焦实例用 accent 底高亮，其余回退 bar 令牌色。 */
static void t_title_render(block_id id) {
    block *b = btable_get(id);
    if (!b) return;
    term_t *t = (term_t *)b->usr;
    const struct style *tok = gui_active_style();
    int active_idx = (t && t->in_use) ? (int)(t - terms) : -1;
    uint32_t c = (active_idx == active) ? tok->accent : tok->bar;
    gui_fill_rect(b->x, b->y, b->w, b->h, c);
}

/* 聚焦某实例：切 active + 切键盘钩子 + 置前；新旧标题条补脏换高亮。 */
static void t_focus(int idx) {
    if (idx < 0 || idx >= TERM_MAX || !terms[idx].in_use) return;
    int prev = active;
    active = idx;
    gui_set_key_hook(t_key, &terms[idx]);
    gui_raise_to_front(terms[idx].root);
    if (prev >= 0 && terms[prev].in_use) {
        block *pb = btable_get(terms[prev].title_blk);
        if (pb) pb->dirty_paint = 1;
    }
    block *nb = btable_get(terms[idx].title_blk);
    if (nb) nb->dirty_paint = 1;
    ser_write("TERM:focus idx="); t_ser_dec((uint32_t)idx); ser_write("\n");
}

static void t_root_click(block_id id, int32_t lx, int32_t ly) {
    (void)lx; (void)ly;
    block *b = btable_get(id);
    term_t *t = b ? (term_t *)b->usr : 0;
    if (!t || !t->in_use) return;
    t_focus((int)(t - terms));
}

static void t_close_inst(int idx) {
    if (idx < 0 || idx >= TERM_MAX || !terms[idx].in_use) return;
    term_t *t = &terms[idx];
    int rx = 0, ry = 0, rw = 0, rh = 0;
    block *pr = btable_get(t->root);
    if (pr) { rx = pr->x; ry = pr->y; rw = pr->w; rh = pr->h; }
    if (t->root) { b_remove(t->root); t->root = 0; }
    t->body = 0; t->status = 0; t->title_blk = 0;
    t->in_use = 0;
    ser_write("TERM:close idx="); t_ser_dec((uint32_t)idx); ser_write("\n");
    /* 仅释放块子树；槽位内 cwd 保留，行环/历史/输入行在下次 open 时清空。 */
    if (active == idx) {
        active = -1;
        for (int i = 0; i < TERM_MAX; i++)
            if (terms[i].in_use) { active = i; break; }
        if (active >= 0) gui_set_key_hook(t_key, &terms[active]);
        else gui_set_key_hook(0, 0);
    }
    gui_mark_dirty_union(0, rx, ry, rw, rh);
}

/* ---- 打开 / 关闭 --------------------------------------------------------- */

static void t_close_click(block_id id, int32_t lx, int32_t ly) {
    (void)lx; (void)ly;
    block *b = btable_get(id);
    term_t *t = b ? (term_t *)b->usr : 0;
    if (!t || !t->in_use) return;
    t_close_inst((int)(t - terms));
}

static void t_build(int idx) {
    term_t *t = &terms[idx];
    block_id root_id = gui_root_block()->id;

    /* 高度 = pad + 标题 + gap + 体块 + gap + 状态 + pad（gap=pad=T_PAD）。 */
    blk_spec root = t_panel(LAYER_WORK, T_BODY_W + T_PAD * 2,
                            T_TITLE_H + T_BODY_H + T_STAT_H + T_PAD * 4, 1);
    root.pad_l = T_PAD; root.pad_r = T_PAD;
    root.pad_t = T_PAD; root.pad_b = T_PAD;
    root.gap = T_PAD;
    root.focusable = 1;
    root.on_click = t_root_click;
    root.usr = t;
    t->root = b_column(root_id, &root);
    block *pr = btable_get(t->root);
    pr->x = 36 + idx * 20;
    pr->y = 28 + idx * 16;
    pr->moved = 1;
    pr->z = layer_max_z(LAYER_WORK) + 1;

    blk_spec title = t_panel(LAYER_WORK, T_BODY_W, T_TITLE_H, 0);
    title.role_bg = ROLE_BAR;
    title.align_cross = XALN_CENTER;
    title.align_main = ALN_SPACE_BETWEEN;   /* 左标签 + 右 Close 分列，Close 右缘贴窗口右边距 */
    title.render = t_title_render;
    title.usr = t;
    block_id bar = b_row(t->root, &title);
    t->title_blk = bar;
    char *tt = t->title_buf;
    t_copy(tt, sizeof(t->title_buf), "Terminal ");
    t_cat(tt, sizeof(t->title_buf), t_digit(t->num));
    blk_spec ttxt = t_panel(LAYER_WORK, 0, T_TITLE_H - 4, 0);
    ttxt.role_bg = ROLE_NONE;
    b_text(bar, &ttxt, tt);
    blk_spec close = t_panel(LAYER_WORK, 56, T_TITLE_H - 4, 1);
    close.on_click = t_close_click;
    close.usr = t;
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
    if (idx < 0) {
        /* 到上限：向当前聚焦实例回一行提示，不崩、不开新槽。 */
        if (active >= 0 && terms[active].in_use) {
            t_ring_push(&terms[active], "terminal limit reached (4)");
            block *b = btable_get(terms[active].body);
            if (b) b->dirty_paint = 1;
        }
        ser_write("TERM:full\n");
        return;
    }

    term_t *t = &terms[idx];
    t->in_use = 1;
    t->num = idx + 1;                       /* 空槽取最小可用号；关闭后其余编号不重排 */
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
    ser_write("TERM:open idx="); t_ser_dec((uint32_t)idx);
    ser_write(" cwd="); ser_write(t->cwd); ser_write("\n");
}

void gui_term_close_active(void) {
    t_close_inst(active);
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
    t_copy(echo, sizeof(echo), "> ");
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
    t_copy(echo, sizeof(echo), "> ");
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

int gui_term_lines(int i) {
    if (i < 0 || i >= TERM_MAX || !terms[i].in_use) return 0;
    return terms[i].ring_n;
}

const char *gui_term_line(int i, int idx) {
    if (i < 0 || i >= TERM_MAX || !terms[i].in_use) return "";
    return t_ring_at(&terms[i], idx);
}

/* ---- 多实例隔离探针读取/注入口径（不进 CE ABI） -------------------------- */

void gui_term_focus(int i) {
    if (i >= 0 && i < TERM_MAX) t_focus(i);
}

void gui_term_close_inst(int i) {
    if (i >= 0 && i < TERM_MAX) t_close_inst(i);
}

void gui_term_type(int i, const char *s) {
    if (i < 0 || i >= TERM_MAX || !terms[i].in_use || !s) return;
    while (*s) { t_insert(&terms[i], *s); s++; }
    block *b = btable_get(terms[i].body);
    if (b) b->dirty_paint = 1;
}

const char *gui_term_line_buf(int i) {
    if (i < 0 || i >= TERM_MAX || !terms[i].in_use) return "";
    return terms[i].line;
}

int gui_term_hist_n(int i) {
    if (i < 0 || i >= TERM_MAX || !terms[i].in_use) return 0;
    return terms[i].hist_n;
}

/* 复位（进桌面 / 退桌面时清空会话句柄，缓冲槽位保留供探针复用）。 */
void gui_term_reset(void) {
    for (int i = 0; i < TERM_MAX; i++) {
        if (terms[i].root) b_remove(terms[i].root);
        terms[i].root = terms[i].body = terms[i].status = terms[i].title_blk = 0;
        terms[i].in_use = 0;
    }
    active = -1;
    gui_set_key_hook(0, 0);
}
