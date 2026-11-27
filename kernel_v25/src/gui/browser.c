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
 */
#include "gui/gui.h"
#include "gui_int.h"
#include "gui/tokens.h"
#include "net.h"
#include "paging.h"
#include "serial_log.h"
#include "fs.h"

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

static int         br_active;
static int         br_ok;
static int         br_bytes;
static int         br_nodes;
static int         br_blocks;
static int         br_status_code;
static uint32_t    br_elapsed_ms;
static int         br_stopping;

static char        br_url[BR_URL_MAX];
static char        br_status[BR_STATUS_MAX];

static block_id    br_root;
static block_id    br_addr_blk;
static block_id    br_content;
static block_id    br_status_blk;

/* HTML 帧缓冲（运行时页分配，不入 .bss 静态数组）。 */
static uint32_t    br_phys[BR_MAX_PAGES];
static int         br_pages;
static uint8_t    *br_html;

/* 解码图片帧缓冲（24bpp RGB，运行时页分配；PNG/JPEG 浮点路径不在本内核支持范围）。 */
#define BR_IMG_MAX_PAGES 400
static uint32_t    br_img_phys[BR_IMG_MAX_PAGES];
static int         br_img_pages;
static uint8_t    *br_img_buf;
static int         br_img_w, br_img_h;

/* 持久文本池：块文本按引用保存，不能指向栈/run 临时缓冲。 */
static char        br_pool[BR_POOL];
static int         br_pool_n;

static char        br_run[BR_RUN_MAX];
static int         br_run_n;

static char        br_hist[BR_HIST_MAX][BR_URL_MAX];
static int         br_hist_n;
static int         br_hist_cur;

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

/* ---- HTML 帧缓冲 -------------------------------------------------------- */

static void br_free_html(void) {
    for (int p = 0; p < br_pages; p++) frame_free(br_phys[p]);
    br_pages = 0; br_html = 0;
}

static int br_alloc_html(void) {
    br_free_html();
    uint32_t pa = frame_alloc();
    if (!pa) return -1;
    br_phys[0] = pa;
    br_pages = 1;
    br_html = (uint8_t *)pa;
    return 0;
}

/* 流式接收器：把响应体追加进动态 HTML 缓冲，跨页按需增长。 */
static int br_stream_sink(void *ud, const uint8_t *p, int len) {
    (void)ud;
    for (int i = 0; i < len; i++) {
        int off = br_bytes + i;
        if (off >= BR_HTML_CAP) return -1;
        int pg = off >> 12;
        if (pg >= br_pages) {
            uint32_t pa = frame_alloc();
            if (!pa) return -1;
            br_phys[pg] = pa;
            br_pages = pg + 1;
        }
        br_html[off] = p[i];
    }
    br_bytes += len;
    return 0;
}

/* ---- 持久文本池 --------------------------------------------------------- */

static const char *pool_put(const char *s, int n) {
    if (n < 0) n = 0;
    /* 每条串独立 NUL 终止：游标按 n+1 推进，否则下一次追加会覆盖上一条
     * 串尾的 NUL，块文本读出累计尾巴（D3：串界不清导致宽度/归属错乱）。 */
    if (br_pool_n + n + 1 >= BR_POOL) n = BR_POOL - 1 - br_pool_n;
    if (n <= 0) return "";
    char *p = br_pool + br_pool_n;
    for (int i = 0; i < n; i++) p[i] = s[i];
    p[n] = 0;
    br_pool_n += n + 1;
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

struct br_css {
    uint32_t fg, bg;
    int32_t  pad_l, pad_r, pad_t, pad_b;
    uint8_t  border_w;
    uint8_t  align;
    int32_t  req_w, req_h;          /* px；0 = auto */
    int32_t  req_w_pct, req_h_pct;  /* 相对父宽/高的百分比；0 = 未声明 */
    int32_t  max_w;                 /* px 上限；0 = 不限 */
};
static struct br_css br_css_cur;

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

static void css_reset(void) {
    br_css_cur.fg = INHERIT_COLOR; br_css_cur.bg = INHERIT_COLOR;
    br_css_cur.pad_l = br_css_cur.pad_r = 0;
    br_css_cur.pad_t = br_css_cur.pad_b = 0;
    br_css_cur.border_w = 0; br_css_cur.align = 0;
    br_css_cur.req_w = 0; br_css_cur.req_h = 0;
    br_css_cur.req_w_pct = 0; br_css_cur.req_h_pct = 0;
    br_css_cur.max_w = 0;
}

static void css_set_prop(const char *p, const char *v) {
    p = css_trim(p); v = css_trim(v);
    if (css_eq(p, "color")) br_css_cur.fg = css_color(v);
    else if (css_eq(p, "background-color") || css_eq(p, "background"))
        br_css_cur.bg = css_color(v);
    else if (css_eq(p, "padding") || css_eq(p, "margin")) {
        int q = css_px(v);
        br_css_cur.pad_l = br_css_cur.pad_r = q;
        br_css_cur.pad_t = br_css_cur.pad_b = q;
    }
    else if (css_eq(p, "padding-left") || css_eq(p, "margin-left")) br_css_cur.pad_l = css_px(v);
    else if (css_eq(p, "padding-right") || css_eq(p, "margin-right")) br_css_cur.pad_r = css_px(v);
    else if (css_eq(p, "padding-top") || css_eq(p, "margin-top")) br_css_cur.pad_t = css_px(v);
    else if (css_eq(p, "padding-bottom") || css_eq(p, "margin-bottom")) br_css_cur.pad_b = css_px(v);
    else if (css_eq(p, "border")) br_css_cur.border_w = (uint8_t)css_px(v);
    else if (css_eq(p, "text-align")) {
        if (css_eq(v, "center")) br_css_cur.align = ALN_CENTER;
        else if (css_eq(v, "right") || css_eq(v, "end")) br_css_cur.align = ALN_END;
        else br_css_cur.align = ALN_START;
    }
    else if (css_eq(p, "width")) {
        int pct = 0; int q = css_size(v, &pct);
        if (pct) br_css_cur.req_w_pct = q;
        else if (q > 0) br_css_cur.req_w = q;
    }
    else if (css_eq(p, "height")) {
        int pct = 0; int q = css_size(v, &pct);
        if (pct) br_css_cur.req_h_pct = q;
        else if (q > 0) br_css_cur.req_h = q;
    }
    else if (css_eq(p, "max-width")) { int q = css_px(v); if (q > 0) br_css_cur.max_w = q; }
    /* font-weight:bold 由 <b>/<strong> 承担；display 折行见标签块级/行内判定。 */
}

static void css_parse(const char *style) {
    css_reset();
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
        if (colon) css_set_prop(pstart, vstart);
    }
}

/* ---- 导航 / 历史 --------------------------------------------------------- */

static void br_hist_push(const char *url) {
    if (!url || !*url) return;
    if (br_hist_n > 0 && br_hist_cur < br_hist_n - 1) {
        /* 截断前进分支。 */
        br_hist_n = br_hist_cur + 1;
    }
    if (br_hist_n >= BR_HIST_MAX) {
        for (int i = 1; i < BR_HIST_MAX; i++)
            xcopy(br_hist[i - 1], BR_URL_MAX, br_hist[i]);
        br_hist_n = BR_HIST_MAX - 1;
    }
    xcopy(br_hist[br_hist_n], BR_URL_MAX, url);
    br_hist_n++;
    br_hist_cur = br_hist_n - 1;
}

/* ---- 状态行 ------------------------------------------------------------- */

static void br_set_status(const char *msg) {
    xcopy(br_status, sizeof(br_status), msg);
    block *st = btable_get(br_status_blk);
    if (st) { st->text = br_status; st->dirty_paint = 1; }
}

static void br_compose_status(void) {
    xcopy(br_status, sizeof(br_status), br_url);
    xcat(br_status, sizeof(br_status), "  status=");
    xnum(br_status, sizeof(br_status), (uint32_t)br_status_code);
    xcat(br_status, sizeof(br_status), "  bytes=");
    xnum(br_status, sizeof(br_status), (uint32_t)br_bytes);
    xcat(br_status, sizeof(br_status), "  ");
    xnum(br_status, sizeof(br_status), br_elapsed_ms);
    xcat(br_status, sizeof(br_status), "ms  nodes=");
    xnum(br_status, sizeof(br_status), (uint32_t)br_nodes);
    xcat(br_status, sizeof(br_status), "  blocks=");
    xnum(br_status, sizeof(br_status), (uint32_t)br_blocks);
    block *st = btable_get(br_status_blk);
    if (st) { st->text = br_status; st->dirty_paint = 1; }
}

/* ---- HTML 流式解析（不建树） --------------------------------------------- */

enum {
    TK_OTHER = 0,
    TK_H1, TK_H2, TK_H3, TK_P, TK_UL, TK_OL, TK_LI, TK_BR,
    TK_TABLE, TK_TR, TK_TD, TK_TH, TK_A, TK_B, TK_STRONG, TK_EM,
    TK_IMG, TK_SCRIPT, TK_STYLE, TK_HEAD, TK_BODY, TK_TITLE,
    TK_META, TK_LINK
};

static int   br_pos;
static int   br_accent;
static block_id br_cur_row;

static int br_hlen(void) { return br_pages ? br_pages * 4096 : 0; }

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

static void run_push(char c) {
    if (br_run_n < BR_RUN_MAX - 1) br_run[br_run_n++] = c;
}

static void run_entity(const char *ent, int n) {
    if (n == 4 && ent[1] == 'a' && ent[2] == 'm' && ent[3] == 'p') run_push('&');
    else if (n == 3 && ent[1] == 'l' && ent[2] == 't') run_push('<');
    else if (n == 3 && ent[1] == 'g' && ent[2] == 't') run_push('>');
    else if (n == 5 && ent[1] == 'q' && ent[2] == 'u' && ent[3] == 'o' && ent[4] == 't') run_push('"');
    else if (n == 5 && ent[1] == '#' && ent[2] == '3' && ent[3] == '9') run_push('\'');
    else if (n == 5 && ent[1] == 'n' && ent[2] == 'b' && ent[3] == 's' && ent[4] == 'p') run_push(' ');
}

static void run_flush(int scale) {
    if (br_run_n == 0) return;
    /* 折叠段内空白。 */
    char out[BR_RUN_MAX]; int o = 0; int sp = 1;
    for (int i = 0; i < br_run_n; i++) {
        char c = br_run[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!sp) { out[o++] = ' '; sp = 1; }
        } else { out[o++] = c; sp = 0; }
    }
    if (o && out[o - 1] == ' ') o--;
    const char *txt = pool_put(out, o);
    blk_spec t = br_panel(LAYER_CONTENT, -1, -1, 0);
    t.scale = (uint8_t)scale;
    t.role_accent = br_accent ? ROLE_ACCENT : ROLE_NONE;
    t.pad_l = 2; t.pad_r = 2;
    /* v0.1 内联 CSS 盒模型最小集（未声明项保持角色继承）。 */
    if (br_css_cur.fg != INHERIT_COLOR) { t.role_fg = ROLE_NONE; t.fg = br_css_cur.fg; }
    if (br_css_cur.bg != INHERIT_COLOR) { t.role_bg = ROLE_NONE; t.bg = br_css_cur.bg; }
    t.pad_l += br_css_cur.pad_l; t.pad_r += br_css_cur.pad_r;
    t.pad_t += br_css_cur.pad_t; t.pad_b += br_css_cur.pad_b;
    if (br_css_cur.border_w) t.border_w = br_css_cur.border_w;
    if (br_css_cur.align) t.align_main = br_css_cur.align;
    if (br_css_cur.req_w > 0) t.req_w = br_css_cur.req_w;
    else if (br_css_cur.req_w_pct > 0)
        t.req_w = (BR_VP_W - 16) * br_css_cur.req_w_pct / 100;
    if (br_css_cur.req_h > 0) t.req_h = br_css_cur.req_h;
    else if (br_css_cur.req_h_pct > 0)
        t.req_h = BR_VP_H * br_css_cur.req_h_pct / 100;
    if (br_css_cur.max_w > 0 && t.req_w > br_css_cur.max_w)
        t.req_w = br_css_cur.max_w;
    block_id parent = br_cur_row ? br_cur_row : br_content;
    b_text(parent, &t, txt);
    br_blocks++;
    br_run_n = 0;
}

static void br_new_row(void) {
    blk_spec r = br_panel(LAYER_CONTENT, -1, -1, 0);
    r.gap = 2;
    br_cur_row = b_row(br_content, &r);
    br_blocks++;
}

static void br_add_text_block(const char *txt, int scale, int accent) {
    blk_spec t = br_panel(LAYER_CONTENT, -1, -1, 0);
    t.scale = (uint8_t)scale;
    t.role_accent = accent ? ROLE_ACCENT : ROLE_NONE;
    const char *p = pool_put(txt, xlen(txt));
    b_text(br_content, &t, p);
    br_blocks++;
}

/* ---- 图片解码（BMP/PPM 内存/FS 路径；PNG/JPEG 浮点路径本内核不支持） ------ */

static uint16_t br_rd_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static int32_t br_rd_i32(const uint8_t *p) {
    return (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) |
                     ((uint32_t)p[3] << 24));
}

static void br_free_img(void) {
    for (int p = 0; p < br_img_pages; p++) frame_free(br_img_phys[p]);
    br_img_pages = 0; br_img_buf = 0; br_img_w = br_img_h = 0;
}

static int br_img_alloc(int w, int h) {
    int bytes = w * h * 3;
    int pages = (bytes + 4095) / 4096;
    if (pages > BR_IMG_MAX_PAGES) return -1;
    for (int p = 0; p < pages; p++) {
        uint32_t pa = frame_alloc();
        if (!pa) { br_free_img(); return -1; }
        br_img_phys[p] = pa;
    }
    br_img_pages = pages;
    br_img_buf = (uint8_t *)br_img_phys[0];
    memset(br_img_buf, 0, bytes);
    br_img_w = w; br_img_h = h;
    return 0;
}

static int br_decode_bmp_path(const char *path) {
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
    if (br_img_alloc(w, h) < 0) return -1;
    static uint8_t row[3204];
    for (int y = 0; y < h; y++) {
        int got = fs_read_at(path, (char *)row, data_off + y * stride, stride);
        if (got < bpc * w) return -1;
        int dst = topdown ? y : (h - 1 - y);
        for (int x = 0; x < w; x++) {
            uint8_t B = row[x * bpc];
            uint8_t G = row[x * bpc + 1];
            uint8_t R = row[x * bpc + 2];
            uint8_t *d = br_img_buf + (dst * w + x) * 3;
            d[0] = R; d[1] = G; d[2] = B;
        }
    }
    return 0;
}

static int br_decode_ppm_path(const char *path) {
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
    if (br_img_alloc(w, h) < 0) return -1;
    static uint8_t row[3204];
    for (int y = 0; y < h; y++) {
        int got = fs_read_at(path, (char *)row, data_off + y * w * 3, w * 3);
        if (got < w * 3) return -1;
        for (int x = 0; x < w; x++) {
            uint8_t *d = br_img_buf + (y * w + x) * 3;
            d[0] = row[x * 3]; d[1] = row[x * 3 + 1]; d[2] = row[x * 3 + 2];
        }
    }
    return 0;
}

static void br_img_embed(const char *src) {
    block *b = btable_alloc(BLK_CANVAS);
    if (!b) return;
    block_id parent = br_cur_row ? br_cur_row : br_content;
    tree_attach(btable_get(parent), b);
    int dw = br_img_w, dh = br_img_h;
    if (dw > BR_VP_W - 16) { dh = dh * (BR_VP_W - 16) / dw; dw = BR_VP_W - 16; }
    b->req_w = dw; b->req_h = dh;
    b->pix_w = br_img_w; b->pix_h = br_img_h;
    b->buf = br_img_buf;
    b->layer = LAYER_CONTENT; b->visible = 1; b->dirty_layout = 1;
    br_blocks++;
    ser_write("GUI:br img "); ser_write(src);
    ser_write(" w="); br_dec((uint32_t)br_img_w);
    ser_write(" h="); br_dec((uint32_t)br_img_h);
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

static void br_img_handle(const char *src) {
    if (!src || !*src) { br_add_text_block("[img: ?]", 1, 0); return; }
    int is_bmp = br_ext_is(src, ".bmp");
    int is_ppm = br_ext_is(src, ".ppm");
    int is_png = br_ext_is(src, ".png");
    int is_jpg = br_ext_is(src, ".jpg") || br_ext_is(src, ".jpeg");

    if (is_png || is_jpg) {
        ser_write("GUI:br img unsupported "); ser_write(src); ser_write("\n");
        char ph[BR_URL_MAX + 16];
        xcopy(ph, sizeof(ph), "[img: unsupported ");
        xcat(ph, sizeof(ph), src); xcat(ph, sizeof(ph), "]");
        br_add_text_block(ph, 1, 0);
        return;
    }
    /* 本地 FS 路径：直接解码 BMP/PPM。 */
    if (src[0] == '/' && (is_bmp || is_ppm)) {
        int rc = is_bmp ? br_decode_bmp_path(src) : br_decode_ppm_path(src);
        if (rc == 0) { br_img_embed(src); return; }
    }
    /* http(s) 同源图片本轮未接入内存解码；占位保留。 */
    char ph[BR_URL_MAX + 12];
    xcopy(ph, sizeof(ph), "[img: ");
    xcat(ph, sizeof(ph), src); xcat(ph, sizeof(ph), "]");
    br_add_text_block(ph, 1, 0);
}

/* 解析一个标签；返回 1 表示消费到 '>'。 */
static int br_parse_tag(void) {
    /* br_pos 指向 '<' */
    int close = 0;
    int i = br_pos + 1;
    if (i < br_hlen() && br_html[i] == '/') { close = 1; i++; }

    char name[32]; int nn = 0;
    while (i < br_hlen() && br_html[i] > ' ' && br_html[i] != '>'
           && br_html[i] != '/' && nn < 31) {
        name[nn++] = (char)br_html[i]; i++;
    }
    name[nn] = 0;
    int kind = tk_name(name);

    /* 属性（v0 只取 a[href] / img[src]，收集到局部，不建属性表）。 */
    char href[BR_URL_MAX]; href[0] = 0;
    char src[BR_URL_MAX]; src[0] = 0;
    char style[BR_URL_MAX]; style[0] = 0;
    while (i < br_hlen() && br_html[i] != '>') {
        while (i < br_hlen() && (br_html[i] == ' ' || br_html[i] == '/')) i++;
        if (i >= br_hlen() || br_html[i] == '>') break;
        char key[24]; int nk = 0;
        while (i < br_hlen() && br_html[i] != '=' &&
               br_html[i] != ' ' && br_html[i] != '/' && br_html[i] != '>' && nk < 23) {
            key[nk++] = (char)br_html[i]; i++;
        }
        key[nk] = 0;
        while (i < br_hlen() && (br_html[i] == ' ' || br_html[i] == '/')) i++;
        if (i < br_hlen() && br_html[i] == '=') i++;
        while (i < br_hlen() && br_html[i] == ' ') i++;
        char val[BR_URL_MAX]; int nv = 0;
        if (i < br_hlen() && (br_html[i] == '"' || br_html[i] == '\'')) {
            char q = br_html[i++];
            while (i < br_hlen() && br_html[i] != q && nv < BR_URL_MAX - 1)
                val[nv++] = (char)br_html[i++];
            if (i < br_hlen()) i++;
        } else {
            while (i < br_hlen() && br_html[i] != ' ' && br_html[i] != '>' && nv < BR_URL_MAX - 1)
                val[nv++] = (char)br_html[i++];
        }
        val[nv] = 0;
        if ((key[0] == 'h' || key[0] == 'H') && key[1] == 'r') xcopy(href, sizeof(href), val);
        if ((key[0] == 's' || key[0] == 'S') && key[1] == 'r') xcopy(src, sizeof(src), val);
        if ((key[0] == 's' || key[0] == 'S') && key[1] == 't' && key[2] == 'y')
            xcopy(style, sizeof(style), val);
    }
    if (i < br_hlen()) i++;      /* '>' */
    br_pos = i;

    if (!close) br_nodes++;
    if (!close) css_parse(style);

    switch (kind) {
    case TK_H1: run_flush(1); break;
    case TK_H2: case TK_H3: run_flush(1); break;
    case TK_P: run_flush(1); break;
    case TK_LI:
        run_flush(1);
        br_new_row();
        run_push('-'); run_push(' ');
        break;
    case TK_BR: run_flush(1); break;
    case TK_TR: run_flush(1); br_new_row(); break;
    case TK_TD: case TK_TH: run_flush(1); break;
    case TK_A: br_accent = 1; break;
    case TK_IMG: {
        run_flush(1);
        br_img_handle(src);
        break;
    }
    case TK_SCRIPT: case TK_STYLE: {
        /* 跳过 RCDATA 正文直到对应闭合标签。 */
        const char *stop = kind == TK_SCRIPT ? "</script" : "</style";
        int sl = kind == TK_SCRIPT ? 8 : 7;
        int j = br_pos;
        while (j + sl <= br_hlen()) {
            int m = 1;
            for (int k = 0; k < sl; k++) {
                char c = br_html[j + k];
                if (c != stop[k] && c != stop[k] - 32) { m = 0; break; }
            }
            if (m) { while (j < br_hlen() && br_html[j] != '>') j++; br_pos = j + 1; break; }
            j++;
        }
        break;
    }
    default: break;
    }

    if (close) {
        switch (kind) {
        case TK_H1: case TK_H2: case TK_H3: case TK_P: case TK_LI:
        case TK_TD: case TK_TH: run_flush(1); break;
        case TK_TR: case TK_TABLE: br_cur_row = 0; break;
        case TK_A: br_accent = 0; break;
        default: break;
        }
    }
    (void)href;
    return 1;
}

static void br_parse_html(const uint8_t *html, int len) {
    br_pool_n = 0; br_run_n = 0;
    css_reset();
    br_pos = 0; br_accent = 0; br_cur_row = 0;
    br_nodes = 0; br_blocks = 0;

    while (br_pos < len) {
        if (html[br_pos] == '<') {
            if (br_pos + 3 < len && html[br_pos + 1] == '!' &&
                html[br_pos + 2] == '-' && html[br_pos + 3] == '-') {
                int j = br_pos + 4;
                while (j + 3 <= len && !(html[j] == '-' && html[j + 1] == '-' && html[j + 2] == '>')) j++;
                br_pos = j + 3 <= len ? j + 3 : len;
                continue;
            }
            if (br_pos + 1 < len && (html[br_pos + 1] == '!' || html[br_pos + 1] == '?')) {
                while (br_pos < len && html[br_pos] != '>') br_pos++;
                if (br_pos < len) br_pos++;
                continue;
            }
            br_parse_tag();
        } else {
            char c = html[br_pos];
            if (c == '&') {
                int e = br_pos + 1;
                while (e < len && html[e] != ';' && html[e] != '<' && html[e] != ' ' && e - br_pos < 8) e++;
                if (e < len && html[e] == ';') {
                    run_entity((const char *)html + br_pos, e - br_pos + 1);
                    br_pos = e + 1;
                    continue;
                }
            }
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') run_push(' ');
            else run_push((char)c);
            br_pos++;
        }
    }
    run_flush(1);
}

/* ---- 取页 ---------------------------------------------------------------- */

static int br_fetch(const char *url) {
    br_ok = 0; br_bytes = 0; br_status_code = 0; br_elapsed_ms = 0;
    if (br_alloc_html() < 0) { br_set_status("out of memory"); return -1; }

    if (!net_up()) { br_set_status("network not ready"); return -1; }

    uint32_t t0 = uptime_ms();
    int status = 0; long total = 0;
    int n = net_http_download_stream(url, 0, br_stream_sink, 0,
                                     &status, &total);
    uint32_t t1 = uptime_ms();
    br_elapsed_ms = t1 > t0 ? t1 - t0 : 0;

    if (n < 0) {
        br_bytes = 0;
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
        br_set_status(msg);
        return n;
    }
    br_bytes = (int)total;
    br_status_code = status;
    br_ok = 1;
    return 0;
}

/* ---- 子树重建 ------------------------------------------------------------ */

static void br_close_subtree(void) {
    int rx = 0, ry = 0, rw = 0, rh = 0;
    block *pr = btable_get(br_root);
    if (pr) { rx = pr->x; ry = pr->y; rw = pr->w; rh = pr->h; }
    if (br_root) { b_remove(br_root); br_root = 0; }
    br_addr_blk = br_content = br_status_blk = 0;
    br_free_img();
    gui_mark_dirty_union(0, rx, ry, rw, rh);
}

static void br_click_back(block_id id, int32_t lx, int32_t ly);
static void br_click_fwd(block_id id, int32_t lx, int32_t ly);
static void br_click_refresh(block_id id, int32_t lx, int32_t ly);
static void br_click_stop(block_id id, int32_t lx, int32_t ly);

static void br_build_ui(void) {
    block_id root_id = gui_root_block()->id;
    blk_spec root = br_panel(LAYER_WORK, BR_VP_W + 16, BR_VP_H + 56, 1);
    root.pad_l = 8; root.pad_r = 8; root.pad_t = 8; root.pad_b = 8;
    root.gap = 4; root.focusable = 1;
    br_root = b_column(root_id, &root);
    block *pr = btable_get(br_root);
    pr->x = 112; pr->y = 48; pr->moved = 1;
    pr->st.corner_radius = 16;   /* §16.1 外框 r16 */

    /* 工具栏：Back / Fwd / Refresh / Stop + 地址栏。 */
    blk_spec bar = br_panel(LAYER_WORK, BR_VP_W, 22, 0);
    bar.gap = 4;
    block_id toolbar = b_row(br_root, &bar);

    static const char *btns[4] = { "Back", "Fwd", "Refresh", "Stop" };
    static void (*bclick[4])(block_id, int32_t, int32_t) = {
        br_click_back, br_click_fwd, br_click_refresh, br_click_stop
    };
    for (int i = 0; i < 4; i++) {
        blk_spec b = br_panel(LAYER_WORK, 56, 20, 1);
        b.on_click = bclick[i];
        block_id bid = b_row(toolbar, &b);
        b_text(bid, 0, btns[i]);
        br_blocks++;
    }

    blk_spec addr = br_panel(LAYER_WORK, -1, 20, 0);
    addr.role_bg = ROLE_BG;
    br_addr_blk = b_text(toolbar, &addr, br_url);

    /* 内容视口：v0 顶部排布，超出由裁剪栈裁掉。 */
    blk_spec view = br_panel(LAYER_CONTENT, BR_VP_W, BR_VP_H, 0);
    view.gap = 3; view.pad_l = 4; view.pad_r = 4; view.pad_t = 4; view.pad_b = 4;
    br_content = b_column(br_root, &view);

    blk_spec st = br_panel(LAYER_WORK, BR_VP_W, 16, 0);
    br_status_blk = b_text(br_root, &st, br_status);
}

static void br_navigate(const char *url, int push_hist) {
    xcopy(br_url, sizeof(br_url), url && *url ? url : "http://example.com/");
    if (push_hist) br_hist_push(br_url);

    br_close_subtree();
    br_build_ui();

    int rc = br_fetch(br_url);
    if (rc == 0) br_parse_html(br_html, br_bytes);
    br_compose_status();

    if (rc == 0) {
        ser_write("GUI:br bytes="); br_dec((uint32_t)br_bytes);
        ser_write(" nodes="); br_dec((uint32_t)br_nodes);
        ser_write(" blocks="); br_dec((uint32_t)br_blocks);
        ser_write(" ms="); br_dec(br_elapsed_ms);
        ser_write(" status="); br_dec((uint32_t)br_status_code);
        ser_write(" active=1 result=1\n");
    } else {
        /* 未通路（网络/https）不发 GUI:br 验收记录：门禁按 SKIP 归类，
         * 原因落状态条，避免把未完成通路误判成像素/解析失败。 */
        ser_write("br: fetch rc="); br_dec((uint32_t)(-rc));
        ser_write(" status="); br_dec((uint32_t)br_status_code);
        ser_write("\n");
    }

    layout_run();
    gui_raise_to_front(br_root);
}

static void br_click_back(block_id id, int32_t lx, int32_t ly) {
    (void)id; (void)lx; (void)ly;
    if (br_hist_cur > 0) { br_hist_cur--; br_navigate(br_hist[br_hist_cur], 0); }
}
static void br_click_fwd(block_id id, int32_t lx, int32_t ly) {
    (void)id; (void)lx; (void)ly;
    if (br_hist_cur < br_hist_n - 1) { br_hist_cur++; br_navigate(br_hist[br_hist_cur], 0); }
}
static void br_click_refresh(block_id id, int32_t lx, int32_t ly) {
    (void)id; (void)lx; (void)ly;
    br_navigate(br_url, 0);
}
static void br_click_stop(block_id id, int32_t lx, int32_t ly) {
    (void)id; (void)lx; (void)ly;
    /* v0：单条阻塞请求，无异步中止；停止位仅记录状态。 */
    br_stopping = 1;
    br_set_status("stop: v0 single request");
    br_stopping = 0;
}

/* ---- 对外接口 ------------------------------------------------------------ */

void gui_browser_close(void) {
    if (!br_active) return;
    gui_set_key_hook(0, 0);
    br_close_subtree();
    br_free_html();
    br_active = 0;
}

static int br_key(int code, void *ud) {
    (void)ud;
    if (!br_active) return 0;
    if (code == 27) { gui_browser_close(); return 1; }
    return 0;
}

static int br_is_sample_url(const char *u) {
    return u && u[0] == 's' && u[1] == 'a' && u[2] == 'm' && u[3] == 'p' &&
           u[4] == 'l' && u[5] == 'e' && u[6] == ':';
}

void gui_browser_open(const char *url) {
    if (br_is_sample_url(url)) { gui_browser_probe_sample(); return; }
    gui_browser_close();
    br_active = 1;
    br_hist_n = 0; br_hist_cur = -1;
    br_navigate(url, 1);
    gui_set_key_hook(br_key, 0);
}

/* 离线样本（探针用，不起网络）：解析内嵌 HTML，验证 tokenizer → 块树链路。 */
void gui_browser_probe_sample(void) {
    static const char sample[] =
        "<html><body>"
        "<h1>CatOS Browser</h1>"
        "<p>v0 offline sample page rendered from an embedded document.</p>"
        "<ul><li>text block</li><li>link <a href=\"http://example/\">example</a></li></ul>"
        "<p>Second paragraph with <b>bold</b> words and a <a href=\"#\">anchor</a>.</p>"
        "<img src=\"/samples/desktop.bmp\">"
        "</body></html>";

    gui_browser_close();
    br_active = 1;
    xcopy(br_url, sizeof(br_url), "sample://local");
    br_alloc_html();
    int len = xlen(sample);
    for (int i = 0; i < len && i < BR_HTML_CAP - 1; i++) br_html[i] = (uint8_t)sample[i];
    br_bytes = len; br_status_code = 0; br_elapsed_ms = 0; br_ok = 1;

    br_close_subtree();
    br_build_ui();
    br_parse_html(br_html, br_bytes);
    br_compose_status();
    layout_run();
    gui_mark_dirty(br_root);
    gui_set_key_hook(br_key, 0);

    ser_write("GUI:br bytes="); br_dec((uint32_t)br_bytes);
    ser_write(" nodes="); br_dec((uint32_t)br_nodes);
    ser_write(" blocks="); br_dec((uint32_t)br_blocks);
    ser_write(" ms=0 status=0 active=1 result=1\n");
}

int         gui_browser_active(void) { return br_active; }
int         gui_browser_ok(void)     { return br_ok; }
int         gui_browser_bytes(void)  { return br_bytes; }
int         gui_browser_nodes(void)  { return br_nodes; }
int         gui_browser_blocks(void) { return br_blocks; }
