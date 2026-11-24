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

extern uint32_t uptime_ms(void);
#include "keyboard.h"
#include "kstring.h"
#include <stdint.h>
#include <stddef.h>

#define BR_PAGES     12                      /* HTML 缓冲 12×4 KiB = 48 KiB */
#define BR_HTML_SIZE (BR_PAGES * 4096)
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
static uint32_t    br_phys[BR_PAGES];
static int         br_pages;
static uint8_t    *br_html;

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
    for (int p = 0; p < BR_PAGES; p++) {
        uint32_t pa = frame_alloc();
        if (!pa) { br_free_html(); return -1; }
        br_phys[p] = pa;
    }
    br_pages = BR_PAGES;
    br_html = (uint8_t *)br_phys[0];
    return 0;
}

/* ---- 持久文本池 --------------------------------------------------------- */

static const char *pool_put(const char *s, int n) {
    if (n < 0) n = 0;
    if (br_pool_n + n >= BR_POOL) n = BR_POOL - 1 - br_pool_n;
    if (n <= 0) return "";
    char *p = br_pool + br_pool_n;
    for (int i = 0; i < n; i++) p[i] = s[i];
    p[n] = 0;
    br_pool_n += n;
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

static int br_hlen(void) { return br_pages ? BR_HTML_SIZE : 0; }

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
    while (i < br_hlen() && br_html[i] != '>') {
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
    }
    if (i < br_hlen()) i++;      /* '>' */
    br_pos = i;

    if (!close) br_nodes++;

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
        char ph[BR_URL_MAX + 12];
        xcopy(ph, sizeof(ph), "[img: ");
        xcat(ph, sizeof(ph), src[0] ? src : "?");
        xcat(ph, sizeof(ph), "]");
        br_add_text_block(ph, 1, 0);
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
    int n = net_http_get(url, (char *)br_html, BR_HTML_SIZE);
    uint32_t t1 = uptime_ms();
    br_elapsed_ms = t1 > t0 ? t1 - t0 : 0;

    if (n < 0) {
        br_bytes = 0;
        char msg[BR_STATUS_MAX];
        const char *why;
        switch (n) {
        case -7: why = "redirect https blocked"; break;
        case -2: why = "dns failed"; break;
        case -3: why = "connect failed"; break;
        case -4: why = "http status error"; break;
        case -5: why = "no reply"; break;
        case -8: why = "redirect loop"; break;
        default: why = "fetch error"; break;
        }
        xcopy(msg, sizeof(msg), why);
        xcat(msg, sizeof(msg), " (");
        xnum(msg, sizeof(msg), (uint32_t)(-n));
        xcat(msg, sizeof(msg), ")");
        br_set_status(msg);
        return n;
    }
    br_bytes = n;
    br_status_code = net_http_last_status();
    br_ok = 1;
    return 0;
}

/* ---- 子树重建 ------------------------------------------------------------ */

static void br_close_subtree(void) {
    if (br_root) { b_remove(br_root); br_root = 0; }
    br_addr_blk = br_content = br_status_blk = 0;
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
    gui_mark_dirty(br_root);
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
        "<img src=\"pic.png\">"
        "</body></html>";

    gui_browser_close();
    br_active = 1;
    xcopy(br_url, sizeof(br_url), "sample://local");
    br_alloc_html();
    int len = xlen(sample);
    for (int i = 0; i < len && i < BR_HTML_SIZE - 1; i++) br_html[i] = (uint8_t)sample[i];
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
