/*
 * input.c -- 命中、焦点、拖拽 / resize（规格 §8）。
 *
 * 命中顺序：layer 降序 → z 降序 → 最深层；inert 跳过。
 * 拖拽期间被捕获子树的内容块不参与命中，其他子树不重排。
 * 焦点：focused_block 单槽 + focus_stack[4]。
 */
#include "gui/gui.h"
#include "gui_int.h"
#include "mouse.h"
#include "keyboard.h"

#define DRAG_NONE   0
#define DRAG_MOVE   1
#define DRAG_RESIZE 2
#define HANDLE      12
#define TITLE_H     24

static int       exit_requested;
static block    *focused;
static block    *focus_stack[4];
static int       focus_n;
static block    *drag_block;
static int       drag_mode;
static int       drag_off_x, drag_off_y;

static int inside(block *b, int x, int y) {
    return x >= b->x && x < b->x + b->w &&
           y >= b->y && y < b->y + b->h;
}

#define HIT_MAX 256
static block *hit_list[HIT_MAX];
static int    hit_depth[HIT_MAX];
static int    hit_n;

static void hit_collect(block *b, int depth) {
    if (!b) return;
    if (b->visible && !b->inert && hit_n < HIT_MAX) {
        hit_list[hit_n] = b;
        hit_depth[hit_n] = depth;
        hit_n++;
    }
    for (block *c = b->first_child; c; c = c->next_sibling)
        hit_collect(c, depth + 1);
}

static block *hit_test(int x, int y) {
    hit_n = 0;
    hit_collect(gui_root_block(), 0);
    /* layer 降序 → z 降序 → 深度降序；选择排序，首个命中即返回。 */
    for (int i = 0; i < hit_n; i++) {
        int best = i;
        for (int j = i + 1; j < hit_n; j++) {
            block *a = hit_list[best], *b = hit_list[j];
            if (b->layer > a->layer ||
                (b->layer == a->layer && b->z > a->z) ||
                (b->layer == a->layer && b->z == a->z &&
                 hit_depth[j] > hit_depth[best]))
                best = j;
        }
        if (best != i) {
            block *tb = hit_list[i]; hit_list[i] = hit_list[best];
            hit_list[best] = tb;
            int td = hit_depth[i]; hit_depth[i] = hit_depth[best];
            hit_depth[best] = td;
        }
        if (inside(hit_list[i], x, y)) return hit_list[i];
    }
    return 0;
}

static void focus_set(block *b) {
    if (focused) focused->focused = 0;
    focused = b;
    if (b) {
        b->focused = 1;
        if (focus_n < 4) focus_stack[focus_n++] = b;
        else {
            for (int i = 1; i < 4; i++) focus_stack[i - 1] = focus_stack[i];
            focus_stack[3] = b;
        }
    }
}

static void on_press(int x, int y) {
    block *b = hit_test(x, y);
    if (!b) return;
    if (b->focusable) focus_set(b);

    int rx = b->x + b->w, ry = b->y + b->h;
    if (x >= rx - HANDLE && y >= ry - HANDLE) {
        drag_block = b; drag_mode = DRAG_RESIZE;
        drag_off_x = x - b->x; drag_off_y = y - b->y;
    } else if (y < b->y + TITLE_H) {
        drag_block = b; drag_mode = DRAG_MOVE;
        drag_off_x = x - b->x; drag_off_y = y - b->y;
    }
    if (b->on_click) b->on_click(b->id, x - b->x, y - b->y);
}

static void on_drag_move(int x, int y) {
    if (!drag_block) return;
    block *b = drag_block;
    if (drag_mode == DRAG_MOVE) {
        b->x = x - drag_off_x;
        b->y = y - drag_off_y;
        if (b->on_drag) b->on_drag(b->id, x - b->x, y - b->y, 1);
    } else if (drag_mode == DRAG_RESIZE) {
        int nw = x - b->x, nh = y - b->y;
        if (nw < 24) nw = 24;
        if (nh < 24) nh = 24;
        b->req_w = nw; b->req_h = nh;
        if (b->on_drag) b->on_drag(b->id, nw, nh, 1);
    }
    b->dirty_layout = 1;
    b->dirty_paint = 1;
    b->moved = 1;
}

static void on_release(void) {
    if (drag_block && drag_block->on_drag)
        drag_block->on_drag(drag_block->id, 0, 0, 2); /* phase 2 = end */
    drag_block = 0;
    drag_mode = DRAG_NONE;
}

void gui_pump_input(void) {
    mouse_event_t e;
    while (mouse_poll_event(&e)) {
        if (e.down && (e.changed & MOUSE_BTN_LEFT)) on_press(e.x, e.y);
        else if (!(e.buttons & MOUSE_BTN_LEFT)) {
            if (drag_mode != DRAG_NONE) on_release();
        } else if (drag_block) {
            on_drag_move(e.x, e.y);
        }
    }
    int k = kb_getkey_timed(1);
    if (k == 27) exit_requested = 1;   /* Esc 退回文本控制台 */
}

int gui_should_exit(void) { return exit_requested; }

/* ---- 探针注入钩子：复用真实命中 / 拖拽路径，不另起一套模拟。 ---- */
void gui_inject_press(int x, int y)  { on_press(x, y); }
void gui_inject_move(int x, int y)   { on_drag_move(x, y); }
void gui_inject_release(void)        { on_release(); }
block_id gui_hit_test(int x, int y) {
    block *b = hit_test(x, y);
    return b ? b->id : 0;
}

/* 供探针 / M3 使用：模态块出现时向下层块设 inert。 */
void gui_modal_inert(block_id overlay_id) {
    block *ov = btable_get(overlay_id);
    if (!ov) return;
    for (uint32_t id = 1; id <= btable_capacity(); id++) {
        block *b = btable_get(id);
        if (b && b->layer < ov->layer) b->inert = 1;
    }
}
