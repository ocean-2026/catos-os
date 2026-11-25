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
    return 0;
}
