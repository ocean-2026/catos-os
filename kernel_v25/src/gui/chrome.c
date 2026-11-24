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

static uint32_t s_idle_frame_tick;
static int gui_idle_step(void);

static int strcmp_probe(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void split_word(const char *arg, char *word, int cap, const char **rest) {
    word[0] = 0; *rest = 0;
    if (!arg) return;
    while (*arg == ' ') arg++;
    int n = 0;
    while (*arg && *arg != ' ' && n < cap - 1) word[n++] = *arg++;
    word[n] = 0;
    while (*arg == ' ') arg++;
    *rest = arg;
}

static void xcopy_local(char *d, int cap, const char *s) {
    int n = 0; if (s) while (s[n]) n++;
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++) d[i] = s[i];
    d[n] = 0;
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
    memset(&s, 0, sizeof(s));
    s.req_w = w; s.req_h = h;
    s.layer = layer; s.z = 0;
    s.visible = 1; s.focusable = 0; s.inert = 0;
    /* 按语义角色跟随活动令牌，不固化当时 RGB；切主题时整树重解析。 */
    s.role_fg = ROLE_FG; s.role_accent = ROLE_ACCENT;
    if (layer == LAYER_BG)                       s.role_bg = ROLE_BG;
    else if (layer == LAYER_CHROME || layer == LAYER_OVERLAY) s.role_bg = ROLE_BAR;
    else                                          s.role_bg = ROLE_PANEL;
    s.fg = INHERIT_COLOR; s.bg = INHERIT_COLOR; s.accent = INHERIT_COLOR;
    s.scale = 1; s.border_w = border;
    return s;
}

static block_id clock_row = 0;
static block_id clock_text_blk = 0;
static char clock_last[16] = "";

#define DEMO_ICON_W 16
#define DEMO_ICON_H 16
static uint8_t demo_icon[DEMO_ICON_W * DEMO_ICON_H * 3];
static int demo_icon_ready;

static void demo_icon_build(void) {
    if (demo_icon_ready) return;
    for (int y = 0; y < DEMO_ICON_H; y++) {
        for (int x = 0; x < DEMO_ICON_W; x++) {
            uint8_t v = ((x ^ y) & 1) ? 0xE0u : 0x20u;
            uint8_t *p = &demo_icon[((y * DEMO_ICON_W) + x) * 3];
            p[0] = v; p[1] = v; p[2] = v;
        }
    }
    demo_icon_ready = 1;
}

/* D7 裁剪探针：一张 200x40 的强调色位图，塞进 80x40 的窄父块，
 * 超出父框的部分必须被裁剪栈切掉，父框外保持桌面底色。 */
#define CROP_BMP_W 200
#define CROP_BMP_H 40
static uint8_t crop_bmp[CROP_BMP_W * CROP_BMP_H * 3];
static int crop_bmp_ready;

static void crop_bmp_build(void) {
    if (crop_bmp_ready) return;
    uint32_t acc = gui_active_style()->accent;
    uint8_t r = (uint8_t)((acc >> 16) & 0xFFu);
    uint8_t g = (uint8_t)((acc >> 8) & 0xFFu);
    uint8_t b = (uint8_t)(acc & 0xFFu);
    for (int i = 0; i < CROP_BMP_W * CROP_BMP_H; i++) {
        crop_bmp[i * 3 + 0] = r;
        crop_bmp[i * 3 + 1] = g;
        crop_bmp[i * 3 + 2] = b;
    }
    crop_bmp_ready = 1;
}

static void shot_crop_build(void) {
    crop_bmp_build();
    blk_spec parent = spec_panel(LAYER_WORK, 80, 40, 1);
    block_id pid = b_column(0, &parent);
    b_image(pid, 0, crop_bmp, CROP_BMP_W, CROP_BMP_H);
    layout_run();
    block *pp = btable_get(pid);
    pp->x = 440; pp->y = 80; pp->w = 80; pp->h = 40; pp->moved = 1;
    for (block *c = pp->first_child; c; c = c->next_sibling) {
        c->x = 440; c->y = 80; c->moved = 1;
    }
}

/* 11b 拖拽残影探针：A 先落在旧矩形 (64,300,120,80)，拖到右侧；
 * 抓帧断言旧矩形区域已回到桌面底色，不残留 A 的旧像素。 */
static void shot_drag_build(void) {
    blk_spec wa = spec_panel(LAYER_WORK, 120, 80, 1);
    block_id A = b_stack(0, &wa);
    b_text(A, 0, "A");
    block_id B = b_stack(0, &wa);
    b_text(B, 0, "B");
    layout_run();
    block *pa = btable_get(A);
    block *pb = btable_get(B);
    pa->x = 64;  pa->y = 300; pa->moved = 1;
    pb->x = 300; pb->y = 300; pb->moved = 1;
    gui_frame();                              /* 先把 A/B 画到固定位置 */
    gui_inject_press(70, 305);
    gui_inject_move(200, 320);
    gui_inject_release();
    gui_frame();                               /* 拖拽后帧：旧矩形应回底色 */
}

static block_id demo_add_sample(void) {
    block_id root_id = gui_root_block()->id;
    blk_spec sample_spec = spec_panel(LAYER_WORK, 320, 200, 1);
    sample_spec.focusable = 1;
    sample_spec.pad_l = sample_spec.pad_r = sample_spec.pad_t = sample_spec.pad_b = 0;
    block_id sample = b_column(root_id, &sample_spec);

    blk_spec title = spec_panel(LAYER_WORK, 320, 24, 0);
    title.pad_l = 4;
    title.align_cross = XALN_CENTER;
    title.role_bg = ROLE_BAR;
    title.bg = INHERIT_COLOR;
    block_id bar = b_row(sample, &title);
    blk_spec title_txt = spec_panel(LAYER_WORK, 96, 16, 0);
    title_txt.align_cross = XALN_CENTER;
    b_text(bar, &title_txt, "CatOS Sample");

    blk_spec body = spec_panel(LAYER_WORK, 320, 176, 0);
    body.pad_l = 4; body.pad_r = 4; body.pad_t = 4; body.pad_b = 4;
    body.gap = 8;
    block_id body_id = b_column(sample, &body);
    blk_spec body_txt = spec_panel(LAYER_WORK, 200, 16, 0);
    b_text(body_id, &body_txt, "demo content area");

    /* 完整链路样例：TEXT → DIVIDER → IMAGE → SPACER 竖排（COLUMN）。 */
    blk_spec rule = spec_panel(LAYER_WORK, 200, 1, 0);
    b_divider(body_id, &rule);
    demo_icon_build();
    b_image(body_id, 0, demo_icon, DEMO_ICON_W, DEMO_ICON_H);
    b_space(body_id, 200, 8);
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
    root->w = GFX24_W; root->h = GFX24_H;
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
    start.align_cross = XALN_CENTER;
    start.on_click = gui_menu_toggle;
    block_id start_id = b_row(taskbar, &start);
    blk_spec start_txt = spec_panel(LAYER_CHROME, 40, 16, 0);
    start_txt.align_cross = XALN_CENTER;
    b_text(start_id, &start_txt, "Start");

    blk_spec clk_s = spec_panel(LAYER_CHROME, 80, 24, 0);
    clk_s.align_main = ALN_END;
    clk_s.align_cross = XALN_CENTER;
    clk_s.pad_l = 4;
    clock_row = b_row(taskbar, &clk_s);
    blk_spec clk_txt = spec_panel(LAYER_CHROME, 64, 16, 0);
    clk_txt.align_cross = XALN_CENTER;
    clock_text_blk = b_text(clock_row, &clk_txt, "00:00:00");
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
    int32_t act = row->z;
    if (act > 0) {
        gui_focus_raise((block_id)act);
    } else if (act == 0) {
        demo_add_sample();
    } else {
        switch (-act) {
        case 1: gui_notepad_open(0); break;
        case 2: gui_canvas_demo();   break;
        case 3: gui_filemgr_open(0); break;
        case 4: gui_images_open(0);  break;
        case 5: gui_browser_open(0); break;
        }
    }
    gui_menu_close();
}

void gui_menu_open(void) {
    if (menu_open) return;
    block_id root_id = gui_root_block()->id;

    blk_spec menu = spec_panel(LAYER_OVERLAY, 180, 0, 0);
    menu.pad_l = 4; menu.pad_r = 4; menu.pad_t = 4; menu.pad_b = 4; menu.gap = 2;
    menu_root = b_column(root_id, &menu);
    block *mr = btable_get(menu_root);
    mr->x = 8;
    mr->y = 400;
    mr->moved = 1;   /* 浮层固定锚点，根 STACK 布局不得改写其绝对位置 */

    block_id menu_ids[32];
    int n = menu_collect(menu_ids, 32);
    static char menu_labels[32][24];   /* 条目标签常驻，避免栈缓冲悬空 */
    for (int i = 0; i < n; i++) {
        blk_spec row = spec_panel(LAYER_OVERLAY, 172, 24, 0);
        row.on_click = on_menu_row;
        row.z = (int32_t)menu_ids[i];
        block_id rid = b_row(menu_root, &row);
        id_label(menu_ids[i], menu_labels[i]);
        b_text(rid, 0, menu_labels[i]);
    }

    static const char *app_labels[5] = { "Notepad", "Canvas demo", "Files", "Images", "Browser" };
    for (int a = 0; a < 5; a++) {
        blk_spec app = spec_panel(LAYER_OVERLAY, 172, 24, 0);
        app.on_click = on_menu_row;
        app.z = -(a + 1);
        block_id aid = b_row(menu_root, &app);
        b_text(aid, 0, app_labels[a]);
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

/* 生产节流实测：走与空闲循环同一条 gui_idle_step 路径跑满 99 tick（≈1s），
 * 不自行 sleep 3 tick 造节拍；帧计数应 ≈33（≤33），同 tick 重复提交为 0。 */
void gui_probe_idle(void) {
    gui_begin();
    gui_demo_build();
    layout_run();

    s_idle_frame_tick = timer_ticks - 3;   /* 首帧立即提交 */
    uint32_t t0 = timer_ticks;
    while (timer_ticks - t0 < 99) {
        gui_idle_step();
        task_sleep_ticks(1);
    }
    uint32_t frames = stat_frames();
    uint32_t dc = stat_double_commit();
    ser_write("GUI:idle frames="); ser_dec(frames);
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
    block_id menu_ids[32];
    items = menu_collect(menu_ids, 32);

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

/* diff 路径两阶段销毁：第二轮描述里删掉一个块，应先进入 leaving 帧
 * （句柄仍在块表、live 暂不回落），再过一帧才由 presence_sweep 回收。 */
void gui_probe_diff(void) {
    static blk_desc d_full[2], d_trim[1];
    static blk_spec sp_root, sp_child;

    gui_begin();
    gui_root_block()->w = 800;
    gui_root_block()->h = 600;

    memset(&sp_root, 0, sizeof(sp_root));
    sp_root.req_w = 200; sp_root.req_h = 120;
    sp_root.layer = LAYER_WORK; sp_root.visible = 1; sp_root.scale = 1;
    sp_root.fg = gui_active_style()->fg; sp_root.bg = gui_active_style()->bg;
    memset(&sp_child, 0, sizeof(sp_child));
    sp_child.req_w = 60; sp_child.req_h = 40;
    sp_child.layer = LAYER_WORK; sp_child.visible = 1; sp_child.scale = 1;
    sp_child.fg = gui_active_style()->fg; sp_child.bg = gui_active_style()->bg;

    d_full[0].id = 1; d_full[0].kind = BLK_STACK; d_full[0].parent = 0;
    d_full[0].spec = &sp_root; d_full[0].text = 0; d_full[0].pix = 0;
    d_full[1].id = 2; d_full[1].kind = BLK_RECT; d_full[1].parent = 1;
    d_full[1].spec = &sp_child; d_full[1].text = 0; d_full[1].pix = 0;
    gui_commit_desc(d_full, 2);
    layout_run();
    gui_frame();
    uint32_t base = stat_blocks_live();

    /* 第二轮描述只留根：id 2 本轮 unseen，应走 leaving 两阶段。 */
    d_trim[0] = d_full[0];
    gui_commit_desc(d_trim, 1);
    layout_run();
    timer_ticks++;               /* 跨帧节拍，避免本帧被同 tick 去重 */
    gui_frame();                 /* leaving 第一帧：句柄仍在块表 */
    block *gone = btable_get(2);
    uint32_t leaving = (gone && gone->leaving) ? 1 : 0;
    uint32_t live_leaving = stat_blocks_live();

    timer_ticks++;               /* 再跨一拍，第二帧由 sweep 回收 */
    gui_frame();
    uint32_t live_after = stat_blocks_live();
    uint32_t leaked = stat_blocks_leaked();

    ser_write("GUI:diff base="); ser_dec(base);
    ser_write(" leaving="); ser_dec(leaving);
    ser_write(" live_leaving="); ser_dec(live_leaving);
    ser_write(" live_after="); ser_dec(live_after);
    ser_write(" leaked="); ser_dec(leaked);
    int ok = leaving == 1 && live_leaving == base &&
             live_after == base - 1 && leaked == 0;
    ser_write(" result="); ser_dec(ok ? 1 : 0); ser_write("\n");
    gui_end();
}

void gui_probe_theme(void) {
    gui_begin();
    gui_demo_build();
    layout_run();

    gui_theme_set(TOK_DARK);
    const struct style *sd = gui_active_style();
    gui_theme_set(TOK_LIGHT);
    const struct style *sl = gui_active_style();
    gui_theme_set(TOK_HIGH_CONTRAST);
    const struct style *sh = gui_active_style();

    int ok = sd->bg != sl->bg && sl->fg != sh->fg && sd->accent != sh->accent;
    ser_write("GUI:theme dark_bg="); ser_hash(sd->bg);
    ser_write(" light_bg="); ser_hash(sl->bg);
    ser_write(" hc_bg="); ser_hash(sh->bg);
    ser_write(" result="); ser_dec(ok ? 1 : 0); ser_write("\n");
    gui_end();
}

void gui_probe_reset(void) {
    gui_begin();
    gui_demo_build();
    layout_run();
    uint32_t nodes = tree_count_nodes();
    uint32_t h = gui_tree_hash();
    uint32_t live = stat_blocks_live();

    gui_reset();
    gui_demo_build();
    layout_run();
    uint32_t nodes2 = tree_count_nodes();
    uint32_t h2 = gui_tree_hash();
    uint32_t live2 = stat_blocks_live();
    uint32_t orphan = live2 > nodes2 ? live2 - nodes2 : 0;

    int ok = nodes == nodes2 && h == h2 && orphan == 0 && live == live2;
    (void)live;
    ser_write("GUI:reset nodes="); ser_dec(nodes2);
    ser_write(" hash="); ser_hash(h2);
    ser_write(" orphan="); ser_dec(orphan);
    ser_write(" match="); ser_dec(h == h2 ? 1 : 0);
    ser_write(" result="); ser_dec(ok ? 1 : 0); ser_write("\n");
    gui_end();
}

static int gesture_clicked;
static void probe_click_consume(block_id id, int32_t lx, int32_t ly) {
    gesture_clicked = 1;   /* 模拟应用块消费点击 */
}

void gui_probe_gesture(void) {
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;

    blk_spec s = spec_panel(LAYER_WORK, 120, 80, 1);
    s.focusable = 1;
    block_id A = b_stack(0, &s);
    block *pa = btable_get(A);
    pa->on_click = probe_click_consume;
    layout_run();
    int x0 = pa->x, y0 = pa->y;

    /* 标题栏拖拽：即便块挂了消费型 on_click，系统拖拽手势仍必须生效。 */
    gui_inject_press(10, 10);
    gui_inject_move(90, 70);
    gui_inject_release();
    int drag_moved = (pa->x != x0 || pa->y != y0);

    /* 任务栏属于系统层：点击仍命中任务栏，不被应用块消费拦截。 */
    blk_spec tb = spec_panel(LAYER_CHROME, GFX24_W, 32, 0);
    block_id tb_id = b_row(0, &tb);
    layout_run();
    block *tbar = btable_get(tb_id);
    block_id tb_hit = gui_hit_test(tbar->x + 10, tbar->y + 16);

    int ok = drag_moved && tb_hit == tb_id && gesture_clicked == 1;
    ser_write("GUI:gesture clicked="); ser_dec(gesture_clicked);
    ser_write(" drag_moved="); ser_dec(drag_moved);
    ser_write(" taskbar_hit="); ser_dec(tb_hit);
    ser_write(" result="); ser_dec(ok ? 1 : 0); ser_write("\n");
    gui_end();
}

void gui_probe_resize(void) {
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;

    blk_spec col = spec_panel(LAYER_WORK, 120, 80, 1);
    block_id A = b_column(0, &col);
    blk_spec leaf = spec_panel(LAYER_CONTENT, 60, 40, 0);
    block_id B = b_rect(A, &leaf);
    blk_spec cs = spec_panel(LAYER_WORK, 60, 40, 1);
    block_id C = b_stack(0, &cs);
    layout_run();

    block *pa = btable_get(A);
    block *pc = btable_get(C);
    int c0x = pc->x, c0y = pc->y;
    int aw0 = pa->w, ah0 = pa->h;

    /* 右下角 resize 手柄：按压角落拖放，改 req_w/req_h，只重排该子树。 */
    int hx = pa->x + pa->w - 6, hy = pa->y + pa->h - 6;
    gui_inject_press(hx, hy);
    gui_inject_move(pa->x + aw0 + 40, pa->y + ah0 + 30);
    gui_inject_release();
    layout_run();

    int grew = pa->w > aw0 && pa->h > ah0;
    int sibling_same = pc->x == c0x && pc->y == c0y;
    block *pb = btable_get(B);
    int leaf_inside = pb->x >= pa->x && pb->y >= pa->y &&
                      pb->x + pb->w <= pa->x + pa->w &&
                      pb->y + pb->h <= pa->y + pa->h;
    int ok = grew && sibling_same && leaf_inside;
    ser_write("GUI:resize w="); ser_dec(pa->w);
    ser_write(" h="); ser_dec(pa->h);
    ser_write(" sibling_same="); ser_dec(sibling_same);
    ser_write(" leaf_inside="); ser_dec(leaf_inside);
    ser_write(" result="); ser_dec(ok ? 1 : 0); ser_write("\n");
    gui_end();
}

/* ---- 组件层自检探针 ------------------------------------------------------ */

static void gui_probe_note(void) {
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;
    gui_notepad_open("/notes.txt");
    gui_frame();
    ser_write("GUI:note lines="); ser_dec(gui_notepad_lines());
    ser_write(" col="); ser_dec(gui_notepad_col());
    ser_write(" dirty="); ser_dec(gui_notepad_dirty());
    ser_write(" full="); ser_dec(gui_notepad_full());
    ser_write(" result="); ser_dec(gui_notepad_active() ? 1 : 0);
    ser_write("\n");
    gui_end();
}

static void gui_probe_cv(void) {
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;
    gui_canvas_demo();
    gui_frame();
    ser_write("GUI:canvas w="); ser_dec(gui_canvas_w());
    ser_write(" h="); ser_dec(gui_canvas_h());
    ser_write(" active="); ser_dec(gui_canvas_active());
    ser_write(" result="); ser_dec(gui_canvas_active() ? 1 : 0);
    ser_write("\n");
    gui_end();
}

static void gui_probe_fm(void) {
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;
    gui_filemgr_open("/");
    gui_frame();
    ser_write("GUI:fm entries="); ser_dec(gui_filemgr_count());
    ser_write(" active="); ser_dec(gui_filemgr_active());
    ser_write(" result="); ser_dec(gui_filemgr_active() ? 1 : 0);
    ser_write("\n");
    gui_end();
}

static void gui_probe_img(void) {
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;
    gui_images_open("/samples/desktop.bmp");
    gui_frame();
    ser_write("GUI:img file="); ser_write(gui_images_name());
    ser_write(" w="); ser_dec(gui_images_w());
    ser_write(" h="); ser_dec(gui_images_h());
    ser_write(" ok="); ser_dec(gui_images_ok());
    ser_write(" result="); ser_dec(gui_images_ok() ? 1 : 0);
    ser_write("\n");
    gui_end();
}

static void gui_probe_br(void) {
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;
    gui_browser_probe_sample();
    gui_frame();
    ser_write("GUI:br bytes="); ser_dec(gui_browser_bytes());
    ser_write(" nodes="); ser_dec(gui_browser_nodes());
    ser_write(" blocks="); ser_dec(gui_browser_blocks());
    ser_write(" active="); ser_dec(gui_browser_active());
    ser_write(" result="); ser_dec(gui_browser_active() ? 1 : 0);
    ser_write("\n");
    gui_end();
}

/* ---- shell 主循环 ------------------------------------------------------- */

/* 生产节拍：PIT 100Hz，每 3 tick 提交一帧（≈33fps）。返回本步是否提交了帧；
 * 同 tick 内的重复提交仍由合成层去重（stat_double_commit）。 */
static int gui_idle_step(void) {
    gui_pump_input();
    clock_refresh(0);
    if (timer_ticks - s_idle_frame_tick < 3)
        return 0;
    s_idle_frame_tick = timer_ticks;
    gui_frame();
    return 1;
}

static void gui_idle_loop(void) {
    s_idle_frame_tick = timer_ticks - 3;   /* 进入桌面后首帧尽快提交 */
    while (!gui_should_exit()) {
        gui_idle_step();
        task_sleep_ticks(1);         /* 100Hz 节拍，帧由 3-tick 门控节流 */
    }
}

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
        if (!strcmp_probe(arg, "idle"))  { gui_probe_idle(); return; }
        if (!strcmp_probe(arg, "home"))  { gui_probe_home(); return; }
        if (!strcmp_probe(arg, "menu"))  { gui_probe_menu(); return; }
        if (!strcmp_probe(arg, "presence")) { gui_probe_presence(); return; }
        if (!strcmp_probe(arg, "diff"))   { gui_probe_diff(); return; }
        if (!strcmp_probe(arg, "theme"))  { gui_probe_theme(); return; }
        if (!strcmp_probe(arg, "reset"))  { gui_probe_reset(); return; }
        if (!strcmp_probe(arg, "gesture")) { gui_probe_gesture(); return; }
        if (!strcmp_probe(arg, "resize"))  { gui_probe_resize(); return; }
        if (!strcmp_probe(arg, "note"))    { gui_probe_note(); return; }
        if (!strcmp_probe(arg, "cv"))      { gui_probe_cv(); return; }
        if (!strcmp_probe(arg, "fm"))      { gui_probe_fm(); return; }
        if (!strcmp_probe(arg, "img"))     { gui_probe_img(); return; }
        if (!strcmp_probe(arg, "br"))      { gui_probe_br(); return; }
    }

    /* 截图专用状态：shot-menu / shot-light / shot-hc / shot-reset /
     * shot-crop（D7 超框裁剪）/ shot-drag（11b 拖拽残影）；
     * 组件态：shot-notepad / shot-canvas / shot-files / shot-images。 */
    int shot_menu = 0, shot_reset = 0, shot_crop = 0, shot_drag = 0;
    enum gui_token shot_tok = TOK_DARK;
    enum { APP_NONE, APP_NOTEPAD, APP_CANVAS, APP_FILEMGR, APP_IMAGES, APP_BROWSER };
    int app_open = APP_NONE;
    char app_path[160]; app_path[0] = 0;
    const char *rest = 0;
    char word[24];
    split_word(arg, word, sizeof(word), &rest);
    if (!strcmp_probe(word, "shot-menu"))  shot_menu = 1;
    else if (!strcmp_probe(word, "shot-light")) shot_tok = TOK_LIGHT;
    else if (!strcmp_probe(word, "shot-hc"))    shot_tok = TOK_HIGH_CONTRAST;
    else if (!strcmp_probe(word, "shot-reset")) shot_reset = 1;
    else if (!strcmp_probe(word, "shot-crop"))  shot_crop = 1;
    else if (!strcmp_probe(word, "shot-drag"))  shot_drag = 1;
    else if (!strcmp_probe(word, "shot-notepad")) { app_open = APP_NOTEPAD; app_path[0] = 0; }
    else if (!strcmp_probe(word, "shot-canvas"))  { app_open = APP_CANVAS; }
    else if (!strcmp_probe(word, "shot-files"))   { app_open = APP_FILEMGR; app_path[0] = 0; }
    else if (!strcmp_probe(word, "shot-images"))  { app_open = APP_IMAGES; app_path[0] = 0; }
    else if (!strcmp_probe(word, "shot-browser")) { app_open = APP_BROWSER; app_path[0] = 0; }
    else if (!strcmp_probe(word, "notepad")) { app_open = APP_NOTEPAD; if (rest) xcopy_local(app_path, sizeof(app_path), rest); }
    else if (!strcmp_probe(word, "canvas"))   { app_open = APP_CANVAS; }
    else if (!strcmp_probe(word, "files"))    { app_open = APP_FILEMGR; if (rest) xcopy_local(app_path, sizeof(app_path), rest); }
    else if (!strcmp_probe(word, "images"))    { app_open = APP_IMAGES; if (rest) xcopy_local(app_path, sizeof(app_path), rest); }
    else if (!strcmp_probe(word, "browser"))   { app_open = APP_BROWSER; if (rest) xcopy_local(app_path, sizeof(app_path), rest); }

    int r = con_request_mode(CON_MODE_VBE24, CON_ORIGIN_CONSOLE);
    if (r != 0) {
        con_write("gui: 进入图形块桌面失败\n");
        return;
    }
    gui_begin();
    gui_demo_build();
    if (shot_reset) { gui_reset(); gui_demo_build(); }
    if (shot_tok != TOK_DARK) gui_theme_set(shot_tok);
    if (shot_menu) gui_menu_open();
    if (shot_crop) shot_crop_build();
    if (shot_drag) shot_drag_build();
    if (app_open == APP_NOTEPAD) gui_notepad_open(app_path[0] ? app_path : 0);
    if (app_open == APP_CANVAS)  gui_canvas_demo();
    if (app_open == APP_FILEMGR) gui_filemgr_open(app_path[0] ? app_path : 0);
    if (app_open == APP_IMAGES)  gui_images_open(app_path[0] ? app_path : 0);
    if (app_open == APP_BROWSER) {
        if (app_path[0]) gui_browser_open(app_path);
        else gui_browser_probe_sample();   /* 无网回归：内嵌样本页 */
    }
    if (!shot_crop && !shot_drag) gui_frame();

    gui_idle_loop();

    gui_end();
    con_force_text();
    con_clear();
}
