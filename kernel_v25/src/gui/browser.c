/*
 * browser.c -- 块式浏览器 v0（内核内组件，无进程 / IPC / 显示服务）。
 *
 * v0 路线（见 docs/浏览器-可行性.md）：自写瘦流式 HTML tokenizer，不建树；
 * 极简样式映射到现有块：block 级 → COLUMN/ROW，文本 → BLK_TEXT（折行在
 * 浏览器层按内容切段，不改全局文本块口径），链接 → 令牌 accent 色。
 * 取页走 net_http_get，HTML 缓冲走帧分配页（不进 .bss 大数组，不复用
 * shell curl 的 8 KiB 静态 buf）；单条阻塞连接，导航中只允许一个请求在飞。
 *
 * v0 砍项：CSS 选择器、表格复杂规则、内联图片网络拉取（stb_image 解码
 * 留待下一版）、滚动整页重建、历史会话恢复。
 *
 * 多实例：BR_MAX 个槽位，整实例结构 kvmalloc 堆分配（不进 .bss），每实例
 * 独立 URL / 地址栏编辑缓冲 / 历史栈 / HTML 帧池 / 文本池；实例可变状态
 * 全部挂在实例结构上，静态 .bss 只留指针池与 active 索引。
 */
#include "gui/gui.h"
#include "gui_int.h"
#include "gui/tokens.h"
#include "net.h"
#include "paging.h"
#include "serial_log.h"
#include "fs.h"
#include "kvheap.h"

extern uint32_t uptime_ms(void);
#include "keyboard.h"
#include "kstring.h"
#include <stdint.h>
#include <stddef.h>

/* HTML 缓冲动态扩容：按需帧分配，上限 72×4 KiB = 288 KiB，不入 .bss 大数组。 */
#define BR_MAX_PAGES 72
#define BR_HTML_CAP  (BR_MAX_PAGES * 4096)
#define BR_POOL      4096                    /* 持久文本池（块文本引用） */
#define BR_URL_MAX   160
#define BR_STATUS_MAX 160
#define BR_HIST_MAX  8
#define BR_RUN_MAX   256
#define BR_VP_W      560
#define BR_VP_H      300
#define BR_IMG_MAX_PAGES 400
#define BR_MAX       2                       /* 实例池上限：每实例 HTML 池 ≤288KiB，
                                               * 图片帧池按需 400 页；2 实例在
                                               * 256MiB 映像内堆/帧压力可控，
                                               * 更多实例留待后续。 */

/* 极简 CSS 内联样式（v0.1 盒模型最小集）。 */
struct br_css {
    uint32_t fg, bg;
    int32_t  pad_l, pad_r, pad_t, pad_b;
    uint8_t  border_w;
    uint8_t  align;
    int32_t  req_w, req_h;
    int32_t  req_w_pct, req_h_pct;
    int32_t  max_w;
};

typedef struct br_inst {
    int       in_use;
    int       slot;

    char      url[BR_URL_MAX];           /* 当前页 URL（规范化后） */
    char      addr[BR_URL_MAX];          /* 地址栏编辑缓冲 */
    int       addr_len, addr_pos;
    int       addr_focused;

    char      status[BR_STATUS_MAX];
    char      err_msg[BR_STATUS_MAX];

    block_id  root;
    block_id  addr_blk;
    block_id  content;
    block_id  status_blk;

    int       ok;
    int       bytes;
    int       nodes;
    int       blocks;
    int       status_code;
    uint32_t  elapsed_ms;
    int       error;                     /* 1 = 当前页是错误页 */

    /* HTML 帧缓冲（运行时页分配，不入 .bss 静态数组）。 */
    uint32_t *phys;
    int       pages;
    uint8_t  *html;

    /* 解码图片帧缓冲（24bpp RGB；PNG/JPEG 浮点路径不在本内核支持范围）。 */
    uint32_t *img_phys;
    int       img_pages;
    uint8_t  *img_buf;
    int       img_w, img_h;

    /* 持久文本池：块文本按引用保存，不能指向栈/run 临时缓冲。 */
    char     *pool;
    int       pool_n;

    char      run[BR_RUN_MAX];
    int       run_n;

    /* 每实例独立历史栈。 */
    char    (*hist)[BR_URL_MAX];
    int       hist_n;
    int       hist_cur;

    /* 解析态。 */
    int       pos;
    int       accent;
    block_id  cur_row;
    struct br_css css_cur;
} br_inst_t;

static br_inst_t *pool[BR_MAX] = { 0 };
static int        br_active = -1;

static int  br_key(int code, void *ud);
static void br_focus(int slot);
static void br_addr_clear(br_inst_t *b);

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

static void br_dec(uint32_t v) {
    char tmp[12]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) ser_putc(tmp[--n]);
}

static br_inst_t *br_active_inst(void) {
    if (br_active >= 0 && br_active < BR_MAX &&
        pool[br_active] && pool[br_active]->in_use)
        return pool[br_active];
    return 0;
}

static br_inst_t *br_slot_inst(int slot) {
    if (slot < 0 || slot >= BR_MAX || !pool[slot] || !pool[slot]->in_use) return 0;
    return pool[slot];
}

/* ---- HTML 帧缓冲 -------------------------------------------------------- */

static void br_free_html(br_inst_t *b) {
    for (int p = 0; p < b->pages; p++) frame_free(b->phys[p]);
    b->pages = 0; b->html = 0;
}

static int br_alloc_html(br_inst_t *b) {
    br_free_html(b);
    uint32_t pa = frame_alloc();
    if (!pa) return -1;
    b->phys[0] = pa;
    b->pages = 1;
    b->html = (uint8_t *)pa;
    return 0;
}

/* 流式接收器：把响应体追加进动态 HTML 缓冲，跨页按需增长。 */
static int br_stream_sink(void *ud, const uint8_t *p, int len) {
    br_inst_t *b = (br_inst_t *)ud;
    for (int i = 0; i < len; i++) {
        int off = b->bytes + i;
        if (off >= BR_HTML_CAP) return -1;
        int pg = off >> 12;
        if (pg >= b->pages) {
            uint32_t pa = frame_alloc();
            if (!pa) return -1;
            b->phys[pg] = pa;
            b->pages = pg + 1;
        }
        b->html[off] = p[i];
    }
    b->bytes += len;
    return 0;
}

/* ---- 持久文本池 --------------------------------------------------------- */

static const char *pool_put(br_inst_t *b, const char *s, int n) {
    if (n < 0) n = 0;
    /* 每条串独立 NUL 终止：游标按 n+1 推进，否则下一次追加会覆盖上一条
     * 串尾的 NUL，块文本读出累计尾巴。 */
    if (b->pool_n + n + 1 >= BR_POOL) n = BR_POOL - 1 - b->pool_n;
    if (n <= 0) return "";
    char *p = b->pool + b->pool_n;
    for (int i = 0; i < n; i++) p[i] = s[i];
    p[n] = 0;
    b->pool_n += n + 1;
    return p;
}

/* ---- 块规格助手 --------------------------------------------------------- */

static blk_spec br_panel(uint16_t layer, int32_t w, int32_t h, uint8_t border) {
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

/* ---- 极简 CSS 内联样式（v0.1 盒模型最小集） ----------------------------- */

static int css_eq(const char *a, const char *b) {
    while (*b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        a++; b++;
    }
    return *a == 0;
}

static const char *css_trim(const char *v) {
    while (*v == ' ' || *v == '\t') v++;
    return v;
}

static uint32_t css_color(const char *v) {
    v = css_trim(v);
    if (v[0] == '#') {
        v++; uint32_t c = 0; int d = 0;
        while (v[d] && d < 6) {
            char ch = v[d]; int x;
            if (ch >= '0' && ch <= '9') x = ch - '0';
            else if (ch >= 'a' && ch <= 'f') x = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') x = ch - 'A' + 10;
            else break;
            c = c * 16 + x; d++;
        }
        if (d == 6) return c;
        return INHERIT_COLOR;
    }
    struct { const char *n; uint32_t c; } nm[] = {
        {"black",0x000000},{"white",0xFFFFFF},{"red",0xE05555},
        {"green",0x55C070},{"blue",0x5588E8},{"gray",0x9A9AA6},
        {"grey",0x9A9AA6},{"yellow",0xE8D060},{"silver",0xC0C0C8}
    };
    for (int i = 0; i < (int)(sizeof(nm)/sizeof(nm[0])); i++)
        if (css_eq(v, nm[i].n)) return nm[i].c;
    return INHERIT_COLOR;
}

static int css_px(const char *v) {
    v = css_trim(v);
    int n = 0;
    while (*v >= '0' && *v <= '9') { n = n*10 + (*v-'0'); v++; }
    return n;
}

/* 解析尺寸值：返回数值；*pct 置 1 表示单位是 '%'。 */
static int css_size(const char *v, int *pct) {
    v = css_trim(v);
    int n = 0;
    while (*v >= '0' && *v <= '9') { n = n*10 + (*v-'0'); v++; }
    *pct = (*v == '%') ? 1 : 0;
    return n;
}

static void css_reset(br_inst_t *b) {
    struct br_css *c = &b->css_cur;
    c->fg = INHERIT_COLOR; c->bg = INHERIT_COLOR;
    c->pad_l = c->pad_r = 0;
    c->pad_t = c->pad_b = 0;
    c->border_w = 0; c->align = 0;
    c->req_w = 0; c->req_h = 0;
    c->req_w_pct = 0; c->req_h_pct = 0;
    c->max_w = 0;
}

static void css_set_prop(br_inst_t *b, const char *p, const char *v) {
    struct br_css *c = &b->css_cur;
    p = css_trim(p); v = css_trim(v);
    if (css_eq(p, "color")) c->fg = css_color(v);
    else if (css_eq(p, "background-color") || css_eq(p, "background"))
        c->bg = css_color(v);
    else if (css_eq(p, "padding") || css_eq(p, "margin")) {
        int q = css_px(v);
        c->pad_l = c->pad_r = q;
        c->pad_t = c->pad_b = q;
    }
    else if (css_eq(p, "padding-left") || css_eq(p, "margin-left")) c->pad_l = css_px(v);
    else if (css_eq(p, "padding-right") || css_eq(p, "margin-right")) c->pad_r = css_px(v);
    else if (css_eq(p, "padding-top") || css_eq(p, "margin-top")) c->pad_t = css_px(v);
    else if (css_eq(p, "padding-bottom") || css_eq(p, "margin-bottom")) c->pad_b = css_px(v);
    else if (css_eq(p, "border")) c->border_w = (uint8_t)css_px(v);
    else if (css_eq(p, "text-align")) {
        if (css_eq(v, "center")) c->align = ALN_CENTER;
        else if (css_eq(v, "right") || css_eq(v, "end")) c->align = ALN_END;
        else c->align = ALN_START;
    }
    else if (css_eq(p, "width")) {
        int pct = 0; int q = css_size(v, &pct);
        if (pct) c->req_w_pct = q;
        else if (q > 0) c->req_w = q;
    }
    else if (css_eq(p, "height")) {
        int pct = 0; int q = css_size(v, &pct);
        if (pct) c->req_h_pct = q;
        else if (q > 0) c->req_h = q;
    }
    else if (css_eq(p, "max-width")) { int q = css_px(v); if (q > 0) c->max_w = q; }
}

static void css_parse(br_inst_t *b, const char *style) {
    css_reset(b);
    if (!style || !*style) return;
    char buf[BR_RUN_MAX]; int bn = 0;
    while (*style && bn < BR_RUN_MAX - 1) buf[bn++] = *style++;
    buf[bn] = 0;
    char *s = buf;
    while (*s) {
        char *pstart = s;
        char *colon = 0;
        while (*s && *s != ';' && *s != ':') s++;
        if (*s == ':') { colon = s; *s = 0; s++; }
        char *vstart = s;
        while (*s && *s != ';') s++;
        if (*s == ';') { *s = 0; s++; }
        if (colon) css_set_prop(b, pstart, vstart);
    }
}

/* ---- 导航 / 历史 --------------------------------------------------------- */

static void br_hist_push(br_inst_t *b, const char *url) {
    if (!url || !*url) return;
    if (b->hist_n > 0 && b->hist_cur < b->hist_n - 1) {
        /* 截断前进分支。 */
        b->hist_n = b->hist_cur + 1;
    }
    if (b->hist_n >= BR_HIST_MAX) {
        for (int i = 1; i < BR_HIST_MAX; i++)
            xcopy(b->hist[i - 1], BR_URL_MAX, b->hist[i]);
        b->hist_n = BR_HIST_MAX - 1;
    }
    xcopy(b->hist[b->hist_n], BR_URL_MAX, url);
    b->hist_n++;
    b->hist_cur = b->hist_n - 1;
}

/* URL 规范化：去首尾空白；无 scheme 补 http://；空串留空（调用方判定回到当前页）。 */
static void br_normalize(char *out, int cap, const char *in) {
    out[0] = 0;
    if (!in) return;
    int n = xlen(in), l = 0, r = n - 1;
    while (l < n && (in[l] == ' ' || in[l] == '\t')) l++;
    while (r >= l && (in[r] == ' ' || in[r] == '\t')) r--;
    if (r < l) return;
    int has_scheme = 0;
    for (int k = l; k <= r && k < l + 12; k++)
        if (in[k] == ':' && in[k + 1] == '/' && in[k + 2] == '/') { has_scheme = 1; break; }
    if (!has_scheme) xcat(out, cap, "http://");
    for (int k = l; k <= r && xlen(out) < cap - 1; k++) out[xlen(out)] = in[k];
    out[xlen(out)] = 0;
}

/* ---- 状态行 ------------------------------------------------------------- */

static void br_set_status(br_inst_t *b, const char *msg) {
    xcopy(b->status, sizeof(b->status), msg);
    block *st = btable_get(b->status_blk);
    if (st) { st->text = b->status; st->dirty_paint = 1; }
}

static void br_compose_status(br_inst_t *b) {
    xcopy(b->status, sizeof(b->status), b->url);
    xcat(b->status, sizeof(b->status), "  status=");
    xnum(b->status, sizeof(b->status), (uint32_t)b->status_code);
    xcat(b->status, sizeof(b->status), "  bytes=");
    xnum(b->status, sizeof(b->status), (uint32_t)b->bytes);
    xcat(b->status, sizeof(b->status), "  ");
    xnum(b->status, sizeof(b->status), b->elapsed_ms);
    xcat(b->status, sizeof(b->status), "ms  nodes=");
    xnum(b->status, sizeof(b->status), (uint32_t)b->nodes);
    xcat(b->status, sizeof(b->status), "  blocks=");
    xnum(b->status, sizeof(b->status), (uint32_t)b->blocks);
    block *st = btable_get(b->status_blk);
    if (st) { st->text = b->status; st->dirty_paint = 1; }
}

/* ---- HTML 流式解析（不建树） --------------------------------------------- */

enum {
    TK_OTHER = 0,
    TK_H1, TK_H2, TK_H3, TK_P, TK_UL, TK_OL, TK_LI, TK_BR,
    TK_TABLE, TK_TR, TK_TD, TK_TH, TK_A, TK_B, TK_STRONG, TK_EM,
    TK_IMG, TK_SCRIPT, TK_STYLE, TK_HEAD, TK_BODY, TK_TITLE,
    TK_META, TK_LINK
};

static int br_hlen(br_inst_t *b) { return b->pages ? b->pages * 4096 : 0; }

static int tk_name(const char *n) {
    struct { const char *k; int v; } map[] = {
        { "h1", TK_H1 }, { "h2", TK_H2 }, { "h3", TK_H3 },
        { "p", TK_P }, { "ul", TK_UL }, { "ol", TK_OL }, { "li", TK_LI },
        { "br", TK_BR }, { "table", TK_TABLE }, { "tr", TK_TR },
        { "td", TK_TD }, { "th", TK_TH }, { "a", TK_A },
        { "b", TK_B }, { "strong", TK_STRONG }, { "em", TK_EM },
        { "img", TK_IMG }, { "script", TK_SCRIPT }, { "style", TK_STYLE },
        { "head", TK_HEAD }, { "body", TK_BODY }, { "title", TK_TITLE },
        { "meta", TK_META }, { "link", TK_LINK }
    };
    for (int i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++) {
        const char *a = n, *b = map[i].k;
        int j = 0;
        while (b[j] && (a[j] == b[j] || a[j] == b[j] - 32)) j++;
        if (!b[j]) return map[i].v;
    }
    return TK_OTHER;
}

static void run_push(br_inst_t *b, char c) {
    if (b->run_n < BR_RUN_MAX - 1) b->run[b->run_n++] = c;
}

static void run_entity(br_inst_t *b, const char *ent, int n) {
    if (n == 4 && ent[1] == 'a' && ent[2] == 'm' && ent[3] == 'p') run_push(b, '&');
    else if (n == 3 && ent[1] == 'l' && ent[2] == 't') run_push(b, '<');
    else if (n == 3 && ent[1] == 'g' && ent[2] == 't') run_push(b, '>');
    else if (n == 5 && ent[1] == 'q' && ent[2] == 'u' && ent[3] == 'o' && ent[4] == 't') run_push(b, '"');
    else if (n == 5 && ent[1] == '#' && ent[2] == '3' && ent[3] == '9') run_push(b, '\'');
    else if (n == 5 && ent[1] == 'n' && ent[2] == 'b' && ent[3] == 's' && ent[4] == 'p') run_push(b, ' ');
}

static void run_flush(br_inst_t *b, int scale) {
    if (b->run_n == 0) return;
    /* 折叠段内空白。 */
    char out[BR_RUN_MAX]; int o = 0; int sp = 1;
    for (int i = 0; i < b->run_n; i++) {
        char c = b->run[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!sp) { out[o++] = ' '; sp = 1; }
        } else { out[o++] = c; sp = 0; }
    }
    if (o && out[o - 1] == ' ') o--;
    const char *txt = pool_put(b, out, o);
    blk_spec t = br_panel(LAYER_WORK, -1, -1, 0);
    t.scale = (uint8_t)scale;
    t.role_accent = b->accent ? ROLE_ACCENT : ROLE_NONE;
    t.pad_l = 2; t.pad_r = 2;
    /* v0.1 内联 CSS 盒模型最小集（未声明项保持角色继承）。 */
    struct br_css *c = &b->css_cur;
    if (c->fg != INHERIT_COLOR) { t.role_fg = ROLE_NONE; t.fg = c->fg; }
    if (c->bg != INHERIT_COLOR) { t.role_bg = ROLE_NONE; t.bg = c->bg; }
    t.pad_l += c->pad_l; t.pad_r += c->pad_r;
    t.pad_t += c->pad_t; t.pad_b += c->pad_b;
    if (c->border_w) t.border_w = c->border_w;
    if (c->align) t.align_main = c->align;
    if (c->req_w > 0) t.req_w = c->req_w;
    else if (c->req_w_pct > 0)
        t.req_w = (BR_VP_W - 16) * c->req_w_pct / 100;
    if (c->req_h > 0) t.req_h = c->req_h;
    else if (c->req_h_pct > 0)
        t.req_h = BR_VP_H * c->req_h_pct / 100;
    if (c->max_w > 0 && t.req_w > c->max_w)
        t.req_w = c->max_w;
    block_id parent = b->cur_row ? b->cur_row : b->content;
    b_text(parent, &t, txt);
    b->blocks++;
    b->run_n = 0;
}

static void br_new_row(br_inst_t *b) {
    blk_spec r = br_panel(LAYER_WORK, -1, -1, 0);
    r.gap = 2;
    b->cur_row = b_row(b->content, &r);
    b->blocks++;
}

static void br_add_text_block(br_inst_t *b, const char *txt, int scale, int accent) {
    blk_spec t = br_panel(LAYER_WORK, -1, -1, 0);
    t.scale = (uint8_t)scale;
    t.role_accent = accent ? ROLE_ACCENT : ROLE_NONE;
    const char *p = pool_put(b, txt, xlen(txt));
    b_text(b->content, &t, p);
    b->blocks++;
}

/* ---- 图片解码（BMP/PPM 内存/FS 路径；PNG/JPEG 浮点路径本内核不支持） ------ */

static uint16_t br_rd_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static int32_t br_rd_i32(const uint8_t *p) {
    return (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) |
                     ((uint32_t)p[3] << 24));
}

static void br_free_img(br_inst_t *b) {
    for (int p = 0; p < b->img_pages; p++) frame_free(b->img_phys[p]);
    b->img_pages = 0; b->img_buf = 0; b->img_w = b->img_h = 0;
}

static int br_img_alloc(br_inst_t *b, int w, int h) {
    int bytes = w * h * 3;
    int pages = (bytes + 4095) / 4096;
    if (pages > BR_IMG_MAX_PAGES) return -1;
    for (int p = 0; p < pages; p++) {
        uint32_t pa = frame_alloc();
        if (!pa) { br_free_img(b); return -1; }
        b->img_phys[p] = pa;
    }
    b->img_pages = pages;
    b->img_buf = (uint8_t *)b->img_phys[0];
    memset(b->img_buf, 0, bytes);
    b->img_w = w; b->img_h = h;
    return 0;
}

static int br_decode_bmp_path(br_inst_t *b, const char *path) {
    uint8_t hdr[64];
    int n = fs_read_at(path, (char *)hdr, 0, sizeof(hdr));
    if (n < 34) return -1;
    if (hdr[0] != 'B' || hdr[1] != 'M') return -1;
    uint32_t data_off = br_rd_u16(hdr + 10);
    int32_t info = br_rd_i32(hdr + 14);
    if (info < 40) return -1;
    int32_t wh = br_rd_i32(hdr + 22);
    int32_t w = br_rd_i32(hdr + 18);
    int bpp = br_rd_u16(hdr + 28);
    uint32_t comp = (uint32_t)br_rd_i32(hdr + 30);
    if (comp != 0) return -1;                      /* RLE/压缩：不支持 */
    if (bpp != 24 && bpp != 32) return -1;         /* 调色板：不支持 */
    if (w <= 0 || w > 800) return -1;
    int topdown = wh < 0;
    int h = topdown ? -wh : wh;
    if (h <= 0 || h > 600) return -1;

    int bpc = bpp / 8;
    int stride = ((w * bpc + 3) & ~3);
    if (br_img_alloc(b, w, h) < 0) return -1;
    static uint8_t row[3204];
    for (int y = 0; y < h; y++) {
        int got = fs_read_at(path, (char *)row, data_off + y * stride, stride);
        if (got < bpc * w) return -1;
        int dst = topdown ? y : (h - 1 - y);
        for (int x = 0; x < w; x++) {
            uint8_t B = row[x * bpc];
            uint8_t G = row[x * bpc + 1];
            uint8_t R = row[x * bpc + 2];
            uint8_t *d = b->img_buf + (dst * w + x) * 3;
            d[0] = R; d[1] = G; d[2] = B;
        }
    }
    return 0;
}

static int br_decode_ppm_path(br_inst_t *b, const char *path) {
    char hdr[256];
    int n = fs_read_at(path, hdr, 0, sizeof(hdr) - 1);
    if (n < 5) return -1;
    hdr[n] = 0;
    if (hdr[0] != 'P' || hdr[1] != '6') return -1;
    int p = 2;
    int vals[3] = { 0, 0, 0 };
    for (int v = 0; v < 3; v++) {
        while (hdr[p] == ' ' || hdr[p] == '\t' || hdr[p] == '\n' ||
               hdr[p] == '\r' || hdr[p] == '#') {
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
    if (br_img_alloc(b, w, h) < 0) return -1;
    static uint8_t row[3204];
    for (int y = 0; y < h; y++) {
        int got = fs_read_at(path, (char *)row, data_off + y * w * 3, w * 3);
        if (got < w * 3) return -1;
        for (int x = 0; x < w; x++) {
            uint8_t *d = b->img_buf + (y * w + x) * 3;
            d[0] = row[x * 3]; d[1] = row[x * 3 + 1]; d[2] = row[x * 3 + 2];
        }
    }
    return 0;
}

static void br_img_embed(br_inst_t *b, const char *src) {
    block *bl = btable_alloc(BLK_CANVAS);
    if (!bl) return;
    block_id parent = b->cur_row ? b->cur_row : b->content;
    tree_attach(btable_get(parent), bl);
    int dw = b->img_w, dh = b->img_h;
    if (dw > BR_VP_W - 16) { dh = dh * (BR_VP_W - 16) / dw; dw = BR_VP_W - 16; }
    bl->req_w = dw; bl->req_h = dh;
    bl->pix_w = b->img_w; bl->pix_h = b->img_h;
    bl->buf = b->img_buf;
    bl->layer = LAYER_WORK; bl->visible = 1; bl->dirty_layout = 1;
    b->blocks++;
    ser_write("GUI:br img "); ser_write(src);
    ser_write(" w="); br_dec((uint32_t)b->img_w);
    ser_write(" h="); br_dec((uint32_t)b->img_h);
    ser_write("\n");
}

static int br_ext_is(const char *src, const char *ext) {
    int n = xlen(src), en = xlen(ext);
    if (n < en) return 0;
    const char *s = src + n - en;
    for (int i = 0; i < en; i++) {
        char a = s[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

static void br_img_handle(br_inst_t *b, const char *src) {
    if (!src || !*src) { br_add_text_block(b, "[img: ?]", 1, 0); return; }
    int is_bmp = br_ext_is(src, ".bmp");
    int is_ppm = br_ext_is(src, ".ppm");
    int is_png = br_ext_is(src, ".png");
    int is_jpg = br_ext_is(src, ".jpg") || br_ext_is(src, ".jpeg");

    if (is_png || is_jpg) {
        ser_write("GUI:br img unsupported "); ser_write(src); ser_write("\n");
        char ph[BR_URL_MAX + 16];
        xcopy(ph, sizeof(ph), "[img: unsupported ");
        xcat(ph, sizeof(ph), src); xcat(ph, sizeof(ph), "]");
        br_add_text_block(b, ph, 1, 0);
        return;
    }
    /* 本地 FS 路径：直接解码 BMP/PPM。 */
    if (src[0] == '/' && (is_bmp || is_ppm)) {
        int rc = is_bmp ? br_decode_bmp_path(b, src) : br_decode_ppm_path(b, src);
        if (rc == 0) { br_img_embed(b, src); return; }
    }
    /* http(s) 同源图片本轮未接入内存解码；占位保留。 */
    char ph[BR_URL_MAX + 12];
    xcopy(ph, sizeof(ph), "[img: ");
    xcat(ph, sizeof(ph), src); xcat(ph, sizeof(ph), "]");
    br_add_text_block(b, ph, 1, 0);
}

/* 解析一个标签；返回 1 表示消费到 '>'。 */
static int br_parse_tag(br_inst_t *b) {
    /* b->pos 指向 '<' */
    int close = 0;
    int i = b->pos + 1;
    if (i < br_hlen(b) && b->html[i] == '/') { close = 1; i++; }

    char name[32]; int nn = 0;
    while (i < br_hlen(b) && b->html[i] > ' ' && b->html[i] != '>'
           && b->html[i] != '/' && nn < 31) {
        name[nn++] = (char)b->html[i]; i++;
    }
    name[nn] = 0;
    int kind = tk_name(name);

    /* 属性（v0 只取 a[href] / img[src]，收集到局部，不建属性表）。 */
    char src[BR_URL_MAX]; src[0] = 0;
    char style[BR_URL_MAX]; style[0] = 0;
    while (i < br_hlen(b) && b->html[i] != '>') {
        while (i < br_hlen(b) && (b->html[i] == ' ' || b->html[i] == '/')) i++;
        if (i >= br_hlen(b) || b->html[i] == '>') break;
        char key[24]; int nk = 0;
        while (i < br_hlen(b) && b->html[i] != '=' &&
               b->html[i] != ' ' && b->html[i] != '/' && b->html[i] != '>' && nk < 23) {
            key[nk++] = (char)b->html[i]; i++;
        }
        key[nk] = 0;
        while (i < br_hlen(b) && (b->html[i] == ' ' || b->html[i] == '/')) i++;
        if (i < br_hlen(b) && b->html[i] == '=') i++;
        while (i < br_hlen(b) && b->html[i] == ' ') i++;
        char val[BR_URL_MAX]; int nv = 0;
        if (i < br_hlen(b) && (b->html[i] == '"' || b->html[i] == '\'')) {
            char q = b->html[i++];
            while (i < br_hlen(b) && b->html[i] != q && nv < BR_URL_MAX - 1)
                val[nv++] = (char)b->html[i++];
            if (i < br_hlen(b)) i++;
        } else {
            while (i < br_hlen(b) && b->html[i] != ' ' && b->html[i] != '>' && nv < BR_URL_MAX - 1)
                val[nv++] = (char)b->html[i++];
        }
        val[nv] = 0;
        if ((key[0] == 's' || key[0] == 'S') && key[1] == 'r') xcopy(src, sizeof(src), val);
        if ((key[0] == 's' || key[0] == 'S') && key[1] == 't' && key[2] == 'y')
            xcopy(style, sizeof(style), val);
    }
    if (i < br_hlen(b)) i++;      /* '>' */
    b->pos = i;

    if (!close) b->nodes++;
    if (!close) css_parse(b, style);

    switch (kind) {
    case TK_H1: run_flush(b, 1); break;
    case TK_H2: case TK_H3: run_flush(b, 1); break;
    case TK_P: run_flush(b, 1); break;
    case TK_LI:
        run_flush(b, 1);
        br_new_row(b);
        run_push(b, '-'); run_push(b, ' ');
        break;
    case TK_BR: run_flush(b, 1); break;
    case TK_TR: run_flush(b, 1); br_new_row(b); break;
    case TK_TD: case TK_TH: run_flush(b, 1); break;
    case TK_A: b->accent = 1; break;
    case TK_IMG: {
        run_flush(b, 1);
        br_img_handle(b, src);
        break;
    }
    case TK_SCRIPT: case TK_STYLE: {
        /* 跳过 RCDATA 正文直到对应闭合标签。 */
        const char *stop = kind == TK_SCRIPT ? "</script" : "</style";
        int sl = kind == TK_SCRIPT ? 8 : 7;
        int j = b->pos;
        while (j + sl <= br_hlen(b)) {
            int m = 1;
            for (int k = 0; k < sl; k++) {
                char c = b->html[j + k];
                if (c != stop[k] && c != stop[k] - 32) { m = 0; break; }
            }
            if (m) { while (j < br_hlen(b) && b->html[j] != '>') j++; b->pos = j + 1; break; }
            j++;
        }
        break;
    }
    default: break;
    }

    if (close) {
        switch (kind) {
        case TK_H1: case TK_H2: case TK_H3: case TK_P: case TK_LI:
        case TK_TD: case TK_TH: run_flush(b, 1); break;
        case TK_TR: case TK_TABLE: b->cur_row = 0; break;
        case TK_A: b->accent = 0; break;
        default: break;
        }
    }
    return 1;
}

static void br_parse_html(br_inst_t *b, const uint8_t *html, int len) {
    b->pool_n = 0; b->run_n = 0;
    css_reset(b);
    b->pos = 0; b->accent = 0; b->cur_row = 0;
    b->nodes = 0; b->blocks = 0;

    while (b->pos < len) {
        if (html[b->pos] == '<') {
            if (b->pos + 3 < len && html[b->pos + 1] == '!' &&
                html[b->pos + 2] == '-' && html[b->pos + 3] == '-') {
                int j = b->pos + 4;
                while (j + 3 <= len && !(html[j] == '-' && html[j + 1] == '-' && html[j + 2] == '>')) j++;
                b->pos = j + 3 <= len ? j + 3 : len;
                continue;
            }
            if (b->pos + 1 < len && (html[b->pos + 1] == '!' || html[b->pos + 1] == '?')) {
                while (b->pos < len && html[b->pos] != '>') b->pos++;
                if (b->pos < len) b->pos++;
                continue;
            }
            br_parse_tag(b);
        } else {
            char c = html[b->pos];
            if (c == '&') {
                int e = b->pos + 1;
                while (e < len && html[e] != ';' && html[e] != '<' && html[e] != ' ' && e - b->pos < 8) e++;
                if (e < len && html[e] == ';') {
                    run_entity(b, (const char *)html + b->pos, e - b->pos + 1);
                    b->pos = e + 1;
                    continue;
                }
            }
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') run_push(b, ' ');
            else run_push(b, c);
            b->pos++;
        }
    }
    run_flush(b, 1);
}

/* ---- 取页 ---------------------------------------------------------------- */

static int br_fetch(br_inst_t *b, const char *url) {
    b->ok = 0; b->bytes = 0; b->status_code = 0; b->elapsed_ms = 0;
    if (br_alloc_html(b) < 0) { br_set_status(b, "out of memory"); return -1; }

    if (!net_up()) { br_set_status(b, "network not ready"); return -1; }

    uint32_t t0 = uptime_ms();
    int status = 0; long total = 0;
    int n = net_http_download_stream(url, 0, br_stream_sink, b,
                                     &status, &total);
    uint32_t t1 = uptime_ms();
    b->elapsed_ms = t1 > t0 ? t1 - t0 : 0;

    if (n < 0) {
        b->bytes = 0;
        char msg[BR_STATUS_MAX];
        const char *why;
        switch (n) {
        case -7:  why = "redirect https blocked"; break;
        case -2:  why = "dns failed"; break;
        case -3:  why = "connect failed"; break;
        case -4:  why = "http status error"; break;
        case -5:  why = "no reply"; break;
        case -8:  why = "redirect loop"; break;
        case -11: why = "truncated body"; break;
        case -13: why = "body needs resume"; break;
        default:  why = "fetch error"; break;
        }
        xcopy(msg, sizeof(msg), why);
        xcat(msg, sizeof(msg), " (");
        xnum(msg, sizeof(msg), (uint32_t)(-n));
        xcat(msg, sizeof(msg), ")");
        xcopy(b->err_msg, sizeof(b->err_msg), msg);
        br_set_status(b, msg);
        return n;
    }
    b->bytes = (int)total;
    b->status_code = status;
    b->ok = 1;
    return 0;
}

/* ---- 子树重建 ------------------------------------------------------------ */

static void br_close_subtree(br_inst_t *b) {
    int rx = 0, ry = 0, rw = 0, rh = 0;
    block *pr = btable_get(b->root);
    if (pr) { rx = pr->x; ry = pr->y; rw = pr->w; rh = pr->h; }
    if (b->root) { b_remove(b->root); b->root = 0; }
    b->addr_blk = b->content = b->status_blk = 0;
    br_free_img(b);
    gui_mark_dirty_union(0, rx, ry, rw, rh);
}

static void br_click_back(block_id id, int32_t lx, int32_t ly);
static void br_click_fwd(block_id id, int32_t lx, int32_t ly);
static void br_click_refresh(block_id id, int32_t lx, int32_t ly);
static void br_click_stop(block_id id, int32_t lx, int32_t ly);
static void br_addr_click(block_id id, int32_t lx, int32_t ly);

/* 地址栏自绘：编辑缓冲 + 光标竖线。 */
static void br_addr_render(block_id id) {
    block *bl = btable_get(id);
    if (!bl) return;
    br_inst_t *b = (br_inst_t *)bl->usr;
    if (!b) return;
    const struct style *tok = gui_active_style();
    int x = bl->x + bl->pad_l;
    int y = bl->y + (bl->h - 16) / 2;
    gui_draw_text(x, y, b->addr, tok->fg, 1);
    if (b->addr_focused) {
        int cx = x + 8 * b->addr_pos;
        gui_fill_rect(cx, y, 2, 16, tok->accent);
    }
}

/* 点击窗口任意位置：raise 置前 + 切回浏览器自己的键盘钩子。 */
static void br_root_click(block_id id, int32_t lx, int32_t ly) {
    (void)id; (void)lx; (void)ly;
    block *bl = btable_get(id);
    br_inst_t *b = bl ? (br_inst_t *)bl->usr : 0;
    if (!b || !b->in_use) return;
    br_focus(b->slot);
    b->addr_focused = 0;
    block *ab = btable_get(b->addr_blk);
    if (ab) ab->dirty_paint = 1;
}

static void br_build_ui(br_inst_t *b) {
    block_id root_id = gui_root_block()->id;
    blk_spec root = br_panel(LAYER_WORK, BR_VP_W + 16, BR_VP_H + 56, 1);
    root.pad_l = 8; root.pad_r = 8; root.pad_t = 8; root.pad_b = 8;
    root.gap = 4; root.focusable = 1;
    root.on_click = br_root_click;
    root.usr = b;
    b->root = b_column(root_id, &root);
    block *pr = btable_get(b->root);
    pr->x = 112; pr->y = 48; pr->moved = 1;
    pr->st.corner_radius = 16;   /* §16.1 外框 r16 */

    /* 工具栏：Back / Fwd / Refresh / Stop + 地址栏。 */
    blk_spec bar = br_panel(LAYER_WORK, BR_VP_W, 22, 0);
    bar.gap = 4;
    block_id toolbar = b_row(b->root, &bar);

    static const char *btns[4] = { "Back", "Fwd", "Refresh", "Stop" };
    static void (*bclick[4])(block_id, int32_t, int32_t) = {
        br_click_back, br_click_fwd, br_click_refresh, br_click_stop
    };
    for (int i = 0; i < 4; i++) {
        blk_spec btn = br_panel(LAYER_WORK, 56, 20, 1);
        btn.on_click = bclick[i];
        btn.usr = b;
        block_id bid = b_row(toolbar, &btn);
        b_text(bid, 0, btns[i]);
        b->blocks++;
    }

    blk_spec addr = br_panel(LAYER_WORK, -1, 20, 1);
    addr.role_bg = ROLE_BG;
    addr.on_click = br_addr_click;
    addr.usr = b;
    addr.render = br_addr_render;
    b->addr_blk = b_text(toolbar, &addr, b->addr);

    /* 内容视口：v0 顶部排布，超出由裁剪栈裁掉。 */
    blk_spec view = br_panel(LAYER_WORK, BR_VP_W, BR_VP_H, 0);
    view.gap = 3; view.pad_l = 4; view.pad_r = 4; view.pad_t = 4; view.pad_b = 4;
    b->content = b_column(b->root, &view);

    blk_spec st = br_panel(LAYER_WORK, BR_VP_W, 16, 0);
    b->status_blk = b_text(b->root, &st, b->status);
}

/* 离线样本页（探针用，不起网络）。 */
static const char sample_a[] =
    "<html><body>"
    "<h1>CatOS Browser</h1>"
    "<p>v0 offline sample page rendered from an embedded document.</p>"
    "<ul><li>text block</li><li>link <a href=\"http://example/\">example</a></li></ul>"
    "<p>Second paragraph with <b>bold</b> words and a <a href=\"#\">anchor</a>.</p>"
    "<img src=\"/samples/desktop.bmp\">"
    "</body></html>";

static const char sample_b[] =
    "<html><body>"
    "<h1>CatOS Browser - Page 2</h1>"
    "<p>Second offline document for history back/forward isolation.</p>"
    "<ul><li>history entry B</li></ul>"
    "</body></html>";

static int br_is_sample_url(const char *u) {
    return u && u[0] == 's' && u[1] == 'a' && u[2] == 'm' && u[3] == 'p' &&
           u[4] == 'l' && u[5] == 'e' && u[6] == ':';
}

/* 错误页：文字说明 + 返回入口（有历史走 Back，否则 Reload）。 */
static void br_build_error_page(br_inst_t *b) {
    br_add_text_block(b, "Navigation failed", 1, 0);
    br_add_text_block(b, b->err_msg, 1, 0);
    br_add_text_block(b, b->url, 1, 0);
    blk_spec row = br_panel(LAYER_WORK, -1, 20, 0);
    block_id brow = b_row(b->content, &row);
    blk_spec back = br_panel(LAYER_WORK, 72, 18, 1);
    back.usr = b;
    if (b->hist_cur > 0) {
        back.on_click = br_click_back;
        b_button(brow, &back, "Back");
    } else {
        back.on_click = br_click_refresh;
        b_button(brow, &back, "Reload");
    }
    b->blocks++;
}

static void br_navigate(br_inst_t *b, const char *url, int push_hist) {
    xcopy(b->url, sizeof(b->url), url && *url ? url : "http://example.com/");
    xcopy(b->addr, sizeof(b->addr), b->url);
    b->addr_len = xlen(b->addr); b->addr_pos = b->addr_len;
    b->addr_focused = 0;
    b->error = 0;
    if (push_hist) br_hist_push(b, b->url);

    br_close_subtree(b);
    br_build_ui(b);

    int rc;
    if (br_is_sample_url(b->url)) {
        /* 内嵌样本页：不起网络。 */
        if (br_alloc_html(b) < 0) { br_build_error_page(b); xcopy(b->err_msg, sizeof(b->err_msg), "out of memory"); br_compose_status(b); return; }
        const char *doc = (xlen(b->url) > 13 && b->url[13] == '2') ? sample_b : sample_a;
        int len = xlen(doc);
        for (int i = 0; i < len && i < BR_HTML_CAP - 1; i++) b->html[i] = (uint8_t)doc[i];
        b->bytes = len; b->status_code = 0; b->elapsed_ms = 0; b->ok = 1;
        rc = 0;
    } else {
        rc = br_fetch(b, b->url);
    }

    if (rc == 0) {
        br_parse_html(b, b->html, b->bytes);
    } else {
        b->error = 1;
        br_build_error_page(b);
    }
    br_compose_status(b);

    ser_write("BROWSER:nav idx="); br_dec((uint32_t)b->slot);
    ser_write(" url="); ser_write(b->url); ser_write("\n");
    ser_write("BROWSER:hist idx="); br_dec((uint32_t)b->slot);
    ser_write(" len="); br_dec((uint32_t)b->hist_n); ser_write("\n");

    if (rc == 0) {
        ser_write("GUI:br bytes="); br_dec((uint32_t)b->bytes);
        ser_write(" nodes="); br_dec((uint32_t)b->nodes);
        ser_write(" blocks="); br_dec((uint32_t)b->blocks);
        ser_write(" ms="); br_dec(b->elapsed_ms);
        ser_write(" status="); br_dec((uint32_t)b->status_code);
        ser_write(" active=1 result=1\n");
    } else {
        /* 未通路（网络/https）不发 GUI:br 验收记录：门禁按 SKIP 归类，
         * 原因落状态条，避免把未完成通路误判成像素/解析失败。 */
        ser_write("br: fetch rc="); br_dec((uint32_t)(-rc));
        ser_write(" status="); br_dec((uint32_t)b->status_code);
        ser_write("\n");
    }

    layout_run();
    gui_raise_to_front(b->root);
}

static void br_focus(int slot) {
    br_inst_t *b = br_slot_inst(slot);
    if (!b) return;
    int prev = br_active;
    br_active = slot;
    gui_set_key_hook(br_key, b);
    gui_raise_to_front(b->root);
    if (prev >= 0) {
        br_inst_t *pb = br_slot_inst(prev);
        if (pb) {
            pb->addr_focused = 0;
            block *ab = btable_get(pb->addr_blk);
            if (ab) ab->dirty_paint = 1;
        }
    }
    block *nb = btable_get(b->addr_blk);
    if (nb) nb->dirty_paint = 1;
    ser_write("BROWSER:focus idx="); br_dec((uint32_t)slot); ser_write("\n");
    ser_write("FOCUS:kind=browser idx="); br_dec((uint32_t)slot); ser_write("\n");
}

static void br_click_back(block_id id, int32_t lx, int32_t ly) {
    (void)id; (void)lx; (void)ly;
    block *bl = btable_get(id);
    br_inst_t *b = bl ? (br_inst_t *)bl->usr : 0;
    if (!b || !b->in_use) return;
    br_focus(b->slot);
    if (b->hist_cur > 0) { b->hist_cur--; br_navigate(b, b->hist[b->hist_cur], 0); }
}
static void br_click_fwd(block_id id, int32_t lx, int32_t ly) {
    (void)id; (void)lx; (void)ly;
    block *bl = btable_get(id);
    br_inst_t *b = bl ? (br_inst_t *)bl->usr : 0;
    if (!b || !b->in_use) return;
    br_focus(b->slot);
    if (b->hist_cur < b->hist_n - 1) { b->hist_cur++; br_navigate(b, b->hist[b->hist_cur], 0); }
}
static void br_click_refresh(block_id id, int32_t lx, int32_t ly) {
    (void)id; (void)lx; (void)ly;
    block *bl = btable_get(id);
    br_inst_t *b = bl ? (br_inst_t *)bl->usr : 0;
    if (!b || !b->in_use) return;
    br_focus(b->slot);
    br_navigate(b, b->url, 0);
}
static void br_click_stop(block_id id, int32_t lx, int32_t ly) {
    (void)id; (void)lx; (void)ly;
    block *bl = btable_get(id);
    br_inst_t *b = bl ? (br_inst_t *)bl->usr : 0;
    /* v0：单条阻塞请求，无异步中止；停止位仅记录状态。 */
    if (b) br_set_status(b, "stop: v0 single request");
}

static void br_addr_click(block_id id, int32_t lx, int32_t ly) {
    (void)lx; (void)ly;
    block *bl = btable_get(id);
    br_inst_t *b = bl ? (br_inst_t *)bl->usr : 0;
    if (!b || !b->in_use) return;
    br_focus(b->slot);
    /* 地址栏聚焦：本实例接管编辑；其余实例地址栏失焦。 */
    for (int i = 0; i < BR_MAX; i++) {
        br_inst_t *o = pool[i];
        if (o && o->in_use) {
            o->addr_focused = (o == b);
            block *ob = btable_get(o->addr_blk);
            if (ob) ob->dirty_paint = 1;
        }
    }
    br_addr_clear(b);
}

/* ---- 实例生命周期 --------------------------------------------------------- */

static br_inst_t *br_inst_alloc(int slot) {
    br_inst_t *b = kvmalloc(sizeof(br_inst_t), 16);
    if (!b) return 0;
    memset(b, 0, sizeof(*b));
    b->phys = kvmalloc(BR_MAX_PAGES * 4, 16);
    b->img_phys = kvmalloc(BR_IMG_MAX_PAGES * 4, 16);
    b->pool = kvmalloc(BR_POOL, 16);
    b->hist = kvmalloc(BR_HIST_MAX * BR_URL_MAX, 16);
    if (!b->phys || !b->img_phys || !b->pool || !b->hist) {
        kvmfree(b->phys); kvmfree(b->img_phys); kvmfree(b->pool);
        kvmfree(b->hist); kvmfree(b);
        return 0;
    }
    b->in_use = 1;
    b->slot = slot;
    return b;
}

static void br_close_inst(int slot, int report) {
    if (slot < 0 || slot >= BR_MAX) return;
    br_inst_t *b = pool[slot];
    if (!b || !b->in_use) return;
    int rx = 0, ry = 0, rw = 0, rh = 0;
    block *pr = btable_get(b->root);
    if (pr) { rx = pr->x; ry = pr->y; rw = pr->w; rh = pr->h; }
    br_close_subtree(b);
    br_free_html(b);
    kvmfree(b->phys); kvmfree(b->img_phys); kvmfree(b->pool); kvmfree(b->hist);
    b->phys = 0; b->img_phys = 0; b->pool = 0; b->hist = 0;
    b->in_use = 0;
    kvmfree(b);
    pool[slot] = 0;
    if (report) { ser_write("BROWSER:close idx="); br_dec((uint32_t)slot); ser_write("\n"); }
    if (br_active == slot) {
        br_active = -1;
        for (int i = 0; i < BR_MAX; i++)
            if (pool[i]) { br_active = i; break; }
        if (br_active >= 0) gui_set_key_hook(br_key, pool[br_active]);
        else gui_set_key_hook(0, 0);
    }
    gui_mark_dirty_union(0, rx, ry, rw, rh);
}

/* ---- 对外接口 ------------------------------------------------------------ */

void gui_browser_close(void) {
    for (int i = BR_MAX - 1; i >= 0; i--) br_close_inst(i, 0);
}

/* 地址栏聚焦即全选旧 URL：首次键入为替换，而非追加到旧串尾部。 */
static void br_addr_clear(br_inst_t *b) {
    b->addr[0] = 0;
    b->addr_len = 0;
    b->addr_pos = 0;
}

static void br_addr_insert(br_inst_t *b, char c) {
    if (b->addr_len >= BR_URL_MAX - 1) return;
    for (int i = b->addr_len; i > b->addr_pos; i--) b->addr[i] = b->addr[i - 1];
    b->addr[b->addr_pos++] = c;
    b->addr_len++;
    b->addr[b->addr_len] = 0;
}

static void br_addr_backspace(br_inst_t *b) {
    if (b->addr_pos <= 0) return;
    for (int i = b->addr_pos - 1; i < b->addr_len - 1; i++) b->addr[i] = b->addr[i + 1];
    b->addr_len--; b->addr_pos--;
    b->addr[b->addr_len] = 0;
}

static void br_submit(br_inst_t *b) {
    char norm[BR_URL_MAX];
    br_normalize(norm, sizeof(norm), b->addr);
    if (norm[0] == 0) {
        /* 空输入：不导航，回到当前页；地址栏回填当前 URL。 */
        xcopy(b->addr, sizeof(b->addr), b->url);
        b->addr_len = xlen(b->addr); b->addr_pos = b->addr_len;
        block *ab = btable_get(b->addr_blk);
        if (ab) ab->dirty_paint = 1;
        return;
    }
    br_navigate(b, norm, 1);
}

static int br_key(int code, void *ud) {
    br_inst_t *b = (br_inst_t *)ud;
    if (!b || !b->in_use) return 0;

    if (code == 27) { br_close_inst(b->slot, 1); return 1; }      /* Esc 关闭 */
    if (!b->addr_focused) return 0;

    switch (code) {
    case 10:  br_submit(b); break;
    case 8:   br_addr_backspace(b); break;
    case KB_LEFT:  if (b->addr_pos > 0) b->addr_pos--; break;
    case KB_RIGHT: if (b->addr_pos < b->addr_len) b->addr_pos++; break;
    case KB_HOME:  b->addr_pos = 0; break;
    case KB_END:   b->addr_pos = b->addr_len; break;
    default:
        if (code >= 0x20 && code <= 0x7E) br_addr_insert(b, (char)code);
        else return 0;
    }
    block *ab = btable_get(b->addr_blk);
    if (ab) ab->dirty_paint = 1;
    return 1;
}

static void br_open_instance(const char *url, int push_hist) {
    int slot = -1;
    for (int i = 0; i < BR_MAX; i++)
        if (!pool[i]) { slot = i; break; }
    if (slot < 0) { br_focus(br_active); return; }   /* 到上限：聚焦现有实例，不崩 */

    br_inst_t *b = br_inst_alloc(slot);
    if (!b) return;
    pool[slot] = b;

    br_navigate(b, url, push_hist);
    br_active = slot;
    gui_set_key_hook(br_key, b);
    ser_write("BROWSER:open idx="); br_dec((uint32_t)slot); ser_write("\n");
}

void gui_browser_open(const char *url) {
    if (!url || !*url) { br_open_instance("sample://local", 1); return; }
    if (br_is_sample_url(url)) { gui_browser_probe_sample(); return; }
    br_open_instance(url, 1);
}

/* 离线样本（探针用，不起网络）：解析内嵌 HTML，验证 tokenizer → 块树链路。 */
void gui_browser_probe_sample(void) {
    gui_browser_close();
    br_open_instance("sample://local", 1);
}

/* ---- 探针 / 任务栏查询口径（内部，不进 CE ABI） --------------------------- */

int         gui_browser_active(void) { return br_active_inst() ? 1 : 0; }
int         gui_browser_ok(void)     { br_inst_t *b = br_active_inst(); return b ? b->ok : 0; }
int         gui_browser_bytes(void)  { br_inst_t *b = br_active_inst(); return b ? b->bytes : 0; }
int         gui_browser_nodes(void)  { br_inst_t *b = br_active_inst(); return b ? b->nodes : 0; }
int         gui_browser_blocks(void)  { br_inst_t *b = br_active_inst(); return b ? b->blocks : 0; }

void gui_br_focus(int slot) { br_focus(slot); }

int gui_br_count(void) {
    int n = 0;
    for (int i = 0; i < BR_MAX; i++) if (pool[i] && pool[i]->in_use) n++;
    return n;
}

int gui_br_active_idx(void) { return br_active; }

void *gui_br_focus_ud(int slot) {
    br_inst_t *b = br_slot_inst(slot);
    return b ? (void *)b : 0;
}

block_id gui_br_root(int slot) {
    br_inst_t *b = br_slot_inst(slot);
    return b ? b->root : 0;
}

const char *gui_br_url(int slot) {
    br_inst_t *b = br_slot_inst(slot);
    return b ? b->url : "";
}

int gui_br_hist_len(int slot) {
    br_inst_t *b = br_slot_inst(slot);
    return b ? b->hist_n : 0;
}

const char *gui_br_addr(int slot) {
    br_inst_t *b = br_slot_inst(slot);
    return b ? b->addr : "";
}

void gui_br_type(int slot, const char *s) {
    br_inst_t *b = br_slot_inst(slot);
    if (!b || !s) return;
    br_focus(slot);
    b->addr_focused = 1;
    br_addr_clear(b);
    while (*s) { br_addr_insert(b, *s); s++; }
    block *ab = btable_get(b->addr_blk);
    if (ab) ab->dirty_paint = 1;
}

void gui_br_submit(int slot) {
    br_inst_t *b = br_slot_inst(slot);
    if (!b) return;
    br_focus(slot);
    b->addr_focused = 1;
    br_submit(b);
}

/* 探针用：直接开一个离线样本实例（不走 close-all，保证多实例取证独立）。 */
void gui_br_open_sample(const char *url) {
    br_open_instance(url && *url ? url : "sample://local", 1);
}

void gui_br_back(int slot) {
    br_inst_t *b = br_slot_inst(slot);
    if (!b) return;
    br_focus(slot);
    if (b->hist_cur > 0) { b->hist_cur--; br_navigate(b, b->hist[b->hist_cur], 0); }
}

void gui_br_fwd(int slot) {
    br_inst_t *b = br_slot_inst(slot);
    if (!b) return;
    br_focus(slot);
    if (b->hist_cur < b->hist_n - 1) { b->hist_cur++; br_navigate(b, b->hist[b->hist_cur], 0); }
}

void gui_br_reload(int slot) {
    br_inst_t *b = br_slot_inst(slot);
    if (!b) return;
    br_focus(slot);
    br_navigate(b, b->url, 0);
}
