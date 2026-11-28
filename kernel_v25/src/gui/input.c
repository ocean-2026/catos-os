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
#include "intr.h"

extern void ser_write(const char *s);
extern void ser_putc(char c);

static void ser_dec_u(uint32_t v) {
    char buf[12]; int n = 0;
    if (v == 0) { ser_write("0"); return; }
    while (v) { buf[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) ser_putc(buf[--n]);
}

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
static block    *pressed_block;     /* E 轮新增：左键按住期间命中的按钮（三态按下反馈） */

/* v2.83 A7.2 松开命中触发：按下只记账，松开时重算命中同块才回调。仅
 * click_on_up=1 的按钮/菜单行/开始菜单行进入此态；进 drag（位移超阈值或块
 * 进 DRAG_*）立即清 armed。 */
static block    *armed_block;
static int       armed_x0, armed_y0;

/* R5 双击判定：同一块在 DBLCLICK_MS 窗口内二次按下算双击；本次手势发生过
 * 超阈值拖拽则不参与配对。时间源 uptime_ms()（不走 rtc）。 */
#define DBLCLICK_MS       450
#define DBL_DRAG_THRESH   4
static block    *last_press_block;
static uint32_t  last_press_ms;
static int       last_press_was_drag;
static block    *cur_press_block;
static int       press_x0, press_y0;
static int       press_dragged;

/* 运行时调试开关（默认关，release 惰性）：故障注入时对硬禁拖块临时放行，
 * 仅用于核验“被拖块出现在本不该出现的位置时，兜底重合成仍不烙残影”。
 * 唯一绕过点：drag_denied / drag_band_hit；不写任何像素。 */
int g_dbg_dragfull = 0;

void gui_dbg_set_dragfull(int on) {
    g_dbg_dragfull = on ? 1 : 0;
    ser_write("GUI:warn fault=dragfull ");
    ser_write(g_dbg_dragfull ? "on\n" : "off\n");
}

block *gui_hover_target(void) { return hover_block; }
block *gui_press_target(void) { return pressed_block; }

/* 组件键盘钩子：返回 1 表示消费该键，默认 Esc 退出路径不再触发。 */
static int       (*key_hook)(int code, void *ud);
static void     *key_ud;

/* popover.c 内部入口（gui_int.h 冻结清单之外）。 */
extern void pop_bridge_note(int x, int y);

/* sb.c 拖动态访问器（gui_int.h 冻结清单之外的本地接线）。 */
extern block *sb_drag_target(void);
extern void   sb_state_reset(void);

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
    /* 故障注入开关：临时把禁拖块视为整块可拖（仅此一处）。 */
    if (g_dbg_dragfull) return 1;
    /* 默认 DRAG_NONE 不可拖；仅显式声明 TITLE/FULL 的块参与拖拽带判定，
     * 不再把 0 回落成标题带可拖。 */
    if (cur->drag_mode == DRAG_FULL) return 1;
    if (cur->drag_mode == DRAG_TITLE && y < cur->y + TITLE_H) return 1;
    return 0;
}

/* 硬禁拖：背景层 / 内容层 / 根块 / 全出血块一律不可拖，即使显式声明
 * DRAG_FULL 也忽略。这类块是桌面底、容器或任务栏，拖拽只会误移动底色。 */
static int drag_denied(block *b) {
    if (!b) return 1;
    /* 故障注入开关：临时放行硬禁拖块（仅此一处绕过，默认关）。 */
    if (g_dbg_dragfull) return 0;
    if (b->layer == LAYER_BG || b->layer == LAYER_CONTENT) return 1;
    if (b->kind == BLK_ROOT) return 1;
    if (b->bleed) return 1;
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
           target->drag_mode == DRAG_NONE)
        target = target->parent;

    /* 硬禁拖：背景层 / 内容层 / 根块 / 全出血块一律不可拖，即使声明
     * DRAG_FULL 也忽略；记串口告警供探针核验。 */
    if (drag_denied(target)) {
        ser_write("GUI:warn dragdeny id="); ser_dec_u(target->id);
        ser_write(" layer="); ser_dec_u(target->layer); ser_write("\n");
        *mode = DRAG_NONE;
        return target;
    }

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

/* 沿父链找最近的可滚动块（OVF_SCROLL 且滚动条可见）；没有则返回 0。 */
static block *scroll_ancestor(block *b) {
    for (; b; b = b->parent)
        if (b->overflow == OVF_SCROLL && sb_visible(b)) return b;
    return 0;
}

/* 滚轮分发：命中可滚动块则内核滚动一格（3 行），并把 ev_kind=4 回投该块
 * 所属应用；命中块整条父链都没有滚动条时，回投最近带 owner 的祖先，免得
 * 应用收不到滚轮。 */
static void on_wheel(int x, int y, int dz) {
    block *b = hit_test(x, y);
    if (!b) return;
    block *sc = scroll_ancestor(b);
    if (sc) {
        sb_wheel(sc, dz);
        decl_post_wheel(sc, 0, dz * 3);
        sc->dirty_layout = 1; sc->dirty_paint = 1;
        gui_mark_dirty(sc->id);
        return;
    }
    block *own = b;
    while (own && !own->owner) own = own->parent;
    if (own) decl_post_wheel(own, 0, dz * 3);
}

static void on_press(int x, int y) {
    /* 点在最上层浮层矩形外：先关闭浮层并回投 dismiss，本帧不再向下下发点击。 */
    if (pop_dismiss_at(x, y)) return;
    block *b = hit_test(x, y);
    if (!b) return;

    /* 滚动条 thumb 按下：进入拖动态，抢占窗口拖拽 / 点击回投（点在 track
     * 空白处不算抓到，落回下面的常规路径）。 */
    block *sc = scroll_ancestor(b);
    if (sc) sb_mouse(sc, x, y, 0);
    if (sc && sb_drag_target()) {
        sc->dirty_layout = 1; sc->dirty_paint = 1;
        return;
    }

    /* 菜单模态：点菜单外部（含下层全部被 inert 后命中为空）只关闭菜单，不下发命中。 */
    if (gui_menu_is_open() && (!b || b->layer < LAYER_OVERLAY)) {
        gui_menu_close();
        return;
    }
    if (!b) return;

    /* E 轮三态：左键按住期间记录命中块（优先上溯到按钮），供按下反馈；松开清空。 */
    { block *p = b; while (p && p->kind != BLK_BUTTON) p = p->parent;
      pressed_block = p ? p : b; }

    /* 点击冒泡：命中的通常是最里层的标签/文本块，而 on_click 声明在
     * 行/面板上。向上找最近一个带 on_click 的祖先（含自身），否则
     * Start、菜单条目这类"行 + 文本子块"永远点不动。 */
    block *act = b;
    while (act && !act->on_click) act = act->parent;
    block *act_dbl = b;
    while (act_dbl && !act_dbl->on_dblclick) act_dbl = act_dbl->parent;

    /* R5 双击配对：同一个带 on_dblclick 的祖先块、上一次手势未拖出阈值、
     * 间隔在窗口内才算双击。配对锚点取解析后的祖先（两次命中叶子可能
     * 分别是图标块或标签块）。 */
    block *dbl_anchor = act_dbl ? act_dbl : act;
    uint32_t now_ms = uptime_ms();
    int is_dbl = (last_press_block == dbl_anchor && !last_press_was_drag &&
                  now_ms - last_press_ms <= DBLCLICK_MS);
    cur_press_block = dbl_anchor; press_x0 = x; press_y0 = y; press_dragged = 0;

    if (act && act->focusable) focus_set(act);
    else if (b->focusable)    focus_set(b);

    /* 输入框聚焦：接管键盘；点到别处则释放输入框钩子。
     * act 上溯到无 on_click 的祖先可能为 null，故按命中块 b 回退判 BLK_INPUT。 */
    block *ib_target = (act && act->kind == BLK_INPUT) ? act
                     : (b->kind == BLK_INPUT) ? b : 0;
    if (ib_target) {
        gui_set_key_hook(ib_key, (void *)ib_target->id);
        ib_activate(ib_target->id);
    }
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
    /* 手势进入拖拽/缩放：不再是一次点击，清掉 armed 按钮。 */
    if (mode != DRAG_NONE) armed_block = 0;

    /* A7.2：click_on_up 按钮按下只记账，松开重算命中同块才回调；其余块维持
     * 旧的按下即触发。双击锚点（桌面图标等非 armed 块）仍走按下即派发。 */
    int armed = act && act->click_on_up;
    if (armed) { armed_block = act; armed_x0 = x; armed_y0 = y; }
    if (is_dbl && act_dbl && act_dbl->on_dblclick)
        act_dbl->on_dblclick(act_dbl->id, x - act_dbl->x, y - act_dbl->y);
    else if (!armed && act && act->on_click)
        act->on_click(act->id, x - act->x, y - act->y);
    /* 声明式应用块：命中事件回投到所属应用队列；fit 缩放块按逆映射换算到源坐标。 */
    if (b->owner) {
        int isx = x - b->x, isy = y - b->y;
        gui_fit_inv_map(b, isx, isy, &isx, &isy);
        decl_post_click(b, isx, isy);
    }
}

static void on_drag_move(int x, int y) {
    block *sb = sb_drag_target();
    if (sb) {
        sb_mouse(sb, x, y, 1);
        sb->dirty_layout = 1; sb->dirty_paint = 1; sb->moved = 1;
        /* 只标脏滚动容器子树（含 thumb/track），旧 thumb 位由损伤矩形底色补齐。 */
        gui_mark_dirty(sb->id);
        return;
    }
    if (!drag_block) return;
    block *b = drag_block;
    if (drag_mode == DRAG_MOVE) {
        /* 位移超阈值即判定本次手势为拖拽，松开后不参与双击配对；同时清 armed。 */
        if (cur_press_block &&
            (x - press_x0 > DBL_DRAG_THRESH || press_x0 - x > DBL_DRAG_THRESH ||
             y - press_y0 > DBL_DRAG_THRESH || press_y0 - y > DBL_DRAG_THRESH)) {
            press_dragged = 1;
            armed_block = 0;
        }
        /* 脏矩形：改几何前缓存旧矩形，供合成层算「旧 ∪ 新」损伤条带。 */
        b->old_x = b->x; b->old_y = b->y;
        b->old_w = b->w; b->old_h = b->h; b->has_old = 1;
        int nx = x - drag_off_x, ny = y - drag_off_y;
        /* drag_bounds：0 不限 / 1 父框 / 2 屏幕 / 3 宽松出屏。 */
        if (b->drag_bounds == BOUND_SCREEN) {
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            if (nx + b->w > GFX24_W) nx = GFX24_W - b->w;
            if (ny + b->h > GFX24_H) ny = GFX24_H - b->h;
        } else if (b->drag_bounds == BOUND_LOOSE) {
            /* 宽松：顶部标题条（高 24，块更矮时取块高）必须在屏内；水平至少留
             * 6px 可见；块的其余部分允许出屏。与 BOUND_SCREEN 的整窗夹紧互不影响。 */
            int th = b->h < 24 ? b->h : 24;
            if (nx < -(b->w - 6)) nx = -(b->w - 6);
            if (nx > GFX24_W - 6) nx = GFX24_W - 6;
            if (ny < 0) ny = 0;
            if (ny + th > GFX24_H) ny = GFX24_H - th;
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
    /* 损伤 = 被拖块子树新包围盒 ∪ 旧包围盒（compose 第一遍按 has_old 回推旧位，
     * dmg_add_pad 已含阴影外扩）；损伤矩形内先以底色填实再重绘相交节点，旧位置
     * 由背景/兄弟在同帧补齐，不再整树标脏。OVF_VISIBLE 逃逸子块的父块脏位已在
     * 上方并入。旧位残影以 gui_pixel_assert drag 组 old-rect 断言复核。 */
    gui_mark_dirty(b->id);
}

static void on_release(int x, int y) {
    /* A7.2 松开命中触发：armed 按钮在松开点重算命中，仍落在同一回调块上才触发；
     * 已进拖拽 / 位移超阈值 / 移出块外都取消，不回调。 */
    if (armed_block) {
        block *ab = armed_block;
        armed_block = 0;
        if (!drag_block && !press_dragged) {
            block *rb = hit_test(x, y);
            block *ract = rb;
            while (ract && !ract->on_click) ract = ract->parent;
            if (ract == ab && ab->on_click)
                ab->on_click(ab->id, x - ab->x, y - ab->y);
        }
    }
    pressed_block = 0;
    if (drag_block && drag_block->on_drag)
        drag_block->on_drag(drag_block->id, 0, 0, 2); /* phase 2 = end */
    /* 记录本次按下供下一次双击配对；发生过拖拽的手势不参与配对。 */
    last_press_block = cur_press_block;
    last_press_ms = uptime_ms();
    last_press_was_drag = press_dragged;
    cur_press_block = 0;
    press_dragged = 0;
    drag_block = 0;
    drag_mode = DRAG_NONE;
    block *sb = sb_drag_target();
    if (sb) sb_mouse(sb, 0, 0, 2);
}

void gui_pump_input(void) {
    mouse_event_t e;
    while (mouse_poll_event(&e)) {
        if (e.dz) {
            on_wheel(e.x, e.y, e.dz);
            continue;
        }
        if (e.down && (e.changed & MOUSE_BTN_LEFT)) on_press(e.x, e.y);
        else if (!(e.buttons & MOUSE_BTN_LEFT)) {
            pressed_block = 0;
            if (drag_mode != DRAG_NONE || sb_drag_target()) on_release(e.x, e.y);
            else if (armed_block) on_release(e.x, e.y);  /* armed 按钮松开重算命中 */
            else hover_block = hit_test(e.x, e.y);
            pop_bridge_note(e.x, e.y);
        } else if (drag_block) {
            on_drag_move(e.x, e.y);
        } else if (sb_drag_target()) {
            on_drag_move(e.x, e.y);
        }
    }
    int k = kb_getkey_timed(1);
    if (k >= 0) {
        int alt_held = kb_alt();
        int alpha = (k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z');
        if (alt_held && alpha) {
            /* Alt+字母：菜单加速键（开菜单 / 直达匹配），一律吞键，不落输入框。 */
            gui_menu_accel(k);
            k = -1;
        } else if (key_hook) {
            if (key_hook(k, key_ud)) k = -1;          /* 组件消费，不走默认路径 */
        } else if (pop_key(k)) {
            k = -1;                                  /* 浮层导航消费 */
        }
    }
    if (k == 27) {
        if (gui_sak_esc()) return;             /* SAK 面板优先消费 Esc */
        if (pop_esc()) return;                  /* Esc 先关最上层浮层 */
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
    pressed_block = 0;
    armed_block = 0;
    armed_x0 = armed_y0 = 0;
    last_press_block = 0;
    last_press_ms = 0;
    last_press_was_drag = 0;
    cur_press_block = 0;
    press_dragged = 0;
    key_hook = 0;
    key_ud = 0;
    sb_state_reset();
}

/* ---- 探针注入钩子：复用真实命中 / 拖拽路径，不另起一套模拟。 ---- */
void gui_inject_press(int x, int y)  { on_press(x, y); }
void gui_inject_move(int x, int y)   { on_drag_move(x, y); }
void gui_inject_release(void)        { on_release(armed_x0, armed_y0); }
void gui_inject_release_at(int x, int y) { on_release(x, y); }
void gui_inject_wheel(int x, int y, int dz) { on_wheel(x, y, dz); }
void gui_inject_hover(int x, int y) { hover_block = hit_test(x, y); }
void gui_inject_press_block(block_id id) { pressed_block = btable_get(id); }
void gui_inject_hover_block(block_id id) { hover_block = btable_get(id); }
void gui_inject_key(int code) { if (key_hook) key_hook(code, key_ud); }
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
