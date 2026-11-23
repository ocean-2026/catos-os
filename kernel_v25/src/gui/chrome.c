/*
 * chrome.c -- 内核内 demo 块树、shell 主循环与自检探针（规格 §10 M0）。
 *
 * v1 供给方只有内核内模块：任务栏、开始按钮占位、时钟、示例块。
 * 不做应用节点化；CE 接入留 v2。
 */
#include "gui/gui.h"
#include "gui_int.h"
#include "gui/tokens.h"
#include "gfx24.h"
#include "render.h"
#include "rtc.h"
#include "serial_log.h"
#include "task.h"
#include "kstring.h"

static void geom_walk(block *b);
static void gui_menu_toggle(block_id id, int32_t lx, int32_t ly);

static int strcmp_probe(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void ser_dec(uint32_t v) {
    char b[12]; int n = 0;
    if (v == 0) { ser_write("0"); return; }
    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) ser_putc(b[--n]);
}

static void ser_hash(uint32_t h) {
    ser_write("0x");
    for (int i = 7; i >= 0; i--) {
        int d = (h >> (i * 4)) & 15;
        ser_putc((char)(d < 10 ? '0' + d : 'A' + d - 10));
    }
}

/* ---- demo 块树 ---------------------------------------------------------- */

static blk_spec spec_panel(uint16_t layer, int32_t w, int32_t h, uint8_t border) {
    blk_spec s;
    const struct style *tok = gui_active_style();
    memset(&s, 0, sizeof(s));
    s.req_w = w; s.req_h = h;
    s.layer = layer; s.z = 0;
    s.visible = 1; s.focusable = 0; s.inert = 0;
    s.fg = tok->fg; s.bg = tok->bg; s.accent = tok->accent;
    s.scale = 1; s.border_w = border;
    return s;
}

static block_id clock_row = 0;
static block_id clock_text_blk = 0;
static char clock_last[16] = "";

static block_id demo_add_sample(void) {
    block_id root_id = gui_root_block()->id;
    blk_spec sample_spec = spec_panel(LAYER_WORK, 320, 200, 1);
    sample_spec.focusable = 1;
    sample_spec.pad_l = sample_spec.pad_r = sample_spec.pad_t = sample_spec.pad_b = 0;
    block_id sample = b_column(root_id, &sample_spec);

    blk_spec title = spec_panel(LAYER_WORK, 320, 24, 0);
    title.pad_l = 4;
    block_id bar = b_row(sample, &title);
    b_text(bar, 0, "CatOS Sample");

    blk_spec body = spec_panel(LAYER_WORK, 320, 176, 0);
    body.pad_l = 4;
    block_id body_id = b_rect(sample, &body);
    blk_spec body_txt = spec_panel(LAYER_WORK, 200, 16, 0);
    b_text(body_id, &body_txt, "demo content area");
    return sample;
}

static void clock_refresh(int force) {
    rtc_time_t t;
    rtc_read(&t);
    char clk[16];
    int o = 0;
    clk[o++] = (char)('0' + t.hour / 10); clk[o++] = (char)('0' + t.hour % 10);
    clk[o++] = ':';
    clk[o++] = (char)('0' + t.minute / 10); clk[o++] = (char)('0' + t.minute % 10);
    clk[o++] = ':';
    clk[o++] = (char)('0' + t.second / 10); clk[o++] = (char)('0' + t.second % 10);
    clk[o] = 0;
    if (!force && clock_last[0] && strcmp_probe(clk, clock_last) == 0) return;
    for (int i = 0; i < 16; i++) clock_last[i] = clk[i];
    block *tb = btable_get(clock_text_blk);
    if (tb) {
        tb->text = clock_last;
        tb->dirty_paint = 1;   /* 只打自身脏位，不整树重排 */
    }
}

void gui_demo_build(void) {
    block *root = gui_root_block();
    root->pad_l = 64; root->pad_r = 64; root->pad_t = 64; root->pad_b = 64;
    block_id root_id = root->id;

    /* 桌面底色：全出血贴屏，忽略父级 padding。 */
    blk_spec bg = spec_panel(LAYER_BG, GFX24_W, GFX24_H, 0);
    block_id bg_id = b_rect(root_id, &bg);
    btable_get(bg_id)->bleed = 1;

    demo_add_sample();

    /* 任务栏：全出血贴底，宽 = 屏宽，交叉轴 END 钉下边缘。 */
    blk_spec bar_tb = spec_panel(LAYER_CHROME, GFX24_W, 32, 0);
    bar_tb.align_cross = XALN_END;
    block_id taskbar = b_row(root_id, &bar_tb);
    btable_get(taskbar)->bleed = 1;

    blk_spec start = spec_panel(LAYER_CHROME, 64, 24, 0);
    start.pad_l = 4;
    start.on_click = gui_menu_toggle;
    block_id start_id = b_row(taskbar, &start);
    b_text(start_id, 0, "Start");

    blk_spec clk_s = spec_panel(LAYER_CHROME, 80, 24, 0);
    clk_s.align_main = ALN_END;
    clk_s.pad_l = 4;
    clock_row = b_row(taskbar, &clk_s);
    clock_text_blk = b_text(clock_row, 0, "00:00:00");
    clock_refresh(1);
}

/* ---- 开始菜单（LAYER_OVERLAY，模态） ---------------------------------- */

static block_id menu_root = 0;
static int      menu_open = 0;

static void id_label(block_id id, char *out) {
    const char *pre = "block ";
    int o = 0;
    while (*pre) out[o++] = *pre++;
    char num[10]; int n = 0;
    if (id == 0) num[n++] = '0';
    while (id) { num[n++] = (char)('0' + id % 10); id /= 10; }
    while (n) out[o++] = num[--n];
    out[o] = 0;
}

static void menu_clear_inert(void) {
    for (uint32_t id = 1; id <= btable_capacity(); id++) {
        block *b = btable_get(id);
        if (b) b->inert = 0;
    }
}

static int menu_collect(block_id *out, int cap) {
    int n = 0;
    for (uint32_t id = 1; id <= btable_capacity() && n < cap; id++) {
        block *b = btable_get(id);
        if (b && b->layer == LAYER_WORK && b->focusable) out[n++] = id;
    }
    return n;
}

static void on_menu_row(block_id id, int32_t lx, int32_t ly) {
    block *row = btable_get(id);
    if (!row) return;
    block_id target = (block_id)row->z;
    if (target) gui_focus_raise(target);
    else        demo_add_sample();
    gui_menu_close();
}

void gui_menu_open(void) {
    if (menu_open) return;
    block_id root_id = gui_root_block()->id;

    blk_spec menu = spec_panel(LAYER_OVERLAY, 180, 24, 0);
    menu.pad_l = 4; menu.pad_r = 4; menu.pad_t = 4; menu.pad_b = 4; menu.gap = 2;
    menu_root = b_column(root_id, &menu);
    btable_get(menu_root)->x = 8;
    btable_get(menu_root)->y = 400;

    block_id wins[32];
    int n = menu_collect(wins, 32);
    for (int i = 0; i < n; i++) {
        blk_spec row = spec_panel(LAYER_OVERLAY, 172, 24, 0);
        row.on_click = on_menu_row;
        row.z = (int32_t)wins[i];
        block_id rid = b_row(menu_root, &row);
        char lbl[24];
        id_label(wins[i], lbl);
        b_text(rid, 0, lbl);
    }

    blk_spec launch = spec_panel(LAYER_OVERLAY, 172, 24, 0);
    launch.on_click = on_menu_row;
    launch.z = 0;
    block_id lid = b_row(menu_root, &launch);
    b_text(lid, 0, "Launch demo");

    gui_modal_inert(menu_root);
    menu_open = 1;
}

void gui_menu_close(void) {
    if (!menu_open) return;
    if (menu_root) { b_remove(menu_root); menu_root = 0; }
    menu_clear_inert();
    menu_open = 0;
}

int gui_menu_is_open(void) { return menu_open; }

static void gui_menu_toggle(block_id id, int32_t lx, int32_t ly) {
    if (menu_open) gui_menu_close(); else gui_menu_open();
}

/* ---- 自检探针 ---------------------------------------------------------- */

#define PROBE_DESC_MAX 64

void gui_probe_idempotent(void) {
    static blk_desc descs[PROBE_DESC_MAX];
    static blk_spec specs[PROBE_DESC_MAX];
    uint32_t seed = 2463534242u;
    int fail = 0;

    gui_begin();
    gui_root_block()->w = 800;
    gui_root_block()->h = 600;

    for (int round = 0; round < 20; round++) {
        seed = seed * 1664525u + 1013904223u;
        int n = 5 + (int)((seed >> 16) % 20);
        if (n > PROBE_DESC_MAX) n = PROBE_DESC_MAX;
        for (int i = 0; i < n; i++) {
            seed = seed * 1664525u + 1013904223u;
            memset(&specs[i], 0, sizeof(specs[i]));
            specs[i].req_w = 20 + (int)((seed >> 13) % 200);
            specs[i].req_h = 20 + (int)((seed >> 7) % 120);
            specs[i].pad_l = specs[i].pad_r = 2;
            specs[i].pad_t = specs[i].pad_b = 2;
            specs[i].gap = 2;
            specs[i].layer = LAYER_CONTENT;
            specs[i].visible = 1;
            specs[i].border_w = 1; specs[i].scale = 1;
            specs[i].fg = gui_active_style()->fg;
            specs[i].bg = gui_active_style()->bg;
            descs[i].id = (block_id)(i + 1);
            descs[i].kind = i == 0 ? BLK_STACK
                : (blk_kind)(BLK_RECT + ((seed >> 5) % 4));
            descs[i].parent = i == 0 ? 0 : 1;
            descs[i].spec = &specs[i];
            descs[i].text = 0; descs[i].pix = 0;
        }
        gui_rebuild(descs, n);
        layout_run();
        uint32_t h1 = gui_tree_hash();
        gui_commit_desc(descs, n);
        layout_run();
        uint32_t h2 = gui_tree_hash();
        if (h1 != h2) fail = 1;
        ser_write("GUI:idempotent round="); ser_dec((uint32_t)round);
        ser_write(" hash="); ser_hash(h2);
        ser_write(" match="); ser_dec(h1 == h2 ? 1 : 0); ser_write("\n");
    }
    ser_write(fail ? "GUI:idempotent=FAIL\n" : "GUI:idempotent=OK\n");
    gui_end();
}

void gui_probe_leak(void) {
    gui_begin();
    for (int i = 0; i < 1000; i++) {
        block_id id = b_rect(0, 0);
        b_remove(id);
        /* 批次回收：两轮 sweep 对应 leaving 两帧，句柄归还空闲链。 */
        if ((i & 7) == 7) { gui_presence_sweep(); gui_presence_sweep(); }
    }
    gui_presence_sweep();
    gui_presence_sweep();
    ser_write("GUI:leak live="); ser_dec(stat_blocks_live());
    ser_write(" tree="); ser_dec(tree_count_nodes());
    ser_write(" leaked="); ser_dec(stat_blocks_leaked()); ser_write("\n");
    gui_end();
}

void gui_probe_dirty(void) {
    gui_begin();
    gui_root_block()->w = 800;
    gui_root_block()->h = 600;

    blk_spec col = spec_panel(LAYER_CONTENT, 0, 0, 0);
    col.gap = 2;
    col.pad_l = col.pad_r = col.pad_t = col.pad_b = 2;
    block_id A = b_column(0, &col);
    blk_spec leaf = spec_panel(LAYER_CONTENT, 60, 40, 1);
    block_id a1 = b_rect(A, &leaf);
    b_rect(A, &leaf);
    block_id B = b_column(0, &col);
    b_rect(B, &leaf);
    b_rect(B, &leaf);

    layout_run();

    blk_spec ch = leaf;
    ch.req_w = 120;
    b_set_props(a1, &ch);
    layout_run();

    uint32_t measured = stat_layout_nodes();
    uint32_t total = tree_count_nodes();
    block *pa = btable_get(A);
    uint32_t sub = tree_count_subtree(pa);

    ser_write("GUI:dirty measured="); ser_dec(measured);
    ser_write(" total="); ser_dec(total);
    ser_write(" affected_subtree="); ser_dec(sub);
    ser_write(" untouched="); ser_dec(total - sub - 1);
    int ok = (measured <= sub + 1) && (measured < total);
    ser_write(" result="); ser_dec(ok ? 1 : 0); ser_write("\n");
    geom_walk(gui_root_block());
    gui_end();
}

void gui_probe_drag(void) {
    gui_begin();
    gui_root_block()->w = 800;
    gui_root_block()->h = 600;

    blk_spec wa = spec_panel(LAYER_WORK, 120, 80, 1);
    block_id A = b_stack(0, &wa);
    b_text(A, 0, "A");
    block_id B = b_stack(0, &wa);
    b_text(B, 0, "B");
    layout_run();

    block *pa = btable_get(A);
    block *pb = btable_get(B);
    /* B 挪到右下，避免与 A 在原点重叠造成命中归属歧义。 */
    pb->x = 200; pb->y = 100; pb->moved = 1;
    int b0x = pb->x, b0y = pb->y, b0w = pb->w, b0h = pb->h;

    gui_inject_press(20, 10);    /* A 标题栏区域 */
    gui_inject_move(130, 110);    /* 拖到新位置 */
    gui_inject_release();

    gui_frame();

    uint32_t measured = stat_layout_nodes();
    uint32_t rects = stat_damage_rects();
    uint32_t area = stat_damage_area();
    uint32_t us = stat_last_frame_us();
    uint32_t sub = tree_count_subtree(pa);
    int b_same = pb->x == b0x && pb->y == b0y && pb->w == b0w && pb->h == b0h;

    ser_write("GUI:drag ax="); ser_dec((uint32_t)pa->x);
    ser_write(" ay="); ser_dec((uint32_t)pa->y);
    ser_write(" bx="); ser_dec((uint32_t)pb->x);
    ser_write(" by="); ser_dec((uint32_t)pb->y);
    ser_write(" measured="); ser_dec(measured);
    ser_write(" total="); ser_dec(tree_count_nodes());
    ser_write(" subtree="); ser_dec(sub);
    ser_write(" damage_rects="); ser_dec(rects);
    ser_write(" damage_area="); ser_dec(area);
    ser_write(" frame_us="); ser_dec(us);
    ser_write(" b_same="); ser_dec(b_same ? 1 : 0);
    int ok = b_same && measured <= sub + 1 && area <= 120000u && us <= 4000u;
    ser_write(" result="); ser_dec(ok ? 1 : 0); ser_write("\n");
    gui_end();
}

void gui_probe_hit(void) {
    gui_begin();
    gui_root_block()->w = 800;
    gui_root_block()->h = 600;

    blk_spec lo = spec_panel(LAYER_CONTENT, 160, 100, 1);
    b_stack(0, &lo);
    blk_spec hi = spec_panel(LAYER_CONTENT, 160, 100, 1);
    hi.z = 1;
    block_id Hi = b_stack(0, &hi);
    layout_run();

    block_id hit = gui_hit_test(80, 80);
    ser_write("GUI:hit hit="); ser_dec(hit);
    ser_write(" want="); ser_dec(Hi);
    int ok = hit == Hi;
    ser_write(" result="); ser_dec(ok ? 1 : 0); ser_write("\n");
    gui_end();
}

extern volatile uint32_t timer_ticks;

void gui_probe_rate(void) {
    gui_begin();
    gui_demo_build();
    layout_run();

    uint32_t t0 = timer_ticks;
    for (int i = 0; i < 33; i++) {   /* 约 1 秒，3 tick/帧 */
        gui_frame();
        task_sleep_ticks(3);
    }
    uint32_t frames = stat_frames();
    uint32_t dc = stat_double_commit();
    ser_write("GUI:rate frames="); ser_dec(frames);
    ser_write(" double_commit="); ser_dec(dc);
    ser_write(" ticks="); ser_dec(timer_ticks - t0);
    int ok = frames <= 33 && dc == 0;
    ser_write(" result="); ser_dec(ok ? 1 : 0); ser_write("\n");
    gui_end();
}

static void geom_walk(block *b) {
    ser_write("GUI:geom id="); ser_dec(b->id);
    ser_write(" kind="); ser_dec((uint32_t)b->kind);
    ser_write(" x="); ser_dec((uint32_t)b->x);
    ser_write(" y="); ser_dec((uint32_t)b->y);
    ser_write(" w="); ser_dec((uint32_t)b->w);
    ser_write(" h="); ser_dec((uint32_t)b->h);
    ser_write(" layer="); ser_dec(b->layer);
    ser_write(" z="); ser_dec((uint32_t)b->z);
    ser_write("\n");
    for (block *c = b->first_child; c; c = c->next_sibling)
        geom_walk(c);
}

void gui_probe_geom(void) {
    gui_begin();
    gui_demo_build();
    layout_run();
    geom_walk(gui_root_block());
    gui_end();
}

static int bounds_walk(block *b, int *bad) {
    int ok = 1;
    for (block *c = b->first_child; c; c = c->next_sibling) {
        int px = b->x, py = b->y, pw = b->w, ph = b->h;
        if (!c->bleed) {
            px += b->pad_l; py += b->pad_t;
            pw -= b->pad_l + b->pad_r; ph -= b->pad_t + b->pad_b;
        }
        int inside = c->x >= px && c->y >= py &&
                     c->x + c->w <= px + pw && c->y + c->h <= py + ph;
        if (!inside) {
            *bad = 1;
            ser_write("GUI:bounds id="); ser_dec(c->id);
            ser_write(" parent="); ser_dec(b->id);
            ser_write(" child=("); ser_dec(c->x); ser_write(","); ser_dec(c->y);
            ser_write(","); ser_dec(c->w); ser_write(","); ser_dec(c->h);
            ser_write(") parent=("); ser_dec(px); ser_write(","); ser_dec(py);
            ser_write(","); ser_dec(pw); ser_write(","); ser_dec(ph);
            ser_write(")\n");
        }
        ok &= inside;
        ok &= bounds_walk(c, bad);
    }
    return ok;
}

void gui_probe_bounds(void) {
    gui_begin();
    gui_demo_build();
    layout_run();
    int bad = 0;
    block *root = gui_root_block();
    int root_ok = root->x >= 0 && root->y >= 0 &&
                  root->x + root->w <= GFX24_W && root->y + root->h <= GFX24_H;
    if (!root_ok) bad = 1;
    bounds_walk(root, &bad);
    ser_write("GUI:bounds result="); ser_dec(bad ? 0 : 1); ser_write("\n");
    gui_end();
}

void gui_probe_home(void) {
    gui_begin();
    gui_demo_build();
    gui_frame();                 /* 首帧全量合成，打点 GUI:frame first=1 */
    uint32_t us = stat_last_frame_us();
    ser_write("GUI:home frame_us="); ser_dec(us);
    ser_write(" result="); ser_dec(us <= 8000u ? 1 : 0); ser_write("\n");
    gui_end();
}

void gui_probe_menu(void) {
    gui_begin();
    gui_demo_build();
    layout_run();

    gui_menu_open();
    layout_run();

    int overlay = 0;
    for (uint32_t id = 1; id <= btable_capacity(); id++) {
        block *b = btable_get(id);
        if (b && b->layer == LAYER_OVERLAY) overlay++;
    }
    int items = 0;
    block_id wins[32];
    items = menu_collect(wins, 32);

    /* 触发 Launch demo 条目：应新增一个示例块并关闭菜单。 */
    block *mr = btable_get(menu_root);
    block *launch = 0;
    if (mr) for (block *c = mr->first_child; c; c = c->next_sibling) launch = c;
    if (launch && launch->on_click) launch->on_click(launch->id, 0, 0);
    layout_run();
    int work_after = 0;
    for (uint32_t id = 1; id <= btable_capacity(); id++) {
        block *b = btable_get(id);
        if (b && b->layer == LAYER_WORK && b->focusable) work_after++;
    }

    ser_write("GUI:menu open="); ser_dec(menu_open);
    ser_write(" items="); ser_dec(items);
    ser_write(" overlay="); ser_dec(overlay);
    ser_write(" launched="); ser_dec(work_after);
    ser_write(" result="); ser_dec((overlay > 0 && work_after == 2) ? 1 : 0);
    ser_write("\n");

    gui_menu_close();
    gui_end();
}

void gui_probe_presence(void) {
    gui_begin();
    gui_demo_build();
    layout_run();
    uint32_t base = stat_blocks_live();

    blk_spec s = spec_panel(LAYER_WORK, 40, 30, 1);
    block_id q = b_stack(0, &s);
    layout_run();
    uint32_t added = stat_blocks_live();

    extern volatile uint32_t timer_ticks;
    b_remove(q);
    gui_frame();                 /* leaving 第一帧：句柄仍在块表 */
    block *qb = btable_get(q);
    uint32_t leaving = qb ? 1 : 0;
    uint32_t live_leaving = stat_blocks_live();
    timer_ticks++;               /* 跨过帧节拍，避免第二帧被同 tick 去重 */
    gui_frame();                 /* 下一帧回收，句柄归还空闲链 */
    uint32_t live_after = stat_blocks_live();
    uint32_t leaked = stat_blocks_leaked();

    ser_write("GUI:presence base="); ser_dec(base);
    ser_write(" added="); ser_dec(added);
    ser_write(" leaving="); ser_dec(leaving);
    ser_write(" live_leaving="); ser_dec(live_leaving);
    ser_write(" live_after="); ser_dec(live_after);
    ser_write(" leaked="); ser_dec(leaked);
    ser_write(" result=");
    ser_dec((live_after == base && leaked == 0 && leaving == 1) ? 1 : 0);
    ser_write("\n");
    gui_end();
}

/* ---- shell 主循环 ------------------------------------------------------- */

void gui_shell_cmd(const char *arg) {
    if (arg && *arg) {
        if (!strcmp_probe(arg, "idem"))  { gui_probe_idempotent(); return; }
        if (!strcmp_probe(arg, "leak")) { gui_probe_leak(); return; }
        if (!strcmp_probe(arg, "geom"))  { gui_probe_geom(); return; }
        if (!strcmp_probe(arg, "bounds")) { gui_probe_bounds(); return; }
        if (!strcmp_probe(arg, "dirty")) { gui_probe_dirty(); return; }
        if (!strcmp_probe(arg, "drag"))  { gui_probe_drag(); return; }
        if (!strcmp_probe(arg, "hit"))   { gui_probe_hit(); return; }
        if (!strcmp_probe(arg, "rate"))  { gui_probe_rate(); return; }
        if (!strcmp_probe(arg, "home"))  { gui_probe_home(); return; }
        if (!strcmp_probe(arg, "menu"))  { gui_probe_menu(); return; }
        if (!strcmp_probe(arg, "presence")) { gui_probe_presence(); return; }
    }
    int r = con_request_mode(CON_MODE_VBE24, CON_ORIGIN_CONSOLE);
    if (r != 0) {
        con_write("gui: 进入图形块桌面失败\n");
        return;
    }
    gui_begin();
    gui_demo_build();
    gui_frame();

    while (!gui_should_exit()) {
        gui_pump_input();
        clock_refresh(0);
        gui_frame();                 /* 同 tick 重复调用由合成层去重 */
        task_sleep_ticks(1);         /* 100Hz 节拍，3 tick 一帧 ≈33fps */
    }

    gui_end();
    con_force_text();
    con_clear();
}
