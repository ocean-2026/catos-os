/*
 * chrome.c -- 内核内 demo 块树、shell 主循环与自检探针（规格 §10 M0）。
 *
 * v1 供给方只有内核内模块：任务栏、开始按钮占位、时钟、示例块。
 * 不做应用节点化；CE 接入留 v2。
 */
#include "gui/gui.h"
#include "gui_int.h"
#include "render.h"
#include "rtc.h"
#include "serial_log.h"
#include "task.h"
#include "kstring.h"

static void geom_walk(block *b);

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

static blk_spec spec_rect(uint16_t layer, int32_t w, int32_t h,
                         uint32_t bg, uint32_t fg, uint8_t border) {
    blk_spec s;
    memset(&s, 0, sizeof(s));
    s.req_w = w; s.req_h = h;
    s.layer = layer; s.z = 0;
    s.visible = 1; s.focusable = 0; s.inert = 0;
    s.fg = fg; s.bg = bg; s.accent = 0x4C8DFFu;
    s.scale = 1; s.border_w = border;
    return s;
}

void gui_demo_build(void) {
    block *root = gui_root_block();
    root->pad_l = 64; root->pad_r = 64; root->pad_t = 64; root->pad_b = 64;
    block_id root_id = root->id;

    /* 桌面底色 */
    blk_spec bg = spec_rect(LAYER_BG, 800, 600, 0x1E1E26u, 0xE8E8ECu, 0);
    b_rect(root_id, &bg);

    /* 示例块：COLUMN = 标题栏 + 内容区，320x200，可拖拽 / 可聚焦 */
    blk_spec win = spec_rect(LAYER_WORK, 320, 200, 0x2D2D38u, 0xE8E8ECu, 1);
    win.focusable = 1;
    win.pad_l = win.pad_r = win.pad_t = win.pad_b = 0;
    block_id sample = b_column(root_id, &win);

    blk_spec title = spec_rect(LAYER_WORK, 320, 24, 0x3C3C4Eu, 0xF2F2F6u, 0);
    block_id bar = b_row(sample, &title);
    b_text(bar, 0, "CatOS 示例块");

    blk_spec body = spec_rect(LAYER_WORK, 320, 176, 0x26262Eu, 0xB8B8C4u, 0);
    block_id body_id = b_rect(sample, &body);
    blk_spec body_txt = spec_rect(LAYER_WORK, 200, 16, 0x26262Eu, 0xB8B8C4u, 0);
    b_text(body_id, &body_txt, "demo content area");

    /* 任务栏（CHROME 层，交叉轴 END 钉底） */
    blk_spec bar_tb = spec_rect(LAYER_CHROME, 800, 32, 0x17171Du, 0xE8E8ECu, 0);
    bar_tb.align_cross = XALN_END;
    block_id taskbar = b_row(root_id, &bar_tb);
    blk_spec start = spec_rect(LAYER_CHROME, 64, 24, 0x3C3C4Eu, 0xF2F2F6u, 0);
    block_id start_id = b_row(taskbar, &start);
    b_text(start_id, 0, "开始");

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
    blk_spec clk_s = spec_rect(LAYER_CHROME, 80, 24, 0x17171Du, 0xE8E8ECu, 0);
    clk_s.align_main = ALN_END;
    block_id clk_id = b_row(taskbar, &clk_s);
    b_text(clk_id, 0, clk);
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
            specs[i].fg = 0xFFFFFFu; specs[i].bg = 0x333333u;
            specs[i].border_w = 1; specs[i].scale = 1;
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
    }
    ser_write("GUI:leak live="); ser_dec(stat_blocks_live());
    ser_write(" tree="); ser_dec(tree_count_nodes());
    ser_write(" leaked="); ser_dec(stat_blocks_leaked()); ser_write("\n");
    gui_end();
}

void gui_probe_dirty(void) {
    gui_begin();
    gui_root_block()->w = 800;
    gui_root_block()->h = 600;

    blk_spec col = spec_rect(LAYER_CONTENT, 0, 0, 0x26262Eu, 0xFFFFFFu, 0);
    col.gap = 2;
    col.pad_l = col.pad_r = col.pad_t = col.pad_b = 2;
    block_id A = b_column(0, &col);
    blk_spec leaf = spec_rect(LAYER_CONTENT, 60, 40, 0x333333u, 0xFFFFFFu, 1);
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

/* ---- shell 主循环 ------------------------------------------------------- */

void gui_shell_cmd(const char *arg) {
    if (arg && *arg) {
        if (!strcmp_probe(arg, "idem"))  { gui_probe_idempotent(); return; }
        if (!strcmp_probe(arg, "leak")) { gui_probe_leak(); return; }
        if (!strcmp_probe(arg, "geom"))  { gui_probe_geom(); return; }
        if (!strcmp_probe(arg, "dirty")) { gui_probe_dirty(); return; }
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
        gui_frame();                 /* 同 tick 重复调用由合成层去重 */
        task_sleep_ticks(1);         /* 100Hz 节拍，3 tick 一帧 ≈33fps */
    }

    gui_end();
    con_force_text();
    con_clear();
}
