/*
 * cap2p_interact.c -- 交互包探针（cap2 §A/§B）。
 * 消费 `gui cap2-<case>` 的 case 名（不含 "cap2-" 前缀）；返回 1 = 已消费。
 * 几何 / 帧位置全部真实取点（块几何 + 内核辅助计数），不写死数字。
 *
 * 覆盖：
 *   pop-down / pop-up / pop-left / pop-right  四向几何 + 8px 间距
 *   pop-clamp      贴角锚定，浮层四边均在屏内安全边距
 *   pop-scroll     内容超高 -> OVF_SCROLL / scroll_axis=2 / 滚动上限
 *   pop-dismiss    外点关闭并回传 dismiss
 *   pop-bridge     400ms 桥内斜移不关，超时后关
 *   anim-outback  12 帧采样：末帧落目标、存在过冲帧并回弹
 *   anim-interrupt 中途改目标 -> 收敛到新目标
 *   anim-reduced   开关后一帧直达目标
 *   sb-wheel      滚轮一格 = 3 行行高；CE 回读 ev_kind=4 wheel_dx/dy
 *   sb-drag       拖 thumb 到轨道 50% / 越界 120% -> 半程 / 上限钳制
 *   sb-edge       内容高 0、thumb 最小尺寸两例不崩不除零，钳回边界
 */
#include <stdint.h>
#include <stddef.h>
#include "gui/block.h"
#include "gui/gui.h"
#include "gui/tokens.h"
#include "gui_int.h"
#include "gfx24.h"
#include "render.h"
#include "serial_log.h"
#include "ce.h"

extern volatile uint32_t timer_ticks;

/* popover.c 探针辅助（gui_int.h 冻结清单之外的本地入口）。 */
extern uint32_t pop_probe_dismiss_count(void);
extern block_id pop_probe_dismiss_id(void);
extern void     pop_probe_bridge_arm(void);
extern void     pop_probe_bridge_expire(void);
extern void     pop_probe_reset(void);

/* prefer 位（与 popover.c 一致）。 */
#define PD_UP    1
#define PD_DOWN  2
#define PD_LEFT  4
#define PD_RIGHT 8

/* ---- 串口输出 ------------------------------------------------------------ */

static void pd(uint32_t v) {
    char b[12]; int n = 0;
    if (!v) { ser_putc('0'); return; }
    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    while (n--) ser_putc(b[n]);
}

static void wi(int v) {
    if (v < 0) { ser_putc('-'); v = -v; }
    pd((uint32_t)v);
}

static int eq_str(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

static void line_head(const char *case_) {
    ser_write("GUI:cap2-"); ser_write(case_); ser_putc(' ');
}

/* ---- 探针环境 ------------------------------------------------------------ */

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

static block_id free_root(void) {
    blk_spec s = mk_spec(LAYER_BG, GFX24_W, GFX24_H);
    s.role_bg = ROLE_BG;
    return b_free(0, &s);
}

static block_id anchor_rect(block_id fr, int x, int y, int w, int h) {
    blk_spec s = mk_spec(LAYER_WORK, w, h);
    s.px_x = (int16_t)x; s.px_y = (int16_t)y;
    return b_rect(fr, &s);
}

static block_id pop_make(block_id fr, block_id anchor, uint8_t prefer,
                        uint16_t max_h, const char *txt) {
    blk_spec s = mk_spec(LAYER_WORK, -1, -1);
    s.anchor_id = anchor;
    s.pop_prefer = prefer;
    s.pop_max_h = max_h;
    block_id pop = b_column(fr, &s);
    /* 内核侧探针无 b_popover 构造器：直接把节点类型置为 BLK_POPOVER，
     * 与 CE 提交 rec kind=BLK_POPOVER 同口径。 */
    block *pb = btable_get(pop);
    if (pb) pb->kind = BLK_POPOVER;
    blk_spec t = mk_spec(LAYER_WORK, -1, -1);
    b_text(pop, &t, txt);
    return pop;
}

static void kv_rect(const char *tag, block *b) {
    ser_write(" "); ser_write(tag); ser_putc('=');
    wi(b ? b->x : -1); ser_putc(',');
    wi(b ? b->y : -1); ser_putc(',');
    wi(b ? b->w : -1); ser_putc(',');
    wi(b ? b->h : -1);
}

/* ---- 四向几何 ------------------------------------------------------------ */

static int report_dir(const char *case_, block *a, block *p, int gap, int ok) {
    line_head(case_);
    ser_write(ok ? "PASS" : "FAIL");
    kv_rect("anchor", a);
    kv_rect("pop", p);
    ser_write(" gap="); wi(gap);
    ser_putc('\n');
    return 1;
}

static int case_down(const char *arg) {
    (void)arg;
    if (!probe_enter()) { line_head("pop-down"); ser_write("FAIL enter=0\n"); return 1; }
    block_id fr = free_root();
    block_id an = anchor_rect(fr, 100, 100, 80, 40);
    block_id pid = pop_make(fr, an, PD_DOWN, 0, "menu item");
    layout_run(); gui_frame();

    block *a = btable_get(an), *p = btable_get(pid);
    int gap = p ? p->y - (a->y + a->h) : -1;
    int in_x = p && p->x >= 8 && p->x + p->w <= GFX24_W - 8;
    int ok = p && a && gap == 8 && in_x && p->w > 0 && p->h > 0;
    int r = report_dir("pop-down", a, p, gap, ok);
    probe_leave();
    return r;
}

static int case_up(const char *arg) {
    (void)arg;
    if (!probe_enter()) { line_head("pop-up"); ser_write("FAIL enter=0\n"); return 1; }
    block_id fr = free_root();
    block_id an = anchor_rect(fr, 100, 300, 80, 40);
    block_id pid = pop_make(fr, an, PD_UP, 0, "menu item");
    layout_run(); gui_frame();

    block *a = btable_get(an), *p = btable_get(pid);
    int gap = p ? a->y - (p->y + p->h) : -1;
    int in_x = p && p->x >= 8 && p->x + p->w <= GFX24_W - 8;
    int ok = p && a && gap == 8 && in_x && p->w > 0 && p->h > 0;
    int r = report_dir("pop-up", a, p, gap, ok);
    probe_leave();
    return r;
}

static int case_left(const char *arg) {
    (void)arg;
    if (!probe_enter()) { line_head("pop-left"); ser_write("FAIL enter=0\n"); return 1; }
    block_id fr = free_root();
    block_id an = anchor_rect(fr, 400, 100, 80, 40);
    block_id pid = pop_make(fr, an, PD_LEFT, 0, "menu item");
    layout_run(); gui_frame();

    block *a = btable_get(an), *p = btable_get(pid);
    int gap = p ? a->x - (p->x + p->w) : -1;
    int in_y = p && p->y >= 8 && p->y + p->h <= GFX24_H - 8;
    int ok = p && a && gap == 8 && in_y && p->w > 0 && p->h > 0;
    int r = report_dir("pop-left", a, p, gap, ok);
    probe_leave();
    return r;
}

static int case_right(const char *arg) {
    (void)arg;
    if (!probe_enter()) { line_head("pop-right"); ser_write("FAIL enter=0\n"); return 1; }
    block_id fr = free_root();
    block_id an = anchor_rect(fr, 100, 100, 80, 40);
    block_id pid = pop_make(fr, an, PD_RIGHT, 0, "menu item");
    layout_run(); gui_frame();

    block *a = btable_get(an), *p = btable_get(pid);
    int gap = p ? p->x - (a->x + a->w) : -1;
    int in_y = p && p->y >= 8 && p->y + p->h <= GFX24_H - 8;
    int ok = p && a && gap == 8 && in_y && p->w > 0 && p->h > 0;
    int r = report_dir("pop-right", a, p, gap, ok);
    probe_leave();
    return r;
}

/* ---- 贴角钳制：四边均在屏内安全边距 -------------------------------------- */

static int case_clamp(const char *arg) {
    (void)arg;
    if (!probe_enter()) { line_head("pop-clamp"); ser_write("FAIL enter=0\n"); return 1; }
    block_id fr = free_root();
    block_id an = anchor_rect(fr, GFX24_W - 40, 8, 30, 30);
    block_id pid = pop_make(fr, an, PD_DOWN | PD_RIGHT, 0, "clamp me");
    layout_run(); gui_frame();

    block *p = btable_get(pid);
    int ok = p && p->x >= 8 && p->y >= 8 &&
             p->x + p->w <= GFX24_W - 8 && p->y + p->h <= GFX24_H - 8;
    line_head("pop-clamp");
    ser_write(ok ? "PASS" : "FAIL");
    kv_rect("pop", p);
    ser_putc('\n');
    probe_leave();
    return 1;
}

/* ---- 超高内部滚动 --------------------------------------------------------- */

static int case_scroll(const char *arg) {
    (void)arg;
    if (!probe_enter()) { line_head("pop-scroll"); ser_write("FAIL enter=0\n"); return 1; }
    block_id fr = free_root();
    block_id an = anchor_rect(fr, 100, 100, 80, 40);
    blk_spec s = mk_spec(LAYER_WORK, -1, -1);
    s.anchor_id = an; s.pop_prefer = PD_DOWN; s.pop_max_h = 60;
    block_id pid = b_column(fr, &s);
    { block *pp = btable_get(pid); if (pp) pp->kind = BLK_POPOVER; }
    for (int i = 0; i < 10; i++) {
        blk_spec t = mk_spec(LAYER_WORK, -1, -1);
        b_text(pid, &t, "scroll line");
    }
    layout_run(); gui_frame();

    block *p = btable_get(pid);
    int max_scroll = p ? p->pop_content_h - 60 : -1;
    int ok = p && p->overflow == OVF_SCROLL && p->scroll_axis == 2 &&
             p->pop_content_h > 60 && p->scroll_y >= 0 &&
             p->scroll_y <= max_scroll;
    line_head("pop-scroll");
    ser_write(ok ? "PASS" : "FAIL");
    ser_write(" content_h="); wi(p ? p->pop_content_h : -1);
    ser_write(" max_scroll="); wi(max_scroll);
    ser_write(" scroll_y="); wi(p ? p->scroll_y : -1);
    ser_write(" ovf="); wi(p ? p->overflow : -1);
    ser_write(" axis="); wi(p ? p->scroll_axis : -1);
    ser_putc('\n');
    probe_leave();
    return 1;
}

/* ---- 外点关闭 + dismiss 回传 -------------------------------------------- */

static int case_dismiss(const char *arg) {
    (void)arg;
    if (!probe_enter()) { line_head("pop-dismiss"); ser_write("FAIL enter=0\n"); return 1; }
    pop_probe_reset();
    block_id fr = free_root();
    block_id an = anchor_rect(fr, 100, 100, 80, 40);
    block_id pid = pop_make(fr, an, PD_DOWN, 0, "dismiss me");
    layout_run(); gui_frame();

    gui_inject_press(5, 5);              /* 浮层外点 */
    gui_presence_sweep(); gui_presence_sweep();
    block *p = btable_get(pid);
    int alive = p && p->visible && p->parent;
    uint32_t cnt = pop_probe_dismiss_count();
    int ok = !alive && cnt == 1;
    line_head("pop-dismiss");
    ser_write(ok ? "PASS" : "FAIL");
    ser_write(" alive="); wi(alive);
    ser_write(" count="); pd(cnt);
    ser_write(" did="); pd(pop_probe_dismiss_id());
    ser_putc('\n');
    probe_leave();
    return 1;
}

/* ---- 斜向 hover 桥 400ms ------------------------------------------------- */

static int case_bridge(const char *arg) {
    (void)arg;
    if (!probe_enter()) { line_head("pop-bridge"); ser_write("FAIL enter=0\n"); return 1; }
    pop_probe_reset();
    block_id fr = free_root();
    block_id an = anchor_rect(fr, 100, 100, 80, 40);
    block_id pid = pop_make(fr, an, PD_DOWN, 0, "bridge me");
    layout_run(); gui_frame();

    /* 桥窗口内：外点不关。 */
    pop_probe_bridge_arm();
    gui_inject_press(5, 5);
    block *p1 = btable_get(pid);
    int kept = p1 && p1->visible && p1->parent;
    uint32_t cnt1 = pop_probe_dismiss_count();

    /* 桥过期后：外点关闭。 */
    pop_probe_bridge_expire();
    gui_inject_press(5, 5);
    gui_presence_sweep(); gui_presence_sweep();
    block *p2 = btable_get(pid);
    int closed = !(p2 && p2->visible && p2->parent);
    uint32_t cnt2 = pop_probe_dismiss_count();

    int ok = kept && cnt1 == 0 && closed && cnt2 == 1;
    line_head("pop-bridge");
    ser_write(ok ? "PASS" : "FAIL");
    ser_write(" kept="); wi(kept);
    ser_write(" cnt1="); pd(cnt1);
    ser_write(" closed="); wi(closed);
    ser_write(" cnt2="); pd(cnt2);
    ser_putc('\n');
    probe_leave();
    return 1;
}

/* ---- 动画：out-back 12 帧落点 ------------------------------------------- */

static int case_anim_outback(const char *arg) {
    (void)arg;
    if (!probe_enter()) { line_head("anim-outback"); ser_write("FAIL enter=0\n"); return 1; }
    block_id fr = free_root();
    blk_spec s = mk_spec(LAYER_WORK, 80, 40);
    s.px_x = 0; s.px_y = 0;
    s.anim_ms = 192; s.anim_ease = 5; s.anim_mask = 1;   /* 仅 x */
    block_id id = b_rect(fr, &s);
    layout_run(); gui_frame();

    block *b = btable_get(id);
    anim_kick(b);
    blk_spec t2 = s; t2.px_x = 120;
    b_set_props(id, &t2);
    b->dirty_layout = 1;
    layout_run();                            /* 目标 = 120 */

    int max_x = b->x, last_x = b->x, over = 0;
    int frames[12];
    for (int f = 0; f < 12; f++) {
        anim_tick(16);
        anim_apply(b);
        frames[f] = b->x;
        if (b->x > max_x) max_x = b->x;
        if (b->x > 120) over = 1;
        last_x = b->x;
    }
    int ok = last_x == 120 && over && max_x > 120;
    line_head("anim-outback");
    ser_write(ok ? "PASS" : "FAIL");
    ser_write(" last="); wi(last_x);
    ser_write(" max="); wi(max_x);
    ser_write(" over="); wi(over);
    ser_write(" target=120");
    ser_write(" frames=");
    for (int f = 0; f < 12; f++) {
        if (f) ser_putc(',');
        wi(frames[f]);
    }
    ser_putc('\n');
    probe_leave();
    return 1;
}

/* ---- 动画：中途改目标，收敛新目标 ---------------------------------------- */

static int case_anim_interrupt(const char *arg) {
    (void)arg;
    if (!probe_enter()) { line_head("anim-interrupt"); ser_write("FAIL enter=0\n"); return 1; }
    block_id fr = free_root();
    blk_spec s = mk_spec(LAYER_WORK, 80, 40);
    s.px_x = 0; s.px_y = 0;
    s.anim_ms = 192; s.anim_ease = 1; s.anim_mask = 1;
    block_id id = b_rect(fr, &s);
    layout_run(); gui_frame();

    block *b = btable_get(id);
    anim_kick(b);
    blk_spec ta = s; ta.px_x = 80;
    b_set_props(id, &ta); b->dirty_layout = 1; layout_run();
    for (int f = 0; f < 3; f++) { anim_tick(16); anim_apply(b); }

    /* 中途改目标到 160：重新 kick 接管，起点 = 当前帧值。 */
    anim_kick(b);
    blk_spec tb = s; tb.px_x = 160;
    b_set_props(id, &tb); b->dirty_layout = 1; layout_run();
    for (int f = 0; f < 12; f++) { anim_tick(16); anim_apply(b); }

    int ok = b->x == 160;
    line_head("anim-interrupt");
    ser_write(ok ? "PASS" : "FAIL");
    ser_write(" x="); wi(b->x);
    ser_write(" target=160 busy="); wi(anim_busy());
    ser_putc('\n');
    probe_leave();
    return 1;
}

/* ---- 动画：reduced motion 一帧直达 --------------------------------------- */

static int case_anim_reduced(const char *arg) {
    (void)arg;
    if (!probe_enter()) { line_head("anim-reduced"); ser_write("FAIL enter=0\n"); return 1; }
    anim_reduced(1);
    block_id fr = free_root();
    blk_spec s = mk_spec(LAYER_WORK, 80, 40);
    s.px_x = 0; s.px_y = 0;
    s.anim_ms = 192; s.anim_ease = 1; s.anim_mask = 1;
    block_id id = b_rect(fr, &s);
    blk_spec t2 = s; t2.px_x = 100;
    b_set_props(id, &t2);
    layout_run();
    block *b = btable_get(id);
    anim_kick(b);
    anim_apply(b);
    int ok = !anim_busy() && b->x == 100;
    line_head("anim-reduced");
    ser_write(ok ? "PASS" : "FAIL");
    ser_write(" x="); wi(b->x);
    ser_write(" target=100 busy="); wi(anim_busy());
    ser_putc('\n');
    anim_reduced(0);
    probe_leave();
    return 1;
}

/* ---- sb-wheel / sb-drag / sb-edge：滚动条生产路径探针 --------------------- */

/* decl.c 探针专用入口（内核自检；与 cap2p_ce.c 同口径）。 */
extern int      decl_ce_probe_commit(int tid, const void *recs, int n,
                                     const char *strings, int slen,
                                     uint32_t rec_size);
extern int      decl_ce_probe_poll2(int tid, ce_blk_event2_t *ev);
extern block_id decl_ce_probe_local_to_global(int tid, int lid);
extern void     decl_ce_probe_cleanup(int tid);
/* input.c 探针注入钩子：复用真实命中 / 滚轮 / 拖拽路径。 */
extern void     gui_inject_wheel(int x, int y, int dz);

static void sb_rec2_init(ce_blk_rec2_t *r, int id, int parent, uint8_t kind) {
    for (unsigned i = 0; i < sizeof(*r); i++) ((uint8_t *)r)[i] = 0;
    r->base.id = id; r->base.parent = parent;
    r->base.kind = kind; r->base.layer = LAYER_WORK;
    r->base.req_w = -1; r->base.req_h = -1;
    r->base.text_off = -1; r->base.pix_off = -1;
    r->base.pix_w = 0; r->base.pix_h = 0; r->base.res = -1;
    r->cap_ver = CE_BLK_REC2_VER;
}

/* 搭一棵归属于 tid 的纵向滚动面板：vw×vh 视口，内部子块高 ch_h（0 = 无子块）。 */
static block *sb_probe_panel(int tid, int vw, int vh, int ch_h) {
    ce_blk_rec2_t r;
    sb_rec2_init(&r, 1, 0, BLK_COLUMN);
    if (decl_ce_probe_commit(tid, &r, 1, "", 0,
                             (uint32_t)sizeof(ce_blk_rec2_t)) != 0)
        return 0;
    block_id gid = decl_ce_probe_local_to_global(tid, 1);
    block *b = btable_get(gid);
    if (!b) return 0;
    b->overflow = OVF_SCROLL;
    b->scrollbar = 1; b->scroll_axis = 2;
    b->req_w = vw; b->req_h = vh;
    b->x = 40; b->y = 40; b->w = vw; b->h = vh;
    if (ch_h > 0) {
        blk_spec ch = mk_spec(LAYER_CONTENT, vw, ch_h);
        ch.px_x = 0; ch.px_y = 0;
        b_rect(gid, &ch);
    }
    layout_run();
    gui_frame();
    return b;
}

/* thumb 纵向几何（复刻 sb.c y_thumb，探针只取数不写死像素）。 */
static void sb_thumb_y(block *b, int ch, int track, int *ty, int *th) {
    int view = b->h;
    int ratio = ch > view ? view * 255 / ch : 255;
    int len = track * ratio / 255;
    if (len < 16) len = 16;
    if (len > track) len = track;
    int maxs = ch - view;
    int frac = maxs > 0 ? b->scroll_y * (track - len) / maxs : 0;
    *ty = b->y + frac;
    *th = len;
}

/* sb-wheel：滚轮 1 格 -> thumb 位移 = 3 行 × 行高；CE 侧回读 ev_kind=4。 */
static int case_sb_wheel(const char *arg) {
    (void)arg;
    if (!probe_enter()) { line_head("sb-wheel"); ser_write("FAIL enter=0\n"); return 1; }
    int tid = 6;
    decl_ce_probe_cleanup(tid);
    block *b = sb_probe_panel(tid, 120, 80, 220);
    if (!b) {
        line_head("sb-wheel"); ser_write("FAIL panel=0\n");
        probe_leave(); return 1;
    }

    const struct style *tok = gui_active_style();
    int line = tok->line_h ? tok->line_h : 16;
    int want = 3 * line;
    int before = b->scroll_y;
    gui_inject_wheel(b->x + b->w / 2, b->y + b->h / 2, 1);
    int after = b->scroll_y;

    ce_blk_event2_t ev;
    for (unsigned i = 0; i < sizeof(ev); i++) ((uint8_t *)&ev)[i] = 0;
    int got = decl_ce_probe_poll2(tid, &ev);
    int ev_ok = got == 1 && ev.ev_kind == 4 &&
                ev.wheel_dx == 0 && ev.wheel_dy == 3;
    int ok = (after - before) == want && ev_ok;

    line_head("sb-wheel");
    ser_write(ok ? "PASS" : "FAIL");
    ser_write(" delta="); wi(after - before);
    ser_write(" want="); wi(want);
    ser_write(" ev="); wi(got);
    ser_write(" kind="); wi(ev.ev_kind);
    ser_write(" dx="); wi(ev.wheel_dx);
    ser_write(" dy="); wi(ev.wheel_dy);
    ser_putc('\n');
    decl_ce_probe_cleanup(tid);
    probe_leave();
    return 1;
}

/* sb-drag：拖 thumb 到轨道 50% -> 半程；甩到轨道外 120% -> 钳到上限。 */
static int case_sb_drag(const char *arg) {
    (void)arg;
    if (!probe_enter()) { line_head("sb-drag"); ser_write("FAIL enter=0\n"); return 1; }
    int tid = 7;
    decl_ce_probe_cleanup(tid);
    int vw = 120, vh = 80, ch_h = 220;
    block *b = sb_probe_panel(tid, vw, vh, ch_h);
    if (!b) {
        line_head("sb-drag"); ser_write("FAIL panel=0\n");
        probe_leave(); return 1;
    }
    int maxs = ch_h - vh;

    int my, mh; sb_thumb_y(b, ch_h, vh, &my, &mh);
    int grab = mh / 2;
    int px = b->x + b->w - 3;
    gui_inject_press(px, my + grab);

    /* 半程：thumb 顶 = 轨道顶 + 半程 × (track-len)/maxs，指针再加抓取偏移。 */
    int half = maxs / 2;
    int mid_y = b->y + half * (vh - mh) / maxs + grab;
    gui_inject_move(px, mid_y);
    int s_half = b->scroll_y;

    /* 越界 120%：指针甩到轨道下方之外，必须钳到上限 maxs。 */
    gui_inject_move(px, b->y + vh + vh / 5);
    int s_top = b->scroll_y;
    gui_inject_release();

    int ok = s_half == half && s_top == maxs;
    line_head("sb-drag");
    ser_write(ok ? "PASS" : "FAIL");
    ser_write(" half="); wi(s_half);
    ser_write(" top="); wi(s_top);
    ser_write(" maxs="); wi(maxs);
    ser_write(" thumb="); wi(mh);
    ser_putc('\n');
    decl_ce_probe_cleanup(tid);
    probe_leave();
    return 1;
}

/* sb-edge：内容高为 0、thumb 最小尺寸两例不崩、不除零，越界钳回边界。 */
static int case_sb_edge(const char *arg) {
    (void)arg;
    if (!probe_enter()) { line_head("sb-edge"); ser_write("FAIL enter=0\n"); return 1; }
    int tid = 8;
    decl_ce_probe_cleanup(tid);

    /* 例①：内容高为 0（无子块）。滚轮 + 按下 / 拖出轨道 / 松开，保持原位 0。 */
    block *b0 = sb_probe_panel(tid, 120, 80, 0);
    int e0_wheel = -1, e0_drag = -1;
    if (b0) {
        gui_inject_wheel(b0->x + b0->w / 2, b0->y + b0->h / 2, 1);
        e0_wheel = b0->scroll_y;
        gui_inject_press(b0->x + b0->w - 3, b0->y + b0->h / 2);
        gui_inject_move(b0->x + b0->w - 3, b0->y + b0->h + 40);
        gui_inject_release();
        e0_drag = b0->scroll_y;
    }

    /* 例②：thumb 最小尺寸（内容超高，thumb 钳到 16px）。甩出上下两端 -> 0 / maxs。 */
    decl_ce_probe_cleanup(tid);
    int vh = 80, ch_h = 800;
    block *b1 = sb_probe_panel(tid, 120, vh, ch_h);
    int maxs = ch_h - vh;
    int e1_low = -1, e1_high = -1, mh = -1;
    if (b1) {
        int my, len; sb_thumb_y(b1, ch_h, vh, &my, &len);
        mh = len;
        int grab = len / 2;
        int px = b1->x + b1->w - 3;
        gui_inject_press(px, my + grab);
        gui_inject_move(px, b1->y - 40);            /* 甩出轨道上端 */
        e1_low = b1->scroll_y;
        gui_inject_move(px, b1->y + vh + 40);       /* 甩出轨道下端 */
        e1_high = b1->scroll_y;
        gui_inject_release();
    }

    int ok = b0 && b1 && e0_wheel == 0 && e0_drag == 0 &&
             e1_low == 0 && e1_high == maxs && mh == 16;
    line_head("sb-edge");
    ser_write(ok ? "PASS" : "FAIL");
    ser_write(" empty_wheel="); wi(e0_wheel);
    ser_write(" empty_drag="); wi(e0_drag);
    ser_write(" min_low="); wi(e1_low);
    ser_write(" min_high="); wi(e1_high);
    ser_write(" maxs="); wi(maxs);
    ser_write(" thumb="); wi(mh);
    ser_putc('\n');
    decl_ce_probe_cleanup(tid);
    probe_leave();
    return 1;
}

int cap2_probe_interact(const char *arg) {
    if (!arg) return 0;
    if (eq_str(arg, "pop-down"))       return case_down(arg);
    if (eq_str(arg, "pop-up"))         return case_up(arg);
    if (eq_str(arg, "pop-left"))       return case_left(arg);
    if (eq_str(arg, "pop-right"))      return case_right(arg);
    if (eq_str(arg, "pop-clamp"))      return case_clamp(arg);
    if (eq_str(arg, "pop-scroll"))      return case_scroll(arg);
    if (eq_str(arg, "pop-dismiss"))    return case_dismiss(arg);
    if (eq_str(arg, "pop-bridge"))     return case_bridge(arg);
    if (eq_str(arg, "anim-outback"))   return case_anim_outback(arg);
    if (eq_str(arg, "anim-interrupt"))  return case_anim_interrupt(arg);
    if (eq_str(arg, "anim-reduced"))    return case_anim_reduced(arg);
    if (eq_str(arg, "sb-wheel"))        return case_sb_wheel(arg);
    if (eq_str(arg, "sb-drag"))         return case_sb_drag(arg);
    if (eq_str(arg, "sb-edge"))         return case_sb_edge(arg);
    return 0;
}
