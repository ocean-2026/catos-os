/*
 * cap2p_render.c -- 渲染/合成包探针（alpha / 阴影 / 逐角 / 图标 / 滚动条绘制 / 性能）。
 * 消费 `gui cap2-<case>` 的 case 名（不含 "cap2-" 前缀）；返回 1 = 已消费。
 * 所有断言均为真实取点（gfx24_getpx），不写死数字。
 */
#include "gui/gui.h"
#include "gui_int.h"
#include "gui/tokens.h"
#include "gfx24.h"
#include "render.h"
#include "task.h"
#include "serial_log.h"

static int iabs(int v) { return v < 0 ? -v : v; }

extern volatile uint32_t timer_ticks;

/* ---- 串口小数输出 ---------------------------------------------------------- */

static void pd(uint32_t v) {
    char b[12]; int n = 0;
    if (v == 0) { ser_write("0"); return; }
    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) ser_putc(b[--n]);
}

static int eq_str(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

/* ---- 探针环境进出 ---------------------------------------------------------- */

static int probe_enter(void) {
    con_set_gui_mode(1);
    if (con_request_mode(CON_MODE_VBE24, CON_ORIGIN_CONSOLE) != 0) {
        con_set_gui_mode(0);
        return 0;
    }
    gui_begin();
    gui_root_block()->w = GFX24_W;
    gui_root_block()->h = GFX24_H;
    return 1;
}

static void probe_leave(void) {
    gui_end();
    con_set_gui_mode(0);
    con_force_text();
    con_clear();
}

static blk_spec mk_spec(uint16_t layer, int32_t w, int32_t h) {
    blk_spec s;
    for (unsigned i = 0; i < sizeof(s); i++) ((uint8_t *)&s)[i] = 0;
    s.req_w = w; s.req_h = h;
    s.layer = layer; s.z = 0;
    s.visible = 1;
    s.role_fg = ROLE_FG; s.role_accent = ROLE_ACCENT;
    s.role_bg = ROLE_PANEL;
    s.fg = INHERIT_COLOR; s.bg = INHERIT_COLOR; s.accent = INHERIT_COLOR;
    s.scale = 1;
    return s;
}

/* 自由布局根包裹层：子块按 px_x/px_y 落位（与 chrome.c 样例同范式）。
 * 包裹层走 BG 角色 + LAYER_BG，不刷面板色，避免盖住子块外角切出的底色。 */
static block_id free_root(void) {
    blk_spec s = mk_spec(LAYER_BG, GFX24_W, GFX24_H);
    s.role_bg = ROLE_BG;
    return b_free(0, &s);
}

static uint32_t avg_px(int x, int y, int w, int h) {
    uint32_t sr = 0, sg = 0, sb = 0, n = 0;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            uint32_t p = gfx24_getpx(x + i, y + j);
            sr += p >> 16 & 0xFF; sg += p >> 8 & 0xFF; sb += p & 0xFF;
            n++;
        }
    if (!n) return 0;
    return ((sr / n) << 16) | ((sg / n) << 8) | (sb / n);
}

static int ch_r(uint32_t c) { return c >> 16 & 0xFF; }
static int ch_g(uint32_t c) { return c >>  8 & 0xFF; }
static int ch_b(uint32_t c) { return c & 0xFF; }

/* 观测覆盖率：(obs-under)/(col-under)，三通道取最稳的一条分母。返回 0..255。 */
static int obs_cov(uint32_t obs, uint32_t under, uint32_t col) {
    int dr = ch_r(col) - ch_r(under);
    int dg = ch_g(col) - ch_g(under);
    int db = ch_b(col) - ch_b(under);
    int d = dr; int o = ch_r(obs) - ch_r(under);
    if (iabs(dg) > iabs(d)) { d = dg; o = ch_g(obs) - ch_g(under); }
    if (iabs(db) > iabs(d)) { d = db; o = ch_b(obs) - ch_b(under); }
    if (iabs(d) < 8) return 255;
    int c = o * 255 / d;
    if (c < 0) c = 0;
    if (c > 255) c = 255;
    return c;
}

/* ---- alpha64 / alpha128 / alpha192 ---------------------------------------- */

static void probe_alpha(int A) {
    int ok = 1; int maxerr = 0;
    for (int t = 0; t < 3; t++) {
        if (!probe_enter()) { ser_write("GUI:cap2-alpha ENTER-FAIL\n"); return; }
        gui_theme_set(t);
        const struct style *tok = gui_active_style();
        uint32_t under = tok->bg, col = tok->accent;
        block_id fr = free_root();
        blk_spec s = mk_spec(LAYER_CONTENT, 100, 60);
        s.px_x = 30; s.px_y = 30;
        s.bg = col; s.role_bg = 0; s.alpha = (uint8_t)A;
        block_id id = b_rect(fr, &s);
        layout_run();
        gui_mark_dirty(fr);
        gui_frame();
        block *b = btable_get(id);
        uint32_t o = avg_px(b->x + 10, b->y + 10, 60, 30);
        probe_leave();
        int cov = obs_cov(o, under, col);
        int err = cov - A; if (err < 0) err = -err;
        if (err > 26) ok = 0;                 /* 容忍一档抖动噪声（≈17/255） */
        if (err > maxerr) maxerr = err;
    }
    ser_write("GUI:cap2-alpha");
    ser_write(A == 64 ? "64" : A == 128 ? "128" : "192");
    ser_write(ok ? " PASS" : " FAIL");
    ser_write(" cov_maxerr="); pd((uint32_t)maxerr);
    ser_write(" result="); pd(ok ? 1 : 0);
    ser_write("\n");
}

/* ---- shadow-slices：6 档 9-slice 拼缝 0px ---------------------------------- */

static void probe_shadow(void) {
    if (!probe_enter()) { ser_write("GUI:cap2-shadow-slices ENTER-FAIL\n"); return; }
    gui_theme_set(TOK_DARK);
    const struct style *tok = gui_active_style();
    uint32_t bg = tok->bg;
    int present = 0, seam_bad = 0;
    block_id fr = free_root();
    block *blocks[6];
    for (int e = 0; e < 6; e++) {
        blk_spec s = mk_spec(LAYER_WORK, 90, 60);
        s.px_x = 20 + e * 120; s.px_y = 60;
        s.elevation = (uint8_t)e;
        block_id id = b_rect(fr, &s);
        blocks[e] = btable_get(id);
    }
    layout_run();
    gui_mark_dirty(fr);
    gui_frame();

    for (int e = 0; e < 6; e++) {
        block *b = blocks[e];
        /* 右下外扩环带内取最暗像素（各档 ox/oy/pad 不同）。 */
        int bl = ch_r(bg) + ch_g(bg) + ch_b(bg);
        int darkest = bl;
        for (int d = 1; d <= 10; d++) {
            uint32_t pr = gfx24_getpx(b->x + b->w + d, b->y + b->h / 2);
            uint32_t pb = gfx24_getpx(b->x + b->w / 2, b->y + b->h + d);
            int lr = ch_r(pr) + ch_g(pr) + ch_b(pr);
            int lb = ch_r(pb) + ch_g(pb) + ch_b(pb);
            if (lr < darkest) darkest = lr;
            if (lb < darkest) darkest = lb;
        }
        int darker = darkest < bl - 4;
        if (e == 0) {
            if (darker) seam_bad++;          /* flat 档不得有投影 */
        } else if (darker) present++;
        /* 拼缝 0px：底边中段与近角像素亮度差不得出现亮缝（回露底色）。 */
        uint32_t mid = gfx24_getpx(b->x + b->w / 2, b->y + b->h + 2);
        uint32_t near = gfx24_getpx(b->x + b->w - 7, b->y + b->h + 2);
        int lmid = ch_r(mid) + ch_g(mid) + ch_b(mid);
        int lnear = ch_r(near) + ch_g(near) + ch_b(near);
        if (lmid > lnear + 30) seam_bad++;
    }
    probe_leave();
    int ok = present == 5 && seam_bad == 0;
    ser_write(ok ? "GUI:cap2-shadow-slices PASS" : "GUI:cap2-shadow-slices FAIL");
    ser_write(" present="); pd((uint32_t)present);
    ser_write(" seam_bad="); pd((uint32_t)seam_bad);
    ser_write("\n");
}

/* ---- corner-per：逐角 8 点采样 ------------------------------------------- */

static void probe_corner_per(void) {
    if (!probe_enter()) { ser_write("GUI:cap2-corner-per ENTER-FAIL\n"); return; }
    gui_theme_set(TOK_DARK);
    const struct style *tok = gui_active_style();
    uint32_t bg = tok->bg, panel = tok->panel;
    block_id fr = free_root();
    blk_spec s = mk_spec(LAYER_CONTENT, 100, 80);
    s.px_x = 40; s.px_y = 40;
    s.corner_tl = 8; s.corner_tr = 16; s.corner_br = 24; s.corner_bl = 4;
    block_id id = b_rect(fr, &s);
    block *b = btable_get(id);
    layout_run();
    gui_mark_dirty(fr);
    gui_frame();

    /* 外角 4 点应切到底色；边中 4 点应是面板色。 */
    int cut[4], body[4];
    cut[0] = gfx24_getpx(b->x, b->y);
    cut[1] = gfx24_getpx(b->x + b->w - 1, b->y);
    cut[2] = gfx24_getpx(b->x, b->y + b->h - 1);
    cut[3] = gfx24_getpx(b->x + b->w - 1, b->y + b->h - 1);
    body[0] = gfx24_getpx(b->x + b->w / 2, b->y + 2);
    body[1] = gfx24_getpx(b->x + b->w - 2, b->y + b->h / 2);
    body[2] = gfx24_getpx(b->x + b->w / 2, b->y + b->h - 2);
    body[3] = gfx24_getpx(b->x + 2, b->y + b->h / 2);

    int cut_ok = 1, body_ok = 1;
    for (int i = 0; i < 4; i++) {
        if (cut[i] == panel) cut_ok = 0;
        if (body[i] == bg)   body_ok = 0;
    }
    probe_leave();
    int ok = cut_ok && body_ok;
    ser_write(ok ? "GUI:cap2-corner-per PASS" : "GUI:cap2-corner-per FAIL");
    ser_write(" cut="); pd(cut_ok); ser_write(" body="); pd(body_ok);
    ser_write("\n");
}

/* ---- corner-pill：胶囊 ----------------------------------------------------- */

static void probe_corner_pill(void) {
    if (!probe_enter()) { ser_write("GUI:cap2-corner-pill ENTER-FAIL\n"); return; }
    gui_theme_set(TOK_DARK);
    const struct style *tok = gui_active_style();
    uint32_t panel = tok->panel;
    block_id fr = free_root();
    blk_spec s = mk_spec(LAYER_CONTENT, 100, 28);
    s.px_x = 40; s.px_y = 40;
    s.corner_tl = s.corner_tr = s.corner_br = s.corner_bl = 128;
    block_id id = b_rect(fr, &s);
    block *b = btable_get(id);
    layout_run();
    gui_mark_dirty(fr);
    gui_frame();
    int outer = gfx24_getpx(b->x, b->y) == panel;
    int mid = gfx24_getpx(b->x + b->w / 2, b->y + b->h / 2) == panel;
    probe_leave();
    int ok = !outer && mid;
    ser_write(ok ? "GUI:cap2-corner-pill PASS" : "GUI:cap2-corner-pill FAIL");
    ser_write(" outer_cut="); pd(!outer);
    ser_write(" body="); pd(mid);
    ser_write("\n");
}

/* ---- corner-concentric：auto 同心圆角 ------------------------------------- */

static void probe_corner_concentric(void) {
    if (!probe_enter()) { ser_write("GUI:cap2-corner-concentric ENTER-FAIL\n"); return; }
    gui_theme_set(TOK_DARK);
    const struct style *tok = gui_active_style();
    uint32_t panel = tok->panel;
    block_id fr = free_root();
    blk_spec p = mk_spec(LAYER_CONTENT, 220, 140);
    p.px_x = 30; p.px_y = 30;
    p.pad_l = p.pad_r = p.pad_t = p.pad_b = 16;
    p.corner_tl = p.corner_tr = p.corner_br = p.corner_bl = 255;
    block_id pid = b_rect(fr, &p);
    block *bp = btable_get(pid);
    blk_spec c = mk_spec(LAYER_CONTENT, 120, 80);
    c.px_x = 16; c.px_y = 16;
    c.corner_tl = c.corner_tr = c.corner_br = c.corner_bl = 255;
    block_id cid = b_rect(pid, &c);
    block *bc = btable_get(cid);
    layout_run();
    gui_mark_dirty(fr);
    gui_frame();
    int child_body = gfx24_getpx(bc->x + bc->w / 2, bc->y + bc->h / 2) == panel;
    int parent_body = gfx24_getpx(bp->x + bp->w / 2, bp->y + 4) == panel;
    probe_leave();
    int ok = child_body && parent_body;
    ser_write(ok ? "GUI:cap2-corner-concentric PASS" : "GUI:cap2-corner-concentric FAIL");
    ser_write(" child_body="); pd(child_body);
    ser_write(" parent_body="); pd(parent_body);
    ser_write("\n");
}

/* ---- icon-tint：三角色着色 ------------------------------------------------- */

static void probe_icon_tint(void) {
    if (!probe_enter()) { ser_write("GUI:cap2-icon-tint ENTER-FAIL\n"); return; }
    gui_theme_set(TOK_DARK);
    const struct style *tok = gui_active_style();
    uint32_t want[3] = { tok->accent, tok->border, tok->fg };
    uint8_t role[3] = { 3, 4, 1 };
    int hit[3] = { 0, 0, 0 };
    uint32_t actual[3] = { 0, 0, 0 };
    block_id fr = free_root();
    block *ib[3];
    for (int i = 0; i < 3; i++) {
        blk_spec s = mk_spec(LAYER_CONTENT, 32, 32);
        s.px_x = 30 + i * 48; s.px_y = 40;
        block_id id = b_rect(fr, &s);
        block *b = btable_get(id);
        b->kind = BLK_ICON;
        b->icon = 7;                       /* close：X 中心行列有像素 */
        b->tint_role = role[i];
        ib[i] = b;
    }
    layout_run();
    gui_mark_dirty(fr);
    gui_frame();
    for (int i = 0; i < 3; i++) {
        block *b = ib[i];
        /* close 图标 row7 col7/8 点亮；32 档 2×。 */
        uint32_t px = gfx24_getpx(b->x + 7 * 2 + 1, b->y + 7 * 2 + 1);
        actual[i] = px;
        hit[i] = (px == want[i]);
    }
    probe_leave();
    int ok = hit[0] && hit[1] && hit[2];
    ser_write(ok ? "GUI:cap2-icon-tint PASS" : "GUI:cap2-icon-tint FAIL");
    ser_write(" accent="); pd(hit[0]);
    ser_write(" border="); pd(hit[1]);
    ser_write(" fg="); pd(hit[2]);
    ser_write(" act0="); pd(actual[0]);
    ser_write(" act1="); pd(actual[1]);
    ser_write(" act2="); pd(actual[2]);
    ser_write(" want1="); pd(want[1]);
    ser_write(" want2="); pd(want[2]);
    ser_write("\n");
}

/* ---- sb-paint：track / thumb 像素 ----------------------------------------- */

static void probe_sb_paint(void) {
    if (!probe_enter()) { ser_write("GUI:cap2-sb-paint ENTER-FAIL\n"); return; }
    gui_theme_set(TOK_DARK);
    const struct style *tok = gui_active_style();
    block_id fr = free_root();
    blk_spec s = mk_spec(LAYER_CONTENT, 120, 80);
    s.px_x = 40; s.px_y = 40;
    s.overflow = OVF_SCROLL;
    s.scrollbar = 1; s.scroll_axis = 2;
    block_id id = b_rect(fr, &s);
    block *b = btable_get(id);
    blk_spec ch = mk_spec(LAYER_CONTENT, 120, 220);
    ch.px_x = 0; ch.px_y = 0;
    block_id cid = b_rect(id, &ch);
    block *bc = btable_get(cid);
    layout_run();
    gui_mark_dirty(fr);
    gui_frame();

    int tx = b->x + b->w - 3;
    /* thumb 顶部（比例 80/220≈29px）；thumb 下方为 track。 */
    uint32_t thumb = gfx24_getpx(tx, b->y + 8);
    uint32_t track = gfx24_getpx(tx, b->y + b->h - 6);
    int thumb_ok = thumb == tok->fg;
    int track_ok = track == tok->line || track == tok->border;
    probe_leave();
    int ok = thumb_ok && track_ok;
    ser_write(ok ? "GUI:cap2-sb-paint PASS" : "GUI:cap2-sb-paint FAIL");
    ser_write(" thumb="); pd(thumb_ok);
    ser_write(" track="); pd(track_ok);
    ser_write("\n");
}

/* ---- perf：半透明浮层整屏重合成实测 --------------------------------------- */

static void build_perf_tree(void) {
    block_id fr = free_root();
    for (int r = 0; r < 14; r++) {
        blk_spec row = mk_spec(LAYER_WORK, 200, 24);
        row.px_x = 20; row.px_y = 20 + r * 28;
        row.alpha = 200; row.elevation = 2;
        block_id rid = b_rect(fr, &row);
        (void)rid;
    }
    blk_spec ov = mk_spec(LAYER_OVERLAY, 420, 300);
    ov.px_x = 250; ov.px_y = 120;
    ov.alpha = 192; ov.elevation = 4;
    block_id oid = b_rect(fr, &ov);
    block *bo = btable_get(oid);
    for (int r = 0; r < 8; r++) {
        blk_spec it = mk_spec(LAYER_OVERLAY, 32, 32);
        it.px_x = 16 + (r % 4) * 48;
        it.px_y = 24 + (r / 4) * 48;
        it.tint_role = 3;
        block_id iid = b_rect(oid, &it);
        block *bi = btable_get(iid);
        bi->kind = BLK_ICON; bi->icon = (uint8_t)(r & 7);
    }
}

static void probe_perf(void) {
    if (!probe_enter()) { ser_write("GUI:cap2-perf ENTER-FAIL\n"); return; }
    gui_theme_set(TOK_DARK);
    build_perf_tree();
    layout_run();
    gui_mark_dirty(gui_root_block()->id);

    gui_perf_set("reset");
    /* 全量重合成：连续脏帧。 */
    uint32_t full_us = 0; int nf = 12;
    for (int i = 0; i < nf; i++) {
        gui_force_next_frame();
        gui_frame();
        full_us += stat_last_frame_us();
        task_sleep_ticks(3);
    }
    uint32_t full_avg = full_us / (uint32_t)nf;

    /* 损伤区合成：无新脏位，只合成 damage 区。 */
    uint32_t idle_us = 0;
    for (int i = 0; i < nf; i++) {
        gui_force_next_frame();
        gui_frame();
        idle_us += stat_last_frame_us();
        task_sleep_ticks(3);
    }
    uint32_t idle_avg = idle_us / (uint32_t)nf;
    uint32_t area = stat_damage_area();
    probe_leave();

    int ok = idle_avg <= 16000u;         /* 平均帧 ≤16ms 不退化 */
    ser_write(ok ? "GUI:cap2-perf PASS" : "GUI:cap2-perf FAIL");
    ser_write(" full_us="); pd(full_avg);
    ser_write(" idle_us="); pd(idle_avg);
    ser_write(" area="); pd(area);
    ser_write("\n");
}

int cap2_probe_render(const char *arg) {
    if (!arg) return 0;
    if (eq_str(arg, "alpha64"))       { probe_alpha(64);  return 1; }
    if (eq_str(arg, "alpha128"))      { probe_alpha(128); return 1; }
    if (eq_str(arg, "alpha192"))      { probe_alpha(192); return 1; }
    if (eq_str(arg, "shadow-slices")) { probe_shadow();   return 1; }
    if (eq_str(arg, "corner-per"))     { probe_corner_per(); return 1; }
    if (eq_str(arg, "corner-pill"))    { probe_corner_pill(); return 1; }
    if (eq_str(arg, "corner-concentric")) { probe_corner_concentric(); return 1; }
    if (eq_str(arg, "icon-tint"))      { probe_icon_tint(); return 1; }
    if (eq_str(arg, "sb-paint"))       { probe_sb_paint();  return 1; }
    if (eq_str(arg, "perf"))           { probe_perf();     return 1; }
    return 0;
}
