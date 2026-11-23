/*
 * layout.c -- 两阶段布局（规格 §3）。
 *
 * measure 自底向上算 auto 尺寸；layout 自顶向下写绝对 x/y。
 * 只重算带 dirty_layout 标记的子树：父脏不会强制整棵子树重算。
 * 容器：COLUMN（纵主轴）/ ROW（横主轴）/ STACK（层叠）。
 */
#include "gui/gui.h"
#include "gui_int.h"

static void layout_node(block *b, int ax, int ay, int aw, int ah);

static int text_len(const char *s) {
    int n = 0;
    if (!s) return 0;
    while (*s++) n++;
    return n;
}

static int leaf_natural_w(block *b) {
    if (b->kind == BLK_TEXT) {
        uint8_t sc = b->st.scale ? b->st.scale : 1;
        return 8 * (int)sc * text_len(b->text);
    }
    if (b->kind == BLK_IMAGE || b->kind == BLK_SURFACE)
        return b->pix_w;
    if (b->kind == BLK_SPACER) return b->req_w;
    if (b->kind == BLK_DIVIDER) return 0;
    return 0;
}

static int leaf_natural_h(block *b) {
    if (b->kind == BLK_TEXT) {
        uint8_t sc = b->st.scale ? b->st.scale : 1;
        return 16 * (int)sc;
    }
    if (b->kind == BLK_IMAGE || b->kind == BLK_SURFACE)
        return b->pix_h;
    if (b->kind == BLK_SPACER) return b->req_h;
    if (b->kind == BLK_DIVIDER) return 1;
    return 0;
}

static void measure_node(block *b) {
    if (!b) return;
    stat_add_layout_nodes(1);

    /* 根节点帧尺寸由 gui_begin 固定，不参与 auto 聚合。 */
    if (b->kind == BLK_ROOT) return;

    /* 子块尺寸已由 measure_dirty 后序遍历算好，这里只做聚合，不再下行重复计数。 */
    block *c = b->first_child;
    if (b->kind == BLK_COLUMN || b->kind == BLK_ROW || b->kind == BLK_STACK) {
        int max_w = 0, max_h = 0, sum_w = 0, sum_h = 0, n = 0;
        for (c = b->first_child; c; c = c->next_sibling, n++) {
            if (c->w > max_w) max_w = c->w;
            if (c->h > max_h) max_h = c->h;
            sum_w += c->w; sum_h += c->h;
        }
        int gap = n > 1 ? b->gap * (n - 1) : 0;
        int nat_w, nat_h;
        if (b->kind == BLK_COLUMN) {
            nat_w = b->pad_l + b->pad_r + max_w;
            nat_h = b->pad_t + b->pad_b + sum_h + gap;
        } else if (b->kind == BLK_ROW) {
            nat_w = b->pad_l + b->pad_r + sum_w + gap;
            nat_h = b->pad_t + b->pad_b + max_h;
        } else {
            nat_w = b->pad_l + b->pad_r + max_w;
            nat_h = b->pad_t + b->pad_b + max_h;
        }
        b->w = b->req_w > 0 ? b->req_w : nat_w;
        b->h = b->req_h > 0 ? b->req_h : nat_h;
    } else {
        int nw = leaf_natural_w(b), nh = leaf_natural_h(b);
        b->w = b->req_w > 0 ? b->req_w : nw;
        b->h = b->req_h > 0 ? b->req_h : nh;
    }
}

static void place_child(block *child, int ax, int ay, int aw, int ah,
                        int cross_axis) {
    /* cross_axis: 0 = 交叉轴横向（COLUMN），1 = 交叉轴纵向（ROW） */
    int cx = ax, cy = ay;
    if (cross_axis == 0) {
        if (child->align_cross == XALN_STRETCH) child->w = aw;
        else if (child->align_cross == XALN_CENTER) cx = ax + (aw - child->w) / 2;
        else if (child->align_cross == XALN_END)   cx = ax + aw - child->w;
    } else {
        if (child->align_cross == XALN_STRETCH) child->h = ah;
        else if (child->align_cross == XALN_CENTER) cy = ay + (ah - child->h) / 2;
        else if (child->align_cross == XALN_END)   cy = ay + ah - child->h;
    }
    child->x = cx; child->y = cy;
}

static void layout_column(block *b, int ax, int ay, int aw, int ah) {
    int n = 0;
    for (block *c = b->first_child; c; c = c->next_sibling) n++;
    if (n == 0) return;
    int total_h = 0, gap = n > 1 ? b->gap : 0;
    for (block *c = b->first_child; c; c = c->next_sibling) total_h += c->h;
    total_h += gap * (n - 1);

    int y0;
    if (b->align_main == ALN_CENTER)      y0 = ay + (ah - total_h) / 2;
    else if (b->align_main == ALN_END)    y0 = ay + ah - total_h;
    else                                  y0 = ay;

    int extra = 0, slot = 0;
    if (b->align_main == ALN_SPACE_BETWEEN && n > 1) {
        extra = ah - total_h;
        slot = extra / (n - 1);
    }
    int y = y0;
    for (block *c = b->first_child; c; c = c->next_sibling) {
        place_child(c, ax, y, aw, ah, 0);
        layout_node(c, c->x, c->y, c->w, c->h);
        y += c->h + gap + slot;
    }
}

static void layout_row(block *b, int ax, int ay, int aw, int ah) {
    int n = 0;
    for (block *c = b->first_child; c; c = c->next_sibling) n++;
    if (n == 0) return;
    int total_w = 0, gap = n > 1 ? b->gap : 0;
    for (block *c = b->first_child; c; c = c->next_sibling) total_w += c->w;
    total_w += gap * (n - 1);

    int x0;
    if (b->align_main == ALN_CENTER)      x0 = ax + (aw - total_w) / 2;
    else if (b->align_main == ALN_END)    x0 = ax + aw - total_w;
    else                                  x0 = ax;

    int extra = 0, slot = 0;
    if (b->align_main == ALN_SPACE_BETWEEN && n > 1) {
        extra = aw - total_w;
        slot = extra / (n - 1);
    }
    int x = x0;
    for (block *c = b->first_child; c; c = c->next_sibling) {
        place_child(c, x, ay, aw, ah, 1);
        layout_node(c, c->x, c->y, c->w, c->h);
        x += c->w + gap + slot;
    }
}

static void layout_stack(block *b, int ax, int ay, int aw, int ah) {
    /* 主轴 = 横向，交叉轴 = 纵向（与 ROW 一致）。 */
    for (block *c = b->first_child; c; c = c->next_sibling) {
        int cx = ax, cy = ay;
        if (c->align_main == ALN_CENTER)       cx = ax + (aw - c->w) / 2;
        else if (c->align_main == ALN_END)     cx = ax + aw - c->w;
        if (c->align_cross == XALN_STRETCH)    c->h = ah;
        else if (c->align_cross == XALN_CENTER) cy = ay + (ah - c->h) / 2;
        else if (c->align_cross == XALN_END)   cy = ay + ah - c->h;
        if (!c->moved) { c->x = cx; c->y = cy; }
        layout_node(c, c->x, c->y, c->w, c->h);
    }
}

void layout_node(block *b, int ax, int ay, int aw, int ah) {
    if (!b) return;
    int pad_l = b->pad_l, pad_r = b->pad_r, pad_t = b->pad_t, pad_b = b->pad_b;
    int cax = ax + pad_l, cay = ay + pad_t;
    int caw = aw - pad_l - pad_r, cah = ah - pad_t - pad_b;
    if (caw < 0) caw = 0;
    if (cah < 0) cah = 0;

    switch (b->kind) {
    case BLK_COLUMN: layout_column(b, cax, cay, caw, cah); break;
    case BLK_ROW:    layout_row(b,    cax, cay, caw, cah); break;
    case BLK_STACK:
    case BLK_ROOT:  layout_stack(b, cax, cay, caw, cah); break;
    default: break;
    }

    /* 叶子块的子块继承父块矩形（无自身容器布局）。 */
    if (b->kind != BLK_COLUMN && b->kind != BLK_ROW &&
        b->kind != BLK_STACK && b->kind != BLK_ROOT) {
        for (block *c = b->first_child; c; c = c->next_sibling) {
            c->x = b->x; c->y = b->y;
            layout_node(c, c->x, c->y, c->w, c->h);
        }
    }
}

static void measure_dirty(block *b) {
    if (!b) return;
    /* 子树无脏标记则整体跳过，不向上冒泡重算。 */
    int child_dirty = 0;
    for (block *c = b->first_child; c; c = c->next_sibling) {
        measure_dirty(c);
        if (c->dirty_layout) child_dirty = 1;
    }
    if (!b->dirty_layout && !child_dirty) return;
    if (child_dirty) b->dirty_layout = 1;   /* 脏位冒泡到最近布局容器 */
    measure_node(b);
}

static int layout_dirty(block *b) {
    if (!b) return 0;
    int dirty = b->dirty_layout;
    for (block *c = b->first_child; c; c = c->next_sibling)
        if (layout_dirty(c)) dirty = 1;
    if (!dirty) return 0;
    layout_node(b, b->x, b->y, b->w, b->h);
    b->dirty_layout = 0;
    return 1;
}

void layout_run(void) {
    stat_set_layout_nodes(0);
    measure_dirty(gui_root_block());
    layout_dirty(gui_root_block());
}
