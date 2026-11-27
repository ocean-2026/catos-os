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

/* ---- 溢出硬约束（R4）：未声明允许溢出的块，子块不得超出父内容框 ---- */
/* 规则：父 overflow == OVF_CLIP 时，非 moved / 非 bleed / 非浮层（layer 更高）/
 * 非 OVF_VISIBLE 的子块矩形收缩进父内容框；OVF_SCROLL 由滚动视口平移处理，
 * 不做几何裁剪。这是"没有声明允许超出父块 = 绝对不能超出"的统一落点：
 * 几何层收缩 + 绘制层像素裁剪（clip_putpx）双保险，命中也沿用祖先框判定。 */
static void clamp_child(block *p, block *c,
                        int cx, int cy, int cw, int ch) {
    if (!p || !c) return;
    if (p->overflow != OVF_CLIP) return;
    if (c->overflow == OVF_VISIBLE || c->moved || c->bleed) return;
    if (c->layer > p->layer) return;
    if (c->x < cx) { c->w -= (cx - c->x); c->x = cx; }
    if (c->y < cy) { c->h -= (cy - c->y); c->y = cy; }
    if (c->x + c->w > cx + cw) c->w = cx + cw - c->x;
    if (c->y + c->h > cy + ch) c->h = cy + ch - c->y;
    if (c->w < 0) c->w = 0;
    if (c->h < 0) c->h = 0;
}



static int leaf_natural_w(block *b) {
    if (b->kind == BLK_TEXT || b->kind == BLK_BUTTON) {
        uint8_t sc = b->st.scale ? b->st.scale : 1;
        return txt_measure_w(b->text, b->font_size, sc, b->font_tabular);
    }
    if (b->kind == BLK_INPUT) return 80;
    if (b->kind == BLK_IMAGE || b->kind == BLK_SURFACE ||
        b->kind == BLK_CANVAS)
        return b->pix_w;
    if (b->kind == BLK_ICON) return b->req_w;   /* v2.82 R3: 内置图标取显式尺寸 */
    if (b->kind == BLK_SPACER) return b->req_w;
    if (b->kind == BLK_DIVIDER) return 0;
    return 0;
}

/* Calm 8pt 网格：内边距 / 间距归整到 {4,8,12,16,24}；1px 描边不动。 */
static int grid_snap(int v) {
    if (!gui_style_calm() || v <= 0) return v;
    static const int steps[5] = { 4, 8, 12, 16, 24 };
    for (int i = 0; i < 5; i++)
        if (v <= steps[i]) return steps[i];
    return 24;
}

static int leaf_natural_h(block *b) {
    if (b->kind == BLK_TEXT || b->kind == BLK_BUTTON) {
        uint8_t sc = b->st.scale ? b->st.scale : 1;
        int h;
        if (b->wrap_lines == 0) {
            /* 旧口径：单行块高 = 字高（wrap=0/16 像素回归硬门禁）。 */
            h = txt_font_h(b->font_size, sc);
        } else {
            int maxw = b->req_w > 0 ? b->req_w : 1000000;
            h = txt_wrapped_h(b->text, b->font_size, sc, b->font_tabular,
                              maxw, b->wrap_lines, b->ellipsis, b->line_h);
        }
        if (gui_style_calm() && b->kind != BLK_TEXT && h < 24) {
            /* 控件高 24（16 字形 + 上下各 4） */
            h = 24;
        }
        return h;
    }
    if (b->kind == BLK_INPUT) {
        uint8_t sc = b->st.scale ? b->st.scale : 1;
        int h = 16 * (int)sc;
        if (gui_style_calm()) h = h < 24 ? 24 : h;
        return h;
    }
    if (b->kind == BLK_IMAGE || b->kind == BLK_SURFACE ||
        b->kind == BLK_CANVAS)
        return b->pix_h;
    if (b->kind == BLK_ICON) return b->req_h;   /* v2.82 R3: 内置图标取显式尺寸 */
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
    if (b->kind == BLK_COLUMN || b->kind == BLK_ROW || b->kind == BLK_STACK ||
        b->kind == BLK_FREE) {
        int max_w = 0, max_h = 0, sum_w = 0, sum_h = 0, n = 0;
        for (c = b->first_child; c; c = c->next_sibling, n++) {
            if (c->w > max_w) max_w = c->w;
            if (c->h > max_h) max_h = c->h;
            sum_w += c->w; sum_h += c->h;
        }
        int gap = n > 1 ? grid_snap(b->gap) * (n - 1) : 0;
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

/* 子块可用框：bleed 子块对齐父级整框，其余对齐父级内容框（父原点+padding）。 */
static void child_box(block *c,
                      int fx, int fy, int fw, int fh,
                      int cx, int cy, int cw, int ch,
                      int *ox, int *oy, int *ow, int *oh) {
    if (c->bleed) { *ox = fx; *oy = fy; *ow = fw; *oh = fh; }
    else          { *ox = cx; *oy = cy; *ow = cw; *oh = ch; }
}

static void place_child(block *child, int bx, int by, int bw, int bh,
                        int cross_axis, int mx, int my) {
    /* cross_axis: 0 = 交叉轴横向（COLUMN），1 = 交叉轴纵向（ROW）；
     * mx/my 为主轴光标位置（已按父内容框起算）。 */
    int cx = mx, cy = my;
    if (cross_axis == 0) {
        if (child->align_cross == XALN_STRETCH) child->w = bw;
        else if (child->align_cross == XALN_CENTER) cx = bx + (bw - child->w) / 2;
        else if (child->align_cross == XALN_END)   cx = bx + bw - child->w;
    } else {
        if (child->align_cross == XALN_STRETCH) child->h = bh;
        else if (child->align_cross == XALN_CENTER) cy = by + (bh - child->h) / 2;
        else if (child->align_cross == XALN_END)   cy = by + bh - child->h;
    }
    child->x = cx; child->y = cy;
}

static void layout_column(block *b,
                           int fx, int fy, int fw, int fh,
                           int cx, int cy, int cw, int ch) {
    int n = 0;
    for (block *c = b->first_child; c; c = c->next_sibling) n++;
    if (n == 0) return;
    int total_h = 0, gap = n > 1 ? grid_snap(b->gap) : 0;
    for (block *c = b->first_child; c; c = c->next_sibling) total_h += c->h;
    total_h += gap * (n - 1);

    int y0;
    if (b->align_main == ALN_CENTER)      y0 = cy + (ch - total_h) / 2;
    else if (b->align_main == ALN_END)    y0 = cy + ch - total_h;
    else                                  y0 = cy;

    int extra = 0, slot = 0;
    if (b->align_main == ALN_SPACE_BETWEEN && n > 1) {
        extra = ch - total_h;
        slot = extra / (n - 1);
    }
    int y = y0;
    for (block *c = b->first_child; c; c = c->next_sibling) {
        int ox, oy, ow, oh;
        child_box(c, fx, fy, fw, fh, cx, cy, cw, ch, &ox, &oy, &ow, &oh);
        place_child(c, ox, oy, ow, oh, 0, ox, y);
        clamp_child(b, c, ox, oy, ow, oh);
        layout_node(c, c->x, c->y, c->w, c->h);
        y += c->h + gap + slot;
    }
}

static void layout_row(block *b,
                       int fx, int fy, int fw, int fh,
                       int cx, int cy, int cw, int ch) {
    int n = 0;
    for (block *c = b->first_child; c; c = c->next_sibling) n++;
    if (n == 0) return;
    int total_w = 0, gap = n > 1 ? grid_snap(b->gap) : 0;
    for (block *c = b->first_child; c; c = c->next_sibling) total_w += c->w;
    total_w += gap * (n - 1);

    int x0;
    if (b->align_main == ALN_CENTER)      x0 = cx + (cw - total_w) / 2;
    else if (b->align_main == ALN_END)    x0 = cx + cw - total_w;
    else                                  x0 = cx;

    int extra = 0, slot = 0;
    if (b->align_main == ALN_SPACE_BETWEEN && n > 1) {
        extra = cw - total_w;
        slot = extra / (n - 1);
    }
    int x = x0;
    for (block *c = b->first_child; c; c = c->next_sibling) {
        int ox, oy, ow, oh;
        child_box(c, fx, fy, fw, fh, cx, cy, cw, ch, &ox, &oy, &ow, &oh);
        place_child(c, ox, oy, ow, oh, 1, x, oy);
        clamp_child(b, c, ox, oy, ow, oh);
        layout_node(c, c->x, c->y, c->w, c->h);
        x += c->w + gap + slot;
    }
}

static void layout_stack(block *b,
                         int fx, int fy, int fw, int fh,
                         int cx, int cy, int cw, int ch) {
    /* 主轴 = 横向，交叉轴 = 纵向（与 ROW 一致）。 */
    for (block *c = b->first_child; c; c = c->next_sibling) {
        int ox, oy, ow, oh;
        child_box(c, fx, fy, fw, fh, cx, cy, cw, ch, &ox, &oy, &ow, &oh);
        int ccx = ox, ccy = oy;
        if (c->align_main == ALN_CENTER)       ccx = ox + (ow - c->w) / 2;
        else if (c->align_main == ALN_END)    ccx = ox + ow - c->w;
        if (c->align_cross == XALN_STRETCH)   c->h = oh;
        else if (c->align_cross == XALN_CENTER) ccy = oy + (oh - c->h) / 2;
        else if (c->align_cross == XALN_END)   ccy = oy + oh - c->h;
        if (!c->moved) { c->x = ccx; c->y = ccy; }
        clamp_child(b, c, ox, oy, ow, oh);
        layout_node(c, c->x, c->y, c->w, c->h);
    }
}

/* FREE 子块解约束：父框 → 锚点/百分比 → min/max → 落矩形。
 * 布局阶段绝不 clamp 子块坐标（越界与否由溢出策略在绘制/命中层对齐）；
 * FREE 子块不参与父容器主轴流动，各自按声明几何落位。 */
static void layout_free_child(block *c, int fx, int fy, int fw, int fh) {
    int l = c->m_l, r = c->m_r, t = c->m_t, b = c->m_b;
    int ax = fx + l, ay = fy + t;
    int aw = fw - l - r, ah = fh - t - b;
    if (aw < 0) aw = 0;
    if (ah < 0) ah = 0;

    int w, h;
    if (c->pct_w > 0) w = aw * (int)c->pct_w / 1000;
    else              w = c->req_w > 0 ? c->req_w : leaf_natural_w(c);
    if (c->pct_h > 0) h = ah * (int)c->pct_h / 1000;
    else              h = c->req_h > 0 ? c->req_h : leaf_natural_h(c);

    /* min/max 只约束尺寸，不回写坐标。 */
    if (c->min_w > 0 && w < c->min_w) w = c->min_w;
    if (c->min_h > 0 && h < c->min_h) h = c->min_h;
    if (c->max_w > 0 && w > c->max_w) w = c->max_w;
    if (c->max_h > 0 && h > c->max_h) h = c->max_h;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    c->w = w; c->h = h;

    int x, y;
    if (c->anchor & ANC_R)            x = ax + aw - w - c->px_x;
    else if (c->anchor & ANC_HCENTER) x = ax + (aw - w) / 2 + c->px_x;
    else {
        x = ax + c->px_x;
        if (c->pct_x > 0) x = ax + aw * (int)c->pct_x / 1000;
    }
    if (c->anchor & ANC_B)            y = ay + ah - h - c->px_y;
    else if (c->anchor & ANC_VCENTER) y = ay + (ah - h) / 2 + c->px_y;
    else {
        y = ay + c->px_y;
        if (c->pct_y > 0) y = ay + ah * (int)c->pct_y / 1000;
    }

    /* 拖拽收敛为 pos 偏移属性；moved 后布局保留绝对几何，跨帧不重置。 */
    if (!c->moved) {
        c->x = x + c->pos_x;
        c->y = y + c->pos_y;
    }
}

static void layout_free(block *b,
                        int fx, int fy, int fw, int fh,
                        int cx, int cy, int cw, int ch) {
    for (block *c = b->first_child; c; c = c->next_sibling) {
        int ox, oy, ow, oh;
        child_box(c, fx, fy, fw, fh, cx, cy, cw, ch, &ox, &oy, &ow, &oh);
        layout_free_child(c, ox, oy, ow, oh);
        layout_node(c, c->x, c->y, c->w, c->h);
    }
}

void layout_node(block *b, int ax, int ay, int aw, int ah) {
    if (!b) return;
    int pad_l = grid_snap(b->pad_l), pad_r = grid_snap(b->pad_r);
    int pad_t = grid_snap(b->pad_t), pad_b = grid_snap(b->pad_b);
    int cax = ax + pad_l, cay = ay + pad_t;
    int caw = aw - pad_l - pad_r, cah = ah - pad_t - pad_b;
    if (caw < 0) caw = 0;
    if (cah < 0) cah = 0;

    switch (b->kind) {
    case BLK_COLUMN: layout_column(b, ax, ay, aw, ah, cax, cay, caw, cah); break;
    case BLK_ROW:    layout_row(b,    ax, ay, aw, ah, cax, cay, caw, cah); break;
    case BLK_STACK:
    case BLK_ROOT:  layout_stack(b,  ax, ay, aw, ah, cax, cay, caw, cah); break;
    case BLK_FREE:  layout_free(b,  ax, ay, aw, ah, cax, cay, caw, cah); break;
    default: break;
    }

    /* 叶子块的子块继承父块内容框（父原点 + padding）。 */
    if (b->kind != BLK_COLUMN && b->kind != BLK_ROW &&
        b->kind != BLK_STACK && b->kind != BLK_ROOT &&
        b->kind != BLK_FREE) {
        int lx = b->x + pad_l, ly = b->y + pad_t;
        for (block *c = b->first_child; c; c = c->next_sibling) {
            /* 拖拽/缩放收敛为 moved 几何：跨帧保留绝对位置，不被父内容框重置。 */
            if (!c->moved) { c->x = lx; c->y = ly; }
            clamp_child(b, c, lx, ly, b->w - pad_l - pad_r,
                        b->h - pad_t - pad_b);
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

/* 普通布局之后：锚定浮层按锚点矩形做第二趟 place（measure 自然高 -> 翻转/钳制）。 */
static void pop_walk(block *b) {
    if (!b) return;
    if (b->kind == BLK_POPOVER) pop_layout(b);
    for (block *c = b->first_child; c; c = c->next_sibling) pop_walk(c);
}

void layout_run(void) {
    stat_set_layout_nodes(0);
    measure_dirty(gui_root_block());
    layout_dirty(gui_root_block());
    pop_walk(gui_root_block());
}
