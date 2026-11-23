/*
 * compose.c -- 帧调度、damage 合并、统计打点（规格 §4 / §5 / §9）。
 *
 * 节拍：PIT 100Hz，同一 tick 内重复 gui_frame() 只算一次提交
 * （stat_double_commit）。damage 矩形上限 8 个，超出退化为包围盒；
 * 只合成脏区，禁止整帧重绘。
 */
#include "gui/gui.h"
#include "gui_int.h"
#include "gui/tokens.h"
#include "gfx24.h"
#include "render.h"
#include "render_be.h"
#include "mouse.h"
#include "kstring.h"

extern volatile uint32_t timer_ticks;
extern uint64_t perf_now(void);
extern uint32_t perf_mhz(void);

#define DMG_MAX 8
static struct { int x, y, w, h; } dmg[DMG_MAX];
static int dmg_n;
static int dmg_full;

static uint32_t s_frames, s_layout_nodes, s_double_commit;
static uint32_t s_damage_rects, s_damage_area;
static uint64_t s_frame_cycles;
static uint32_t s_last_us;
static uint32_t s_last_tick;

/* ---- damage 合并 -------------------------------------------------------- */

static int rects_overlap(int ax, int ay, int aw, int ah,
                         int bx, int by, int bw, int bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

static void dmg_reset(void) { dmg_n = 0; dmg_full = 0; }

static void dmg_add(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (dmg_full) return;
    for (int i = 0; i < dmg_n; i++) {
        if (rects_overlap(x, y, w, h, dmg[i].x, dmg[i].y, dmg[i].w, dmg[i].h)) {
            int x0 = x < dmg[i].x ? x : dmg[i].x;
            int y0 = y < dmg[i].y ? y : dmg[i].y;
            int x1 = x + w > dmg[i].x + dmg[i].w ? x + w : dmg[i].x + dmg[i].w;
            int y1 = y + h > dmg[i].y + dmg[i].h ? y + h : dmg[i].y + dmg[i].h;
            dmg[i].x = x0; dmg[i].y = y0; dmg[i].w = x1 - x0; dmg[i].h = y1 - y0;
            return;
        }
    }
    if (dmg_n < DMG_MAX) {
        dmg[dmg_n].x = x; dmg[dmg_n].y = y;
        dmg[dmg_n].w = w; dmg[dmg_n].h = h;
        dmg_n++;
        return;
    }
    /* 超出上限：退化为全部已有 damage 的包围盒。 */
    int x0 = dmg[0].x, y0 = dmg[0].y;
    int x1 = x0 + dmg[0].w, y1 = y0 + dmg[0].h;
    for (int i = 1; i < dmg_n; i++) {
        if (dmg[i].x < x0) x0 = dmg[i].x;
        if (dmg[i].y < y0) y0 = dmg[i].y;
        if (dmg[i].x + dmg[i].w > x1) x1 = dmg[i].x + dmg[i].w;
        if (dmg[i].y + dmg[i].h > y1) y1 = dmg[i].y + dmg[i].h;
    }
    dmg[0].x = x0; dmg[0].y = y0; dmg[0].w = x1 - x0; dmg[0].h = y1 - y0;
    dmg_n = 1;
    dmg_full = 1;
}

/* ---- 绘制顺序收集：layer 升序 → z 升序 → 树先序（稳定） --------------- */

#define GUI_PAINT_MAX 256
static block *paint_list[GUI_PAINT_MAX];
static int paint_count;

static void collect(block *b) {
    if (!b) return;
    if (b->visible && paint_count < GUI_PAINT_MAX)
        paint_list[paint_count++] = b;
    for (block *c = b->first_child; c; c = c->next_sibling)
        collect(c);
}

static void sort_paint(void) {
    /* 稳定插入排序：layer 升序、z 升序；先序次序作为平局保留。 */
    for (int i = 1; i < paint_count; i++) {
        block *key = paint_list[i];
        int j = i - 1;
        while (j >= 0 &&
               (paint_list[j]->layer > key->layer ||
                (paint_list[j]->layer == key->layer &&
                 paint_list[j]->z > key->z))) {
            paint_list[j + 1] = paint_list[j];
            j--;
        }
        paint_list[j + 1] = key;
    }
}

extern void ser_write(const char *s);
extern void ser_putc(char c);

static void ser_dec_u(uint32_t v) {
    char buf[12]; int n = 0;
    if (v == 0) { ser_write("0"); return; }
    while (v) { buf[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) ser_putc(buf[--n]);
}

/* ---- 统计 --------------------------------------------------------------- */

void stat_set_layout_nodes(uint32_t n) { s_layout_nodes = n; }
void stat_add_layout_nodes(uint32_t n)  { s_layout_nodes += n; }
void stat_note_double_commit(void)      { s_double_commit++; }

void stat_note_frame(uint64_t cycles, uint32_t rects, uint32_t area) {
    s_frame_cycles = cycles;
    s_damage_rects = rects;
    s_damage_area = area;
    uint32_t mhz = perf_mhz();
    /* freestanding 无 64 位除法：取低 32 位周期做 32 位换算。 */
    s_last_us = mhz ? (uint32_t)((uint32_t)cycles / mhz) : 0;
}

uint32_t stat_frames(void)          { return s_frames; }
uint32_t stat_layout_nodes(void)    { return s_layout_nodes; }
uint32_t stat_blocks_live(void)     { return btable_live(); }
uint32_t stat_blocks_leaked(void)   {
    uint32_t live = btable_live();
    uint32_t tree = tree_count_nodes();
    return live > tree ? live - tree : 0;
}
uint32_t stat_double_commit(void)    { return s_double_commit; }
uint32_t stat_damage_rects(void)     { return s_damage_rects; }
uint32_t stat_damage_area(void)      { return s_damage_area; }
uint64_t stat_frame_cycles(void)    { return s_frame_cycles; }
uint32_t stat_last_frame_us(void)   { return s_last_us; }

/* ---- 帧 ----------------------------------------------------------------- */

void gui_frame(void) {
    if (timer_ticks == s_last_tick) {
        stat_note_double_commit();
        return;
    }
    s_last_tick = timer_ticks;

    uint64_t t0 = perf_now();

    paint_count = 0;
    collect(gui_root_block());
    sort_paint();

    /* 布局清位前先快照本帧脏块；布局阶段会清 dirty_layout，绘制以快照为准。 */
    for (int i = 0; i < paint_count; i++)
        paint_list[i]->snap =
            (paint_list[i]->dirty_layout || paint_list[i]->dirty_paint) ? 1 : 0;

    layout_run();

    dmg_reset();
    paint_begin_frame();

    /* 第一遍：收集脏块矩形（v1 记当前矩形），合并成 damage 链表。 */
    for (int i = 0; i < paint_count; i++) {
        block *b = paint_list[i];
        if (b->snap)
            dmg_add(b->x, b->y, b->w, b->h);
    }

    /* 第二遍：只重绘本帧脏节点。干净节点即使与 damage 相交也不整节点
     * 重绘——背景/容器的命中重绘会把尚未重绘的干净前景整片擦掉（§4
     * 损伤合成的擦旧位置应由背景在损伤条带内补齐，而不是整节点重绘）。 */
    for (int i = 0; i < paint_count; i++) {
        block *b = paint_list[i];
        if (!b->snap) continue;
        paint_node(b);
    }

    /* 光标：合成结束后统一画，使用后端已有的 save/restore 精灵机制。 */
    int mx, my; uint8_t btn;
    mouse_get_state(&mx, &my, &btn);
    struct render_be *be = con_backend();
    if (be && be->mouse_cursor) be->mouse_cursor(mx, my, 1);

    uint64_t t1 = perf_now();

    uint32_t area = 0;
    for (int i = 0; i < dmg_n; i++) area += (uint32_t)(dmg[i].w * dmg[i].h);
    stat_note_frame(t1 - t0, (uint32_t)dmg_n, area);

    uint32_t seq = s_frames;
    if (seq == 0) {
        /* 首帧全量合成打点：验收首帧 ≤8ms；frame_us 别名供测量器直取。 */
        ser_write("GUI:frame first=1 us="); ser_dec_u(s_last_us);
        ser_write(" rects="); ser_dec_u((uint32_t)dmg_n);
        ser_write(" area="); ser_dec_u(area);
        ser_write(" frame_us="); ser_dec_u(s_last_us);
        ser_write("\n");
    } else if (seq % 30 == 0) {
        ser_write("GUI:frame n="); ser_dec_u(seq + 1);
        ser_write(" us="); ser_dec_u(s_last_us);
        ser_write("\n");
    }
    s_frames++;

    gui_presence_sweep();
}

static void mark_all_paint(block *b) {
    if (!b) return;
    b->dirty_paint = 1;
    b->snap = 1;
    for (block *c = b->first_child; c; c = c->next_sibling)
        mark_all_paint(c);
}

/* 切主题：换活动令牌后整树标脏重绘（不做局部配色残留）。 */
void gui_theme_set(enum gui_token t) {
    gui_set_token(t);
    mark_all_paint(gui_root_block());
}

/* Presence 回收：标 leaving 的块存活一帧（leaving 帧计数），
 * 下一帧末尾归还块表空闲链；计数到 2 才真正回收。 */
void gui_presence_sweep(void) {
    for (uint32_t id = 1; id <= btable_capacity(); id++) {
        block *b = btable_get(id);
        if (!b || !b->leaving) continue;
        if (b->leaving >= 2) {
            b->parent = b->first_child = b->next_sibling = 0;
            btable_free(b);
        } else {
            b->leaving++;
        }
    }
}

void gui_reset(void) {
    tree_reset();
    dmg_reset();
    s_frames = 0; s_layout_nodes = 0; s_double_commit = 0;
    s_damage_rects = 0; s_damage_area = 0;
    s_frame_cycles = 0; s_last_us = 0;
    s_last_tick = 0;
}

void gui_begin(void) {
    gui_reset();
    gfx24_clear(gui_active_style()->bg);
    block *root = gui_root_block();
    root->x = 0; root->y = 0;
    root->w = GFX24_W; root->h = GFX24_H;
    root->dirty_layout = 1;
}

void gui_end(void) {
    gui_reset();
}
