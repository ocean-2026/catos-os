/*
 * popover.c -- cap2 锚定浮层：measure -> place -> 翻转 -> 钳制(8px) -> 内部滚动。
 *
 * 口径（规格 cap2 §A）：
 *   - 两趟：先按子内容自然高写 pop_content_h，再 place 到锚点矩形旁。
 *   - 方向按 pop_prefer 位序（默认 down→up→right→left）；首选方向空间不足
 *     翻转对侧；两侧都不足则钳制回屏（允许压住锚点）。四边 8px 安全边距，
 *     越界一律不出屏。
 *   - 高度 min(pop_content_h, pop_max_h)；pop_max_h==0 不限。超出则本块
 *     OVF_SCROLL、scroll_axis=2(y)，滚动条走已有 sb_*。
 *   - 层级：块 layer 为 0 时置 LAYER_OVERLAY+1（4001），不高于 TOAST。
 *   - 关闭：点在最上层浮层矩形外 -> 关闭并向 owner 投 decl_post_dismiss；
 *     ESC 同理。子菜单斜向 hover 桥 400ms：窗口内不外点关闭。
 *
 * 整数实现；不新增大静态数组。
 */
#include "gui/block.h"
#include "gui/gui.h"
#include "gui_int.h"
#include "gfx24.h"

#define POP_GAP      8
#define POP_SAFE     8
#define BRIDGE_TICKS 40              /* 100Hz PIT：400ms */

/* prefer 位：up1 / down2 / left4 / right8。 */
#define PD_UP    1
#define PD_DOWN  2
#define PD_LEFT  4
#define PD_RIGHT 8

extern volatile uint32_t timer_ticks;

/* ---- 探针可见的关闭记录（owner==0 的内核探针不走 CE 队列） ---- */
static uint32_t pop_dismiss_count;
static block_id pop_dismiss_id;

/* ---- 斜向 hover 桥 ------------------------------------------------------ */
static uint32_t bridge_until;
static int      prev_in_pop;

static int inside(block *b, int x, int y) {
    return x >= b->x && x < b->x + b->w &&
           y >= b->y && y < b->y + b->h;
}

static int clampv(int v, int lo, int hi) {
    if (lo > hi) return lo;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* 最上层可见浮层：layer 高 -> z 高 -> id 大（树先序后挂）。 */
static block *top_pop(void) {
    block *best = 0;
    for (uint32_t id = 1; id <= btable_capacity(); id++) {
        block *b = btable_get(id);
        if (!b || b->kind != BLK_POPOVER || !b->visible) continue;
        if (!best || b->layer > best->layer ||
            (b->layer == best->layer && b->z > best->z) ||
            (b->layer == best->layer && b->z == best->z && b->id > best->id))
            best = b;
    }
    return best;
}

static void anchor_rect(block *b, int *ax, int *ay, int *aw, int *ah) {
    block *a = btable_get(b->anchor_id);
    if (a && a->visible) {
        *ax = a->x; *ay = a->y; *aw = a->w; *ah = a->h;
    } else if (b->parent) {
        *ax = b->parent->x; *ay = b->parent->y;
        *aw = b->parent->w; *ah = b->parent->h;
    } else {
        *ax = 0; *ay = 0; *aw = GFX24_W; *ah = GFX24_H;
    }
}

void pop_layout(block *b) {
    if (!b) return;

    int ax, ay, aw, ah;
    anchor_rect(b, &ax, &ay, &aw, &ah);
    b->pop_ax = ax; b->pop_ay = ay; b->pop_aw = aw; b->pop_ah = ah;

    /* 第一趟：子内容自然宽高。宽取子块最大宽；高按纵列堆叠求和
     * （含子块间距）。子块自然尺寸跨趟稳定；不按绝对坐标相对父框
     * 求偏移——父框落位后子块坐标尚未回流，相对偏移会变负。 */
    int cmaxw = 0, sum_h = 0, n_child = 0;
    for (block *c = b->first_child; c; c = c->next_sibling) {
        n_child++;
        if (c->w > cmaxw) cmaxw = c->w;
        sum_h += c->h;
    }
    if (n_child > 1) sum_h += b->gap * (n_child - 1);
    int content_w = b->pad_l + b->pad_r + cmaxw;
    int content_h = b->pad_t + b->pad_b + sum_h;
    b->pop_content_h = content_h;

    /* 宽高：宽取声明或自然宽；高 min(content, max_h)，max_h==0 不限。 */
    int cw = b->req_w > 0 ? b->req_w : content_w;
    if (cw < 24) cw = 24;
    int ch = content_h;
    if (b->pop_max_h > 0 && ch > (int)b->pop_max_h) ch = (int)b->pop_max_h;
    if (ch > GFX24_H - 2 * POP_SAFE) ch = GFX24_H - 2 * POP_SAFE;

    /* 候选方向序：prefer 置位者优先，补齐默认 down→up→right→left 去重。 */
    uint8_t order[4];
    int n = 0;
    if (b->pop_prefer & PD_DOWN) order[n++] = PD_DOWN;
    if (b->pop_prefer & PD_UP)   order[n++] = PD_UP;
    if (b->pop_prefer & PD_RIGHT) order[n++] = PD_RIGHT;
    if (b->pop_prefer & PD_LEFT)  order[n++] = PD_LEFT;
    static const uint8_t def[4] = { PD_DOWN, PD_UP, PD_RIGHT, PD_LEFT };
    for (int d = 0; d < 4; d++) {
        int dup = 0;
        for (int i = 0; i < n; i++) if (order[i] == def[d]) dup = 1;
        if (!dup) order[n++] = def[d];
    }

    int cx = ax, cy = ay;
    int placed = 0;
    for (int k = 0; k < 4 && !placed; k++) {
        int x, y;
        switch (order[k]) {
        case PD_UP:    x = ax + (aw - cw) / 2; y = ay - ch - POP_GAP; break;
        case PD_RIGHT: x = ax + aw + POP_GAP;  y = ay + (ah - ch) / 2; break;
        case PD_LEFT:  x = ax - cw - POP_GAP;  y = ay + (ah - ch) / 2; break;
        default:       x = ax + (aw - cw) / 2; y = ay + ah + POP_GAP; break;
        }
        if (x >= POP_SAFE && y >= POP_SAFE &&
            x + cw <= GFX24_W - POP_SAFE && y + ch <= GFX24_H - POP_SAFE) {
            cx = x; cy = y; placed = 1;
        }
    }
    /* 首选 / 翻转都不足：钳制回屏（允许压住锚点），四边不出安全边距。 */
    if (!placed) {
        cx = ax + (aw - cw) / 2;
        cy = ay + ah + POP_GAP;
    }
    cx = clampv(cx, POP_SAFE, GFX24_W - POP_SAFE - cw);
    cy = clampv(cy, POP_SAFE, GFX24_H - POP_SAFE - ch);

    b->x = cx; b->y = cy; b->w = cw; b->h = ch;

    /* 落位后把非拖拽子块吸回内容框，按纵列堆叠；跨趟保持一致。 */
    {
        int ly = b->y + b->pad_t;
        for (block *c = b->first_child; c; c = c->next_sibling) {
            if (!c->moved) { c->x = b->x + b->pad_l; c->y = ly; }
            ly += c->h + b->gap;
        }
    }

    /* 超高：内部纵向滚动，滚动条走 sb_*。 */
    if (content_h > ch) {
        b->overflow = OVF_SCROLL;
        b->scroll_axis = 2;
        int maxs = content_h - ch;
        if (b->scroll_y > maxs) b->scroll_y = maxs;
        if (b->scroll_y < 0)    b->scroll_y = 0;
    }

    /* 层级：未声明层的浮层置 OVERLAY 之上；不高于 TOAST。 */
    if (b->layer == 0) b->layer = LAYER_OVERLAY + 1;
}

static void dismiss_pop(block *b) {
    if (!b) return;
    decl_post_dismiss(b);
    pop_dismiss_count++;
    pop_dismiss_id = b->id;
    b_remove(b->id);
    gui_mark_dirty(gui_root_block()->id);
}

int pop_dismiss_at(int x, int y) {
    block *top = top_pop();
    if (!top) return 0;
    if (inside(top, x, y)) return 0;
    /* 斜向 hover 桥窗口内：不外点关闭。 */
    if (prev_in_pop && (int32_t)timer_ticks < (int32_t)bridge_until) return 0;
    dismiss_pop(top);
    return 1;
}

int pop_esc(void) {
    block *top = top_pop();
    if (!top) return 0;
    dismiss_pop(top);
    return 1;
}

/* 指针轨迹：刚离开浮层起开 400ms 桥；进入浮层立即清桥。 */
void pop_bridge_note(int x, int y) {
    block *top = top_pop();
    int in = top && inside(top, x, y);
    if (!in && prev_in_pop) bridge_until = timer_ticks + BRIDGE_TICKS;
    if (in) bridge_until = 0;
    prev_in_pop = in;
}

/* ---- 探针辅助（cap2p_interact.c 本地 extern 调用） ---- */

uint32_t pop_probe_dismiss_count(void) { return pop_dismiss_count; }
block_id pop_probe_dismiss_id(void)    { return pop_dismiss_id; }
void     pop_probe_bridge_expire(void) { bridge_until = 0; prev_in_pop = 0; }
void     pop_probe_bridge_arm(void) { prev_in_pop = 1; bridge_until = timer_ticks + BRIDGE_TICKS; }
void     pop_probe_reset(void) {
    pop_dismiss_count = 0;
    pop_dismiss_id = 0;
    bridge_until = 0;
    prev_in_pop = 0;
}
