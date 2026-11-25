/*
 * input.c -- 命中、焦点、拖拽 / resize（规格 §8）。
 *
 * 命中顺序：layer 降序 → z 降序 → 最深层；inert 跳过。
 * 拖拽期间被捕获子树的内容块不参与命中，其他子树不重排。
 * 焦点：focused_block 单槽 + focus_stack[4]。
 */
#include "gui/gui.h"
#include "gui_int.h"
#include "gui/decl.h"
#include "mouse.h"
#include "keyboard.h"
#include "gfx24.h"

#define DRAG_NONE   0
#define DRAG_MOVE   1
#define DRAG_RESIZE 2
#define TITLE_H     24
#define HANDLE      12

static int       exit_requested;
static block    *focused;
static block    *focus_stack[4];
static int       focus_n;
static block    *drag_block;
static int       drag_mode;
static uint8_t   drag_resize_dir;
static int       drag_off_x, drag_off_y;
static block    *hover_block;

block *gui_hover_target(void) { return hover_block; }

/* 组件键盘钩子：返回 1 表示消费该键，默认 Esc 退出路径不再触发。 */
static int       (*key_hook)(int code, void *ud);
static void     *key_ud;

void gui_set_key_hook(int (*fn)(int code, void *ud), void *ud) {
    key_hook = fn;
    key_ud = ud;
}

static int inside(block *b, int x, int y) {
    return x >= b->x && x < b->x + b->w &&
           y >= b->y && y < b->y + b->h;
}

/* 命中与视觉同真：祖先为 CLIP/SCROLL 且本块层不高于祖先时，点必须落在
 * 祖先框内；OVF_VISIBLE 放开；层高于父级的浮层逃逸父框。 */
static int hit_allowed(block *b, int x, int y) {
    for (block *p = b->parent; p; p = p->parent) {
        if (b->layer > p->layer) continue;
        if (b->overflow == OVF_VISIBLE || p->overflow == OVF_VISIBLE) continue;
        if (!inside(p, x, y)) return 0;
    }
    return 1;
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

/* 命中顺序：layer 降序 → 深度降序（子块压过祖先）→ z 降序；选择排序，首个命中即返回。 */
static block *hit_test(int x, int y) {
    hit_n = 0;
    hit_collect(gui_root_block(), 0);
    for (int i = 0; i < hit_n; i++) {
        int best = i;
        for (int j = i + 1; j < hit_n; j++) {
            block *a = hit_list[best], *b = hit_list[j];
            if (b->layer > a->layer ||
                (b->layer == a->layer && hit_depth[j] > hit_depth[best]) ||
                (b->layer == a->layer && hit_depth[j] == hit_depth[best] &&
                 b->z > a->z))
                best = j;
        }
        if (best != i) {
            block *tb = hit_list[i]; hit_list[i] = hit_list[best];
            hit_list[best] = tb;
            int td = hit_depth[i]; hit_depth[i] = hit_depth[best];
            hit_depth[best] = td;
        }
        if (inside(hit_list[i], x, y) && hit_allowed(hit_list[i], x, y))
            return hit_list[i];
    }
    return 0;
}

/* ---- v2 声明式手势：resize 边缘带 > 拖拽带 > 子块命中 ------------- */

static uint8_t resize_dir_at(block *b, int x, int y) {
    uint8_t dir = b->resize_dir ? b->resize_dir : (RESIZE_R | RESIZE_B);
    int edge = b->resize_edge ? b->resize_edge : 6;
    uint8_t d = 0;
    if ((dir & RESIZE_L) && x < b->x + edge)            d |= RESIZE_L;
    if ((dir & RESIZE_R) && x >= b->x + b->w - edge)    d |= RESIZE_R;
    if ((dir & RESIZE_T) && y < b->y + edge)            d |= RESIZE_T;
    if ((dir & RESIZE_B) && y >= b->y + b->h - edge)    d |= RESIZE_B;
    return d;
}

static int in_resize_band(block *b, int x, int y) {
    return resize_dir_at(b, x, y) != 0;
}

static int drag_band_hit(block *cur, int y) {
    /* drag_mode=0 回落标题带拖拽（v1 兼容默认；v2 可显式声明整块/不可拖）。 */
    uint8_t dm = cur->drag_mode ? cur->drag_mode : DRAG_TITLE;
    if (dm == DRAG_FULL) return 1;
    if (dm == DRAG_TITLE && y < cur->y + TITLE_H) return 1;
    return 0;
}

/* 命中优先级：先上溯到最近的可聚焦面板（叶子文本/行不单独拖），
 * 再判 resize 边缘带 > 标题/整块拖拽带。返回作用块；*mode 为
 * DRAG_NONE/MOVE/RESIZE，*rdir 填缩放方向位。 */
static block *resolve_interaction(block *b, int x, int y,
                                  int *mode, uint8_t *rdir) {
    *mode = DRAG_NONE; *rdir = 0;

    /* 交互目标：沿父链上溯到最近的可聚焦面板；点标题文字时拖的是面板本体，
     * 而不是那个字块。根的直接子块即停（根不可拖）。自身声明了缩放方向或
     * 显式拖拽模式的块本身就是交互目标，不继续上溯。 */
    block *target = b;
    while (target->parent && target->parent->kind != BLK_ROOT &&
           !target->focusable && !target->resize_dir &&
           target->drag_mode == 0)
        target = target->parent;

    /* 1) resize 边缘带（优先于拖拽带）。 */
    if (in_resize_band(target, x, y)) {
        *mode = DRAG_RESIZE;
        *rdir = resize_dir_at(target, x, y);
        return target;
    }
    /* 2) 拖拽带：整块 DRAG_FULL，或标题带 TITLE_H。 */
    if (drag_band_hit(target, y)) {
        *mode = DRAG_MOVE;
        return target;
    }
    return target;
}

static int clamp_step(int v, int step) {
    if (step <= 0) return v;
    int r = v % step;
    return r >= step / 2 ? v - r + step : v - r;
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
    /* 菜单模态：点下层空白只关闭菜单，不下发命中。 */
    if (gui_menu_is_open() && b->layer < LAYER_OVERLAY) {
        gui_menu_close();
        return;
    }

    /* 点击冒泡：命中的通常是最里层的标签/文本块，而 on_click 声明在
     * 行/面板上。向上找最近一个带 on_click 的祖先（含自身），否则
     * Start、菜单条目这类"行 + 文本子块"永远点不动。 */
    block *act = b;
    while (act && !act->on_click) act = act->parent;

    if (act && act->focusable) focus_set(act);
    else if (b->focusable)    focus_set(b);

    /* 输入框聚焦：接管键盘；点到别处则释放输入框钩子。 */
    if (act && act->kind == BLK_INPUT) gui_set_key_hook(ib_key, (void *)act->id);
    else if (ib_is_active())           gui_set_key_hook(0, 0);

    /* 拖拽/缩放：解析到可聚焦面板（resize 边缘带 > 标题/整块拖拽带）。 */
    int mode; uint8_t rdir;
    block *drag_target = resolve_interaction(b, x, y, &mode, &rdir);
    if (mode == DRAG_RESIZE) {
        drag_block = drag_target; drag_mode = DRAG_RESIZE; drag_resize_dir = rdir;
        drag_off_x = x - drag_target->x; drag_off_y = y - drag_target->y;
    } else if (mode == DRAG_MOVE) {
        drag_block = drag_target; drag_mode = DRAG_MOVE;
        drag_off_x = x - drag_target->x; drag_off_y = y - drag_target->y;
    }
    if (act && act->on_click) act->on_click(act->id, x - act->x, y - act->y);
    /* 声明式应用块：命中事件回投到所属应用队列（不带屏幕绝对坐标）。 */
    if (b->owner) decl_post_click(b, x - b->x, y - b->y);
}

static void on_drag_move(int x, int y) {
    if (!drag_block) return;
    block *b = drag_block;
    if (drag_mode == DRAG_MOVE) {
        /* 脏矩形：改几何前缓存旧矩形，供合成层算「旧 ∪ 新」损伤条带。 */
        b->old_x = b->x; b->old_y = b->y;
        b->old_w = b->w; b->old_h = b->h; b->has_old = 1;
        int nx = x - drag_off_x, ny = y - drag_off_y;
        /* drag_bounds：0 不限 / 1 父框 / 2 屏幕。 */
        if (b->drag_bounds == BOUND_SCREEN) {
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            if (nx + b->w > GFX24_W) nx = GFX24_W - b->w;
            if (ny + b->h > GFX24_H) ny = GFX24_H - b->h;
        } else if (b->drag_bounds == BOUND_PARENT && b->parent) {
            block *p = b->parent;
            int px = p->x + p->pad_l, py = p->y + p->pad_t;
            int pw = p->w - p->pad_l - p->pad_r;
            int ph = p->h - p->pad_t - p->pad_b;
            if (nx < px) nx = px;
            if (ny < py) ny = py;
            if (nx + b->w > px + pw) nx = px + pw - b->w;
            if (ny + b->h > py + ph) ny = py + ph - b->h;
        }
        /* 只改声明几何 / 偏移属性，不直接写像素。 */
        b->x = nx; b->y = ny;
        b->pos_x = nx; b->pos_y = ny;
        b->moved = 1;
        if (b->on_drag) b->on_drag(b->id, 0, 0, 1);
        /* VISIBLE 子块移动：父块脏区并入，旧位置由父底色补齐防残影。 */
        if (b->overflow == OVF_VISIBLE && b->parent)
            b->parent->dirty_paint = 1;
    } else if (drag_mode == DRAG_RESIZE) {
        b->old_x = b->x; b->old_y = b->y;
        b->old_w = b->w; b->old_h = b->h; b->has_old = 1;
        uint8_t d = drag_resize_dir;
        int nl = b->x, nt = b->y, nr = b->x + b->w, nb = b->y + b->h;
        if (d & RESIZE_L) nl = x;
        if (d & RESIZE_R) nr = x;
        if (d & RESIZE_T) nt = y;
        if (d & RESIZE_B) nb = y;
        int nw = nr - nl, nh = nb - nt;
        int minw = b->resize_min_w ? b->resize_min_w : 24;
        int minh = b->resize_min_h ? b->resize_min_h : 24;
        if (nw < minw) nw = minw;
        if (nh < minh) nh = minh;
        if (b->resize_max_w && nw > b->resize_max_w) nw = b->resize_max_w;
        if (b->resize_max_h && nh > b->resize_max_h) nh = b->resize_max_h;
        nw = clamp_step(nw, b->resize_step);
        nh = clamp_step(nh, b->resize_step);
        if (d & RESIZE_L) nl = nr - nw;
        if (d & RESIZE_T) nt = nb - nh;
        b->x = nl; b->y = nt; b->w = nw; b->h = nh;
        b->req_w = nw; b->req_h = nh;
        b->moved = 1;
        if (b->on_drag) b->on_drag(b->id, nw, nh, 1);
    }
    b->dirty_layout = 1;
    b->dirty_paint = 1;
    b->moved = 1;
    /* 整树标脏（当前定论）：旧位置的背景补齐依赖同帧重绘的背景/兄弟节点，
     * 条带级背景补齐（只在损伤条带内擦旧位）尚未实现，收窄脏位会在拖出
     * 轨迹上留残影。实测拖动帧时/损伤无全屏回退、无残影，权衡与口径见
     * evidence-fix/09-perf/dirty-rect-summary.md；条带化补齐留后续轮次。 */
    gui_mark_dirty(gui_root_block()->id);
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
            else hover_block = hit_test(e.x, e.y);
        } else if (drag_block) {
            on_drag_move(e.x, e.y);
        }
    }
    int k = kb_getkey_timed(1);
    if (k >= 0 && key_hook) {
        if (key_hook(k, key_ud)) k = -1;          /* 组件消费，不走默认路径 */
    }
    if (k == 27) {
        if (gui_sak_esc()) return;             /* SAK 面板优先消费 Esc */
        if (gui_menu_is_open()) gui_menu_close();  /* Esc 先关菜单 */
        else exit_requested = 1;                   /* Esc 退回文本控制台 */
    }
}

void gui_focus_raise(block_id id) {
    block *b = btable_get(id);
    if (!b) return;
    focus_set(b);
    /* 摘挂到父链末尾 + 同层 z 递增 + 整树标脏（摘挂后父指针已清空，必须先
     * 缓存父节点再重挂；z 同步重算，避免点一次"跳层"或摘树后消失）。 */
    gui_raise_to_front(id);
}

int gui_should_exit(void) { return exit_requested; }

/* v2.80 hotfix: 每次进桌面必须把输入态归零。
 * `exit_requested` / 拖拽捕获 / 焦点栈都是 static，Esc 退出后不清会把
 * "上一轮的退出请求"带进下一轮，第二次 `gui` 一进来就立刻退出（看起来像闪退）。 */
void gui_input_reset(void) {
    exit_requested = 0;
    drag_block = 0;
    drag_mode = DRAG_NONE;
    drag_off_x = drag_off_y = 0;
    focused = 0;
    focus_n = 0;
    for (int i = 0; i < 4; i++) focus_stack[i] = 0;
    hover_block = 0;
    key_hook = 0;
    key_ud = 0;
}

/* ---- 探针注入钩子：复用真实命中 / 拖拽路径，不另起一套模拟。 ---- */
void gui_inject_press(int x, int y)  { on_press(x, y); }
void gui_inject_move(int x, int y)   { on_drag_move(x, y); }
void gui_inject_release(void)        { on_release(); }
block_id gui_hit_test(int x, int y) {
    block *b = hit_test(x, y);
    return b ? b->id : 0;
}

/* 探针复用：解析某点的手势归属（resize 边缘带 > 拖拽带 > 子块命中）。
 * 返回内部模式 DRAG_NONE/MOVE/RESIZE；*out 填作用块 id，*rdir 填缩放方向。 */
int gui_interaction_at(int x, int y, block_id *out, uint8_t *rdir) {
    block *b = hit_test(x, y);
    if (!b) { if (out) *out = 0; if (rdir) *rdir = 0; return DRAG_NONE; }
    int mode; uint8_t d;
    block *act = resolve_interaction(b, x, y, &mode, &d);
    if (out) *out = act ? act->id : 0;
    if (rdir) *rdir = d;
    return mode;
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
